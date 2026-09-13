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
	const int64_t *value = order.getptr(p_id);
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
	const Section *section = sections.getptr(p_id);
	if (section && !section->path.is_empty()) {
		r_source = section->path;
		return section->path.get_base_dir();
	}
	const PrefabMember *member = prefab_members.getptr(p_id);
	if (!member) {
		return String();
	}
	const Variant value = prefab_instances.get(member->instance, Variant());
	if (value.get_type() != Variant::DICTIONARY) {
		return String();
	}
	const Dictionary instance = value;
	if (!instance.has("cell")) {
		return String();
	}
	r_source = String("prefabs").path_join(member->instance + ".escn");
	const Variant cell = instance["cell"];
	return cell.get_type() == Variant::STRING ? String(cell) : String();
}

void EntityScene::_forget_cell(EntityId p_id) {
	globals.erase(p_id);
	EntityCatalog::Record *record = catalog.records.getptr(p_id);
	if (!record || !record->has_cell) {
		return;
	}
	const CellKey key{ record->cell_grid, record->cell_x, record->cell_y, record->cell_z };
	record->cell_grid = String();
	record->has_cell = false;
	CellMembers *members = cells.getptr(key);
	if (members) {
		members->erase(p_id);
		if (members->is_empty()) {
			cells.erase(key);
		}
	}
}

