#include "entity_catalog.h"

#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

bool EntityCatalog::_id_less(EntityId p_left, EntityId p_right) {
	return p_left.high != p_right.high ? p_left.high < p_right.high : p_left.low < p_right.low;
}

EntityCatalog::~EntityCatalog() {
	flush_maintenance();
	clear();
}

void EntityCatalog::clear() {
	flush_maintenance();
	for (CellRuntimeBlock *block : blocks) {
		if (block) {
			memdelete(block);
		}
	}
	blocks.clear();
	locator_runs.clear();
	child_runs.clear();
	active_records = 0;
	resident_records = 0;
}

const EntityCatalog::LocatorEntry *EntityCatalog::_find_in_run(const LocatorRun &p_run, EntityId p_id) const {
	int32_t low = 0;
	int32_t high = p_run.entries.size() - 1;
	while (low <= high) {
		const int32_t middle = low + (high - low) / 2;
		const LocatorEntry &entry = p_run.entries[middle];
		if (entry.id == p_id) {
			return &entry;
		}
		if (_id_less(entry.id, p_id)) {
			low = middle + 1;
		} else {
			high = middle - 1;
		}
	}
	return nullptr;
}

EntityCatalog::RowLocation EntityCatalog::locate(EntityId p_id) const {
	for (int32_t i = locator_runs.size() - 1; i >= 0; i--) {
		const LocatorEntry *entry = _find_in_run(*locator_runs[i].ptr(), p_id);
		if (!entry) {
			continue;
		}
		if (!entry->location.is_valid()) {
			return {};
		}
		const RowState *state = get_state_ptr(entry->location);
		const CellRuntimeBlock *block = get_block(entry->location.block);
		if (state && state->active && block && block->ids[entry->location.row] == p_id) {
			return entry->location;
		}
		return {};
	}
	return {};
}

EntityCatalog::CellRuntimeBlock *EntityCatalog::get_block(uint32_t p_block) {
	return p_block < blocks.size() ? blocks[p_block] : nullptr;
}

const EntityCatalog::CellRuntimeBlock *EntityCatalog::get_block(uint32_t p_block) const {
	return p_block < blocks.size() ? blocks[p_block] : nullptr;
}

EntityCatalog::RowState *EntityCatalog::get_state_ptr(RowLocation p_location) {
	CellRuntimeBlock *block = get_block(p_location.block);
	return block && block->generation == p_location.generation && p_location.row < uint32_t(block->states.size()) ? &block->states.write[p_location.row] : nullptr;
}

const EntityCatalog::RowState *EntityCatalog::get_state_ptr(RowLocation p_location) const {
	const CellRuntimeBlock *block = get_block(p_location.block);
	return block && block->generation == p_location.generation && p_location.row < uint32_t(block->states.size()) ? &block->states[p_location.row] : nullptr;
}

EntityCatalog::RowState *EntityCatalog::get_state_ptr(EntityId p_id) {
	return get_state_ptr(locate(p_id));
}

const EntityCatalog::RowState *EntityCatalog::get_state_ptr(EntityId p_id) const {
	return get_state_ptr(locate(p_id));
}

const EntityCatalog::Record *EntityCatalog::get_record(RowLocation p_location) const {
	const CellRuntimeBlock *block = get_block(p_location.block);
	const RowState *state = get_state_ptr(p_location);
	if (!block || !state || !state->active) {
		return nullptr;
	}
	return state->overlay == UINT32_MAX ? &block->base[p_location.row] : &block->overlays[state->overlay];
}

const EntityCatalog::Record *EntityCatalog::get_record(EntityId p_id) const {
	return get_record(locate(p_id));
}

EntityCatalog::Record *EntityCatalog::edit_record(RowLocation p_location) {
	CellRuntimeBlock *block = get_block(p_location.block);
	RowState *state = get_state_ptr(p_location);
	if (!block || !state || !state->active) {
		return nullptr;
	}
	if (state->overlay == UINT32_MAX) {
		state->overlay = block->overlays.size();
		block->overlays.push_back(block->base[p_location.row]);
	}
	return &block->overlays.write[state->overlay];
}

EntityCatalog::Record *EntityCatalog::edit_record(EntityId p_id) {
	return edit_record(locate(p_id));
}

