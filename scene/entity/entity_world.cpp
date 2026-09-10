#include "entity_world.h"

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "servers/rendering/rendering_server.h"

#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_3d/navigation_server_3d.h"
#endif

#include <atomic>

static std::atomic<uint64_t> entity_world_generation{ 1 };

EntityWorld::EntityWorld(EntityCatalog &p_catalog) :
		catalog(p_catalog), generation(entity_world_generation.fetch_add(1)), transforms(*this), rendering(*this) {
	ecs.component<Identity>();
	register_entity_component_schemas(ecs, schemas);
}

EntityWorld::~EntityWorld() {
	DEV_ASSERT(_is_owner());
	ecs.quit();
	for (const KeyValue<EntityId, Resident> &entry : residents) {
		if (ecs_is_alive(ecs.c_ptr(), entry.value.handle.entity)) {
			ecs.entity(entry.value.handle.entity).destruct();
		}
	}
	residents.clear();
	finalize_services();
}

Error EntityWorld::initialize_services() {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(scenario.is_valid(), ERR_ALREADY_IN_USE);
	RenderingServer *server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(server, ERR_UNCONFIGURED);
	scenario = server->scenario_create();
	camera = server->camera_create();
	if (scenario.is_null() || camera.is_null()) {
		finalize_services();
		return ERR_CANT_CREATE;
	}
#ifndef NAVIGATION_3D_DISABLED
	NavigationServer3D *navigation = NavigationServer3D::get_singleton();
	if (navigation) {
		navigation_map = navigation->map_create();
		if (navigation_map.is_null()) {
			finalize_services();
			return ERR_CANT_CREATE;
		}
		navigation->map_set_cell_size(navigation_map, GLOBAL_GET("navigation/3d/default_cell_size"));
		navigation->map_set_cell_height(navigation_map, GLOBAL_GET("navigation/3d/default_cell_height"));
		navigation->map_set_up(navigation_map, GLOBAL_GET("navigation/3d/default_up"));
		navigation->map_set_merge_rasterizer_cell_scale(navigation_map, GLOBAL_GET("navigation/3d/merge_rasterizer_cell_scale"));
		navigation->map_set_use_edge_connections(navigation_map, GLOBAL_GET("navigation/3d/use_edge_connections"));
		navigation->map_set_edge_connection_margin(navigation_map, GLOBAL_GET("navigation/3d/default_edge_connection_margin"));
		navigation->map_set_link_connection_radius(navigation_map, GLOBAL_GET("navigation/3d/default_link_connection_radius"));
		navigation->map_set_active(navigation_map, true);
	}
#endif
	for (const KeyValue<EntityId, Resident> &entry : residents) {
		rendering.mark_dirty(entry.key);
	}
	return OK;
}

void EntityWorld::finalize_services() {
	DEV_ASSERT(_is_owner());
	rendering.release();
#ifndef NAVIGATION_3D_DISABLED
	if (navigation_map.is_valid()) {
		NavigationServer3D::get_singleton()->map_set_active(navigation_map, false);
		NavigationServer3D::get_singleton()->free_rid(navigation_map);
		navigation_map = RID();
	}
#endif
	RenderingServer *server = RenderingServer::get_singleton();
	if (camera.is_valid()) {
		server->free_rid(camera);
		camera = RID();
	}
	if (scenario.is_valid()) {
		server->free_rid(scenario);
		scenario = RID();
	}
	fallback_environment.unref();
}

Error EntityWorld::load_default_environment() {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(scenario.is_null(), ERR_UNCONFIGURED);
	String path = String(GLOBAL_GET("rendering/environment/defaults/default_environment")).strip_edges();
	Ref<Environment> environment;
	if (!path.is_empty()) {
		environment = ResourceLoader::load(path);
		ERR_FAIL_COND_V(environment.is_null(), ERR_CANT_OPEN);
	}
	RenderingServer::get_singleton()->scenario_set_fallback_environment(scenario, environment.is_valid() ? environment->get_rid() : RID());
	fallback_environment = environment;
	return OK;
}

void EntityWorld::_component_changed(EntityHandle p_handle, uint64_t p_component) {
	uint32_t mask = EntityRenderSystem::component_mask(p_component);
	if (mask & EntityRenderUpdate::POSE) {
		transforms.mark_dirty(p_handle.entity);
	}
	_mark_changed(get_id(p_handle), mask);
}

