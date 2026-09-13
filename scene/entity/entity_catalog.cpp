#include "entity_catalog.h"

#include "entity_world.h"

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
	pending_compaction_bytes = 0;
}

uint64_t EntityCatalog::_run_bytes() const {
	uint64_t bytes = 0;
	for (const Ref<LocatorRun> &run : locator_runs) {
		const uint64_t added = uint64_t(run->entries.size()) * sizeof(LocatorEntry);
		if (added > UINT64_MAX - bytes) {
			return UINT64_MAX;
		}
		bytes += added;
	}
	for (const Ref<ChildRun> &run : child_runs) {
		const uint64_t added = (uint64_t(run->by_child.size()) + uint64_t(run->by_parent.size())) * sizeof(ChildEntry);
		if (added > UINT64_MAX - bytes) {
			return UINT64_MAX;
		}
		bytes += added;
	}
	return bytes;
}

void EntityCatalog::_record_run_high_water() {
	max_locator_runs = MAX(max_locator_runs, uint64_t(locator_runs.size()));
	max_child_runs = MAX(max_child_runs, uint64_t(child_runs.size()));
	max_run_bytes = MAX(max_run_bytes, _run_bytes());
}

Error EntityCatalog::_ensure_run_capacity(uint32_t p_locator_runs, uint32_t p_child_runs, uint64_t p_bytes) {
	_collect_compaction();
	const uint64_t current_bytes = _run_bytes();
	const bool bounded_runs = p_locator_runs <= RUN_HARD_CAP - MIN(uint32_t(locator_runs.size()), RUN_HARD_CAP) && p_child_runs <= RUN_HARD_CAP - MIN(uint32_t(child_runs.size()), RUN_HARD_CAP);
	const bool bounded_bytes = current_bytes <= RUN_BYTE_HARD_CAP && p_bytes <= RUN_BYTE_HARD_CAP - current_bytes;
	const bool bounded = bounded_runs && bounded_bytes;
	if (bounded) {
		return OK;
	}
	_schedule_compaction(true);
	_collect_compaction();
	const uint64_t collected_bytes = _run_bytes();
	const bool collected_runs = p_locator_runs <= RUN_HARD_CAP - MIN(uint32_t(locator_runs.size()), RUN_HARD_CAP) && p_child_runs <= RUN_HARD_CAP - MIN(uint32_t(child_runs.size()), RUN_HARD_CAP);
	const bool collected_capacity = collected_bytes <= RUN_BYTE_HARD_CAP && p_bytes <= RUN_BYTE_HARD_CAP - collected_bytes;
	if (collected_runs && collected_capacity) {
		return OK;
	}
	run_backpressure_count++;
	if (OS::get_singleton()->is_use_benchmark_set()) {
		print_line(vformat("Entity catalog backpressure locator_runs=%d child_runs=%d current_bytes=%d pending_bytes=%d requested_bytes=%d hard_runs=%d hard_bytes=%d count=%d", int64_t(locator_runs.size()), int64_t(child_runs.size()), int64_t(collected_bytes), int64_t(pending_compaction_bytes), int64_t(p_bytes), int64_t(RUN_HARD_CAP), int64_t(RUN_BYTE_HARD_CAP), int64_t(run_backpressure_count)));
	}
	return ERR_BUSY;
}

Error EntityCatalog::_prepare_child_publication(uint32_t p_rows) {
	return _ensure_run_capacity(0, p_rows ? 1 : 0, uint64_t(p_rows) * sizeof(ChildEntry) * 2);
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
	_publish_locator_entries(std::move(run->entries));
}

void EntityCatalog::_publish_locator_entries(Vector<LocatorEntry> &&p_entries) {
	if (p_entries.is_empty()) {
		return;
	}
	ERR_FAIL_COND(locator_runs.size() >= RUN_HARD_CAP);
	const uint64_t current_bytes = _run_bytes();
	const uint64_t added_bytes = uint64_t(p_entries.size()) * sizeof(LocatorEntry);
	ERR_FAIL_COND(current_bytes > RUN_BYTE_HARD_CAP || added_bytes > RUN_BYTE_HARD_CAP - current_bytes);
	Ref<LocatorRun> run;
	run.instantiate();
	run->entries = std::move(p_entries);
	run->entries.sort_custom<LocatorOrder>();
	locator_runs.push_back(run);
	_record_run_high_water();
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
	_publish_child_entries(std::move(run->by_child));
}

