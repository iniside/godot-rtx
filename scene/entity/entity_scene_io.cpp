#include "entity_scene_io.h"

#include "entity_scene_commands.h"
#include "entity_transform_system.h"

#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/variant/variant_parser.h"
#include "servers/rendering/rendering_server_enums.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <cstdio>
#endif

namespace {

struct SceneFile {
	String path;
	bool cluster = false;
};

struct StorageTree {
	HashMap<EntityId, Dictionary, EntityIdHasher> records;
	HashMap<EntityId, SceneFile, EntityIdHasher> files;
	Vector<String> paths;
	Vector<String> prefab_paths;
	Dictionary prefabs;
};

struct EntityIdSorter {
	bool operator()(const EntityId &p_left, const EntityId &p_right) const {
		return p_left.high != p_right.high ? p_left.high < p_right.high : p_left.low < p_right.low;
	}
};

String component_key(uint64_t p_id) {
	return String::num_uint64(p_id, 16);
}

String native_path(const String &p_path) {
	String result = ProjectSettings::get_singleton()->globalize_path(p_path);
#ifdef WINDOWS_ENABLED
	result = result.begins_with("//") ? String("\\\\?\\UNC\\") + result.substr(2).replace_char('/', '\\') : String("\\\\?\\") + result.replace_char('/', '\\');
#endif
	return result;
}

Error move_file(const String &p_from, const String &p_to) {
	const String from = native_path(p_from);
	const String to = native_path(p_to);
#ifdef WINDOWS_ENABLED
	if (!MoveFileExW((LPCWSTR)from.utf16().get_data(), (LPCWSTR)to.utf16().get_data(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		return ERR_FILE_CANT_WRITE;
	}
#else
	if (std::rename(from.utf8().get_data(), to.utf8().get_data()) != 0) {
		return ERR_FILE_CANT_WRITE;
	}
#endif
	return OK;
}

Error write_file(const String &p_path, const String &p_text) {
	EntityId nonce;
	Error error = EntityId::generate(nonce);
	if (error != OK) {
		return error;
	}
	const String temporary = p_path + "." + nonce.to_string() + ".tmp";
	{
		Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE, &error);
		if (error != OK) {
			return error;
		}
		file->store_string(p_text);
		file->flush();
		error = file->get_error();
		file->close();
	}
	if (error != OK) {
		DirAccess::remove_absolute(temporary);
		return ERR_FILE_CANT_WRITE;
	}
	error = move_file(temporary, p_path);
	if (error != OK) {
		DirAccess::remove_absolute(temporary);
	}
	return error;
}

bool file_matches(const String &p_path, const String &p_text) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	return file.is_valid() && file->get_as_text() == p_text;
}

Error read_text(const String &p_path, String &r_text) {
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &error);
	if (error != OK) {
		return error;
	}
	r_text = file->get_as_text();
	return OK;
}

bool grid_name_is_valid(const String &p_name) {
	if (p_name.is_empty() || p_name == "." || p_name == ".." || p_name.length() > 64) {
		return false;
	}
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!allowed) {
			return false;
		}
	}
	return true;
}

bool cell_name_is_valid(const String &p_name) {
	const Vector<String> parts = p_name.split("_", true);
	if (parts.size() != 3) {
		return false;
	}
	for (const String &part : parts) {
		if (part.is_empty() || !part.is_valid_int() || itos(part.to_int()) != part) {
			return false;
		}
	}
	return true;
}

String cell_directory(const String &p_grid, int64_t p_x, int64_t p_y, int64_t p_z) {
	return "cells/" + p_grid + "/" + itos(p_x) + "_" + itos(p_y) + "_" + itos(p_z);
}

