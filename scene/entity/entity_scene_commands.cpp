#include "entity_scene_commands.h"

#include "entity_scene_io.h"

Error EntitySceneCommands::_snapshot(EntityScene &p_scene, const Vector<EntityId> &p_ids, Dictionary &r_records) {
	for (EntityId id : p_ids) {
		Dictionary record;
		if (p_scene.catalog.get_state(id) == EntityReferenceState::MISSING) {
			record["components"] = Dictionary();
			record["parent"] = EntityId().to_string();
			record["deleted"] = true;
			record["order"] = int64_t(0);
		} else {
			Error error = p_scene._read_record(id, record);
			if (error != OK) {
				return error;
			}
		}
		Array children;
		if (&p_scene != &document) {
			for (EntityId child : document.catalog.get_children(id)) {
				if (!p_ids.has(child)) {
					children.push_back(child.to_string());
				}
			}
		}
		for (EntityId child : p_scene.catalog.get_children(id)) {
			children.push_back(child.to_string());
		}
		children.sort();
		record["children"] = children;
		r_records[id.to_string()] = record.duplicate(true);
	}
	return OK;
}

Error EntitySceneCommands::_restore(const Dictionary &p_records, const Dictionary &p_prefabs) {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	Ref<EntityScene> prepared;
	prepared.instantiate();
	Vector<EntityId> ids;
	for (const Variant &key : p_records.get_key_list()) {
		EntityId id;
		EntityId parent;
		Dictionary record = p_records[key];
		if (EntityId::parse(key, id) != OK || EntityId::parse(record["parent"], parent) != OK) {
			return ERR_INVALID_DATA;
		}
		prepared->catalog.records.insert(id, { bool(record["deleted"]), { parent } });
		prepared->order.insert(id, record["order"]);
		ids.push_back(id);
	}
	Vector<EntityId> ordered;
	Error error = prepared->_collect_required(ids, ordered);
	if (error != OK) {
		return error;
	}
	for (EntityId id : ordered) {
		if (prepared->catalog.records[id].deleted) {
			continue;
		}
		prepared->catalog._set_parent(id, prepared->catalog.get_parent(id));
		error = prepared->_install(id, p_records[id.to_string()]);
		if (error != OK) {
			document.last_error = prepared->last_error;
			return error;
		}
	}
	error = document._can_commit(**prepared, ordered);
	if (error != OK) {
		return error;
	}
	document.prefab_instances = p_prefabs.duplicate(true);
	document._commit(**prepared, ordered, true);
	document.emit_changed();
	return OK;
}

void EntitySceneCommands::_push(History p_history) {
	history.resize(cursor);
	history.push_back(p_history);
	cursor++;
}

Error EntitySceneCommands::_remap_fields(uint64_t p_type, Dictionary &r_fields, const Dictionary &p_remap) {
	const EntityComponentSchema *schema = document.get_world()->schemas.find(p_type);
	ERR_FAIL_NULL_V(schema, ERR_INVALID_DATA);
	for (const EntityFieldSchema &field : schema->fields) {
		String key = String::num_uint64(field.id, 16);
		if (!field.serialized || !r_fields.has(key)) {
			continue;
		}
		Variant value = r_fields[key];
		bool array = value.get_type() == Variant::ARRAY;
		Array elements = array ? Array(value) : Array{value};
		for (int i = 0; i < elements.size(); i++) {
			if (field.entity_reference && p_remap.has(elements[i])) {
				elements[i] = p_remap[elements[i]];
			} else if (field.nested_type_id) {
				Dictionary fields = elements[i];
				Error error = _remap_fields(field.nested_type_id, fields, p_remap);
				if (error != OK) {
					return error;
				}
				elements[i] = fields;
			}
		}
		r_fields[key] = array ? Variant(elements) : elements[0];
	}
	return OK;
}

Error EntitySceneCommands::_remap_record(Dictionary &r_record, const Dictionary &p_remap) {
	if (p_remap.has(r_record["parent"])) {
		r_record["parent"] = p_remap[r_record["parent"]];
	}
	Dictionary components = r_record["components"];
	for (const Variant &key : components.get_key_list()) {
		Dictionary fields = components[key];
		Error error = _remap_fields(String(key).hex_to_int(), fields, p_remap);
		if (error != OK) {
			return error;
		}
		components[key] = fields;
	}
	r_record["components"] = components;
	return OK;
}