void EntityCatalog::_publish_child_entries(Vector<ChildEntry> &&p_entries) {
	if (p_entries.is_empty()) {
		return;
	}
	ERR_FAIL_COND(child_runs.size() >= RUN_HARD_CAP);
	const uint64_t current_bytes = _run_bytes();
	const uint64_t added_bytes = uint64_t(p_entries.size()) * sizeof(ChildEntry) * 2;
	ERR_FAIL_COND(current_bytes > RUN_BYTE_HARD_CAP || added_bytes > RUN_BYTE_HARD_CAP - current_bytes);
	Ref<ChildRun> run;
	run.instantiate();
	run->by_child = std::move(p_entries);
	run->by_child.sort_custom<ChildOrder>();
	for (const ChildEntry &entry : run->by_child) {
		if (entry.location.is_valid()) {
			run->by_parent.push_back(entry);
		}
	}
	run->by_parent.sort_custom<ParentRunOrder>();
	child_runs.push_back(run);
	_record_run_high_water();
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
	last_publish_error = OK;
	ERR_FAIL_COND_V(p_ids.size() != p_records.size(), UINT32_MAX);
	Vector<EntityId> sorted_ids = p_ids;
	sorted_ids.sort_custom<IdOrder>();
	for (int32_t i = 1; i < sorted_ids.size(); i++) {
		ERR_FAIL_COND_V(sorted_ids[i] == sorted_ids[i - 1], UINT32_MAX);
	}
	for (EntityId id : p_ids) {
		ERR_FAIL_COND_V(!id.is_valid() || has_record(id), UINT32_MAX);
	}
	const uint64_t run_bytes = uint64_t(p_ids.size()) * (sizeof(LocatorEntry) + sizeof(ChildEntry) * 2);
	last_publish_error = _ensure_run_capacity(1, 1, run_bytes);
	ERR_FAIL_COND_V(last_publish_error != OK, UINT32_MAX);
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
	uint32_t index = blocks.size();
	for (uint32_t slot = 0; slot < blocks.size(); slot++) {
		if (!blocks[slot]) {
			index = slot;
			break;
		}
	}
	if (index == blocks.size()) {
		blocks.push_back(block);
	} else {
		blocks[index] = block;
	}
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
	if (add_block(ids, records) != UINT32_MAX) {
		return OK;
	}
	return last_publish_error == OK ? ERR_CANT_CREATE : last_publish_error;
}

bool EntityCatalog::erase_record(EntityId p_id) {
	const RowLocation location = locate(p_id);
	if (!location.is_valid()) {
		return false;
	}
	Vector<RowLocation> rows;
	rows.push_back(location);
	return retire_rows(rows) == OK;
}

