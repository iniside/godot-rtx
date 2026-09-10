#pragma once

#include "entity_catalog.h"
#include "entity_component_schema.gen.h"
#include "entity_component_schema.h"

#include "core/os/thread.h"
#include "core/templates/hash_set.h"

class EntityWorld {
	struct Identity {
		EntityId id;
	};
	struct Resident {
		EntityHandle handle;
		uint64_t revision = 0;
	};

	EntityCatalog &catalog;
	flecs::world ecs;
	EntitySchemaRegistry schemas;
	HashMap<EntityId, Resident, EntityIdHasher> residents;
	HashSet<EntityId, EntityIdHasher> changed;
	uint64_t generation = 0;
	uint64_t change_serial = 0;
	Thread::ID owner_thread = Thread::get_caller_id();

	bool _is_owner() const { return owner_thread == Thread::get_caller_id(); }
	EntityHandle _materialize(EntityId p_id);
	void _mark_changed(EntityId p_id);

public:
	explicit EntityWorld(EntityCatalog &p_catalog);
	~EntityWorld();
	EntityWorld(const EntityWorld &) = delete;
	EntityWorld &operator=(const EntityWorld &) = delete;

	Error create_entity(EntityHandle &r_handle, EntityId p_id = EntityId());
	Error load_entity(EntityId p_id, EntityHandle &r_handle);
	Error restore_entity(EntityId p_id, EntityHandle &r_handle);
	Error unload_entity(EntityHandle p_handle);
	Error delete_entity(EntityId p_id);
	EntityResolution resolve(EntityRef p_reference) const;
	bool is_alive(EntityHandle p_handle) const;
	EntityId get_id(EntityHandle p_handle) const;
	uint64_t get_revision(EntityHandle p_handle) const;
	uint64_t get_generation() const { return generation; }
	int get_resident_count() const { return residents.size(); }
	Vector<EntityId> drain_changed();
	const EntitySchemaRegistry &get_schemas() const { return schemas; }

	template <typename T>
	const T *get(EntityHandle p_handle) const {
		static_assert(EntityComponentTraits<T>::is_component);
		ERR_FAIL_COND_V(!_is_owner(), nullptr);
		if (!is_alive(p_handle)) {
			return nullptr;
		}
		return ecs.entity(p_handle.entity).template try_get<T>();
	}

	template <typename T>
	bool has(EntityHandle p_handle) const { return get<T>(p_handle) != nullptr; }

	template <typename T>
	Error set(EntityHandle p_handle, const T &p_value) {
		static_assert(EntityComponentTraits<T>::is_component);
		ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
		ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
		T value = p_value;
		ecs.entity(p_handle.entity).template set<T>(value);
		_mark_changed(get_id(p_handle));
		return OK;
	}

	template <typename T>
	Error remove(EntityHandle p_handle) {
		static_assert(EntityComponentTraits<T>::is_component);
		ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
		ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
		if (ecs.entity(p_handle.entity).template has<T>()) {
			ecs.entity(p_handle.entity).template remove<T>();
			_mark_changed(get_id(p_handle));
		}
		return OK;
	}

	Error add_component(EntityHandle p_handle, uint64_t p_component);
	Error remove_component(EntityHandle p_handle, uint64_t p_component);
	Error read_component(EntityHandle p_handle, uint64_t p_component, Variant &r_value) const;
	Error write_component(EntityHandle p_handle, uint64_t p_component, const Variant &p_value);
	Error read_field(EntityHandle p_handle, uint64_t p_component, uint64_t p_field, Variant &r_value) const;
	Error write_field(EntityHandle p_handle, uint64_t p_component, uint64_t p_field, const Variant &p_value);
};