Error EntitySceneCommands::_apply(EntityScene &p_scene, const Command &p_command, Vector<EntityId> &r_changed, Dictionary &r_remap) {
	EntityWorld *world = p_scene.get_world();
	EntityId id = p_command.entity;
	EntityResolution target = world->resolve({ id });
	Error error = OK;
	switch (p_command.kind) {
		case CREATE: {
			ERR_FAIL_COND_V(!id.is_valid() || target.state != EntityReferenceState::MISSING, ERR_ALREADY_EXISTS);
			ERR_FAIL_COND_V(p_command.after.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER);
			Dictionary record = Dictionary(p_command.after).duplicate(true);
			p_scene.catalog.records.insert(id, { false, { p_command.parent } });
			p_scene.catalog._set_parent(id, { p_command.parent });
			error = p_scene._install(id, record);
			r_changed.push_back(id);
		} break;
		case DELETE: {
			ERR_FAIL_COND_V(target.state == EntityReferenceState::MISSING || target.state == EntityReferenceState::DELETED, ERR_DOES_NOT_EXIST);
			error = world->delete_hierarchy(id);
		} break;
		case DUPLICATE: {
			ERR_FAIL_COND_V(target.state != EntityReferenceState::RESIDENT, ERR_UNAVAILABLE);
			Vector<EntityId> originals;
			originals.push_back(id);
			for (int i = 0; i < originals.size(); i++) {
				originals.append_array(p_scene.catalog.get_children(originals[i]));
			}
			Dictionary remap = r_remap.duplicate(true);
			for (EntityId original : originals) {
				if (remap.has(original.to_string())) {
					continue;
				}
				EntityId copy;
				error = EntityId::generate(copy);
				if (error != OK) {
					return error;
				}
				remap[original.to_string()] = copy.to_string();
				r_remap[original.to_string()] = copy.to_string();
			}
			for (EntityId original : originals) {
				Dictionary record;
				error = p_scene._read_record(original, record);
				if (error != OK) {
					return error;
				}
				error = _remap_record(record, remap);
				if (error != OK) {
					return error;
				}
				EntityId copy;
				EntityId parent;
				EntityId::parse(remap[original.to_string()], copy);
				EntityId::parse(record["parent"], parent);
				p_scene.catalog.records.insert(copy, { false, { parent } });
				p_scene.catalog._set_parent(copy, { parent });
				p_scene.order.insert(copy, record["order"]);
				error = p_scene._install(copy, record);
				if (error != OK) {
					return error;
				}
				r_changed.push_back(copy);
			}
		} break;
		case REPARENT: {
			error = world->reparent(target.handle, { p_command.parent }, p_command.reparent_mode);
		} break;
		case SET_FIELD: {
			Variant before;
			error = world->read_field(target.handle, p_command.component, p_command.field, before);
			if (error != OK) {
				return error;
			}
			ERR_FAIL_COND_V(before != p_command.before, ERR_BUSY);
			error = world->write_field(target.handle, p_command.component, p_command.field, p_command.after);
		} break;
		case ADD_COMPONENT: {
			error = world->add_component(target.handle, p_command.component);
			if (error == OK && p_command.after.get_type() != Variant::NIL) {
				error = world->write_component(target.handle, p_command.component, p_command.after);
			}
		} break;
		case REMOVE_COMPONENT: {
			Variant before;
			error = world->read_component(target.handle, p_command.component, before);
			if (error != OK) {
				return error;
			}
			ERR_FAIL_COND_V(before != p_command.before, ERR_BUSY);
			error = world->remove_component(target.handle, p_command.component);
		} break;
		case SET_ORDER: {
			ERR_FAIL_COND_V(target.state != EntityReferenceState::RESIDENT || p_command.after.get_type() != Variant::INT, ERR_INVALID_PARAMETER);
			ERR_FAIL_COND_V(Variant(p_scene.get_order(id)) != p_command.before, ERR_BUSY);
			p_scene.order.insert(id, p_command.after);
		} break;
	}
	return error;
}

