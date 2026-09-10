#include "entity_scene_io.h"
#include "entity_scene_commands.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/marshalls.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <cstdio>
#endif

Error EntitySceneIO::encode(const Variant &p_value, PackedByteArray &r_bytes) {
	auto validate = [](auto &p_self, const Variant &p_entry, int p_depth) -> Error {
		if (p_depth > 64) {
			return ERR_CYCLIC_LINK;
		}
		Variant::Type type = p_entry.get_type();
		if (type == Variant::OBJECT || type == Variant::RID || type == Variant::CALLABLE || type == Variant::SIGNAL) {
			return ERR_INVALID_DATA;
		}
		if (type == Variant::ARRAY) {
			for (const Variant &value : Array(p_entry)) {
				Error error = p_self(p_self, value, p_depth + 1);
				if (error != OK) {
					return error;
				}
			}
		} else if (type == Variant::DICTIONARY) {
			Dictionary fields = p_entry;
			for (const Variant &key : fields.get_key_list()) {
				if (key.get_type() != Variant::STRING) {
					return ERR_INVALID_DATA;
				}
				Error error = p_self(p_self, fields[key], p_depth + 1);
				if (error != OK) {
					return error;
				}
			}
		}
		return OK;
	};
	Error error = validate(validate, p_value, 0);
	if (error != OK) {
		return error;
	}
	int size = 0;
	error = encode_variant(p_value, nullptr, size, false);
	if (error != OK) {
		return error;
	}
	r_bytes.resize(size);
	return encode_variant(p_value, r_bytes.ptrw(), size, false);
}

Error EntitySceneIO::decode(const PackedByteArray &p_bytes, Variant &r_value) {
	int used = 0;
	Error error = decode_variant(r_value, p_bytes.ptr(), p_bytes.size(), &used, false);
	return error == OK && used != p_bytes.size() ? ERR_FILE_CORRUPT : error;
}

Error EntitySceneIO::read_manifest(const String &p_path, Dictionary &r_manifest, Ref<FileAccess> &r_file) {
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &error);
	if (error != OK) {
		return error;
	}
	if (file->get_length() < 32 || file->get_32() != 0x4e435345 || file->get_32() != FORMAT_VERSION) {
		return ERR_FILE_UNRECOGNIZED;
	}
	ResourceUID::ID uid = file->get_64();
	uint64_t offset = file->get_64();
	uint64_t length = file->get_64();
	if (offset < 32 || offset > file->get_length() || length != file->get_length() - offset || length > INT32_MAX) {
		return ERR_FILE_CORRUPT;
	}
	file->seek(offset);
	Variant manifest;
	error = decode(file->get_buffer(length), manifest);
	if (error != OK || manifest.get_type() != Variant::DICTIONARY) {
		return ERR_FILE_CORRUPT;
	}
	r_manifest = manifest;
	if (!r_manifest.has("document") || !r_manifest.has("records") || r_manifest["records"].get_type() != Variant::ARRAY || !r_manifest.has("types") || r_manifest["types"].get_type() != Variant::DICTIONARY || !r_manifest.has("prefabs") || r_manifest["prefabs"].get_type() != Variant::DICTIONARY) {
		return ERR_FILE_CORRUPT;
	}
	r_manifest["uid"] = int64_t(uid);
	r_manifest["section_end"] = int64_t(offset);
	r_file = file;
	return OK;
}

