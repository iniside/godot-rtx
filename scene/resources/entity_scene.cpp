#ifndef _3D_DISABLED

#include "entity_scene.h"

#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/entity/entity_scene_commands.h"
#include "scene/entity/entity_scene_io.h"
#include "servers/rendering/rendering_server.h"

EntityScene::EntityScene() {
	cell_mailbox.instantiate();
	cleanup_mailbox.instantiate();
	Error error = EntityId::generate(document_id);
	ERR_FAIL_COND(error != OK);
	default_grid = "default";
	default_range = 256.0;
	grids.insert(default_grid, { 64.0, 0.0 });
}

EntityScene::~EntityScene() {
	flush_streaming();
	if (commands) {
		memdelete(commands);
	}
	if (world) {
		memdelete(world);
	}
}

void EntityScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_document_id"), &EntityScene::get_document_id_text);
	ClassDB::bind_method(D_METHOD("get_record_count"), &EntityScene::get_record_count);
	ClassDB::bind_method(D_METHOD("get_resident_count"), &EntityScene::get_resident_count);
	ClassDB::bind_method(D_METHOD("get_last_error"), &EntityScene::get_last_error);
}

Error EntityScene::_owner() {
	if (!owner_thread) {
		owner_thread = Thread::get_caller_id();
	}
	ERR_FAIL_COND_V(owner_thread != Thread::get_caller_id(), ERR_UNAUTHORIZED);
	return OK;
}

const EntityScene::Section *EntityScene::_get_section(EntityId p_id) const {
	const EntityCatalog::Record *record = catalog.get_record(p_id);
	return record && record->has_section ? &record->section : nullptr;
}

EntityScene::Section *EntityScene::_edit_section(EntityId p_id) {
	EntityCatalog::Record *record = catalog.edit_record(p_id);
	if (!record) {
		return nullptr;
	}
	record->has_section = true;
	return &record->section;
}

void EntityScene::_erase_section(EntityId p_id) {
	EntityCatalog::Record *record = catalog.edit_record(p_id);
	if (record && record->has_section) {
		record->section = Section();
		record->has_section = false;
	}
}

const int64_t *EntityScene::_get_order_ptr(EntityId p_id) const {
	const EntityCatalog::Record *record = catalog.get_record(p_id);
	return record && record->has_order ? &record->order : nullptr;
}

void EntityScene::_set_order(EntityId p_id, int64_t p_order) {
	EntityCatalog::Record *record = catalog.edit_record(p_id);
	if (record) {
		record->order = p_order;
		record->has_order = true;
	}
}

void EntityScene::_erase_order(EntityId p_id) {
	EntityCatalog::Record *record = catalog.edit_record(p_id);
	if (record) {
		record->order = 0;
		record->has_order = false;
	}
}

bool EntityScene::_is_dirty(EntityId p_id) const {
	const EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	return state && state->document_dirty;
}

void EntityScene::_set_dirty(EntityId p_id, bool p_dirty) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	if (state) {
		state->document_dirty = p_dirty;
	}
}

bool EntityScene::_is_global(EntityId p_id) const {
	const EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	return state && state->global;
}

void EntityScene::_set_global(EntityId p_id, bool p_global) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	if (state) {
		state->global = p_global;
	}
}

bool EntityScene::_is_pinned(EntityId p_id) const {
	const EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	return state && state->pin_count != 0;
}

void EntityScene::_pin_row(EntityId p_id) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	if (state) {
		state->pin_count++;
	}
}

void EntityScene::_unpin_row(EntityId p_id) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	if (state && state->pin_count) {
		state->pin_count--;
	}
}

bool EntityScene::_cell_members_has(const CellMembers &p_members, EntityId p_id) {
	return _snapshot_has(p_members, p_id);
}

void EntityScene::_cell_members_insert(CellMembers &r_members, EntityId p_id) {
	const int index = r_members.bsearch_custom<SnapshotIdOrder>(p_id, true);
	if (index == r_members.size() || r_members[index] != p_id) {
		r_members.insert(index, p_id);
	}
}

void EntityScene::_cell_members_erase(CellMembers &r_members, EntityId p_id) {
	const int index = r_members.bsearch_custom<SnapshotIdOrder>(p_id, true);
	if (index < r_members.size() && r_members[index] == p_id) {
		r_members.remove_at(index);
	}
}

EntityWorld *EntityScene::get_world() {
	ERR_FAIL_COND_V(_owner() != OK, nullptr);
	if (!world) {
		world = memnew(EntityWorld(catalog));
	}
	return world;
}

EntitySceneCommands &EntityScene::get_commands() {
	DEV_ASSERT(_owner() == OK);
	if (!commands) {
		commands = memnew(EntitySceneCommands(*this));
	}
	return *commands;
}

EntityResolution EntityScene::resolve(EntityId p_id) const {
	return world ? world->resolve({ p_id }) : EntityResolution{ catalog.get_state(p_id), {} };
}

int64_t EntityScene::get_order(EntityId p_id) const {
	const int64_t *value = _get_order_ptr(p_id);
	return value ? *value : 0;
}

Error EntityScene::_fail(EntityId p_id, const String &p_field, Error p_error) {
	last_error = "Entity " + p_id.to_string() + ", field " + p_field + ": " + error_names[p_error];
	ERR_PRINT(last_error);
	return p_error;
}

bool EntityScene::_parse_cell_directory(const String &p_directory, CellKey &r_key) {
	const char32_t *text = p_directory.get_data();
	const int length = p_directory.length();
	const int prefix = 6;
	if (length <= prefix || p_directory[5] != '/' || !p_directory.begins_with("cells/")) {
		return false;
	}
	int separator = prefix;
	while (separator < length && text[separator] != '/') {
		separator++;
	}
	if (separator == prefix || separator >= length - 1) {
		return false;
	}
	int64_t values[3] = { 0, 0, 0 };
	int cursor = separator + 1;
	for (int i = 0; i < 3; i++) {
		if (i > 0) {
			if (cursor >= length || text[cursor] != '_') {
				return false;
			}
			cursor++;
		}
		const bool negative = cursor < length && text[cursor] == '-';
		if (negative) {
			cursor++;
		}
		const int begin = cursor;
		int64_t value = 0;
		while (cursor < length && text[cursor] >= '0' && text[cursor] <= '9') {
			if (cursor - begin > 10) {
				return false;
			}
			value = value * 10 + int64_t(text[cursor] - '0');
			cursor++;
		}
		if (cursor == begin || (text[begin] == '0' && cursor - begin > 1) || (negative && value == 0)) {
			return false;
		}
		value = negative ? -value : value;
		if (value < INT32_MIN || value > INT32_MAX) {
			return false;
		}
		values[i] = value;
	}
	if (cursor != length) {
		return false;
	}
	r_key.grid = p_directory.substr(prefix, separator - prefix);
	r_key.x = int32_t(values[0]);
	r_key.y = int32_t(values[1]);
	r_key.z = int32_t(values[2]);
	return true;
}

int32_t EntityScene::_cell_index(double p_value, double p_size) {
	const double index = Math::floor(p_value / p_size);
	return int32_t(CLAMP(index, double(INT32_MIN), double(INT32_MAX)));
}

String EntityScene::_storage_directory(EntityId p_id, String &r_source) const {
	const Section *section = _get_section(p_id);
	if (section && !section->path.is_empty()) {
		r_source = section->path;
		return section->path.get_base_dir();
	}
	const EntityCatalog::Record *record = catalog.get_record(p_id);
	if (!record || record->prefab_instance.is_empty()) {
		return String();
	}
	const Variant value = prefab_instances.get(record->prefab_instance, Variant());
	if (value.get_type() != Variant::DICTIONARY) {
		return String();
	}
	const Dictionary instance = value;
	if (!instance.has("cell")) {
		return String();
	}
	r_source = String("prefabs").path_join(record->prefab_instance + ".escn");
	const Variant cell = instance["cell"];
	return cell.get_type() == Variant::STRING ? String(cell) : String();
}

Error EntityScene::_assign_cell(EntityId p_id) {
	if (catalog.get_state(p_id) != EntityReferenceState::UNLOADED) {
		return OK;
	}
	String source;
	const String directory = _storage_directory(p_id, source);
	if (directory.is_empty()) {
		return source.is_empty() ? OK : _fail(p_id, "cell/" + source, ERR_INVALID_DATA);
	}
	if (directory == "global") {
		_set_global(p_id, true);
		return OK;
	}
	if (directory != assigned_directory) {
		CellKey key;
		if (!_parse_cell_directory(directory, key) || !grids.has(key.grid)) {
			return _fail(p_id, "cell/" + source, ERR_INVALID_DATA);
		}
		assigned_directory = directory;
		assigned_key = key;
	}
	const EntityCatalog::Record *current = catalog.get_record(p_id);
	ERR_FAIL_NULL_V(current, ERR_DOES_NOT_EXIST);
	const CellKey previous{ current->cell_grid, current->cell_x, current->cell_y, current->cell_z };
	EntityCatalog::Record *record = nullptr;
	if (!current->has_cell || previous != assigned_key) {
		if (current->has_cell) {
			CellMembers *old_members = cells.getptr(previous);
			if (old_members) {
				_cell_members_erase(*old_members, p_id);
			}
		}
		record = catalog.edit_record(p_id);
		ERR_FAIL_NULL_V(record, ERR_DOES_NOT_EXIST);
		record->cell_grid = assigned_key.grid;
		record->cell_x = assigned_key.x;
		record->cell_y = assigned_key.y;
		record->cell_z = assigned_key.z;
		record->has_cell = true;
	}
	_set_global(p_id, false);
	_cell_members_insert(cells[assigned_key], p_id);
	return OK;
}

Error EntityScene::_assign_cells(const Vector<EntityId> &p_ids) {
	Error result = OK;
	HashMap<CellKey, CellMembers, CellKeyHasher> batches;
	for (EntityId id : p_ids) {
		if (catalog.get_state(id) != EntityReferenceState::UNLOADED) {
			continue;
		}
		String source;
		const String directory = _storage_directory(id, source);
		if (directory.is_empty()) {
			if (!source.is_empty() && result == OK) {
				result = _fail(id, "cell/" + source, ERR_INVALID_DATA);
			}
			continue;
		}
		if (directory == "global") {
			_set_global(id, true);
			continue;
		}
		CellKey key;
		if (!_parse_cell_directory(directory, key) || !grids.has(key.grid)) {
			if (result == OK) {
				result = _fail(id, "cell/" + source, ERR_INVALID_DATA);
			}
			continue;
		}
		const EntityCatalog::Record *current = catalog.get_record(id);
		if (!current) {
			continue;
		}
		const CellKey previous{ current->cell_grid, current->cell_x, current->cell_y, current->cell_z };
		if (!current->has_cell || previous != key) {
			EntityCatalog::Record *record = catalog.edit_record(id);
			if (!record) {
				continue;
			}
			record->cell_grid = key.grid;
			record->cell_x = key.x;
			record->cell_y = key.y;
			record->cell_z = key.z;
			record->has_cell = true;
		}
		_set_global(id, false);
		batches[key].push_back(id);
	}
	for (KeyValue<CellKey, CellMembers> &batch : batches) {
		CellMembers &members = cells[batch.key];
		members.append_array(batch.value);
		_sort_snapshot_ids(members);
	}
	return result;
}

String EntityScene::_cell_path(const CellKey &p_cell) const {
	if (storage_path.is_empty()) {
		return String();
	}
	return EntitySceneIO::scene_directory(storage_path).path_join(EntitySceneIO::cell_directory(p_cell.grid, p_cell.x, p_cell.y, p_cell.z));
}

bool EntityScene::cell_exists(const CellKey &p_cell, int *r_probe_budget) {
	ERR_FAIL_COND_V(_owner() != OK, false);
	if (resident_cells.has(p_cell) || cells.has(p_cell) || _is_cell_in_flight(p_cell)) {
		return true;
	}
	if (probed_revision != revision) {
		probed_revision = revision;
		probed_cells.clear();
	}
	const bool *probed = probed_cells.getptr(p_cell);
	if (probed) {
		return *probed;
	}
	if (r_probe_budget && *r_probe_budget <= 0) {
		return false;
	}
	if (r_probe_budget) {
		(*r_probe_budget)--;
	}
	const String path = _cell_path(p_cell);
	const bool exists = !path.is_empty() && DirAccess::dir_exists_absolute(path);
	probed_cells.insert(p_cell, exists);
	return exists;
}

const EntityScene::PrefabMember *EntityScene::_find_prefab_member(EntityId p_id) const {
	int32_t low = 0;
	int32_t high = prefab_mappings.size() - 1;
	while (low <= high) {
		const int32_t middle = low + (high - low) / 2;
		if (prefab_mappings[middle].id == p_id) {
			return &prefab_mappings[middle].member;
		}
		if (SnapshotIdOrder()(prefab_mappings[middle].id, p_id)) {
			low = middle + 1;
		} else {
			high = middle - 1;
		}
	}
	return nullptr;
}