Error EntitySceneCommands::execute(const String &p_name, const Vector<Command> &p_commands, Dictionary *r_remap) {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_commands.is_empty(), ERR_INVALID_PARAMETER);
	Vector<EntityId> needed;
	HashSet<EntityId, EntityIdHasher> creating;
	for (const Command &command : p_commands) {
		ERR_FAIL_COND_V(command.document != document.document_id, ERR_INVALID_PARAMETER);
		if (command.kind == CREATE) {
			creating.insert(command.entity);
		}
	}
	for (const Command &command : p_commands) {
		if (command.parent.is_valid() && !creating.has(command.parent)) {
			needed.push_back(command.parent);
		}
		if (creating.has(command.entity)) {
			continue;
		}
		needed.push_back(command.entity);
		if (command.kind == DELETE || command.kind == DUPLICATE || (command.kind == REPARENT && command.reparent_mode == EntityWorld::KEEP_WORLD)) {
			Vector<EntityId> descendants;
			descendants.push_back(command.entity);
			for (int i = 0; i < descendants.size(); i++) {
				EntityId id = descendants[i];
				needed.push_back(id);
				if (command.kind == REPARENT) {
					EntityResolution resolution = document.resolve(id);
					bool transformed = resolution.state == EntityReferenceState::RESIDENT ? document.world->has<EntityTransform>(resolution.handle) : document.sections.has(id) && document.sections[id].components.has(String::num_uint64(EntityComponentTraits<EntityTransform>::id, 16));
					if (transformed) {
						continue;
					}
				}
				descendants.append_array(document.catalog.get_children(id));
			}
		}
	}
	Ref<EntityScene> prepared;
	Error error = document._prepare(needed, prepared);
	if (error != OK) {
		return error;
	}
	prepared->prefab_instances = document.prefab_instances.duplicate(true);
	Vector<EntityId> changed = prepared->catalog.get_ids();
	Dictionary remap;
	for (const Command &command : p_commands) {
		if (command.kind != DUPLICATE) {
			continue;
		}
		Vector<EntityId> originals;
		originals.push_back(command.entity);
		for (int i = 0; i < originals.size(); i++) {
			originals.append_array(prepared->catalog.get_children(originals[i]));
			String key = originals[i].to_string();
			ERR_FAIL_COND_V(remap.has(key), ERR_INVALID_PARAMETER);
			EntityId copy;
			error = EntityId::generate(copy);
			if (error != OK) {
				return error;
			}
			remap[key] = copy.to_string();
		}
	}
	for (const Command &command : p_commands) {
		Dictionary poses_before;
		if (command.kind == REPARENT && command.reparent_mode == EntityWorld::KEEP_WORLD) {
			for (EntityId id : prepared->catalog.get_ids()) {
				EntityResolution resolution = prepared->resolve(id);
				if (resolution.state == EntityReferenceState::RESIDENT && prepared->world->has<EntityTransform>(resolution.handle)) {
					Variant pose;
					prepared->world->read_field(resolution.handle, EntityComponentTraits<EntityTransform>::id, 1, pose);
					poses_before[id.to_string()] = pose;
				}
			}
		}
		error = _apply(**prepared, command, changed, remap);
		if (error != OK) {
			document.last_error = prepared->last_error;
			return error;
		}
		for (const Variant &key : prepared->prefab_instances.get_key_list()) {
			Dictionary instance = prepared->prefab_instances[key];
			Dictionary mapping = instance["mapping"];
			Array overrides = instance["overrides"];
			if (command.kind == DUPLICATE) {
				Vector<EntityId> originals;
				originals.push_back(command.entity);
				for (int i = 0; i < originals.size(); i++) {
					originals.append_array(prepared->catalog.get_children(originals[i]));
					String original = originals[i].to_string();
					bool member = false;
					for (const Variant &source : mapping.get_key_list()) {
						member |= mapping[source] == original;
					}
					if (!member) {
						continue;
					}
					String copy_key = remap[original];
					EntityId copy;
					EntityId::parse(copy_key, copy);
					Dictionary record;
					error = prepared->_read_record(copy, record);
					if (error != OK) {
						return error;
					}
					Dictionary override;
					override["source"] = copy_key;
					override["kind"] = int(CREATE);
					override["component"] = "0";
					override["field"] = "0";
					override["before"] = Variant();
					override["after"] = record;
					override["parent"] = record["parent"];
					overrides.push_back(override);
					mapping[copy_key] = copy_key;
				}
				instance["mapping"] = mapping;
				instance["overrides"] = overrides;
				prepared->prefab_instances[key] = instance;
				continue;
			}
			String source_id;
			for (const Variant &source : mapping.get_key_list()) {
				if (mapping[source] == command.entity.to_string()) {
					source_id = source;
					break;
				}
			}
			if (source_id.is_empty() && command.kind == CREATE) {
				for (const Variant &source : mapping.get_key_list()) {
					if (mapping[source] == command.parent.to_string()) {
						source_id = command.entity.to_string();
						mapping[source_id] = command.entity.to_string();
						instance["mapping"] = mapping;
						break;
					}
				}
			}
			if (source_id.is_empty()) {
				continue;
			}
			Dictionary override;
			override["source"] = source_id;
			override["kind"] = int(command.kind);
			override["component"] = String::num_uint64(command.component, 16);
			override["field"] = String::num_uint64(command.field, 16);
			override["before"] = command.before.duplicate(true);
			override["after"] = command.after.duplicate(true);
			override["parent"] = command.parent.to_string();
			overrides.push_back(override);
			for (const Variant &local_id : poses_before.get_key_list()) {
				String source_key;
				for (const Variant &source : mapping.get_key_list()) {
					if (mapping[source] == local_id) {
						source_key = source;
						break;
					}
				}
				if (source_key.is_empty()) {
					continue;
				}
				EntityId id;
				EntityId::parse(local_id, id);
				Variant pose;
				prepared->world->read_field(prepared->resolve(id).handle, EntityComponentTraits<EntityTransform>::id, 1, pose);
				if (pose != poses_before[local_id]) {
					Dictionary local_override;
					local_override["source"] = source_key;
					local_override["kind"] = int(SET_FIELD);
					local_override["component"] = String::num_uint64(EntityComponentTraits<EntityTransform>::id, 16);
					local_override["field"] = "1";
					local_override["before"] = poses_before[local_id];
					local_override["after"] = pose;
					local_override["parent"] = EntityId().to_string();
					overrides.push_back(local_override);
				}
			}
			instance["overrides"] = overrides;
			prepared->prefab_instances[key] = instance;
		}
	}
	History item;
	item.name = p_name;
	item.prefabs_before = document.prefab_instances.duplicate(true);
	item.prefabs_after = prepared->prefab_instances.duplicate(true);
	error = _snapshot(document, changed, item.before);
	if (error == OK) {
		error = _snapshot(**prepared, changed, item.after);
	}
	if (error != OK) {
		return error;
	}
	for (EntityId id : changed) {
		if (prepared->catalog.get_state(id) != EntityReferenceState::DELETED) {
			EntityScene::Section section;
			error = prepared->_describe(id, item.after[id.to_string()], section);
			if (error != OK) {
				return error;
			}
			prepared->sections.insert(id, section);
		}
	}
	error = document._can_commit(**prepared, changed);
	if (error != OK) {
		return error;
	}
	document.prefab_instances = prepared->prefab_instances;
	document._commit(**prepared, changed, true);
	_push(item);
	document.emit_changed();
	if (r_remap) {
		*r_remap = remap;
	}
	return OK;
}

Error EntitySceneCommands::undo() {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!can_undo(), ERR_UNAVAILABLE);
	const History &item = history[cursor - 1];
	Vector<EntityId> ids;
	for (const Variant &key : item.after.get_key_list()) {
		EntityId id;
		EntityId::parse(key, id);
		ids.push_back(id);
	}
	Dictionary current;
	Error snapshot_error = _snapshot(document, ids, current);
	if (snapshot_error != OK) {
		return snapshot_error;
	}
	ERR_FAIL_COND_V(current != item.after || document.prefab_instances != item.prefabs_after, ERR_BUSY);
	Error error = _restore(item.before, item.prefabs_before);
	if (error == OK) {
		cursor--;
	}
	return error;
}

