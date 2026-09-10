#pragma once

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/resources/entity_scene.h"

class EntitySceneIO {
public:
	static constexpr uint32_t FORMAT_VERSION = 1;
	static Error encode(const Variant &p_value, PackedByteArray &r_bytes);
	static Error decode(const PackedByteArray &p_bytes, Variant &r_value);
	static Error read_manifest(const String &p_path, Dictionary &r_manifest, Ref<FileAccess> &r_file);
	static Error load(const String &p_path, Ref<EntityScene> &r_scene);
	static Error save(EntityScene &p_scene, const String &p_path, ResourceUID::ID p_uid = ResourceUID::INVALID_ID);
};

class ResourceFormatLoaderEntityScene : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderEntityScene, ResourceFormatLoader);

public:
	Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	void get_recognized_extensions(List<String> *p_extensions) const override;
	bool handles_type(const String &p_type) const override;
	String get_resource_type(const String &p_path) const override;
	ResourceUID::ID get_resource_uid(const String &p_path) const override;
	bool has_custom_uid_support() const override { return true; }
	void get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types = false) override;
	Error rename_dependencies(const String &p_path, const HashMap<String, String> &p_map) override;
};

class ResourceFormatSaverEntityScene : public ResourceFormatSaver {
	GDSOFTCLASS(ResourceFormatSaverEntityScene, ResourceFormatSaver);

public:
	Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	Error set_uid(const String &p_path, ResourceUID::ID p_uid) override;
	bool recognize(const Ref<Resource> &p_resource) const override;
	void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
};