Error EntityCatalog::build_release_plan(const Vector<EntityId> &p_members, ReleasePlan &r_plan) {
	r_plan = ReleasePlan();
	struct BlockMarks {
		uint32_t block = UINT32_MAX;
		Vector<uint8_t> unload;
		Vector<uint8_t> retire;
		Vector<uint8_t> unload_blocked;
		Vector<uint8_t> retire_blocked;
		Vector<RowLocation> parent;
	};
	LocalVector<BlockMarks> marks;
	Vector<int32_t> block_marks;
	block_marks.resize(blocks.size());
	for (int32_t &index : block_marks) {
		index = -1;
	}
	for (EntityId id : p_members) {
		const RowLocation location = locate(id);
		const CellRuntimeBlock *block = get_block(location.block);
		const RowState *state = get_state_ptr(location);
		const Record *record = get_record(location);
		if (!block || !state || !record) {
			continue;
		}
		int32_t mark_index = block_marks[location.block];
		if (mark_index < 0) {
			mark_index = marks.size();
			block_marks.write[location.block] = mark_index;
			BlockMarks mark;
			mark.block = location.block;
			mark.unload.resize_initialized(block->ids.size());
			mark.retire.resize_initialized(block->ids.size());
			mark.unload_blocked.resize_initialized(block->ids.size());
			mark.retire_blocked.resize_initialized(block->ids.size());
			mark.parent.resize(block->ids.size());
			marks.push_back(std::move(mark));
		}
		BlockMarks &mark = marks[mark_index];
		mark.parent.write[location.row] = record->parent.id.is_valid() ? locate(record->parent.id) : RowLocation();
		const bool unload = state->resident && !state->pin_count && !state->document_dirty;
		const bool retire = block->ordinary_cell && !state->document_dirty && !state->pin_count && !state->prefab && !state->dependency && !state->tombstone && (!state->resident || unload);
		mark.unload.write[location.row] = unload;
		mark.retire.write[location.row] = retire;
	}

	for (BlockMarks &mark : marks) {
		const CellRuntimeBlock *block = get_block(mark.block);
		for (uint32_t parent = 0; parent < uint32_t(block->ids.size()); parent++) {
			if (!mark.unload[parent] && !mark.retire[parent]) {
				continue;
			}
			for (uint32_t edge = block->child_offsets[parent]; edge < block->child_offsets[parent + 1]; edge++) {
				const uint32_t child = block->child_rows[edge];
				const RowState &child_state = block->states[child];
				if (child_state.active && child_state.resident && !mark.unload[child]) {
					mark.unload_blocked.write[parent] = true;
				}
				if (child_state.active && !mark.retire[child]) {
					mark.retire_blocked.write[parent] = true;
				}
			}
		}
	}

	struct CandidateParent {
		EntityId id;
		uint32_t mark = 0;
		uint32_t row = 0;
	};
	struct CandidateOrder {
		bool operator()(const CandidateParent &p_left, const CandidateParent &p_right) const { return EntityCatalog::_id_less(p_left.id, p_right.id); }
	};
	Vector<CandidateParent> parents;
	for (uint32_t mark_index = 0; mark_index < marks.size(); mark_index++) {
		const BlockMarks &mark = marks[mark_index];
		const CellRuntimeBlock *block = get_block(mark.block);
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (mark.unload[row] || mark.retire[row]) {
				parents.push_back({ block->ids[row], mark_index, row });
			}
		}
	}
	parents.sort_custom<CandidateOrder>();
	Vector<uint32_t> edge_cursors;
	edge_cursors.resize_initialized(child_runs.size());
	uint32_t parent_index = 0;
	while (parent_index < uint32_t(parents.size())) {
		int32_t best_run = -1;
		for (uint32_t run_index = 0; run_index < child_runs.size(); run_index++) {
			if (edge_cursors[run_index] >= uint32_t(child_runs[run_index]->by_parent.size())) {
				continue;
			}
			if (best_run < 0 || ParentRunOrder()(child_runs[run_index]->by_parent[edge_cursors[run_index]], child_runs[best_run]->by_parent[edge_cursors[best_run]])) {
				best_run = run_index;
			}
		}
		if (best_run < 0) {
			break;
		}
		const ChildEntry current_edge = child_runs[best_run]->by_parent[edge_cursors[best_run]];
		int32_t newest_run = best_run;
		for (uint32_t run_index = 0; run_index < child_runs.size(); run_index++) {
			if (edge_cursors[run_index] >= uint32_t(child_runs[run_index]->by_parent.size())) {
				continue;
			}
			const ChildEntry &candidate_edge = child_runs[run_index]->by_parent[edge_cursors[run_index]];
			if (candidate_edge.parent == current_edge.parent && candidate_edge.child == current_edge.child) {
				newest_run = MAX(newest_run, int32_t(run_index));
				edge_cursors.write[run_index]++;
			}
		}
		const ChildEntry &effective_edge = child_runs[newest_run]->by_parent[edge_cursors[newest_run] - 1];
		while (parent_index < uint32_t(parents.size()) && _id_less(parents[parent_index].id, effective_edge.parent)) {
			parent_index++;
		}
		if (parent_index >= uint32_t(parents.size())) {
			break;
		}
		if (_id_less(effective_edge.parent, parents[parent_index].id)) {
			continue;
		}
		const Record *record = get_record(effective_edge.location);
		const RowState *child_state = get_state_ptr(effective_edge.location);
		if (!record || !child_state || record->deleted || record->parent.id != effective_edge.parent) {
			continue;
		}
		bool child_unload = false;
		bool child_retire = false;
		if (effective_edge.location.block < uint32_t(block_marks.size())) {
			const int32_t child_mark_index = block_marks[effective_edge.location.block];
			if (child_mark_index >= 0) {
				const BlockMarks &child_mark = marks[child_mark_index];
				child_unload = child_mark.unload[effective_edge.location.row];
				child_retire = child_mark.retire[effective_edge.location.row];
			}
		}
		BlockMarks &parent_mark = marks[parents[parent_index].mark];
		if (child_state->resident && !child_unload) {
			parent_mark.unload_blocked.write[parents[parent_index].row] = true;
		}
		if (!child_retire) {
			parent_mark.retire_blocked.write[parents[parent_index].row] = true;
		}
	}

	Vector<RowLocation> unload_queue;
	Vector<RowLocation> retire_queue;
	for (const BlockMarks &mark : marks) {
		const CellRuntimeBlock *block = get_block(mark.block);
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (mark.unload_blocked[row]) {
				unload_queue.push_back({ mark.block, block->generation, row });
			}
			if (mark.retire_blocked[row]) {
				retire_queue.push_back({ mark.block, block->generation, row });
			}
		}
	}
	for (uint32_t index = 0; index < uint32_t(unload_queue.size()); index++) {
		const RowLocation parent = marks[block_marks[unload_queue[index].block]].parent[unload_queue[index].row];
		if (!parent.is_valid() || parent.block >= uint32_t(block_marks.size()) || block_marks[parent.block] < 0) {
			continue;
		}
		BlockMarks &mark = marks[block_marks[parent.block]];
		if (mark.unload[parent.row] && !mark.unload_blocked[parent.row]) {
			mark.unload_blocked.write[parent.row] = true;
			unload_queue.push_back(parent);
		}
	}
	for (uint32_t index = 0; index < uint32_t(retire_queue.size()); index++) {
		const RowLocation parent = marks[block_marks[retire_queue[index].block]].parent[retire_queue[index].row];
		if (!parent.is_valid() || parent.block >= uint32_t(block_marks.size()) || block_marks[parent.block] < 0) {
			continue;
		}
		BlockMarks &mark = marks[block_marks[parent.block]];
		if (mark.retire[parent.row] && !mark.retire_blocked[parent.row]) {
			mark.retire_blocked.write[parent.row] = true;
			retire_queue.push_back(parent);
		}
	}

	bool any_retained = false;
	bool any_retiring = false;
	uint64_t publication_bytes = 0;
	for (const BlockMarks &mark : marks) {
		const CellRuntimeBlock *block = get_block(mark.block);
		uint32_t retiring = 0;
		uint32_t retained = 0;
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (mark.retire[row] && !mark.retire_blocked[row]) {
				r_plan.retiring.push_back({ mark.block, block->generation, row });
				r_plan.retiring_ids.push_back(block->ids[row]);
				retiring++;
			} else if (block->states[row].active) {
				retained++;
			}
		}
		if (retiring) {
			any_retiring = true;
			any_retained |= retained > 0;
			publication_bytes += uint64_t(retiring + retained) * (sizeof(LocatorEntry) + sizeof(ChildEntry) * 2);
		}
	}
	Vector<Vector<uint32_t>> unload_child_counts;
	unload_child_counts.resize(marks.size());
	Vector<RowLocation> leaves;
	uint32_t unload_count = 0;
	for (uint32_t mark_index = 0; mark_index < marks.size(); mark_index++) {
		const BlockMarks &mark = marks[mark_index];
		const CellRuntimeBlock *block = get_block(mark.block);
		unload_child_counts.write[mark_index].resize_initialized(block->ids.size());
	}
	for (uint32_t mark_index = 0; mark_index < marks.size(); mark_index++) {
		const BlockMarks &mark = marks[mark_index];
		const CellRuntimeBlock *block = get_block(mark.block);
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (!mark.unload[row] || mark.unload_blocked[row]) {
				continue;
			}
			unload_count++;
			const RowLocation parent = mark.parent[row];
			if (parent.is_valid() && parent.block < uint32_t(block_marks.size()) && block_marks[parent.block] >= 0) {
				const BlockMarks &parent_mark = marks[block_marks[parent.block]];
				if (parent_mark.unload[parent.row] && !parent_mark.unload_blocked[parent.row]) {
					unload_child_counts.write[block_marks[parent.block]].write[parent.row]++;
				}
			}
		}
	}
	for (uint32_t mark_index = 0; mark_index < marks.size(); mark_index++) {
		const BlockMarks &mark = marks[mark_index];
		const CellRuntimeBlock *block = get_block(mark.block);
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (mark.unload[row] && !mark.unload_blocked[row] && unload_child_counts[mark_index][row] == 0) {
				leaves.push_back({ mark.block, block->generation, row });
			}
		}
	}
	for (uint32_t index = 0; index < uint32_t(leaves.size()); index++) {
		const RowLocation location = leaves[index];
		const BlockMarks &mark = marks[block_marks[location.block]];
		const CellRuntimeBlock *block = get_block(location.block);
		r_plan.unloading.push_back(block->ids[location.row]);
		const RowLocation parent = mark.parent[location.row];
		if (parent.is_valid() && parent.block < uint32_t(block_marks.size()) && block_marks[parent.block] >= 0) {
			const int32_t parent_mark_index = block_marks[parent.block];
			const BlockMarks &parent_mark = marks[parent_mark_index];
			if (parent_mark.unload[parent.row] && !parent_mark.unload_blocked[parent.row] && --unload_child_counts.write[parent_mark_index].write[parent.row] == 0) {
				leaves.push_back(parent);
			}
		}
	}
	ERR_FAIL_COND_V(uint32_t(r_plan.unloading.size()) != unload_count, ERR_CYCLIC_LINK);
	r_plan.retiring_ids.sort_custom<IdOrder>();
	if (any_retiring) {
		const uint32_t publications = 1 + uint32_t(any_retained);
		const Error capacity = _ensure_run_capacity(publications, publications, publication_bytes);
		if (capacity != OK) {
			r_plan = ReleasePlan();
			return capacity;
		}
	}
	return OK;
}