Error EntitySceneCommands::redo() {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!can_redo(), ERR_UNAVAILABLE);
	const History &item = history[cursor];
	Vector<EntityId> ids;
	for (const Variant &key : item.before.get_key_list()) {
		EntityId id;
		EntityId::parse(key, id);
		ids.push_back(id);
	}
	Dictionary current;
	Error snapshot_error = _snapshot(document, ids, current);
	if (snapshot_error != OK) {
		return snapshot_error;
	}
	ERR_FAIL_COND_V(current != item.before || document.prefab_instances != item.prefabs_before, ERR_BUSY);
	Error error = _restore(item.after, item.prefabs_after);
	if (error == OK) {
		cursor++;
	}
	return error;
}

void EntitySceneCommands::clear() {
	ERR_FAIL_COND(document._owner() != OK);
	history.clear();
	cursor = 0;
}

Error EntitySceneCommands::_override_record(Dictionary &r_record, const Dictionary &p_instance, const String &p_source) {
	Dictionary components = r_record["components"];
	for (const Variant &value : Array(p_instance["overrides"])) {
		Dictionary override = value;
		if (override["source"] != p_source) {
			continue;
		}
		String component = override["component"];
		String field = override["field"];
		switch (int(override["kind"])) {
			case SET_FIELD: {
				if (!components.has(component)) {
					return ERR_BUSY;
				}
				Dictionary fields = components[component];
				if (!fields.has(field)) {
					return ERR_BUSY;
				}
				fields[field] = override["after"].duplicate(true);
				components[component] = fields;
			} break;
			case ADD_COMPONENT: {
				if (override["after"].get_type() == Variant::NIL) {
					const EntityComponentSchema *schema = document.get_world()->schemas.find(component.hex_to_int());
					ERR_FAIL_NULL_V(schema, ERR_UNAVAILABLE);
					Dictionary fields;
					for (const EntityFieldSchema &entry : schema->fields) {
						if (entry.serialized) {
							fields[String::num_uint64(entry.id, 16)] = entry.default_value.duplicate(true);
						}
					}
					components[component] = fields;
				} else {
					components[component] = override["after"].duplicate(true);
				}
			} break;
			case REMOVE_COMPONENT: {
				components.erase(component);
			} break;
			case REPARENT: {
				r_record["parent"] = override["parent"];
			} break;
			case SET_ORDER: {
				r_record["order"] = override["after"];
			} break;
			case DELETE: {
				r_record["deleted"] = true;
			} break;
			case CREATE: {
				components = Dictionary(override["after"])["components"];
				r_record["parent"] = override["parent"];
				r_record["deleted"] = false;
			} break;
			default: {
				return ERR_INVALID_DATA;
			}
		}
	}
	r_record["components"] = components;
	return OK;
}

Error EntitySceneCommands::_prefab_record(EntityId p_id, Dictionary &r_record, bool &r_found) {
	r_found = false;
	for (const Variant &key : document.prefab_instances.get_key_list()) {
		Dictionary instance = document.prefab_instances[key];
		Dictionary mapping = instance["mapping"];
		String source_id;
		for (const Variant &source : mapping.get_key_list()) {
			if (mapping[source] == p_id.to_string()) {
				source_id = source;
				break;
			}
		}
		if (source_id.is_empty()) {
			continue;
		}
		Ref<Resource> resource;
		Error error = entity_decode_asset(instance["uid"], resource);
		Ref<EntityScene> prefab = resource;
		if (error != OK || prefab.is_null() || prefab->document_id == document.document_id) {
			return document._fail(p_id, "prefab/source", error == OK ? ERR_CYCLIC_LINK : error);
		}
		if (int64_t(instance["revision"]) == int64_t(prefab->revision)) {
			return OK;
		}
		EntityId source;
		EntityId::parse(source_id, source);
		EntityReferenceState state = prefab->catalog.get_state(source);
		if (state == EntityReferenceState::DELETED || state == EntityReferenceState::MISSING) {
			bool added = false;
			bool edited = false;
			for (const Variant &value : Array(instance["overrides"])) {
				Dictionary override = value;
				if (override["source"] == source_id) {
					added |= int(override["kind"]) == CREATE;
					edited |= int(override["kind"]) != DELETE;
				}
			}
			if (!added) {
				return document._fail(p_id, edited ? "prefab/removed_source_with_local_edits" : "prefab/removed_source", ERR_BUSY);
			}
			r_record["components"] = Dictionary();
			r_record["parent"] = EntityId().to_string();
			r_record["deleted"] = false;
			r_record["order"] = int64_t(0);
		} else {
			error = prefab->_read_record(source, r_record);
			if (error != OK) {
				return error;
			}
			error = _remap_record(r_record, mapping);
			if (error != OK) {
				return error;
			}
		}
		error = _override_record(r_record, instance, source_id);
		if (error != OK) {
			return document._fail(p_id, "prefab/override", error);
		}
		r_found = true;
		return OK;
	}
	return OK;
}