Error EntitySceneIO::load(const String &p_path, Ref<EntityScene> &r_scene) {
	Dictionary manifest;
	Ref<FileAccess> file;
	Error error = read_manifest(p_path, manifest, file);
	if (error != OK) {
		return error;
	}
	Ref<EntityScene> scene;
	scene.instantiate();
	error = EntityId::parse(manifest["document"], scene->document_id);
	if (error != OK || !scene->document_id.is_valid()) {
		return ERR_FILE_CORRUPT;
	}
	EntityCatalog schema_catalog;
	EntityWorld schemas(schema_catalog);
	Dictionary types = manifest["types"];
	Array entries = manifest["records"];
	uint64_t previous_end = 32;
	for (const Variant &value : entries) {
		if (value.get_type() != Variant::DICTIONARY) {
			return ERR_FILE_CORRUPT;
		}
		Dictionary entry = value;
		EntityId id;
		EntityRef parent;
		if (!entry.has_all(Array{"id", "parent", "deleted", "order", "offset", "length", "components", "dependencies"}) || EntityId::parse(entry["id"], id) != OK || !id.is_valid() || EntityId::parse(entry["parent"], parent.id) != OK || scene->catalog.records.has(id)) {
			return scene->_fail(id, "catalog", ERR_FILE_CORRUPT);
		}
		if (entry["deleted"].get_type() != Variant::BOOL || entry["order"].get_type() != Variant::INT || entry["offset"].get_type() != Variant::INT || entry["length"].get_type() != Variant::INT || entry["components"].get_type() != Variant::ARRAY || entry["dependencies"].get_type() != Variant::ARRAY) {
			return scene->_fail(id, "catalog", ERR_FILE_CORRUPT);
		}
		scene->catalog.records.insert(id, { bool(entry["deleted"]), parent });
		scene->order.insert(id, entry["order"]);
		if (bool(entry["deleted"])) {
			continue;
		}
		EntityScene::Section section;
		section.offset = int64_t(entry["offset"]);
		section.length = int64_t(entry["length"]);
		uint64_t end = int64_t(manifest["section_end"]);
		if (section.offset < previous_end || section.offset > end || section.length > end - section.offset || !section.length || section.length > INT32_MAX) {
			return scene->_fail(id, "section", ERR_FILE_CORRUPT);
		}
		previous_end = section.offset + section.length;
		section.components = entry["components"];
		section.dependencies = entry["dependencies"];
		for (const Variant &component : section.components) {
			if (component.get_type() != Variant::STRING || !types.has(component)) {
				return scene->_fail(id, String(component), ERR_INVALID_DATA);
			}
			Vector<String> pending;
			HashSet<String> seen;
			pending.push_back(component);
			for (int i = 0; i < pending.size(); i++) {
				String type_id = pending[i];
				if (seen.has(type_id)) {
					continue;
				}
				seen.insert(type_id);
				const EntityComponentSchema *schema = schemas.get_schemas().find(type_id.hex_to_int());
				if (!schema || !types.has(type_id) || types[type_id].get_type() != Variant::DICTIONARY) {
					return scene->_fail(id, type_id, ERR_UNAVAILABLE);
				}
				Dictionary fields = types[type_id];
				int count = 0;
				for (const EntityFieldSchema &field : schema->fields) {
					if (!field.serialized) {
						continue;
					}
					count++;
					String field_id = String::num_uint64(field.id, 16);
					if (!fields.has(field_id) || fields[field_id] != String(field.native_type)) {
						return scene->_fail(id, type_id + "/" + field_id, ERR_INVALID_DATA);
					}
					if (field.nested_type_id) {
						pending.push_back(String::num_uint64(field.nested_type_id, 16));
					}
				}
				if (count != fields.size()) {
					return scene->_fail(id, type_id + "/manifest", ERR_INVALID_DATA);
				}
			}
		}
		for (const Variant &dependency : section.dependencies) {
			if (dependency.get_type() != Variant::DICTIONARY || !Dictionary(dependency).has_all(Array{"uid", "path", "type", "entity", "field"})) {
				return scene->_fail(id, "dependencies", ERR_FILE_CORRUPT);
			}
		}
		scene->sections.insert(id, section);
	}
	for (EntityId id : scene->catalog.get_ids()) {
		if (scene->catalog.records[id].deleted) {
			continue;
		}
		EntityRef parent = scene->catalog.get_parent(id);
		if (parent.id.is_valid() && scene->catalog.get_state(parent.id) != EntityReferenceState::UNLOADED) {
			return scene->_fail(id, "parent", ERR_INVALID_DATA);
		}
		scene->catalog._set_parent(id, parent);
	}
	Vector<EntityId> ordered;
	error = scene->_collect_required(scene->catalog.get_ids(), ordered);
	if (error != OK) {
		return error;
	}
	scene->revision = manifest.get("revision", 0);
	scene->prefab_instances = manifest["prefabs"];
	scene->source = file;
	scene->storage_path = p_path;
	EntitySceneCommands commands(**scene);
	error = commands._reconcile_prefab_catalog();
	if (error != OK) {
		return error;
	}
	r_scene = scene;
	return OK;
}

