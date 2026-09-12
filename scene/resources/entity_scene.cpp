#ifndef _3D_DISABLED

#include "entity_scene.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include "scene/entity/entity_scene_commands.h"
#include "scene/entity/entity_scene_io.h"

EntityScene::EntityScene() {
	Error error = EntityId::generate(document_id);
	ERR_FAIL_COND(error != OK);
	default_grid = "default";
	default_range = 256.0;
	grids.insert(default_grid, { 64.0, 0.0 });
}

EntityScene::~EntityScene() {
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

void EntityScene::_clear_scratch() {
	if (scratch.is_null()) {
		return;
	}
	EntityScene &target = **scratch;
	if (target.world) {
		Vector<EntityId> ordered;
		Error error = target._collect_required(target.catalog.get_ids(), ordered);
		for (int i = ordered.size() - 1; error == OK && i >= 0; i--) {
			EntityResolution resolution = target.resolve(ordered[i]);
			if (resolution.state == EntityReferenceState::RESIDENT) {
				error = target.world->unload_entity(resolution.handle);
			}
		}
		if (error != OK || target.world->get_resident_count() != 0) {
			scratch.unref();
			return;
		}
		target.world->drain_changed();
		target.world->get_rendering().clear();
	}
	target.catalog.children.clear();
	target.catalog.records.clear();
	target.order.clear();
	target.sections.clear();
	target.prefab_instances = Dictionary();
	target.prefab_members.clear();
	target.last_error = String();
}

Error EntityScene::_read_stored(EntityId p_id, Dictionary &r_record) {
	const Section *section = sections.getptr(p_id);
	if (!section) {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	if (!section->record.is_empty()) {
		r_record = section->record.duplicate(true);
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
		r_record = Dictionary(value).duplicate(true);
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

Error EntityScene::_validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, bool p_decode_assets, const String &p_prefix) {
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
				Error error = _validate_fields(p_id, field.nested_type_id, entries[i], p_decode_assets, address);
				if (error != OK) {
					return error;
				}
			}
		}
		if (field.asset_reference && p_decode_assets) {
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

Error EntityScene::_describe_components(EntityId p_id, const Dictionary &p_record, Section &r_section, Vector<uint64_t> &r_types) {
	if (!p_record.has("components") || p_record["components"].get_type() != Variant::DICTIONARY) {
		return _fail(p_id, "components", ERR_INVALID_DATA);
	}
	const Dictionary components = p_record["components"];
	const EntitySchemaRegistry &schemas = get_world()->schemas;
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
			return _fail(p_id, text, ERR_INVALID_DATA);
		}
		r_section.components.push_back(schema->key);
		r_types.push_back(schema->id);
	}
	return OK;
}

Error EntityScene::_describe(EntityId p_id, const Dictionary &p_record, Section &r_section, bool p_decode_assets) {
	Vector<uint64_t> types;
	Error error = _describe_components(p_id, p_record, r_section, types);
	if (error != OK) {
		return error;
	}
	const Dictionary components = p_record["components"];
	for (int i = 0; i < types.size(); i++) {
		error = _validate_fields(p_id, types[i], components[r_section.components[i]], p_decode_assets);
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

Error EntityScene::_install(EntityId p_id, const Dictionary &p_record, LoadProfile *r_profile) {
	Section section;
	Vector<uint64_t> types;
	uint64_t phase_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	Error error = _describe_components(p_id, p_record, section, types);
	if (r_profile) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		r_profile->describe += now - phase_begin;
		phase_begin = now;
	}
	if (error != OK) {
		return error;
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
	if (r_profile) {
		r_profile->world += OS::get_singleton()->get_ticks_usec() - phase_begin;
	}
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

Error EntityScene::_prepare(const Vector<EntityId> &p_ids, Ref<EntityScene> &r_scene, bool p_prefer_stored, LoadProfile *r_profile, bool p_scratch) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	Vector<EntityId> required;
	Error error = _collect_required(p_ids, required);
	if (error != OK) {
		return error;
	}
	Ref<EntityScene> prepared;
	if (p_scratch) {
		_clear_scratch();
		if (scratch.is_null()) {
			scratch.instantiate();
		}
		prepared = scratch;
	} else {
		prepared.instantiate();
	}
	prepared->document_id = document_id;
	for (EntityId id : required) {
		Dictionary record;
		const uint64_t read_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
		error = _read_record(id, record, nullptr, p_prefer_stored);
		if (r_profile) {
			r_profile->read_record += OS::get_singleton()->get_ticks_usec() - read_begin;
		}
		if (error != OK) {
			return error;
		}
		prepared->catalog.records.insert(id, catalog.records[id]);
		prepared->order.insert(id, get_order(id));
		if (!bool(record["deleted"])) {
			prepared->catalog._set_parent(id, catalog.get_parent(id));
			const uint64_t install_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
			error = prepared->_install(id, record, r_profile);
			if (r_profile) {
				r_profile->install += OS::get_singleton()->get_ticks_usec() - install_begin;
			}
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

Error EntityScene::_can_commit(const EntityScene &p_prepared, const Vector<EntityId> &p_ids) const {
	HashSet<EntityId, EntityIdHasher> affected;
	for (EntityId id : p_ids) {
		affected.insert(id);
	}
	for (EntityId id : p_ids) {
		if (p_prepared.catalog.records[id].deleted) {
			if (pins.has(id)) {
				return ERR_BUSY;
			}
			for (EntityId child : catalog.get_children(id)) {
				if (!affected.has(child) || (!p_prepared.catalog.records[child].deleted && p_prepared.catalog.get_parent(child).id == id)) {
					return ERR_BUSY;
				}
			}
		}
	}
	return OK;
}

void EntityScene::_commit(EntityScene &p_prepared, const Vector<EntityId> &p_ids, bool p_resident, bool p_dirty) {
	EntityWorld *target = get_world();
	HashSet<EntityId, EntityIdHasher> residency;
	Vector<EntityId> active;
	for (EntityId id : p_ids) {
		if (!p_prepared.catalog.records[id].deleted && (p_resident || resolve(id).state == EntityReferenceState::RESIDENT)) {
			active.push_back(id);
		}
	}
	Vector<EntityId> required;
	p_prepared._collect_required(active, required);
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
		const EntityCatalog::Record &record = p_prepared.catalog.records[id];
		catalog.records.insert(id, { record.deleted, {} });
		EntityResolution existing = resolve(id);
		bool resident = existing.state == EntityReferenceState::RESIDENT;
		if (record.deleted && resident) {
			target->transforms.forget(existing.handle.entity);
			target->residents.erase(id);
			target->ecs.entity(existing.handle.entity).destruct();
			resident = false;
		}
		if (!record.deleted && residency.has(id)) {
			EntityHandle handle = resident ? existing.handle : target->_materialize(id);
			EntityHandle from = p_prepared.resolve(id).handle;
			for (const KeyValue<uint64_t, EntityComponentSchema> &entry : target->schemas.get_types()) {
				const EntityComponentSchema &schema = entry.value;
				if (!schema.is_component) {
					continue;
				}
				const EntityComponentSchema *source_schema = p_prepared.world->schemas.find(schema.id);
				const void *value = ecs_get_id(p_prepared.world->ecs.c_ptr(), from.entity, source_schema->runtime_id);
				if (value) {
					schema.copy_to(target->ecs, handle.entity, value);
				} else {
					target->ecs.entity(handle.entity).remove(schema.runtime_id);
				}
			}
			target->_component_changed(handle);
		} else {
			target->_mark_changed(id);
		}
		order.insert(id, p_prepared.get_order(id));
		if (!record.deleted && p_prepared.sections.has(id)) {
			Section section = p_prepared.sections[id];
			const Section *previous = sections.getptr(id);
			if (previous && section.path.is_empty()) {
				section.path = previous->path;
				section.cluster = previous->cluster;
			}
			sections.insert(id, section);
		} else {
			sections.erase(id);
		}
		if (p_dirty) {
			dirty.insert(id);
		}
	}
	for (EntityId id : p_ids) {
		const EntityCatalog::Record &record = p_prepared.catalog.records[id];
		catalog.records[id].parent = record.parent;
		if (!record.deleted) {
			catalog._set_parent(id, record.parent);
			EntityResolution existing = resolve(id);
			if (existing.state == EntityReferenceState::RESIDENT && record.parent.id.is_valid()) {
				target->ecs.entity(existing.handle.entity).set<flecs::Parent>({ resolve(record.parent.id).handle.entity });
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
}

Error EntityScene::_load_resident(const Vector<EntityId> &p_ids, LoadProfile *r_profile) {
	Ref<EntityScene> prepared;
	const uint64_t prepare_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	Error error = _prepare(p_ids, prepared, false, r_profile, true);
	if (r_profile) {
		r_profile->prepare = OS::get_singleton()->get_ticks_usec() - prepare_begin;
	}
	if (error == OK) {
		const uint64_t check_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
		error = _can_commit(**prepared, p_ids);
		if (r_profile) {
			r_profile->check = OS::get_singleton()->get_ticks_usec() - check_begin;
		}
	}
	if (error == OK) {
		const uint64_t commit_begin = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
		const uint64_t previous_revision = revision;
		_commit(**prepared, p_ids, true, false);
		revision = previous_revision;
		if (r_profile) {
			r_profile->commit = OS::get_singleton()->get_ticks_usec() - commit_begin;
		}
	}
	prepared.unref();
	_clear_scratch();
	return error;
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

Error EntityScene::request_cells(const Vector<CellKey> &p_cells, int p_max_entities, int *r_remaining) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	if (r_remaining) {
		*r_remaining = 0;
	}
	Vector<EntityId> batch;
	HashSet<EntityId, EntityIdHasher> queued;
	Vector<CellKey> accepted;
	Vector<Vector<EntityId>> accepted_ancestors;
	int remaining = 0;
	bool exhausted = false;
	for (const CellKey &cell : p_cells) {
		if (resident_cells.has(cell) || !cells.has(cell) || accepted.has(cell)) {
			continue;
		}
		if (exhausted) {
			remaining++;
			continue;
		}
		Vector<EntityId> ids;
		Vector<EntityId> ancestors;
		Error error = _cell_entities(cell, ids, ancestors);
		if (error != OK) {
			return error;
		}
		Vector<EntityId> pending;
		for (EntityId id : ancestors) {
			if (!queued.has(id) && resolve(id).state != EntityReferenceState::RESIDENT) {
				pending.push_back(id);
			}
		}
		for (EntityId id : ids) {
			if (!queued.has(id) && resolve(id).state != EntityReferenceState::RESIDENT) {
				pending.push_back(id);
			}
		}
		if (p_max_entities > 0 && !batch.is_empty() && batch.size() + pending.size() > p_max_entities) {
			exhausted = true;
			remaining++;
			continue;
		}
		for (EntityId id : pending) {
			queued.insert(id);
			batch.push_back(id);
		}
		accepted.push_back(cell);
		accepted_ancestors.push_back(ancestors);
	}
	if (r_remaining) {
		*r_remaining = remaining;
	}
	if (accepted.is_empty()) {
		return OK;
	}
	Error error = batch.is_empty() ? OK : load_subset(batch);
	if (error != OK) {
		return error;
	}
	for (int i = 0; i < accepted.size(); i++) {
		error = pin(accepted_ancestors[i]);
		if (error != OK) {
			return error;
		}
		resident_cells.insert(accepted[i], accepted_ancestors[i]);
	}
	residency_serial++;
	return OK;
}

Error EntityScene::release_cells(const Vector<CellKey> &p_cells) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	Error result = OK;
	bool released = false;
	for (const CellKey &cell : p_cells) {
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