bool EntityWorld::_has_resident_children(EntityHandle p_handle) const {
	ecs_iter_t children = ecs_children(ecs.c_ptr(), p_handle.entity);
	while (ecs_children_next(&children)) {
		if (children.count) {
			ecs_iter_fini(&children);
			return true;
		}
	}
	return false;
}

void EntityWorld::_mark_changed(EntityId p_id, uint32_t p_render_mask) {
	change_serial++;
	Resident *resident = residents.getptr(p_id);
	if (resident) {
		resident->revision = change_serial;
	}
	changed.insert(p_id);
	rendering.mark_dirty(p_id, p_render_mask);
}

EntityHandle EntityWorld::_materialize(EntityId p_id) {
	flecs::entity entity = ecs.entity().set<Identity>({ p_id });
	EntityRef parent = catalog.get_parent(p_id);
	if (parent.id.is_valid()) {
		entity.set<flecs::Parent>({ resolve(parent).handle.entity });
	}
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
	EntityRef parent = catalog.get_parent(p_id);
	ERR_FAIL_COND_V(parent.id.is_valid() && resolve(parent).state != EntityReferenceState::RESIDENT, ERR_UNAVAILABLE);
	r_handle = _materialize(p_id);
	return OK;
}

Error EntityWorld::restore_entity(EntityId p_id, EntityHandle &r_handle) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(catalog.get_state(p_id) != EntityReferenceState::DELETED, ERR_INVALID_PARAMETER);
	EntityRef parent = catalog.get_parent(p_id);
	ERR_FAIL_COND_V(parent.id.is_valid() && resolve(parent).state != EntityReferenceState::RESIDENT, ERR_UNAVAILABLE);
	catalog.records[p_id].deleted = false;
	catalog._set_parent(p_id, parent);
	r_handle = _materialize(p_id);
	return OK;
}

Error EntityWorld::unload_entity(EntityHandle p_handle) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(_has_resident_children(p_handle), ERR_BUSY);
	EntityId id = get_id(p_handle);
	transforms.forget(p_handle.entity);
	residents.erase(id);
	ecs.entity(p_handle.entity).destruct();
	_mark_changed(id);
	return OK;
}

Error EntityWorld::delete_entity(EntityId p_id) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	EntityCatalog::Record *record = catalog.records.getptr(p_id);
	ERR_FAIL_COND_V(!record, ERR_DOES_NOT_EXIST);
	if (record->deleted) {
		return OK;
	}
	ERR_FAIL_COND_V(catalog.children.has(p_id), ERR_BUSY);
	Resident *resident = residents.getptr(p_id);
	if (resident) {
		EntityHandle handle = resident->handle;
		transforms.forget(handle.entity);
		residents.erase(p_id);
		ecs.entity(handle.entity).destruct();
	}
	record->deleted = true;
	catalog._unlink_parent(p_id);
	_mark_changed(p_id);
	return OK;
}

Error EntityWorld::delete_hierarchy(EntityId p_id) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!catalog.records.has(p_id), ERR_DOES_NOT_EXIST);
	Vector<EntityId> ordered;
	ordered.push_back(p_id);
	for (int i = 0; i < ordered.size(); i++) {
		const HashSet<EntityId, EntityIdHasher> *descendants = catalog.children.getptr(ordered[i]);
		if (descendants) {
			for (EntityId descendant : *descendants) {
				ordered.push_back(descendant);
			}
		}
	}
	for (int i = ordered.size() - 1; i >= 0; i--) {
		EntityId id = ordered[i];
		Resident *resident = residents.getptr(id);
		if (resident) {
			EntityHandle handle = resident->handle;
			transforms.forget(handle.entity);
			residents.erase(id);
			ecs.entity(handle.entity).destruct();
		}
		catalog.records[id].deleted = true;
		catalog._unlink_parent(id);
		_mark_changed(id);
	}
	return OK;
}