Error EntitySceneIO::save(EntityScene &p_scene, const String &p_path, ResourceUID::ID p_uid) {
	ERR_FAIL_COND_V(p_scene._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_path.get_extension().to_lower() != "escn", ERR_INVALID_PARAMETER);
	for (const Variant &key : p_scene.prefab_instances.get_key_list()) {
		Dictionary instance = p_scene.prefab_instances[key];
		if (!Array(instance["conflicts"]).is_empty()) {
			return p_scene._fail(EntityId(), "prefab/conflicts", ERR_BUSY);
		}
	}
	EntityId nonce;
	Error error = EntityId::generate(nonce);
	if (error != OK) {
		return error;
	}
	String temporary = p_path + "." + nonce.to_string() + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE_READ, &error);
	if (error != OK) {
		return error;
	}
	if (p_uid == ResourceUID::INVALID_ID) {
		p_uid = ResourceSaver::get_resource_id_for_path(p_path, true);
		if (p_uid == ResourceUID::INVALID_ID) {
			p_uid = ResourceUID::get_singleton()->create_id();
		}
	}
	file->store_32(0x4e435345);
	file->store_32(FORMAT_VERSION);
	file->store_64(p_uid);
	file->store_64(0);
	file->store_64(0);
	Dictionary manifest;
	manifest["document"] = p_scene.document_id.to_string();
	manifest["revision"] = int64_t(p_scene.revision + 1);
	manifest["prefabs"] = p_scene.prefab_instances;
	Dictionary types;
	for (const KeyValue<uint64_t, EntityComponentSchema> &entry : p_scene.get_world()->get_schemas().get_types()) {
		Dictionary fields;
		for (const EntityFieldSchema &field : entry.value.fields) {
			if (field.serialized) {
				fields[String::num_uint64(field.id, 16)] = String(field.native_type);
			}
		}
		types[String::num_uint64(entry.key, 16)] = fields;
	}
	manifest["types"] = types;
	Array entries;
	HashMap<EntityId, EntityScene::Section, EntityIdHasher> sections;
	for (EntityId id : p_scene.catalog.get_ids()) {
		Dictionary entry;
		entry["id"] = id.to_string();
		entry["parent"] = p_scene.catalog.get_parent(id).id.to_string();
		entry["order"] = p_scene.get_order(id);
		bool deleted = p_scene.catalog.get_state(id) == EntityReferenceState::DELETED;
		entry["deleted"] = deleted;
		EntityScene::Section section;
		PackedByteArray bytes;
		if (!deleted) {
			Dictionary record;
			bool stored = false;
			error = p_scene._read_record(id, record, &stored);
			if (error == OK) {
				error = p_scene._describe(id, record, section);
			}
			if (error == OK) {
				error = stored ? p_scene._read_bytes(id, bytes) : encode(record, bytes);
			}
			if (error != OK) {
				break;
			}
			section.offset = file->get_position();
			section.length = bytes.size();
			if (!file->store_buffer(bytes)) {
				error = ERR_FILE_CANT_WRITE;
				break;
			}
			sections.insert(id, section);
		}
		entry["offset"] = int64_t(section.offset);
		entry["length"] = int64_t(section.length);
		entry["components"] = section.components;
		entry["dependencies"] = section.dependencies;
		entries.push_back(entry);
	}
	manifest["records"] = entries;
	uint64_t manifest_offset = file->get_position();
	PackedByteArray manifest_bytes;
	if (error == OK) {
		error = encode(manifest, manifest_bytes);
	}
	if (error == OK && !file->store_buffer(manifest_bytes)) {
		error = ERR_FILE_CANT_WRITE;
	}
	if (error == OK) {
		file->seek(16);
		if (!file->store_64(manifest_offset) || !file->store_64(manifest_bytes.size())) {
			error = ERR_FILE_CANT_WRITE;
		}
		file->flush();
		if (file->get_error() != OK) {
			error = ERR_FILE_CANT_WRITE;
		}
	}
	file->close();
	file.unref();
	if (error == OK) {
		Dictionary written;
		Ref<FileAccess> verify;
		error = read_manifest(temporary, written, verify);
		if (error == OK && written["records"] != manifest["records"]) {
			error = ERR_FILE_CORRUPT;
		}
	}
	if (error != OK) {
		DirAccess::remove_absolute(temporary);
		return error;
	}
	p_scene.source.unref();
	String from = ProjectSettings::get_singleton()->globalize_path(temporary);
	String to = ProjectSettings::get_singleton()->globalize_path(p_path);