void EntityCatalog::_publish_locator_rows(uint32_t p_block, const Vector<uint32_t> *p_rows, bool p_invalid) {
	CellRuntimeBlock *block = get_block(p_block);
	ERR_FAIL_NULL(block);
	Ref<LocatorRun> run;
	run.instantiate();
	const uint32_t count = p_rows ? p_rows->size() : block->ids.size();
	run->entries.resize(count);
	for (uint32_t index = 0; index < count; index++) {
		const uint32_t row = p_rows ? (*p_rows)[index] : index;
		run->entries.write[index] = { block->ids[row], p_invalid ? RowLocation() : RowLocation{ p_block, block->generation, row } };
	}
	run->entries.sort_custom<LocatorOrder>();
	locator_runs.push_back(run);
}

void EntityCatalog::_publish_child_rows(uint32_t p_block, const Vector<uint32_t> *p_rows, bool p_invalid) {
	CellRuntimeBlock *block = get_block(p_block);
	ERR_FAIL_NULL(block);
	Ref<ChildRun> run;
	run.instantiate();
	const uint32_t count = p_rows ? p_rows->size() : block->ids.size();
	run->by_child.resize(count);
	for (uint32_t index = 0; index < count; index++) {
		const uint32_t row = p_rows ? (*p_rows)[index] : index;
		const RowLocation location{ p_block, block->generation, row };
		const Record *record = p_invalid ? nullptr : get_record(location);
		const RowLocation parent = record && record->parent.id.is_valid() ? locate(record->parent.id) : RowLocation();
		const bool external = record && !record->deleted && parent.is_valid() && parent.block != p_block;
		run->by_child.write[index] = { block->ids[row], external ? record->parent.id : EntityId(), external ? location : RowLocation() };
	}
	run->by_child.sort_custom<ChildOrder>();
	for (const ChildEntry &entry : run->by_child) {
		if (entry.location.is_valid()) {
			run->by_parent.push_back(entry);
		}
	}
	run->by_parent.sort_custom<ParentRunOrder>();
	child_runs.push_back(run);
}

void EntityCatalog::_rebuild_block_indices(uint32_t p_block) {
	CellRuntimeBlock *block = get_block(p_block);
	ERR_FAIL_NULL(block);
	const uint32_t count = block->ids.size();
	block->topological_rows.clear();
	block->topological_rows.reserve(count);
	Vector<uint32_t> indegree;
	indegree.resize_initialized(count);
	block->child_offsets.resize_initialized(count + 1);
	for (uint32_t row = 0; row < count; row++) {
		const Record *record = get_record({ p_block, block->generation, row });
		if (!record || record->deleted || !record->parent.id.is_valid()) {
			continue;
		}
		const RowLocation parent = locate(record->parent.id);
		if (parent.is_valid() && parent.block == p_block) {
			indegree.write[row]++;
			block->child_offsets.write[parent.row + 1]++;
		}
	}
	for (uint32_t row = 1; row <= count; row++) {
		block->child_offsets.write[row] += block->child_offsets[row - 1];
	}
	block->child_rows.resize(block->child_offsets[count]);
	Vector<uint32_t> cursors = block->child_offsets;
	for (uint32_t row = 0; row < count; row++) {
		const Record *record = get_record({ p_block, block->generation, row });
		if (!record || record->deleted) {
			continue;
		}
		const RowLocation parent = locate(record->parent.id);
		if (parent.is_valid() && parent.block == p_block) {
			block->child_rows.write[cursors.write[parent.row]++] = row;
		}
	}
	Vector<uint32_t> ready;
	for (uint32_t row = 0; row < count; row++) {
		const Record *record = get_record({ p_block, block->generation, row });
		if (record && !record->deleted && indegree[row] == 0) {
			ready.push_back(row);
		}
	}
	for (uint32_t index = 0; index < uint32_t(ready.size()); index++) {
		const uint32_t row = ready[index];
		block->topological_rows.push_back(row);
		for (uint32_t child_index = block->child_offsets[row]; child_index < block->child_offsets[row + 1]; child_index++) {
			const uint32_t child = block->child_rows[child_index];
			if (--indegree.write[child] == 0) {
				ready.push_back(child);
			}
		}
	}
}

