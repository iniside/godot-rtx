#include "entity_task_scheduler.h"

#include "core/object/script_language.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

EntityTaskScheduler *EntityTaskScheduler::singleton = nullptr;

void EntityTaskScheduler::Mailbox::push(Graph *p_graph) {
	MutexLock lock(mutex);
	if (last) {
		last->completion_next = p_graph;
	} else {
		first = p_graph;
	}
	last = p_graph;
}

EntityTaskScheduler::Graph *EntityTaskScheduler::Mailbox::pop() {
	MutexLock lock(mutex);
	Graph *graph = first;
	if (graph) {
		first = graph->completion_next;
		graph->completion_next = nullptr;
		if (!first) {
			last = nullptr;
		}
	}
	return graph;
}

EntityTaskScheduler::Graph::Graph() {
	budget = singleton->budget;
	for (uint32_t i = 0; i < 3; i++) {
		tasks[i].graph = this;
		tasks[i].phase = i;
	}
	completion.graph = this;
}

EntityTaskScheduler::Graph::~Graph() {
	if (budget.is_valid()) {
		MutexLock lock(budget->mutex);
		if (profile) {
			print_line(vformat("Entity request release payload_reserved_bytes=%d global_reserved_bytes=%d", payload_bytes.load(std::memory_order_relaxed), budget->reserved_bytes));
		}
		budget->reserved_bytes -= payload_bytes.load(std::memory_order_relaxed);
		if (admitted) {
			budget->graphs--;
		}
	}
}

bool EntityTaskScheduler::Graph::reserve_payload(uint64_t p_bytes) const {
	MutexLock lock(budget->mutex);
	const uint64_t current = payload_bytes.load(std::memory_order_relaxed);
	if (p_bytes > BYTE_BUDGET - current || p_bytes > MAX_RESERVED_BYTES - budget->reserved_bytes) {
		return false;
	}
	payload_bytes.store(current + p_bytes, std::memory_order_relaxed);
	budget->reserved_bytes += p_bytes;
	return true;
}

void EntityTaskScheduler::Graph::release_payload(uint64_t p_bytes) const {
	MutexLock lock(budget->mutex);
	payload_bytes.fetch_sub(p_bytes, std::memory_order_relaxed);
	budget->reserved_bytes -= p_bytes;
}

void EntityTaskScheduler::Graph::Task::ExecuteRange(enki::TaskSetPartition p_range, uint32_t p_thread) {
	ScriptServer::thread_enter();
	const Thread::ID caller = Thread::get_caller_id();
	CRASH_COND_MSG(caller == graph->owner_thread, "Entity preparation ran on its owner thread.");
	const uint64_t began = graph->profile ? OS::get_singleton()->get_ticks_usec() : 0;
	if (phase == 0) {
		graph->range_count = graph->cancelled.is_set() ? 0 : graph->enumerate();
		graph->read_lane_count = MAX(uint32_t(1), MIN(MAX_READ_LANES, graph->range_count));
		graph->tasks[1].m_SetSize = graph->read_lane_count;
	} else if (phase == 1) {
		for (uint32_t lane = p_range.start; lane < p_range.end; lane++) {
			for (uint32_t i = lane; i < graph->range_count; i += graph->read_lane_count) {
				if (graph->cancelled.is_set()) {
					break;
				}
				graph->read_range(i);
			}
		}
	} else if (!graph->cancelled.is_set()) {
		graph->prepare(graph->decode_only);
	}
	if (graph->profile) {
		print_line(vformat("Entity task phase=%d thread=%d enki_thread=%d owner_thread=%d begin=%d end=%d work_us=%d queue_us=%d", phase, caller, p_thread, graph->owner_thread, p_range.start, p_range.end, OS::get_singleton()->get_ticks_usec() - began, graph->queue_usec));
	}
}

void EntityTaskScheduler::Graph::Completion::OnDependenciesComplete(enki::TaskScheduler *p_scheduler, uint32_t p_thread) {
	Graph *completed = graph;
	Ref<Mailbox> destination = completed->mailbox;
	ICompletable::OnDependenciesComplete(p_scheduler, p_thread);
	// The owner may destroy the entire graph as soon as this terminal token is published.
	destination->push(completed);
}

