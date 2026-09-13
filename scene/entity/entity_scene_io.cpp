#include "entity_scene_io.h"

#include "entity_record_parser.h"
#include "entity_scene_commands.h"

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

Error collect_entity_directories(const String &p_directory, const HashSet<String> &p_grids, Vector<String> &r_relative) {
	r_relative.push_back("global");
	const String cells = p_directory.path_join("cells");
	if (!DirAccess::dir_exists_absolute(cells)) {
		return OK;
	}
	for (const String &grid : DirAccess::get_directories_at(cells)) {
		ERR_FAIL_COND_V_MSG(!p_grids.has(grid), ERR_FILE_CORRUPT, "Entity scene has no grid named \"" + grid + "\": " + cells.path_join(grid));
		const String grid_directory = cells.path_join(grid);
		for (const String &cell : DirAccess::get_directories_at(grid_directory)) {
			ERR_FAIL_COND_V_MSG(!cell_name_is_valid(cell), ERR_FILE_CORRUPT, "Invalid cell directory name: " + grid_directory.path_join(cell));
			r_relative.push_back("cells/" + grid + "/" + cell);
		}
	}
	return OK;
}

Error read_tree(const String &p_directory, bool p_parse, StorageTree &r_tree) {
	Error error = read_entity_directory(p_directory, "global", p_parse, r_tree);
	if (error != OK) {
		return error;
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

void collect_asset_uids(const EntitySchemaRegistry &p_schemas, uint64_t p_type, const Dictionary &p_fields, HashSet<String> &r_uids) {
	const EntityComponentSchema *schema = p_schemas.find(p_type);
	if (!schema) {
		return;
	}
	for (const EntityFieldSchema &field : schema->fields) {
		if (!field.serialized || !p_fields.has(field.key)) {
			continue;
		}
		const Variant value = p_fields[field.key];
		Array entries;
		if (value.get_type() == Variant::ARRAY) {
			entries = value;
		} else {
			entries.push_back(value);
		}
		if (field.nested_type_id) {
			for (int i = 0; i < entries.size(); i++) {
				if (entries[i].get_type() == Variant::DICTIONARY) {
					collect_asset_uids(p_schemas, field.nested_type_id, entries[i], r_uids);
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
			r_uids.insert(text.get_slice("::", 0));
		}
	}
}

void collect_record_uids(const EntitySchemaRegistry &p_schemas, const Variant &p_record, HashSet<String> &r_uids) {
	if (p_record.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Variant components_value = Dictionary(p_record).get("components", Variant());
	if (components_value.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary components = components_value;
	for (const Variant &key : components.get_key_list()) {
		if (key.get_type() != Variant::STRING || components[key].get_type() != Variant::DICTIONARY) {
			continue;
		}
		collect_asset_uids(p_schemas, String(key).hex_to_int(), components[key], r_uids);
	}
}

void scan_entity_dependencies(const String &p_directory, const String &p_relative, const EntitySchemaRegistry &p_schemas, HashSet<String> &r_uids) {
	const String absolute = p_directory.path_join(p_relative);
	if (!DirAccess::dir_exists_absolute(absolute)) {
		return;
	}
	for (const String &name : DirAccess::get_files_at(absolute)) {
		if (name.get_extension().to_lower() != "escn") {
			continue;
		}
		const String path = absolute.path_join(name);
		Variant parsed;
		Error error = EntitySceneIO::read_variant_file(path, parsed);
		if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
			error = ERR_FILE_CORRUPT;
		}
		if (error != OK) {
			ERR_PRINT("Cannot read entity record: " + path);
			continue;
		}
		if (name.ends_with(".cluster.escn")) {
			const Dictionary cluster = parsed;
			for (const Variant &key : cluster.get_key_list()) {
				collect_record_uids(p_schemas, cluster[key], r_uids);
			}
			continue;
		}
		collect_record_uids(p_schemas, parsed, r_uids);
	}
}

void scan_prefab_dependencies(const String &p_directory, HashMap<String, String> &r_uids) {
	const String prefabs = p_directory.path_join("prefabs");
	if (!DirAccess::dir_exists_absolute(prefabs)) {
		return;
	}
	for (const String &name : DirAccess::get_files_at(prefabs)) {
		if (name.get_extension().to_lower() != "escn") {
			continue;
		}
		const String path = prefabs.path_join(name);
		Variant parsed;
		Error error = EntitySceneIO::read_variant_file(path, parsed);
		if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
			error = ERR_FILE_CORRUPT;
		}
		if (error != OK) {
			ERR_PRINT("Cannot read prefab instance: " + path);
			continue;
		}
		const Dictionary instance = parsed;
		const Variant uid = instance.get("uid", Variant());
		if (uid.get_type() != Variant::STRING || !String(uid).begins_with("uid://")) {
			ERR_PRINT("Prefab instance has no valid uid: " + path);
			continue;
		}
		const Variant source = instance.get("path", Variant());
		r_uids.insert(uid, source.get_type() == Variant::STRING ? String(source) : String());
	}
}

} // namespace

String EntitySceneIO::cell_directory(const String &p_grid, int64_t p_x, int64_t p_y, int64_t p_z) {
	return "cells/" + p_grid + "/" + itos(p_x) + "_" + itos(p_y) + "_" + itos(p_z);
}

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

Error EntitySceneIO::reserve_record(const Variant &p_value, const EntityTaskScheduler::Graph &p_job, uint32_t p_depth) {
	if (p_depth > 64 || !p_job.reserve_payload(512)) {
		return ERR_OUT_OF_MEMORY;
	}
	if (p_value.get_type() == Variant::STRING) {
		const String text = p_value;
		return p_job.reserve_payload(uint64_t(text.length() + 1) * 16) ? OK : ERR_OUT_OF_MEMORY;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary dictionary = p_value;
		if (!p_job.reserve_payload(uint64_t(dictionary.size()) * 256)) {
			return ERR_OUT_OF_MEMORY;
		}
		for (const Variant *key = dictionary.next(nullptr); key; key = dictionary.next(key)) {
			Error error = reserve_record(*key, p_job, p_depth + 1);
			if (error == OK) {
				error = reserve_record(dictionary[*key], p_job, p_depth + 1);
			}
			if (error != OK) {
				return error;
			}
		}
	} else if (p_value.get_type() == Variant::ARRAY) {
		const Array array = p_value;
		for (const Variant &value : array) {
			const Error error = reserve_record(value, p_job, p_depth + 1);
			if (error != OK) {
				return error;
			}
		}
	} else if (p_value.get_type() >= Variant::PACKED_BYTE_ARRAY || p_value.get_type() == Variant::OBJECT) {
		return ERR_INVALID_DATA;
	}
	return OK;
}

Error EntitySceneIO::read_variant_file(const String &p_path, Variant &r_value, const EntityTaskScheduler::Graph *p_job) {
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &error);
	if (error != OK) {
		return error;
	}
	const uint64_t length = file->get_length();
	const uint64_t source_bytes = (length + 1) * 2;
	if (length >= UINT32_MAX || (p_job && !p_job->reserve_payload(source_bytes))) {
		return ERR_OUT_OF_MEMORY;
	}
	struct SourceReservation {
		const EntityTaskScheduler::Graph *job;
		uint64_t bytes;
		~SourceReservation() {
			if (job) {
				job->release_payload(bytes);
			}
		}
	} source_reservation{ p_job, source_bytes };
	LocalVector<uint8_t> data;
	data.resize(length + 1);
	if (file->get_buffer(data.ptr(), length) != length) {
		return ERR_FILE_CANT_READ;
	}
	data[length] = 0;
	if (p_job) {
		uint64_t payload = length * 16 + 512;
		uint32_t depth = 0;
		for (uint64_t i = 0; i < length; i++) {
			const uint8_t character = data[i];
			if (character == '"' || character == '\'') {
				while (++i < length && data[i] != character) {
					if (data[i] == '\\') {
						i++;
					}
				}
			} else if (character == ';') {
				while (++i < length && data[i] != '\n') {
				}
			} else if (character == '{' || character == '[' || character == '(') {
				if (++depth > 64) {
					return ERR_FILE_CORRUPT;
				}
				payload += 512;
			} else if (character == '}' || character == ']' || character == ')') {
				if (depth == 0) {
					return ERR_FILE_CORRUPT;
				}
				depth--;
			} else if (character == ',' || character == ':') {
				payload += 256;
			} else if (character >= 'A' && character <= 'Z') {
				char identifier[64];
				uint32_t count = 0;
				while (i < length && ((data[i] >= 'a' && data[i] <= 'z') || (data[i] >= 'A' && data[i] <= 'Z') || (data[i] >= '0' && data[i] <= '9') || data[i] == '_')) {
					if (count == sizeof(identifier) - 1) {
						return ERR_FILE_CORRUPT;
					}
					identifier[count++] = char(data[i++]);
				}
				i--;
				identifier[count] = 0;
				if (strcmp(identifier, "Object") == 0 || strcmp(identifier, "Resource") == 0 || strcmp(identifier, "SubResource") == 0 || strcmp(identifier, "ExtResource") == 0) {
					return ERR_INVALID_DATA;
				}
			}
		}
		// This envelope covers parser/COW growth and retained metadata, not allocator bookkeeping or shared resources.
		if (!p_job->reserve_payload(payload)) {
			return ERR_OUT_OF_MEMORY;
		}
	}
	if (EntityRecordParser::parse_utf8(data.ptr(), length, r_value)) {
		return OK;
	}
	String text;
	text.append_utf8((const char *)data.ptr(), length);
	return decode(text, r_value);
}

String EntitySceneIO::scene_directory(const String &p_path) {
	return p_path.get_basename();
}

String EntitySceneIO::companion_directory(const String &p_path) {
	if (p_path.get_extension().to_lower() != "escn" || is_child_path(p_path)) {
		return String();
	}
	if (ResourceLoader::get_resource_type(p_path) != "EntityScene") {
		return String();
	}
	return scene_directory(p_path);
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
	error = read_tree(directory, true, tree);
	if (error != OK) {
		return error;
	}
	Vector<EntityId> ids;
	ids.reserve(tree.records.size());
	for (const KeyValue<EntityId, Dictionary> &entry : tree.records) {
		ids.push_back(entry.key);
	}
	ids.sort_custom<EntityIdSorter>();
	const EntitySchemaRegistry &schemas = EntitySchemaRegistry::descriptors();
	const String streaming_key = component_key(EntityComponentTraits<EntityStreaming>::id);
	const String name_key = component_key(EntityComponentTraits<EntityName>::id);
	Vector<EntityCatalog::Record> catalog_records;
	catalog_records.resize(ids.size());
	for (int32_t id_index = 0; id_index < ids.size(); id_index++) {
		const EntityId id = ids[id_index];
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
		EntityCatalog::Record &catalog_record = catalog_records.write[id_index];
		catalog_record.deleted = deleted;
		catalog_record.parent = parent;
		catalog_record.order = order_value;
		catalog_record.has_order = true;
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
		const Variant streaming = components.get(streaming_key, Variant());
		if (streaming.get_type() == Variant::DICTIONARY) {
			const Variant grid = Dictionary(streaming).get("1", Variant());
			if (grid.get_type() == Variant::STRING && !String(grid).is_empty()) {
				ERR_FAIL_COND_V_MSG(!scene->grids.has(grid), scene->_fail(id, "streaming/grid", ERR_INVALID_DATA), "Entity uses unknown grid \"" + String(grid) + "\": " + path);
			}
		}
		const Variant name = components.get(name_key, Variant());
		if (name.get_type() == Variant::DICTIONARY) {
			const Variant text = Dictionary(name).get("1", Variant());
			if (text.get_type() == Variant::STRING) {
				section.name = text;
			}
		}
		for (const Variant &component : components.get_key_list()) {
			if (component.get_type() != Variant::STRING || components[component].get_type() != Variant::DICTIONARY || !types.has(component)) {
				ERR_FAIL_V_MSG(scene->_fail(id, String(component), ERR_INVALID_DATA), "Invalid component in entity record: " + path);
			}
			Vector<uint64_t> pending;
			HashSet<uint64_t> seen;
			pending.push_back(String(component).hex_to_int());
			for (int i = 0; i < pending.size(); i++) {
				uint64_t type = pending[i];
				if (seen.has(type)) {
					continue;
				}
				seen.insert(type);
				const EntityComponentSchema *schema = schemas.find(type);
				if (!schema) {
					ERR_FAIL_V_MSG(scene->_fail(id, String(component), ERR_UNAVAILABLE), "Unknown component type in entity record: " + path);
				}
				const String type_id = schema->key;
				if (!types.has(type_id) || types[type_id].get_type() != Variant::DICTIONARY) {
					ERR_FAIL_V_MSG(scene->_fail(id, type_id, ERR_UNAVAILABLE), "Unknown component type in entity record: " + path);
				}
				Dictionary fields = types[type_id];
				int count = 0;
				for (const EntityFieldSchema &field : schema->fields) {
					if (!field.serialized) {
						continue;
					}
					count++;
					if (!fields.has(field.key) || fields[field.key] != String(field.native_type)) {
						ERR_FAIL_V_MSG(scene->_fail(id, type_id + "/" + field.key, ERR_INVALID_DATA), "Entity scene type manifest does not match the engine: " + p_path);
					}
					if (field.nested_type_id) {
						pending.push_back(field.nested_type_id);
					}
				}
				if (count != fields.size()) {
					ERR_FAIL_V_MSG(scene->_fail(id, type_id + "/manifest", ERR_INVALID_DATA), "Entity scene type manifest does not match the engine: " + p_path);
				}
			}
			section.components.push_back(component);
		}
		catalog_record.section = section;
		catalog_record.has_section = true;
		EntityScene::CellKey cell;
		const String section_directory = section.path.get_base_dir();
		if (EntityScene::_parse_cell_directory(section_directory, cell)) {
			catalog_record.cell_grid = cell.grid;
			catalog_record.cell_x = cell.x;
			catalog_record.cell_y = cell.y;
			catalog_record.cell_z = cell.z;
			catalog_record.has_cell = true;
		}
	}
	for (const Variant &instance_key : tree.prefabs.get_key_list()) {
		const Variant instance_value = tree.prefabs[instance_key];
		if (instance_value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Variant mapping_value = Dictionary(instance_value).get("mapping", Variant());
		if (mapping_value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary mapping = mapping_value;
		for (const Variant &source : mapping.get_key_list()) {
			EntityId id;
			if (mapping[source].get_type() != Variant::STRING || EntityId::parse(mapping[source], id) != OK) {
				continue;
			}
			const int index = ids.bsearch_custom<EntityIdSorter>(id, true);
			if (index < ids.size() && ids[index] == id && catalog_records[index].prefab_instance.is_empty()) {
				catalog_records.write[index].prefab_instance = instance_key;
				catalog_records.write[index].prefab_source = source;
			}
		}
	}
	ERR_FAIL_COND_V(scene->catalog.add_block(ids, catalog_records) == UINT32_MAX, scene->catalog.get_last_publish_error() == OK ? ERR_CANT_CREATE : scene->catalog.get_last_publish_error());
	for (EntityId id : scene->catalog.get_ids()) {
		if (scene->catalog.get_record(id)->deleted) {
			continue;
		}
		EntityRef parent = scene->catalog.get_parent(id);
		if (parent.id.is_valid() && scene->catalog.get_state(parent.id) != EntityReferenceState::UNLOADED) {
			return scene->_fail(id, "parent", ERR_INVALID_DATA);
		}
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
	error = scene->_assign_cells(scene->catalog.get_ids());
	ERR_FAIL_COND_V_MSG(error != OK, error, "Entity scene has an invalid storage cell: " + directory + " (" + scene->get_last_error() + ")");
	r_scene = scene;
	return OK;
}

Error EntitySceneIO::save(EntityScene &p_scene, const String &p_path, ResourceUID::ID p_uid) {
	ERR_FAIL_COND_V(p_scene._owner() != OK, ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(p_path.get_extension().to_lower() != "escn", ERR_INVALID_PARAMETER);
	p_scene.flush_streaming();
	for (const Variant &key : p_scene.prefab_instances.get_key_list()) {
		Dictionary instance = p_scene.prefab_instances[key];
		if (!Array(instance["conflicts"]).is_empty()) {
			return p_scene._fail(EntityId(), "prefab/conflicts", ERR_BUSY);
		}
	}
	ERR_FAIL_COND_V_MSG(!p_scene.grids.has(p_scene.default_grid), ERR_INVALID_DATA, "Entity scene default grid \"" + p_scene.default_grid + "\" is not configured: " + p_path);
	const String directory = scene_directory(p_path);
	ERR_FAIL_COND_V_MSG(!p_scene.storage_path.is_empty() && p_scene.storage_path != p_path && DirAccess::dir_exists_absolute(directory), ERR_ALREADY_EXISTS, "Entity scene companion directory already exists: " + directory);
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
	const bool complete = p_scene.storage_path != p_path;
	HashSet<String> copied;
	if (complete && !p_scene.storage_path.is_empty()) {
		const String source = scene_directory(p_scene.storage_path).path_join("cells");
		if (DirAccess::dir_exists_absolute(source)) {
			Ref<DirAccess> access = DirAccess::create_for_path(source);
			ERR_FAIL_COND_V_MSG(access.is_null(), ERR_CANT_CREATE, "Cannot read entity scene cells: " + source);
			for (const String &grid : DirAccess::get_directories_at(source)) {
				ERR_FAIL_COND_V_MSG(!grid_names.has(grid), ERR_FILE_CORRUPT, "Entity scene has no grid named \"" + grid + "\": " + source.path_join(grid));
				const String grid_directory = source.path_join(grid);
				for (const String &name : DirAccess::get_directories_at(grid_directory)) {
					ERR_FAIL_COND_V_MSG(!cell_name_is_valid(name), ERR_FILE_CORRUPT, "Invalid cell directory name: " + grid_directory.path_join(name));
					const String relative = "cells/" + grid + "/" + name;
					EntityScene::CellKey key;
					if (EntityScene::_parse_cell_directory(relative, key) && p_scene.resident_cells.has(key)) {
						continue;
					}
					error = access->copy_dir(grid_directory.path_join(name), directory.path_join(relative));
					if (error != OK) {
						return error;
					}
					copied.insert(relative);
				}
			}
		}
	}
	StorageTree existing;
	error = read_tree(directory, false, existing);
	if (error != OK) {
		return error;
	}
	Vector<EntityId> ids = p_scene.catalog.get_ids();
	ids.sort_custom<EntityIdSorter>();
	auto add_existing = [&](EntityId p_id, const EntityScene::Section &p_section) {
		if (p_section.path.is_empty() || existing.files.has(p_id)) {
			return;
		}
		const String base = p_section.path.get_base_dir();
		EntityScene::CellKey key;
		if (complete && !copied.has(base) && !(p_section.cluster && EntityScene::_parse_cell_directory(base, key) && p_scene.resident_cells.has(key))) {
			return;
		}
		existing.files.insert(p_id, { p_section.path, p_section.cluster });
		if (!p_section.cluster) {
			existing.paths.push_back(p_section.path);
		}
	};
	for (EntityId id : ids) {
		const EntityScene::Section *section = p_scene._get_section(id);
		if (section) {
			add_existing(id, *section);
		}
	}
	for (const KeyValue<EntityId, EntityScene::Section> &entry : p_scene.deleted_storage) {
		add_existing(entry.key, entry.value);
	}
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
	HashSet<EntityId, EntityIdHasher> scope;
	if (complete) {
		for (EntityId id : ids) {
			scope.insert(id);
		}
	} else {
		HashMap<String, Vector<EntityId>> cluster_members;
		for (const KeyValue<EntityId, SceneFile> &entry : existing.files) {
			if (entry.value.cluster) {
				cluster_members[entry.value.path].push_back(entry.key);
			}
		}
		Vector<EntityId> pending;
		for (EntityId id : ids) {
			if (p_scene._is_dirty(id) || !existing.files.has(id)) {
				pending.push_back(id);
			}
		}
		for (int i = 0; i < pending.size(); i++) {
			const EntityId id = pending[i];
			if (scope.has(id)) {
				continue;
			}
			scope.insert(id);
			pending.append_array(p_scene.catalog.get_children(id));
			const SceneFile *file = existing.files.getptr(id);
			if (file && file->cluster) {
				pending.append_array(cluster_members[file->path]);
			}
		}
	}
	HashSet<EntityId, EntityIdHasher> read_scope;
	for (EntityId id : scope) {
		for (EntityId ancestor = id; ancestor.is_valid() && !read_scope.has(ancestor); ancestor = p_scene.catalog.get_parent(ancestor).id) {
			read_scope.insert(ancestor);
		}
	}
	HashMap<EntityId, Dictionary, EntityIdHasher> records;
	HashMap<EntityId, EntityScene::Section, EntityIdHasher> sections;
	for (EntityId id : ids) {
		if (p_scene.catalog.get_state(id) == EntityReferenceState::DELETED || !read_scope.has(id)) {
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
	HashMap<EntityId, String, EntityIdHasher> roots;
	auto root_directory = [&](EntityId p_id, String &r_directory) -> Error {
		const String *cached = roots.getptr(p_id);
		if (cached) {
			r_directory = *cached;
			return OK;
		}
		const Dictionary *record = records.getptr(p_id);
		const SceneFile *file = existing.files.getptr(p_id);
		if (!record || (!scope.has(p_id) && file)) {
			r_directory = file ? file->path.get_base_dir() : String();
			roots.insert(p_id, r_directory);
			return OK;
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
			r_directory = "global";
			roots.insert(p_id, r_directory);
			return OK;
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
		ERR_FAIL_COND_V_MSG(!configuration, p_scene._fail(p_id, "streaming/grid", ERR_INVALID_DATA), "Entity " + p_id.to_string() + " uses unknown grid \"" + grid + "\": " + p_path);
		EntityPose pose;
		const Variant transform = components.get(transform_key, Variant());
		if (transform.get_type() == Variant::DICTIONARY) {
			EntityCodec<EntityPose>::decode(Dictionary(transform).get("1", Variant()), pose);
		}
		const int32_t x = EntityScene::_cell_index(pose.translation.x, configuration->size);
		const int32_t y = EntityScene::_cell_index(pose.translation.y, configuration->size);
		const int32_t z = EntityScene::_cell_index(pose.translation.z, configuration->size);
		r_directory = EntitySceneIO::cell_directory(grid, x, y, z);
		roots.insert(p_id, r_directory);
		return OK;
	};
	HashMap<EntityId, String, EntityIdHasher> directories;
	for (EntityId id : ids) {
		EntityId root = id;
		int guard = p_scene.catalog.get_record_count() + 1;
		while (guard-- > 0) {
			const EntityId parent = p_scene.catalog.get_parent(root).id;
			if (!parent.is_valid() || !p_scene.catalog.has_record(parent)) {
				break;
			}
			root = parent;
		}
		String target;
		error = root_directory(root, target);
		if (error != OK) {
			return error;
		}
		if (!target.is_empty()) {
			directories.insert(id, target);
		}
	}
	HashMap<String, String> texts;
	HashMap<String, Dictionary> clusters;
	HashMap<String, Vector<EntityId>> cluster_previous;
	HashMap<EntityId, String, EntityIdHasher> targets;
	for (EntityId id : ids) {
		if (!scope.has(id)) {
			continue;
		}
		const SceneFile *stale = existing.files.getptr(id);
		if (stale && stale->cluster) {
			cluster_previous[stale->path].push_back(id);
		}
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
		ERR_FAIL_COND_V_MSG(cell.is_empty(), ERR_INVALID_DATA, "Entity " + id.to_string() + " has no storage cell: " + p_path);
		const String target = cell.path_join(id.to_string() + ".escn");
		targets.insert(id, target);
		String text;
		error = encode(stored, text);
		if (error != OK) {
			return error;
		}
		texts.insert(target, text);
	}
	Vector<String> cluster_paths;
	for (const KeyValue<String, Vector<EntityId>> &entry : cluster_previous) {
		cluster_paths.push_back(entry.key);
	}
	for (const KeyValue<String, Dictionary> &entry : clusters) {
		if (!cluster_previous.has(entry.key)) {
			cluster_paths.push_back(entry.key);
		}
	}
	cluster_paths.sort();
	Vector<String> emptied;
	for (const String &path : cluster_paths) {
		const String absolute = directory.path_join(path);
		Dictionary seed;
		if (FileAccess::exists(absolute)) {
			Variant parsed;
			error = read_variant_file(absolute, parsed);
			if (error == OK && parsed.get_type() != Variant::DICTIONARY) {
				error = ERR_FILE_CORRUPT;
			}
			ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot read entity cluster: " + absolute);
			seed = parsed;
		}
		const Vector<EntityId> *previous = cluster_previous.getptr(path);
		if (previous) {
			for (EntityId id : *previous) {
				seed.erase(id.to_string());
			}
		}
		const Dictionary *updates = clusters.getptr(path);
		if (updates) {
			for (const Variant &key : updates->get_key_list()) {
				seed[key] = (*updates)[key];
			}
		}
		if (seed.is_empty()) {
			emptied.push_back(path);
			continue;
		}
		String text;
		error = encode(seed, text);
		if (error != OK) {
			return error;
		}
		texts.insert(path, text);
	}
	for (const String &key : instance_keys) {
		Dictionary instance = Dictionary(p_scene.prefab_instances[key]).duplicate(true);
		Dictionary mapping = instance["mapping"];
		String cell;
		EntityId root;
		for (const Variant &source : mapping.get_key_list()) {
			EntityId id;
			if (EntityId::parse(mapping[source], id) != OK || !directories.has(id) || p_scene.catalog.get_state(id) != EntityReferenceState::UNLOADED) {
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
	for (EntityId id : ids) {
		const SceneFile *file = existing.files.getptr(id);
		if (file && !scope.has(id)) {
			keep.insert(file->path);
		}
	}
	int changes = 0;
	HashSet<String> moved;
	HashSet<String> vacated;
	for (EntityId id : ids) {
		const SceneFile *previous = existing.files.getptr(id);
		const String *target = targets.getptr(id);
		if (!scope.has(id) || !previous || previous->cluster || !target || previous->path == *target) {
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
		vacated.insert(previous->path.get_base_dir());
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
				fields[field.key] = String(field.native_type);
			}
		}
		const String key = entry.value.key;
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
	removed.append_array(emptied);
	for (const String &path : removed) {
		if (keep.has(path) || moved.has(path)) {
			continue;
		}
		const String absolute = directory.path_join(path);
		error = DirAccess::remove_absolute(absolute);
		ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot remove stale entity file: " + absolute);
		vacated.insert(path.get_base_dir());
	}
	for (const String &relative : vacated) {
		if (!relative.begins_with("cells/")) {
			continue;
		}
		const String cell_path = directory.path_join(relative);
		if (!DirAccess::dir_exists_absolute(cell_path) || !DirAccess::get_files_at(cell_path).is_empty() || !DirAccess::get_directories_at(cell_path).is_empty()) {
			continue;
		}
		ERR_CONTINUE_MSG(DirAccess::remove_absolute(cell_path) != OK, "Cannot remove empty cell directory: " + cell_path);
		const String grid_directory = cell_path.get_base_dir();
		if (DirAccess::get_files_at(grid_directory).is_empty() && DirAccess::get_directories_at(grid_directory).is_empty()) {
			ERR_CONTINUE_MSG(DirAccess::remove_absolute(grid_directory) != OK, "Cannot remove empty grid directory: " + grid_directory);
		}
	}
	for (EntityId id : scope) {
		EntityScene::Section *section = sections.getptr(id);
		if (!section) {
			p_scene._erase_section(id);
			continue;
		}
		const String *target = targets.getptr(id);
		if (target) {
			section->path = *target;
			section->cluster = target->ends_with(".cluster.escn");
		}
		*p_scene._edit_section(id) = *section;
	}
	p_scene._relocate(p_path);
	p_scene.revision = revision;
	for (EntityId id : p_scene.catalog.get_ids()) {
		p_scene._set_dirty(id, false);
	}
	p_scene.deleted_storage.clear();
	error = p_scene._assign_cells(ids);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Entity scene has an invalid storage cell: " + directory + " (" + p_scene.get_last_error() + ")");
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
	Ref<ConfigFile> config;
	ERR_FAIL_COND_MSG(read_main(p_path, config) != OK, "Cannot read entity scene: " + p_path);
	const String directory = EntitySceneIO::scene_directory(p_path);
	ERR_FAIL_COND_MSG(!DirAccess::dir_exists_absolute(directory), "Entity scene directory is missing: " + directory);
	HashSet<String> grid_names;
	if (config->has_section("grids")) {
		for (const String &name : config->get_section_keys("grids")) {
			grid_names.insert(name);
		}
	}
	config.unref();
	Vector<String> directories;
	if (collect_entity_directories(directory, grid_names, directories) != OK) {
		return;
	}
	const EntitySchemaRegistry &schemas = EntitySchemaRegistry::descriptors();
	HashSet<String> assets;
	for (const String &relative : directories) {
		scan_entity_dependencies(directory, relative, schemas, assets);
	}
	HashMap<String, String> prefabs;
	scan_prefab_dependencies(directory, prefabs);
	Vector<String> uids;
	uids.reserve(assets.size() + prefabs.size());
	for (const String &uid : assets) {
		uids.push_back(uid);
	}
	for (const KeyValue<String, String> &entry : prefabs) {
		if (!assets.has(entry.key)) {
			uids.push_back(entry.key);
		}
	}
	uids.sort();
	for (const String &uid : uids) {
		const ResourceUID::ID id = ResourceUID::get_singleton()->text_to_id(uid);
		String path = ResourceUID::get_singleton()->has_id(id) ? ResourceUID::get_singleton()->get_id_path(id) : String();
		const bool asset = assets.has(uid);
		if (!asset && path.is_empty()) {
			path = prefabs[uid];
		}
		String type;
		if (p_add_types) {
			type = asset ? (path.is_empty() ? String("Resource") : ResourceLoader::get_resource_type(path)) : String("EntityScene");
		}
		p_dependencies->push_back(uid + "::" + type + "::" + path);
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
	if (p_path.get_extension().to_lower() != "escn" || EntitySceneIO::is_child_path(p_path)) {
		return ERR_FILE_UNRECOGNIZED;
	}
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