uint32_t EntityCatalog::add_block(const Vector<EntityId> &p_ids, const Vector<Record> &p_records, const String &p_grid, int32_t p_x, int32_t p_y, int32_t p_z, bool p_ordinary_cell) {
	ERR_FAIL_COND_V(p_ids.size() != p_records.size(), UINT32_MAX);
	Vector<EntityId> sorted_ids = p_ids;
	sorted_ids.sort_custom<IdOrder>();
	for (int32_t i = 1; i < sorted_ids.size(); i++) {
		ERR_FAIL_COND_V(sorted_ids[i] == sorted_ids[i - 1], UINT32_MAX);
	}
	for (EntityId id : p_ids) {
		ERR_FAIL_COND_V(!id.is_valid() || has_record(id), UINT32_MAX);
	}
	CellRuntimeBlock *block = memnew(CellRuntimeBlock);
	block->generation = next_block_generation++;
	block->grid = p_grid;
	block->x = p_x;
	block->y = p_y;
	block->z = p_z;
	block->ordinary_cell = p_ordinary_cell;
	block->ids = p_ids;
	block->base = p_records;
	block->states.resize(p_ids.size());
	for (uint32_t row = 0; row < uint32_t(p_ids.size()); row++) {
		block->states.write[row].tombstone = p_records[row].deleted;
		block->states.write[row].prefab = !p_records[row].prefab_instance.is_empty();
	}
	const uint32_t index = blocks.size();
	blocks.push_back(block);
	active_records += p_ids.size();
	_publish_locator_rows(index);
	_rebuild_block_indices(index);
	_publish_child_rows(index);
	maintenance();
	return index;
}

Error EntityCatalog::add_record(EntityId p_id, EntityRef p_parent) {
	Record record;
	record.parent = p_parent;
	return insert_record(p_id, record);
}

