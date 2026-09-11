#include "micro_geometry_selection.h"

#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/profiling/profiling.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererSceneRenderImplementation;

MicroGeometrySelection::Pass::~Pass() {
	if (capacity_feedback.is_valid()) {
		MicroGeometrySelection::_retire_capacity(capacity_feedback.ptr());
	}
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	if (raster_memory_accounted) {
		storage->remove_raster_selection_memory(memory_bytes);
	}
	for (const Pin &pin : pins) {
		storage->unpin_group(pin.asset, pin.group);
	}
	for (RID asset : assets) {
		storage->release(asset);
	}
	if (feedback.is_valid()) {
		RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->feedback_free(feedback);
	}
	for (RID resource : resources) {
		RD::get_singleton()->free_rid(resource);
	}
}

MicroGeometrySelection::DepthPyramid::~DepthPyramid() {
	if (texture.is_valid()) {
		RD::get_singleton()->free_rid(texture);
	}
}

MicroGeometrySelection::MicroGeometrySelection() {
	capacity_history.instantiate();
	Vector<String> modes;
	modes.push_back("");
	String defines;
	defines = "\n#define USE_DOUBLE_PRECISION\n";
	shader.initialize(modes, defines);
	version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(version, 0));
	hzb_shader.initialize(modes);
	hzb_version = hzb_shader.version_create();
	hzb_pipeline = RD::get_singleton()->compute_pipeline_create(hzb_shader.version_get_shader(hzb_version, 0));
}

MicroGeometrySelection::~MicroGeometrySelection() {
	shader.version_free(version);
	hzb_shader.version_free(hzb_version);
}

