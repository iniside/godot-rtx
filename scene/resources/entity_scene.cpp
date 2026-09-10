#ifndef _3D_DISABLED

#include "entity_scene.h"
#include "core/object/class_db.h"

#include "scene/entity/entity_scene_commands.h"
#include "scene/entity/entity_scene_io.h"

EntityScene::EntityScene() {
	Error error = EntityId::generate(document_id);
	ERR_FAIL_COND(error != OK);
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

Error EntityScene::_read_bytes(EntityId p_id, PackedByteArray &r_bytes) {
	const Section *section = sections.getptr(p_id);
	if (!section) {
		return _fail(p_id, "record", ERR_DOES_NOT_EXIST);
	}
	if (!section->bytes.is_empty()) {
		r_bytes = section->bytes;
		return OK;
	}
	if (source.is_null() && !storage_path.is_empty()) {
		source = FileAccess::open(storage_path, FileAccess::READ);
	}
	if (source.is_null() || !section->length || section->offset > source->get_length() || section->length > source->get_length() - section->offset) {
		return _fail(p_id, "section", ERR_FILE_CORRUPT);
	}
	source->seek(section->offset);
	r_bytes = source->get_buffer(section->length);
	return uint64_t(r_bytes.size()) == section->length ? OK : _fail(p_id, "section", ERR_FILE_CORRUPT);
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
				return _fail(p_id, String::num_uint64(schema.id, 16) + "/" + String::num_uint64(field.id, 16), error);
			}
			value[String::num_uint64(field.id, 16)] = field_value;
		}
		components[String::num_uint64(schema.id, 16)] = value;
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
		if (!p_prefer_stored || !section || (section->bytes.is_empty() && !section->length)) {
			error = get_commands()._prefab_record(p_id, r_record, prefab_record);
		}
		if (error != OK) {
			return error;
		}
		if (prefab_record) {
			return OK;
		}
		PackedByteArray bytes;
		error = _read_bytes(p_id, bytes);
		Variant record;
		if (error == OK) {
			error = EntitySceneIO::decode(bytes, record);
		}
		if (error == OK && record.get_type() != Variant::DICTIONARY) {
			error = ERR_FILE_CORRUPT;
		}
		if (error == OK) {
			r_record = record;
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

Error EntityScene::_validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, Array *r_dependencies, const String &p_prefix) {
	const EntityComponentSchema *schema = get_world()->schemas.find(p_type);
	String prefix = p_prefix.is_empty() ? String::num_uint64(p_type, 16) : p_prefix;
	if (!schema) {
		return _fail(p_id, prefix, ERR_UNAVAILABLE);
	}
	int expected = 0;
	for (const EntityFieldSchema &field : schema->fields) {
		if (!field.serialized) {
			continue;
		}
		expected++;
		String key = String::num_uint64(field.id, 16);
		String address = prefix + "/" + key;
		if (!p_fields.has(key)) {
			return _fail(p_id, address, ERR_DOES_NOT_EXIST);
		}
		Variant value = p_fields[key];
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
				Error error = _validate_fields(p_id, field.nested_type_id, entries[i], r_dependencies, address);
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
				if (asset.is_valid() && r_dependencies) {
					Dictionary dependency;
					String uid = assets[i];
					dependency["uid"] = uid.get_slice("::", 0);
					dependency["path"] = asset->get_path().get_slice("::", 0);
					dependency["type"] = ResourceLoader::get_resource_type(dependency["path"]);
					dependency["entity"] = p_id.to_string();
					dependency["field"] = address + "[" + itos(i) + "]";
					r_dependencies->push_back(dependency);
				}
			}
		}
	}
	if (expected != p_fields.size()) {
		for (const Variant &key : p_fields.get_key_list()) {
			bool found = false;
			for (const EntityFieldSchema &field : schema->fields) {
				found |= field.serialized && key == String::num_uint64(field.id, 16);
			}
			if (!found) {
				return _fail(p_id, prefix + "/" + String(key), ERR_INVALID_DATA);
			}
		}
	}
	return OK;
}

