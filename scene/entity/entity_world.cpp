#include "entity_world.h"

#include <atomic>

static std::atomic<uint64_t> entity_world_generation{ 1 };

EntityWorld::EntityWorld(EntityCatalog &p_catalog) :
		catalog(p_catalog), generation(entity_world_generation.fetch_add(1)) {
	ecs.component<Identity>();
	register_entity_component_schemas(ecs, schemas);
}

EntityWorld::~EntityWorld() {
	DEV_ASSERT(_is_owner());
	for (const KeyValue<EntityId, Resident> &entry : residents) {
		ecs.entity(entry.value.handle.entity).destruct();
	}
}

void EntityWorld::_mark_changed(EntityId p_id) {
	change_serial++;
	Resident *resident = residents.getptr(p_id);
	if (resident) {
		resident->revision = change_serial;
	}
	changed.insert(p_id);
}

EntityHandle EntityWorld::_materialize(EntityId p_id) {
	flecs::entity entity = ecs.entity().set<Identity>({ p_id });
	EntityHandle handle{ generation, entity.id() };
	residents.insert(p_id, { handle, 0 });
	_mark_changed(p_id);
	return handle;
}

Error EntityWorld::create_entity(EntityHandle &r_handle, EntityId p_id) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	if (!p_id.is_valid()) {
		Error error = EntityId::generate(p_id);
		if (error != OK) {
			return error;
		}
	}
	Error error = catalog.add_record(p_id);
	if (error != OK) {
		return error;
	}
	r_handle = _materialize(p_id);
	return OK;
}

Error EntityWorld::load_entity(EntityId p_id, EntityHandle &r_handle) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(residents.has(p_id), ERR_ALREADY_EXISTS);
	ERR_FAIL_COND_V(catalog.get_state(p_id) != EntityReferenceState::UNLOADED, ERR_DOES_NOT_EXIST);
	r_handle = _materialize(p_id);
	return OK;
}

Error EntityWorld::restore_entity(EntityId p_id, EntityHandle &r_handle) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(catalog.get_state(p_id) != EntityReferenceState::DELETED, ERR_INVALID_PARAMETER);
	catalog.records[p_id] = false;
	r_handle = _materialize(p_id);
	return OK;
}

Error EntityWorld::unload_entity(EntityHandle p_handle) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	EntityId id = get_id(p_handle);
	residents.erase(id);
	ecs.entity(p_handle.entity).destruct();
	_mark_changed(id);
	return OK;
}

Error EntityWorld::delete_entity(EntityId p_id) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	bool *deleted = catalog.records.getptr(p_id);
	ERR_FAIL_COND_V(!deleted, ERR_DOES_NOT_EXIST);
	if (*deleted) {
		return OK;
	}
	Resident *resident = residents.getptr(p_id);
	if (resident) {
		EntityHandle handle = resident->handle;
		residents.erase(p_id);
		ecs.entity(handle.entity).destruct();
	}
	*deleted = true;
	_mark_changed(p_id);
	return OK;
}

EntityResolution EntityWorld::resolve(EntityRef p_reference) const {
	ERR_FAIL_COND_V(!_is_owner(), EntityResolution());
	const Resident *resident = residents.getptr(p_reference.id);
	if (resident && is_alive(resident->handle)) {
		return { EntityReferenceState::RESIDENT, resident->handle };
	}
	return { catalog.get_state(p_reference.id), {} };
}

bool EntityWorld::is_alive(EntityHandle p_handle) const {
	ERR_FAIL_COND_V(!_is_owner(), false);
	if (p_handle.world_generation != generation || !p_handle.entity || !ecs_is_alive(ecs.c_ptr(), p_handle.entity)) {
		return false;
	}
	const Identity *identity = ecs.entity(p_handle.entity).try_get<Identity>();
	if (!identity) {
		return false;
	}
	const Resident *resident = residents.getptr(identity->id);
	return resident && resident->handle == p_handle;
}

EntityId EntityWorld::get_id(EntityHandle p_handle) const {
	ERR_FAIL_COND_V(!is_alive(p_handle), EntityId());
	return ecs.entity(p_handle.entity).get<Identity>().id;
}

uint64_t EntityWorld::get_revision(EntityHandle p_handle) const {
	ERR_FAIL_COND_V(!is_alive(p_handle), 0);
	return residents[get_id(p_handle)].revision;
}

Vector<EntityId> EntityWorld::drain_changed() {
	ERR_FAIL_COND_V(!_is_owner(), Vector<EntityId>());
	Vector<EntityId> result;
	result.reserve(changed.size());
	for (EntityId id : changed) {
		result.push_back(id);
	}
	changed.clear();
	return result;
}

Error EntityWorld::add_component(EntityHandle p_handle, uint64_t p_component) {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	if (ecs_has_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id)) {
		return ERR_ALREADY_EXISTS;
	}
	schema->add_default(ecs, p_handle.entity);
	_mark_changed(get_id(p_handle));
	return OK;
}

Error EntityWorld::remove_component(EntityHandle p_handle, uint64_t p_component) {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	if (ecs_has_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id)) {
		ecs_remove_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id);
		_mark_changed(get_id(p_handle));
	}
	return OK;
}

Error EntityWorld::read_component(EntityHandle p_handle, uint64_t p_component, Variant &r_value) const {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	const void *component = ecs_get_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id);
	ERR_FAIL_NULL_V(component, ERR_DOES_NOT_EXIST);
	Error error = schema->encode(component, r_value);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot encode entity " + get_id(p_handle).to_string() + " component " + String(schema->name));
	return OK;
}

Error EntityWorld::write_component(EntityHandle p_handle, uint64_t p_component, const Variant &p_value) {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	Error error = schema->set_serialized(ecs, p_handle.entity, p_value);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot decode entity " + get_id(p_handle).to_string() + " component " + String(schema->name));
	_mark_changed(get_id(p_handle));
	return OK;
}

Error EntityWorld::read_field(EntityHandle p_handle, uint64_t p_component, uint64_t p_field, Variant &r_value) const {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	const EntityFieldSchema *field = schema->find_field(p_field);
	ERR_FAIL_NULL_V(field, ERR_DOES_NOT_EXIST);
	const void *component = ecs_get_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id);
	ERR_FAIL_NULL_V(component, ERR_DOES_NOT_EXIST);
	return field->read(component, r_value);
}

Error EntityWorld::write_field(EntityHandle p_handle, uint64_t p_component, uint64_t p_field, const Variant &p_value) {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	Error error = schema->set_field(ecs, p_handle.entity, p_field, p_value);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot edit entity " + get_id(p_handle).to_string() + " component " + String(schema->name));
	_mark_changed(get_id(p_handle));
	return OK;
}