Error EntityCatalog::retire_rows(const Vector<RowLocation> &p_rows, EntityWorld *p_world) {
	Vector<RowLocation> rows = p_rows;
	struct RowOrder {
		bool operator()(const RowLocation &p_left, const RowLocation &p_right) const {
			return p_left.block != p_right.block ? p_left.block < p_right.block : p_left.row < p_right.row;
		}
	};
	rows.sort_custom<RowOrder>();
	bool any_retained = false;
	uint32_t total_retiring = 0;
	uint64_t publication_bytes = 0;
	for (uint32_t begin = 0; begin < uint32_t(rows.size());) {
		const uint32_t block_index = rows[begin].block;
		const CellRuntimeBlock *block = get_block(block_index);
		uint32_t end = begin;
		Vector<uint8_t> selected;
		if (block) {
			selected.resize_initialized(block->ids.size());
		}
		while (end < uint32_t(rows.size()) && rows[end].block == block_index) {
			const RowLocation location = rows[end++];
			const RowState *state = get_state_ptr(location);
			if (state && state->active && !selected[location.row]) {
				ERR_FAIL_COND_V(state->resident, ERR_BUSY);
				selected.write[location.row] = true;
				total_retiring++;
			}
		}
		if (block) {
			uint32_t retained = 0;
			for (uint32_t row = 0; row < uint32_t(block->states.size()); row++) {
				retained += block->states[row].active && !selected[row];
			}
			any_retained |= retained > 0;
			publication_bytes += uint64_t(retained) * (sizeof(LocatorEntry) + sizeof(ChildEntry) * 2);
		}
		begin = end;
	}
	publication_bytes += uint64_t(total_retiring) * (sizeof(LocatorEntry) + sizeof(ChildEntry));
	const uint32_t publications = total_retiring ? 1 + uint32_t(any_retained) : 0;
	ERR_FAIL_COND_V(_ensure_run_capacity(publications, publications, publication_bytes) != OK, ERR_BUSY);
	struct Replacement {
		uint32_t index = UINT32_MAX;
		CellRuntimeBlock *old_block = nullptr;
		CellRuntimeBlock *new_block = nullptr;
		Vector<RowLocation> old_locations;
	};
	LocalVector<Replacement> replacements;
	Vector<LocatorEntry> retired_locators;
	Vector<ChildEntry> retired_children;
	uint32_t begin = 0;
	while (begin < uint32_t(rows.size())) {
		const uint32_t block_index = rows[begin].block;
		CellRuntimeBlock *block = get_block(block_index);
		uint32_t end = begin;
		Vector<uint8_t> retired;
		if (block) {
			retired.resize_initialized(block->ids.size());
		}
		while (end < uint32_t(rows.size()) && rows[end].block == block_index) {
			const RowLocation location = rows[end++];
			const RowState *state = get_state_ptr(location);
			if (!block || !state || !state->active || retired[location.row]) {
				continue;
			}
			retired.write[location.row] = true;
		}
		if (block) {
			CellRuntimeBlock *replacement = memnew(CellRuntimeBlock);
			replacement->generation = next_block_generation++;
			replacement->grid = block->grid;
			replacement->x = block->x;
			replacement->y = block->y;
			replacement->z = block->z;
			replacement->ordinary_cell = block->ordinary_cell;
			Vector<RowLocation> old_locations;
			for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
				if (!block->states[row].active) {
					continue;
				}
				if (retired[row]) {
					retired_locators.push_back({ block->ids[row], RowLocation() });
					retired_children.push_back({ block->ids[row], EntityId(), RowLocation() });
					continue;
				}
				old_locations.push_back({ block_index, block->generation, row });
				replacement->ids.push_back(block->ids[row]);
				replacement->base.push_back(*get_record({ block_index, block->generation, row }));
				RowState state = block->states[row];
				state.overlay = UINT32_MAX;
				replacement->states.push_back(state);
				Vector<uint64_t> signature;
				for (const ArchetypeSpan &span : block->archetypes) {
					if (row >= span.first && row < span.first + span.count) {
						signature = span.signature;
						break;
					}
				}
				const uint32_t new_row = replacement->ids.size() - 1;
				if (!replacement->archetypes.is_empty() && replacement->archetypes[replacement->archetypes.size() - 1].signature == signature && replacement->archetypes[replacement->archetypes.size() - 1].first + replacement->archetypes[replacement->archetypes.size() - 1].count == new_row) {
					replacement->archetypes.write[replacement->archetypes.size() - 1].count++;
				} else {
					replacement->archetypes.push_back({ signature, new_row, 1 });
				}
			}
			replacements.push_back({ block_index, block, replacement, std::move(old_locations) });
		}
		begin = end;
	}
	_publish_locator_entries(std::move(retired_locators));
	_publish_child_entries(std::move(retired_children));
	Vector<LocatorEntry> retained_locators;
	for (Replacement &replacement : replacements) {
		blocks[replacement.index] = replacement.new_block->ids.is_empty() ? nullptr : replacement.new_block;
		for (uint32_t row = 0; row < uint32_t(replacement.new_block->ids.size()); row++) {
			retained_locators.push_back({ replacement.new_block->ids[row], { replacement.index, replacement.new_block->generation, row } });
		}
	}
	_publish_locator_entries(std::move(retained_locators));
	Vector<ChildEntry> retained_children;
	for (Replacement &replacement : replacements) {
		if (replacement.new_block->ids.is_empty()) {
			continue;
		}
		_rebuild_block_indices(replacement.index);
		for (uint32_t row = 0; row < uint32_t(replacement.new_block->ids.size()); row++) {
			const RowLocation location{ replacement.index, replacement.new_block->generation, row };
			const Record *record = get_record(location);
			const RowLocation parent = record && record->parent.id.is_valid() ? locate(record->parent.id) : RowLocation();
			const bool external = record && !record->deleted && parent.is_valid() && parent.block != replacement.index;
			retained_children.push_back({ replacement.new_block->ids[row], external ? record->parent.id : EntityId(), external ? location : RowLocation() });
		}
	}
	_publish_child_entries(std::move(retained_children));
	for (Replacement &replacement : replacements) {
		if (p_world) {
			for (uint32_t row = 0; row < uint32_t(replacement.new_block->states.size()); row++) {
				p_world->remap_catalog_row(replacement.new_block->states[row].handle, replacement.old_locations[row], { replacement.index, replacement.new_block->generation, row });
			}
		}
		if (replacement.new_block->ids.is_empty()) {
			memdelete(replacement.new_block);
		}
		memdelete(replacement.old_block);
	}
	active_records -= total_retiring;
	maintenance();
	return OK;
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

