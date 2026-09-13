#pragma once

#include "thirdparty/enkits/TaskScheduler.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"

class EntityTaskScheduler {
public:
	class Graph;
	class Mailbox : public RefCounted {
		Mutex mutex;
		Graph *first = nullptr;
		Graph *last = nullptr;

	public:
		void push(Graph *p_graph);
		Graph *pop();
	};

	enum Priority {
		HIGH = enki::TASK_PRIORITY_HIGH,
		NORMAL = enki::TASK_PRIORITY_MED,
		LOW = enki::TASK_PRIORITY_LOW,
	};

private:
	class Budget : public RefCounted {
	public:
		Mutex mutex;
		uint64_t reserved_bytes = 0;
		uint32_t graphs = 0;
	};

public:
	class Graph {
		friend class EntityTaskScheduler;
		friend class Mailbox;
		struct Task : enki::ITaskSet {
			Graph *graph = nullptr;
			uint32_t phase = 0;
			void ExecuteRange(enki::TaskSetPartition p_range, uint32_t p_thread) override;
		};
		struct Completion : enki::ICompletable {
			Graph *graph = nullptr;
			void OnDependenciesComplete(enki::TaskScheduler *p_scheduler, uint32_t p_thread) override;
		};
		Task tasks[3];
		Completion completion;
		enki::Dependency dependencies[3];
		Ref<Mailbox> mailbox;
		Ref<Budget> budget;
		Graph *completion_next = nullptr;
		bool decode_only = false;
		bool submitted = false;
		uint64_t queued_usec = 0;
		uint64_t queue_usec = 0;
		uint32_t range_count = 0;
		uint32_t read_lane_count = 1;
		mutable std::atomic<uint64_t> payload_bytes{ 0 };
		Thread::ID owner_thread = 0;

	protected:
		virtual uint32_t enumerate() = 0;
		virtual void read_range(uint32_t p_index) = 0;
		virtual void prepare(bool p_decode_only) = 0;

	public:
		static constexpr uint64_t BYTE_BUDGET = 256 * 1024 * 1024;
		static constexpr uint32_t MAX_READ_LANES = 8;
		bool reserve_payload(uint64_t p_bytes) const;
		void release_payload(uint64_t p_bytes) const;
		uint64_t get_payload_bytes() const { return payload_bytes.load(std::memory_order_relaxed); }
		SafeFlag cancelled;
		bool profile = false;
		bool ready = false;
		Graph();
		virtual ~Graph();
	};

private:
	static constexpr uint32_t MAX_GRAPHS = 2;
	static constexpr uint32_t INGRESS_CAPACITY = 16;
	static constexpr uint64_t MAX_RESERVED_BYTES = 512 * 1024 * 1024;
	static EntityTaskScheduler *singleton;
	enki::TaskScheduler scheduler;
	Thread ingress;
	Mutex ingress_mutex;
	Semaphore ingress_wake;
	Semaphore ingress_started;
	Graph *requests[INGRESS_CAPACITY] = {};
	uint32_t request_count = 0;
	Ref<Budget> budget;
	bool accepting = false;
	bool stopping = false;
	bool registered = false;
	uint32_t worker_count = 1;
	SafeNumeric<uint64_t> scheduler_allocations;
	SafeNumeric<uint32_t> active_workers;

	static void _ingress(void *p_userdata);
	static void _worker_start(uint32_t p_thread);
	static void _worker_stop(uint32_t p_thread);
	static void *_allocate(size_t p_alignment, size_t p_size, void *p_userdata, const char *p_file, int p_line);
	static void _free(void *p_pointer, size_t p_size, void *p_userdata, const char *p_file, int p_line);

public:
	static EntityTaskScheduler *get_singleton() { return singleton; }
	Error submit(Graph *p_graph, const Ref<Mailbox> &p_mailbox, bool p_decode_only = false, Priority p_priority = NORMAL);
	uint32_t get_worker_count() const { return worker_count; }
	EntityTaskScheduler();
	~EntityTaskScheduler();
};