void EntityScene::_index_prefabs() {
	HashMap<EntityId, PrefabMember, EntityIdHasher> desired;
	for (const Variant &key : prefab_instances.get_key_list()) {
		const Variant value = prefab_instances[key];
		if (value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Variant mapping = Dictionary(value).get("mapping", Variant());
		if (mapping.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entries = mapping;
		for (const Variant &source : entries.get_key_list()) {
			EntityId id;
			if (entries[source].get_type() == Variant::STRING && EntityId::parse(entries[source], id) == OK && id.is_valid() && !desired.has(id)) {
				desired.insert(id, { key, source });
			}
		}
	}
	prefab_mappings.clear();
	prefab_mappings.reserve(desired.size());
	for (const KeyValue<EntityId, PrefabMember> &entry : desired) {
		prefab_mappings.push_back({ entry.key, entry.value });
	}
	struct PrefabMappingOrder {
		bool operator()(const PrefabMapping &p_left, const PrefabMapping &p_right) const { return SnapshotIdOrder()(p_left.id, p_right.id); }
	};
	prefab_mappings.sort_custom<PrefabMappingOrder>();
	for (uint32_t block_index = 0; block_index < catalog.blocks.size(); block_index++) {
		EntityCatalog::CellRuntimeBlock *block = catalog.blocks[block_index];
		if (!block) {
			continue;
		}
		for (uint32_t row = 0; row < uint32_t(block->states.size()); row++) {
			EntityCatalog::RowState &state = block->states.write[row];
			if (!state.active || !state.prefab) {
				continue;
			}
			const EntityCatalog::RowLocation location{ block_index, block->generation, row };
			const EntityCatalog::Record *record = catalog.get_record(location);
			const PrefabMember *member = desired.getptr(block->ids[row]);
			if (record && member && record->prefab_instance == member->instance && record->prefab_source == member->source) {
				desired.erase(block->ids[row]);
				continue;
			}
			state.prefab = false;
			EntityCatalog::Record *edited = catalog.edit_record(location);
			if (edited) {
				edited->prefab_instance = String();
				edited->prefab_source = String();
			}
		}
	}
	for (const KeyValue<EntityId, PrefabMember> &entry : desired) {
		EntityCatalog::Record *record = catalog.edit_record(entry.key);
		EntityCatalog::RowState *state = catalog.get_state_ptr(entry.key);
		if (record && state) {
			record->prefab_instance = entry.value.instance;
			record->prefab_source = entry.value.source;
			state->prefab = true;
		}
	}
}

EntityScene::PreparedGroup::~PreparedGroup() {
	for (uint32_t row = 0; row < constructed.size(); row++) {
		release_row(row);
	}
	for (const PreparedColumn &column : columns) {
		Memory::free_aligned_static(column.buffer);
	}
}

Error EntityScene::PreparedGroup::allocate() {
	ERR_FAIL_COND_V(!columns.is_empty() || !constructed.is_empty(), ERR_ALREADY_IN_USE);
	constructed.resize(capacity);
	for (uint32_t &count : constructed) {
		count = 0;
	}
	columns.reserve(signature.size());
	const EntitySchemaRegistry &schemas = EntitySchemaRegistry::descriptors();
	for (uint64_t id : signature) {
		const EntityComponentSchema *schema = schemas.find(id);
		ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
		ERR_FAIL_COND_V(!schema->construct || !schema->destruct || !schema->decode || !schema->size || !schema->alignment, ERR_INVALID_PARAMETER);
		const size_t overhead = schema->alignment - 1 + sizeof(uint32_t);
		ERR_FAIL_COND_V(capacity > (SIZE_MAX - overhead) / schema->size, ERR_OUT_OF_MEMORY);
		const size_t bytes = schema->size * capacity;
		void *buffer = Memory::alloc_aligned_static(bytes, schema->alignment);
		ERR_FAIL_NULL_V(buffer, ERR_OUT_OF_MEMORY);
		columns.push_back({ schema, buffer });
		allocated_bytes += bytes + overhead;
	}
	return OK;
}

void EntityScene::PreparedGroup::release_row(uint32_t p_row) {
	if (p_row >= constructed.size()) {
		return;
	}
	uint32_t &count = constructed[p_row];
	while (count > 0) {
		const PreparedColumn &column = columns[--count];
		column.schema->destruct(column.row(p_row));
	}
}

Error EntityScene::PreparedGroup::decode_row(uint32_t p_row, const Dictionary &p_components, String &r_field) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_row, constructed.size(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(constructed[p_row] != 0, ERR_ALREADY_IN_USE);
	for (const PreparedColumn &column : columns) {
		void *value = column.row(p_row);
		column.schema->construct(value);
		constructed[p_row]++;
		if (profile) {
			constructions.increment();
		}
		const Error error = column.schema->decode(value, p_components[column.schema->key]);
		if (error != OK) {
			r_field = column.schema->key;
			release_row(p_row);
			return error;
		}
	}
	return OK;
}

void EntityScene::PreparedGroup::move_row(uint32_t p_from, uint32_t p_to, const LocalVector<const ecs_type_info_t *> &p_types) {
	DEV_ASSERT(p_to < p_from && constructed[p_to] == 0 && constructed[p_from] == columns.size());
	for (uint32_t i = 0; i < columns.size(); i++) {
		const PreparedColumn &column = columns[i];
		void *destination = column.row(p_to);
		column.schema->construct(destination);
		constructed[p_to]++;
		if (profile) {
			constructions.increment();
		}
		if (p_types[i]->hooks.move) {
			p_types[i]->hooks.move(destination, column.row(p_from), 1, p_types[i]);
		} else {
			memcpy(destination, column.row(p_from), column.schema->size);
		}
	}
	release_row(p_from);
}

EntityScene::PreparedCell::~PreparedCell() {
	entities.clear();
	for (PreparedGroup *group : groups) {
		memdelete(group);
	}
}

EntityScene::PreparedEntity &EntityScene::PreparedEntity::operator=(PreparedEntity &&p_other) {
	if (this == &p_other) {
		return *this;
	}
	release();
	id = p_other.id;
	parent = p_other.parent;
	deleted = p_other.deleted;
	live = p_other.live;
	order = p_other.order;
	name = std::move(p_other.name);
	path = std::move(p_other.path);
	cluster = p_other.cluster;
	components = std::move(p_other.components);
	group = p_other.group;
	group_index = p_other.group_index;
	row = p_other.row;
	owns_group = p_other.owns_group;
	p_other.group = nullptr;
	p_other.group_index = UINT32_MAX;
	p_other.owns_group = false;
	return *this;
}

void EntityScene::PreparedEntity::release() {
	if (!group) {
		return;
	}
	group->release_row(row);
	if (owns_group) {
		memdelete(group);
	}
	group = nullptr;
	group_index = UINT32_MAX;
	owns_group = false;
}

EntityScene::PreparedSet::PreparedSet(const LocalVector<PreparedEntity> &p_entities, const LocalVector<PreparedGroup *> *p_bulk_groups) :
		entities(&p_entities), bulk_groups(p_bulk_groups) {
	sorted_rows.resize(p_entities.size());
	for (uint32_t i = 0; i < p_entities.size(); i++) {
		sorted_rows.write[i] = i;
	}
	sorted_rows.sort_custom<RowOrder>(&p_entities);
}

const EntityScene::PreparedEntity *EntityScene::PreparedSet::find(EntityId p_id) const {
	const int32_t row = find_row(p_id);
	return row >= 0 ? &(*entities)[row] : nullptr;
}

int32_t EntityScene::PreparedSet::find_row(EntityId p_id) const {
	if (!entities) {
		return -1;
	}
	int32_t low = 0;
	int32_t high = sorted_rows.size() - 1;
	while (low <= high) {
		const int32_t middle = low + (high - low) / 2;
		const PreparedEntity &entry = (*entities)[sorted_rows[middle]];
		if (entry.id == p_id) {
			return sorted_rows[middle];
		}
		if (entry.id.high != p_id.high ? entry.id.high < p_id.high : entry.id.low < p_id.low) {
			low = middle + 1;
		} else {
			high = middle - 1;
		}
	}
	return -1;
}

bool EntityScene::PreparedSet::is_deleted(EntityId p_id) const {
	if (scene) {
		const EntityCatalog::Record *record = scene->catalog.get_record(p_id);
		return !record || record->deleted;
	}
	const PreparedEntity *entry = find(p_id);
	return entry ? entry->deleted : true;
}

EntityRef EntityScene::PreparedSet::get_parent(EntityId p_id) const {
	if (scene) {
		return scene->catalog.get_parent(p_id);
	}
	const PreparedEntity *entry = find(p_id);
	return entry ? entry->parent : EntityRef();
}

int64_t EntityScene::PreparedSet::get_order(EntityId p_id) const {
	if (scene) {
		return scene->get_order(p_id);
	}
	const PreparedEntity *entry = find(p_id);
	return entry ? entry->order : 0;
}

Error EntityScene::PreparedSet::collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const {
	if (scene) {
		return scene->_collect_required(p_ids, r_ids);
	}
	Vector<bool> seen;
	seen.resize_initialized(entities->size());
	r_ids.reserve(r_ids.size() + p_ids.size());
	LocalVector<uint32_t> ancestors;
	for (EntityId id : p_ids) {
		ancestors.clear();
		while (id.is_valid()) {
			const int32_t row = find_row(id);
			if (row < 0) {
				return ERR_DOES_NOT_EXIST;
			}
			if (seen[row]) {
				break;
			}
			if (ancestors.has(row)) {
				return ERR_CYCLIC_LINK;
			}
			ancestors.push_back(row);
			const PreparedEntity &entry = (*entities)[row];
			id = entry.deleted ? EntityId() : entry.parent.id;
		}
		for (uint32_t i = ancestors.size(); i > 0; i--) {
			seen.write[ancestors[i - 1]] = true;
			r_ids.push_back((*entities)[ancestors[i - 1]].id);
		}
	}
	return OK;
}

EntityScene::SectionAction EntityScene::PreparedSet::build_section(EntityId p_id, Section &r_section) const {
	if (scene) {
		const Section *section = scene->_get_section(p_id);
		if (!section) {
			return SECTION_ERASE;
		}
		r_section = *section;
		return SECTION_SET;
	}
	const PreparedEntity *entry = find(p_id);
	if (!entry) {
		return SECTION_ERASE;
	}
	if (entry->live) {
		return SECTION_KEEP;
	}
	r_section.name = entry->name;
	r_section.components = entry->components;
	r_section.path = entry->path;
	r_section.cluster = entry->cluster;
	return SECTION_SET;
}

void EntityScene::PreparedSet::build_commit_item(EntityId p_id, CommitItem &r_item) const {
	r_item.id = p_id;
	if (scene) {
		const EntityCatalog::Record *record = scene->catalog.get_record(p_id);
		r_item.deleted = record ? record->deleted : true;
		r_item.parent = record ? record->parent : EntityRef();
		r_item.order = scene->get_order(p_id);
	} else {
		r_item.prepared = find(p_id);
		r_item.deleted = r_item.prepared ? r_item.prepared->deleted : true;
		r_item.parent = r_item.prepared ? r_item.prepared->parent : EntityRef();
		r_item.order = r_item.prepared ? r_item.prepared->order : 0;
	}
	r_item.section_action = r_item.deleted ? SECTION_ERASE : build_section(p_id, r_item.section);
}

void EntityScene::PreparedSet::write_components(EntityWorld &p_target, EntityHandle p_handle, EntityId p_id, const PreparedEntity *p_entry, CommitProfile *r_profile) const {
	const bool timed = r_profile != nullptr;
	uint64_t began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	if (scene) {
		EntityHandle from = scene->resolve(p_id).handle;
		if (timed) {
			r_profile->prepared_schema_lookup_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		for (const KeyValue<uint64_t, EntityComponentSchema> &entry : p_target.schemas.get_types()) {
			const EntityComponentSchema &schema = entry.value;
			if (!schema.is_component) {
				continue;
			}
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			const EntityComponentSchema *source = scene->world->schemas.find(schema.id);
			const void *value = source ? ecs_get_id(scene->world->ecs.c_ptr(), from.entity, source->runtime_id) : nullptr;
			if (timed) {
				r_profile->prepared_schema_lookup_usec += OS::get_singleton()->get_ticks_usec() - began;
			}
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			if (value) {
				schema.copy_to(p_target.ecs, p_handle.entity, value);
			} else {
				p_target.ecs.entity(p_handle.entity).remove(schema.runtime_id);
			}
			if (timed) {
				r_profile->component_mutation_usec += OS::get_singleton()->get_ticks_usec() - began;
				r_profile->component_mutations++;
			}
		}
		return;
	}
	if (timed) {
		r_profile->prepared_schema_lookup_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (!p_entry || p_entry->live || !p_entry->group) {
		return;
	}
	for (const PreparedColumn &column : p_entry->group->columns) {
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		const EntityComponentSchema *schema = p_target.schemas.find(column.schema->id);
		if (timed) {
			r_profile->prepared_schema_lookup_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		if (schema) {
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			schema->copy_to(p_target.ecs, p_handle.entity, column.row(p_entry->row));
			if (timed) {
				r_profile->component_mutation_usec += OS::get_singleton()->get_ticks_usec() - began;
				r_profile->component_mutations++;
			}
		}
	}
}

Error EntityScene::PreparedSet::materialize_groups(EntityWorld &p_target, LocalVector<EntityInitialRenderGroup> &r_initial, CommitProfile *r_profile) const {
	DEV_ASSERT(entities && bulk_groups);
	uint64_t began = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	LocalVector<LocalVector<const PreparedEntity *>> rows;
	rows.resize(bulk_groups->size());
	for (uint32_t i = 0; i < bulk_groups->size(); i++) {
		PreparedGroup *group = (*bulk_groups)[i];
		rows[i].resize(group->capacity);
		for (const PreparedEntity *&entry : rows[i]) {
			entry = nullptr;
		}
	}
	for (const PreparedEntity &entry : *entities) {
		if (entry.group && !entry.live && !entry.deleted) {
			DEV_ASSERT(entry.group_index < bulk_groups->size() && (*bulk_groups)[entry.group_index] == entry.group);
			rows[entry.group_index][entry.row] = &entry;
		}
	}
	if (r_profile) {
		r_profile->bulk_compact_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	EntityWorld::MaterializeProfile materialize_profile;
	for (uint32_t index = 0; index < bulk_groups->size(); index++) {
		PreparedGroup &group = *(*bulk_groups)[index];
		began = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
		ecs_bulk_desc_t desc = {};
		void *data[FLECS_ID_DESC_MAX] = {};
		desc.data = data;
		uint32_t render_components = 0;
		LocalVector<const ecs_type_info_t *> types;
		types.reserve(group.columns.size());
		for (uint32_t i = 0; i < group.columns.size(); i++) {
			const PreparedColumn &column = group.columns[i];
			const EntityComponentSchema *schema = p_target.schemas.find(column.schema->id);
			DEV_ASSERT(schema && schema->runtime_id);
			desc.ids[i + 2] = schema->runtime_id;
			data[i + 2] = column.buffer;
			types.push_back(ecs_get_type_info(p_target.ecs.c_ptr(), schema->runtime_id));
			render_components |= EntityRenderSystem::component_mask(schema->id) & EntityRenderUpdate::COMPONENTS;
		}
		if (r_profile) {
			r_profile->prepared_schema_lookup_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		began = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
		LocalVector<EntityId> ids;
		ids.reserve(group.capacity);
		for (uint32_t row = 0; row < group.capacity; row++) {
			const PreparedEntity *entry = rows[index][row];
			if (!entry) {
				if (r_profile) {
					r_profile->skipped_rows++;
				}
				continue;
			}
			if (row != ids.size()) {
				group.move_row(row, ids.size(), types);
				if (r_profile) {
					r_profile->compacted_rows++;
				}
			}
			ids.push_back(entry->id);
		}
		if (r_profile) {
			r_profile->bulk_compact_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		if (ids.is_empty()) {
			continue;
		}
		const ecs_table_t *table = nullptr;
		ecs_entity_t storage_tag = 0;
		const Error error = p_target._materialize_bulk(ids, nullptr, desc, table, storage_tag, nullptr, nullptr, true, r_profile ? &materialize_profile : nullptr);
		if (error != OK) {
			return error;
		}
		group.moved_rows = ids.size();
		EntityInitialRenderGroup initial;
		initial.table = table;
		initial.storage_tag = storage_tag;
		initial.entities.reserve(ids.size());
		for (EntityId id : ids) {
			const EntityCatalog::RowState *resident = p_target._resident(id);
			DEV_ASSERT(resident);
			if (resident) {
				initial.entities.push_back({ id, resident->handle, render_components });
			}
		}
		r_initial.push_back(std::move(initial));
		if (r_profile) {
			r_profile->bulk_groups++;
			r_profile->bulk_rows += ids.size();
		}
	}
	if (r_profile) {
		r_profile->ecs_bulk_create_components_usec += materialize_profile.ecs_bulk_create_components_usec;
		r_profile->resident_insert_usec += materialize_profile.resident_insert_usec;
		r_profile->entities_materialized += materialize_profile.entities_materialized;
	}
	return OK;
}

Dictionary EntityScene::_shallow_record(const Dictionary &p_record) {
	Dictionary result;
	for (const Variant &key : p_record.get_key_list()) {
		result[key] = p_record[key];
	}
	return result;
}

Error EntityScene::_read_stored(EntityId p_id, Dictionary &r_record) {
	const Section *section = _get_section(p_id);
	if (!section) {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	if (!section->record.is_empty()) {
		r_record = _shallow_record(section->record);
		return OK;
	}
	if (section->path.is_empty() || storage_path.is_empty()) {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	const String path = EntitySceneIO::scene_directory(storage_path).path_join(section->path);
	if (section->cluster) {
		if (cluster_path != path) {
			Variant parsed;
			Error error = EntitySceneIO::read_variant_file(path, parsed);
			if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
				error = ERR_FILE_CORRUPT;
			}
			if (error != OK) {
				return _fail(p_id, "cluster", error);
			}
			cluster_path = path;
			cluster_records = parsed;
		}
		Variant value = cluster_records.get(p_id.to_string(), Variant());
		if (value.get_type() != Variant::DICTIONARY) {
			return _fail(p_id, "cluster", ERR_FILE_CORRUPT);
		}
		r_record = _shallow_record(value);
		return OK;
	}
	Variant parsed;
	Error error = EntitySceneIO::read_variant_file(path, parsed);
	if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
		error = ERR_FILE_CORRUPT;
	}
	if (error != OK) {
		return _fail(p_id, "record", error);
	}
	r_record = parsed;
	return OK;
}

Error EntityScene::_encode_record(EntityId p_id, Dictionary &r_record) {
	Dictionary components;
	EntityResolution target = resolve(p_id);
	ERR_FAIL_COND_V(target.state != EntityReferenceState::RESIDENT, ERR_UNAVAILABLE);
	for (const KeyValue<uint64_t, EntityComponentSchema> &entry : world->schemas.get_types()) {
		const EntityComponentSchema &schema = entry.value;
		if (!schema.is_component || !ecs_has_id(world->ecs.c_ptr(), target.handle.entity, schema.runtime_id)) {
			continue;
		}
		Dictionary value;
		for (const EntityFieldSchema &field : schema.fields) {
			if (!field.serialized) {
				continue;
			}
			Variant field_value;
			Error error = world->read_field(target.handle, schema.id, field.id, field_value);
			if (error != OK) {
				return _fail(p_id, schema.key + "/" + field.key, error);
			}
			value[field.key] = field_value;
		}
		components[schema.key] = value;
	}
	r_record["components"] = components;
	return OK;
}

Error EntityScene::_read_record(EntityId p_id, Dictionary &r_record, bool *r_stored, bool p_prefer_stored) {
	if (r_stored) {
		*r_stored = false;
	}
	EntityResolution target = resolve(p_id);
	Error error = OK;
	if (target.state == EntityReferenceState::RESIDENT) {
		error = _encode_record(p_id, r_record);
	} else if (target.state == EntityReferenceState::UNLOADED) {
		bool prefab_record = false;
		const Section *section = _get_section(p_id);
		if (!p_prefer_stored || !section || (section->record.is_empty() && section->path.is_empty())) {
			error = get_commands()._prefab_record(p_id, r_record, prefab_record);
		}
		if (error != OK) {
			return error;
		}
		if (prefab_record) {
			return OK;
		}
		Dictionary stored;
		error = _read_stored(p_id, stored);
		if (error == OK) {
			r_record = stored;
			if (r_stored) {
				*r_stored = true;
			}
		}
	} else if (target.state == EntityReferenceState::DELETED) {
		r_record["components"] = Dictionary();
	} else {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	if (error != OK) {
		return _fail(p_id, "record", error);
	}
	r_record["parent"] = catalog.get_parent(p_id).id.to_string();
	r_record["deleted"] = target.state == EntityReferenceState::DELETED;
	r_record["order"] = get_order(p_id);
	return OK;
}

Error EntityScene::_validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, const String &p_prefix) {
	const EntityComponentSchema *schema = get_world()->schemas.find(p_type);
	if (!schema) {
		return _fail(p_id, p_prefix.is_empty() ? String::num_uint64(p_type, 16) : p_prefix, ERR_UNAVAILABLE);
	}
	const String prefix = p_prefix.is_empty() ? schema->key : p_prefix;
	int recognized = 0;
	for (const EntityFieldSchema &field : schema->fields) {
		if (!field.serialized || !p_fields.has(field.key)) {
			continue;
		}
		recognized++;
		String address = prefix + "/" + field.key;
		Variant value = p_fields[field.key];
		Error validation_error = field.validate(value);
		if (validation_error != OK) {
			return _fail(p_id, address, validation_error);
		}
		if (field.nested_type_id) {
			Array entries;
			if (value.get_type() == Variant::ARRAY) {
				entries = value;
			} else {
				entries.push_back(value);
			}
			for (int i = 0; i < entries.size(); i++) {
				if (entries[i].get_type() != Variant::DICTIONARY) {
					return _fail(p_id, address, ERR_INVALID_DATA);
				}
				Error error = _validate_fields(p_id, field.nested_type_id, entries[i], address);
				if (error != OK) {
					return error;
				}
			}
		}
		if (field.asset_reference) {
			Array assets;
			if (value.get_type() == Variant::ARRAY) {
				assets = value;
			} else {
				assets.push_back(value);
			}
			for (int i = 0; i < assets.size(); i++) {
				Ref<Resource> asset;
				Error error = entity_decode_asset(assets[i], asset);
				if (error != OK) {
					return _fail(p_id, address + "[" + itos(i) + "]", error);
				}
			}
		}
	}
	if (recognized != p_fields.size()) {
		for (const Variant &key : p_fields.get_key_list()) {
			bool found = false;
			for (const EntityFieldSchema &field : schema->fields) {
				found |= field.serialized && key == field.key;
			}
			if (!found) {
				return _fail(p_id, prefix + "/" + String(key), ERR_INVALID_DATA);
			}
		}
	}
	return OK;
}

Error EntityScene::_describe_components(const Dictionary &p_record, Section &r_section, Vector<uint64_t> &r_types, String &r_field) {
	if (!p_record.has("components") || p_record["components"].get_type() != Variant::DICTIONARY) {
		r_field = "components";
		return ERR_INVALID_DATA;
	}
	const Dictionary components = p_record["components"];
	const EntitySchemaRegistry &schemas = EntitySchemaRegistry::descriptors();
	r_section.components.clear();
	r_section.name = String();
	const EntityComponentSchema *name_schema = schemas.find(EntityComponentTraits<EntityName>::id);
	const Variant entity_name = name_schema ? components.get(name_schema->key, Variant()) : Variant();
	if (entity_name.get_type() == Variant::DICTIONARY) {
		const Variant text = Dictionary(entity_name).get("1", Variant());
		if (text.get_type() == Variant::STRING) {
			r_section.name = text;
		}
	}
	for (const Variant &key : components.get_key_list()) {
		const String text = key;
		const EntityComponentSchema *schema = schemas.find(text.hex_to_int());
		if (!schema || !schema->is_component || text != schema->key || components[key].get_type() != Variant::DICTIONARY) {
			r_field = text;
			return ERR_INVALID_DATA;
		}
		r_section.components.push_back(schema->key);
		r_types.push_back(schema->id);
	}
	return OK;
}

Error EntityScene::_describe(EntityId p_id, const Dictionary &p_record, Section &r_section) {
	Vector<uint64_t> types;
	String field;
	Error error = _describe_components(p_record, r_section, types, field);
	if (error != OK) {
		return _fail(p_id, field, error);
	}
	const Dictionary components = p_record["components"];
	for (int i = 0; i < types.size(); i++) {
		error = _validate_fields(p_id, types[i], components[r_section.components[i]]);
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

Error EntityScene::_install(EntityId p_id, const Dictionary &p_record) {
	Section section;
	Vector<uint64_t> types;
	String field;
	Error error = _describe_components(p_record, section, types, field);
	if (error != OK) {
		return _fail(p_id, field, error);
	}
	EntityHandle handle;
	error = get_world()->load_entity(p_id, handle);
	if (error != OK) {
		return error;
	}
	const Dictionary components = p_record["components"];
	for (int i = 0; i < types.size(); i++) {
		const String key = section.components[i];
		error = world->write_component(handle, types[i], components[key]);
		if (error != OK) {
			world->unload_entity(handle);
			return _fail(p_id, key, error);
		}
	}
	*_edit_section(p_id) = section;
	return OK;
}

Error EntityScene::_collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const {
	static std::atomic<uint64_t> next_visit{ 2 };
	const uint64_t visit = next_visit.fetch_add(2, std::memory_order_relaxed);
	for (EntityId id : p_ids) {
		LocalVector<EntityCatalog::RowLocation> ancestors;
		while (id.is_valid()) {
			const EntityCatalog::RowLocation location = catalog.locate(id);
			const EntityCatalog::RowState *row = catalog.get_state_ptr(location);
			if (!row) {
				return ERR_DOES_NOT_EXIST;
			}
			if (row->snapshot_visit == visit) {
				break;
			}
			if (row->snapshot_visit == visit + 1) {
				return ERR_CYCLIC_LINK;
			}
			EntityReferenceState state = catalog.get_state(id);
			row->snapshot_visit = visit + 1;
			ancestors.push_back(location);
			id = state == EntityReferenceState::DELETED ? EntityId() : catalog.get_parent(id).id;
		}
		for (uint32_t i = ancestors.size(); i > 0; i--) {
			const EntityCatalog::RowLocation location = ancestors[i - 1];
			const EntityCatalog::CellRuntimeBlock *block = catalog.get_block(location.block);
			catalog.get_state_ptr(location)->snapshot_visit = visit;
			r_ids.push_back(block->ids[location.row]);
		}
	}
	return OK;
}

Error EntityScene::_prepare(const Vector<EntityId> &p_ids, Ref<EntityScene> &r_scene, bool p_prefer_stored) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	Vector<EntityId> required;
	Error error = _collect_required(p_ids, required);
	if (error != OK) {
		return error;
	}
	Ref<EntityScene> prepared;
	prepared.instantiate();
	prepared->document_id = document_id;
	for (EntityId id : required) {
		Dictionary record;
		error = _read_record(id, record, nullptr, p_prefer_stored);
		if (error != OK) {
			return error;
		}
		EntityCatalog::Record record_metadata = *catalog.get_record(id);
		record_metadata.order = get_order(id);
		record_metadata.has_order = true;
		prepared->catalog.insert_record(id, record_metadata);
		if (!bool(record["deleted"])) {
			prepared->catalog._set_parent(id, catalog.get_parent(id));
			error = prepared->_install(id, record);
			if (error != OK) {
				last_error = prepared->last_error;
				return error;
			}
			EntityResolution existing = resolve(id);
			if (existing.state == EntityReferenceState::RESIDENT) {
				EntityHandle target = prepared->resolve(id).handle;
				for (const KeyValue<uint64_t, EntityComponentSchema> &entry : world->schemas.get_types()) {
					if (!entry.value.is_component) {
						continue;
					}
					const void *value = ecs_get_id(world->ecs.c_ptr(), existing.handle.entity, entry.value.runtime_id);
					if (value) {
						entry.value.copy_to(prepared->world->ecs, target.entity, value);
					}
				}
			}
		}
	}
	r_scene = prepared;
	return OK;
}

Error EntityScene::_decode_record(const Dictionary &p_record, PreparedEntity &r_prepared, String &r_field, LoadProfile *r_profile) {
	Section section;
	Vector<uint64_t> types;
	uint64_t phase_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	Error error = _describe_components(p_record, section, types, r_field);
	if (r_profile) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		r_profile->describe += now - phase_begin;
		phase_begin = now;
	}
	if (error != OK) {
		return error;
	}
	r_prepared.name = section.name;
	r_prepared.components = section.components;
	types.sort();
	r_prepared.group = memnew(PreparedGroup);
	r_prepared.owns_group = true;
	r_prepared.group->signature = types;
	r_prepared.group->capacity = 1;
	error = r_prepared.group->allocate();
	if (error == OK) {
		error = r_prepared.group->decode_row(r_prepared.row, p_record["components"], r_field);
	}
	if (r_profile) {
		r_profile->world += OS::get_singleton()->get_ticks_usec() - phase_begin;
	}
	return error;
}

Error EntityScene::_decode_entity(EntityId p_id, const Dictionary &p_record, PreparedEntity &r_prepared, LoadProfile *r_profile) {
	String field;
	const Error error = _decode_record(p_record, r_prepared, field, r_profile);
	return error == OK ? OK : _fail(p_id, field, error);
}

Error EntityScene::_prepare_entities(const Vector<EntityId> &p_ids, LocalVector<PreparedEntity> &r_entities, LoadProfile *r_profile) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	Vector<EntityId> required;
	Error error = _collect_required(p_ids, required);
	if (error != OK) {
		return error;
	}
	r_entities.reserve(required.size());
	for (EntityId id : required) {
		PreparedEntity prepared;
		prepared.id = id;
		prepared.parent = catalog.get_parent(id);
		prepared.order = get_order(id);
		prepared.deleted = catalog.get_record(id)->deleted;
		if (resolve(id).state == EntityReferenceState::RESIDENT) {
			prepared.live = true;
			r_entities.push_back(std::move(prepared));
			continue;
		}
		Dictionary record;
		const uint64_t read_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
		error = _read_record(id, record, nullptr, false);
		if (r_profile) {
			r_profile->read_record += OS::get_singleton()->get_ticks_usec() - read_begin;
		}
		if (error != OK) {
			return error;
		}
		if (!prepared.deleted) {
			const uint64_t install_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
			error = _decode_entity(id, record, prepared, r_profile);
			if (r_profile) {
				r_profile->install += OS::get_singleton()->get_ticks_usec() - install_begin;
			}
			if (error != OK) {
				return error;
			}
		}
		r_entities.push_back(std::move(prepared));
	}
	return OK;
}

void EntityScene::_build_commit_items(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, LocalVector<CommitItem> &r_items) {
	r_items.clear();
	r_items.reserve(p_ids.size());
	for (EntityId id : p_ids) {
		CommitItem item;
		p_prepared.build_commit_item(id, item);
		item.existing = resolve(id);
		const EntityCatalog::Record *previous = catalog.get_record(id);
		item.had_record = previous != nullptr;
		if (previous) {
			item.previous_deleted = previous->deleted;
			item.previous_parent = previous->parent;
		}
		item.parent_changed = !previous || !(item.previous_parent == item.parent);
		const EntityId previous_index_parent = previous && !previous->deleted ? previous->parent.id : EntityId();
		const EntityId final_index_parent = !item.deleted ? item.parent.id : EntityId();
		item.parent_index_changed = previous_index_parent != final_index_parent;
		if (previous && previous->has_cell) {
			item.had_cell = true;
			item.cell_grid = previous->cell_grid;
			item.cell_x = previous->cell_x;
			item.cell_y = previous->cell_y;
			item.cell_z = previous->cell_z;
		}
		item.had_global = _is_global(id);
		const int64_t *previous_order = _get_order_ptr(id);
		item.had_order = previous_order != nullptr;
		item.order_changed = previous_order ? *previous_order != item.order : item.order != 0;
		const Section *previous_section = _get_section(id);
		item.had_section = previous_section != nullptr;
		if (item.section_action == SECTION_SET) {
			if (previous_section && item.section.path.is_empty()) {
				item.section.path = previous_section->path;
				item.section.cluster = previous_section->cluster;
			}
			item.section_changed = !previous_section || previous_section->path != item.section.path || previous_section->cluster != item.section.cluster || previous_section->name != item.section.name || previous_section->components != item.section.components || previous_section->record != item.section.record;
		} else if (item.section_action == SECTION_ERASE) {
			item.section_changed = previous_section != nullptr;
		}
		r_items.push_back(std::move(item));
	}
}

Error EntityScene::_can_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids) {
	LocalVector<CommitItem> items;
	return _can_commit(p_prepared, p_ids, items);
}

Error EntityScene::_can_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, LocalVector<CommitItem> &r_items) {
	_build_commit_items(p_prepared, p_ids, r_items);
	auto validate_parent = [&](EntityId p_id) -> Error {
		if (p_prepared.is_deleted(p_id)) {
			return OK;
		}
		const EntityId parent = p_prepared.get_parent(p_id).id;
		if (!parent.is_valid()) {
			return OK;
		}
		const EntityCatalog::Record *parent_record = p_prepared.scene ? p_prepared.scene->catalog.get_record(parent) : nullptr;
		const PreparedEntity *parent_entity = p_prepared.find(parent);
		EntityReferenceState parent_state;
		if (parent_record) {
			parent_state = parent_record->deleted ? EntityReferenceState::DELETED : EntityReferenceState::UNLOADED;
		} else if (parent_entity) {
			parent_state = parent_entity->deleted ? EntityReferenceState::DELETED : EntityReferenceState::UNLOADED;
		} else {
			parent_state = resolve(parent).state;
		}
		if (parent_state == EntityReferenceState::DELETED || parent_state == EntityReferenceState::MISSING) {
			return _fail(p_id, "parent (" + parent.to_string() + ")", ERR_DOES_NOT_EXIST);
		}
		return OK;
	};
	if (p_prepared.entities) {
		for (const PreparedEntity &entity : *p_prepared.entities) {
			const Error error = validate_parent(entity.id);
			if (error != OK) {
				return error;
			}
		}
	} else {
		for (EntityId id : p_prepared.scene->catalog.get_ids()) {
			const Error error = validate_parent(id);
			if (error != OK) {
				return error;
			}
		}
	}
	Vector<EntityId> affected = p_ids;
	_sort_snapshot_ids(affected);
	for (const CommitItem &item : r_items) {
		if (item.deleted) {
			if (_is_pinned(item.id)) {
				return _fail(item.id, "record", ERR_BUSY);
			}
			for (EntityId child : catalog.get_children(item.id)) {
				if (!_snapshot_has(affected, child) || (!p_prepared.is_deleted(child) && p_prepared.get_parent(child).id == item.id)) {
					return _fail(item.id, "children", ERR_BUSY);
				}
			}
		}
	}
	return OK;
}

Error EntityScene::_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, bool p_resident, bool p_dirty, CommitProfile *r_profile) {
	LocalVector<CommitItem> items;
	_build_commit_items(p_prepared, p_ids, items);
	return _commit(p_prepared, items, p_resident, p_dirty, r_profile, nullptr);
}

Error EntityScene::_commit(const PreparedSet &p_prepared, LocalVector<CommitItem> &p_items, bool p_resident, bool p_dirty, CommitProfile *r_profile, const CellKey *p_streamed_cell, const Vector<EntityId> *p_streamed_members) {
	EntityWorld *target = get_world();
	const bool bulk = p_prepared.bulk_groups != nullptr;
	LocalVector<EntityInitialRenderGroup> initial;
	if (bulk) {
		for (const PreparedGroup *group : *p_prepared.bulk_groups) {
			ERR_FAIL_COND_V(group->columns.size() + 3 >= FLECS_ID_DESC_MAX, ERR_INVALID_DATA);
		}
	}
	const bool timed = r_profile != nullptr;
	CommitProfile unused;
	CommitProfile &profile = timed ? *r_profile : unused;
	uint64_t began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	Vector<EntityId> active;
	active.reserve(p_items.size());
	for (const CommitItem &item : p_items) {
		if (!item.deleted && (p_resident || item.existing.state == EntityReferenceState::RESIDENT)) {
			active.push_back(item.id);
		}
	}
	Vector<EntityId> required;
	const Error required_error = p_prepared.collect_required(active, required);
	if (timed) {
		profile.required_catalog_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (required_error != OK) {
		return _fail(active.is_empty() ? EntityId() : active[0], "required", required_error);
	}
	Vector<EntityId> residency;
	Vector<EntityId> item_ids;
	item_ids.resize(p_items.size());
	uint32_t item_index = 0;
	for (const CommitItem &item : p_items) {
		item_ids.write[item_index++] = item.id;
	}
	_sort_snapshot_ids(item_ids);
	for (EntityId id : required) {
		if (p_prepared.is_deleted(id)) {
			return _fail(id, "required ancestor", ERR_DOES_NOT_EXIST);
		}
		residency.push_back(id);
		if (!_snapshot_has(item_ids, id) && resolve(id).state != EntityReferenceState::RESIDENT) {
			Vector<EntityId> added_ids;
			added_ids.push_back(id);
			LocalVector<CommitItem> added_items;
			_build_commit_items(p_prepared, added_ids, added_items);
			p_items.push_back(std::move(added_items[0]));
		}
		if (p_prepared.scene && p_prepared.scene->resolve(id).state != EntityReferenceState::RESIDENT && resolve(id).state != EntityReferenceState::RESIDENT) {
			return _fail(id, "prepared ancestor", ERR_UNAVAILABLE);
		}
	}
	_sort_snapshot_ids(residency);
	if (bulk) {
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		target->transforms.update();
		if (timed) {
			profile.transform_finalize_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	uint32_t new_deleted_storage = 0;
	for (const CommitItem &item : p_items) {
		new_deleted_storage += item.section_changed && item.section_action == SECTION_ERASE && item.deleted && item.had_section && !deleted_storage.has(item.id);
	}
	if (timed) {
		profile.required_catalog_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	if (new_deleted_storage) {
		deleted_storage.reserve(deleted_storage.size() + new_deleted_storage);
	}
	if (timed) {
		profile.sections_order_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	for (CommitItem &item : p_items) {
		item.install = !item.deleted && _snapshot_has(residency, item.id);
	}
	Vector<EntityId> new_ids;
	Vector<EntityCatalog::Record> new_base;
	for (const CommitItem &item : p_items) {
		if (!item.had_record) {
			EntityCatalog::Record record;
			record.deleted = item.deleted;
			record.parent = item.parent;
			record.order = item.order;
			record.has_order = item.order_changed;
			if (item.section_changed && item.section_action == SECTION_SET) {
				record.section = item.section;
				record.has_section = true;
			}
			if (p_streamed_cell && !item.deleted) {
				record.cell_grid = p_streamed_cell->grid;
				record.cell_x = p_streamed_cell->x;
				record.cell_y = p_streamed_cell->y;
				record.cell_z = p_streamed_cell->z;
				record.has_cell = true;
			}
			if (const PrefabMember *prefab = _find_prefab_member(item.id)) {
				record.prefab_instance = prefab->instance;
				record.prefab_source = prefab->source;
			}
			new_ids.push_back(item.id);
			new_base.push_back(record);
		}
	}
	uint32_t new_block_index = UINT32_MAX;
	if (!new_ids.is_empty()) {
		new_block_index = catalog.add_block(new_ids, new_base, p_streamed_cell ? p_streamed_cell->grid : String(), p_streamed_cell ? p_streamed_cell->x : 0, p_streamed_cell ? p_streamed_cell->y : 0, p_streamed_cell ? p_streamed_cell->z : 0, p_streamed_cell != nullptr);
		ERR_FAIL_COND_V(new_block_index == UINT32_MAX, catalog.get_last_publish_error() == OK ? ERR_CANT_CREATE : catalog.get_last_publish_error());
		EntityCatalog::CellRuntimeBlock *block = catalog.get_block(new_block_index);
		for (uint32_t row = 0; row < uint32_t(new_ids.size()); row++) {
			const PreparedEntity *prepared = p_prepared.find(new_ids[row]);
			const Vector<uint64_t> signature = prepared && prepared->group ? prepared->group->signature : Vector<uint64_t>();
			if (!block->archetypes.is_empty() && block->archetypes[block->archetypes.size() - 1].signature == signature && block->archetypes[block->archetypes.size() - 1].first + block->archetypes[block->archetypes.size() - 1].count == row) {
				block->archetypes.write[block->archetypes.size() - 1].count++;
			} else {
				block->archetypes.push_back({ signature, row, 1 });
			}
		}
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	for (CommitItem &item : p_items) {
		if (!item.had_record) {
			if (item.order_changed) {
				if (timed) {
					profile.metadata_order_updates++;
				}
			}
			if (item.section_changed && item.section_action == SECTION_SET) {
				if (timed) {
					profile.metadata_section_updates++;
				}
			}
			if (timed) {
				profile.metadata_new_records++;
			}
		} else if (item.previous_deleted != item.deleted || item.parent_changed) {
			EntityCatalog::Record *record = catalog.edit_record(item.id);
			record->deleted = item.deleted;
			if (item.parent_changed) {
				catalog._set_parent(item.id, item.parent);
			}
			if (timed) {
				profile.metadata_record_updates++;
			}
		}
		if (item.had_record && item.order_changed) {
			_set_order(item.id, item.order);
			if (timed) {
				profile.metadata_order_updates++;
			}
		}
		if (item.had_record && item.section_changed && item.section_action == SECTION_SET) {
			*_edit_section(item.id) = std::move(item.section);
			if (timed) {
				profile.metadata_section_updates++;
			}
		} else if (item.had_record && item.section_changed && item.section_action == SECTION_ERASE) {
			const Section *previous = _get_section(item.id);
			if (item.deleted && previous && !previous->path.is_empty()) {
				Section storage;
				storage.path = previous->path;
				storage.cluster = previous->cluster;
				deleted_storage[item.id] = std::move(storage);
			}
			_erase_section(item.id);
			if (timed) {
				profile.metadata_section_updates++;
			}
		}
		if (p_dirty) {
			_set_dirty(item.id);
		}
		EntityCatalog::RowState *row_state = catalog.get_state_ptr(item.id);
		if (row_state) {
			row_state->tombstone = item.deleted;
		}
	}
	if (timed) {
		profile.metadata_commit_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	EntityWorld::MaterializeProfile materialize_profile;
	if (!bulk) {
		for (EntityId id : required) {
			if (resolve(id).state != EntityReferenceState::RESIDENT) {
				target->_materialize(id, timed ? &materialize_profile : nullptr);
			}
		}
	}
	for (const CommitItem &item : p_items) {
		bool resident = item.existing.state == EntityReferenceState::RESIDENT;
		if (resident && !item.deleted && item.parent_changed) {
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			target->ecs.entity(item.existing.handle.entity).remove<flecs::Parent>();
			if (timed) {
				profile.ecs_parent_remove_usec += OS::get_singleton()->get_ticks_usec() - began;
				profile.parent_removals++;
			}
		}
		if (item.deleted && resident) {
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			target->transforms.forget(item.existing.handle.entity);
			target->_clear_resident(item.id);
			if (timed) {
				profile.resident_remove_usec += OS::get_singleton()->get_ticks_usec() - began;
			}
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			target->ecs.entity(item.existing.handle.entity).destruct();
			if (timed) {
				profile.ecs_entity_destroy_usec += OS::get_singleton()->get_ticks_usec() - began;
				profile.entities_destroyed++;
			}
			resident = false;
		}
		if (!item.deleted && item.install && !bulk) {
			EntityHandle handle = resident ? item.existing.handle : resolve(item.id).handle;
			p_prepared.write_components(*target, handle, item.id, item.prepared, timed ? &profile : nullptr);
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			target->_component_changed(handle);
			if (timed) {
				profile.component_changed_usec += OS::get_singleton()->get_ticks_usec() - began;
			}
		} else if (item.deleted || !item.install) {
			began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			target->_mark_changed(item.id);
			if (timed) {
				profile.change_bookkeeping_usec += OS::get_singleton()->get_ticks_usec() - began;
			}
		}
	}
	if (timed) {
		profile.required_catalog_usec += materialize_profile.catalog_usec;
		profile.ecs_entity_create_identity_usec += materialize_profile.ecs_entity_create_identity_usec;
		profile.ecs_materialize_parent_set_usec += materialize_profile.ecs_parent_set_usec;
		profile.resident_insert_usec += materialize_profile.resident_insert_usec;
		profile.initial_dirty_usec += materialize_profile.initial_dirty_usec;
		profile.entities_materialized += materialize_profile.entities_materialized;
		profile.materialize_parent_sets += materialize_profile.parent_sets;
	}
	if (bulk) {
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		const Error error = p_prepared.materialize_groups(*target, initial, timed ? &profile : nullptr);
		if (error != OK) {
			return error;
		}
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	for (const CommitItem &item : p_items) {
		if (item.deleted || !item.parent.id.is_valid() || !item.install) {
			continue;
		}
		const bool needs_parent = bulk ? (item.existing.state != EntityReferenceState::RESIDENT || item.parent_changed) : (item.existing.state == EntityReferenceState::RESIDENT && item.parent_changed);
		if (!needs_parent) {
			continue;
		}
		const EntityCatalog::RowState *resident = target->_resident(item.id);
		if (!resident || !target->is_alive(resident->handle)) {
			return _fail(item.id, "resident", ERR_BUG);
		}
		EntityResolution parent_resolution = resolve(item.parent.id);
		if (parent_resolution.state != EntityReferenceState::RESIDENT) {
			return _fail(item.id, "parent (" + item.parent.id.to_string() + ")", ERR_BUG);
		}
		target->ecs.entity(resident->handle.entity).set<flecs::Parent>({ parent_resolution.handle.entity });
		if (timed) {
			profile.final_parent_sets++;
		}
	}
	if (timed) {
		profile.ecs_final_parent_set_usec += OS::get_singleton()->get_ticks_usec() - began;
		began = OS::get_singleton()->get_ticks_usec();
	}
	if (p_dirty) {
		_index_prefabs();
	}
	if (timed) {
		profile.catalog_parent_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (bulk) {
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		LocalVector<uint64_t> ordered;
		ordered.reserve(active.size());
		for (EntityId id : required) {
			const PreparedEntity *entry = p_prepared.find(id);
			if (entry && !entry->live && !entry->deleted) {
				const EntityCatalog::RowState *resident = target->_resident(id);
				if (!resident || !target->is_alive(resident->handle)) {
					return _fail(id, "resident", ERR_BUG);
				}
				ordered.push_back(resident->handle.entity);
			}
		}
		const Error transform_error = target->transforms.finalize_created(ordered);
		if (transform_error != OK) {
			return transform_error;
		}
		if (timed) {
			profile.transform_finalize_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		EntityInitialRenderProfile render_profile;
		target->rendering.prepare_initial(initial, timed ? &render_profile : nullptr);
		if (timed) {
			profile.initial_packet_prepare_usec += OS::get_singleton()->get_ticks_usec() - began;
			for (const EntityInitialRenderGroup &group : initial) {
				profile.initial_packet_updates += group.entities.size();
			}
			profile.initial_packet_table_rows += render_profile.table_rows;
			profile.initial_packet_fallback_rows += render_profile.fallback_rows;
		}
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		const uint64_t change_serial = ++target->change_serial;
		for (const EntityInitialRenderGroup &group : initial) {
			for (const EntityInitialRender &created : group.entities) {
				EntityCatalog::RowState *resident = target->_resident(created.id);
				if (resident) {
					resident->revision = change_serial;
				}
				const EntityCatalog::RowLocation location = target->catalog.locate(created.id);
				if (resident && !resident->world_changed) {
					resident->world_changed = true;
					target->changed_rows.push_back({ location, created.id });
				}
			}
		}
		if (timed) {
			profile.change_bookkeeping_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	if (bulk && p_resident && p_streamed_cell) {
		HashMap<CellKey, Vector<EntityId>, CellKeyHasher> removals;
		for (const CommitItem &item : p_items) {
			if (item.had_global) {
				_set_global(item.id, false);
			}
			if (!item.had_cell) {
				continue;
			}
			const CellKey key{ item.cell_grid, item.cell_x, item.cell_y, item.cell_z };
			if (!item.install || key != *p_streamed_cell) {
				removals[key].push_back(item.id);
				if (timed) {
					profile.metadata_membership_removals++;
				}
			}
		}
		Vector<EntityId> incoming;
		incoming.reserve(p_streamed_members->size());
		for (EntityId id : *p_streamed_members) {
			const PreparedEntity *entity = p_prepared.find(id);
			if (entity ? entity->deleted : catalog.get_state(id) == EntityReferenceState::DELETED || deleted_storage.has(id)) {
				continue;
			}
			const EntityCatalog::Record *current = catalog.get_record(id);
			DEV_ASSERT(current);
			if (!current) {
				continue;
			}
			const CellKey previous_key{ current->cell_grid, current->cell_x, current->cell_y, current->cell_z };
			if (current->has_cell && previous_key != *p_streamed_cell) {
				removals[previous_key].push_back(id);
				if (timed) {
					profile.metadata_membership_removals++;
				}
			}
			if (!current->has_cell || previous_key != *p_streamed_cell) {
				EntityCatalog::Record *record = catalog.edit_record(id);
				record->cell_grid = p_streamed_cell->grid;
				record->cell_x = p_streamed_cell->x;
				record->cell_y = p_streamed_cell->y;
				record->cell_z = p_streamed_cell->z;
				record->has_cell = true;
			}
			_set_global(id, false);
			incoming.push_back(id);
			if (timed) {
				profile.cell_membership_insertions++;
			}
		}
		for (KeyValue<CellKey, Vector<EntityId>> &entry : removals) {
			CellMembers *members = cells.getptr(entry.key);
			if (!members) {
				continue;
			}
			_sort_snapshot_ids(entry.value);
			int write = 0;
			int remove = 0;
			for (EntityId id : *members) {
				while (remove < entry.value.size() && SnapshotIdOrder()(entry.value[remove], id)) {
					remove++;
				}
				if (remove >= entry.value.size() || entry.value[remove] != id) {
					members->write[write++] = id;
				}
			}
			members->resize(write);
			if (members->is_empty()) {
				cells.erase(entry.key);
			}
		}
		CellMembers &members = cells[*p_streamed_cell];
		members.append_array(incoming);
		_sort_snapshot_ids(members);
	} else {
		for (const CommitItem &item : p_items) {
			_assign_cell(item.id);
		}
	}
	if (timed) {
		profile.assign_cell_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	revision++;
	return OK;
}

Error EntityScene::_load_resident(const Vector<EntityId> &p_ids, LoadProfile *r_profile) {
	LocalVector<PreparedEntity> prepared;
	const uint64_t prepare_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	Error error = _prepare_entities(p_ids, prepared, r_profile);
	if (r_profile) {
		r_profile->prepare = OS::get_singleton()->get_ticks_usec() - prepare_begin;
	}
	if (error != OK) {
		return error;
	}
	const PreparedSet set(prepared);
	LocalVector<CommitItem> items;
	const uint64_t check_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	error = _can_commit(set, p_ids, items);
	if (r_profile) {
		r_profile->check = OS::get_singleton()->get_ticks_usec() - check_begin;
	}
	if (error != OK) {
		return error;
	}
	const uint64_t commit_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	const uint64_t previous_revision = revision;
	error = _commit(set, items, true, false);
	revision = previous_revision;
	if (error != OK) {
		return error;
	}
	if (r_profile) {
		r_profile->commit = OS::get_singleton()->get_ticks_usec() - commit_begin;
	}
	return OK;
}

Error EntityScene::load_subset(const Vector<EntityId> &p_ids) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	const bool profiling = OS::get_singleton()->is_use_benchmark_set();
	if (profiling) {
		entity_asset_profile_reset();
	}
	LoadProfile profile;
	const uint64_t started = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	Vector<EntityId> required;
	Error error = _collect_required(p_ids, required);
	if (error != OK) {
		return error;
	}
	Vector<EntityId> unloaded;
	for (EntityId id : required) {
		EntityResolution target = resolve(id);
		if (target.state == EntityReferenceState::DELETED) {
			return _fail(id, "record", ERR_DOES_NOT_EXIST);
		}
		if (target.state == EntityReferenceState::UNLOADED) {
			unloaded.push_back(id);
		}
	}
	if (unloaded.is_empty()) {
		return OK;
	}
	const uint64_t collected = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	error = _load_resident(unloaded, profiling ? &profile : nullptr);
	if (error != OK) {
		return error;
	}
	const uint64_t committed = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	if (profiling) {
		uint64_t asset_usec = 0;
		uint32_t asset_loads = 0;
		uint32_t asset_cache_hits = 0;
		entity_asset_profile_get(asset_usec, asset_loads, asset_cache_hits);
		const double to_ms = 1.0 / 1000.0;
		print_line(vformat("EntityScene load_subset: records=%d collect=%.2fms prepare=%.2fms (read_record=%.2fms install=%.2fms describe=%.2fms world=%.2fms asset_load=%.2fms loads=%d cache_hits=%d) can_commit=%.2fms commit=%.2fms total=%.2fms",
				unloaded.size(),
				double(collected - started) * to_ms,
				double(profile.prepare) * to_ms,
				double(profile.read_record) * to_ms,
				double(profile.install) * to_ms,
				double(profile.describe) * to_ms,
				double(profile.world) * to_ms,
				double(asset_usec) * to_ms,
				asset_loads,
				asset_cache_hits,
				double(profile.check) * to_ms,
				double(profile.commit) * to_ms,
				double(committed - started) * to_ms));
	}
	return OK;
}

Error EntityScene::unload_subset(const Vector<EntityId> &p_ids) {
	return _unload_subset(p_ids, false);
}

Error EntityScene::_unload_subset(const Vector<EntityId> &p_ids, bool p_prevalidated_order) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	Vector<EntityId> requested = p_ids;
	if (!p_prevalidated_order) {
		_sort_snapshot_ids(requested);
	}
	Vector<Section> saved;
	saved.resize(p_ids.size());
	for (int32_t requested_index = 0; requested_index < requested.size(); requested_index++) {
		const EntityId id = requested[requested_index];
		if (_is_pinned(id) || _is_dirty(id)) {
			return ERR_BUSY;
		}
		if (resolve(id).state != EntityReferenceState::RESIDENT) {
			return ERR_UNAVAILABLE;
		}
		const Section *stored = _get_section(id);
		if (stored && !stored->path.is_empty()) {
			Section section = *stored;
			section.record = Dictionary();
			saved.write[requested_index] = section;
			continue;
		}
		Dictionary record;
		Error error = _encode_record(id, record);
		Section section;
		if (error == OK) {
			error = _describe(id, record, section);
		}
		if (error != OK) {
			return error;
		}
		section.record = record;
		saved.write[requested_index] = section;
	}
	if (!p_prevalidated_order) {
		for (EntityId id : p_ids) {
			for (EntityId child : catalog.get_children(id)) {
				if (resolve(child).state == EntityReferenceState::RESIDENT && !_snapshot_has(requested, child)) {
					return ERR_BUSY;
				}
			}
		}
	}
	Vector<EntityId> ordered;
	if (p_prevalidated_order) {
		ordered = p_ids;
	} else {
		Error error = _collect_required(p_ids, ordered);
		if (error != OK) {
			return error;
		}
	}
	if (p_prevalidated_order) {
		for (int32_t i = 0; i < ordered.size(); i++) {
			const EntityId id = ordered[i];
			*_edit_section(id) = saved[i];
			const Error error = world->unload_entity(resolve(id).handle);
			if (error != OK) {
				return error;
			}
		}
	} else {
		for (int i = ordered.size() - 1; i >= 0; i--) {
			EntityId id = ordered[i];
			if (_snapshot_has(requested, id)) {
				const int32_t saved_index = requested.bsearch_custom<SnapshotIdOrder>(id, true);
				DEV_ASSERT(saved_index >= 0);
				*_edit_section(id) = saved[saved_index];
				world->unload_entity(resolve(id).handle);
			}
		}
	}
	return OK;
}

Error EntityScene::pin(const Vector<EntityId> &p_ids) {
	Error error = load_subset(p_ids);
	if (error != OK) {
		return error;
	}
	for (EntityId id : p_ids) {
		_pin_row(id);
	}
	return OK;
}

void EntityScene::unpin(const Vector<EntityId> &p_ids) {
	ERR_FAIL_COND(_owner() != OK);
	for (EntityId id : p_ids) {
		_unpin_row(id);
	}
}

void EntityScene::_relocate(const String &p_path) {
	flush_streaming();
	storage_path = p_path;
	cluster_path = String();
	cluster_records = Dictionary();
	probed_cells.clear();
	set_path_cache(p_path);
}

Error EntityScene::relocate(const String &p_path) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_path.get_extension().to_lower() != "escn" || EntitySceneIO::is_child_path(p_path), ERR_FILE_UNRECOGNIZED);
	_relocate(p_path);
	return OK;
}

Error EntityScene::create_play_document(Ref<EntityScene> &r_scene) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	flush_streaming();
	Ref<EntityScene> result;
	result.instantiate();
	result->document_id = document_id;
	result->revision = revision;
	result->grids = grids;
	result->default_grid = default_grid;
	result->default_range = default_range;
	result->prefab_instances = prefab_instances.duplicate(true);
	result->storage_path = storage_path;
	const Vector<EntityId> ids = catalog.get_ids();
	Vector<EntityCatalog::Record> copied_records;
	Vector<Dictionary> serialized_records;
	copied_records.resize(ids.size());
	serialized_records.resize(ids.size());
	for (int32_t i = 0; i < ids.size(); i++) {
		const EntityId id = ids[i];
		Dictionary record;
		Error error = _read_record(id, record);
		if (error != OK) {
			return error;
		}
		copied_records.write[i] = *catalog.get_record(id);
		serialized_records.write[i] = record;
	}
	ERR_FAIL_COND_V(result->catalog.add_block(ids, copied_records) == UINT32_MAX, result->catalog.get_last_publish_error() == OK ? ERR_CANT_CREATE : result->catalog.get_last_publish_error());
	for (int32_t i = 0; i < ids.size(); i++) {
		const EntityId id = ids[i];
		result->_set_dirty(id, _is_dirty(id));
		if (!copied_records[i].deleted) {
			Section section;
			Error error = _describe(id, serialized_records[i], section);
			if (error != OK) {
				return error;
			}
			section.record = serialized_records[i];
			const Section *source = _get_section(id);
			if (source) {
				section.path = source->path;
				section.cluster = source->cluster;
			}
			*result->_edit_section(id) = section;
			error = result->_install(id, serialized_records[i]);
			if (error != OK) {
				return error;
			}
		}
	}
	result->_index_prefabs();
	Error cell_error = result->_assign_cells(result->catalog.get_ids());
	if (cell_error != OK) {
		last_error = result->last_error;
		return cell_error;
	}
	r_scene = result;
	return OK;
}

Error EntityScene::load_global() {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	if (global_pinned) {
		return OK;
	}
	Vector<EntityId> ids;
	for (EntityId id : catalog.get_ids()) {
		if (_is_global(id) && catalog.get_state(id) == EntityReferenceState::UNLOADED) {
			ids.push_back(id);
		}
	}
	Error error = pin(ids);
	if (error != OK) {
		return error;
	}
	global_pinned = true;
	residency_serial++;
	return OK;
}

bool EntityScene::_is_cell_in_flight(const CellKey &p_cell) const {
	for (const CellJob *job : cell_jobs) {
		if (job->result.key == p_cell) {
			return true;
		}
	}
	return false;
}

uint32_t EntityScene::_max_cell_jobs() {
	const EntityTaskScheduler *scheduler = EntityTaskScheduler::get_singleton();
	static const uint32_t limit = CLAMP(scheduler ? scheduler->get_worker_count() : 1, uint32_t(1), uint32_t(4));
	return limit;
}

bool EntityScene::_snapshot_has(const Vector<EntityId> &p_ids, EntityId p_id) {
	const int index = p_ids.bsearch_custom<SnapshotIdOrder>(p_id, true);
	return index < p_ids.size() && p_ids[index] == p_id;
}

void EntityScene::_sort_snapshot_ids(Vector<EntityId> &r_ids) {
	r_ids.sort_custom<SnapshotIdOrder>();
	int count = 0;
	for (int i = 0; i < r_ids.size(); i++) {
		if (count == 0 || r_ids[i] != r_ids[count - 1]) {
			r_ids.write[count++] = r_ids[i];
		}
	}
	r_ids.resize(count);
}

Error EntityScene::_dispatch_cell(const CellKey &p_cell) {
	EntityTaskScheduler *scheduler = EntityTaskScheduler::get_singleton();
	if (!scheduler) {
		return ERR_UNAVAILABLE;
	}
	const CellMembers *members = cells.getptr(p_cell);
	static std::atomic<uint64_t> next_snapshot{ 2 };
	const uint64_t snapshot = next_snapshot.fetch_add(2, std::memory_order_relaxed);
	const uint64_t member_count = members ? members->size() : 0;
	uint64_t global_count = 0;
	for (const EntityCatalog::CellRuntimeBlock *block : catalog.blocks) {
		if (!block) {
			continue;
		}
		for (const EntityCatalog::RowState &state : block->states) {
			global_count += state.active && state.global;
		}
	}
	uint64_t ancestor_count = 0;
	uint64_t snapshot_bytes = sizeof(CellJob) + 16384 + global_count * sizeof(EntityId) * 4 + member_count * sizeof(EntityId) * 8 + uint64_t(deleted_storage.size()) * sizeof(EntityId) * 4;
	snapshot_bytes += uint64_t(storage_path.length() + p_cell.grid.length() + 128) * 128;
	if (members) {
		for (EntityId member : *members) {
			EntityId id = member;
			if (resolve(id).state == EntityReferenceState::RESIDENT || _is_dirty(id)) {
				continue;
			}
			while (id.is_valid()) {
				const EntityCatalog::Record *record = catalog.get_record(id);
				EntityCatalog::RowState *state = catalog.get_state_ptr(id);
				if (!record) {
					return _fail(id, "required", ERR_DOES_NOT_EXIST);
				}
				if (state->snapshot_visit == snapshot) {
					break;
				}
				if (state->snapshot_visit == snapshot + 1) {
					return _fail(id, "required", ERR_CYCLIC_LINK);
				}
				state->snapshot_visit = snapshot + 1;
				ancestor_count++;
				snapshot_bytes += sizeof(EntityId) * 16 + sizeof(PendingRecord) * 4 + 2048 + uint64_t(record->section.path.length() + 1) * 64;
				if (snapshot_bytes > CellJob::BYTE_BUDGET) {
					return ERR_OUT_OF_MEMORY;
				}
				id = record->deleted ? EntityId() : record->parent.id;
			}
			for (id = member; id.is_valid();) {
				const EntityCatalog::Record *record = catalog.get_record(id);
				EntityCatalog::RowState *state = catalog.get_state_ptr(id);
				if (!record || !state || state->snapshot_visit != snapshot + 1) {
					break;
				}
				state->snapshot_visit = snapshot;
				id = record->deleted ? EntityId() : record->parent.id;
			}
		}
	}
	EntityTaskScheduler::Reservation reservation;
	const Error admission_error = scheduler->reserve(reservation, snapshot_bytes);
	if (admission_error != OK) {
		return admission_error;
	}
	CellJob *job = memnew(CellJob);
	scheduler->adopt(job, reservation);
	job->profile = OS::get_singleton()->is_use_benchmark_set();
	job->scene_revision = revision;
	job->result.key = p_cell;
	job->storage_directory = EntitySceneIO::scene_directory(storage_path).simplify_path();
	LocalVector<EntityId, uint32_t, false, true> required_rows;
	required_rows.resize(uint32_t(ancestor_count));
	uint32_t required_count = 0;
	job->members.resize(int(member_count));
	int member_index = 0;
	if (members) {
		for (EntityId member_id : *members) {
			job->members.write[member_index++] = member_id;
			if (resolve(member_id).state == EntityReferenceState::RESIDENT || _is_dirty(member_id)) {
				continue;
			}
			for (EntityId id = member_id; id.is_valid();) {
				const EntityCatalog::Record *record = catalog.get_record(id);
				EntityCatalog::RowState *state = catalog.get_state_ptr(id);
				if (!record || !state || state->snapshot_visit != snapshot) {
					break;
				}
				state->snapshot_visit = snapshot + 1;
				required_rows[required_count++] = id;
				id = record->deleted ? EntityId() : record->parent.id;
			}
		}
	}
	required_rows.sort_custom<SnapshotIdOrder>();
	uint32_t unique_count = 0;
	for (uint32_t i = 0; i < required_count; i++) {
		if (i == 0 || required_rows[i] != required_rows[i - 1]) {
			unique_count++;
		}
	}
	job->required.resize(unique_count);
	unique_count = 0;
	for (uint32_t i = 0; i < required_count; i++) {
		if (i == 0 || required_rows[i] != required_rows[i - 1]) {
			job->required.write[unique_count++] = required_rows[i];
		}
	}
	required_rows.reset();
	_sort_snapshot_ids(job->members);
	uint32_t external_count = 0;
	for (EntityId id : job->required) {
		external_count += !_snapshot_has(job->members, id);
	}
	job->ancestors.resize(external_count);
	external_count = 0;
	LocalVector<EntityId, uint32_t, false, true> skipped;
	skipped.reserve(uint32_t(member_count) + unique_count + deleted_storage.size());
	for (EntityId id : job->members) {
		if (resolve(id).state == EntityReferenceState::RESIDENT || _is_dirty(id)) {
			skipped.push_back(id);
		}
	}
	job->required_files.reserve(unique_count);
	job->pending.reserve(unique_count);
	for (EntityId id : job->required) {
		if (!_snapshot_has(job->members, id)) {
			job->ancestors.write[external_count++] = id;
		}
		if (resolve(id).state == EntityReferenceState::RESIDENT || _is_dirty(id)) {
			skipped.push_back(id);
			continue;
		}
		const Section *section = _get_section(id);
		const EntityCatalog::Record *record_metadata = catalog.get_record(id);
		const bool prefab = record_metadata && !record_metadata->prefab_instance.is_empty();
		if (catalog.get_state(id) == EntityReferenceState::DELETED || (!prefab && (!section || (section->record.is_empty() && (section->path.is_empty() || storage_path.is_empty()))))) {
			skipped.reset();
			memdelete(job);
			return _fail(id, "record", ERR_DOES_NOT_EXIST);
		}
		const String path = section ? section->path.simplify_path() : String();
		if ((section && !section->path.is_empty() && path.is_empty()) || path.is_absolute_path() || path.contains(":") || path == ".." || path.begins_with("../")) {
			skipped.reset();
			memdelete(job);
			return _fail(id, "record path", ERR_INVALID_DATA);
		}
		const bool cluster = section && section->cluster;
		if (prefab || !section->record.is_empty()) {
			if (section && !section->record.is_empty()) {
				const Error reserve_error = EntitySceneIO::reserve_record(section->record, *job);
				if (reserve_error != OK) {
					skipped.reset();
					memdelete(job);
					return reserve_error;
				}
			}
			Dictionary record;
			const Error record_error = _read_record(id, record);
			if (record_error != OK) {
				skipped.reset();
				memdelete(job);
				return record_error;
			}
			const Error reserve_error = EntitySceneIO::reserve_record(record, *job);
			if (reserve_error != OK) {
				skipped.reset();
				memdelete(job);
				return reserve_error;
			}
			job->pending.push_back({ id, path, cluster, record.duplicate(true) });
			skipped.push_back(id);
		} else {
			job->required_files.insert(path, cluster);
		}
	}
	const String relative = EntitySceneIO::cell_directory(p_cell.grid, p_cell.x, p_cell.y, p_cell.z);
	for (const KeyValue<EntityId, Section> &entry : deleted_storage) {
		if (entry.value.path.get_base_dir() == relative) {
			skipped.push_back(entry.key);
		}
	}
	skipped.sort_custom<SnapshotIdOrder>();
	uint32_t skip_count = 0;
	for (uint32_t i = 0; i < skipped.size(); i++) {
		if (i == 0 || skipped[i] != skipped[i - 1]) {
			skip_count++;
		}
	}
	job->skip.resize(skip_count);
	skip_count = 0;
	for (uint32_t i = 0; i < skipped.size(); i++) {
		if (i == 0 || skipped[i] != skipped[i - 1]) {
			job->skip.write[skip_count++] = skipped[i];
		}
	}
	skipped.reset();
	job->globals.resize(global_count);
	int global_index = 0;
	for (const EntityCatalog::CellRuntimeBlock *block : catalog.blocks) {
		if (!block) {
			continue;
		}
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (block->states[row].active && block->states[row].global) {
				job->globals.write[global_index++] = block->ids[row];
			}
		}
	}
	_sort_snapshot_ids(job->globals);
	Vector<EntityId> external_ids = job->globals;
	for (EntityId id : job->skip) {
		external_ids.push_back(id);
	}
	_sort_snapshot_ids(external_ids);
	job->external_transforms.reserve(external_ids.size());
	for (EntityId id : external_ids) {
		const EntityResolution resolved = resolve(id);
		if (resolved.state != EntityReferenceState::RESIDENT) {
			continue;
		}
		ExternalTransformSnapshot snapshot;
		snapshot.id = id;
		snapshot.parent = catalog.get_parent(id);
		const EntityCatalog::RowState *state = catalog.get_state_ptr(id);
		snapshot.revision = state ? state->revision : 0;
		if (const EntityTransform *transform = world->get<EntityTransform>(resolved.handle)) {
			snapshot.pose = transform->current;
			snapshot.has_transform = true;
		}
		if (const EntityVisibility *visibility = world->get<EntityVisibility>(resolved.handle)) {
			snapshot.visible = visibility->effective;
			snapshot.has_visibility = true;
		}
		job->external_transforms.push_back(snapshot);
	}
	job->directory = _cell_path(p_cell);
	if ((job->directory.is_empty() || !DirAccess::dir_exists_absolute(job->directory)) && job->required_files.is_empty() && job->pending.is_empty()) {
		if (members) {
			const Error error = pin(job->ancestors);
			if (error != OK) {
				skipped.reset();
				memdelete(job);
				return error;
			}
			resident_cells.insert(p_cell, job->ancestors);
			residency_serial++;
		} else {
			probed_cells.insert(p_cell, false);
		}
		skipped.reset();
		memdelete(job);
		return OK;
	}
	job->relative = relative;
	if (job->profile) {
		print_line(vformat("Entity request snapshot reserved_bytes=%d globals=%d members=%d required=%d skipped=%d ancestors=%d", snapshot_bytes, job->globals.size(), job->members.size(), job->required.size(), job->skip.size(), job->ancestors.size()));
	}
	const Error error = scheduler->submit(job, cell_mailbox);
	if (error != OK) {
		skipped.reset();
		memdelete(job);
		return error;
	}
	cell_jobs.push_back(job);
	return OK;
}

Error EntityScene::_prepare_stored(EntityId p_id, const Dictionary &p_record, PreparedEntity &r_prepared, Vector<uint64_t> &r_types, String &r_field) {
	const Variant parent_value = p_record.get("parent", Variant());
	EntityRef parent;
	if (parent_value.get_type() != Variant::STRING || EntityId::parse(parent_value, parent.id) != OK) {
		r_field = "parent";
		return ERR_FILE_CORRUPT;
	}
	const Variant order_value = p_record.get("order", Variant());
	if (order_value.get_type() != Variant::INT) {
		r_field = "order";
		return ERR_FILE_CORRUPT;
	}
	const Variant deleted_value = p_record.get("deleted", false);
	if (deleted_value.get_type() != Variant::BOOL) {
		r_field = "deleted";
		return ERR_FILE_CORRUPT;
	}
	r_prepared.id = p_id;
	r_prepared.parent = parent;
	r_prepared.order = order_value;
	r_prepared.deleted = deleted_value;
	if (r_prepared.deleted) {
		return OK;
	}
	Section section;
	const Error error = _describe_components(p_record, section, r_types, r_field);
	r_prepared.name = section.name;
	r_prepared.components = section.components;
	return error;
}

void EntityScene::_enumerate_cell(void *p_job) {
	CellJob *job = static_cast<CellJob *>(p_job);
	const uint64_t began = job->profile ? OS::get_singleton()->get_ticks_usec() : 0;
	if (!job->cancelled.is_set() && !ResourceLoader::is_cleaning_tasks()) {
		HashMap<String, String> files;
		auto add_file = [&](const String &p_path) {
			if (!job->reserve_payload(uint64_t(p_path.length() + 1) * 32 + 1024)) {
				job->result.error = ERR_OUT_OF_MEMORY;
				job->result.failing_field = "file catalog budget";
				return;
			}
#ifdef WINDOWS_ENABLED
			const String key = p_path.to_lower();
#else
			const String &key = p_path;
#endif
			String *existing = files.getptr(key);
			if (!existing || p_path < *existing) {
				files.insert(key, p_path);
			}
		};
		if (!job->directory.is_empty() && DirAccess::dir_exists_absolute(job->directory)) {
			Ref<DirAccess> directory = DirAccess::open(job->directory);
			if (directory.is_null() || directory->list_dir_begin() != OK) {
				job->result.error = ERR_CANT_OPEN;
				return;
			}
			for (String name = directory->get_next(); !name.is_empty(); name = directory->get_next()) {
				if (job->cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
					job->result.error = ERR_SKIP;
					break;
				}
				if (!directory->current_is_dir() && name.get_extension().to_lower() == "escn") {
					add_file(job->relative.path_join(name));
					if (job->result.error != OK) {
						break;
					}
				}
			}
			directory->list_dir_end();
		}
		HashMap<String, bool> required_files;
		for (const KeyValue<String, bool> &entry : job->required_files) {
			if (job->result.error != OK) {
				break;
			}
			add_file(entry.key);
#ifdef WINDOWS_ENABLED
			required_files.insert(entry.key.to_lower(), entry.value);
#else
			required_files.insert(entry.key, entry.value);
#endif
		}
		job->required_files = std::move(required_files);
		for (const KeyValue<String, String> &entry : files) {
			job->filenames.push_back(entry.value);
		}
		job->filenames.sort();
	} else {
		job->result.error = ERR_SKIP;
	}
	if (job->profile) {
		job->enumerate_usec = OS::get_singleton()->get_ticks_usec() - began;
		job->worker_total_usec += job->enumerate_usec;
	}
}

Error EntityScene::_read_cell_range(const CellJob &p_job, uint32_t p_index, CellReadRange &r_range) {
	const uint32_t begin = p_index * CELL_READ_RANGE_FILES;
	const uint32_t end = MIN(begin + CELL_READ_RANGE_FILES, uint32_t(p_job.filenames.size()));
	for (uint32_t file_index = begin; file_index < end; file_index++) {
		if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
			return ERR_SKIP;
		}
		const String &relative = p_job.filenames[file_index];
		const String name = relative.get_file();
		const String path = p_job.storage_directory.path_join(relative);
#ifdef WINDOWS_ENABLED
		const bool *required_cluster = p_job.required_files.getptr(relative.to_lower());
#else
		const bool *required_cluster = p_job.required_files.getptr(relative);
#endif
		const bool cluster = required_cluster ? *required_cluster : name.ends_with(".cluster.escn");
		EntityId file_id;
		if (!cluster) {
			if (EntityId::parse(name.get_basename(), file_id) != OK || !file_id.is_valid()) {
				r_range.failing_field = "record (" + path + ")";
				return ERR_FILE_CORRUPT;
			}
			if (_snapshot_has(p_job.skip, file_id)) {
				r_range.records.push_back({ file_id, relative, false, Dictionary() });
				continue;
			}
		}
		Variant parsed;
		const uint64_t began = p_job.profile ? OS::get_singleton()->get_ticks_usec() : 0;
		Error error = EntitySceneIO::read_variant_file(path, parsed, &p_job);
		if (p_job.profile) {
			r_range.read_parse_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		r_range.files_read++;
		if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
			error = ERR_FILE_CORRUPT;
		}
		if (error != OK) {
			r_range.failing_field = (cluster ? "cluster (" : "record (") + path + ")";
			return error;
		}
		if (cluster) {
			const uint32_t first_record = r_range.records.size();
			const Dictionary entries = parsed;
			for (const Variant &key : entries.get_key_list()) {
				EntityId id;
				if (key.get_type() != Variant::STRING || EntityId::parse(key, id) != OK || !id.is_valid() || entries[key].get_type() != Variant::DICTIONARY) {
					r_range.records.resize(first_record);
					r_range.failing_field = "cluster (" + path + ")";
					return ERR_FILE_CORRUPT;
				}
				r_range.records.push_back({ id, relative, cluster, entries[key] });
			}
		} else {
			r_range.records.push_back({ file_id, relative, cluster, parsed });
		}
	}
	return OK;
}

void EntityScene::_run_cell_read_range(void *p_job, uint32_t p_index) {
	CellJob &job = *static_cast<CellJob *>(p_job);
	CellReadRange &range = job.read_ranges[p_index];
	if (job.profile) {
		range.began_usec = OS::get_singleton()->get_ticks_usec();
	}
	range.error = _read_cell_range(job, p_index, range);
	if (job.profile) {
		range.ended_usec = OS::get_singleton()->get_ticks_usec();
		range.worker_total_usec = range.ended_usec - range.began_usec;
	}
}

Error EntityScene::_merge_cell_reads(CellJob &p_job) {
	uint32_t record_count = 0;
	for (const CellReadRange &range : p_job.read_ranges) {
		record_count += range.records.size();
	}
	if (!p_job.reserve_payload((uint64_t(p_job.members.size()) + record_count) * sizeof(EntityId) * 4 + 64)) {
		return ERR_OUT_OF_MEMORY;
	}
	p_job.members.reserve(p_job.members.size() + record_count);
	struct Owner {
		String path;
		bool seen_file = false;
	};
	HashMap<EntityId, Owner, EntityIdHasher> owners;
	owners.reserve(p_job.pending.size() + record_count);
	for (const PendingRecord &record : p_job.pending) {
		const String path = record.path.is_empty() ? "<inline/prefab record>" : p_job.storage_directory.path_join(record.path);
		const Owner *owner = owners.getptr(record.id);
		if (owner) {
			p_job.result.failing = record.id;
			p_job.result.failing_field = "record (" + path + " and " + owner->path + ")";
			return ERR_FILE_CORRUPT;
		}
		owners.insert(record.id, { path, false });
	}
	p_job.pending.reserve(p_job.pending.size() + record_count);
	for (CellReadRange &range : p_job.read_ranges) {
		for (PendingRecord &record : range.records) {
			const String path = p_job.storage_directory.path_join(record.path);
			if (record.path.get_base_dir() == p_job.relative) {
				p_job.members.push_back(record.id);
			}
			Owner *owner = owners.getptr(record.id);
			if (owner) {
				const String normalized_path = path.simplify_path();
				const String normalized_owner = owner->path.simplify_path();
#ifdef WINDOWS_ENABLED
				const bool same_source = normalized_path.to_lower() == normalized_owner.to_lower();
#else
				const bool same_source = normalized_path == normalized_owner;
#endif
				if (!owner->seen_file && same_source) {
					owner->seen_file = true;
					continue;
				}
				p_job.result.failing = record.id;
				p_job.result.failing_field = "record (" + path + " and " + owner->path + ")";
				return ERR_FILE_CORRUPT;
			}
			owners.insert(record.id, { path, true });
			if (_snapshot_has(p_job.skip, record.id) || (record.path.get_base_dir() != p_job.relative && !_snapshot_has(p_job.required, record.id))) {
				continue;
			}
			p_job.pending.push_back(std::move(record));
		}
		if (range.error != OK) {
			p_job.result.failing_field = range.failing_field;
			return range.error;
		}
	}
	for (EntityId id : p_job.required) {
		if (!_snapshot_has(p_job.skip, id) && !owners.has(id)) {
			p_job.result.failing = id;
			p_job.result.failing_field = "record";
			return ERR_FILE_CORRUPT;
		}
	}
	_sort_snapshot_ids(p_job.members);
	p_job.pending.sort();
	return OK;
}

uint32_t EntityScene::CellJob::enumerate() {
	_enumerate_cell(this);
	if (result.error != OK) {
		return 0;
	}
	const uint32_t ranges = (uint32_t(filenames.size()) + CELL_READ_RANGE_FILES - 1) / CELL_READ_RANGE_FILES;
	if (!reserve_payload(uint64_t(ranges) * sizeof(CellReadRange))) {
		result.error = ERR_OUT_OF_MEMORY;
		result.failing_field = "read range arena";
		return 0;
	}
	read_ranges.resize(ranges);
	read_range_count = ranges;
	read_lanes = MIN(ranges, MAX_READ_LANES);
	return ranges;
}

void EntityScene::CellJob::read_range(uint32_t p_index) {
	_run_cell_read_range(this, p_index);
}

uint32_t EntityScene::CellJob::prepare(bool p_decode_only) {
	CellJob &p_job = *this;
	if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
		p_job.result.error = ERR_SKIP;
		return 0;
	}
	if (p_job.refresh_only) {
		return 0;
	}
	if (!p_decode_only && p_job.result.error == OK) {
		uint64_t first = UINT64_MAX;
		uint64_t last = 0;
		for (const CellReadRange &range : p_job.read_ranges) {
			p_job.worker_total_usec += range.worker_total_usec;
			p_job.parse_usec += range.worker_total_usec;
			p_job.read_parse_usec += range.read_parse_usec;
			p_job.files_read += range.files_read;
			first = MIN(first, range.began_usec);
			last = MAX(last, range.ended_usec);
		}
		if (p_job.profile && !p_job.read_ranges.is_empty()) {
			p_job.read_parse_wall_usec = last - first;
		}
		p_job.result.error = _merge_cell_reads(p_job);
		p_job.read_ranges.clear();
		p_job.filenames.clear();
	}
	if (p_job.result.error == OK) {
		p_job.result.error = _plan_cell_decode(p_job);
	}
	return p_job.result.error == OK ? p_job.decode_ranges.size() : 0;
}

void EntityScene::CellJob::prepare_range(uint32_t p_index) {
	_run_cell_decode_range(*this, p_index);
}

void EntityScene::CellJob::finish_prepare() {
	const uint64_t began = profile ? OS::get_singleton()->get_ticks_usec() : 0;
	result.error = _finish_cell_decode(*this, refresh_only);
	refresh_only = false;
	if (profile) {
		worker_total_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
}

EntityScene::CleanupJob::~CleanupJob() {
	if (payload) {
		memdelete(payload);
	}
}

void EntityScene::CleanupJob::finish_prepare() {
	memdelete(payload);
	payload = nullptr;
}

void EntityScene::_collect_cell_jobs() {
	while (EntityTaskScheduler::Graph *graph = cell_mailbox->pop()) {
		graph->ready = true;
	}
}

void EntityScene::_collect_cleanup_jobs() {
	while (EntityTaskScheduler::Graph *graph = cleanup_mailbox->pop()) {
		CleanupJob *job = static_cast<CleanupJob *>(graph);
		for (uint32_t i = 0; i < cleanup_jobs.size(); i++) {
			if (cleanup_jobs[i] == job) {
				cleanup_jobs.remove_at(i);
				break;
			}
		}
		memdelete(job);
	}
	while (!pending_cleanup_jobs.is_empty()) {
		CellJob *payload = pending_cleanup_jobs[pending_cleanup_jobs.size() - 1];
		EntityTaskScheduler *scheduler = EntityTaskScheduler::get_singleton();
		EntityTaskScheduler::Reservation reservation;
		if (!scheduler || scheduler->reserve(reservation, 0, true) != OK) {
			break;
		}
		CleanupJob *cleanup = memnew(CleanupJob(payload));
		scheduler->adopt(cleanup, reservation);
		const Error error = scheduler->submit(cleanup, cleanup_mailbox, true, EntityTaskScheduler::LOW);
		if (error != OK) {
			memdelete(cleanup);
			pending_cleanup_jobs.resize(pending_cleanup_jobs.size() - 1);
			continue;
		}
		pending_cleanup_jobs.resize(pending_cleanup_jobs.size() - 1);
		cleanup_jobs.push_back(cleanup);
	}
}

void EntityScene::_defer_job_cleanup(CellJob *p_job) {
	p_job->assets.clear();
	pending_cleanup_jobs.push_back(p_job);
	_collect_cleanup_jobs();
}

void EntityScene::_flush_cleanup_jobs() {
	while (!cleanup_jobs.is_empty() || !pending_cleanup_jobs.is_empty()) {
		catalog.maintenance();
		_collect_cleanup_jobs();
		if (!cleanup_jobs.is_empty() || !pending_cleanup_jobs.is_empty()) {
			OS::get_singleton()->delay_usec(1000);
		}
	}
}

Error EntityScene::_plan_cell_decode(CellJob &p_job) {
	if (!p_job.result.grouped) {
		p_job.result.entities.reserve(p_job.pending.size());
		Vector<uint32_t> signature_rows;
		signature_rows.reserve(p_job.pending.size());
		for (uint32_t pending_index = 0; pending_index < p_job.pending.size(); pending_index++) {
			PendingRecord &record = p_job.pending[pending_index];
			if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
				return ERR_SKIP;
			}
			PreparedEntity prepared;
			prepared.path = record.path;
			prepared.cluster = record.cluster;
			String field;
			const Error error = _prepare_stored(record.id, record.record, prepared, record.signature, field);
			if (error != OK) {
				p_job.result.failing = record.id;
				p_job.result.failing_field = field + " (" + p_job.storage_directory.path_join(record.path) + ")";
				return error;
			}
			if (!prepared.deleted) {
				record.signature.sort();
				signature_rows.push_back(pending_index);
			}
			record.prepared_index = p_job.result.entities.size();
			p_job.result.entities.push_back(std::move(prepared));
		}
		signature_rows.sort_custom<PendingSignatureOrder>(&p_job.pending);
		PreparedGroup *group = nullptr;
		Vector<uint64_t> previous_signature;
		for (uint32_t pending_index : signature_rows) {
			PendingRecord &record = p_job.pending[pending_index];
			if (!group || record.signature != previous_signature) {
				group = memnew(PreparedGroup);
				group->profile = p_job.profile;
				group->signature = record.signature;
				p_job.result.groups.push_back(group);
				previous_signature = record.signature;
			}
			PreparedEntity &prepared = p_job.result.entities[record.prepared_index];
			prepared.group = group;
			prepared.group_index = p_job.result.groups.size() - 1;
			prepared.row = group->capacity++;
		}
		for (PreparedGroup *group : p_job.result.groups) {
			if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
				return ERR_SKIP;
			}
			uint64_t bytes = uint64_t(group->capacity) * sizeof(uint32_t);
			for (uint64_t id : group->signature) {
				const EntityComponentSchema *schema = EntitySchemaRegistry::descriptors().find(id);
				if (!schema || uint64_t(group->capacity) > CellJob::BYTE_BUDGET / MAX(size_t(1), schema->size)) {
					p_job.result.failing_field = "prepared column arena";
					return ERR_OUT_OF_MEMORY;
				}
				bytes += uint64_t(group->capacity) * schema->size + schema->alignment - 1 + sizeof(uint32_t);
			}
			if (!p_job.reserve_payload(bytes)) {
				p_job.result.failing_field = "prepared column arena";
				return ERR_OUT_OF_MEMORY;
			}
			p_job.prepared_bytes += bytes;
			const Error error = group->allocate();
			if (error != OK) {
				p_job.result.failing_field = "prepared columns";
				return error;
			}
		}
		p_job.result.grouped = true;
	}
	const uint32_t ranges = (p_job.pending.size() + CELL_DECODE_RANGE_RECORDS - 1) / CELL_DECODE_RANGE_RECORDS;
	if (!p_job.reserve_payload(uint64_t(ranges) * sizeof(CellDecodeRange))) {
		return ERR_OUT_OF_MEMORY;
	}
	p_job.decode_ranges.resize(ranges);
	for (uint32_t i = 0; i < ranges; i++) {
		CellDecodeRange &range = p_job.decode_ranges[i];
		range.begin = i * CELL_DECODE_RANGE_RECORDS;
		range.end = MIN(range.begin + CELL_DECODE_RANGE_RECORDS, uint32_t(p_job.pending.size()));
		range.error = OK;
		range.failing = EntityId();
		range.failing_field = String();
		range.missing.clear();
		range.decode_usec = 0;
	}
	return OK;
}

void EntityScene::_run_cell_decode_range(CellJob &p_job, uint32_t p_index) {
	CellDecodeRange &range = p_job.decode_ranges[p_index];
	const uint64_t began = p_job.profile ? OS::get_singleton()->get_ticks_usec() : 0;
	entity_decode_assets_cached_only(true);
	entity_decode_set_budget([](void *p_userdata, uint64_t p_bytes) {
		return static_cast<CellJob *>(p_userdata)->reserve_payload(p_bytes);
	},
			&p_job);
	for (uint32_t i = range.begin; i < range.end; i++) {
		if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
			range.error = ERR_SKIP;
			break;
		}
		PendingRecord &record = p_job.pending[i];
		PreparedEntity &prepared = p_job.result.entities[record.prepared_index];
		String field;
		const Error record_error = prepared.deleted ? OK : prepared.group->decode_row(prepared.row, record.record["components"], field);
		if (record_error == ERR_UNAVAILABLE) {
			if (range.error == OK) {
				range.error = ERR_UNAVAILABLE;
				range.failing = record.id;
				range.failing_field = field + " (" + p_job.storage_directory.path_join(record.path) + ")";
			}
			continue;
		}
		if (record_error != OK) {
			range.error = record_error;
			range.failing = record.id;
			range.failing_field = field + " (" + p_job.directory.path_join(record.path.get_file()) + ")";
			break;
		}
		record.record = Dictionary();
	}
	entity_decode_set_budget(nullptr);
	entity_decode_assets_cached_only(false);
	entity_decode_take_missing_assets(range.missing);
	if (p_job.profile) {
		range.decode_usec = OS::get_singleton()->get_ticks_usec() - began;
	}
}

Error EntityScene::_finish_cell_decode(CellJob &p_job, bool p_refresh_only) {
	Error result = OK;
	if (!p_refresh_only) {
		p_job.missing.clear();
		uint32_t remaining = 0;
		for (const CellDecodeRange &range : p_job.decode_ranges) {
			p_job.decode_usec += range.decode_usec;
			p_job.worker_total_usec += range.decode_usec;
			if (range.error != OK && result == OK) {
				result = range.error;
				p_job.result.failing = range.failing;
				p_job.result.failing_field = range.failing_field;
			}
			for (const String &path : range.missing) {
				if (!p_job.missing.has(path)) {
					p_job.missing.push_back(path);
				}
			}
		}
		if (result != OK && result != ERR_UNAVAILABLE) {
			p_job.decode_ranges.clear();
			return result;
		}
		for (uint32_t i = 0; i < p_job.pending.size(); i++) {
			if (!p_job.pending[i].record.is_empty()) {
				if (remaining != i) {
					p_job.pending[remaining] = std::move(p_job.pending[i]);
				}
				remaining++;
			}
		}
		p_job.pending.resize(remaining);
		p_job.decode_ranges.clear();
		if (result == ERR_UNAVAILABLE) {
			p_job.missing.sort();
			return result;
		}
	}
	const uint64_t inside_count = uint64_t(p_job.skip.size()) + p_job.result.entities.size();
	const uint64_t hierarchy_bytes = inside_count * sizeof(EntityId) * 4 + uint64_t(p_job.result.entities.size()) * (sizeof(uint32_t) * 5 + sizeof(int32_t)) + 128;
	if (!p_refresh_only && !p_job.reserve_payload(hierarchy_bytes)) {
		return ERR_OUT_OF_MEMORY;
	}
	p_job.result.sorted_rows.clear();
	p_job.result.parent_rows.clear();
	p_job.result.child_offsets.clear();
	p_job.result.children.clear();
	p_job.result.layer_offsets.clear();
	p_job.result.layer_rows.clear();
	p_job.result.catalog_ids.clear();
	p_job.result.catalog_records.clear();
	p_job.result.catalog_topological_rows.clear();
	p_job.result.catalog_child_offsets.clear();
	p_job.result.catalog_child_rows.clear();
	p_job.result.parent_edge_rows.clear();
	p_job.result.catalog_archetypes.clear();
	Vector<EntityId> inside;
	inside.resize(inside_count);
	int inside_index = 0;
	for (EntityId id : p_job.skip) {
		inside.write[inside_index++] = id;
	}
	for (const PreparedEntity &entity : p_job.result.entities) {
		inside.write[inside_index++] = entity.id;
	}
	_sort_snapshot_ids(inside);
	for (const PreparedEntity &entity : p_job.result.entities) {
		const EntityId parent = entity.parent.id;
		if (!parent.is_valid() || _snapshot_has(inside, parent) || _snapshot_has(p_job.globals, parent)) {
			continue;
		}
		p_job.result.failing = entity.id;
		p_job.result.failing_field = "parent (" + p_job.storage_directory.path_join(entity.path) + ")";
		return ERR_INVALID_DATA;
	}
	const uint32_t entity_count = p_job.result.entities.size();
	p_job.result.sorted_rows.resize(entity_count);
	p_job.result.parent_rows.resize(entity_count);
	p_job.result.child_offsets.resize(entity_count + 1);
	for (uint32_t row = 0; row < entity_count; row++) {
		p_job.result.sorted_rows.write[row] = row;
		p_job.result.parent_rows.write[row] = -1;
		p_job.result.child_offsets.write[row] = 0;
	}
	p_job.result.child_offsets.write[entity_count] = 0;
	p_job.result.sorted_rows.sort_custom<PreparedSet::RowOrder>(&p_job.result.entities);
	auto find_row = [&](EntityId p_id) -> int32_t {
		int32_t low = 0;
		int32_t high = p_job.result.sorted_rows.size() - 1;
		while (low <= high) {
			const int32_t middle = low + (high - low) / 2;
			const EntityId candidate = p_job.result.entities[p_job.result.sorted_rows[middle]].id;
			if (candidate == p_id) {
				return p_job.result.sorted_rows[middle];
			}
			if (candidate.high < p_id.high || (candidate.high == p_id.high && candidate.low < p_id.low)) {
				low = middle + 1;
			} else {
				high = middle - 1;
			}
		}
		return -1;
	};
	for (uint32_t row = 0; row < entity_count; row++) {
		const EntityId parent = p_job.result.entities[row].parent.id;
		const int32_t parent_row = parent.is_valid() ? find_row(parent) : -1;
		p_job.result.parent_rows.write[row] = parent_row;
		if (parent_row >= 0) {
			p_job.result.child_offsets.write[parent_row + 1]++;
		}
	}
	for (uint32_t row = 1; row <= entity_count; row++) {
		p_job.result.child_offsets.write[row] += p_job.result.child_offsets[row - 1];
	}
	p_job.result.children.resize(p_job.result.child_offsets[entity_count]);
	Vector<uint32_t> child_cursor = p_job.result.child_offsets;
	for (uint32_t row = 0; row < entity_count; row++) {
		const int32_t parent_row = p_job.result.parent_rows[row];
		if (parent_row >= 0) {
			p_job.result.children.write[child_cursor.write[parent_row]++] = row;
		}
	}
	Vector<uint32_t> frontier;
	for (uint32_t row = 0; row < entity_count; row++) {
		if (p_job.result.parent_rows[row] < 0) {
			frontier.push_back(row);
		}
	}
	p_job.result.layer_offsets.push_back(0);
	while (!frontier.is_empty()) {
		Vector<uint32_t> next;
		for (uint32_t row : frontier) {
			p_job.result.layer_rows.push_back(row);
			for (uint32_t child = p_job.result.child_offsets[row]; child < p_job.result.child_offsets[row + 1]; child++) {
				next.push_back(p_job.result.children[child]);
			}
		}
		p_job.result.layer_offsets.push_back(p_job.result.layer_rows.size());
		frontier = std::move(next);
	}
	if (p_job.result.layer_rows.size() != entity_count) {
		return ERR_CYCLIC_LINK;
	}
	auto column = [](PreparedEntity &p_entity, uint64_t p_type) -> void * {
		if (!p_entity.group) {
			return nullptr;
		}
		for (const PreparedColumn &prepared_column : p_entity.group->columns) {
			if (prepared_column.schema->id == p_type) {
				return prepared_column.row(p_entity.row);
			}
		}
		return nullptr;
	};
	auto external = [&](EntityId p_id) -> const ExternalTransformSnapshot * {
		int32_t low = 0;
		int32_t high = p_job.external_transforms.size() - 1;
		while (low <= high) {
			const int32_t middle = low + (high - low) / 2;
			const ExternalTransformSnapshot &candidate = p_job.external_transforms[middle];
			if (candidate.id == p_id) {
				return &candidate;
			}
			if (SnapshotIdOrder()(candidate.id, p_id)) {
				low = middle + 1;
			} else {
				high = middle - 1;
			}
		}
		return nullptr;
	};
	for (uint32_t row : p_job.result.layer_rows) {
		PreparedEntity &entity = p_job.result.entities[row];
		EntityTransform *transform = static_cast<EntityTransform *>(column(entity, EntityComponentTraits<EntityTransform>::id));
		if (transform) {
			EntityPose pose = transform->local;
			int32_t parent_row = p_job.result.parent_rows[row];
			EntityId external_parent = entity.parent.id;
			while (parent_row >= 0) {
				PreparedEntity &parent = p_job.result.entities[parent_row];
				if (EntityTransform *parent_transform = static_cast<EntityTransform *>(column(parent, EntityComponentTraits<EntityTransform>::id))) {
					pose = EntityTransformSystem::compose(parent_transform->current, pose);
					external_parent = EntityId();
					break;
				}
				external_parent = parent.parent.id;
				parent_row = p_job.result.parent_rows[parent_row];
			}
			while (external_parent.is_valid()) {
				const ExternalTransformSnapshot *snapshot = external(external_parent);
				if (!snapshot) {
					break;
				}
				if (snapshot->has_transform) {
					pose = EntityTransformSystem::compose(snapshot->pose, pose);
					break;
				}
				external_parent = snapshot->parent.id;
			}
			transform->current = pose;
			transform->previous = pose;
			transform->render = pose;
		}
		EntityVisibility *visibility = static_cast<EntityVisibility *>(column(entity, EntityComponentTraits<EntityVisibility>::id));
		if (visibility) {
			visibility->effective = visibility->visible;
			if (visibility->inherit_parent && entity.parent.id.is_valid()) {
				const int32_t parent_row = p_job.result.parent_rows[row];
				if (parent_row >= 0) {
					if (EntityVisibility *parent_visibility = static_cast<EntityVisibility *>(column(p_job.result.entities[parent_row], EntityComponentTraits<EntityVisibility>::id))) {
						visibility->effective &= parent_visibility->effective;
					}
				} else if (const ExternalTransformSnapshot *snapshot = external(entity.parent.id)) {
					if (snapshot->has_visibility) {
						visibility->effective &= snapshot->visible;
					}
				}
			}
		}
	}
	for (PreparedGroup *group : p_job.result.groups) {
		group->ids.resize(group->capacity);
		group->catalog_rows.resize(group->capacity);
		group->render_updates.resize(group->capacity);
	}
	auto copy_render = [&](PreparedEntity &p_entity) {
		if (!p_entity.group || p_entity.deleted) {
			return;
		}
		PreparedGroup &group = *p_entity.group;
		group.ids[p_entity.row] = p_entity.id;
		EntityRenderUpdate &update = group.render_updates.write[p_entity.row];
		update = EntityRenderUpdate();
		update.id = p_entity.id;
		for (const PreparedColumn &prepared_column : group.columns) {
			const uint64_t id = prepared_column.schema->id;
			const void *value = prepared_column.row(p_entity.row);
			update.components |= EntityRenderSystem::component_mask(id) & EntityRenderUpdate::COMPONENTS;
			if (id == EntityComponentTraits<EntityTransform>::id) {
				update.pose = static_cast<const EntityTransform *>(value)->render;
			} else if (id == EntityComponentTraits<EntityVisibility>::id) {
				update.visible = static_cast<const EntityVisibility *>(value)->effective;
			} else if (id == EntityComponentTraits<EntityMesh>::id) {
				update.mesh = *static_cast<const EntityMesh *>(value);
			} else if (id == EntityComponentTraits<EntityGeometry>::id) {
				update.geometry = *static_cast<const EntityGeometry *>(value);
				update.procedural = update.geometry.rt_procedural;
			} else if (id == EntityComponentTraits<EntityCamera>::id) {
				update.camera = *static_cast<const EntityCamera *>(value);
			} else if (id == EntityComponentTraits<EntityLight>::id) {
				update.light = *static_cast<const EntityLight *>(value);
			} else if (id == EntityComponentTraits<EntityEnvironment>::id) {
				update.environment = *static_cast<const EntityEnvironment *>(value);
			} else if (id == EntityComponentTraits<EntityMultiMesh>::id) {
				update.multimesh = *static_cast<const EntityMultiMesh *>(value);
			} else if (id == EntityComponentTraits<EntityDecal>::id) {
				update.decal = *static_cast<const EntityDecal *>(value);
			} else if (id == EntityComponentTraits<EntityFogVolume>::id) {
				update.fog_volume = *static_cast<const EntityFogVolume *>(value);
			} else if (id == EntityComponentTraits<EntityReflectionProbe>::id) {
				update.reflection_probe = *static_cast<const EntityReflectionProbe *>(value);
			} else if (id == EntityComponentTraits<EntityParticles>::id) {
				update.particles = *static_cast<const EntityParticles *>(value);
			} else if (id == EntityComponentTraits<EntityVoxelGI>::id) {
				update.voxel_gi = *static_cast<const EntityVoxelGI *>(value);
			} else if (id == EntityComponentTraits<EntityLightmap>::id) {
				update.lightmap = *static_cast<const EntityLightmap *>(value);
			} else if (id == EntityComponentTraits<EntitySkinningPose>::id) {
				update.skinning_pose = *static_cast<const EntitySkinningPose *>(value);
			} else if (id == EntityComponentTraits<EntityParticlesCollision>::id) {
				update.particles_collision = *static_cast<const EntityParticlesCollision *>(value);
			}
		}
		update.changed_components = EntityRenderUpdate::COMPONENTS;
	};
	for (PreparedEntity &entity : p_job.result.entities) {
		copy_render(entity);
	}
	Vector<uint32_t> original_to_catalog;
	original_to_catalog.resize(entity_count);
	p_job.result.catalog_ids.resize(entity_count);
	p_job.result.catalog_records.resize(entity_count);
	for (uint32_t catalog_row = 0; catalog_row < entity_count; catalog_row++) {
		const uint32_t original_row = p_job.result.sorted_rows[catalog_row];
		PreparedEntity &entity = p_job.result.entities[original_row];
		original_to_catalog.write[original_row] = catalog_row;
		p_job.result.catalog_ids.write[catalog_row] = entity.id;
		EntityCatalog::Record &record = p_job.result.catalog_records.write[catalog_row];
		record.deleted = entity.deleted;
		record.parent = entity.parent;
		record.order = entity.order;
		record.has_order = entity.order != 0;
		record.cell_grid = p_job.result.key.grid;
		record.cell_x = p_job.result.key.x;
		record.cell_y = p_job.result.key.y;
		record.cell_z = p_job.result.key.z;
		record.has_cell = !entity.deleted;
		if (!entity.deleted) {
			record.section.name = entity.name;
			record.section.path = entity.path;
			record.section.cluster = entity.cluster;
			record.section.components = entity.components;
			record.has_section = true;
		}
		const Vector<uint64_t> signature = entity.group ? entity.group->signature : Vector<uint64_t>();
		if (!p_job.result.catalog_archetypes.is_empty() && p_job.result.catalog_archetypes[p_job.result.catalog_archetypes.size() - 1].signature == signature) {
			p_job.result.catalog_archetypes.write[p_job.result.catalog_archetypes.size() - 1].count++;
		} else {
			p_job.result.catalog_archetypes.push_back({ signature, catalog_row, 1 });
		}
	}
	for (uint32_t row = 0; row < entity_count; row++) {
		const PreparedEntity &entity = p_job.result.entities[row];
		if (entity.group && !entity.deleted) {
			entity.group->catalog_rows[entity.row] = original_to_catalog[row];
		}
	}
	p_job.result.catalog_topological_rows.resize(entity_count);
	for (uint32_t i = 0; i < entity_count; i++) {
		p_job.result.catalog_topological_rows.write[i] = original_to_catalog[p_job.result.layer_rows[i]];
	}
	p_job.result.catalog_child_offsets.resize_initialized(entity_count + 1);
	for (uint32_t row = 0; row < entity_count; row++) {
		const int32_t parent_row = p_job.result.parent_rows[row];
		if (parent_row >= 0) {
			p_job.result.catalog_child_offsets.write[original_to_catalog[parent_row] + 1]++;
		}
	}
	for (uint32_t row = 1; row <= entity_count; row++) {
		p_job.result.catalog_child_offsets.write[row] += p_job.result.catalog_child_offsets[row - 1];
	}
	p_job.result.catalog_child_rows.resize(p_job.result.catalog_child_offsets[entity_count]);
	Vector<uint32_t> catalog_child_cursor = p_job.result.catalog_child_offsets;
	for (uint32_t row = 0; row < entity_count; row++) {
		const int32_t parent_row = p_job.result.parent_rows[row];
		if (p_job.result.entities[row].parent.id.is_valid()) {
			p_job.result.parent_edge_rows.push_back(original_to_catalog[row]);
		}
		if (parent_row >= 0) {
			const uint32_t catalog_parent = original_to_catalog[parent_row];
			p_job.result.catalog_child_rows.write[catalog_child_cursor.write[catalog_parent]++] = original_to_catalog[row];
		}
	}
	return OK;
}

Error EntityScene::_revalidate_job(CellJob &p_job) {
	p_job.refresh_requested = false;
	if (resident_cells.has(p_job.result.key)) {
		return ERR_BUSY;
	}
	for (const PreparedEntity &entity : p_job.result.entities) {
		if (catalog.has_record(entity.id)) {
			return ERR_BUSY;
		}
	}
	for (const ExternalTransformSnapshot &snapshot : p_job.external_transforms) {
		const EntityCatalog::RowState *state = catalog.get_state_ptr(snapshot.id);
		if (!state || !state->resident || state->revision != snapshot.revision) {
			p_job.refresh_requested = true;
			return ERR_BUSY;
		}
	}
	return OK;
}

Error EntityScene::_refresh_external_transforms(CellJob &p_job) {
	Vector<EntityId> ids;
	ids.reserve(p_job.external_transforms.size());
	for (const ExternalTransformSnapshot &snapshot : p_job.external_transforms) {
		ids.push_back(snapshot.id);
	}
	Vector<EntityId> ancestors = p_job.ancestors;
	_sort_snapshot_ids(ancestors);
	uint32_t cursor = 0;
	while (cursor < ids.size()) {
		const EntityId id = ids[cursor++];
		const EntityResolution resolved = resolve(id);
		if (resolved.state != EntityReferenceState::RESIDENT) {
			return ERR_BUSY;
		}
		const EntityRef parent = catalog.get_parent(id);
		if (parent.id.is_valid() && !_snapshot_has(ids, parent.id)) {
			ids.push_back(parent.id);
			_sort_snapshot_ids(ids);
			cursor = 0;
		}
	}
	cursor = 0;
	while (cursor < ancestors.size()) {
		const EntityId id = ancestors[cursor++];
		if (resolve(id).state != EntityReferenceState::RESIDENT) {
			return ERR_BUSY;
		}
		const EntityRef parent = catalog.get_parent(id);
		if (parent.id.is_valid() && !_snapshot_has(ancestors, parent.id)) {
			ancestors.push_back(parent.id);
			_sort_snapshot_ids(ancestors);
			cursor = 0;
		}
	}
	Vector<ExternalTransformSnapshot> refreshed;
	refreshed.reserve(ids.size());
	for (EntityId id : ids) {
		const EntityResolution resolved = resolve(id);
		const EntityCatalog::RowState *state = catalog.get_state_ptr(id);
		if (resolved.state != EntityReferenceState::RESIDENT || !state) {
			return ERR_BUSY;
		}
		ExternalTransformSnapshot snapshot;
		snapshot.id = id;
		snapshot.parent = catalog.get_parent(id);
		snapshot.revision = state->revision;
		if (const EntityTransform *transform = world->get<EntityTransform>(resolved.handle)) {
			snapshot.pose = transform->current;
			snapshot.has_transform = true;
		}
		if (const EntityVisibility *visibility = world->get<EntityVisibility>(resolved.handle)) {
			snapshot.visible = visibility->effective;
			snapshot.has_visibility = true;
		}
		refreshed.push_back(snapshot);
	}
	p_job.external_transforms = std::move(refreshed);
	p_job.ancestors = std::move(ancestors);
	return OK;
}

Error EntityScene::_commit_prepared_cell(CellJob &p_job, CommitProfile *r_profile) {
	EntityWorld *target = get_world();
	ERR_FAIL_NULL_V(target, ERR_UNCONFIGURED);
	for (const ExternalTransformSnapshot &snapshot : p_job.external_transforms) {
		const EntityCatalog::RowState *state = catalog.get_state_ptr(snapshot.id);
		if (!state || !state->resident || state->revision != snapshot.revision) {
			return ERR_BUSY;
		}
	}
	const uint32_t entity_count = p_job.result.catalog_ids.size();
	ERR_FAIL_COND_V(p_job.result.catalog_records.size() != entity_count || p_job.result.catalog_child_offsets.size() != entity_count + 1 || p_job.result.catalog_topological_rows.size() != entity_count, ERR_INVALID_DATA);
	uint32_t archetype_end = 0;
	for (const EntityCatalog::ArchetypeSpan &span : p_job.result.catalog_archetypes) {
		ERR_FAIL_COND_V(span.first != archetype_end || span.count > entity_count - span.first, ERR_INVALID_DATA);
		archetype_end += span.count;
	}
	ERR_FAIL_COND_V(archetype_end != entity_count, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_job.result.catalog_child_offsets[0] != 0 || p_job.result.catalog_child_offsets[entity_count] != uint32_t(p_job.result.catalog_child_rows.size()), ERR_INVALID_DATA);
	Vector<uint8_t> topological_rows;
	topological_rows.resize_initialized(entity_count);
	for (uint32_t row : p_job.result.catalog_topological_rows) {
		ERR_FAIL_COND_V(row >= entity_count || topological_rows[row], ERR_INVALID_DATA);
		topological_rows.write[row] = 1;
	}
	for (uint32_t row = 0; row < entity_count; row++) {
		ERR_FAIL_COND_V(p_job.result.catalog_child_offsets[row] > p_job.result.catalog_child_offsets[row + 1], ERR_INVALID_DATA);
	}
	for (uint32_t row : p_job.result.catalog_child_rows) {
		ERR_FAIL_COND_V(row >= entity_count, ERR_INVALID_DATA);
	}
	Vector<uint8_t> materialized_rows;
	materialized_rows.resize_initialized(entity_count);
	for (PreparedGroup *group : p_job.result.groups) {
		ERR_FAIL_NULL_V(group, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(group->ids.size() != group->capacity || group->catalog_rows.size() != group->capacity || group->render_updates.size() != group->capacity || group->columns.size() + 3 >= FLECS_ID_DESC_MAX, ERR_INVALID_DATA);
		for (const PreparedColumn &column : group->columns) {
			ERR_FAIL_COND_V(!column.schema || !target->schemas.find(column.schema->id) || (group->capacity && !column.buffer), ERR_INVALID_DATA);
		}
		for (uint32_t row : group->catalog_rows) {
			ERR_FAIL_COND_V(row >= entity_count || materialized_rows[row], ERR_INVALID_DATA);
			ERR_FAIL_COND_V(p_job.result.catalog_records[row].deleted, ERR_INVALID_DATA);
			materialized_rows.write[row] = 1;
		}
		for (uint32_t row = 0; row < group->capacity; row++) {
			ERR_FAIL_COND_V(group->ids[row] != p_job.result.catalog_ids[group->catalog_rows[row]] || group->render_updates[row].id != group->ids[row], ERR_INVALID_DATA);
		}
	}
	for (uint32_t row = 0; row < entity_count; row++) {
		ERR_FAIL_COND_V(bool(materialized_rows[row]) == p_job.result.catalog_records[row].deleted, ERR_INVALID_DATA);
	}
	struct PreparedParentEdge {
		uint32_t child = 0;
		uint32_t parent = UINT32_MAX;
		EntityHandle external;
	};
	LocalVector<PreparedParentEdge> parent_edges;
	parent_edges.reserve(p_job.result.parent_edge_rows.size());
	auto find_catalog_row = [&](EntityId p_id) -> uint32_t {
		int32_t low = 0;
		int32_t high = p_job.result.catalog_ids.size() - 1;
		while (low <= high) {
			const int32_t middle = low + (high - low) / 2;
			const EntityId candidate = p_job.result.catalog_ids[middle];
			if (candidate == p_id) {
				return middle;
			}
			if (SnapshotIdOrder()(candidate, p_id)) {
				low = middle + 1;
			} else {
				high = middle - 1;
			}
		}
		return UINT32_MAX;
	};
	for (EntityId ancestor : p_job.ancestors) {
		const uint32_t row = find_catalog_row(ancestor);
		if (row != UINT32_MAX) {
			ERR_FAIL_COND_V(!materialized_rows[row] || p_job.result.catalog_records[row].deleted, ERR_INVALID_DATA);
		} else if (resolve(ancestor).state != EntityReferenceState::RESIDENT) {
			return ERR_BUSY;
		}
	}
	for (uint32_t child : p_job.result.parent_edge_rows) {
		ERR_FAIL_COND_V(child >= entity_count, ERR_INVALID_DATA);
		const EntityCatalog::Record &record = p_job.result.catalog_records[child];
		ERR_FAIL_COND_V(record.deleted || !record.parent.id.is_valid() || !materialized_rows[child], ERR_INVALID_DATA);
		PreparedParentEdge edge;
		edge.child = child;
		edge.parent = find_catalog_row(record.parent.id);
		if (edge.parent != UINT32_MAX) {
			ERR_FAIL_COND_V(!materialized_rows[edge.parent] || p_job.result.catalog_records[edge.parent].deleted, ERR_INVALID_DATA);
		} else {
			const EntityResolution parent = resolve(record.parent.id);
			if (parent.state != EntityReferenceState::RESIDENT) {
				return ERR_BUSY;
			}
			edge.external = parent.handle;
		}
		parent_edges.push_back(edge);
	}
	const uint64_t began = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	EntityCatalog::PreparedBlock prepared_block;
	const Error prepare_error = catalog.prepare_block(std::move(p_job.result.catalog_ids), std::move(p_job.result.catalog_records), std::move(p_job.result.catalog_topological_rows), std::move(p_job.result.catalog_child_offsets), std::move(p_job.result.catalog_child_rows), std::move(p_job.result.catalog_archetypes), p_job.result.key.grid, p_job.result.key.x, p_job.result.key.y, p_job.result.key.z, prepared_block);
	if (prepare_error != OK) {
		return prepare_error;
	}
	EntityCatalog::CellRuntimeBlock *block = prepared_block.block;
	const uint32_t block_index = prepared_block.index;
	const uint64_t previous_change_serial = target->change_serial;
	const uint64_t previous_reset_serial = target->_get_transform_reset_serial();
	const uint32_t previous_storage_tags = target->bulk_storage_tags.size();
	const uint64_t row_revision = previous_change_serial + 1;
	LocalVector<EntityHandle> created;
	CommitProfile committed_profile;
	auto rollback = [&]() {
		for (EntityHandle handle : created) {
			if (target->ecs.is_alive(handle.entity)) {
				target->ecs.entity(handle.entity).destruct();
			}
		}
		for (uint32_t i = previous_storage_tags; i < target->bulk_storage_tags.size(); i++) {
			if (target->ecs.is_alive(target->bulk_storage_tags[i])) {
				target->ecs.entity(target->bulk_storage_tags[i]).destruct();
			}
		}
		target->bulk_storage_tags.resize(previous_storage_tags);
		target->_set_transform_reset_serial(previous_reset_serial);
		target->change_serial = previous_change_serial;
	};
	for (PreparedGroup *group : p_job.result.groups) {
		if (group->ids.is_empty()) {
			continue;
		}
		ecs_bulk_desc_t desc = {};
		void *data[FLECS_ID_DESC_MAX] = {};
		desc.data = data;
		for (uint32_t column_index = 0; column_index < group->columns.size(); column_index++) {
			const PreparedColumn &column = group->columns[column_index];
			const EntityComponentSchema *schema = target->schemas.find(column.schema->id);
			ERR_FAIL_NULL_V(schema, ERR_BUG);
			desc.ids[column_index + 2] = schema->runtime_id;
			data[column_index + 2] = column.buffer;
		}
		LocalVector<EntityCatalog::RowLocation> locations;
		locations.resize(group->ids.size());
		for (uint32_t row = 0; row < group->ids.size(); row++) {
			locations[row] = { block_index, block->generation, group->catalog_rows[row] };
		}
		const ecs_table_t *table = nullptr;
		ecs_entity_t storage_tag = 0;
		LocalVector<EntityHandle> handles;
		LocalVector<uint64_t> reset_revisions;
		handles.reserve(group->ids.size());
		reset_revisions.reserve(group->ids.size());
		EntityWorld::MaterializeProfile materialize_profile;
		const Error materialize_error = target->_materialize_bulk(group->ids, &locations, desc, table, storage_tag, &handles, &reset_revisions, false, r_profile ? &materialize_profile : nullptr);
		if (materialize_error != OK) {
			rollback();
			return materialize_error;
		}
		for (uint32_t row = 0; row < handles.size(); row++) {
			created.push_back(handles[row]);
			EntityRenderUpdate &update = group->render_updates.write[row];
			update.handle = handles[row];
			update.reset_revision = reset_revisions[row];
			EntityCatalog::RowState &state = block->states.write[group->catalog_rows[row]];
			state.handle = handles[row];
			state.resident = true;
			state.revision = row_revision;
		}
		committed_profile.ecs_bulk_create_components_usec += materialize_profile.ecs_bulk_create_components_usec;
		committed_profile.resident_insert_usec += materialize_profile.resident_insert_usec;
		committed_profile.entities_materialized += materialize_profile.entities_materialized;
		committed_profile.bulk_groups++;
		committed_profile.bulk_rows += group->ids.size();
		committed_profile.initial_packet_updates += group->ids.size();
	}
	for (const PreparedParentEdge &edge : parent_edges) {
		const EntityHandle parent = edge.parent == UINT32_MAX ? edge.external : block->states[edge.parent].handle;
		if (!target->ecs.is_alive(block->states[edge.child].handle.entity) || !target->ecs.is_alive(parent.entity)) {
			rollback();
			return ERR_BUSY;
		}
		target->ecs.entity(block->states[edge.child].handle.entity).set<flecs::Parent>({ parent.entity });
		committed_profile.final_parent_sets++;
	}
	const Error adopt_error = catalog.adopt_block(prepared_block);
	if (adopt_error != OK) {
		rollback();
		return adopt_error;
	}
	target->change_serial = row_revision;
	CellMembers &members = cells[p_job.result.key];
	members = std::move(p_job.members);
	for (PreparedGroup *group : p_job.result.groups) {
		target->rendering.enqueue_initial(std::move(group->render_updates));
	}
	if (r_profile) {
		committed_profile.cell_membership_insertions += members.size();
		committed_profile.metadata_new_records += block->ids.size();
		committed_profile.metadata_commit_usec += OS::get_singleton()->get_ticks_usec() - began;
		r_profile->ecs_bulk_create_components_usec += committed_profile.ecs_bulk_create_components_usec;
		r_profile->resident_insert_usec += committed_profile.resident_insert_usec;
		r_profile->entities_materialized += committed_profile.entities_materialized;
		r_profile->bulk_groups += committed_profile.bulk_groups;
		r_profile->bulk_rows += committed_profile.bulk_rows;
		r_profile->initial_packet_updates += committed_profile.initial_packet_updates;
		r_profile->final_parent_sets += committed_profile.final_parent_sets;
		r_profile->cell_membership_insertions += committed_profile.cell_membership_insertions;
		r_profile->metadata_new_records += committed_profile.metadata_new_records;
		r_profile->metadata_commit_usec += committed_profile.metadata_commit_usec;
	}
	revision++;
	return OK;
}

Error EntityScene::_request_cell_assets(CellJob &p_job) {
	p_job.asset_tickets.clear();
	p_job.asset_request_error = OK;
	for (const String &path : p_job.missing) {
		if (p_job.loaded.has(path)) {
			continue;
		}
		const Error error = ResourceLoader::load_threaded_request(path, "", true, ResourceFormatLoader::CACHE_MODE_REUSE);
		if (error != OK) {
			if (p_job.asset_request_error == OK) {
				p_job.asset_request_error = error;
			}
			continue;
		}
		p_job.asset_tickets.push_back({ path, false });
	}
	p_job.awaiting_assets = !p_job.asset_tickets.is_empty();
	return p_job.awaiting_assets ? OK : p_job.asset_request_error;
}

Error EntityScene::_poll_cell_assets(CellJob &p_job, bool p_retain) {
	bool pending = false;
	Error result = p_job.asset_request_error;
	for (CellAssetTicket &ticket : p_job.asset_tickets) {
		if (ticket.consumed) {
			continue;
		}
		const ResourceLoader::ThreadLoadStatus status = ResourceLoader::load_threaded_get_status(ticket.path);
		if (status == ResourceLoader::THREAD_LOAD_IN_PROGRESS) {
			pending = true;
			continue;
		}
		Error error = OK;
		Ref<Resource> asset = ResourceLoader::load_threaded_get(ticket.path, &error);
		ticket.consumed = true;
		if (status != ResourceLoader::THREAD_LOAD_LOADED || error != OK || asset.is_null()) {
			if (result == OK) {
				result = error == OK ? ERR_FILE_CORRUPT : error;
			}
			continue;
		}
		if (p_retain) {
			p_job.assets.push_back(asset);
			p_job.loaded.insert(ticket.path);
			cell_assets[p_job.result.key].push_back(asset);
		}
	}
	if (pending) {
		return ERR_BUSY;
	}
	p_job.asset_tickets.clear();
	p_job.awaiting_assets = false;
	p_job.asset_request_error = OK;
	return result;
}

Error EntityScene::_commit_cell(CellJob &p_job, Stats &r_stats, OwnerProfile *r_profile) {
	const bool timed = r_profile != nullptr;
	if (p_job.result.error == ERR_SKIP) {
		r_stats.jobs_discarded++;
		cell_assets.erase(p_job.result.key);
		return OK;
	}
	if (p_job.result.error == ERR_UNAVAILABLE) {
		bool progress = false;
		for (const String &path : p_job.missing) {
			progress = progress || !p_job.loaded.has(path);
		}
		Error error = ERR_FILE_NOT_FOUND;
		if (progress) {
			const uint64_t began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
			error = _request_cell_assets(p_job);
			if (timed) {
				r_profile->asset_load_usec += OS::get_singleton()->get_ticks_usec() - began;
			}
		}
		if (error != OK) {
			r_stats.jobs_discarded++;
			failed_cells.insert(p_job.result.key, revision);
			cell_assets.erase(p_job.result.key);
			return _fail(p_job.result.failing, p_job.result.failing_field, error);
		}
		p_job.resume = !p_job.awaiting_assets;
		r_stats.jobs_resumed++;
		return OK;
	}
	if (p_job.result.error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		cell_assets.erase(p_job.result.key);
		return _fail(p_job.result.failing, p_job.result.failing_field, p_job.result.error);
	}
	uint64_t began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	const Error stale = _revalidate_job(p_job);
	if (timed) {
		r_profile->revalidate_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (stale != OK) {
		if (stale == ERR_BUSY && p_job.refresh_requested) {
			const Error refresh_error = _refresh_external_transforms(p_job);
			if (refresh_error == ERR_BUSY) {
				p_job.retry_owner = true;
				return OK;
			}
			if (refresh_error != OK) {
				return refresh_error;
			}
			p_job.refresh_only = true;
			p_job.resume = true;
			r_stats.jobs_resumed++;
			return OK;
		}
		r_stats.jobs_discarded++;
		cell_assets.erase(p_job.result.key);
		if (stale != ERR_BUSY) {
			failed_cells.insert(p_job.result.key, revision);
			return _fail(p_job.result.failing, p_job.result.failing_field.is_empty() ? EntitySceneIO::cell_directory(p_job.result.key.grid, p_job.result.key.x, p_job.result.key.y, p_job.result.key.z) : p_job.result.failing_field, stale);
		}
		return OK;
	}
	const uint64_t previous_revision = revision;
	const CommitProfile previous_install = timed ? r_profile->install : CommitProfile();
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	Error error = _commit_prepared_cell(p_job, timed ? &r_profile->install : nullptr);
	if (timed) {
		r_profile->install_usec += OS::get_singleton()->get_ticks_usec() - began;
		const CommitProfile &install = r_profile->install;
		print_line(vformat("Entity cell bulk grid=%s cell=%d_%d_%d groups=%d rows=%d compacted=%d skipped=%d metadata_usec=%d metadata_new=%d metadata_record_updates=%d metadata_order_updates=%d metadata_section_updates=%d metadata_membership_removals=%d metadata_full_cell_clears=%d cell_membership_insertions=%d ecs_create_components_usec=%d compact_usec=%d parent_usec=%d transform_finalize_usec=%d initial_packet_prepare_usec=%d initial_packet_updates=%d initial_packet_table_rows=%d initial_packet_fallback_rows=%d result=%d", p_job.result.key.grid, p_job.result.key.x, p_job.result.key.y, p_job.result.key.z, install.bulk_groups - previous_install.bulk_groups, install.bulk_rows - previous_install.bulk_rows, install.compacted_rows - previous_install.compacted_rows, install.skipped_rows - previous_install.skipped_rows, install.metadata_commit_usec - previous_install.metadata_commit_usec, install.metadata_new_records - previous_install.metadata_new_records, install.metadata_record_updates - previous_install.metadata_record_updates, install.metadata_order_updates - previous_install.metadata_order_updates, install.metadata_section_updates - previous_install.metadata_section_updates, install.metadata_membership_removals - previous_install.metadata_membership_removals, install.metadata_full_cell_clears - previous_install.metadata_full_cell_clears, install.cell_membership_insertions - previous_install.cell_membership_insertions, install.ecs_bulk_create_components_usec - previous_install.ecs_bulk_create_components_usec, install.bulk_compact_usec - previous_install.bulk_compact_usec, install.ecs_final_parent_set_usec - previous_install.ecs_final_parent_set_usec, install.transform_finalize_usec - previous_install.transform_finalize_usec, install.initial_packet_prepare_usec - previous_install.initial_packet_prepare_usec, install.initial_packet_updates - previous_install.initial_packet_updates, install.initial_packet_table_rows - previous_install.initial_packet_table_rows, install.initial_packet_fallback_rows - previous_install.initial_packet_fallback_rows, error));
	}
	revision = previous_revision;
	cell_assets.erase(p_job.result.key);
	if (error != OK) {
		if (error != ERR_BUSY) {
			failed_cells.insert(p_job.result.key, revision);
		}
		return error;
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	for (EntityId ancestor : p_job.ancestors) {
		_pin_row(ancestor);
	}
	resident_cells.insert(p_job.result.key, p_job.ancestors);
	residency_serial++;
	r_stats.cells_committed++;
	r_stats.entities_committed += p_job.result.entities.size();
	if (timed) {
		r_profile->residency_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	return OK;
}

Error EntityScene::commit_ready(int p_max_entities, Stats *r_stats) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	catalog.maintenance();
	_collect_cleanup_jobs();
	if (cell_jobs.is_empty()) {
		return OK;
	}
	const bool profiling = OS::get_singleton()->is_use_benchmark_set();
	const uint64_t began = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	_collect_cell_jobs();
	Stats stats;
	OwnerProfile profile;
	Error result = OK;
	uint32_t index = 0;
	while (index < cell_jobs.size()) {
		CellJob *job = cell_jobs[index];
		if (job->awaiting_assets) {
			const Error asset_error = _poll_cell_assets(*job, !job->cancelled.is_set());
			if (asset_error == ERR_BUSY) {
				index++;
				continue;
			}
			if (job->cancelled.is_set() || asset_error != OK) {
				cell_jobs.remove_at(index);
				stats.jobs_discarded++;
				cell_assets.erase(job->result.key);
				if (asset_error != OK && result == OK) {
					failed_cells.insert(job->result.key, revision);
					result = _fail(job->result.failing, job->result.failing_field, asset_error);
				}
				memdelete(job);
				continue;
			}
			job->result.error = OK;
			const Error submit_error = EntityTaskScheduler::get_singleton()->submit(job, cell_mailbox, true);
			if (submit_error != OK) {
				cell_jobs.remove_at(index);
				stats.jobs_discarded++;
				cell_assets.erase(job->result.key);
				if (result == OK) {
					result = submit_error;
				}
				memdelete(job);
				continue;
			}
			index++;
			continue;
		}
		const bool budgeted = stats.cells_committed > 0 && p_max_entities > 0 && stats.entities_committed >= p_max_entities;
		if (!job->ready || (budgeted && !job->cancelled.is_set())) {
			index++;
			continue;
		}
		cell_jobs.remove_at(index);
		if (job->cancelled.is_set()) {
			stats.jobs_discarded++;
			cell_assets.erase(job->result.key);
		} else {
			stats.worker_total_usec += job->worker_total_usec;
			stats.enumerate_usec += job->enumerate_usec;
			stats.read_parse_usec += job->read_parse_usec;
			stats.read_parse_wall_usec += job->read_parse_wall_usec;
			stats.files_read += job->files_read;
			stats.read_ranges += job->read_range_count;
			stats.read_lanes += job->read_lanes;
			stats.parse_usec += job->parse_usec;
			stats.decode_usec += job->decode_usec;
			const Error error = _commit_cell(*job, stats, profiling ? &profile : nullptr);
			if (error != OK && result == OK) {
				result = error;
			}
			if (job->awaiting_assets) {
				cell_jobs.push_back(job);
				continue;
			}
			if (job->retry_owner) {
				job->retry_owner = false;
				cell_jobs.push_back(job);
				continue;
			}
			if (job->resume) {
				job->resume = false;
				job->result.error = OK;
				job->worker_total_usec = 0;
				job->enumerate_usec = 0;
				job->read_parse_usec = 0;
				job->read_parse_wall_usec = 0;
				job->files_read = 0;
				job->read_range_count = 0;
				job->read_lanes = 0;
				job->parse_usec = 0;
				job->decode_usec = 0;
				const Error submit_error = EntityTaskScheduler::get_singleton()->submit(job, cell_mailbox, true);
				if (submit_error == OK) {
					cell_jobs.push_back(job);
					continue;
				}
				if (result == OK) {
					result = submit_error;
				}
			}
			stats.jobs_completed++;
		}
		if (profiling) {
			uint64_t columns = 0;
			uint64_t constructed_rows = 0;
			uint64_t live_rows = 0;
			uint64_t moved_rows = 0;
			uint64_t bytes = 0;
			for (const PreparedGroup *group : job->result.groups) {
				columns += group->columns.size();
				constructed_rows += group->constructions.get();
				bytes += group->allocated_bytes;
				moved_rows += group->moved_rows;
				for (uint32_t count : group->constructed) {
					live_rows += count;
				}
			}
			stats.prepared_groups += job->result.groups.size();
			stats.prepared_columns += columns;
			stats.prepared_column_allocations += columns;
			stats.prepared_constructed_rows += constructed_rows;
			stats.peak_cell_prepared_column_bytes = MAX(stats.peak_cell_prepared_column_bytes, bytes);
			print_line(vformat("Entity cell prepared grid=%s cell=%d_%d_%d groups=%d columns=%d column_allocations=%d component_rows_constructed=%d component_rows_owned=%d moved_rows=%d peak_column_bytes=%d result=%d", job->result.key.grid, job->result.key.x, job->result.key.y, job->result.key.z, job->result.groups.size(), columns, columns, constructed_rows, live_rows, moved_rows, bytes, job->result.error));
		}
		_defer_job_cleanup(job);
	}
	if (profiling) {
		stats.owner_total_usec = OS::get_singleton()->get_ticks_usec() - began;
		stats.owner_asset_load_usec = profile.asset_load_usec;
		stats.owner_revalidate_usec = profile.revalidate_usec;
		stats.owner_commit_validate_usec = profile.commit_validate_usec;
		stats.owner_install_usec = profile.install_usec;
		stats.owner_residency_usec = profile.residency_usec;
		const uint64_t owner_classified = stats.owner_asset_load_usec + stats.owner_revalidate_usec + stats.owner_commit_validate_usec + stats.owner_install_usec + stats.owner_residency_usec;
		stats.owner_job_scan_cleanup_usec = stats.owner_total_usec > owner_classified ? stats.owner_total_usec - owner_classified : 0;
		stats.worker_remainder_usec = stats.worker_total_usec > stats.parse_usec + stats.decode_usec ? stats.worker_total_usec - stats.parse_usec - stats.decode_usec : 0;
		const CommitProfile &install = profile.install;
		stats.install_required_catalog_usec = install.required_catalog_usec;
		stats.install_ecs_parent_remove_usec = install.ecs_parent_remove_usec;
		stats.install_ecs_entity_destroy_usec = install.ecs_entity_destroy_usec;
		stats.install_ecs_entity_create_identity_usec = install.ecs_entity_create_identity_usec;
		stats.install_ecs_materialize_parent_set_usec = install.ecs_materialize_parent_set_usec;
		stats.install_resident_remove_usec = install.resident_remove_usec;
		stats.install_resident_insert_usec = install.resident_insert_usec;
		stats.install_initial_dirty_usec = install.initial_dirty_usec;
		stats.install_prepared_schema_lookup_usec = install.prepared_schema_lookup_usec;
		stats.install_component_mutation_usec = install.component_mutation_usec;
		stats.install_component_changed_usec = install.component_changed_usec;
		stats.install_change_bookkeeping_usec = install.change_bookkeeping_usec;
		stats.install_sections_order_usec = install.sections_order_usec;
		stats.install_catalog_parent_usec = install.catalog_parent_usec;
		stats.install_ecs_final_parent_set_usec = install.ecs_final_parent_set_usec;
		stats.install_assign_cell_usec = install.assign_cell_usec;
		stats.install_metadata_commit_usec = install.metadata_commit_usec;
		stats.install_ecs_bulk_create_components_usec = install.ecs_bulk_create_components_usec;
		stats.install_bulk_compact_usec = install.bulk_compact_usec;
		stats.install_transform_finalize_usec = install.transform_finalize_usec;
		stats.install_initial_packet_prepare_usec = install.initial_packet_prepare_usec;
		stats.bulk_groups = install.bulk_groups;
		stats.bulk_rows = install.bulk_rows;
		stats.compacted_rows = install.compacted_rows;
		stats.skipped_rows = install.skipped_rows;
		const uint64_t install_classified = stats.install_required_catalog_usec + stats.install_ecs_parent_remove_usec + stats.install_ecs_entity_destroy_usec + stats.install_ecs_entity_create_identity_usec + stats.install_ecs_materialize_parent_set_usec + stats.install_resident_remove_usec + stats.install_resident_insert_usec + stats.install_initial_dirty_usec + stats.install_prepared_schema_lookup_usec + stats.install_component_mutation_usec + stats.install_component_changed_usec + stats.install_change_bookkeeping_usec + stats.install_sections_order_usec + stats.install_catalog_parent_usec + stats.install_ecs_final_parent_set_usec + stats.install_assign_cell_usec + stats.install_metadata_commit_usec + stats.install_ecs_bulk_create_components_usec + stats.install_bulk_compact_usec + stats.install_transform_finalize_usec + stats.install_initial_packet_prepare_usec;
		stats.install_remainder_usec = stats.owner_install_usec > install_classified ? stats.owner_install_usec - install_classified : 0;
		stats.entities_materialized = install.entities_materialized;
		stats.entities_destroyed = install.entities_destroyed;
		stats.parent_removals = install.parent_removals;
		stats.materialize_parent_sets = install.materialize_parent_sets;
		stats.component_mutations = install.component_mutations;
		stats.final_parent_sets = install.final_parent_sets;
		stats.initial_packet_updates = install.initial_packet_updates;
		stats.initial_packet_table_rows = install.initial_packet_table_rows;
		stats.initial_packet_fallback_rows = install.initial_packet_fallback_rows;
	}
	if (r_stats) {
		r_stats->jobs_completed += stats.jobs_completed;
		r_stats->jobs_discarded += stats.jobs_discarded;
		r_stats->jobs_resumed += stats.jobs_resumed;
		r_stats->cells_committed += stats.cells_committed;
		r_stats->entities_committed += stats.entities_committed;
		r_stats->owner_total_usec += stats.owner_total_usec;
		r_stats->owner_asset_load_usec += stats.owner_asset_load_usec;
		r_stats->owner_revalidate_usec += stats.owner_revalidate_usec;
		r_stats->owner_commit_validate_usec += stats.owner_commit_validate_usec;
		r_stats->owner_install_usec += stats.owner_install_usec;
		r_stats->owner_residency_usec += stats.owner_residency_usec;
		r_stats->owner_job_scan_cleanup_usec += stats.owner_job_scan_cleanup_usec;
		r_stats->worker_total_usec += stats.worker_total_usec;
		r_stats->enumerate_usec += stats.enumerate_usec;
		r_stats->read_parse_usec += stats.read_parse_usec;
		r_stats->read_parse_wall_usec += stats.read_parse_wall_usec;
		r_stats->files_read += stats.files_read;
		r_stats->read_ranges += stats.read_ranges;
		r_stats->read_lanes += stats.read_lanes;
		r_stats->parse_usec += stats.parse_usec;
		r_stats->decode_usec += stats.decode_usec;
		r_stats->prepared_groups += stats.prepared_groups;
		r_stats->prepared_columns += stats.prepared_columns;
		r_stats->prepared_column_allocations += stats.prepared_column_allocations;
		r_stats->prepared_constructed_rows += stats.prepared_constructed_rows;
		r_stats->peak_cell_prepared_column_bytes = MAX(r_stats->peak_cell_prepared_column_bytes, stats.peak_cell_prepared_column_bytes);
		r_stats->worker_remainder_usec += stats.worker_remainder_usec;
		r_stats->install_required_catalog_usec += stats.install_required_catalog_usec;
		r_stats->install_ecs_parent_remove_usec += stats.install_ecs_parent_remove_usec;
		r_stats->install_ecs_entity_destroy_usec += stats.install_ecs_entity_destroy_usec;
		r_stats->install_ecs_entity_create_identity_usec += stats.install_ecs_entity_create_identity_usec;
		r_stats->install_ecs_materialize_parent_set_usec += stats.install_ecs_materialize_parent_set_usec;
		r_stats->install_resident_remove_usec += stats.install_resident_remove_usec;
		r_stats->install_resident_insert_usec += stats.install_resident_insert_usec;
		r_stats->install_initial_dirty_usec += stats.install_initial_dirty_usec;
		r_stats->install_prepared_schema_lookup_usec += stats.install_prepared_schema_lookup_usec;
		r_stats->install_component_mutation_usec += stats.install_component_mutation_usec;
		r_stats->install_component_changed_usec += stats.install_component_changed_usec;
		r_stats->install_change_bookkeeping_usec += stats.install_change_bookkeeping_usec;
		r_stats->install_sections_order_usec += stats.install_sections_order_usec;
		r_stats->install_catalog_parent_usec += stats.install_catalog_parent_usec;
		r_stats->install_ecs_final_parent_set_usec += stats.install_ecs_final_parent_set_usec;
		r_stats->install_assign_cell_usec += stats.install_assign_cell_usec;
		r_stats->install_metadata_commit_usec += stats.install_metadata_commit_usec;
		r_stats->install_remainder_usec += stats.install_remainder_usec;
		r_stats->install_ecs_bulk_create_components_usec += stats.install_ecs_bulk_create_components_usec;
		r_stats->install_bulk_compact_usec += stats.install_bulk_compact_usec;
		r_stats->install_transform_finalize_usec += stats.install_transform_finalize_usec;
		r_stats->install_initial_packet_prepare_usec += stats.install_initial_packet_prepare_usec;
		r_stats->bulk_groups += stats.bulk_groups;
		r_stats->bulk_rows += stats.bulk_rows;
		r_stats->compacted_rows += stats.compacted_rows;
		r_stats->skipped_rows += stats.skipped_rows;
		r_stats->entities_materialized += stats.entities_materialized;
		r_stats->entities_destroyed += stats.entities_destroyed;
		r_stats->parent_removals += stats.parent_removals;
		r_stats->materialize_parent_sets += stats.materialize_parent_sets;
		r_stats->component_mutations += stats.component_mutations;
		r_stats->final_parent_sets += stats.final_parent_sets;
		r_stats->initial_packet_updates += stats.initial_packet_updates;
		r_stats->initial_packet_table_rows += stats.initial_packet_table_rows;
		r_stats->initial_packet_fallback_rows += stats.initial_packet_fallback_rows;
	}
	catalog.maintenance();
	return result;
}

void EntityScene::flush_streaming() {
	if (cell_jobs.is_empty()) {
		_flush_cleanup_jobs();
		catalog.flush_maintenance();
		return;
	}
	for (CellJob *job : cell_jobs) {
		job->cancelled.set();
	}
	CallQueue *queue = MessageQueue::get_main_singleton();
	const bool pump = queue && !queue->is_flushing();
	for (CellJob *job : cell_jobs) {
		const uint64_t began = OS::get_singleton()->get_ticks_usec();
		bool reported = false;
		_collect_cell_jobs();
		while (!job->ready || job->awaiting_assets) {
			if (job->awaiting_assets) {
				const Error asset_error = _poll_cell_assets(*job, false);
				if (asset_error != ERR_BUSY) {
					job->ready = true;
				}
			}
			if (pump) {
				queue->flush();
				if (RenderingServer::get_singleton()) {
					RenderingServer::get_singleton()->sync();
				}
			}
			OS::get_singleton()->delay_usec(1000);
			_collect_cell_jobs();
			if (!reported && OS::get_singleton()->get_ticks_usec() - began > 10000000) {
				reported = true;
				ERR_PRINT("Entity cell load did not stop after cancellation: " + job->result.key.grid + " " + itos(job->result.key.x) + "_" + itos(job->result.key.y) + "_" + itos(job->result.key.z));
			}
		}
		memdelete(job);
	}
	cell_jobs.clear();
	cell_assets.clear();
	_flush_cleanup_jobs();
	catalog.flush_maintenance();
}

Error EntityScene::request_cells(const Vector<CellKey> &p_cells, int *r_remaining, Stats *r_stats) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	catalog.maintenance();
	const bool profiling = r_stats && OS::get_singleton()->is_use_benchmark_set();
	const uint64_t began = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	int remaining = 0;
	int dispatched = 0;
	Error result = OK;
	HashSet<CellKey, CellKeyHasher> wanted;
	for (const CellKey &cell : p_cells) {
		wanted.insert(cell);
		if (resident_cells.has(cell)) {
			continue;
		}
		const uint64_t *failed = failed_cells.getptr(cell);
		if (failed) {
			if (*failed == revision) {
				continue;
			}
			failed_cells.erase(cell);
		}
		if (!_is_cell_in_flight(cell) && cell_jobs.size() < _max_cell_jobs()) {
			const uint32_t before = cell_jobs.size();
			const Error error = _dispatch_cell(cell);
			if (error == ERR_BUSY) {
				remaining++;
				continue;
			}
			if (error != OK) {
				failed_cells.insert(cell, revision);
				if (result == OK) {
					result = error;
				}
				continue;
			}
			dispatched += cell_jobs.size() > before ? 1 : 0;
		}
		if (!resident_cells.has(cell)) {
			remaining++;
		}
	}
	Vector<CellKey> stale_assets;
	for (const KeyValue<CellKey, LocalVector<Ref<Resource>>> &entry : cell_assets) {
		if (!resident_cells.has(entry.key) && !wanted.has(entry.key) && !_is_cell_in_flight(entry.key)) {
			stale_assets.push_back(entry.key);
		}
	}
	for (const CellKey &cell : stale_assets) {
		cell_assets.erase(cell);
	}
	if (r_remaining) {
		*r_remaining = remaining;
	}
	if (r_stats) {
		r_stats->jobs_dispatched += dispatched;
		if (profiling) {
			r_stats->dispatch_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
	}
	return result;
}

Error EntityScene::release_cells(const Vector<CellKey> &p_cells) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	const bool profiling = OS::get_singleton()->is_use_benchmark_set();
	const uint64_t began = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	uint64_t unload_usec = 0;
	uint64_t retire_usec = 0;
	uint32_t unloaded_count = 0;
	uint32_t retired_count = 0;
	Error result = OK;
	bool released = false;
	for (const CellKey &cell : p_cells) {
		for (CellJob *job : cell_jobs) {
			if (job->result.key == cell) {
				job->cancelled.set();
			}
		}
		const Vector<EntityId> *pinned = resident_cells.getptr(cell);
		if (!pinned) {
			cell_assets.erase(cell);
			continue;
		}
		const Vector<EntityId> ancestors = *pinned;
		const CellMembers *stored_members = cells.getptr(cell);
		EntityCatalog::ReleasePlan plan;
		if (stored_members) {
			const Error plan_error = catalog.build_release_plan(*stored_members, plan);
			if (plan_error != OK) {
				result = plan_error;
				continue;
			}
		}
		if (!plan.unloading.is_empty()) {
			const uint64_t unload_began = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
			const Error error = _unload_subset(plan.unloading, true);
			if (profiling) {
				unload_usec += OS::get_singleton()->get_ticks_usec() - unload_began;
			}
			if (error != OK) {
				result = error;
				continue;
			}
			unloaded_count += plan.unloading.size();
		}
		resident_cells.erase(cell);
		unpin(ancestors);
		cell_assets.erase(cell);
		released = true;
		if (!plan.retiring.is_empty()) {
			const uint64_t retire_began = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
			const Error retire_error = catalog.retire_rows(plan.retiring, world);
			if (profiling) {
				retire_usec += OS::get_singleton()->get_ticks_usec() - retire_began;
			}
			if (retire_error != OK) {
				result = retire_error;
				continue;
			}
			retired_count += plan.retiring.size();
			CellMembers *remaining = cells.getptr(cell);
			if (remaining) {
				int write = 0;
				int retired = 0;
				for (EntityId id : *remaining) {
					while (retired < plan.retiring_ids.size() && SnapshotIdOrder()(plan.retiring_ids[retired], id)) {
						retired++;
					}
					if (retired >= plan.retiring_ids.size() || plan.retiring_ids[retired] != id) {
						remaining->write[write++] = id;
					}
				}
				remaining->resize(write);
				if (remaining->is_empty()) {
					cells.erase(cell);
				}
			}
		}
	}
	if (released) {
		residency_serial++;
	}
	catalog.maintenance();
	if (profiling) {
		print_line(vformat("EntityScene release_cells cells=%d unloaded=%d retired=%d unload_ms=%.2f retire_ms=%.2f total_ms=%.2f locator_runs=%d child_runs=%d", p_cells.size(), unloaded_count, retired_count, unload_usec / 1000.0, retire_usec / 1000.0, (OS::get_singleton()->get_ticks_usec() - began) / 1000.0, catalog.get_locator_run_count(), catalog.get_child_run_count()));
	}
	return result;
}

Vector<EntityScene::CellKey> EntityScene::get_resident_cells() const {
	Vector<CellKey> result;
	result.reserve(resident_cells.size());
	for (const KeyValue<CellKey, Vector<EntityId>> &entry : resident_cells) {
		result.push_back(entry.key);
	}
	return result;
}

EntityScene::CellKey EntityScene::cell_for_position(const String &p_grid, const Vector3 &p_position) const {
	CellKey key;
	const String grid_name = p_grid.is_empty() ? default_grid : p_grid;
	const Grid *grid = grids.getptr(grid_name);
	ERR_FAIL_NULL_V_MSG(grid, key, "Entity scene has no grid named \"" + grid_name + "\".");
	ERR_FAIL_COND_V(grid->size <= 0.0, key);
	key.grid = grid_name;
	key.x = _cell_index(double(p_position.x), grid->size);
	key.y = _cell_index(double(p_position.y), grid->size);
	key.z = _cell_index(double(p_position.z), grid->size);
	return key;
}

AABB EntityScene::cell_aabb(const CellKey &p_cell) const {
	const Grid *grid = grids.getptr(p_cell.grid);
	ERR_FAIL_NULL_V_MSG(grid, AABB(), "Entity scene has no grid named \"" + p_cell.grid + "\".");
	const double size = grid->size;
	return AABB(Vector3(double(p_cell.x) * size, double(p_cell.y) * size, double(p_cell.z) * size), Vector3(size, size, size));
}

Vector<String> EntityScene::get_grid_names() const {
	Vector<String> result;
	result.reserve(grids.size());
	for (const KeyValue<String, Grid> &entry : grids) {
		result.push_back(entry.key);
	}
	return result;
}

double EntityScene::get_grid_size(const String &p_grid) const {
	const Grid *grid = grids.getptr(p_grid.is_empty() ? default_grid : p_grid);
	return grid ? grid->size : 0.0;
}

double EntityScene::get_grid_range(const String &p_grid) const {
	const Grid *grid = grids.getptr(p_grid.is_empty() ? default_grid : p_grid);
	if (!grid) {
		return 0.0;
	}
	return grid->range > 0.0 ? grid->range : default_range;
}

String EntityScene::get_entity_name(EntityId p_id) const {
	const Section *section = _get_section(p_id);
	return section ? section->name : String();
}

#endif