void *EntityTaskScheduler::_allocate(size_t p_alignment, size_t p_size, void *p_userdata, const char *p_file, int p_line) {
	static_cast<EntityTaskScheduler *>(p_userdata)->scheduler_allocations.increment();
	return Memory::alloc_aligned_static(p_size, p_alignment);
}

void EntityTaskScheduler::_free(void *p_pointer, size_t p_size, void *p_userdata, const char *p_file, int p_line) {
	Memory::free_aligned_static(p_pointer);
}

void EntityTaskScheduler::_worker_start(uint32_t p_thread) {
	Thread::set_name("Entity worker " + itos(p_thread));
	ScriptServer::thread_enter();
	singleton->active_workers.increment();
}

void EntityTaskScheduler::_worker_stop(uint32_t p_thread) {
	ScriptServer::thread_exit();
	singleton->active_workers.decrement();
}

void EntityTaskScheduler::_ingress(void *p_userdata) {
	EntityTaskScheduler &self = *static_cast<EntityTaskScheduler *>(p_userdata);
	Thread::set_name("Entity task ingress");
	self.registered = self.scheduler.RegisterExternalTaskThread(enki::TaskScheduler::GetNumFirstExternalTaskThread());
	self.ingress_started.post();
	if (!self.registered) {
		return;
	}
	while (true) {
		self.ingress_wake.wait();
		Graph *graph = nullptr;
		bool stopping = false;
		{
			MutexLock lock(self.ingress_mutex);
			stopping = self.stopping;
			if (self.request_count) {
				uint32_t best = 0;
				for (uint32_t i = 1; i < self.request_count; i++) {
					const enki::TaskPriority priority = self.requests[i]->cancelled.is_set() ? enki::TASK_PRIORITY_HIGH : self.requests[i]->tasks[2].m_Priority;
					const enki::TaskPriority best_priority = self.requests[best]->cancelled.is_set() ? enki::TASK_PRIORITY_HIGH : self.requests[best]->tasks[2].m_Priority;
					if (priority < best_priority) {
						best = i;
					}
				}
				graph = self.requests[best];
				for (uint32_t i = best + 1; i < self.request_count; i++) {
					self.requests[i - 1] = self.requests[i];
				}
				self.request_count--;
			}
		}
		if (graph) {
			if (stopping) {
				graph->cancelled.set();
			}
			graph->queue_usec = OS::get_singleton()->get_ticks_usec() - graph->queued_usec;
			if (!graph->pipeline_linked) {
				if (!graph->decode_only) {
					graph->dependencies[0].SetDependency(&graph->tasks[0], &graph->tasks[1]);
					graph->dependencies[1].SetDependency(&graph->tasks[1], &graph->tasks[2]);
				}
				graph->dependencies[2].SetDependency(&graph->tasks[2], &graph->completion);
				graph->pipeline_linked = true;
			}
			self.scheduler.AddTaskSetToPipe(&graph->tasks[graph->decode_only ? 2 : 0]);
		} else if (stopping) {
			break;
		}
	}
	self.scheduler.WaitforAll();
	self.scheduler.DeRegisterExternalTaskThread();
}

EntityTaskScheduler::Reservation::~Reservation() {
	if (budget.is_valid()) {
		MutexLock lock(budget->mutex);
		budget->reserved_bytes -= bytes;
		budget->graphs--;
	}
}

Error EntityTaskScheduler::reserve(Reservation &r_reservation, uint64_t p_snapshot_bytes) {
	ERR_FAIL_COND_V(r_reservation.budget.is_valid(), ERR_ALREADY_IN_USE);
	if (p_snapshot_bytes > Graph::BYTE_BUDGET) {
		return ERR_OUT_OF_MEMORY;
	}
	MutexLock lock(ingress_mutex);
	if (!accepting || request_count == INGRESS_CAPACITY) {
		return ERR_BUSY;
	}
	MutexLock budget_lock(budget->mutex);
	if (budget->graphs >= MAX_GRAPHS || p_snapshot_bytes > MAX_RESERVED_BYTES - budget->reserved_bytes) {
		return ERR_BUSY;
	}
	budget->graphs++;
	budget->reserved_bytes += p_snapshot_bytes;
	r_reservation.bytes = p_snapshot_bytes;
	r_reservation.budget = budget;
	return OK;
}