Error EntityCatalog::insert_record(EntityId p_id, const Record &p_record) {
	ERR_FAIL_COND_V(!p_id.is_valid(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(has_record(p_id), ERR_ALREADY_EXISTS);
	ERR_FAIL_COND_V(p_record.parent.id.is_valid() && get_state(p_record.parent.id) != EntityReferenceState::UNLOADED, ERR_INVALID_PARAMETER);
	Vector<EntityId> ids;
	Vector<Record> records;
	ids.push_back(p_id);
	records.push_back(p_record);
	return add_block(ids, records) == UINT32_MAX ? ERR_CANT_CREATE : OK;
}

bool EntityCatalog::erase_record(EntityId p_id) {
	const RowLocation location = locate(p_id);
	if (!location.is_valid()) {
		return false;
	}
	Vector<RowLocation> rows;
	rows.push_back(location);
	retire_rows(rows);
	return true;
}

void EntityCatalog::retire_rows(const Vector<RowLocation> &p_rows) {
	Vector<RowLocation> rows = p_rows;
	struct RowOrder {
		bool operator()(const RowLocation &p_left, const RowLocation &p_right) const {
			return p_left.block != p_right.block ? p_left.block < p_right.block : p_left.row < p_right.row;
		}
	};
	rows.sort_custom<RowOrder>();
	uint32_t begin = 0;
	while (begin < uint32_t(rows.size())) {
		const uint32_t block_index = rows[begin].block;
		CellRuntimeBlock *block = get_block(block_index);
		uint32_t end = begin;
		Vector<uint32_t> retired;
		while (end < uint32_t(rows.size()) && rows[end].block == block_index) {
			const RowLocation location = rows[end++];
			RowState *state = get_state_ptr(location);
			if (!block || !state || !state->active) {
				continue;
			}
			state->active = false;
			active_records--;
			if (state->resident) {
				resident_records--;
			}
			state->resident = false;
			state->handle = {};
			state->incarnation++;
			retired.push_back(location.row);
		}
		if (block && !retired.is_empty()) {
			_publish_locator_rows(block_index, &retired, true);
			_publish_child_rows(block_index, &retired, true);
			bool retained = false;
			for (const RowState &row : block->states) {
				retained |= row.active;
			}
			if (retained) {
				_rebuild_block_indices(block_index);
			} else {
				memdelete(block);
				blocks[block_index] = nullptr;
			}
		}
		begin = end;
	}
	maintenance();
}

Vector<EntityId> EntityCatalog::get_ids() const {
	Vector<EntityId> result;
	result.reserve(get_record_count());
	for (const CellRuntimeBlock *block : blocks) {
		if (!block) {
			continue;
		}
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (block->states[row].active) {
				result.push_back(block->ids[row]);
			}
		}
	}
	return result;
}

Vector<EntityId> EntityCatalog::get_children(EntityId p_id) const {
	Vector<EntityId> result;
	const RowLocation parent = locate(p_id);
	if (!parent.is_valid()) {
		return result;
	}
	const CellRuntimeBlock *parent_block = blocks[parent.block];
	for (uint32_t index = parent_block->child_offsets[parent.row]; index < parent_block->child_offsets[parent.row + 1]; index++) {
		const uint32_t child = parent_block->child_rows[index];
		const Record *record = get_record({ parent.block, parent.generation, child });
		if (record && !record->deleted) {
			result.push_back(parent_block->ids[child]);
		}
	}
	for (int32_t run_index = child_runs.size() - 1; run_index >= 0; run_index--) {
		const Vector<ChildEntry> &edges = child_runs[run_index]->by_parent;
		int32_t low = 0;
		int32_t high = edges.size() - 1;
		while (low <= high) {
			const int32_t middle = low + (high - low) / 2;
			const EntityId candidate = edges[middle].parent;
			if (_id_less(candidate, p_id)) {
				low = middle + 1;
			} else {
				high = middle - 1;
			}
		}
		for (int32_t edge_index = low; edge_index < edges.size() && edges[edge_index].parent == p_id; edge_index++) {
			const ChildEntry &edge = edges[edge_index];
			const Record *record = get_record(edge.location);
			const CellRuntimeBlock *child_block = get_block(edge.location.block);
			if (record && child_block && !record->deleted && record->parent.id == p_id) {
				result.push_back(edge.child);
			}
		}
	}
	result.sort_custom<IdOrder>();
	int32_t unique = 0;
	for (EntityId id : result) {
		if (unique == 0 || result[unique - 1] != id) {
			result.write[unique++] = id;
		}
	}
	result.resize(unique);
	return result;
}

void EntityCatalog::_refresh_parent(EntityId p_id) {
	const RowLocation location = locate(p_id);
	if (!location.is_valid()) {
		return;
	}
	_rebuild_block_indices(location.block);
	Vector<uint32_t> rows;
	rows.push_back(location.row);
	_publish_child_rows(location.block, &rows);
	maintenance();
}

void EntityCatalog::_set_parent(EntityId p_id, EntityRef p_parent) {
	RowLocation location = locate(p_id);
	Record *record = edit_record(location);
	if (record) {
		record->parent = p_parent;
		_refresh_parent(p_id);
	}
}

void EntityCatalog::CompactionJob::prepare(bool) {
	const uint64_t began = profile ? OS::get_singleton()->get_ticks_usec() : 0;
	struct StampedLocator {
		LocatorEntry entry;
		uint32_t age = 0;
	};
	struct StampedLocatorOrder {
		bool operator()(const StampedLocator &p_left, const StampedLocator &p_right) const {
			if (p_left.entry.id == p_right.entry.id) {
				return p_left.age > p_right.age;
			}
			return EntityCatalog::_id_less(p_left.entry.id, p_right.entry.id);
		}
	};
	Vector<StampedLocator> locator_entries;
	for (uint32_t age = 0; age < locator_snapshot.size(); age++) {
		for (const LocatorEntry &entry : locator_snapshot[age]->entries) {
			locator_entries.push_back({ entry, age });
		}
	}
	locator_entries.sort_custom<StampedLocatorOrder>();
	locator_result.instantiate();
	for (int32_t i = 0; i < locator_entries.size();) {
		const StampedLocator &newest = locator_entries[i];
		if (newest.entry.location.is_valid()) {
			locator_result->entries.push_back(newest.entry);
		}
		const EntityId id = newest.entry.id;
		while (++i < locator_entries.size() && locator_entries[i].entry.id == id) {
		}
	}

	struct StampedChild {
		ChildEntry entry;
		uint32_t age = 0;
	};
	struct StampedChildOrder {
		bool operator()(const StampedChild &p_left, const StampedChild &p_right) const {
			if (p_left.entry.child == p_right.entry.child) {
				return p_left.age > p_right.age;
			}
			return EntityCatalog::_id_less(p_left.entry.child, p_right.entry.child);
		}
	};
	Vector<StampedChild> child_entries;
	for (uint32_t age = 0; age < child_snapshot.size(); age++) {
		for (const ChildEntry &entry : child_snapshot[age]->by_child) {
			child_entries.push_back({ entry, age });
		}
	}
	child_entries.sort_custom<StampedChildOrder>();
	child_result.instantiate();
	for (int32_t i = 0; i < child_entries.size();) {
		const StampedChild &newest = child_entries[i];
		if (newest.entry.location.is_valid()) {
			child_result->by_child.push_back(newest.entry);
			child_result->by_parent.push_back(newest.entry);
		}
		const EntityId id = newest.entry.child;
		while (++i < child_entries.size() && child_entries[i].entry.child == id) {
		}
	}
	child_result->by_parent.sort_custom<ParentRunOrder>();
	retained_bytes = uint64_t(locator_result->entries.size()) * sizeof(LocatorEntry) + uint64_t(child_result->by_child.size() + child_result->by_parent.size()) * sizeof(ChildEntry);
	if (profile) {
		worker_usec = OS::get_singleton()->get_ticks_usec() - began;
	}
}

void EntityCatalog::_collect_compaction() {
	if (!compaction_job || compaction_mailbox.is_null()) {
		return;
	}
	EntityTaskScheduler::Graph *completed = compaction_mailbox->pop();
	if (!completed) {
		return;
	}
	completed->ready = true;
	CompactionJob *job = static_cast<CompactionJob *>(completed);
	const uint64_t began = job->profile ? OS::get_singleton()->get_ticks_usec() : 0;
	bool valid = job->locator_snapshot.size() <= locator_runs.size() && job->child_snapshot.size() <= child_runs.size();
	for (uint32_t i = 0; valid && i < job->locator_snapshot.size(); i++) {
		valid = job->locator_snapshot[i].ptr() == locator_runs[i].ptr();
	}
	for (uint32_t i = 0; valid && i < job->child_snapshot.size(); i++) {
		valid = job->child_snapshot[i].ptr() == child_runs[i].ptr();
	}
	if (valid && !job->cancelled.is_set()) {
		LocalVector<Ref<LocatorRun>> compacted_locator;
		if (!job->locator_result->entries.is_empty()) {
			compacted_locator.push_back(job->locator_result);
		}
		for (uint32_t i = job->locator_snapshot.size(); i < locator_runs.size(); i++) {
			compacted_locator.push_back(locator_runs[i]);
		}
		locator_runs = std::move(compacted_locator);
		LocalVector<Ref<ChildRun>> compacted_children;
		if (!job->child_result->by_child.is_empty()) {
			compacted_children.push_back(job->child_result);
		}
		for (uint32_t i = job->child_snapshot.size(); i < child_runs.size(); i++) {
			compacted_children.push_back(child_runs[i]);
		}
		child_runs = std::move(compacted_children);
	}
	if (job->profile) {
		print_line(vformat("Entity catalog compaction locator_runs=%d child_runs=%d worker_us=%d owner_us=%d retained_bytes=%d valid=%d", int64_t(locator_runs.size()), int64_t(child_runs.size()), int64_t(job->worker_usec), int64_t(OS::get_singleton()->get_ticks_usec() - began), int64_t(job->retained_bytes), int64_t(valid)));
	}
	memdelete(job);
	compaction_job = nullptr;
}

void EntityCatalog::_schedule_compaction() {
	if (shutting_down || compaction_job || (locator_runs.size() <= RUN_COMPACTION_THRESHOLD && child_runs.size() <= RUN_COMPACTION_THRESHOLD)) {
		return;
	}
	EntityTaskScheduler *scheduler = EntityTaskScheduler::get_singleton();
	if (!scheduler) {
		return;
	}
	uint64_t entry_bytes = 0;
	for (const Ref<LocatorRun> &run : locator_runs) {
		entry_bytes += uint64_t(run->entries.size()) * sizeof(LocatorEntry);
	}
	for (const Ref<ChildRun> &run : child_runs) {
		entry_bytes += uint64_t(run->by_child.size()) * sizeof(ChildEntry);
	}
	EntityTaskScheduler::Reservation reservation;
	if (scheduler->reserve(reservation, sizeof(CompactionJob) + entry_bytes * 4 + 4096) != OK) {
		return;
	}
	if (compaction_mailbox.is_null()) {
		compaction_mailbox.instantiate();
	}
	CompactionJob *job = memnew(CompactionJob);
	job->profile = OS::get_singleton()->is_use_benchmark_set();
	job->locator_snapshot = locator_runs;
	job->child_snapshot = child_runs;
	scheduler->adopt(job, reservation);
	const Error submitted = scheduler->submit(job, compaction_mailbox, true, EntityTaskScheduler::LOW);
	if (submitted != OK) {
		memdelete(job);
		return;
	}
	compaction_job = job;
}

void EntityCatalog::maintenance() {
	_collect_compaction();
	_schedule_compaction();
}

void EntityCatalog::flush_maintenance() {
	shutting_down = true;
	if (!compaction_job) {
		shutting_down = false;
		return;
	}
	compaction_job->cancelled.set();
	while (compaction_job) {
		_collect_compaction();
		if (compaction_job) {
			OS::get_singleton()->delay_usec(1000);
		}
	}
	shutting_down = false;
}

EntityReferenceState EntityCatalog::get_state(EntityId p_id) const {
	const Record *record = get_record(p_id);
	return record ? (record->deleted ? EntityReferenceState::DELETED : EntityReferenceState::UNLOADED) : EntityReferenceState::MISSING;
}

EntityRef EntityCatalog::get_parent(EntityId p_id) const {
	const Record *record = get_record(p_id);
	return record ? record->parent : EntityRef();
}

int EntityCatalog::get_record_count() const {
	return active_records;
}

int EntityCatalog::get_resident_count() const {
	return resident_records;
}