Error EntityWorld::reparent(EntityHandle p_handle, EntityRef p_parent, ReparentMode p_mode) {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(p_mode != KEEP_WORLD && p_mode != KEEP_LOCAL, ERR_INVALID_PARAMETER);
	EntityId id = get_id(p_handle);
	EntityResolution parent = resolve(p_parent);
	ERR_FAIL_COND_V(p_parent.id.is_valid() && parent.state != EntityReferenceState::RESIDENT, ERR_UNAVAILABLE);
	for (EntityRef ancestor = p_parent; ancestor.id.is_valid(); ancestor = catalog.get_parent(ancestor.id)) {
		ERR_FAIL_COND_V(ancestor.id == id, ERR_CYCLIC_LINK);
	}
	transforms.update();
	struct LocalChange {
		EntityHandle handle;
		EntityPose local;
	};
	Vector<LocalChange> local_changes;
	if (p_mode == KEEP_WORLD) {
		EntityPose parent_pose;
		for (EntityRef ancestor = p_parent; ancestor.id.is_valid(); ancestor = catalog.get_parent(ancestor.id)) {
			const EntityTransform *parent_transform = get<EntityTransform>(resolve(ancestor).handle);
			if (parent_transform) {
				parent_pose = parent_transform->current;
				break;
			}
		}
		Vector<EntityId> pending;
		pending.push_back(id);
		for (int i = 0; i < pending.size(); i++) {
			EntityResolution descendant = resolve({ pending[i] });
			ERR_FAIL_COND_V(descendant.state != EntityReferenceState::RESIDENT, ERR_UNAVAILABLE);
			const EntityTransform *transform = get<EntityTransform>(descendant.handle);
			if (transform) {
				EntityPose local;
				Error error = EntityTransformSystem::relative_to(transform->current, parent_pose, local);
				if (error != OK) {
					return error;
				}
				local_changes.push_back({ descendant.handle, local });
			} else {
				const HashSet<EntityId, EntityIdHasher> *children = catalog.children.getptr(pending[i]);
				if (children) {
					for (EntityId child : *children) {
						pending.push_back(child);
					}
				}
			}
		}
	}
	flecs::entity entity = ecs.entity(p_handle.entity);
	if (p_parent.id.is_valid()) {
		entity.set<flecs::Parent>({ parent.handle.entity });
	} else {
		entity.remove<flecs::Parent>();
	}
	catalog._set_parent(id, p_parent);
	for (const LocalChange &change : local_changes) {
		ecs.entity(change.handle.entity).get_mut<EntityTransform>().local = change.local;
		if (!(change.handle == p_handle)) {
			_component_changed(change.handle);
		}
	}
	_component_changed(p_handle);
	return OK;
}

Error EntityWorld::teleport(EntityHandle p_handle, const EntityPose &p_local) {
	Error error = edit<EntityTransform>(p_handle, [&](EntityTransform &p_transform) { p_transform.local = p_local; });
	if (error == OK) {
		transforms.teleport(p_handle.entity);
	}
	return error;
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
	_component_changed(p_handle, p_component);
	return OK;
}

Error EntityWorld::remove_component(EntityHandle p_handle, uint64_t p_component) {
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	const EntityComponentSchema *schema = schemas.find(p_component);
	ERR_FAIL_NULL_V(schema, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!schema->is_component, ERR_INVALID_PARAMETER);
	if (ecs_has_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id)) {
		if (p_component == EntityComponentTraits<EntityTransform>::id) {
			transforms.teleport(p_handle.entity);
		}
		ecs_remove_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id);
		_component_changed(p_handle, p_component);
	}
	return OK;
}

bool EntityWorld::has_component(EntityHandle p_handle, uint64_t p_component) const {
	ERR_FAIL_COND_V(!_is_owner(), false);
	const EntityComponentSchema *schema = schemas.find(p_component);
	return is_alive(p_handle) && schema && schema->is_component && ecs_has_id(ecs.c_ptr(), p_handle.entity, schema->runtime_id);
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
	if (p_component == EntityComponentTraits<EntityTransform>::id) {
		EntityTransform value;
		Error error = schema->decode(&value, p_value);
		ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot decode entity " + get_id(p_handle).to_string() + " component " + String(schema->name));
		return set<EntityTransform>(p_handle, value);
	}
	Error error = schema->set_serialized(ecs, p_handle.entity, p_value);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot decode entity " + get_id(p_handle).to_string() + " component " + String(schema->name));
	_component_changed(p_handle, p_component);
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
	_component_changed(p_handle, p_component);
	return OK;
}