void EntityTaskScheduler::adopt(Graph *p_graph, Reservation &p_reservation) {
	DEV_ASSERT(!p_graph->admitted && p_reservation.budget.is_valid());
	p_graph->budget = p_reservation.budget;
	p_graph->payload_bytes.fetch_add(p_reservation.bytes, std::memory_order_relaxed);
	p_graph->admitted = true;
	p_reservation.budget.unref();
	p_reservation.bytes = 0;
}

Error EntityTaskScheduler::submit(Graph *p_graph, const Ref<Mailbox> &p_mailbox, bool p_decode_only, Priority p_priority) {
	ERR_FAIL_COND_V(p_mailbox.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!p_graph->admitted, ERR_UNCONFIGURED);
	ERR_FAIL_COND_V(p_graph->submitted && !p_graph->ready, ERR_BUSY);
	MutexLock lock(ingress_mutex);
	if (!accepting) {
		return ERR_UNAVAILABLE;
	}
	if (request_count == INGRESS_CAPACITY) {
		return ERR_BUSY;
	}
	p_graph->mailbox = p_mailbox;
	p_graph->owner_thread = Thread::get_caller_id();
	p_graph->decode_only = p_decode_only;
	p_graph->submitted = true;
	p_graph->ready = false;
	p_graph->queued_usec = OS::get_singleton()->get_ticks_usec();
	for (Graph::Task &task : p_graph->tasks) {
		task.m_Priority = enki::TaskPriority(p_priority);
	}
	p_graph->completion.m_Priority = enki::TaskPriority(p_priority);
	requests[request_count++] = p_graph;
	if (p_graph->profile) {
		MutexLock budget_lock(budget->mutex);
		print_line(vformat("Entity scheduler enqueue graphs=%d queued=%d reserved_bytes=%d graph_bytes=%d scheduler_allocations=%d", budget->graphs, request_count, budget->reserved_bytes, sizeof(Graph), scheduler_allocations.get()));
	}
	ingress_wake.post();
	return OK;
}

EntityTaskScheduler::EntityTaskScheduler() {
	singleton = this;
	budget.instantiate();
	const int owner_threads = 1;
	const int render_threads = 1;
	const int ingress_threads = 1;
	worker_count = uint32_t(MAX(1, OS::get_singleton()->get_processor_count() - owner_threads - render_threads - ingress_threads));
	enki::TaskSchedulerConfig config;
	config.numTaskThreadsToCreate = worker_count;
	config.numExternalTaskThreads = 1;
	config.profilerCallbacks.threadStart = _worker_start;
	config.profilerCallbacks.threadStop = _worker_stop;
	config.customAllocator.alloc = _allocate;
	config.customAllocator.free = _free;
	config.customAllocator.userData = this;
	scheduler.Initialize(config);
	ingress.start(_ingress, this);
	ingress_started.wait();
	accepting = registered;
	ERR_FAIL_COND_MSG(!registered, "Cannot register the entity scheduler ingress thread.");
}

EntityTaskScheduler::~EntityTaskScheduler() {
	{
		MutexLock lock(ingress_mutex);
		accepting = false;
		stopping = true;
	}
	ingress_wake.post();
	ingress.wait_to_finish();
	scheduler.WaitforAllAndShutdown();
	if (OS::get_singleton()->is_use_benchmark_set()) {
		print_line(vformat("Entity scheduler shutdown workers=%d allocations=%d active_workers=%d reserved_bytes=%d", worker_count, scheduler_allocations.get(), active_workers.get(), budget->reserved_bytes));
	}
	singleton = nullptr;
}