Error EntitySceneCommands::_reconcile_prefab_catalog() {
	for (const Variant &key : document.prefab_instances.get_key_list()) {
		if (document.prefab_instances[key].get_type() != Variant::DICTIONARY) {
			return ERR_FILE_CORRUPT;
		}
		Dictionary instance = document.prefab_instances[key];
		if (!instance.has_all(Array{"uid", "path", "document", "revision", "mapping", "overrides", "conflicts"}) || instance["mapping"].get_type() != Variant::DICTIONARY || instance["overrides"].get_type() != Variant::ARRAY) {
			return ERR_FILE_CORRUPT;
		}
		Ref<Resource> resource;
		Error error = entity_decode_asset(instance["uid"], resource);
		Ref<EntityScene> prefab = resource;
		if (error != OK || prefab.is_null() || prefab->document_id == document.document_id || instance["document"] != prefab->document_id.to_string()) {
			return document._fail(EntityId(), "prefab/source", error == OK ? ERR_INVALID_DATA : error);
		}
		Dictionary mapping = instance["mapping"];
		for (const Variant &source_key : mapping.get_key_list()) {
			EntityId source_id;
			EntityId id;
			if (EntityId::parse(source_key, source_id) != OK || !source_id.is_valid() || EntityId::parse(mapping[source_key], id) != OK || !id.is_valid() || document.catalog.get_state(id) == EntityReferenceState::MISSING) {
				return document._fail(id, "prefab/mapping", ERR_INVALID_DATA);
			}
		}
		for (const Variant &value : Array(instance["overrides"])) {
			if (value.get_type() != Variant::DICTIONARY || !Dictionary(value).has_all(Array{"source", "kind", "component", "field", "before", "after", "parent"})) {
				return ERR_FILE_CORRUPT;
			}
		}
		if (int64_t(instance["revision"]) == int64_t(prefab->revision)) {
			continue;
		}
		for (EntityId source_id : prefab->catalog.get_ids()) {
			if (prefab->catalog.get_state(source_id) == EntityReferenceState::DELETED || mapping.has(source_id.to_string())) {
				continue;
			}
			EntityId id;
			error = EntityId::generate(id);
			if (error != OK) {
				return error;
			}
			mapping[source_id.to_string()] = id.to_string();
			document.catalog.records.insert(id, {});
			EntityScene::Section section;
			section.components = prefab->sections[source_id].components;
			section.dependencies = prefab->sections[source_id].dependencies.duplicate(true);
			for (int i = 0; i < section.dependencies.size(); i++) {
				Dictionary dependency = section.dependencies[i];
				dependency["entity"] = id.to_string();
			}
			document.sections.insert(id, section);
		}
		Array conflicts;
		for (const Variant &source_key : mapping.get_key_list()) {
			EntityId source_id;
			EntityId id;
			EntityId::parse(source_key, source_id);
			EntityId::parse(mapping[source_key], id);
			bool removed = prefab->catalog.get_state(source_id) == EntityReferenceState::MISSING || prefab->catalog.get_state(source_id) == EntityReferenceState::DELETED;
			bool added = false;
			bool edited = false;
			bool explicitly_deleted = false;
			EntityId parent = prefab->catalog.get_parent(source_id).id;
			String parent_key = parent.to_string();
			EntityId::parse(mapping.get(parent_key, parent_key), parent);
			int64_t order = prefab->get_order(source_id);
			for (const Variant &value : Array(instance["overrides"])) {
				Dictionary override = value;
				if (override["parent"] == id.to_string() && (int(override["kind"]) == CREATE || int(override["kind"]) == REPARENT)) {
					edited = true;
				}
				if (override["source"] != source_key) {
					continue;
				}
				int kind = override["kind"];
				added |= kind == CREATE;
				edited |= kind != DELETE;
				explicitly_deleted |= kind == DELETE;
				if (kind == REPARENT || kind == CREATE) {
					EntityId::parse(override["parent"], parent);
				}
				if (kind == SET_ORDER) {
					order = override["after"];
				}
			}
			if (removed && edited && !added) {
				Dictionary conflict;
				conflict["source"] = source_key;
				conflict["entity"] = id.to_string();
				conflict["reason"] = "Source element removed with local edits";
				conflicts.push_back(conflict);
				continue;
			}
			document.catalog._unlink_parent(id);
			document.catalog.records[id].deleted = explicitly_deleted || (removed && !added);
			document.catalog.records[id].parent = { parent };
			document.order.insert(id, order);
			if (!document.catalog.records[id].deleted) {
				document.catalog._set_parent(id, { parent });
			}
		}
		instance["mapping"] = mapping;
		instance["conflicts"] = conflicts;
		document.prefab_instances[key] = instance;
	}
	Vector<EntityId> ordered;
	return document._collect_required(document.catalog.get_ids(), ordered);
}

