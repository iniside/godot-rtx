#ifndef _3D_DISABLED

#include "entity_scene.h"
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
	const Vector<String> parts = p_directory.split("/", true);
	if (parts.size() != 3 || parts[0] != "cells" || parts[1].is_empty()) {
		return false;
	}
	const Vector<String> coordinates = parts[2].split("_", true);
	if (coordinates.size() != 3) {
		return false;
	}
	int64_t values[3] = { 0, 0, 0 };
	for (int i = 0; i < 3; i++) {
		if (coordinates[i].is_empty() || !coordinates[i].is_valid_int()) {
			return false;
		}
		values[i] = coordinates[i].to_int();
		if (itos(values[i]) != coordinates[i] || values[i] < INT32_MIN || values[i] > INT32_MAX) {
			return false;
		}
	}
	r_key.grid = parts[1];
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
	const CellKey *entry = cell_of.getptr(p_id);
	if (!entry) {
		return;
	}
	const CellKey key = *entry;
	cell_of.erase(p_id);
	HashSet<EntityId, EntityIdHasher> *members = cells.getptr(key);
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
	CellKey key;
	if (!_parse_cell_directory(directory, key) || !grids.has(key.grid)) {
		return _fail(p_id, "cell/" + source, ERR_INVALID_DATA);
	}
	cells[key].insert(p_id);
	cell_of.insert(p_id, key);
	return OK;
}

Error EntityScene::_rebuild_cells() {
	cells.clear();
	cell_of.clear();
	globals.clear();
	Error result = OK;
	for (const KeyValue<EntityId, EntityCatalog::Record> &entry : catalog.records) {
		const Error error = _assign_cell(entry.key);
		if (error != OK && result == OK) {
			result = error;
		}
	}
	return result;
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
	components = std::move(p_other.components);
	values = std::move(p_other.values);
	return *this;
}

void EntityScene::PreparedEntity::release() {
	if (values.is_empty()) {
		return;
	}
	const EntitySchemaRegistry &schemas = EntitySchemaRegistry::descriptors();
	for (PreparedComponent &value : values) {
		if (!value.buffer) {
			continue;
		}
		const EntityComponentSchema *schema = schemas.find(value.schema_id);
		if (schema) {
			schema->destruct(value.buffer);
		}
		Memory::free_aligned_static(value.buffer);
		value.buffer = nullptr;
	}
	values.clear();
}

Error EntityScene::PreparedEntity::decode_component(const EntityComponentSchema &p_schema, const Variant &p_value) {
	ERR_FAIL_NULL_V(p_schema.construct, ERR_INVALID_PARAMETER);
	void *buffer = Memory::alloc_aligned_static(p_schema.size, p_schema.alignment);
	ERR_FAIL_NULL_V(buffer, ERR_OUT_OF_MEMORY);
	p_schema.construct(buffer);
	Error error = p_schema.decode(buffer, p_value);
	if (error != OK) {
		p_schema.destruct(buffer);
		Memory::free_aligned_static(buffer);
		return error;
	}
	values.push_back({ p_schema.id, buffer });
	return OK;
}