Error EntityCatalog::_refresh_parent(EntityId p_id) {
	Vector<EntityId> ids;
	ids.push_back(p_id);
	return _refresh_parents(ids);
}

Error EntityCatalog::_refresh_parents(const Vector<EntityId> &p_ids) {
	ERR_FAIL_COND_V(_prepare_child_publication(p_ids.size()) != OK, ERR_BUSY);
	Vector<uint8_t> rebuild;
	rebuild.resize_initialized(blocks.size());
	Vector<ChildEntry> entries;
	entries.reserve(p_ids.size());
	for (EntityId id : p_ids) {
		const RowLocation location = locate(id);
		if (!location.is_valid()) {
			continue;
		}
		rebuild.write[location.block] = true;
	}
	for (uint32_t block = 0; block < uint32_t(rebuild.size()); block++) {
		if (rebuild[block]) {
			_rebuild_block_indices(block);
		}
	}
	for (EntityId id : p_ids) {
		const RowLocation location = locate(id);
		if (!location.is_valid()) {
			continue;
		}
		const Record *record = get_record(location);
		const RowLocation parent = record && record->parent.id.is_valid() ? locate(record->parent.id) : RowLocation();
		const bool external = record && !record->deleted && parent.is_valid() && parent.block != location.block;
		entries.push_back({ id, external ? record->parent.id : EntityId(), external ? location : RowLocation() });
	}
	_publish_child_entries(std::move(entries));
	maintenance();
	return OK;
}