Error EntityScene::_assign_cell(EntityId p_id) {
	_forget_cell(p_id);
	if (catalog.get_state(p_id) != EntityReferenceState::UNLOADED) {
		return OK;
	}
	String source;
	const String directory = _storage_directory(p_id, source);
	if (directory.is_empty()) {
		return source.is_empty() ? OK : _fail(p_id, "cell/" + source, ERR_INVALID_DATA);
	}
	if (directory == "global") {
		globals.insert(p_id);
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
	cells[assigned_key].insert(p_id, true);
	EntityCatalog::Record &record = catalog.records[p_id];
	record.cell_grid = assigned_key.grid;
	record.cell_x = assigned_key.x;
	record.cell_y = assigned_key.y;
	record.cell_z = assigned_key.z;
	record.has_cell = true;
	return OK;
}

Error EntityScene::_assign_cells(const Vector<EntityId> &p_ids) {
	Error result = OK;
	for (EntityId id : p_ids) {
		const Error error = _assign_cell(id);
		if (error != OK && result == OK) {
			result = error;
		}
	}
	return result;
}

void EntityScene::_forget_entity(EntityId p_id) {
	_forget_cell(p_id);
	sections.erase(p_id);
	order.erase(p_id);
	catalog._unlink_parent(p_id);
	catalog.records.erase(p_id);
	catalog.children.erase(p_id);
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

void EntityScene::_index_prefabs() {
	prefab_members.clear();
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
			if (entries[source].get_type() == Variant::STRING && EntityId::parse(entries[source], id) == OK && id.is_valid() && !prefab_members.has(id)) {
				prefab_members.insert(id, { key, source });
			}
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
			constructions++;
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
			constructions++;
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
	row = p_other.row;
	owns_group = p_other.owns_group;
	p_other.group = nullptr;
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
	owns_group = false;
}

EntityScene::PreparedSet::PreparedSet(const LocalVector<PreparedEntity> &p_entities, const LocalVector<PreparedGroup *> *p_bulk_groups) :
		entities(&p_entities), bulk_groups(p_bulk_groups) {
	lookup.reserve(p_entities.size());
	for (uint32_t i = 0; i < p_entities.size(); i++) {
		lookup.insert(p_entities[i].id, i);
	}
}

const EntityScene::PreparedEntity *EntityScene::PreparedSet::find(EntityId p_id) const {
	if (!entities) {
		return nullptr;
	}
	const uint32_t *index = lookup.getptr(p_id);
	return index ? &(*entities)[*index] : nullptr;
}

bool EntityScene::PreparedSet::is_deleted(EntityId p_id) const {
	if (scene) {
		return scene->catalog.records[p_id].deleted;
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
	HashSet<EntityId, EntityIdHasher> seen;
	seen.reserve(p_ids.size());
	r_ids.reserve(r_ids.size() + p_ids.size());
	LocalVector<EntityId> ancestors;
	HashSet<EntityId, EntityIdHasher> chain;
	for (EntityId id : p_ids) {
		ancestors.clear();
		chain.clear();
		while (id.is_valid() && !seen.has(id)) {
			const PreparedEntity *entry = find(id);
			if (!entry) {
				return ERR_DOES_NOT_EXIST;
			}
			if (chain.has(id)) {
				return ERR_CYCLIC_LINK;
			}
			chain.insert(id);
			ancestors.push_back(id);
			id = entry->deleted ? EntityId() : entry->parent.id;
		}
		for (uint32_t i = ancestors.size(); i > 0; i--) {
			seen.insert(ancestors[i - 1]);
			r_ids.push_back(ancestors[i - 1]);
		}
	}
	return OK;
}

EntityScene::SectionAction EntityScene::PreparedSet::build_section(EntityId p_id, Section &r_section) const {
	if (scene) {
		const Section *section = scene->sections.getptr(p_id);
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
		const EntityCatalog::Record *record = scene->catalog.records.getptr(p_id);
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
	HashMap<PreparedGroup *, uint32_t> group_indices;
	LocalVector<LocalVector<const PreparedEntity *>> rows;
	rows.resize(bulk_groups->size());
	group_indices.reserve(bulk_groups->size());
	for (uint32_t i = 0; i < bulk_groups->size(); i++) {
		PreparedGroup *group = (*bulk_groups)[i];
		group_indices.insert(group, i);
		rows[i].resize(group->capacity);
		for (const PreparedEntity *&entry : rows[i]) {
			entry = nullptr;
		}
	}
	for (const PreparedEntity &entry : *entities) {
		if (entry.group && !entry.live && !entry.deleted) {
			rows[group_indices[entry.group]][entry.row] = &entry;
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
		const Error error = p_target._materialize_bulk(ids, desc, table, storage_tag, r_profile ? &materialize_profile : nullptr);
		if (error != OK) {
			return error;
		}
		group.moved_rows = ids.size();
		EntityInitialRenderGroup initial;
		initial.table = table;
		initial.storage_tag = storage_tag;
		initial.entities.reserve(ids.size());
		for (EntityId id : ids) {
			const EntityWorld::Resident *resident = p_target.residents.getptr(id);
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
	const Section *section = sections.getptr(p_id);
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
		const Section *section = sections.getptr(p_id);
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
	sections.insert(p_id, section);
	return OK;
}

Error EntityScene::_collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const {
	HashSet<EntityId, EntityIdHasher> seen;
	for (EntityId id : p_ids) {
		Vector<EntityId> ancestors;
		HashSet<EntityId, EntityIdHasher> chain;
		while (id.is_valid() && !seen.has(id)) {
			if (chain.has(id)) {
				return ERR_CYCLIC_LINK;
			}
			EntityReferenceState state = catalog.get_state(id);
			if (state == EntityReferenceState::MISSING) {
				return ERR_DOES_NOT_EXIST;
			}
			chain.insert(id);
			ancestors.push_back(id);
			id = state == EntityReferenceState::DELETED ? EntityId() : catalog.get_parent(id).id;
		}
		for (int i = ancestors.size() - 1; i >= 0; i--) {
			seen.insert(ancestors[i]);
			r_ids.push_back(ancestors[i]);
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
		prepared->catalog.records.insert(id, catalog.records[id]);
		prepared->order.insert(id, get_order(id));
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
		prepared.deleted = catalog.records[id].deleted;
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
		const EntityCatalog::Record *previous = catalog.records.getptr(id);
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
		item.had_global = globals.has(id);
		const int64_t *previous_order = order.getptr(id);
		item.had_order = previous_order != nullptr;
		item.order_changed = previous_order ? *previous_order != item.order : item.order != 0;
		const Section *previous_section = sections.getptr(id);
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
		const EntityCatalog::Record *parent_record = p_prepared.scene ? p_prepared.scene->catalog.records.getptr(parent) : nullptr;
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
		for (const KeyValue<EntityId, EntityCatalog::Record> &entry : p_prepared.scene->catalog.records) {
			const Error error = validate_parent(entry.key);
			if (error != OK) {
				return error;
			}
		}
	}
	HashSet<EntityId, EntityIdHasher> affected;
	for (EntityId id : p_ids) {
		affected.insert(id);
	}
	for (const CommitItem &item : r_items) {
		if (item.deleted) {
			if (pins.has(item.id)) {
				return _fail(item.id, "record", ERR_BUSY);
			}
			for (EntityId child : catalog.get_children(item.id)) {
				if (!affected.has(child) || (!p_prepared.is_deleted(child) && p_prepared.get_parent(child).id == item.id)) {
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

Error EntityScene::_commit(const PreparedSet &p_prepared, LocalVector<CommitItem> &p_items, bool p_resident, bool p_dirty, CommitProfile *r_profile, const CellKey *p_streamed_cell, const HashSet<EntityId, EntityIdHasher> *p_streamed_members) {
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
	HashSet<EntityId, EntityIdHasher> residency;
	HashSet<EntityId, EntityIdHasher> item_ids;
	for (const CommitItem &item : p_items) {
		item_ids.insert(item.id);
	}
	for (EntityId id : required) {
		if (p_prepared.is_deleted(id)) {
			return _fail(id, "required ancestor", ERR_DOES_NOT_EXIST);
		}
		residency.insert(id);
		if (!item_ids.has(id) && resolve(id).state != EntityReferenceState::RESIDENT) {
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
	if (bulk) {
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		target->transforms.update();
		if (timed) {
			profile.transform_finalize_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	uint32_t new_records = 0;
	uint32_t new_orders = 0;
	uint32_t new_sections = 0;
	uint32_t new_deleted_storage = 0;
	uint32_t new_dirty = 0;
	for (const CommitItem &item : p_items) {
		new_records += !item.had_record;
		new_orders += item.order_changed && !item.had_order;
		new_sections += item.section_changed && item.section_action == SECTION_SET && !item.had_section;
		new_deleted_storage += item.section_changed && item.section_action == SECTION_ERASE && item.deleted && item.had_section && !deleted_storage.has(item.id);
		new_dirty += p_dirty && !dirty.has(item.id);
	}
	if (new_records) {
		catalog.records.reserve(catalog.records.size() + new_records);
	}
	if (timed) {
		profile.required_catalog_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	if (new_orders) {
		order.reserve(order.size() + new_orders);
	}
	if (new_sections) {
		sections.reserve(sections.size() + new_sections);
	}
	if (new_deleted_storage) {
		deleted_storage.reserve(deleted_storage.size() + new_deleted_storage);
	}
	if (new_dirty) {
		dirty.reserve(dirty.size() + new_dirty);
	}
	if (timed) {
		profile.sections_order_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	HashMap<EntityId, uint32_t, EntityIdHasher> child_additions;
	for (const CommitItem &item : p_items) {
		if (item.parent_index_changed && !item.deleted && item.parent.id.is_valid()) {
			child_additions[item.parent.id]++;
		}
	}
	for (const KeyValue<EntityId, uint32_t> &entry : child_additions) {
		HashSet<EntityId, EntityIdHasher> &children = catalog.children[entry.key];
		children.reserve(children.size() + entry.value);
	}
	for (CommitItem &item : p_items) {
		item.install = !item.deleted && residency.has(item.id);
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	HashSet<EntityId, EntityIdHasher> touched_parents;
	touched_parents.reserve(child_additions.size());
	for (CommitItem &item : p_items) {
		if (item.parent_index_changed && item.had_record && !item.previous_deleted && item.previous_parent.id.is_valid()) {
			HashSet<EntityId, EntityIdHasher> *siblings = catalog.children.getptr(item.previous_parent.id);
			if (siblings) {
				siblings->erase(item.id);
			}
			touched_parents.insert(item.previous_parent.id);
		}
		if (!item.had_record) {
			EntityCatalog::Record &record = catalog.records[item.id];
			record.deleted = item.deleted;
			record.parent = item.parent;
			if (item.order_changed) {
				record.order = item.order;
				record.has_order = true;
				if (timed) {
					profile.metadata_order_updates++;
				}
			}
			if (item.section_changed && item.section_action == SECTION_SET) {
				record.section = std::move(item.section);
				record.has_section = true;
				if (timed) {
					profile.metadata_section_updates++;
				}
			}
			if (timed) {
				profile.metadata_new_records++;
			}
		} else if (item.previous_deleted != item.deleted || item.parent_changed) {
			EntityCatalog::Record *record = catalog.records.getptr(item.id);
			record->deleted = item.deleted;
			record->parent = item.parent;
			if (timed) {
				profile.metadata_record_updates++;
			}
		}
		if (item.parent_index_changed && !item.deleted && item.parent.id.is_valid()) {
			catalog.children[item.parent.id].insert(item.id);
		}
		if (item.had_record && item.order_changed) {
			order.insert(item.id, item.order);
			if (timed) {
				profile.metadata_order_updates++;
			}
		}
		if (item.had_record && item.section_changed && item.section_action == SECTION_SET) {
			sections[item.id] = std::move(item.section);
			if (timed) {
				profile.metadata_section_updates++;
			}
		} else if (item.had_record && item.section_changed && item.section_action == SECTION_ERASE) {
			const Section *previous = sections.getptr(item.id);
			if (item.deleted && previous && !previous->path.is_empty()) {
				Section storage;
				storage.path = previous->path;
				storage.cluster = previous->cluster;
				deleted_storage[item.id] = std::move(storage);
			}
			sections.erase(item.id);
			if (timed) {
				profile.metadata_section_updates++;
			}
		}
		if (p_dirty) {
			dirty.insert(item.id);
		}
	}
	for (EntityId parent : touched_parents) {
		const HashSet<EntityId, EntityIdHasher> *children = catalog.children.getptr(parent);
		if (children && children->is_empty()) {
			catalog.children.erase(parent);
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
			target->residents.erase(item.id);
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
		target->residents.reserve(target->residents.size() + active.size());
		if (timed) {
			profile.resident_insert_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
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
		const EntityWorld::Resident *resident = target->residents.getptr(item.id);
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
				const EntityWorld::Resident *resident = target->residents.getptr(id);
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
		target->changed.reserve(target->changed.size() + active.size());
		const uint64_t change_serial = ++target->change_serial;
		for (const EntityInitialRenderGroup &group : initial) {
			for (const EntityInitialRender &created : group.entities) {
				EntityWorld::Resident *resident = target->residents.getptr(created.id);
				if (resident) {
					resident->revision = change_serial;
				}
				target->changed.insert(created.id, true);
			}
		}
		if (timed) {
			profile.change_bookkeeping_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	if (bulk && p_resident && p_streamed_cell) {
		HashSet<CellKey, CellKeyHasher> touched_cells;
		touched_cells.reserve(MIN(uint32_t(p_items.size()), uint32_t(cells.size())));
		for (const CommitItem &item : p_items) {
			if (item.had_global) {
				globals.erase(item.id);
			}
			if (!item.had_cell) {
				continue;
			}
			const CellKey key{ item.cell_grid, item.cell_x, item.cell_y, item.cell_z };
			if (!item.install || key != *p_streamed_cell) {
				CellMembers *members = cells.getptr(key);
				if (members) {
					members->erase(item.id);
				}
				touched_cells.insert(key);
				EntityCatalog::Record *record = catalog.records.getptr(item.id);
				if (record) {
					record->cell_grid = String();
					record->has_cell = false;
				}
				if (timed) {
					profile.metadata_membership_removals++;
				}
			}
		}
		for (const CellKey &cell : touched_cells) {
			const CellMembers *members = cells.getptr(cell);
			if (members && members->is_empty()) {
				cells.erase(cell);
			}
		}
		uint32_t member_count = 0;
		for (EntityId id : *p_streamed_members) {
			const PreparedEntity *entity = p_prepared.find(id);
			member_count += entity ? !entity->deleted : catalog.get_state(id) != EntityReferenceState::DELETED && !deleted_storage.has(id);
		}
		CellMembers &members = cells[*p_streamed_cell];
		members.reserve(members.size() + member_count);
		for (EntityId id : *p_streamed_members) {
			const PreparedEntity *entity = p_prepared.find(id);
			if (entity ? entity->deleted : catalog.get_state(id) == EntityReferenceState::DELETED || deleted_storage.has(id)) {
				continue;
			}
			EntityCatalog::Record *record = catalog.records.getptr(id);
			DEV_ASSERT(record);
			if (!record) {
				continue;
			}
			const CellKey previous_key{ record->cell_grid, record->cell_x, record->cell_y, record->cell_z };
			if (record->has_cell && previous_key != *p_streamed_cell) {
				CellMembers *previous_members = cells.getptr(previous_key);
				if (previous_members) {
					previous_members->erase(id);
					if (previous_members->is_empty()) {
						cells.erase(previous_key);
					}
				}
				if (timed) {
					profile.metadata_membership_removals++;
				}
			}
			globals.erase(id);
			members.insert(id, true);
			record->cell_grid = p_streamed_cell->grid;
			record->cell_x = p_streamed_cell->x;
			record->cell_y = p_streamed_cell->y;
			record->cell_z = p_streamed_cell->z;
			record->has_cell = true;
			if (timed) {
				profile.cell_membership_insertions++;
			}
		}
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
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	HashSet<EntityId, EntityIdHasher> requested;
	HashMap<EntityId, Section, EntityIdHasher> saved;
	for (EntityId id : p_ids) {
		requested.insert(id);
		if (pins.has(id) || dirty.has(id)) {
			return ERR_BUSY;
		}
		if (resolve(id).state != EntityReferenceState::RESIDENT) {
			return ERR_UNAVAILABLE;
		}
		const Section *stored = sections.getptr(id);
		if (stored && !stored->path.is_empty()) {
			Section section = *stored;
			section.record = Dictionary();
			saved.insert(id, section);
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
		saved.insert(id, section);
	}
	for (EntityId id : p_ids) {
		for (EntityId child : catalog.get_children(id)) {
			if (resolve(child).state == EntityReferenceState::RESIDENT && !requested.has(child)) {
				return ERR_BUSY;
			}
		}
	}
	Vector<EntityId> ordered;
	Error error = _collect_required(p_ids, ordered);
	if (error != OK) {
		return error;
	}
	for (int i = ordered.size() - 1; i >= 0; i--) {
		EntityId id = ordered[i];
		if (requested.has(id)) {
			sections.insert(id, saved[id]);
			world->unload_entity(resolve(id).handle);
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
		pins[id]++;
	}
	return OK;
}

void EntityScene::unpin(const Vector<EntityId> &p_ids) {
	ERR_FAIL_COND(_owner() != OK);
	for (EntityId id : p_ids) {
		uint32_t *count = pins.getptr(id);
		if (count && --(*count) == 0) {
			pins.erase(id);
		}
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
	result->dirty = dirty;
	for (EntityId id : catalog.get_ids()) {
		Dictionary record;
		Error error = _read_record(id, record);
		if (error != OK) {
			return error;
		}
		result->catalog.records.insert(id, catalog.records[id]);
		result->order.insert(id, get_order(id));
		if (!catalog.records[id].deleted) {
			result->catalog._set_parent(id, catalog.get_parent(id));
			Section section;
			error = _describe(id, record, section);
			if (error != OK) {
				return error;
			}
			section.record = record;
			const Section *source = sections.getptr(id);
			if (source) {
				section.path = source->path;
				section.cluster = source->cluster;
			}
			result->sections.insert(id, section);
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
	for (EntityId id : globals) {
		if (catalog.get_state(id) == EntityReferenceState::UNLOADED) {
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

Error EntityScene::_dispatch_cell(const CellKey &p_cell) {
	Vector<EntityId> known;
	CellJob *job = memnew(CellJob);
	job->profile = OS::get_singleton()->is_use_benchmark_set();
	job->result.key = p_cell;
	job->storage_directory = EntitySceneIO::scene_directory(storage_path).simplify_path();
	const CellMembers *members = cells.getptr(p_cell);
	if (members) {
		for (const KeyValue<EntityId, bool> &member : *members) {
			const EntityId id = member.key;
			job->members.insert(id);
			if (resolve(id).state == EntityReferenceState::RESIDENT || dirty.has(id)) {
				job->skip.insert(id);
			} else {
				known.push_back(id);
			}
		}
	}
	if (!known.is_empty()) {
		HashSet<EntityId, EntityIdHasher> inside;
		inside.reserve(members->size());
		for (const KeyValue<EntityId, bool> &member : *members) {
			inside.insert(member.key);
		}
		Vector<EntityId> required;
		const Error error = _collect_required(known, required);
		if (error != OK) {
			memdelete(job);
			return error;
		}
		for (EntityId id : required) {
			job->required.insert(id);
			if (!inside.has(id)) {
				job->ancestors.push_back(id);
			}
			if (resolve(id).state == EntityReferenceState::RESIDENT || dirty.has(id)) {
				job->skip.insert(id);
				continue;
			}
			const Section *section = sections.getptr(id);
			const bool prefab = prefab_members.has(id);
			if (catalog.get_state(id) == EntityReferenceState::DELETED || (!prefab && (!section || (section->record.is_empty() && (section->path.is_empty() || storage_path.is_empty()))))) {
				memdelete(job);
				return _fail(id, "record", ERR_DOES_NOT_EXIST);
			}
			const String path = section ? section->path.simplify_path() : String();
			if ((section && !section->path.is_empty() && path.is_empty()) || path.is_absolute_path() || path.contains(":") || path == ".." || path.begins_with("../")) {
				memdelete(job);
				return _fail(id, "record path", ERR_INVALID_DATA);
			}
			const bool cluster = section && section->cluster;
			if (prefab || !section->record.is_empty()) {
				if (section && !section->record.is_empty()) {
					const Error reserve_error = EntitySceneIO::reserve_record(section->record, *job);
					if (reserve_error != OK) {
						memdelete(job);
						return reserve_error;
					}
				}
				Dictionary record;
				const Error record_error = _read_record(id, record);
				if (record_error != OK) {
					memdelete(job);
					return record_error;
				}
				const Error reserve_error = EntitySceneIO::reserve_record(record, *job);
				if (reserve_error != OK) {
					memdelete(job);
					return reserve_error;
				}
				job->pending.push_back({ id, path, cluster, record.duplicate(true) });
				job->skip.insert(id);
			} else {
				job->required_files.insert(path, cluster);
			}
		}
	}
	const String relative = EntitySceneIO::cell_directory(p_cell.grid, p_cell.x, p_cell.y, p_cell.z);
	for (const KeyValue<EntityId, Section> &entry : deleted_storage) {
		if (entry.value.path.get_base_dir() == relative) {
			job->skip.insert(entry.key);
		}
	}
	job->directory = _cell_path(p_cell);
	if ((job->directory.is_empty() || !DirAccess::dir_exists_absolute(job->directory)) && job->required_files.is_empty() && job->pending.is_empty()) {
		if (cells.has(p_cell)) {
			const Error error = pin(job->ancestors);
			if (error != OK) {
				memdelete(job);
				return error;
			}
			resident_cells.insert(p_cell, job->ancestors);
			residency_serial++;
		} else {
			probed_cells.insert(p_cell, false);
		}
		memdelete(job);
		return OK;
	}
	job->relative = relative;
	job->globals = globals;
	EntityTaskScheduler *scheduler = EntityTaskScheduler::get_singleton();
	const Error error = scheduler ? scheduler->submit(job, cell_mailbox) : ERR_UNAVAILABLE;
	if (error != OK) {
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
			if (p_job.skip.has(file_id)) {
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
				p_job.members.insert(record.id);
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
			if (p_job.skip.has(record.id) || (record.path.get_base_dir() != p_job.relative && !p_job.required.has(record.id))) {
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
		if (!p_job.skip.has(id) && !owners.has(id)) {
			p_job.result.failing = id;
			p_job.result.failing_field = "record";
			return ERR_FILE_CORRUPT;
		}
	}
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

void EntityScene::CellJob::prepare(bool p_decode_only) {
	CellJob &p_job = *this;
	if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
		p_job.result.error = ERR_SKIP;
		return;
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
		if (p_job.profile) {
			print_line(vformat("Entity cell reads grid=%s cell=%d_%d_%d enumerate_ms=%.3f read_parse_cpu_ms=%.3f read_parse_wall_ms=%.3f files=%d ranges=%d lanes=%d", p_job.result.key.grid, p_job.result.key.x, p_job.result.key.y, p_job.result.key.z, p_job.enumerate_usec / 1000.0, p_job.read_parse_usec / 1000.0, p_job.read_parse_wall_usec / 1000.0, p_job.files_read, p_job.read_ranges.size(), p_job.read_lanes));
		}
		p_job.read_ranges.clear();
		p_job.filenames.clear();
	}
	if (p_job.result.error == OK) {
		_run_cell_decode(this);
	}
}

void EntityScene::_collect_cell_jobs() {
	while (EntityTaskScheduler::Graph *graph = cell_mailbox->pop()) {
		graph->ready = true;
	}
}

Error EntityScene::_decode_cell(CellJob &p_job) {
	if (!p_job.result.grouped) {
		HashMap<Vector<uint64_t>, PreparedGroup *, PreparedSignatureHasher> groups;
		p_job.result.entities.reserve(p_job.pending.size());
		for (PendingRecord &record : p_job.pending) {
			if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
				return ERR_SKIP;
			}
			PreparedEntity prepared;
			prepared.path = record.path;
			prepared.cluster = record.cluster;
			Vector<uint64_t> types;
			String field;
			const Error error = _prepare_stored(record.id, record.record, prepared, types, field);
			if (error != OK) {
				p_job.result.failing = record.id;
				p_job.result.failing_field = field + " (" + p_job.storage_directory.path_join(record.path) + ")";
				return error;
			}
			if (!prepared.deleted) {
				types.sort();
				PreparedGroup **existing = groups.getptr(types);
				if (existing) {
					prepared.group = *existing;
				} else {
					prepared.group = memnew(PreparedGroup);
					prepared.group->profile = p_job.profile;
					prepared.group->signature = types;
					p_job.result.groups.push_back(prepared.group);
					groups.insert(types, prepared.group);
				}
				prepared.row = prepared.group->capacity++;
			}
			record.prepared_index = p_job.result.entities.size();
			p_job.result.entities.push_back(std::move(prepared));
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
	Error result = OK;
	uint32_t remaining = 0;
	for (uint32_t i = 0; i < p_job.pending.size(); i++) {
		if (p_job.cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
			return ERR_SKIP;
		}
		PendingRecord &record = p_job.pending[i];
		PreparedEntity &prepared = p_job.result.entities[record.prepared_index];
		String field;
		const Error record_error = prepared.deleted ? OK : prepared.group->decode_row(prepared.row, record.record["components"], field);
		if (record_error == ERR_UNAVAILABLE) {
			if (result == OK) {
				p_job.result.failing = record.id;
				p_job.result.failing_field = field + " (" + p_job.storage_directory.path_join(record.path) + ")";
			}
			if (remaining != i) {
				p_job.pending[remaining] = std::move(record);
			}
			remaining++;
			result = ERR_UNAVAILABLE;
			continue;
		}
		if (record_error != OK) {
			p_job.result.failing = record.id;
			p_job.result.failing_field = field + " (" + p_job.directory.path_join(record.path.get_file()) + ")";
			return record_error;
		}
		record.record = Dictionary();
	}
	p_job.pending.resize(remaining);
	if (result != OK) {
		return result;
	}
	HashSet<EntityId, EntityIdHasher> inside(p_job.skip);
	inside.reserve(p_job.skip.size() + p_job.result.entities.size());
	for (const PreparedEntity &entity : p_job.result.entities) {
		inside.insert(entity.id);
	}
	for (const PreparedEntity &entity : p_job.result.entities) {
		const EntityId parent = entity.parent.id;
		if (!parent.is_valid() || inside.has(parent) || p_job.globals.has(parent)) {
			continue;
		}
		p_job.result.failing = entity.id;
		p_job.result.failing_field = "parent (" + p_job.storage_directory.path_join(entity.path) + ")";
		return ERR_INVALID_DATA;
	}
	return OK;
}

void EntityScene::_run_cell_decode(void *p_job) {
	CellJob *job = static_cast<CellJob *>(p_job);
	const uint64_t began = job->profile ? OS::get_singleton()->get_ticks_usec() : 0;
	entity_decode_assets_cached_only(true);
	entity_decode_set_budget([](void *p_userdata, uint64_t p_bytes) {
		return static_cast<CellJob *>(p_userdata)->reserve_payload(p_bytes);
	},
			job);
	const uint64_t decoding = job->profile ? OS::get_singleton()->get_ticks_usec() : 0;
	job->result.error = _decode_cell(*job);
	entity_decode_set_budget(nullptr);
	if (job->profile) {
		job->decode_usec += OS::get_singleton()->get_ticks_usec() - decoding;
	}
	entity_decode_assets_cached_only(false);
	entity_decode_take_missing_assets(job->missing);
	if (job->profile) {
		job->worker_total_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
}

Error EntityScene::_revalidate_job(CellJob &p_job, Vector<EntityId> &r_ids) {
	if (resident_cells.has(p_job.result.key)) {
		return ERR_BUSY;
	}
	for (PreparedEntity &entity : p_job.result.entities) {
		if (entity.live) {
			continue;
		}
		const EntityCatalog::Record *record = catalog.records.getptr(entity.id);
		if (record) {
			if (record->deleted != entity.deleted) {
				return ERR_INVALID_DATA;
			}
			if (resolve(entity.id).state == EntityReferenceState::RESIDENT) {
				entity.live = true;
				entity.parent = record->parent;
				entity.order = get_order(entity.id);
				entity.release();
				continue;
			}
		}
		if (dirty.has(entity.id)) {
			return ERR_BUSY;
		}
		r_ids.push_back(entity.id);
	}
	HashSet<EntityId, EntityIdHasher> inside;
	for (const PreparedEntity &entity : p_job.result.entities) {
		inside.insert(entity.id);
	}
	HashSet<EntityId, EntityIdHasher> pinned_ancestors;
	pinned_ancestors.reserve(p_job.ancestors.size());
	for (EntityId id : p_job.ancestors) {
		pinned_ancestors.insert(id);
	}
	LocalVector<PreparedEntity> ancestors;
	for (const PreparedEntity &entity : p_job.result.entities) {
		EntityId parent = entity.parent.id;
		while (parent.is_valid() && !inside.has(parent)) {
			const EntityReferenceState parent_state = resolve(parent).state;
			if (parent_state == EntityReferenceState::DELETED || parent_state == EntityReferenceState::MISSING) {
				p_job.result.failing = entity.id;
				p_job.result.failing_field = "parent (" + parent.to_string() + ")";
				return ERR_DOES_NOT_EXIST;
			}
			if (parent_state != EntityReferenceState::RESIDENT) {
				return ERR_BUSY;
			}
			PreparedEntity live;
			live.id = parent;
			live.live = true;
			live.parent = catalog.get_parent(parent);
			live.order = get_order(parent);
			inside.insert(parent);
			ancestors.push_back(std::move(live));
			if (!globals.has(parent) && !pinned_ancestors.has(parent)) {
				p_job.ancestors.push_back(parent);
				pinned_ancestors.insert(parent);
			}
			parent = catalog.get_parent(parent).id;
		}
	}
	for (PreparedEntity &entity : ancestors) {
		p_job.result.entities.push_back(std::move(entity));
	}
	return OK;
}

Error EntityScene::_load_cell_assets(CellJob &p_job) {
	LocalVector<Ref<Resource>> &held = cell_assets[p_job.result.key];
	for (const String &path : p_job.missing) {
		Error error = OK;
		Ref<Resource> asset = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
		if (error != OK || asset.is_null()) {
			cell_assets.erase(p_job.result.key);
			return error == OK ? ERR_FILE_CORRUPT : error;
		}
		held.push_back(asset);
		p_job.assets.push_back(asset);
		p_job.loaded.insert(path);
	}
	return OK;
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
			error = _load_cell_assets(p_job);
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
		p_job.resume = true;
		r_stats.jobs_resumed++;
		return OK;
	}
	if (p_job.result.error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		cell_assets.erase(p_job.result.key);
		return _fail(p_job.result.failing, p_job.result.failing_field, p_job.result.error);
	}
	Vector<EntityId> ids;
	uint64_t began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	const Error stale = _revalidate_job(p_job, ids);
	if (timed) {
		r_profile->revalidate_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (stale != OK) {
		r_stats.jobs_discarded++;
		cell_assets.erase(p_job.result.key);
		if (stale != ERR_BUSY) {
			failed_cells.insert(p_job.result.key, revision);
			return _fail(p_job.result.failing, p_job.result.failing_field.is_empty() ? EntitySceneIO::cell_directory(p_job.result.key.grid, p_job.result.key.x, p_job.result.key.y, p_job.result.key.z) : p_job.result.failing_field, stale);
		}
		return OK;
	}
	const PreparedSet set(p_job.result.entities, &p_job.result.groups);
	LocalVector<CommitItem> items;
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	Error error = _can_commit(set, ids, items);
	if (timed) {
		r_profile->commit_validate_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		cell_assets.erase(p_job.result.key);
		return error;
	}
	const uint64_t previous_revision = revision;
	const CommitProfile previous_install = timed ? r_profile->install : CommitProfile();
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	error = _commit(set, items, true, false, timed ? &r_profile->install : nullptr, &p_job.result.key, &p_job.members);
	if (timed) {
		r_profile->install_usec += OS::get_singleton()->get_ticks_usec() - began;
		const CommitProfile &install = r_profile->install;
		print_line(vformat("Entity cell bulk grid=%s cell=%d_%d_%d groups=%d rows=%d compacted=%d skipped=%d metadata_usec=%d metadata_new=%d metadata_record_updates=%d metadata_order_updates=%d metadata_section_updates=%d metadata_membership_removals=%d metadata_full_cell_clears=%d cell_membership_insertions=%d ecs_create_components_usec=%d compact_usec=%d parent_usec=%d transform_finalize_usec=%d initial_packet_prepare_usec=%d initial_packet_updates=%d initial_packet_table_rows=%d initial_packet_fallback_rows=%d result=%d", p_job.result.key.grid, p_job.result.key.x, p_job.result.key.y, p_job.result.key.z, install.bulk_groups - previous_install.bulk_groups, install.bulk_rows - previous_install.bulk_rows, install.compacted_rows - previous_install.compacted_rows, install.skipped_rows - previous_install.skipped_rows, install.metadata_commit_usec - previous_install.metadata_commit_usec, install.metadata_new_records - previous_install.metadata_new_records, install.metadata_record_updates - previous_install.metadata_record_updates, install.metadata_order_updates - previous_install.metadata_order_updates, install.metadata_section_updates - previous_install.metadata_section_updates, install.metadata_membership_removals - previous_install.metadata_membership_removals, install.metadata_full_cell_clears - previous_install.metadata_full_cell_clears, install.cell_membership_insertions - previous_install.cell_membership_insertions, install.ecs_bulk_create_components_usec - previous_install.ecs_bulk_create_components_usec, install.bulk_compact_usec - previous_install.bulk_compact_usec, install.ecs_final_parent_set_usec - previous_install.ecs_final_parent_set_usec, install.transform_finalize_usec - previous_install.transform_finalize_usec, install.initial_packet_prepare_usec - previous_install.initial_packet_prepare_usec, install.initial_packet_updates - previous_install.initial_packet_updates, install.initial_packet_table_rows - previous_install.initial_packet_table_rows, install.initial_packet_fallback_rows - previous_install.initial_packet_fallback_rows, error));
	}
	revision = previous_revision;
	cell_assets.erase(p_job.result.key);
	if (error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		return error;
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	error = pin(p_job.ancestors);
	if (error != OK) {
		if (timed) {
			r_profile->residency_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		return error;
	}
	resident_cells.insert(p_job.result.key, p_job.ancestors);
	residency_serial++;
	r_stats.cells_committed++;
	r_stats.entities_committed += ids.size();
	if (timed) {
		r_profile->residency_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	return OK;
}

Error EntityScene::commit_ready(int p_max_entities, Stats *r_stats) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
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
				constructed_rows += group->constructions;
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
		memdelete(job);
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
	return result;
}

void EntityScene::flush_streaming() {
	if (cell_jobs.is_empty()) {
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
		while (!job->ready) {
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
}

Error EntityScene::request_cells(const Vector<CellKey> &p_cells, int *r_remaining, Stats *r_stats) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
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
	Error result = OK;
	bool released = false;
	for (const CellKey &cell : p_cells) {
		for (CellJob *job : cell_jobs) {
			if (job->result.key == cell) {
				job->cancelled.set();
			}
		}
		cell_assets.erase(cell);
		const Vector<EntityId> *pinned = resident_cells.getptr(cell);
		if (!pinned) {
			continue;
		}
		const Vector<EntityId> ancestors = *pinned;
		resident_cells.erase(cell);
		unpin(ancestors);
		released = true;
		const CellMembers *members = cells.getptr(cell);
		if (!members) {
			continue;
		}
		HashSet<EntityId, EntityIdHasher> candidates;
		for (const KeyValue<EntityId, bool> &member : *members) {
			const EntityId id = member.key;
			if (resolve(id).state == EntityReferenceState::RESIDENT && !pins.has(id) && !dirty.has(id)) {
				candidates.insert(id);
			}
		}
		bool blocked = true;
		while (blocked) {
			blocked = false;
			Vector<EntityId> rejected;
			for (EntityId id : candidates) {
				for (EntityId child : catalog.get_children(id)) {
					if (resolve(child).state == EntityReferenceState::RESIDENT && !candidates.has(child)) {
						rejected.push_back(id);
						break;
					}
				}
			}
			for (EntityId id : rejected) {
				candidates.erase(id);
				blocked = true;
			}
		}
		if (candidates.is_empty()) {
			continue;
		}
		Vector<EntityId> unloading;
		unloading.reserve(candidates.size());
		for (EntityId id : candidates) {
			unloading.push_back(id);
		}
		Error error = unload_subset(unloading);
		if (error != OK) {
			result = error;
			continue;
		}
		HashSet<EntityId, EntityIdHasher> forgotten;
		for (EntityId id : unloading) {
			if (!prefab_members.has(id)) {
				forgotten.insert(id);
			}
		}
		bool pending = true;
		while (pending) {
			pending = false;
			Vector<EntityId> kept;
			for (EntityId id : forgotten) {
				for (EntityId child : catalog.get_children(id)) {
					if (!forgotten.has(child)) {
						kept.push_back(id);
						break;
					}
				}
			}
			for (EntityId id : kept) {
				forgotten.erase(id);
				pending = true;
			}
		}
		for (EntityId id : forgotten) {
			_forget_entity(id);
		}
	}
	if (released) {
		residency_serial++;
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
	const Section *section = sections.getptr(p_id);
	return section ? section->name : String();
}

#endif
