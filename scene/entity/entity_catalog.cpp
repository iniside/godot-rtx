#include "entity_catalog.h"

#include "core/os/memory.h"

bool EntityCatalog::_id_less(EntityId p_left, EntityId p_right) {
	return p_left.high != p_right.high ? p_left.high < p_right.high : p_left.low < p_right.low;
}

EntityCatalog::~EntityCatalog() {
	clear();
}

void EntityCatalog::clear() {
	for (CellRuntimeBlock *block : blocks) {
		if (block) {
			memdelete(block);
		}
	}
	blocks.clear();
	for (LocatorRun *run : locator_runs) {
		memdelete(run);
	}
	locator_runs.clear();
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
		const LocatorEntry *entry = _find_in_run(*locator_runs[i], p_id);
		if (!entry) {
			continue;
		}
		const RowState *state = get_state_ptr(entry->location);
		const CellRuntimeBlock *block = get_block(entry->location.block);
		if (state && state->active && block && block->ids[entry->location.row] == p_id) {
			return entry->location;
		}
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

void EntityCatalog::_add_locator_run(uint32_t p_block) {
	CellRuntimeBlock *block = get_block(p_block);
	ERR_FAIL_NULL(block);
	LocatorRun *run = memnew(LocatorRun);
	run->entries.resize(block->ids.size());
	for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
		run->entries.write[row] = { block->ids[row], { p_block, block->generation, row } };
	}
	run->entries.sort_custom<LocatorOrder>();
	locator_runs.push_back(run);
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
	block->external_parent_edges.clear();
	for (uint32_t row = 0; row < count; row++) {
		const Record *record = get_record({ p_block, block->generation, row });
		if (!record || record->deleted || !record->parent.id.is_valid()) {
			continue;
		}
		const RowLocation parent = locate(record->parent.id);
		if (parent.is_valid() && parent.block == p_block) {
			indegree.write[row]++;
			block->child_offsets.write[parent.row + 1]++;
		} else {
			block->external_parent_edges.push_back({ record->parent.id, row });
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
	block->external_parent_edges.sort_custom<ParentEdgeOrder>();
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
	}
	const uint32_t index = blocks.size();
	blocks.push_back(block);
	active_records += p_ids.size();
	_add_locator_run(index);
	_rebuild_block_indices(index);
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
	RowLocation location = locate(p_id);
	RowState *state = get_state_ptr(location);
	if (!state) {
		return false;
	}
	state->active = false;
	active_records--;
	if (state->resident) {
		resident_records--;
	}
	state->resident = false;
	state->handle = {};
	state->incarnation++;
	_rebuild_block_indices(location.block);
	bool retained = false;
	for (const RowState &row : get_block(location.block)->states) {
		retained |= row.active;
	}
	if (!retained) {
		memdelete(blocks[location.block]);
		blocks[location.block] = nullptr;
	}
	return true;
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
	if (parent.is_valid()) {
		const CellRuntimeBlock *parent_block = blocks[parent.block];
		for (uint32_t index = parent_block->child_offsets[parent.row]; index < parent_block->child_offsets[parent.row + 1]; index++) {
			const uint32_t child = parent_block->child_rows[index];
			const Record *record = get_record({ parent.block, parent.generation, child });
			if (record && !record->deleted) {
				result.push_back(parent_block->ids[child]);
			}
		}
	}
	for (uint32_t block_index = 0; block_index < blocks.size(); block_index++) {
		const CellRuntimeBlock *block = blocks[block_index];
		if (!block) {
			continue;
		}
		int32_t low = 0;
		int32_t high = block->external_parent_edges.size() - 1;
		while (low <= high) {
			const int32_t middle = low + (high - low) / 2;
			const EntityId candidate = block->external_parent_edges[middle].parent;
			if (_id_less(candidate, p_id)) {
				low = middle + 1;
			} else {
				high = middle - 1;
			}
		}
		for (int32_t edge = low; edge < block->external_parent_edges.size() && block->external_parent_edges[edge].parent == p_id; edge++) {
			const uint32_t child = block->external_parent_edges[edge].child;
			const Record *record = get_record({ block_index, block->generation, child });
			if (record && !record->deleted) {
				result.push_back(block->ids[child]);
			}
		}
	}
	return result;
}

void EntityCatalog::_unlink_parent(EntityId p_id) {
	(void)p_id;
}

void EntityCatalog::_set_parent(EntityId p_id, EntityRef p_parent) {
	RowLocation location = locate(p_id);
	Record *record = edit_record(location);
	if (record) {
		record->parent = p_parent;
		_rebuild_block_indices(location.block);
	}
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