Error EntityCatalog::_set_parent(EntityId p_id, EntityRef p_parent) {
	ERR_FAIL_COND_V(_prepare_child_publication(1) != OK, ERR_BUSY);
	const RowLocation location = locate(p_id);
	Record *record = edit_record(location);
	if (record) {
		record->parent = p_parent;
		return _refresh_parent(p_id);
	}
	return ERR_DOES_NOT_EXIST;
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
	retained_bytes = uint64_t(locator_result->entries.size()) * sizeof(LocatorEntry) + (uint64_t(child_result->by_child.size()) + uint64_t(child_result->by_parent.size())) * sizeof(ChildEntry);
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
	pending_compaction_bytes = 0;
	_record_run_high_water();
	if (job->profile) {
		print_line(vformat("Entity catalog compaction locator_runs=%d child_runs=%d worker_us=%d owner_us=%d retained_bytes=%d current_bytes=%d pending_bytes=%d hard_runs=%d hard_bytes=%d max_locator_runs=%d max_child_runs=%d max_bytes=%d backpressure=%d valid=%d", int64_t(locator_runs.size()), int64_t(child_runs.size()), int64_t(job->worker_usec), int64_t(OS::get_singleton()->get_ticks_usec() - began), int64_t(job->retained_bytes), int64_t(_run_bytes()), int64_t(pending_compaction_bytes), int64_t(RUN_HARD_CAP), int64_t(RUN_BYTE_HARD_CAP), int64_t(max_locator_runs), int64_t(max_child_runs), int64_t(max_run_bytes), int64_t(run_backpressure_count), int64_t(valid)));
	}
	memdelete(job);
	compaction_job = nullptr;
}