Error EntitySceneCommands::_refresh_instance(EntityScene &p_target, EntityId p_instance, EntityScene &p_source, Vector<EntityId> &r_changed) {
	String key = p_instance.to_string();
	ERR_FAIL_COND_V(!p_target.prefab_instances.has(key), ERR_DOES_NOT_EXIST);
	Dictionary instance = Dictionary(p_target.prefab_instances[key]).duplicate(true);
	Dictionary mapping = instance["mapping"];
	Dictionary records;
	Array conflicts;
	for (EntityId source : p_source.catalog.get_ids()) {
		if (p_source.catalog.get_state(source) == EntityReferenceState::DELETED) {
			continue;
		}
		String source_key = source.to_string();
		if (!mapping.has(source_key)) {
			EntityId id;
			Error error = EntityId::generate(id);
			if (error != OK) {
				return error;
			}
			mapping[source_key] = id.to_string();
		}
	}
	instance["mapping"] = mapping;
	for (const Variant &source_key : mapping.get_key_list()) {
		EntityId source;
		EntityId id;
		EntityId::parse(source_key, source);
		EntityId::parse(mapping[source_key], id);
		Dictionary record;
		EntityReferenceState state = p_source.catalog.get_state(source);
		bool removed = state == EntityReferenceState::MISSING || state == EntityReferenceState::DELETED;
		if (removed) {
			bool added = false;
			bool edited = false;
			for (const Variant &value : Array(instance["overrides"])) {
				Dictionary override = value;
				if (override["parent"] == id.to_string() && (int(override["kind"]) == CREATE || int(override["kind"]) == REPARENT)) {
					edited = true;
				}
				if (override["source"] == source_key) {
					added |= int(override["kind"]) == CREATE;
					edited |= int(override["kind"]) != DELETE;
				}
			}
			if (edited && !added) {
				Dictionary conflict;
				conflict["source"] = source_key;
				conflict["entity"] = id.to_string();
				conflict["reason"] = "Source element removed with local edits";
				conflicts.push_back(conflict);
				continue;
			}
			record["components"] = Dictionary();
			record["parent"] = EntityId().to_string();
			record["deleted"] = !added;
			record["order"] = int64_t(0);
		} else {
			Error error = p_source._read_record(source, record);
			if (error != OK) {
				return error;
			}
			error = _remap_record(record, mapping);
			if (error != OK) {
				return error;
			}
		}
		Error error = _override_record(record, instance, source_key);
		if (error != OK) {
			return document._fail(id, "prefab/override", error);
		}
		records[id.to_string()] = record;
	}
	instance["conflicts"] = conflicts;
	p_target.prefab_instances[key] = instance;
	if (!conflicts.is_empty()) {
		return ERR_BUSY;
	}
	bool deleted_child = true;
	while (deleted_child) {
		deleted_child = false;
		for (const Variant &id : records.get_key_list()) {
			Dictionary record = records[id];
			if (!bool(record["deleted"]) && records.has(record["parent"]) && bool(Dictionary(records[record["parent"]])["deleted"])) {
				record["deleted"] = true;
				deleted_child = true;
			}
		}
	}
	for (const Variant &key_id : records.get_key_list()) {
		EntityId id;
		EntityId::parse(key_id, id);
		if (p_target.resolve(id).state == EntityReferenceState::RESIDENT) {
			EntityHandle handle = p_target.resolve(id).handle;
			p_target.world->ecs.entity(handle.entity).remove<flecs::Parent>();
		}
	}
	for (const Variant &key_id : records.get_key_list()) {
		EntityId id;
		EntityId parent;
		EntityId::parse(key_id, id);
		Dictionary record = records[key_id];
		EntityId::parse(record["parent"], parent);
		if (p_target.resolve(id).state == EntityReferenceState::RESIDENT) {
			EntityHandle handle = p_target.resolve(id).handle;
			p_target.world->transforms.forget(handle.entity);
			p_target.world->residents.erase(id);
			p_target.world->ecs.entity(handle.entity).destruct();
		}
		if (p_target.catalog.records.has(id)) {
			p_target.catalog._unlink_parent(id);
		}
		p_target.catalog.records.insert(id, { bool(record["deleted"]), { parent } });
		p_target.order.insert(id, record["order"]);
		if (!r_changed.has(id)) {
			r_changed.push_back(id);
		}
	}
	Vector<EntityId> ordered;
	Error error = p_target._collect_required(r_changed, ordered);
	if (error != OK) {
		return error;
	}
	for (EntityId id : ordered) {
		if (!records.has(id.to_string()) || p_target.catalog.records[id].deleted) {
			continue;
		}
		p_target.catalog._set_parent(id, p_target.catalog.get_parent(id));
		error = p_target._install(id, records[id.to_string()]);
		if (error != OK) {
			return error;
		}
		error = EntitySceneIO::encode(records[id.to_string()], p_target.sections[id].bytes);
		if (error != OK) {
			return error;
		}
	}
	instance["revision"] = int64_t(p_source.revision);
	p_target.prefab_instances[key] = instance;
	return OK;
}

Error EntitySceneCommands::instantiate_prefab(const Ref<EntityScene> &p_prefab, EntityId &r_instance) {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_prefab.is_null() || p_prefab->document_id == document.document_id, ERR_CYCLIC_LINK);
	Variant asset;
	Error error = entity_encode_asset(p_prefab, asset);
	if (error != OK) {
		return error;
	}
	EntityId instance_id;
	error = EntityId::generate(instance_id);
	if (error != OK) {
		return error;
	}
	Ref<EntityScene> prepared;
	prepared.instantiate();
	prepared->prefab_instances = document.prefab_instances.duplicate(true);
	Dictionary instance;
	instance["uid"] = asset;
	instance["path"] = p_prefab->get_path();
	instance["document"] = p_prefab->document_id.to_string();
	instance["revision"] = int64_t(p_prefab->revision);
	instance["mapping"] = Dictionary();
	instance["overrides"] = Array();
	instance["conflicts"] = Array();
	prepared->prefab_instances[instance_id.to_string()] = instance;
	Vector<EntityId> changed;
	error = _refresh_instance(**prepared, instance_id, **p_prefab, changed);
	if (error != OK) {
		return error;
	}
	History item;
	item.name = "Instantiate prefab";
	item.prefabs_before = document.prefab_instances.duplicate(true);
	item.prefabs_after = prepared->prefab_instances.duplicate(true);
	error = _snapshot(document, changed, item.before);
	if (error == OK) {
		error = _snapshot(**prepared, changed, item.after);
	}
	if (error != OK) {
		return error;
	}
	error = document._can_commit(**prepared, changed);
	if (error != OK) {
		return error;
	}
	document.prefab_instances = prepared->prefab_instances;
	document._commit(**prepared, changed, true);
	_push(item);
	document.emit_changed();
	r_instance = instance_id;
	return OK;
}