EntityScene::PreparedSet::PreparedSet(const LocalVector<PreparedEntity> &p_entities) :
		entities(&p_entities) {
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
	for (EntityId id : p_ids) {
		Vector<EntityId> ancestors;
		HashSet<EntityId, EntityIdHasher> chain;
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
		for (int i = ancestors.size() - 1; i >= 0; i--) {
			seen.insert(ancestors[i]);
			r_ids.push_back(ancestors[i]);
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
	return SECTION_SET;
}

void EntityScene::PreparedSet::write_components(EntityWorld &p_target, EntityHandle p_handle, EntityId p_id) const {
	if (scene) {
		EntityHandle from = scene->resolve(p_id).handle;
		for (const KeyValue<uint64_t, EntityComponentSchema> &entry : p_target.schemas.get_types()) {
			const EntityComponentSchema &schema = entry.value;
			if (!schema.is_component) {
				continue;
			}
			const EntityComponentSchema *source = scene->world->schemas.find(schema.id);
			const void *value = source ? ecs_get_id(scene->world->ecs.c_ptr(), from.entity, source->runtime_id) : nullptr;
			if (value) {
				schema.copy_to(p_target.ecs, p_handle.entity, value);
			} else {
				p_target.ecs.entity(p_handle.entity).remove(schema.runtime_id);
			}
		}
		return;
	}
	const PreparedEntity *entry = find(p_id);
	if (!entry || entry->live) {
		return;
	}
	for (const PreparedComponent &value : entry->values) {
		const EntityComponentSchema *schema = p_target.schemas.find(value.schema_id);
		if (schema) {
			schema->copy_to(p_target.ecs, p_handle.entity, value.buffer);
		}
	}
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
	const Dictionary components = p_record["components"];
	const EntitySchemaRegistry &schemas = EntitySchemaRegistry::descriptors();
	for (int i = 0; i < types.size(); i++) {
		const EntityComponentSchema *schema = schemas.find(types[i]);
		ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
		error = r_prepared.decode_component(*schema, components[r_prepared.components[i]]);
		if (error != OK) {
			r_field = schema->key;
			break;
		}
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

Error EntityScene::_can_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids) const {
	HashSet<EntityId, EntityIdHasher> affected;
	for (EntityId id : p_ids) {
		affected.insert(id);
	}
	for (EntityId id : p_ids) {
		if (p_prepared.is_deleted(id)) {
			if (pins.has(id)) {
				return ERR_BUSY;
			}
			for (EntityId child : catalog.get_children(id)) {
				if (!affected.has(child) || (!p_prepared.is_deleted(child) && p_prepared.get_parent(child).id == id)) {
					return ERR_BUSY;
				}
			}
		}
	}
	return OK;
}

Error EntityScene::_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, bool p_resident, bool p_dirty) {
	EntityWorld *target = get_world();
	HashSet<EntityId, EntityIdHasher> residency;
	Vector<EntityId> active;
	for (EntityId id : p_ids) {
		if (!p_prepared.is_deleted(id) && (p_resident || resolve(id).state == EntityReferenceState::RESIDENT)) {
			active.push_back(id);
		}
	}
	Vector<EntityId> required;
	const Error required_error = p_prepared.collect_required(active, required);
	if (required_error != OK) {
		return _fail(active.is_empty() ? EntityId() : active[0], "required", required_error);
	}
	for (EntityId id : required) {
		residency.insert(id);
	}
	for (EntityId id : p_ids) {
		EntityResolution existing = resolve(id);
		if (existing.state == EntityReferenceState::RESIDENT) {
			target->ecs.entity(existing.handle.entity).remove<flecs::Parent>();
		}
		if (catalog.records.has(id)) {
			catalog._unlink_parent(id);
		}
	}
	for (EntityId id : p_ids) {
		const bool deleted = p_prepared.is_deleted(id);
		catalog.records.insert(id, { deleted, {} });
		EntityResolution existing = resolve(id);
		bool resident = existing.state == EntityReferenceState::RESIDENT;
		if (deleted && resident) {
			target->transforms.forget(existing.handle.entity);
			target->residents.erase(id);
			target->ecs.entity(existing.handle.entity).destruct();
			resident = false;
		}
		if (!deleted && residency.has(id)) {
			EntityHandle handle = resident ? existing.handle : target->_materialize(id);
			p_prepared.write_components(*target, handle, id);
			target->_component_changed(handle);
		} else {
			target->_mark_changed(id);
		}
		order.insert(id, p_prepared.get_order(id));
		Section section;
		const SectionAction action = deleted ? SECTION_ERASE : p_prepared.build_section(id, section);
		if (action == SECTION_SET) {
			const Section *previous = sections.getptr(id);
			if (previous && section.path.is_empty()) {
				section.path = previous->path;
				section.cluster = previous->cluster;
			}
			sections.insert(id, section);
		} else if (action == SECTION_ERASE) {
			sections.erase(id);
		}
		if (p_dirty) {
			dirty.insert(id);
		}
	}
	for (EntityId id : p_ids) {
		const EntityRef parent = p_prepared.get_parent(id);
		catalog.records[id].parent = parent;
		if (!p_prepared.is_deleted(id)) {
			catalog._set_parent(id, parent);
			EntityResolution existing = resolve(id);
			if (existing.state == EntityReferenceState::RESIDENT && parent.id.is_valid()) {
				target->ecs.entity(existing.handle.entity).set<flecs::Parent>({ resolve(parent.id).handle.entity });
			}
		}
	}
	if (p_dirty) {
		_index_prefabs();
	}
	for (EntityId id : p_ids) {
		_assign_cell(id);
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
	const uint64_t check_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	error = _can_commit(set, p_ids);
	if (r_profile) {
		r_profile->check = OS::get_singleton()->get_ticks_usec() - check_begin;
	}
	if (error != OK) {
		return error;
	}
	const uint64_t commit_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	const uint64_t previous_revision = revision;
	error = _commit(set, p_ids, true, false);
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
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
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
	const uint64_t collected = OS::get_singleton()->get_ticks_usec();
	error = _load_resident(unloaded, profiling ? &profile : nullptr);
	if (error != OK) {
		return error;
	}
	const uint64_t committed = OS::get_singleton()->get_ticks_usec();
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
	Error cell_error = result->_rebuild_cells();
	if (cell_error != OK) {
		last_error = result->last_error;
		return cell_error;
	}
	r_scene = result;
	return OK;
}

Error EntityScene::_cell_entities(const CellKey &p_cell, Vector<EntityId> &r_ids, Vector<EntityId> &r_ancestors) const {
	const HashSet<EntityId, EntityIdHasher> *members = cells.getptr(p_cell);
	if (!members) {
		return ERR_DOES_NOT_EXIST;
	}
	HashSet<EntityId, EntityIdHasher> inside;
	for (EntityId id : *members) {
		if (catalog.get_state(id) != EntityReferenceState::UNLOADED) {
			continue;
		}
		inside.insert(id);
		r_ids.push_back(id);
	}
	HashSet<EntityId, EntityIdHasher> seen;
	for (EntityId id : r_ids) {
		int guard = catalog.get_record_count() + 1;
		for (EntityId ancestor = catalog.get_parent(id).id; ancestor.is_valid(); ancestor = catalog.get_parent(ancestor).id) {
			if (guard-- <= 0) {
				return ERR_CYCLIC_LINK;
			}
			if (inside.has(ancestor) || seen.has(ancestor)) {
				break;
			}
			seen.insert(ancestor);
			r_ancestors.push_back(ancestor);
		}
	}
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

Error EntityScene::_snapshot_source(EntityId p_id, const String &p_directory, RecordSource &r_source) {
	r_source.id = p_id;
	r_source.parent = catalog.get_parent(p_id);
	r_source.order = get_order(p_id);
	r_source.deleted = catalog.records[p_id].deleted;
	if (r_source.deleted) {
		return OK;
	}
	const Section *section = sections.getptr(p_id);
	if (!section) {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	if (!section->record.is_empty()) {
		r_source.record = _shallow_record(section->record);
		return OK;
	}
	if (section->path.is_empty() || p_directory.is_empty()) {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	r_source.path = p_directory.path_join(section->path);
	r_source.cluster = section->cluster;
	return OK;
}

Error EntityScene::_dispatch_cell(const CellKey &p_cell) {
	Vector<EntityId> ids;
	Vector<EntityId> ancestors;
	Error error = _cell_entities(p_cell, ids, ancestors);
	if (error != OK) {
		return error;
	}
	Vector<EntityId> wanted = ancestors;
	wanted.append_array(ids);
	Vector<EntityId> required;
	error = _collect_required(wanted, required);
	if (error != OK) {
		return error;
	}
	for (EntityId id : required) {
		if (prefab_members.has(id) && resolve(id).state != EntityReferenceState::RESIDENT) {
			return _load_cell(p_cell, required, ancestors);
		}
	}
	CellJob *job = memnew(CellJob);
	job->result.key = p_cell;
	job->ancestors = ancestors;
	const String directory = storage_path.is_empty() ? String() : EntitySceneIO::scene_directory(storage_path);
	for (EntityId id : required) {
		const EntityResolution target = resolve(id);
		if (target.state == EntityReferenceState::DELETED) {
			memdelete(job);
			return _fail(id, "record", ERR_DOES_NOT_EXIST);
		}
		if (target.state == EntityReferenceState::RESIDENT) {
			job->resident.push_back(id);
			continue;
		}
		RecordSource source;
		error = _snapshot_source(id, directory, source);
		if (error != OK) {
			memdelete(job);
			return error;
		}
		job->sources.push_back(source);
	}
	if (job->sources.is_empty()) {
		memdelete(job);
		return _load_cell(p_cell, required, ancestors);
	}
	job->task = WorkerThreadPool::get_singleton()->add_native_task(_run_cell_job, job, false, "Entity cell load");
	cell_jobs.push_back(job);
	return OK;
}

Error EntityScene::_load_cell(const CellKey &p_cell, const Vector<EntityId> &p_required, const Vector<EntityId> &p_ancestors) {
	Vector<EntityId> unloaded;
	for (EntityId id : p_required) {
		if (resolve(id).state != EntityReferenceState::RESIDENT) {
			unloaded.push_back(id);
		}
	}
	Error error = unloaded.is_empty() ? OK : load_subset(unloaded);
	if (error != OK) {
		return error;
	}
	error = pin(p_ancestors);
	if (error != OK) {
		return error;
	}
	resident_cells.insert(p_cell, p_ancestors);
	residency_serial++;
	return OK;
}

void EntityScene::_run_cell_job(void *p_job) {
	CellJob *job = static_cast<CellJob *>(p_job);
	const uint64_t began = OS::get_singleton()->get_ticks_usec();
	HashMap<String, Dictionary> clusters;
	job->result.entities.reserve(job->sources.size());
	entity_decode_assets_cached_only(true);
	for (const RecordSource &source : job->sources) {
		if (job->cancelled.is_set() || ResourceLoader::is_cleaning_tasks()) {
			job->result.error = ERR_SKIP;
			break;
		}
		PreparedEntity prepared;
		prepared.id = source.id;
		prepared.parent = source.parent;
		prepared.order = source.order;
		prepared.deleted = source.deleted;
		Error error = OK;
		String field;
		if (!prepared.deleted) {
			Dictionary record = source.record;
			if (record.is_empty()) {
				field = source.cluster ? "cluster" : "record";
				const Dictionary *cluster = source.cluster ? clusters.getptr(source.path) : nullptr;
				if (!cluster) {
					Variant parsed;
					error = EntitySceneIO::read_variant_file(source.path, parsed);
					if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
						error = ERR_FILE_CORRUPT;
					}
					if (error == OK && source.cluster) {
						clusters.insert(source.path, parsed);
						cluster = clusters.getptr(source.path);
					} else if (error == OK) {
						record = parsed;
					}
				}
				if (error == OK && cluster) {
					const Variant value = cluster->get(source.id.to_string(), Variant());
					if (value.get_type() != Variant::DICTIONARY) {
						error = ERR_FILE_CORRUPT;
					} else {
						record = value;
					}
				}
			}
			if (error == OK) {
				error = _decode_record(record, prepared, field, nullptr);
			}
		}
		job->result.entities.push_back(std::move(prepared));
		if (error != OK) {
			if (job->result.error == OK) {
				job->result.error = error;
				job->result.failing = source.id;
				job->result.failing_field = field;
			}
			if (error != ERR_UNAVAILABLE) {
				break;
			}
		}
	}
	entity_decode_assets_cached_only(false);
	entity_decode_take_missing_assets(job->missing);
	job->worker_usec = OS::get_singleton()->get_ticks_usec() - began;
}

bool EntityScene::_revalidate_job(CellJob &p_job, Vector<EntityId> &r_ids) {
	if (!cells.has(p_job.result.key) || resident_cells.has(p_job.result.key)) {
		return false;
	}
	for (EntityId id : p_job.resident) {
		if (resolve(id).state != EntityReferenceState::RESIDENT) {
			return false;
		}
		PreparedEntity live;
		live.id = id;
		live.live = true;
		live.parent = catalog.get_parent(id);
		live.order = get_order(id);
		p_job.result.entities.push_back(std::move(live));
	}
	HashSet<EntityId, EntityIdHasher> prepared_ids;
	for (const PreparedEntity &entity : p_job.result.entities) {
		prepared_ids.insert(entity.id);
	}
	for (PreparedEntity &entity : p_job.result.entities) {
		const EntityCatalog::Record *record = catalog.records.getptr(entity.id);
		if (!record || record->deleted) {
			return false;
		}
		const EntityId parent = record->parent.id;
		if (parent.is_valid() && !prepared_ids.has(parent) && resolve(parent).state != EntityReferenceState::RESIDENT) {
			return false;
		}
		entity.parent = record->parent;
		entity.order = get_order(entity.id);
		if (entity.live || resolve(entity.id).state == EntityReferenceState::RESIDENT) {
			entity.live = true;
			entity.release();
			continue;
		}
		r_ids.push_back(entity.id);
	}
	return true;
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
	}
	return OK;
}

Error EntityScene::_commit_cell(CellJob &p_job, Stats &r_stats) {
	if (p_job.result.error == ERR_SKIP) {
		r_stats.jobs_discarded++;
		cell_assets.erase(p_job.result.key);
		return OK;
	}
	if (p_job.result.error == ERR_UNAVAILABLE) {
		r_stats.jobs_discarded++;
		const Error error = p_job.missing.is_empty() ? ERR_FILE_NOT_FOUND : _load_cell_assets(p_job);
		if (error != OK) {
			failed_cells.insert(p_job.result.key, revision);
			return _fail(p_job.result.failing, p_job.result.failing_field, error);
		}
		return OK;
	}
	if (p_job.result.error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		cell_assets.erase(p_job.result.key);
		return _fail(p_job.result.failing, p_job.result.failing_field, p_job.result.error);
	}
	Vector<EntityId> ids;
	if (!_revalidate_job(p_job, ids)) {
		r_stats.jobs_discarded++;
		cell_assets.erase(p_job.result.key);
		return OK;
	}
	const PreparedSet set(p_job.result.entities);
	Error error = _can_commit(set, ids);
	if (error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		cell_assets.erase(p_job.result.key);
		return _fail(ids.is_empty() ? EntityId() : ids[0], "cell", error);
	}
	const uint64_t previous_revision = revision;
	error = _commit(set, ids, true, false);
	revision = previous_revision;
	cell_assets.erase(p_job.result.key);
	if (error != OK) {
		failed_cells.insert(p_job.result.key, revision);
		return error;
	}
	error = pin(p_job.ancestors);
	if (error != OK) {
		return error;
	}
	resident_cells.insert(p_job.result.key, p_job.ancestors);
	residency_serial++;
	r_stats.cells_committed++;
	r_stats.entities_committed += ids.size();
	return OK;
}

Error EntityScene::commit_ready(int p_max_entities, Stats *r_stats) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	if (cell_jobs.is_empty()) {
		return OK;
	}
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const uint64_t began = OS::get_singleton()->get_ticks_usec();
	Stats stats;
	Error result = OK;
	uint32_t index = 0;
	while (index < cell_jobs.size()) {
		CellJob *job = cell_jobs[index];
		const bool budgeted = stats.cells_committed > 0 && p_max_entities > 0 && stats.entities_committed >= p_max_entities;
		if (!pool->is_task_completed(job->task) || (budgeted && !job->cancelled.is_set())) {
			index++;
			continue;
		}
		cell_jobs.remove_at(index);
		pool->wait_for_task_completion(job->task);
		if (job->cancelled.is_set()) {
			stats.jobs_discarded++;
			cell_assets.erase(job->result.key);
		} else {
			stats.jobs_completed++;
			stats.worker_usec += job->worker_usec;
			const Error error = _commit_cell(*job, stats);
			if (error != OK && result == OK) {
				result = error;
			}
		}
		memdelete(job);
	}
	stats.commit_usec = OS::get_singleton()->get_ticks_usec() - began;
	if (r_stats) {
		r_stats->jobs_completed += stats.jobs_completed;
		r_stats->jobs_discarded += stats.jobs_discarded;
		r_stats->cells_committed += stats.cells_committed;
		r_stats->entities_committed += stats.entities_committed;
		r_stats->commit_usec += stats.commit_usec;
		r_stats->worker_usec += stats.worker_usec;
	}
	return result;
}

void EntityScene::flush_streaming() {
	if (cell_jobs.is_empty()) {
		return;
	}
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	for (CellJob *job : cell_jobs) {
		job->cancelled.set();
	}
	CallQueue *queue = MessageQueue::get_main_singleton();
	const bool pump = queue && !queue->is_flushing();
	for (CellJob *job : cell_jobs) {
		const uint64_t began = OS::get_singleton()->get_ticks_usec();
		bool reported = false;
		while (!pool->is_task_completed(job->task)) {
			if (pump) {
				queue->flush();
				if (RenderingServer::get_singleton()) {
					RenderingServer::get_singleton()->sync();
				}
			}
			OS::get_singleton()->delay_usec(1000);
			if (!reported && OS::get_singleton()->get_ticks_usec() - began > 10000000) {
				reported = true;
				ERR_PRINT("Entity cell load did not stop after cancellation: " + job->result.key.grid + " " + itos(job->result.key.x) + "_" + itos(job->result.key.y) + "_" + itos(job->result.key.z));
			}
		}
		pool->wait_for_task_completion(job->task);
		memdelete(job);
	}
	cell_jobs.clear();
	cell_assets.clear();
}

Error EntityScene::request_cells(const Vector<CellKey> &p_cells, int *r_remaining, Stats *r_stats) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	const uint64_t began = OS::get_singleton()->get_ticks_usec();
	int remaining = 0;
	int dispatched = 0;
	Error result = OK;
	for (const CellKey &cell : p_cells) {
		if (resident_cells.has(cell) || !cells.has(cell)) {
			continue;
		}
		const uint64_t *failed = failed_cells.getptr(cell);
		if (failed) {
			if (*failed == revision) {
				continue;
			}
			failed_cells.erase(cell);
		}
		if (!_is_cell_in_flight(cell) && cell_jobs.size() < MAX_CELL_JOBS) {
			const uint32_t before = cell_jobs.size();
			const Error error = _dispatch_cell(cell);
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
	if (r_remaining) {
		*r_remaining = remaining;
	}
	if (r_stats) {
		r_stats->jobs_dispatched += dispatched;
		r_stats->dispatch_usec += OS::get_singleton()->get_ticks_usec() - began;
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
		const HashSet<EntityId, EntityIdHasher> *members = cells.getptr(cell);
		if (!members) {
			continue;
		}
		HashSet<EntityId, EntityIdHasher> candidates;
		for (EntityId id : *members) {
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
		}
	}
	if (released) {
		residency_serial++;
	}
	return result;
}

Vector<EntityScene::CellKey> EntityScene::get_cells() const {
	Vector<CellKey> result;
	result.reserve(cells.size());
	for (const KeyValue<CellKey, HashSet<EntityId, EntityIdHasher>> &entry : cells) {
		result.push_back(entry.key);
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