Error EntityScene::_describe(EntityId p_id, const Dictionary &p_record, Section &r_section) {
	if (!p_record.has("components") || p_record["components"].get_type() != Variant::DICTIONARY) {
		return _fail(p_id, "components", ERR_INVALID_DATA);
	}
	Dictionary components = p_record["components"];
	r_section.components.clear();
	r_section.dependencies.clear();
	for (const Variant &key : components.get_key_list()) {
		String text = key;
		uint64_t type = text.hex_to_int();
		const EntityComponentSchema *schema = get_world()->schemas.find(type);
		if (!schema || !schema->is_component || text != String::num_uint64(type, 16) || components[key].get_type() != Variant::DICTIONARY) {
			return _fail(p_id, text, ERR_INVALID_DATA);
		}
		Error error = _validate_fields(p_id, type, components[key], &r_section.dependencies);
		if (error != OK) {
			return error;
		}
		r_section.components.push_back(text);
	}
	return OK;
}

Error EntityScene::_install(EntityId p_id, const Dictionary &p_record) {
	Section section;
	Error error = _describe(p_id, p_record, section);
	if (error != OK) {
		return error;
	}
	EntityHandle handle;
	error = get_world()->load_entity(p_id, handle);
	if (error != OK) {
		return error;
	}
	Dictionary components = p_record["components"];
	for (const Variant &key : components.get_key_list()) {
		error = world->write_component(handle, String(key).hex_to_int(), components[key]);
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

void EntityScene::_commit(EntityScene &p_prepared, const Vector<EntityId> &p_ids, bool p_resident) {
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
			sections.insert(id, p_prepared.sections[id]);
		} else {
			sections.erase(id);
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
	revision++;
}

Error EntityScene::load_subset(const Vector<EntityId> &p_ids) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
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
	Ref<EntityScene> prepared;
	error = _prepare(unloaded, prepared);
	if (error != OK) {
		return error;
	}
	error = _can_commit(**prepared, unloaded);
	if (error != OK) {
		return error;
	}
	uint64_t previous_revision = revision;
	_commit(**prepared, unloaded, true);
	revision = previous_revision;
	if (get_resident_count() == sections.size()) {
		source.unref();
	}
	return OK;
}

Error EntityScene::unload_subset(const Vector<EntityId> &p_ids) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	HashSet<EntityId, EntityIdHasher> requested;
	HashMap<EntityId, Section, EntityIdHasher> saved;
	for (EntityId id : p_ids) {
		requested.insert(id);
		for (const KeyValue<EntityId, uint32_t> &pin : pins) {
			for (EntityId ancestor = pin.key; ancestor.is_valid(); ancestor = catalog.get_parent(ancestor).id) {
				if (ancestor == id) {
					return ERR_BUSY;
				}
			}
		}
		if (resolve(id).state != EntityReferenceState::RESIDENT) {
			return ERR_UNAVAILABLE;
		}
		Dictionary record;
		Error error = _encode_record(id, record);
		Section section;
		if (error == OK) {
			error = _describe(id, record, section);
		}
		if (error == OK) {
			error = EntitySceneIO::encode(record, section.bytes);
		}
		if (error != OK) {
			return error;
		}
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

Error EntityScene::create_play_document(Ref<EntityScene> &r_scene) {
	ERR_FAIL_COND_V(_owner() != OK, ERR_UNAUTHORIZED);
	Ref<EntityScene> result;
	result.instantiate();
	result->document_id = document_id;
	result->revision = revision;
	result->prefab_instances = prefab_instances.duplicate(true);
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
			if (error == OK) {
				error = EntitySceneIO::encode(record, section.bytes);
			}
			if (error != OK) {
				return error;
			}
			result->sections.insert(id, section);
		}
	}
	r_scene = result;
	return OK;
}

#endif