RID MicroGeometrySelection::_buffer(Pass &r_pass, uint64_t p_size, const void *p_data, uint32_t p_usage) {
	p_size = MAX(p_size, uint64_t(16));
	ERR_FAIL_COND_V(p_size > UINT32_MAX, RID());
	RID buffer = RD::get_singleton()->storage_buffer_create(p_size, Span<uint8_t>(static_cast<const uint8_t *>(p_data), p_data ? p_size : 0), p_usage, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (buffer.is_valid()) {
		r_pass.resources.push_back(buffer);
		r_pass.memory_bytes += p_size;
	}
	return buffer;
}

MicroGeometrySelection::Capacity *MicroGeometrySelection::_capacity_entry(CapacityFeedback *p_feedback, uint32_t p_index, bool p_create) {
	CapacityHistory &history = *p_feedback->history.ptr();
	if (p_feedback->retired && p_feedback->owner <= history.retired_callback_floor) {
		return nullptr;
	}
	const CapacityKey &key = p_feedback->keys[p_index];
	CapacityHistory::Entry *entry = history.capacities.getptr(key);
	if (!entry && p_create) {
		if (history.capacities.size() >= 65536) {
			List<CapacityKey>::Element *oldest = history.retired.front();
			if (!oldest) {
				p_feedback->failed = true;
				p_feedback->retry = false;
				p_feedback->blocked_revision = history.retirement_revision;
				ERR_PRINT("Microgeometry sparse selection capacity history is occupied by active passes.");
				return nullptr;
			}
			const uint64_t old_owner = history.capacities[oldest->get()].last_owner;
			if (p_feedback->retired && p_feedback->owner <= old_owner) {
				return nullptr;
			}
			history.retired_callback_floor = MAX(history.retired_callback_floor, old_owner);
			history.capacities.erase(oldest->get());
			history.retired.erase(oldest);
		}
		history.capacities.insert(key, CapacityHistory::Entry());
		entry = history.capacities.getptr(key);
	}
	if (!entry) {
		return nullptr;
	}
	entry->last_owner = MAX(entry->last_owner, p_feedback->owner);
	if (entry->retired) {
		history.retired.erase(entry->retired);
		entry->retired = nullptr;
	}
	if (!p_feedback->retired && !p_feedback->leased[p_index]) {
		entry->owners++;
		p_feedback->leased.write[p_index] = 1;
	}
	if (entry->owners == 0) {
		entry->retired = history.retired.push_back(key);
	}
	return &entry->capacity;
}

void MicroGeometrySelection::_retire_capacity(CapacityFeedback *p_feedback) {
	p_feedback->retired = true;
	CapacityHistory &history = *p_feedback->history.ptr();
	for (uint32_t index = 0; index < uint32_t(p_feedback->keys.size()); index++) {
		if (!p_feedback->leased[index]) {
			continue;
		}
		CapacityHistory::Entry &entry = history.capacities[p_feedback->keys[index]];
		entry.owners--;
		if (entry.owners == 0) {
			entry.retired = history.retired.push_back(p_feedback->keys[index]);
			history.retirement_revision++;
		}
		p_feedback->leased.write[index] = 0;
	}
}

void MicroGeometrySelection::_capacity_feedback(const Vector<uint8_t> &p_bytes, Ref<RefCounted> p_feedback) {
	auto *feedback = static_cast<CapacityFeedback *>(p_feedback.ptr());
	feedback->pending = false;
	feedback->retry = p_bytes.size() < int64_t(feedback->keys.size()) * 32;
	if (feedback->retry) {
		return;
	}
	feedback->completed_frame = feedback->sample_frame;
	feedback->queue_overflows = 0;
	feedback->record_overflows = 0;
	for (uint32_t index = 0; index < uint32_t(feedback->keys.size()); index++) {
		const uint8_t *state = p_bytes.ptr() + uint64_t(index) * 32;
		const uint32_t flags = decode_uint32(state + 12);
		feedback->queue_overflows += (flags & 1) != 0;
		feedback->record_overflows += (flags & 2) != 0;
		if ((flags & 3) == 0) {
			continue;
		}
		feedback->retry = true;
		Capacity capacity = feedback->allocated[index];
		const Capacity &limit = feedback->limits[index];
		if (flags & 1) {
			capacity.queue = MIN(uint64_t(limit.queue), MAX(uint64_t(capacity.queue) * 2, uint64_t(decode_uint32(state))));
		}
		if (flags & 2) {
			capacity.records = MIN(uint64_t(limit.records), MAX(uint64_t(capacity.records) * 2, uint64_t(decode_uint32(state + 4))));
		}
		Capacity *retained = _capacity_entry(feedback, index, true);
		if (!retained) {
			if (feedback->failed) {
				return;
			}
			continue;
		}
		retained->queue = MAX(retained->queue, capacity.queue);
		retained->records = MAX(retained->records, capacity.records);
	}
}

void MicroGeometrySelection::_retire_buffers(Pass *p_pass) {
	if (!p_pass->retired_buffers.is_empty() && p_pass->retired_submission <= RD::get_singleton()->get_completed_submission_serial()) {
		for (RID resource : p_pass->retired_buffers) {
			p_pass->resources.erase(resource);
			RD::get_singleton()->free_rid(resource);
		}
		p_pass->retired_buffers.clear();
		p_pass->memory_bytes -= p_pass->retired_bytes;
		if (p_pass->raster_memory_accounted) {
			RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->remove_raster_selection_memory(p_pass->retired_bytes);
		}
		p_pass->retired_bytes = 0;
		p_pass->retired_submission = 0;
	}
}

bool MicroGeometrySelection::needs_retry(Pass *p_pass) const {
	_retire_buffers(p_pass);
	CapacityFeedback &feedback = *p_pass->capacity_feedback.ptr();
	if (feedback.failed && feedback.blocked_revision != feedback.history->retirement_revision) {
		feedback.failed = false;
		feedback.retry = true;
	}
	if (p_pass->admission_failed || feedback.failed || !feedback.retry) {
		return false;
	}
	if (!p_pass->retired_buffers.is_empty()) {
		p_pass->allocation_status = "waiting_retirement";
		return false;
	}
	return true;
}

void MicroGeometrySelection::_report_selection(Pass *p_pass) {
	if (!RSG::utilities->capturing_timestamps) {
		return;
	}
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (p_pass->profile_frame != UINT64_MAX && frame - p_pass->profile_frame < 120) {
		return;
	}
	p_pass->profile_frame = frame;
	const CapacityFeedback &feedback = *p_pass->capacity_feedback.ptr();
	const char *kind = (p_pass->data.flags & 32) ? "rt" : ((p_pass->data.flags & 64) ? "shadow" : "camera");
	print_line(vformat("Microgeometry selection: frame=%d pass=%s owner=%d tasks=%d units=%d active_bytes=%d fixed_bytes=%d retired_bytes=%d requested_active_bytes=%d replacement_peak_bytes=%d budget_bytes=%d resize_attempts=%d append_attempts=%d queue_work=%d queue_reserved=%d queue_holes=%d record_work=%d record_reserved=%d record_holes=%d outcome=%s admission_failed=%d history_failed=%d retry=%d feedback_pending=%d feedback_frame=%d queue_overflow_units=%d record_overflow_units=%d", frame, kind, feedback.owner, p_pass->data.task_count, p_pass->data.unit_count, p_pass->memory_bytes - p_pass->retired_bytes, p_pass->fixed_memory_bytes, p_pass->retired_bytes, p_pass->requested_bytes, p_pass->replacement_peak_bytes, MAX_PASS_BYTES, p_pass->resize_attempts, p_pass->append_attempts, p_pass->data.queue_work, p_pass->queue_reserved, p_pass->queue_holes, p_pass->data.record_work, p_pass->record_reserved, p_pass->record_holes, p_pass->allocation_status, int(p_pass->admission_failed), int(feedback.failed), int(feedback.retry), int(feedback.pending), feedback.completed_frame, feedback.queue_overflows, feedback.record_overflows));
}

uint32_t MicroGeometrySelection::_build_bins(const Vector<Task> &p_tasks, const Vector<Unit> &p_units, uint32_t p_bin_count, Vector<Bin> &r_bins) {
	r_bins.clear();
	r_bins.resize(p_bin_count);
	for (const Unit &unit : p_units) {
		r_bins.write[p_tasks[unit.task].bin].capacity += unit.record_capacity;
	}
	uint32_t offset = 0;
	for (Bin &bin : r_bins) {
		bin.offset = offset;
		offset += bin.capacity;
	}
	return offset;
}

bool MicroGeometrySelection::_validate_bins(Pass *p_pass, const Vector<Bin> &p_bins) {
	if ((p_pass->data.flags & 32) != 0) {
		return true;
	}
	RD *rd = RD::get_singleton();
	ERR_FAIL_COND_V_MSG(!rd->mesh_shader_is_supported(), false, "Microgeometry raster requires mesh shader support.");
	const RD::MeshShaderLimits limits = rd->mesh_shader_get_limits();
	const uint32_t width = MIN(1024u, limits.max_workgroup_count[0]);
	ERR_FAIL_COND_V_MSG(width == 0, false, "Microgeometry mesh dispatch width is unsupported.");
	for (const Bin &bin : p_bins) {
		const uint64_t height = (uint64_t(bin.capacity) + width - 1) / width;
		const uint64_t padded = MIN(bin.capacity, width) * height;
		ERR_FAIL_COND_V_MSG(height > limits.max_workgroup_count[1] || padded > limits.max_workgroup_total_count, false, "Microgeometry bin exceeds mesh shader dispatch limits.");
	}
	p_pass->raster_data.dispatch_width = width;
	return true;
}

void MicroGeometrySelection::_upload_order(Pass *p_pass) {
	if (p_pass->record_order_data.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	rd->buffer_update(p_pass->record_order, 0, uint32_t(p_pass->record_order_data.size()) * 4, p_pass->record_order_data.ptr());
	rd->buffer_update(p_pass->queue_order, 0, uint32_t(p_pass->queue_order_data.size()) * 4, p_pass->queue_order_data.ptr());
}

bool MicroGeometrySelection::_grow(Pass *p_pass, const Vector<Unit> &p_units) {
	_retire_buffers(p_pass);
	if (!p_pass->retired_buffers.is_empty()) {
		p_pass->allocation_status = "waiting_retirement";
		p_pass->capacity_feedback->retry = true;
		return false;
	}
	struct Move {
		uint32_t from;
		uint32_t to;
		uint32_t count;
	};
	Vector<Unit> units = p_units;
	LocalVector<Move> moves;
	LocalVector<uint8_t> queue_moved;
	LocalVector<uint8_t> record_moved;
	LocalVector<uint32_t> queue_appended;
	LocalVector<uint32_t> record_appended;
	queue_moved.resize_initialized(units.size());
	record_moved.resize_initialized(units.size());
	uint64_t queue_used = p_pass->data.queue_work;
	uint64_t record_used = p_pass->data.record_work;
	uint64_t queue_holes = p_pass->queue_holes;
	uint64_t record_holes = p_pass->record_holes;
	for (uint32_t index = 0; index < uint32_t(units.size()); index++) {
		const Unit &old = p_pass->unit_data[index];
		Unit &unit = units.write[index];
		unit.queue_capacity = MAX(unit.queue_capacity, old.queue_capacity);
		unit.record_capacity = MAX(unit.record_capacity, old.record_capacity);
		if (unit.queue_capacity > old.queue_capacity) {
			unit.queue_offset = queue_used;
			unit.hash_offset = unit.queue_offset * 2;
			queue_used += unit.queue_capacity;
			queue_holes += old.queue_capacity;
			queue_moved[index] = 1;
			queue_appended.push_back(index);
		}
		if (unit.record_capacity > old.record_capacity) {
			unit.record_offset = record_used;
			record_used += unit.record_capacity;
			record_holes += old.record_capacity;
			record_moved[index] = 1;
			record_appended.push_back(index);
			moves.push_back({ old.record_offset, unit.record_offset, old.record_capacity });
		}
	}
	if (queue_appended.is_empty() && record_appended.is_empty()) {
		return true;
	}
	if (queue_used > p_pass->queue_reserved || record_used > p_pass->record_reserved || queue_holes * 100 > queue_used * HOLE_PERCENT || record_holes * 100 > record_used * HOLE_PERCENT) {
		if (!_resize(p_pass, units)) {
			return false;
		}
		_upload_order(p_pass);
		return true;
	}
	Vector<Bin> bins;
	const uint32_t selected_capacity = _build_bins(p_pass->task_data, units, p_pass->data.bin_count, bins);
	if (!_validate_bins(p_pass, bins)) {
		p_pass->capacity_feedback->retry = true;
		return false;
	}
	auto rebuild_order = [](const Vector<uint32_t> &p_source, const LocalVector<uint8_t> &p_moved, const LocalVector<uint32_t> &p_appended, Vector<uint32_t> &r_order) {
		r_order.resize(p_source.size());
		uint32_t cursor = 0;
		for (uint32_t index = 0; index < uint32_t(p_source.size()); index++) {
			if (!p_moved[p_source[index]]) {
				r_order.write[cursor++] = p_source[index];
			}
		}
		for (uint32_t index : p_appended) {
			r_order.write[cursor++] = index;
		}
	};
	Vector<uint32_t> record_order;
	Vector<uint32_t> queue_order;
	rebuild_order(p_pass->record_order_data, record_moved, record_appended, record_order);
	rebuild_order(p_pass->queue_order_data, queue_moved, queue_appended, queue_order);
	uint64_t scratch_records = 0;
	for (const Move &move : moves) {
		scratch_records += move.count;
	}
	Pass staging;
	RID scratch = scratch_records != 0 ? _buffer(staging, scratch_records * 8) : RID();
	RID unit_buffer = _buffer(staging, uint64_t(units.size()) * sizeof(Unit), units.ptr());
	if (unit_buffer.is_null() || (scratch_records != 0 && scratch.is_null())) {
		p_pass->allocation_status = "allocation_failed";
		p_pass->capacity_feedback->retry = true;
		ERR_PRINT("Unable to allocate sparse microgeometry selection append buffers.");
		return false;
	}
	RD *rd = RD::get_singleton();
	uint64_t cursor = 0;
	for (const Move &move : moves) {
		rd->buffer_copy(p_pass->committed, scratch, uint64_t(move.from) * 8, cursor * 8, uint64_t(move.count) * 8);
		cursor += uint64_t(move.count);
	}
	cursor = 0;
	for (const Move &move : moves) {
		rd->buffer_copy(scratch, p_pass->committed, cursor * 8, uint64_t(move.to) * 8, uint64_t(move.count) * 8);
		cursor += uint64_t(move.count);
	}
	rd->buffer_update(p_pass->bins, 0, bins.size() * sizeof(Bin), bins.ptr());
	p_pass->retired_bytes = MAX(uint64_t(16), uint64_t(p_pass->unit_data.size()) * sizeof(Unit)) + (scratch.is_valid() ? MAX(uint64_t(16), scratch_records * 8) : 0);
	p_pass->retired_submission = rd->get_pending_submission_serial();
	p_pass->retired_buffers.push_back(p_pass->units);
	if (scratch.is_valid()) {
		p_pass->retired_buffers.push_back(scratch);
	}
	p_pass->units = unit_buffer;
	for (RID resource : staging.resources) {
		p_pass->resources.push_back(resource);
	}
	staging.resources.clear();
	if (p_pass->raster_memory_accounted) {
		RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->add_raster_selection_memory(staging.memory_bytes);
	}
	p_pass->memory_bytes += staging.memory_bytes;
	p_pass->unit_data = units;
	p_pass->bin_data = bins;
	p_pass->record_order_data = record_order;
	p_pass->queue_order_data = queue_order;
	_upload_order(p_pass);
	for (uint32_t index = 0; index < uint32_t(units.size()); index++) {
		p_pass->capacity_feedback->allocated.write[index] = { units[index].queue_capacity, units[index].record_capacity };
	}
	p_pass->data.queue_work = queue_used;
	p_pass->data.record_work = record_used;
	p_pass->queue_holes = queue_holes;
	p_pass->record_holes = record_holes;
	p_pass->selected_capacity = selected_capacity;
	p_pass->append_attempts++;
	p_pass->allocation_status = "appended";
	return true;
}

bool MicroGeometrySelection::_resize(Pass *p_pass, const Vector<Unit> &p_units) {
	_retire_buffers(p_pass);
	if (!p_pass->retired_buffers.is_empty()) {
		p_pass->allocation_status = "waiting_retirement";
		p_pass->capacity_feedback->retry = true;
		return false;
	}
	p_pass->resize_attempts++;
	Vector<Unit> units = p_units;
	Vector<Bin> bins;
	uint64_t queue_count = 0;
	uint64_t record_count = 0;
	uint32_t selected_capacity = 0;
	auto prepare_selection_offsets = [&](uint32_t) {
		for (Unit &unit : units) {
			unit.queue_offset = queue_count;
			unit.hash_offset = queue_count * 2;
			unit.record_offset = record_count;
			queue_count += unit.queue_capacity;
			record_count += unit.record_capacity;
		}
		selected_capacity = _build_bins(p_pass->task_data, units, p_pass->data.bin_count, bins);
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("prepare_selection_offsets");
		(*static_cast<decltype(prepare_selection_offsets) *>(p_data))(p_index);
	},
			&prepare_selection_offsets, 1, 1, true, SNAME("prepare_selection_offsets"));
	pool->wait_for_group_task_completion(job);
	const bool raster = (p_pass->data.flags & 32) == 0;
	const uint64_t dispatch_bytes = raster ? MAX(uint64_t(16), uint64_t(bins.size()) * 12) : 16;
	if (!_validate_bins(p_pass, bins)) {
		return false;
	}
	const uint64_t queue_reserved = queue_count * RESERVE_PERCENT / 100;
	const uint64_t record_reserved = record_count * RESERVE_PERCENT / 100;
	const uint64_t requested_dynamic = MAX(uint64_t(16), uint64_t(units.size()) * sizeof(Unit)) + MAX(uint64_t(16), uint64_t(bins.size()) * sizeof(Bin)) + MAX(uint64_t(16), queue_reserved * 4) + MAX(uint64_t(16), queue_reserved * 16) + 3 * MAX(uint64_t(16), record_reserved * 8) + dispatch_bytes + MAX(uint64_t(16), record_reserved * sizeof(MicroGeometrySelectedCluster));
	p_pass->requested_bytes = requested_dynamic + p_pass->fixed_memory_bytes;
	p_pass->replacement_peak_bytes = p_pass->requested_bytes + p_pass->dynamic_memory_bytes + p_pass->retired_bytes;
	if (queue_reserved > UINT32_MAX / 16 || record_reserved > UINT32_MAX / sizeof(MicroGeometrySelectedCluster) || p_pass->requested_bytes > MAX_PASS_BYTES) {
		p_pass->admission_failed = true;
		p_pass->allocation_status = "admission_rejected";
		_report_selection(p_pass);
		ERR_PRINT(vformat("Microgeometry sparse selection admission failed: %d queue slots, %d cut slots, %d active bytes requested (limit %d, replacement peak %d, flags %d, tasks %d).", queue_reserved, record_reserved, p_pass->requested_bytes, MAX_PASS_BYTES, p_pass->replacement_peak_bytes, p_pass->data.flags, p_pass->data.task_count));
		return false;
	}
	Pass replacement;
	replacement.units = _buffer(replacement, uint64_t(units.size()) * sizeof(Unit), units.ptr());
	replacement.bins = _buffer(replacement, MAX(uint64_t(16), uint64_t(bins.size()) * sizeof(Bin)));
	replacement.queue = _buffer(replacement, queue_reserved * 4);
	replacement.sparse_states = _buffer(replacement, queue_reserved * 16);
	replacement.candidate = _buffer(replacement, record_reserved * 8);
	replacement.committed = _buffer(replacement, record_reserved * 8);
	replacement.rejected = _buffer(replacement, record_reserved * 8);
	replacement.dispatch_arguments = _buffer(replacement, dispatch_bytes, nullptr, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	replacement.selected = _buffer(replacement, record_reserved * sizeof(MicroGeometrySelectedCluster));
	for (RID required : { replacement.units, replacement.bins, replacement.queue, replacement.sparse_states, replacement.candidate, replacement.committed, replacement.rejected, replacement.dispatch_arguments, replacement.selected }) {
		if (required.is_null()) {
			p_pass->admission_failed = true;
			p_pass->allocation_status = "allocation_failed";
			_report_selection(p_pass);
			ERR_PRINT("Unable to allocate sparse microgeometry selection buffers.");
			return false;
		}
	}
	RD *rd = RD::get_singleton();
	rd->buffer_update(replacement.bins, 0, bins.size() * sizeof(Bin), bins.ptr());
	for (uint32_t index = 0; index < uint32_t(p_pass->unit_data.size()); index++) {
		const Unit &old = p_pass->unit_data[index];
		rd->buffer_copy(p_pass->committed, replacement.committed, uint64_t(old.record_offset) * 8, uint64_t(units[index].record_offset) * 8, uint64_t(old.record_capacity) * 8);
	}
	p_pass->retired_bytes = p_pass->dynamic_memory_bytes;
	p_pass->retired_submission = rd->get_pending_submission_serial();
	for (RID resource : { p_pass->units, p_pass->bins, p_pass->queue, p_pass->sparse_states, p_pass->candidate, p_pass->committed, p_pass->rejected, p_pass->dispatch_arguments, p_pass->selected }) {
		if (resource.is_valid()) {
			p_pass->retired_buffers.push_back(resource);
		}
	}
	p_pass->units = replacement.units;
	p_pass->bins = replacement.bins;
	p_pass->queue = replacement.queue;
	p_pass->sparse_states = replacement.sparse_states;
	p_pass->candidate = replacement.candidate;
	p_pass->committed = replacement.committed;
	p_pass->rejected = replacement.rejected;
	p_pass->dispatch_arguments = replacement.dispatch_arguments;
	p_pass->selected = replacement.selected;
	for (RID resource : replacement.resources) {
		p_pass->resources.push_back(resource);
	}
	replacement.resources.clear();
	if (p_pass->raster_memory_accounted) {
		auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
		storage->add_raster_selection_memory(replacement.memory_bytes);
	}
	p_pass->memory_bytes += replacement.memory_bytes;
	p_pass->dynamic_memory_bytes = replacement.memory_bytes;
	p_pass->unit_data = units;
	p_pass->capacity_feedback->allocated.clear();
	for (const Unit &unit : units) {
		p_pass->capacity_feedback->allocated.push_back({ unit.queue_capacity, unit.record_capacity });
	}
	p_pass->bin_data = bins;
	p_pass->record_order_data.resize(units.size());
	p_pass->queue_order_data.resize(units.size());
	for (uint32_t index = 0; index < uint32_t(units.size()); index++) {
		p_pass->record_order_data.write[index] = index;
		p_pass->queue_order_data.write[index] = index;
	}
	p_pass->data.queue_work = queue_count;
	p_pass->data.record_work = record_count;
	p_pass->queue_reserved = queue_reserved;
	p_pass->record_reserved = record_reserved;
	p_pass->queue_holes = 0;
	p_pass->record_holes = 0;
	p_pass->selected_capacity = selected_capacity;
	p_pass->allocation_status = "allocated";
	return true;
}

MicroGeometrySelection::Pass *MicroGeometrySelection::create(const Vector<Task> &p_tasks, uint32_t p_bin_count, const Parameters &p_parameters, uint32_t p_levels, uint32_t p_native_stride, RID p_instances, RID p_surfaces, const Vector<RID> &p_dependencies) {
	ERR_FAIL_COND_V(p_tasks.is_empty() || p_bin_count == 0 || pipeline.is_null(), nullptr);
	Pass *pass = memnew(Pass);
	pass->data = p_parameters;
	pass->data.task_count = p_tasks.size();
	pass->data.bin_count = p_bin_count;
	pass->levels = p_levels;
	pass->persistent_instances = p_instances;
	pass->persistent_surfaces = p_surfaces;
	pass->dependencies = p_dependencies;
	pass->task_data = p_tasks;
	pass->capacity_feedback.instantiate();
	pass->capacity_feedback->history = capacity_history;
	pass->capacity_feedback->owner = ++capacity_history->next_owner;
	Vector<Unit> units;
	bool valid = true;
	auto prepare_selection_units = [&](uint32_t) {
		for (uint32_t task_index = 0; task_index < uint32_t(p_tasks.size()); task_index++) {
			const Task &task = p_tasks[task_index];
			if (task.bin >= p_bin_count || task.group_count == 0 || task.cluster_count == 0 || uint64_t(units.size()) + task.multimesh_count > 1024 * 1024) {
				valid = false;
				return;
			}
			for (uint32_t ordinal = 0; ordinal < task.multimesh_count; ordinal++) {
				CapacityKey key = { task.surface, task.asset, ordinal };
				pass->capacity_feedback->keys.push_back(key);
				pass->capacity_feedback->leased.push_back(0);
				const Capacity *retained = _capacity_entry(pass->capacity_feedback.ptr(), pass->capacity_feedback->keys.size() - 1, false);
				Unit unit;
				unit.task = task_index;
				unit.ordinal = ordinal;
				unit.queue_capacity = MIN(task.group_count, MAX(32u, retained ? retained->queue : 0u));
				unit.record_capacity = MIN(task.cluster_count, MAX(MAX(32u, task.coarse_count), retained ? retained->records : 0u));
				units.push_back(unit);
				pass->capacity_feedback->limits.push_back({ task.group_count, task.cluster_count });
			}
		}
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("prepare_selection_units");
		(*static_cast<decltype(prepare_selection_units) *>(p_data))(p_index);
	},
			&prepare_selection_units, 1, 1, true, SNAME("prepare_selection_units"));
	pool->wait_for_group_task_completion(job);
	if (!valid) {
		memdelete(pass);
		ERR_FAIL_V_MSG(nullptr, "Microgeometry sparse selection task admission failed.");
	}
	pass->data.unit_count = units.size();
	pass->fixed_memory_bytes = MAX(uint64_t(16), uint64_t(p_tasks.size()) * sizeof(Task)) + MAX(uint64_t(16), uint64_t(units.size()) * 32) + MAX(uint64_t(16), uint64_t(p_tasks.size()) * 4) + 2 * MAX(uint64_t(16), uint64_t(p_bin_count) * 4) + STATISTICS_BYTES + MAX(uint64_t(16), uint64_t(p_tasks.size()) * p_native_stride) + sizeof(Parameters) + sizeof(MicroGeometryRasterParameters) + MAX(uint64_t(16), uint64_t(sizeof(RendererRD::MicroGeometryStorage::FeedbackHeader))) + 2 * MAX(uint64_t(16), uint64_t(units.size()) * 4);
	if (!_resize(pass, units)) {
		memdelete(pass);
		return nullptr;
	}
	pass->tasks = _buffer(*pass, uint64_t(p_tasks.size()) * sizeof(Task), p_tasks.ptr());
	pass->unit_states = _buffer(*pass, uint64_t(units.size()) * 32);
	pass->record_order = _buffer(*pass, uint64_t(units.size()) * 4);
	pass->queue_order = _buffer(*pass, uint64_t(units.size()) * 4);
	pass->validity = _buffer(*pass, uint64_t(p_tasks.size()) * 4);
	pass->counts = _buffer(*pass, uint64_t(p_bin_count) * 4, nullptr, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	pass->initial_counts = _buffer(*pass, uint64_t(p_bin_count) * 4);
	pass->statistics = _buffer(*pass, STATISTICS_BYTES);
	pass->native_instances = _buffer(*pass, uint64_t(p_tasks.size()) * p_native_stride);
	pass->parameters = _buffer(*pass, sizeof(Parameters));
	pass->raster_parameters = RD::get_singleton()->uniform_buffer_create(sizeof(MicroGeometryRasterParameters));
	if (pass->raster_parameters.is_valid()) {
		pass->resources.push_back(pass->raster_parameters);
	}
	for (RID required : { pass->tasks, pass->unit_states, pass->record_order, pass->queue_order, pass->validity, pass->counts, pass->initial_counts, pass->native_instances, pass->parameters, pass->raster_parameters, pass->statistics }) {
		if (required.is_null()) {
			memdelete(pass);
			return nullptr;
		}
	}
	RD::get_singleton()->buffer_clear(pass->unit_states, 0, MAX(16u, uint32_t(units.size()) * 32));
	_upload_order(pass);
	pass->memory_bytes += sizeof(MicroGeometryRasterParameters);
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	pass->feedback = storage->feedback_create();
	pass->requests_fallback = _buffer(*pass, sizeof(RendererRD::MicroGeometryStorage::FeedbackHeader));
	if (pass->requests_fallback.is_null()) {
		memdelete(pass);
		return nullptr;
	}
	MicroGeometryRasterParameters raster = pass->raster_data;
	raster.page_pool = RD::get_singleton()->buffer_get_device_address(storage->get_pool());
	RD::get_singleton()->buffer_update(pass->raster_parameters, 0, sizeof(raster), &raster);
	pass->raster_data = raster;
	if ((pass->data.flags & 32) == 0) {
		storage->add_raster_selection_memory(pass->memory_bytes);
		pass->raster_memory_accounted = true;
	}
	return pass;
}

RID MicroGeometrySelection::_prepare_dispatch(Pass *p_pass, RID p_hzb) {
	RD::get_singleton()->buffer_update(p_pass->parameters, 0, sizeof(Parameters), &p_pass->data);
	if (p_hzb.is_null()) {
		p_hzb = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	}
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_pass->parameters));
	uint32_t binding = 1;
	for (RID buffer : { p_pass->tasks, p_pass->bins, p_pass->persistent_instances, p_pass->persistent_surfaces, p_pass->sparse_states, p_pass->rejected, p_pass->validity, p_pass->counts, p_pass->initial_counts, p_pass->unit_states, p_pass->dispatch_arguments, p_pass->selected, p_pass->native_instances, p_pass->requests }) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding++, buffer));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 15, p_hzb));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, p_pass->statistics));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, p_pass->units));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, p_pass->queue));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, p_pass->candidate));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, p_pass->committed));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, p_pass->record_order));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, p_pass->queue_order));
	return UniformSetCacheRD::get_singleton()->get_cache_vec(shader.version_get_shader(version, 0), 0, uniforms);
}