void EntityCatalog::_schedule_compaction(bool p_force) {
	if (shutting_down || compaction_job || (!p_force && locator_runs.size() < RUN_COMPACTION_THRESHOLD && child_runs.size() < RUN_COMPACTION_THRESHOLD)) {
		return;
	}
	EntityTaskScheduler *scheduler = EntityTaskScheduler::get_singleton();
	if (!scheduler) {
		return;
	}
	uint64_t entry_bytes = 0;
	for (const Ref<LocatorRun> &run : locator_runs) {
		const uint64_t added = uint64_t(run->entries.size()) * sizeof(LocatorEntry);
		if (added > UINT64_MAX - entry_bytes) {
			return;
		}
		entry_bytes += added;
	}
	for (const Ref<ChildRun> &run : child_runs) {
		const uint64_t added = (uint64_t(run->by_child.size()) + uint64_t(run->by_parent.size())) * sizeof(ChildEntry);
		if (added > UINT64_MAX - entry_bytes) {
			return;
		}
		entry_bytes += added;
	}
	const uint64_t current_bytes = _run_bytes();
	const uint64_t overhead = sizeof(CompactionJob) + 4096;
	if (current_bytes > RUN_BYTE_HARD_CAP || entry_bytes > (EntityTaskScheduler::Graph::BYTE_BUDGET - overhead) / 4) {
		return;
	}
	EntityTaskScheduler::Reservation reservation;
	if (scheduler->reserve(reservation, overhead + entry_bytes * 4, true) != OK) {
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
	pending_compaction_bytes = entry_bytes;
	_record_run_high_water();
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