Error read_entity_directory(const String &p_directory, const String &p_relative, bool p_parse, StorageTree &r_tree) {
	const String absolute = p_directory.path_join(p_relative);
	if (!DirAccess::dir_exists_absolute(absolute)) {
		return OK;
	}
	for (const String &name : DirAccess::get_files_at(absolute)) {
		if (name.get_extension().to_lower() != "escn") {
			continue;
		}
		const String relative = p_relative.path_join(name);
		const String path = p_directory.path_join(relative);
		if (name.ends_with(".cluster.escn")) {
			Variant parsed;
			Error error = EntitySceneIO::read_variant_file(path, parsed);
			if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
				error = ERR_FILE_CORRUPT;
			}
			ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot read entity cluster: " + path);
			Dictionary cluster = parsed;
			for (const Variant &key : cluster.get_key_list()) {
				EntityId id;
				ERR_FAIL_COND_V_MSG(key.get_type() != Variant::STRING || EntityId::parse(key, id) != OK || !id.is_valid(), ERR_FILE_CORRUPT, "Invalid entity id \"" + String(key) + "\" in cluster: " + path);
				const SceneFile *owner = r_tree.files.getptr(id);
				ERR_FAIL_COND_V_MSG(owner, ERR_FILE_CORRUPT, "Entity " + id.to_string() + " is stored twice: " + p_directory.path_join(owner->path) + " and " + path);
				ERR_FAIL_COND_V_MSG(cluster[key].get_type() != Variant::DICTIONARY, ERR_FILE_CORRUPT, "Invalid record for entity " + id.to_string() + " in cluster: " + path);
				r_tree.files.insert(id, { relative, true });
				if (p_parse) {
					r_tree.records.insert(id, cluster[key]);
				}
			}
			r_tree.paths.push_back(relative);
			continue;
		}
		EntityId id;
		ERR_FAIL_COND_V_MSG(EntityId::parse(name.get_basename(), id) != OK || !id.is_valid(), ERR_FILE_CORRUPT, "Entity file name is not an entity id: " + path);
		const SceneFile *owner = r_tree.files.getptr(id);
		ERR_FAIL_COND_V_MSG(owner, ERR_FILE_CORRUPT, "Entity " + id.to_string() + " is stored twice: " + p_directory.path_join(owner->path) + " and " + path);
		r_tree.files.insert(id, { relative, false });
		r_tree.paths.push_back(relative);
		if (p_parse) {
			Variant parsed;
			Error error = EntitySceneIO::read_variant_file(path, parsed);
			if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
				error = ERR_FILE_CORRUPT;
			}
			ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot read entity record: " + path);
			r_tree.records.insert(id, parsed);
		}
	}
	return OK;
}

Error read_tree(const String &p_directory, const HashSet<String> &p_grids, bool p_parse, StorageTree &r_tree) {
	Error error = read_entity_directory(p_directory, "global", p_parse, r_tree);
	if (error != OK) {
		return error;
	}
	const String cells = p_directory.path_join("cells");
	if (DirAccess::dir_exists_absolute(cells)) {
		for (const String &grid : DirAccess::get_directories_at(cells)) {
			ERR_FAIL_COND_V_MSG(!p_grids.has(grid), ERR_FILE_CORRUPT, "Entity scene has no grid named \"" + grid + "\": " + cells.path_join(grid));
			const String grid_directory = cells.path_join(grid);
			for (const String &cell : DirAccess::get_directories_at(grid_directory)) {
				ERR_FAIL_COND_V_MSG(!cell_name_is_valid(cell), ERR_FILE_CORRUPT, "Invalid cell directory name: " + grid_directory.path_join(cell));
				error = read_entity_directory(p_directory, "cells/" + grid + "/" + cell, p_parse, r_tree);
				if (error != OK) {
					return error;
				}
			}
		}
	}
	const String prefabs = p_directory.path_join("prefabs");
	if (DirAccess::dir_exists_absolute(prefabs)) {
		for (const String &name : DirAccess::get_files_at(prefabs)) {
			if (name.get_extension().to_lower() != "escn") {
				continue;
			}
			const String relative = String("prefabs").path_join(name);
			const String path = p_directory.path_join(relative);
			EntityId id;
			ERR_FAIL_COND_V_MSG(EntityId::parse(name.get_basename(), id) != OK || !id.is_valid(), ERR_FILE_CORRUPT, "Prefab instance file name is not an entity id: " + path);
			Variant parsed;
			error = EntitySceneIO::read_variant_file(path, parsed);
			if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
				error = ERR_FILE_CORRUPT;
			}
			ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot read prefab instance: " + path);
			r_tree.prefab_paths.push_back(relative);
			r_tree.prefabs[id.to_string()] = parsed;
		}
	}
	return OK;
}

ResourceUID::ID read_uid_line(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return ResourceUID::INVALID_ID;
	}
	const String line = file->get_line().strip_edges();
	if (!line.begins_with("uid=\"") || !line.ends_with("\"")) {
		return ResourceUID::INVALID_ID;
	}
	return ResourceUID::get_singleton()->text_to_id(line.trim_prefix("uid=\"").trim_suffix("\""));
}

Error read_main(const String &p_path, Ref<ConfigFile> &r_config) {
	Ref<ConfigFile> config;
	config.instantiate();
	Error error = config->load(p_path);
	if (error != OK) {
		return error;
	}
	r_config = config;
	return OK;
}

