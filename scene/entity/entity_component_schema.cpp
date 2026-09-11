#include "entity_component_schema.h"

#include "core/os/memory.h"
#include "core/os/os.h"

#include <cstring>

void initialize_entity_types() {
	ecs_os_set_api_defaults();
	ecs_os_api_t api = ecs_os_get_api();
	api.malloc_ = [](ecs_size_t p_size) -> void * { return Memory::alloc_static(p_size); };
	api.calloc_ = [](ecs_size_t p_size) -> void * {
		void *memory = Memory::alloc_static(p_size);
		if (memory) {
			memset(memory, 0, p_size);
		}
		return memory;
	};
	api.realloc_ = [](void *p_memory, ecs_size_t p_size) -> void * { return Memory::realloc_static(p_memory, p_size); };
	api.free_ = [](void *p_memory) {
		if (p_memory) {
			Memory::free_static(p_memory);
		}
	};
	api.strdup_ = [](const char *p_text) -> char * {
		if (!p_text) {
			return nullptr;
		}
		size_t length = strlen(p_text) + 1;
		char *copy = static_cast<char *>(Memory::alloc_static(length));
		if (copy) {
			memcpy(copy, p_text, length);
		}
		return copy;
	};
	ecs_os_set_api(&api);
}

const EntityFieldSchema *EntityComponentSchema::find_field(uint64_t p_id) const {
	for (const EntityFieldSchema &field : fields) {
		if (field.id == p_id) {
			return &field;
		}
	}
	return nullptr;
}

void EntitySchemaRegistry::add(EntityComponentSchema p_schema) {
	ERR_FAIL_COND(components.has(p_schema.id));
	components.insert(p_schema.id, p_schema);
}

Error entity_encode_asset(const Ref<Resource> &p_asset, Variant &r_value) {
	if (p_asset.is_null()) {
		r_value = String();
		return OK;
	}
	String path = p_asset->get_path();
	int separator = path.find("::");
	String resource_path = separator < 0 ? path : path.substr(0, separator);
	ResourceUID::ID uid = ResourceLoader::get_resource_uid(resource_path);
	if (uid == ResourceUID::INVALID_ID) {
		return ERR_FILE_NOT_FOUND;
	}
	r_value = ResourceUID::get_singleton()->id_to_text(uid) + (separator < 0 ? String() : path.substr(separator));
	return OK;
}

static uint64_t entity_asset_usec = 0;
static uint32_t entity_asset_loads = 0;
static uint32_t entity_asset_cache_hits = 0;

void entity_asset_profile_reset() {
	entity_asset_usec = 0;
	entity_asset_loads = 0;
	entity_asset_cache_hits = 0;
}

void entity_asset_profile_get(uint64_t &r_usec, uint32_t &r_loads, uint32_t &r_cache_hits) {
	r_usec = entity_asset_usec;
	r_loads = entity_asset_loads;
	r_cache_hits = entity_asset_cache_hits;
}

Error entity_decode_asset(const Variant &p_value, Ref<Resource> &r_asset) {
	if (p_value.get_type() != Variant::STRING) {
		return ERR_INVALID_DATA;
	}
	String address = p_value;
	if (address.is_empty()) {
		r_asset.unref();
		return OK;
	}
	ResourceUID *uids = ResourceUID::get_singleton();
	int separator = address.find("::");
	ResourceUID::ID uid = uids->text_to_id(separator < 0 ? address : address.substr(0, separator));
	if (uid == ResourceUID::INVALID_ID || !uids->has_id(uid)) {
		return ERR_FILE_NOT_FOUND;
	}
	Error error = OK;
	String path = uids->get_id_path(uid);
	const bool profiling = OS::get_singleton()->is_use_benchmark_set();
	const bool cached = profiling && ResourceCache::has(path);
	const uint64_t load_begin = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	Ref<Resource> container = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
	if (profiling) {
		entity_asset_usec += OS::get_singleton()->get_ticks_usec() - load_begin;
		if (cached) {
			entity_asset_cache_hits++;
		} else {
			entity_asset_loads++;
		}
	}
	if (error != OK || container.is_null()) {
		return error == OK ? ERR_FILE_CORRUPT : error;
	}
	Ref<Resource> asset = separator < 0 ? container : ResourceCache::get_ref(path + address.substr(separator));
	if (asset.is_null()) {
		return ERR_FILE_NOT_FOUND;
	}
	r_asset = asset;
	return OK;
}

ecs_entity_t EntityCodec<Basis>::meta_type(flecs::world &p_world) {
	auto component = p_world.component<Basis>();
	if (!ecs_has_id(p_world.c_ptr(), component.id(), ecs_id(EcsOpaque))) {
		ecs_array_desc_t descriptor = {};
		descriptor.type = flecs::F64;
		descriptor.count = 9;
		ecs_entity_t array_type = ecs_array_init(p_world.c_ptr(), &descriptor);
		component.opaque(array_type).serialize([](const flecs::serializer *p_serializer, const Basis *p_value) -> int {
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					double value = (*p_value)[row][column];
					if (p_serializer->value(flecs::F64, &value) != 0) {
						return -1;
					}
				}
			}
			return 0;
		});
	}
	return component.id();
}