Error EntitySceneCommands::refresh_prefab(EntityId p_instance, const Ref<EntityScene> &p_prefab) {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_prefab.is_null() || p_prefab->document_id == document.document_id, ERR_CYCLIC_LINK);
	String key = p_instance.to_string();
	ERR_FAIL_COND_V(!document.prefab_instances.has(key), ERR_DOES_NOT_EXIST);
	Dictionary instance = document.prefab_instances[key];
	ERR_FAIL_COND_V(instance["document"] != p_prefab->document_id.to_string(), ERR_INVALID_PARAMETER);
	Vector<EntityId> ids;
	Dictionary mapping = instance["mapping"];
	for (const Variant &source : mapping.get_key_list()) {
		EntityId id;
		EntityId::parse(mapping[source], id);
		ids.push_back(id);
	}
	Dictionary saved_prefabs = document.prefab_instances;
	document.prefab_instances = Dictionary();
	Ref<EntityScene> prepared;
	Error error = document._prepare(ids, prepared);
	document.prefab_instances = saved_prefabs;
	if (error != OK) {
		return error;
	}
	prepared->prefab_instances = saved_prefabs.duplicate(true);
	Vector<EntityId> changed = prepared->catalog.get_ids();
	error = _refresh_instance(**prepared, p_instance, **p_prefab, changed);
	if (error == ERR_BUSY) {
		Dictionary conflict_instance = prepared->prefab_instances[key];
		instance["conflicts"] = conflict_instance["conflicts"];
		document.prefab_instances[key] = instance;
		document.emit_changed();
		return error;
	}
	if (error != OK) {
		return error;
	}
	History item;
	item.name = "Refresh prefab";
	item.prefabs_before = saved_prefabs.duplicate(true);
	item.prefabs_after = prepared->prefab_instances.duplicate(true);
	document.prefab_instances = Dictionary();
	error = _snapshot(document, changed, item.before);
	document.prefab_instances = saved_prefabs;
	if (error == OK) {
		error = _snapshot(**prepared, changed, item.after);
	}
	if (error != OK) {
		return error;
	}
	for (EntityId id : changed) {
		if (prepared->catalog.get_state(id) != EntityReferenceState::DELETED && prepared->sections[id].bytes.is_empty()) {
			error = EntitySceneIO::encode(item.after[id.to_string()], prepared->sections[id].bytes);
			if (error != OK) {
				return error;
			}
		}
	}
	error = document._can_commit(**prepared, changed);
	if (error != OK) {
		return error;
	}
	document.prefab_instances = prepared->prefab_instances;
	document._commit(**prepared, changed, false);
	_push(item);
	document.emit_changed();
	return OK;
}

Error EntitySceneCommands::revert_override(EntityId p_instance, int p_override, const Ref<EntityScene> &p_prefab) {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	String key = p_instance.to_string();
	ERR_FAIL_COND_V(!document.prefab_instances.has(key), ERR_DOES_NOT_EXIST);
	Dictionary before = document.prefab_instances.duplicate(true);
	Dictionary instance = Dictionary(document.prefab_instances[key]).duplicate(true);
	Array overrides = instance["overrides"];
	ERR_FAIL_INDEX_V(p_override, overrides.size(), ERR_INVALID_PARAMETER);
	overrides.remove_at(p_override);
	instance["overrides"] = overrides;
	document.prefab_instances[key] = instance;
	Error error = refresh_prefab(p_instance, p_prefab);
	if (error != OK) {
		document.prefab_instances = before;
		return error;
	}
	history.write[cursor - 1].prefabs_before = before;
	history.write[cursor - 1].name = "Revert prefab override";
	return OK;
}