void collect_asset_dependencies(const EntitySchemaRegistry &p_schemas, uint64_t p_type, const Dictionary &p_fields, EntityId p_id, const String &p_prefix, Array &r_dependencies) {
	const EntityComponentSchema *schema = p_schemas.find(p_type);
	if (!schema) {
		return;
	}
	const String prefix = p_prefix.is_empty() ? component_key(p_type) : p_prefix;
	for (const EntityFieldSchema &field : schema->fields) {
		if (!field.serialized) {
			continue;
		}
		const String key = component_key(field.id);
		if (!p_fields.has(key)) {
			continue;
		}
		const Variant value = p_fields[key];
		const String address = prefix + "/" + key;
		Array entries;
		if (value.get_type() == Variant::ARRAY) {
			entries = value;
		} else {
			entries.push_back(value);
		}
		if (field.nested_type_id) {
			for (int i = 0; i < entries.size(); i++) {
				if (entries[i].get_type() == Variant::DICTIONARY) {
					collect_asset_dependencies(p_schemas, field.nested_type_id, entries[i], p_id, address, r_dependencies);
				}
			}
		}
		if (!field.asset_reference) {
			continue;
		}
		for (int i = 0; i < entries.size(); i++) {
			if (entries[i].get_type() != Variant::STRING) {
				continue;
			}
			const String text = entries[i];
			if (!text.begins_with("uid://")) {
				continue;
			}
			const String uid = text.get_slice("::", 0);
			const ResourceUID::ID resource_id = ResourceUID::get_singleton()->text_to_id(uid);
			const String path = ResourceUID::get_singleton()->has_id(resource_id) ? ResourceUID::get_singleton()->get_id_path(resource_id) : String();
			Dictionary dependency;
			dependency["uid"] = uid;
			dependency["path"] = path;
			dependency["type"] = path.is_empty() ? String("Resource") : ResourceLoader::get_resource_type(path);
			dependency["entity"] = p_id.to_string();
			dependency["field"] = address + "[" + itos(i) + "]";
			r_dependencies.push_back(dependency);
		}
	}
}

} // namespace

Error EntitySceneIO::encode(const Variant &p_value, String &r_text) {
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
	r_text = String();
	error = VariantWriter::write_to_string(p_value, r_text);
	if (error == OK) {
		r_text += "\n";
	}
	return error;
}

Error EntitySceneIO::decode(const String &p_text, Variant &r_value) {
	VariantParser::StreamString stream;
	stream.s = p_text;
	String message;
	int line = 0;
	Error error = VariantParser::parse(&stream, r_value, message, line);
	if (error != OK) {
		ERR_PRINT("Entity scene parse error at line " + itos(line) + ": " + message);
	}
	return error;
}

Error EntitySceneIO::read_variant_file(const String &p_path, Variant &r_value) {
	String text;
	Error error = read_text(p_path, text);
	if (error != OK) {
		return error;
	}
	return decode(text, r_value);
}

String EntitySceneIO::scene_directory(const String &p_path) {
	return p_path.get_basename();
}

bool EntitySceneIO::is_child_path(const String &p_path) {
	const String name = p_path.get_file();
	if (name.ends_with(".cluster.escn")) {
		return true;
	}
	EntityId id;
	if (EntityId::parse(name.get_basename(), id) == OK) {
		return true;
	}
	const String parent = p_path.get_base_dir().get_file();
	return parent == "global" || parent == "prefabs";
}