void MicroGeometrySelection::_dispatch(Pass *p_pass, uint32_t p_mode, uint32_t p_items, RID p_uniform_set, uint32_t p_level) {
	if (p_items == 0) {
		return;
	}
	PushConstant push = { p_mode, p_level };
	RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(list, p_uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(list, &push, sizeof(push));
	for (RID dependency : p_pass->dependencies) {
		RD::get_singleton()->compute_list_add_buffer_dependency(list, dependency);
	}
	uint32_t groups = (p_items + 63) / 64;
	RD::get_singleton()->compute_list_dispatch(list, MIN(groups, 1024u), (groups + 1023) / 1024, 1);
	RD::get_singleton()->compute_list_end();
}

void MicroGeometrySelection::select(Pass *p_pass, RID p_hzb) {
	ERR_FAIL_NULL(p_pass);
	if (needs_retry(p_pass) && !p_pass->capacity_feedback->pending) {
		Vector<Unit> units = p_pass->unit_data;
		bool resize = false;
		auto prepare_selection_capacity = [&](uint32_t) {
			for (uint32_t index = 0; index < uint32_t(units.size()); index++) {
				Unit &unit = units.write[index];
				const Capacity *capacity = _capacity_entry(p_pass->capacity_feedback.ptr(), index, false);
				if (capacity) {
					resize |= capacity->queue > unit.queue_capacity || capacity->records > unit.record_capacity;
					unit.queue_capacity = MAX(unit.queue_capacity, capacity->queue);
					unit.record_capacity = MAX(unit.record_capacity, capacity->records);
				}
			}
		};
		WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
			GodotProfileZone("prepare_selection_capacity");
			(*static_cast<decltype(prepare_selection_capacity) *>(p_data))(p_index);
		},
				&prepare_selection_capacity, 1, 1, true, SNAME("prepare_selection_capacity"));
		pool->wait_for_group_task_completion(job);
		p_pass->capacity_feedback->retry = false;
		if (resize) {
			_grow(p_pass, units);
		}
	}
	_report_selection(p_pass);
	const bool rt = (p_pass->data.flags & 32) != 0;
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Selection Reset" : "Microgeometry Raster Selection Reset");
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	RD *rd = RD::get_singleton();
	p_pass->requests = storage->feedback_begin(p_pass->feedback);
	p_pass->feedback_active = p_pass->requests.is_valid();
	if (!p_pass->feedback_active) {
		p_pass->requests = p_pass->requests_fallback;
		rd->buffer_clear(p_pass->requests, 0, sizeof(RendererRD::MicroGeometryStorage::FeedbackHeader));
	}
	p_pass->recovered = false;
	rd->buffer_clear(p_pass->statistics, 0, STATISTICS_BYTES);
	rd->draw_command_begin_label("Microgeometry Sparse Selection");
	rd->buffer_clear(p_pass->counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	rd->buffer_clear(p_pass->initial_counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	rd->buffer_clear(p_pass->sparse_states, 0, MAX(16u, p_pass->data.queue_work * 16));
	RID uniform_set = _prepare_dispatch(p_pass, p_hzb);
	_dispatch(p_pass, 0, p_pass->data.task_count, uniform_set);
	_dispatch(p_pass, 1, p_pass->data.unit_count, uniform_set);
	_dispatch(p_pass, 2, p_pass->data.record_work, uniform_set);
	_dispatch(p_pass, 3, p_pass->data.unit_count, uniform_set);
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Sparse Traverse" : "Microgeometry Raster Sparse Traverse");
	for (uint32_t depth = p_pass->levels; depth > 0; depth--) {
		_dispatch(p_pass, 14, p_pass->data.unit_count, uniform_set);
		_dispatch(p_pass, 4, p_pass->data.queue_work, uniform_set, depth - 1);
	}
	_dispatch(p_pass, 5, p_pass->data.queue_work, uniform_set);
	_dispatch(p_pass, 6, p_pass->data.record_work, uniform_set);
	_dispatch(p_pass, 9, p_pass->data.unit_count, uniform_set);
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Cluster Emit" : "Microgeometry Raster Cluster Emit");
	_dispatch(p_pass, 10, p_pass->data.record_work, uniform_set);
	if (!rt) {
		_dispatch(p_pass, 13, p_pass->data.bin_count, uniform_set, p_pass->raster_data.dispatch_width);
	}
	rd->draw_command_end_label();
	if (!p_pass->capacity_feedback->pending && !p_pass->admission_failed && !p_pass->capacity_feedback->failed) {
		p_pass->capacity_feedback->sample_frame = RSG::rasterizer->get_frame_number();
		p_pass->capacity_feedback->pending = true;
		Ref<RefCounted> feedback = p_pass->capacity_feedback;
		if (rd->buffer_get_data_async(p_pass->unit_states, callable_mp_static(&MicroGeometrySelection::_capacity_feedback).bind(feedback)) != OK) {
			_capacity_feedback(Vector<uint8_t>(), feedback);
		}
	}
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Selection Complete" : "Microgeometry Raster Selection Complete");
}

void MicroGeometrySelection::recover(Pass *p_pass, RID p_hzb) {
	ERR_FAIL_NULL(p_pass);
	RENDER_TIMESTAMP("Microgeometry Raster Recovery Select");
	p_pass->recovered = true;
	RD::get_singleton()->buffer_clear(p_pass->counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	RID uniform_set = _prepare_dispatch(p_pass, p_hzb);
	_dispatch(p_pass, 11, p_pass->data.record_work, uniform_set);
	_dispatch(p_pass, 13, p_pass->data.bin_count, uniform_set, p_pass->raster_data.dispatch_width);
	RENDER_TIMESTAMP("Microgeometry Raster Recovery Select Complete");
}

void MicroGeometrySelection::update_frozen(Pass *p_pass) {
	ERR_FAIL_NULL(p_pass);
	p_pass->requests = p_pass->requests_fallback;
	RID uniform_set = _prepare_dispatch(p_pass, RID());
	_dispatch(p_pass, 0, p_pass->data.task_count, uniform_set);
}

bool MicroGeometrySelection::freeze(Pass *p_pass) {
	ERR_FAIL_NULL_V(p_pass, false);
	RENDER_TIMESTAMP("Microgeometry Freeze Prepare");
	RID uniform_set = _prepare_dispatch(p_pass, RID());
	if (p_pass->recovered) {
		_dispatch(p_pass, 12, p_pass->data.bin_count, uniform_set);
	}
	_dispatch(p_pass, 13, p_pass->data.bin_count, uniform_set, p_pass->raster_data.dispatch_width);
	p_pass->recovered = false;
	RENDER_TIMESTAMP("Microgeometry Freeze Readback");
	Vector<uint8_t> counts = RD::get_singleton()->buffer_get_data(p_pass->counts);
	Vector<uint8_t> selected = RD::get_singleton()->buffer_get_data(p_pass->selected);
	RENDER_TIMESTAMP("Microgeometry Freeze Pinning");
	ERR_FAIL_COND_V(counts.size() < int64_t(p_pass->data.bin_count) * 4 || selected.size() < int64_t(p_pass->selected_capacity) * int64_t(sizeof(MicroGeometrySelectedCluster)), false);
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	HashMap<RID, HashSet<uint32_t>> used;
	for (uint32_t bin = 0; bin < p_pass->bin_data.size(); bin++) {
		uint32_t count = decode_uint32(counts.ptr() + bin * 4);
		ERR_FAIL_COND_V(count > p_pass->bin_data[bin].capacity, false);
		for (uint32_t index = 0; index < count; index++) {
			MicroGeometrySelectedCluster record;
			memcpy(&record, selected.ptr() + uint64_t(p_pass->bin_data[bin].offset + index) * sizeof(record), sizeof(record));
			RID asset = RID::from_uint64(record.asset);
			Ref<MicroGeometryData> source = storage->get_source(asset);
			ERR_FAIL_COND_V(source.is_null() || record.cluster >= uint32_t(source->get_metadata().clusters.size()), false);
			used[asset].insert(source->get_metadata().clusters[record.cluster].group);
		}
	}
	for (uint32_t index = 0; index < p_pass->pins.size();) {
		const Pass::Pin &pin = p_pass->pins[index];
		if (!used.has(pin.asset) || !used[pin.asset].has(pin.group)) {
			storage->unpin_group(pin.asset, pin.group);
			p_pass->pins.remove_at(index);
		} else {
			index++;
		}
	}
	p_pass->frozen = true;
	RENDER_TIMESTAMP("Microgeometry Freeze Complete");
	return true;
}

void MicroGeometrySelection::build_depth_pyramid(DepthPyramid &r_pyramid, RID p_depth, RID p_classification, const Size2i &p_size) {
	RENDER_TIMESTAMP("Microgeometry HZB");
	Size2i size(Math::nearest_power_of_2_templated(p_size.x), Math::nearest_power_of_2_templated(p_size.y));
	if (r_pyramid.texture.is_valid() && r_pyramid.size != size) {
		RD::get_singleton()->free_rid(r_pyramid.texture);
		r_pyramid.texture = RID();
		r_pyramid.levels.clear();
	}
	if (r_pyramid.texture.is_null()) {
		RD::TextureFormat format;
		format.width = size.x;
		format.height = size.y;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		format.mipmaps = Math::get_shift_from_power_of_2(uint32_t(MAX(size.x, size.y))) + 1;
		r_pyramid.texture = RD::get_singleton()->texture_create(format, RD::TextureView());
		ERR_FAIL_COND(r_pyramid.texture.is_null());
		r_pyramid.size = size;
		for (uint32_t level = 0; level < format.mipmaps; level++) {
			r_pyramid.levels.push_back(RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), r_pyramid.texture, 0, level));
		}
	}
	RD::get_singleton()->draw_command_begin_label("Microgeometry Depth Pyramid");
	for (uint32_t level = 0; level < r_pyramid.levels.size(); level++) {
		uint32_t push[4] = { uint32_t(level == 0 ? p_size.x : MAX(1, size.x >> (level - 1))), uint32_t(level == 0 ? p_size.y : MAX(1, size.y >> (level - 1))), level == 0 ? 1u : 0u, 0 };
		RID source = level == 0 ? p_depth : r_pyramid.levels[level - 1];
		RID uniform = UniformSetCacheRD::get_singleton()->get_cache(hzb_shader.version_get_shader(hzb_version, 0), 0, RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, source), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, r_pyramid.levels[level]), RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_classification));
		RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(list, hzb_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(list, uniform, 0);
		RD::get_singleton()->compute_list_set_push_constant(list, push, sizeof(push));
		RD::get_singleton()->compute_list_dispatch_threads(list, MAX(1, size.x >> level), MAX(1, size.y >> level), 1);
		RD::get_singleton()->compute_list_end();
	}
	RD::get_singleton()->draw_command_end_label();
	RENDER_TIMESTAMP("Microgeometry HZB Complete");
}

RID MicroGeometrySelection::get_raster_uniform_set(Pass *p_pass, RID p_shader) {
	return UniformSetCacheRD::get_singleton()->get_cache(p_shader, 4,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_pass->selected),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_pass->persistent_instances),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_pass->persistent_surfaces),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, p_pass->raster_parameters),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_pass->native_instances),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_pass->counts),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_pass->initial_counts),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_pass->statistics));
}

void MicroGeometrySelection::add_draw_dependencies(Pass *p_pass, RD::DrawListID p_list) {
	for (RID dependency : p_pass->dependencies) {
		RD::get_singleton()->draw_list_add_buffer_dependency(p_list, dependency);
	}
}

void MicroGeometrySelection::submit_feedback(Pass *p_pass) {
	if (p_pass->feedback_active) {
		RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->feedback_submit(p_pass->feedback);
		p_pass->feedback_active = false;
	}
}