Error EntitySceneCommands::apply_overrides(EntityId p_instance, const Ref<EntityScene> &p_prefab, const Vector<Ref<EntityScene>> &p_users) {
	ERR_FAIL_COND_V(document._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_prefab.is_null() || p_prefab->document_id == document.document_id, ERR_CYCLIC_LINK);
	ERR_FAIL_COND_V(p_prefab->_owner() != OK, ERR_UNAUTHORIZED);
	String key = p_instance.to_string();
	ERR_FAIL_COND_V(!document.prefab_instances.has(key), ERR_DOES_NOT_EXIST);
	Dictionary instance = document.prefab_instances[key];
	ERR_FAIL_COND_V(instance["document"] != p_prefab->document_id.to_string(), ERR_INVALID_PARAMETER);
	Dictionary mapping = instance["mapping"];
	Dictionary reverse;
	Vector<EntityId> source_ids;
	for (const Variant &source : mapping.get_key_list()) {
		reverse[mapping[source]] = source;
		EntityId id;
		EntityId::parse(source, id);
		if (p_prefab->catalog.get_state(id) != EntityReferenceState::MISSING) {
			source_ids.push_back(id);
		}
	}
	Ref<EntityScene> source_prepared;
	Error error = p_prefab->_prepare(source_ids, source_prepared);
	if (error != OK) {
		return error;
	}
	source_prepared->prefab_instances = p_prefab->prefab_instances.duplicate(true);
	Vector<EntityId> source_changed = source_prepared->catalog.get_ids();
	for (const Variant &value : Array(instance["overrides"])) {
		Dictionary override = value;
		Command command;
		command.document = p_prefab->document_id;
		EntityId::parse(override["source"], command.entity);
		command.kind = Kind(int(override["kind"]));
		command.component = String(override["component"]).hex_to_int();
		command.field = String(override["field"]).hex_to_int();
		command.before = override["before"].duplicate(true);
		command.after = override["after"].duplicate(true);
		String parent = override["parent"];
		EntityId::parse(reverse.get(parent, parent), command.parent);
		if (command.kind == SET_FIELD) {
			const EntityComponentSchema *schema = p_prefab->get_world()->schemas.find(command.component);
			const EntityFieldSchema *field = schema ? schema->find_field(command.field) : nullptr;
			ERR_FAIL_NULL_V(field, ERR_INVALID_DATA);
			Dictionary fields;
			fields[String::num_uint64(command.field, 16)] = command.after;
			error = _remap_fields(command.component, fields, reverse);
			if (error != OK) {
				return error;
			}
			command.after = fields[String::num_uint64(command.field, 16)];
			error = source_prepared->get_world()->read_field(source_prepared->resolve(command.entity).handle, command.component, command.field, command.before);
		} else if (command.kind == CREATE) {
			Dictionary record = command.after;
			record["parent"] = parent;
			error = _remap_record(record, reverse);
			command.after = record;
		} else if (command.kind == REMOVE_COMPONENT) {
			error = source_prepared->get_world()->read_component(source_prepared->resolve(command.entity).handle, command.component, command.before);
		} else if (command.kind == ADD_COMPONENT && command.after.get_type() == Variant::DICTIONARY) {
			Dictionary fields = command.after;
			error = _remap_fields(command.component, fields, reverse);
			command.after = fields;
		} else if (command.kind == SET_ORDER) {
			command.before = source_prepared->get_order(command.entity);
		}
		if (error != OK) {
			return error;
		}
		Vector<Command> commands;
		commands.push_back(command);
		error = source_prepared->get_commands().execute("Apply prefab override", commands);
		if (error != OK) {
			return error;
		}
		source_prepared->get_commands().clear();
	}
	source_changed = source_prepared->catalog.get_ids();
	source_prepared->revision = p_prefab->revision + 1;
	for (EntityId id : source_changed) {
		if (source_prepared->catalog.get_state(id) != EntityReferenceState::DELETED) {
			Dictionary record;
			error = source_prepared->_read_record(id, record);
			if (error == OK) {
				error = EntitySceneIO::encode(record, source_prepared->sections[id].bytes);
			}
			if (error != OK) {
				return error;
			}
		}
	}
	Vector<Ref<EntityScene>> users = p_users;
	bool included = false;
	for (const Ref<EntityScene> &user : users) {
		included |= user.ptr() == &document;
	}
	if (!included) {
		users.push_back(Ref<EntityScene>(&document));
	}
	Vector<Ref<EntityScene>> prepared_users;
	Vector<Vector<EntityId>> changed_users;
	HashSet<ObjectID> seen;
	for (const Ref<EntityScene> &user : users) {
		ERR_FAIL_COND_V(user.is_null() || user.ptr() == p_prefab.ptr() || seen.has(user->get_instance_id()), ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(user->_owner() != OK, ERR_UNAUTHORIZED);
		seen.insert(user->get_instance_id());
		Vector<EntityId> needed;
		Vector<EntityId> instances;
		for (const Variant &user_key : user->prefab_instances.get_key_list()) {
			Dictionary user_instance = user->prefab_instances[user_key];
			if (user_instance["document"] != p_prefab->document_id.to_string()) {
				continue;
			}
			EntityId instance_id;
			EntityId::parse(user_key, instance_id);
			instances.push_back(instance_id);
			Dictionary user_mapping = user_instance["mapping"];
			for (const Variant &source : user_mapping.get_key_list()) {
				EntityId id;
				EntityId::parse(user_mapping[source], id);
				needed.push_back(id);
			}
		}
		Dictionary user_prefabs = user->prefab_instances;
		user->prefab_instances = Dictionary();
		Ref<EntityScene> prepared;
		error = user->_prepare(needed, prepared);
		user->prefab_instances = user_prefabs;
		if (error != OK) {
			return error;
		}
		prepared->prefab_instances = user_prefabs.duplicate(true);
		if (user.ptr() == &document) {
			Dictionary applying = prepared->prefab_instances[key];
			applying["overrides"] = Array();
			prepared->prefab_instances[key] = applying;
		}
		Vector<EntityId> changed = prepared->catalog.get_ids();
		for (EntityId instance_id : instances) {
			error = _refresh_instance(**prepared, instance_id, **source_prepared, changed);
			if (error != OK) {
				return error;
			}
		}
		for (EntityId id : changed) {
			if (prepared->catalog.get_state(id) != EntityReferenceState::DELETED) {
				Dictionary record;
				error = prepared->_read_record(id, record);
				if (error == OK) {
					error = EntitySceneIO::encode(record, prepared->sections[id].bytes);
				}
				if (error != OK) {
					return error;
				}
			}
		}
		prepared_users.push_back(prepared);
		changed_users.push_back(changed);
	}
	error = p_prefab->_can_commit(**source_prepared, source_changed);
	if (error != OK) {
		return error;
	}
	for (int i = 0; i < users.size(); i++) {
		error = users[i]->_can_commit(**prepared_users[i], changed_users[i]);
		if (error != OK) {
			return error;
		}
	}
	p_prefab->prefab_instances = source_prepared->prefab_instances;
	p_prefab->_commit(**source_prepared, source_changed, false);
	p_prefab->get_commands().clear();
	for (int i = 0; i < users.size(); i++) {
		users[i]->prefab_instances = prepared_users[i]->prefab_instances;
		users[i]->_commit(**prepared_users[i], changed_users[i], false);
		users[i]->get_commands().clear();
	}
	p_prefab->emit_changed();
	for (const Ref<EntityScene> &user : users) {
		user->emit_changed();
	}
	return OK;
}