Error EntitySceneIO::load(const String &p_path, Ref<EntityScene> &r_scene) {
	Ref<ConfigFile> config;
	Error error = read_main(p_path, config);
	if (error != OK) {
		return error;
	}
	const String directory = scene_directory(p_path);
	ERR_FAIL_COND_V_MSG(!DirAccess::dir_exists_absolute(directory), ERR_FILE_CORRUPT, "Entity scene directory is missing: " + directory);
	Ref<EntityScene> scene;
	scene.instantiate();
	const Variant document = config->get_value("", "document", String());
	ERR_FAIL_COND_V_MSG(document.get_type() != Variant::STRING || EntityId::parse(document, scene->document_id) != OK || !scene->document_id.is_valid(), ERR_FILE_CORRUPT, "Entity scene has no valid document id: " + p_path);
	const Variant revision = config->get_value("", "revision", int64_t(0));
	ERR_FAIL_COND_V_MSG(revision.get_type() != Variant::INT || int64_t(revision) < 0, ERR_FILE_CORRUPT, "Entity scene has no valid revision: " + p_path);
	scene->revision = uint64_t(int64_t(revision));
	const Variant default_grid = config->get_value("", "default_grid", String());
	ERR_FAIL_COND_V_MSG(default_grid.get_type() != Variant::STRING || !grid_name_is_valid(default_grid), ERR_FILE_CORRUPT, "Entity scene has no valid default grid: " + p_path);
	scene->default_grid = default_grid;
	const Variant default_range = config->get_value("", "default_range", 0.0);
	ERR_FAIL_COND_V_MSG(!default_range.is_num() || double(default_range) <= 0.0, ERR_FILE_CORRUPT, "Entity scene has no valid default range: " + p_path);
	scene->default_range = default_range;
	scene->grids.clear();
	HashSet<String> grid_names;
	if (config->has_section("grids")) {
		for (const String &name : config->get_section_keys("grids")) {
			ERR_FAIL_COND_V_MSG(!grid_name_is_valid(name), ERR_FILE_CORRUPT, "Invalid grid name \"" + name + "\": " + p_path);
			const Variant value = config->get_value("grids", name);
			ERR_FAIL_COND_V_MSG(value.get_type() != Variant::DICTIONARY, ERR_FILE_CORRUPT, "Invalid grid \"" + name + "\": " + p_path);
			const Dictionary entry = value;
			const Variant size = entry.get("size", Variant());
			ERR_FAIL_COND_V_MSG(!size.is_num() || double(size) <= 0.0, ERR_FILE_CORRUPT, "Grid \"" + name + "\" has no valid size: " + p_path);
			const Variant range = entry.get("range", 0.0);
			ERR_FAIL_COND_V_MSG(!range.is_num() || double(range) < 0.0, ERR_FILE_CORRUPT, "Grid \"" + name + "\" has no valid range: " + p_path);
			scene->grids.insert(name, { double(size), double(range) });
			grid_names.insert(name);
		}
	}
	ERR_FAIL_COND_V_MSG(!scene->grids.has(scene->default_grid), ERR_FILE_CORRUPT, "Entity scene default grid \"" + scene->default_grid + "\" is not configured: " + p_path);
	Dictionary types;
	if (config->has_section("types")) {
		for (const String &name : config->get_section_keys("types")) {
			types[name] = config->get_value("types", name);
		}
	}
	StorageTree tree;
	error = read_tree(directory, grid_names, true, tree);
	if (error != OK) {
		return error;
	}
	Vector<EntityId> ids;
	ids.reserve(tree.records.size());
	for (const KeyValue<EntityId, Dictionary> &entry : tree.records) {
		ids.push_back(entry.key);
	}
	ids.sort_custom<EntityIdSorter>();
	EntityCatalog schema_catalog;
	EntityWorld schemas(schema_catalog);
	for (EntityId id : ids) {
		const Dictionary record = tree.records[id];
		const String path = directory.path_join(tree.files[id].path);
		const Variant parent_value = record.get("parent", Variant());
		const Variant order_value = record.get("order", Variant());
		EntityRef parent;
		if (parent_value.get_type() != Variant::STRING || EntityId::parse(parent_value, parent.id) != OK || order_value.get_type() != Variant::INT) {
			ERR_FAIL_V_MSG(scene->_fail(id, "catalog", ERR_FILE_CORRUPT), "Invalid entity record: " + path);
		}
		const Variant deleted_value = record.get("deleted", false);
		ERR_FAIL_COND_V_MSG(deleted_value.get_type() != Variant::BOOL, scene->_fail(id, "deleted", ERR_FILE_CORRUPT), "Invalid entity record: " + path);
		const bool deleted = deleted_value;
		scene->catalog.records.insert(id, { deleted, parent });
		scene->order.insert(id, order_value);
		if (deleted) {
			continue;
		}
		const Variant components_value = record.get("components", Variant());
		if (components_value.get_type() != Variant::DICTIONARY) {
			ERR_FAIL_V_MSG(scene->_fail(id, "components", ERR_FILE_CORRUPT), "Invalid entity record: " + path);
		}
		EntityScene::Section section;
		section.path = tree.files[id].path;
		section.cluster = tree.files[id].cluster;
		const Dictionary components = components_value;
		for (const Variant &component : components.get_key_list()) {
			if (component.get_type() != Variant::STRING || components[component].get_type() != Variant::DICTIONARY || !types.has(component)) {
				ERR_FAIL_V_MSG(scene->_fail(id, String(component), ERR_INVALID_DATA), "Invalid component in entity record: " + path);
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
					ERR_FAIL_V_MSG(scene->_fail(id, type_id, ERR_UNAVAILABLE), "Unknown component type in entity record: " + path);
				}
				Dictionary fields = types[type_id];
				int count = 0;
				for (const EntityFieldSchema &field : schema->fields) {
					if (!field.serialized) {
						continue;
					}
					count++;
					String field_id = component_key(field.id);
					if (!fields.has(field_id) || fields[field_id] != String(field.native_type)) {
						ERR_FAIL_V_MSG(scene->_fail(id, type_id + "/" + field_id, ERR_INVALID_DATA), "Entity scene type manifest does not match the engine: " + p_path);
					}
					if (field.nested_type_id) {
						pending.push_back(component_key(field.nested_type_id));
					}
				}
				if (count != fields.size()) {
					ERR_FAIL_V_MSG(scene->_fail(id, type_id + "/manifest", ERR_INVALID_DATA), "Entity scene type manifest does not match the engine: " + p_path);
				}
			}
			section.components.push_back(component);
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
	scene->prefab_instances = tree.prefabs;
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
	ERR_FAIL_COND_V_MSG(!p_scene.grids.has(p_scene.default_grid), ERR_INVALID_DATA, "Entity scene default grid \"" + p_scene.default_grid + "\" is not configured: " + p_path);
	const String directory = scene_directory(p_path);
	Error error = OK;
	if (!DirAccess::dir_exists_absolute(directory)) {
		error = DirAccess::make_dir_recursive_absolute(directory);
		if (error != OK) {
			return error;
		}
		error = write_file(directory.path_join(".gdignore"), String());
		if (error != OK) {
			return error;
		}
	}
	if (p_uid == ResourceUID::INVALID_ID) {
		p_uid = read_uid_line(p_path);
	}
	if (p_uid == ResourceUID::INVALID_ID) {
		p_uid = ResourceSaver::get_resource_id_for_path(p_path, true);
		if (p_uid == ResourceUID::INVALID_ID) {
			p_uid = ResourceUID::get_singleton()->create_id();
		}
	}
	HashSet<String> grid_names;
	for (const KeyValue<String, EntityScene::Grid> &entry : p_scene.grids) {
		grid_names.insert(entry.key);
	}
	StorageTree existing;
	error = read_tree(directory, grid_names, false, existing);
	if (error != OK) {
		return error;
	}
	Vector<EntityId> ids = p_scene.catalog.get_ids();
	ids.sort_custom<EntityIdSorter>();
	HashSet<EntityId, EntityIdHasher> referenced;
	Vector<String> instance_keys;
	for (const Variant &key : p_scene.prefab_instances.get_key_list()) {
		instance_keys.push_back(key);
		Dictionary instance = p_scene.prefab_instances[key];
		Dictionary mapping = instance["mapping"];
		for (const Variant &source : mapping.get_key_list()) {
			EntityId id;
			if (EntityId::parse(mapping[source], id) == OK) {
				referenced.insert(id);
			}
		}
	}
	instance_keys.sort();
	HashMap<EntityId, Dictionary, EntityIdHasher> records;
	HashMap<EntityId, EntityScene::Section, EntityIdHasher> sections;
	for (EntityId id : ids) {
		if (p_scene.catalog.get_state(id) == EntityReferenceState::DELETED) {
			continue;
		}
		Dictionary record;
		error = p_scene._read_record(id, record);
		if (error != OK) {
			return error;
		}
		EntityScene::Section section;
		error = p_scene._describe(id, record, section);
		if (error != OK) {
			return error;
		}
		records.insert(id, record);
		sections.insert(id, section);
	}
	const String transform_key = component_key(EntityComponentTraits<EntityTransform>::id);
	const String streaming_key = component_key(EntityComponentTraits<EntityStreaming>::id);
	const String camera_key = component_key(EntityComponentTraits<EntityCamera>::id);
	const String environment_key = component_key(EntityComponentTraits<EntityEnvironment>::id);
	const String light_key = component_key(EntityComponentTraits<EntityLight>::id);
	HashMap<EntityId, EntityPose, EntityIdHasher> poses;
	auto world_pose = [&](auto &p_self, EntityId p_id) -> EntityPose {
		const EntityPose *cached = poses.getptr(p_id);
		if (cached) {
			return *cached;
		}
		EntityPose local;
		const Dictionary *record = records.getptr(p_id);
		if (record) {
			const Dictionary components = (*record)["components"];
			const Variant transform = components.get(transform_key, Variant());
			if (transform.get_type() == Variant::DICTIONARY) {
				EntityCodec<EntityPose>::decode(Dictionary(transform).get("1", Variant()), local);
			}
		}
		const EntityId parent = p_scene.catalog.get_parent(p_id).id;
		const EntityPose result = parent.is_valid() && records.has(parent) ? EntityTransformSystem::compose(p_self(p_self, parent), local) : local;
		poses.insert(p_id, result);
		return result;
	};
	HashMap<EntityId, String, EntityIdHasher> directories;
	for (EntityId id : ids) {
		const Dictionary *record = records.getptr(id);
		if (!record) {
			continue;
		}
		const Dictionary components = (*record)["components"];
		bool global = !components.has(transform_key) || components.has(environment_key) || components.has(camera_key);
		if (!global) {
			const Variant light = components.get(light_key, Variant());
			if (light.get_type() == Variant::DICTIONARY) {
				const Variant type = Dictionary(light).get("1", int64_t(RSE::LIGHT_DIRECTIONAL));
				global = type.is_num() && int64_t(type) == int64_t(RSE::LIGHT_DIRECTIONAL);
			}
		}
		if (global) {
			directories.insert(id, "global");
			continue;
		}
		String grid = p_scene.default_grid;
		const Variant streaming = components.get(streaming_key, Variant());
		if (streaming.get_type() == Variant::DICTIONARY) {
			const Variant name = Dictionary(streaming).get("1", Variant());
			if (name.get_type() == Variant::STRING && !String(name).is_empty()) {
				grid = name;
			}
		}
		const EntityScene::Grid *configuration = p_scene.grids.getptr(grid);
		ERR_FAIL_COND_V_MSG(!configuration, p_scene._fail(id, "streaming/grid", ERR_INVALID_DATA), "Entity " + id.to_string() + " uses unknown grid \"" + grid + "\": " + p_path);
		const EntityPose pose = world_pose(world_pose, id);
		const int64_t x = int64_t(Math::floor(pose.translation.x / configuration->size));
		const int64_t y = int64_t(Math::floor(pose.translation.y / configuration->size));
		const int64_t z = int64_t(Math::floor(pose.translation.z / configuration->size));
		directories.insert(id, cell_directory(grid, x, y, z));
	}
	HashMap<String, String> texts;
	HashMap<String, Dictionary> clusters;
	HashMap<EntityId, String, EntityIdHasher> targets;
	for (EntityId id : ids) {
		const Dictionary *record = records.getptr(id);
		if (!record) {
			if (!referenced.has(id)) {
				continue;
			}
			Dictionary tombstone;
			tombstone["deleted"] = true;
			tombstone["parent"] = p_scene.catalog.get_parent(id).id.to_string();
			tombstone["order"] = p_scene.get_order(id);
			const String target = String("global").path_join(id.to_string() + ".escn");
			targets.insert(id, target);
			String text;
			error = encode(tombstone, text);
			if (error != OK) {
				return error;
			}
			texts.insert(target, text);
			continue;
		}
		Dictionary stored;
		stored["parent"] = (*record)["parent"];
		stored["order"] = (*record)["order"];
		stored["components"] = (*record)["components"];
		const String cell = directories[id];
		const SceneFile *previous = existing.files.getptr(id);
		if (previous && previous->cluster && previous->path.get_base_dir() == cell) {
			targets.insert(id, previous->path);
			clusters[previous->path][id.to_string()] = stored;
			continue;
		}
		const String target = cell.path_join(id.to_string() + ".escn");
		targets.insert(id, target);
		String text;
		error = encode(stored, text);
		if (error != OK) {
			return error;
		}
		texts.insert(target, text);
	}
	for (const KeyValue<String, Dictionary> &entry : clusters) {
		String text;
		error = encode(entry.value, text);
		if (error != OK) {
			return error;
		}
		texts.insert(entry.key, text);
	}
	for (const String &key : instance_keys) {
		Dictionary instance = Dictionary(p_scene.prefab_instances[key]).duplicate(true);
		Dictionary mapping = instance["mapping"];
		String cell;
		EntityId root;
		for (const Variant &source : mapping.get_key_list()) {
			EntityId id;
			if (EntityId::parse(mapping[source], id) != OK || !directories.has(id)) {
				continue;
			}
			const EntityId parent = p_scene.catalog.get_parent(id).id;
			bool inside = false;
			for (const Variant &other : mapping.get_key_list()) {
				EntityId mapped;
				inside |= EntityId::parse(mapping[other], mapped) == OK && mapped == parent;
			}
			if (inside || (root.is_valid() && EntityIdSorter()(root, id))) {
				continue;
			}
			root = id;
			cell = directories[id];
		}
		instance["cell"] = cell;
		const String target = String("prefabs").path_join(key + ".escn");
		String text;
		error = encode(instance, text);
		if (error != OK) {
			return error;
		}
		texts.insert(target, text);
	}
	Vector<String> written;
	written.reserve(texts.size());
	for (const KeyValue<String, String> &entry : texts) {
		written.push_back(entry.key);
	}
	written.sort();
	HashSet<String> keep;
	for (const String &path : written) {
		keep.insert(path);
	}
	int changes = 0;
	HashSet<String> moved;
	for (EntityId id : ids) {
		const SceneFile *previous = existing.files.getptr(id);
		const String *target = targets.getptr(id);
		if (!previous || previous->cluster || !target || previous->path == *target) {
			continue;
		}
		const String absolute = directory.path_join(*target);
		error = DirAccess::make_dir_recursive_absolute(absolute.get_base_dir());
		if (error != OK) {
			return error;
		}
		error = move_file(directory.path_join(previous->path), absolute);
		if (error != OK) {
			return error;
		}
		moved.insert(previous->path);
		changes++;
	}
	for (const String &path : written) {
		const String absolute = directory.path_join(path);
		if (file_matches(absolute, texts[path])) {
			continue;
		}
		error = DirAccess::make_dir_recursive_absolute(absolute.get_base_dir());
		if (error != OK) {
			return error;
		}
		error = write_file(absolute, texts[path]);
		if (error != OK) {
			return error;
		}
		changes++;
	}
	const uint64_t revision = p_scene.revision + (changes > 0 ? 1 : 0);
	Ref<ConfigFile> main;
	main.instantiate();
	main->set_value("", "uid", ResourceUID::get_singleton()->id_to_text(p_uid));
	main->set_value("", "document", p_scene.document_id.to_string());
	main->set_value("", "revision", int64_t(revision));
	main->set_value("", "default_grid", p_scene.default_grid);
	main->set_value("", "default_range", p_scene.default_range);
	Vector<String> grid_list;
	for (const KeyValue<String, EntityScene::Grid> &entry : p_scene.grids) {
		grid_list.push_back(entry.key);
	}
	grid_list.sort();
	for (const String &name : grid_list) {
		const EntityScene::Grid &grid = p_scene.grids[name];
		Dictionary entry;
		entry["size"] = grid.size;
		if (grid.range > 0.0) {
			entry["range"] = grid.range;
		}
		main->set_value("grids", name, entry);
	}
	Vector<String> type_list;
	HashMap<String, Dictionary> type_fields;
	for (const KeyValue<uint64_t, EntityComponentSchema> &entry : p_scene.get_world()->get_schemas().get_types()) {
		Dictionary fields;
		for (const EntityFieldSchema &field : entry.value.fields) {
			if (field.serialized) {
				fields[component_key(field.id)] = String(field.native_type);
			}
		}
		const String key = component_key(entry.key);
		type_list.push_back(key);
		type_fields.insert(key, fields);
	}
	type_list.sort();
	for (const String &key : type_list) {
		main->set_value("types", key, type_fields[key]);
	}
	const String main_text = main->encode_to_text();
	if (!file_matches(p_path, main_text)) {
		error = write_file(p_path, main_text);
		if (error != OK) {
			return error;
		}
	}
	Vector<String> removed = existing.paths;
	removed.append_array(existing.prefab_paths);
	for (const String &path : removed) {
		if (keep.has(path) || moved.has(path)) {
			continue;
		}
		DirAccess::remove_absolute(directory.path_join(path));
	}
	const String cells = directory.path_join("cells");
	if (DirAccess::dir_exists_absolute(cells)) {
		for (const String &grid : DirAccess::get_directories_at(cells)) {
			const String grid_directory = cells.path_join(grid);
			for (const String &cell : DirAccess::get_directories_at(grid_directory)) {
				const String cell_path = grid_directory.path_join(cell);
				if (DirAccess::get_files_at(cell_path).is_empty() && DirAccess::get_directories_at(cell_path).is_empty()) {
					DirAccess::remove_absolute(cell_path);
				}
			}
			if (DirAccess::get_files_at(grid_directory).is_empty() && DirAccess::get_directories_at(grid_directory).is_empty()) {
				DirAccess::remove_absolute(grid_directory);
			}
		}
	}
	for (KeyValue<EntityId, EntityScene::Section> &entry : sections) {
		const String *target = targets.getptr(entry.key);
		if (target) {
			entry.value.path = *target;
			entry.value.cluster = target->ends_with(".cluster.escn");
		}
	}
	p_scene.sections = sections;
	p_scene.cluster_path = String();
	p_scene.cluster_records = Dictionary();
	p_scene.storage_path = p_path;
	p_scene.revision = revision;
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
	return p_path.get_extension().to_lower() == "escn" && !EntitySceneIO::is_child_path(p_path) ? String("EntityScene") : String();
}

ResourceUID::ID ResourceFormatLoaderEntityScene::get_resource_uid(const String &p_path) const {
	if (p_path.get_extension().to_lower() != "escn" || EntitySceneIO::is_child_path(p_path)) {
		return ResourceUID::INVALID_ID;
	}
	return read_uid_line(p_path);
}

void ResourceFormatLoaderEntityScene::get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types) {
	Ref<EntityScene> scene;
	ERR_FAIL_COND(EntitySceneIO::load(p_path, scene) != OK);
	EntityCatalog catalog;
	EntityWorld schemas(catalog);
	Array dependencies;
	Vector<EntityId> ids = scene->catalog.get_ids();
	ids.sort_custom<EntityIdSorter>();
	for (EntityId id : ids) {
		const EntityScene::Section *section = scene->sections.getptr(id);
		Dictionary record;
		if (!section || section->path.is_empty() || scene->_read_stored(id, record) != OK) {
			continue;
		}
		const Dictionary components = record.get("components", Dictionary());
		for (const Variant &key : components.get_key_list()) {
			if (key.get_type() != Variant::STRING || components[key].get_type() != Variant::DICTIONARY) {
				continue;
			}
			collect_asset_dependencies(schemas.get_schemas(), String(key).hex_to_int(), components[key], id, String(), dependencies);
		}
	}
	Vector<String> instance_keys;
	for (const Variant &key : scene->prefab_instances.get_key_list()) {
		instance_keys.push_back(key);
	}
	instance_keys.sort();
	for (const String &key : instance_keys) {
		Dictionary instance = scene->prefab_instances[key];
		Dictionary dependency;
		dependency["uid"] = instance["uid"];
		dependency["path"] = instance["path"];
		dependency["type"] = "EntityScene";
		dependencies.push_back(dependency);
	}
	HashSet<String> seen;
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
	const String directory = EntitySceneIO::scene_directory(p_path).path_join("prefabs");
	if (!DirAccess::dir_exists_absolute(directory)) {
		return OK;
	}
	for (const String &name : DirAccess::get_files_at(directory)) {
		if (name.get_extension().to_lower() != "escn") {
			continue;
		}
		const String path = directory.path_join(name);
		Variant parsed;
		Error error = EntitySceneIO::read_variant_file(path, parsed);
		if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
			error = ERR_FILE_CORRUPT;
		}
		if (error != OK) {
			return error;
		}
		Dictionary instance = parsed;
		const String source = instance["path"];
		if (!p_map.has(source)) {
			continue;
		}
		instance["path"] = p_map[source];
		String text;
		error = EntitySceneIO::encode(instance, text);
		if (error == OK) {
			error = write_file(path, text);
		}
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

Error ResourceFormatSaverEntityScene::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<EntityScene> scene = p_resource;
	ERR_FAIL_COND_V(scene.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_flags & ~(ResourceSaver::FLAG_CHANGE_PATH), ERR_UNAVAILABLE);
	return EntitySceneIO::save(**scene, p_path);
}

Error ResourceFormatSaverEntityScene::set_uid(const String &p_path, ResourceUID::ID p_uid) {
	ERR_FAIL_COND_V(p_path.get_extension().to_lower() != "escn" || EntitySceneIO::is_child_path(p_path), ERR_FILE_UNRECOGNIZED);
	String text;
	Error error = read_text(p_path, text);
	if (error != OK) {
		return error;
	}
	const int break_at = text.find_char('\n');
	ERR_FAIL_COND_V_MSG(break_at < 0 || !text.begins_with("uid=\""), ERR_FILE_CORRUPT, "Entity scene has no uid on its first line: " + p_path);
	return write_file(p_path, "uid=\"" + ResourceUID::get_singleton()->id_to_text(p_uid) + "\"" + text.substr(break_at));
}

bool ResourceFormatSaverEntityScene::recognize(const Ref<Resource> &p_resource) const {
	return p_resource.is_valid() && Object::cast_to<EntityScene>(*p_resource);
}

void ResourceFormatSaverEntityScene::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (recognize(p_resource)) {
		p_extensions->push_back("escn");
	}
}