#ifdef WINDOWS_ENABLED
	from = from.begins_with("//") ? String("\\\\?\\UNC\\") + from.substr(2).replace_char('/', '\\') : String("\\\\?\\") + from.replace_char('/', '\\');
	to = to.begins_with("//") ? String("\\\\?\\UNC\\") + to.substr(2).replace_char('/', '\\') : String("\\\\?\\") + to.replace_char('/', '\\');
	if (!MoveFileExW((LPCWSTR)from.utf16().get_data(), (LPCWSTR)to.utf16().get_data(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		error = ERR_FILE_CANT_WRITE;
	}
#else
	if (std::rename(from.utf8().get_data(), to.utf8().get_data()) != 0) {
		error = ERR_FILE_CANT_WRITE;
	}
#endif
	if (error != OK) {
		DirAccess::remove_absolute(temporary);
		return error;
	}
	p_scene.storage_path = p_path;
	p_scene.sections = sections;
	p_scene.revision++;
	return OK;
}

Ref<Resource> ResourceFormatLoaderEntityScene::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<EntityScene> scene;
	Error error = EntitySceneIO::load(p_path, scene);
	if (r_error) {
		*r_error = error;
	}
	if (error == OK && r_progress) {
		*r_progress = 1.0;
	}
	return scene;
}

void ResourceFormatLoaderEntityScene::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("escn");
}

bool ResourceFormatLoaderEntityScene::handles_type(const String &p_type) const {
	return p_type == "EntityScene";
}

String ResourceFormatLoaderEntityScene::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "escn" ? String("EntityScene") : String();
}

ResourceUID::ID ResourceFormatLoaderEntityScene::get_resource_uid(const String &p_path) const {
	Dictionary manifest;
	Ref<FileAccess> file;
	return EntitySceneIO::read_manifest(p_path, manifest, file) == OK ? int64_t(manifest["uid"]) : ResourceUID::INVALID_ID;
}

void ResourceFormatLoaderEntityScene::get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types) {
	Dictionary manifest;
	Ref<FileAccess> file;
	ERR_FAIL_COND(EntitySceneIO::read_manifest(p_path, manifest, file) != OK);
	HashSet<String> seen;
	Array dependencies;
	for (const Variant &value : Array(manifest["records"])) {
		dependencies.append_array(Dictionary(value)["dependencies"]);
	}
	Dictionary prefabs = manifest["prefabs"];
	for (const Variant &key : prefabs.get_key_list()) {
		Dictionary instance = prefabs[key];
		Dictionary dependency;
		dependency["uid"] = instance["uid"];
		dependency["path"] = instance["path"];
		dependency["type"] = "EntityScene";
		dependencies.push_back(dependency);
	}
	for (const Variant &value : dependencies) {
		Dictionary dependency = value;
		String uid = dependency["uid"];
		if (seen.has(uid)) {
			continue;
		}
		seen.insert(uid);
		String path = dependency["path"];
		ResourceUID::ID id = ResourceUID::get_singleton()->text_to_id(uid);
		if (ResourceUID::get_singleton()->has_id(id)) {
			path = ResourceUID::get_singleton()->get_id_path(id);
		}
		p_dependencies->push_back(uid + "::" + (p_add_types ? String(dependency["type"]) : String()) + "::" + path);
	}
}

Error ResourceFormatLoaderEntityScene::rename_dependencies(const String &p_path, const HashMap<String, String> &p_map) {
	Ref<EntityScene> scene;
	Error error = EntitySceneIO::load(p_path, scene);
	if (error != OK) {
		return error;
	}
	for (const Variant &key : scene->prefab_instances.get_key_list()) {
		Dictionary instance = scene->prefab_instances[key];
		String path = instance["path"];
		if (p_map.has(path)) {
			instance["path"] = p_map[path];
		}
	}
	return EntitySceneIO::save(**scene, p_path, get_resource_uid(p_path));
}

Error ResourceFormatSaverEntityScene::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<EntityScene> scene = p_resource;
	ERR_FAIL_COND_V(scene.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_flags & ~(ResourceSaver::FLAG_CHANGE_PATH), ERR_UNAVAILABLE);
	return EntitySceneIO::save(**scene, p_path);
}

Error ResourceFormatSaverEntityScene::set_uid(const String &p_path, ResourceUID::ID p_uid) {
	Ref<EntityScene> scene;
	Error error = EntitySceneIO::load(p_path, scene);
	return error == OK ? EntitySceneIO::save(**scene, p_path, p_uid) : error;
}

bool ResourceFormatSaverEntityScene::recognize(const Ref<Resource> &p_resource) const {
	return p_resource.is_valid() && Object::cast_to<EntityScene>(*p_resource);
}

void ResourceFormatSaverEntityScene::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (recognize(p_resource)) {
		p_extensions->push_back("escn");
	}
}
