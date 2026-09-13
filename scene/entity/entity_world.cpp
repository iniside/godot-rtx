#include "entity_world.h"

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_3d/navigation_server_3d.h"
#endif

#include <atomic>

static std::atomic<uint64_t> entity_world_generation{ 1 };

EntityWorld::EntityWorld(EntityCatalog &p_catalog) :
		catalog(p_catalog), generation(entity_world_generation.fetch_add(1)), transforms(*this), rendering(*this) {
	ecs.component<Identity>();
	schemas.bind(ecs);
}

EntityWorld::~EntityWorld() {
	DEV_ASSERT(_is_owner());
	ecs.quit();
	for (EntityCatalog::CellRuntimeBlock *block : catalog.blocks) {
		if (!block) {
			continue;
		}
		for (EntityCatalog::RowState &state : block->states) {
			if (state.resident && ecs_is_alive(ecs.c_ptr(), state.handle.entity)) {
				ecs.entity(state.handle.entity).destruct();
			}
			state.resident = false;
			state.handle = {};
		}
	}
	catalog.resident_records = 0;
	finalize_services();
}

EntityCatalog::RowState *EntityWorld::_resident(EntityId p_id) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	return state && state->resident ? state : nullptr;
}

const EntityCatalog::RowState *EntityWorld::_resident(EntityId p_id) const {
	const EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	return state && state->resident ? state : nullptr;
}

void EntityWorld::_set_resident(EntityId p_id, EntityHandle p_handle) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_id);
	DEV_ASSERT(state);
	if (state) {
		if (!state->resident) {
			catalog.resident_records++;
		}
		state->handle = p_handle;
		state->resident = true;
	}
}

void EntityWorld::_clear_resident(EntityId p_id) {
	_clear_resident(catalog.locate(p_id));
}

void EntityWorld::_clear_resident(EntityCatalog::RowLocation p_location) {
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_location);
	if (state) {
		if (state->resident) {
			catalog.resident_records--;
		}
		state->resident = false;
		state->handle = {};
	}
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
	for (const EntityCatalog::CellRuntimeBlock *block : catalog.blocks) {
		if (!block) {
			continue;
		}
		for (uint32_t row = 0; row < uint32_t(block->ids.size()); row++) {
			if (block->states[row].resident) {
				rendering.mark_dirty(block->ids[row]);
			}
		}
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
	const Identity *identity = ecs.entity(p_handle.entity).try_get<Identity>();
	ERR_FAIL_NULL(identity);
	const uint32_t mask = EntityRenderSystem::component_mask(p_component);
	if (mask & EntityRenderUpdate::POSE) {
		transforms.mark_dirty(p_handle.entity);
	}
	_mark_changed(identity->row, identity->id, mask);
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
	_mark_changed(catalog.locate(p_id), p_id, p_render_mask);
}

void EntityWorld::_mark_changed(EntityCatalog::RowLocation p_location, EntityId p_id, uint32_t p_render_mask) {
	change_serial++;
	EntityCatalog::RowState *state = catalog.get_state_ptr(p_location);
	if (state) {
		state->revision = change_serial;
		state->render_mask |= p_render_mask;
		if (!state->world_changed) {
			state->world_changed = true;
			changed_rows.push_back({ p_location, p_id });
		}
	}
	rendering.mark_dirty(p_id, p_render_mask);
}

EntityHandle EntityWorld::_materialize(EntityId p_id, MaterializeProfile *r_profile) {
	const bool timed = r_profile != nullptr;
	uint64_t began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	const EntityCatalog::RowLocation location = catalog.locate(p_id);
	DEV_ASSERT(location.is_valid());
	flecs::entity entity = ecs.entity().set<Identity>({ p_id, location });
	if (timed) {
		r_profile->ecs_entity_create_identity_usec += OS::get_singleton()->get_ticks_usec() - began;
		r_profile->entities_materialized++;
		began = OS::get_singleton()->get_ticks_usec();
	}
	EntityRef parent = catalog.get_parent(p_id);
	if (timed) {
		r_profile->catalog_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	if (parent.id.is_valid()) {
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		EntityResolution parent_resolution = resolve(parent);
		if (timed) {
			r_profile->catalog_usec += OS::get_singleton()->get_ticks_usec() - began;
		}
		began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
		entity.set<flecs::Parent>({ parent_resolution.handle.entity });
		if (timed) {
			r_profile->ecs_parent_set_usec += OS::get_singleton()->get_ticks_usec() - began;
			r_profile->parent_sets++;
		}
	}
	EntityHandle handle{ generation, entity.id() };
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	_set_resident(p_id, handle);
	if (timed) {
		r_profile->resident_insert_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	began = timed ? OS::get_singleton()->get_ticks_usec() : 0;
	_mark_changed(p_id);
	if (timed) {
		r_profile->initial_dirty_usec += OS::get_singleton()->get_ticks_usec() - began;
	}
	return handle;
}

Error EntityWorld::_materialize_bulk(const LocalVector<EntityId> &p_ids, ecs_bulk_desc_t &r_desc, const ecs_table_t *&r_table, ecs_entity_t &r_storage_tag, MaterializeProfile *r_profile) {
	DEV_ASSERT(_is_owner());
	LocalVector<Identity> identities;
	identities.resize(p_ids.size());
	for (uint32_t i = 0; i < p_ids.size(); i++) {
		DEV_ASSERT(!_resident(p_ids[i]));
		identities[i] = { p_ids[i], catalog.locate(p_ids[i]) };
		DEV_ASSERT(identities[i].row.is_valid());
	}
	r_desc.ids[0] = ecs.id<Identity>();
	r_desc.ids[1] = ecs.id<EntityTransformSystem::State>();
	r_desc.data[0] = identities.ptr();
	r_desc.count = p_ids.size();
	uint64_t began = r_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	ecs_entity_t storage_tag = 0;
	for (ecs_entity_t tag : bulk_storage_tags) {
		if (ecs_count_id(ecs.c_ptr(), tag) == 0) {
			storage_tag = tag;
			break;
		}
	}
	if (!storage_tag) {
		storage_tag = ecs.entity().id();
		bulk_storage_tags.push_back(storage_tag);
	}
	uint32_t tag_index = 2;
	while (r_desc.ids[tag_index]) {
		tag_index++;
	}
	DEV_ASSERT(tag_index + 1 < FLECS_ID_DESC_MAX);
	// An unoccupied tag prevents table growth from relocating earlier live batches, including edited rows.
	r_desc.ids[tag_index] = storage_tag;
	const ecs_entity_t *entities = ecs_bulk_init(ecs.c_ptr(), &r_desc);
	if (r_profile) {
		r_profile->ecs_bulk_create_components_usec += OS::get_singleton()->get_ticks_usec() - began;
		began = OS::get_singleton()->get_ticks_usec();
	}
	ERR_FAIL_NULL_V(entities, ERR_CANT_CREATE);
	r_table = ecs_get_table(ecs.c_ptr(), entities[0]);
	r_storage_tag = storage_tag;
	for (uint32_t i = 0; i < p_ids.size(); i++) {
		_set_resident(p_ids[i], { generation, entities[i] });
	}
	if (r_profile) {
		r_profile->resident_insert_usec += OS::get_singleton()->get_ticks_usec() - began;
		r_profile->entities_materialized += p_ids.size();
	}
	r_desc.data[0] = nullptr;
	r_desc.ids[tag_index] = 0;
	return OK;
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
	ERR_FAIL_COND_V(_resident(p_id), ERR_ALREADY_EXISTS);
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
	catalog.edit_record(p_id)->deleted = false;
	catalog.get_state_ptr(p_id)->tombstone = false;
	catalog._set_parent(p_id, parent);
	r_handle = _materialize(p_id);
	return OK;
}

Error EntityWorld::unload_entity(EntityHandle p_handle) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!is_alive(p_handle), ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(_has_resident_children(p_handle), ERR_BUSY);
	const Identity identity = ecs.entity(p_handle.entity).get<Identity>();
	transforms.forget(p_handle.entity);
	_clear_resident(identity.row);
	ecs.entity(p_handle.entity).destruct();
	_mark_changed(identity.row, identity.id, EntityRenderUpdate::ALL);
	return OK;
}

Error EntityWorld::delete_entity(EntityId p_id) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	EntityCatalog::Record *record = catalog.edit_record(p_id);
	ERR_FAIL_COND_V(!record, ERR_DOES_NOT_EXIST);
	if (record->deleted) {
		return OK;
	}
	ERR_FAIL_COND_V(!catalog.get_children(p_id).is_empty(), ERR_BUSY);
	EntityCatalog::RowState *resident = _resident(p_id);
	if (resident) {
		EntityHandle handle = resident->handle;
		transforms.forget(handle.entity);
		_clear_resident(p_id);
		ecs.entity(handle.entity).destruct();
	}
	record->deleted = true;
	catalog.get_state_ptr(p_id)->tombstone = true;
	catalog._refresh_parent(p_id);
	_mark_changed(p_id);
	return OK;
}

Error EntityWorld::delete_hierarchy(EntityId p_id) {
	ERR_FAIL_COND_V(!_is_owner(), ERR_UNAUTHORIZED);
	ERR_FAIL_COND_V(!catalog.has_record(p_id), ERR_DOES_NOT_EXIST);
	Vector<EntityId> ordered;
	ordered.push_back(p_id);
	for (int i = 0; i < ordered.size(); i++) {
		for (EntityId descendant : catalog.get_children(ordered[i])) {
			ordered.push_back(descendant);
		}
	}
	for (int i = ordered.size() - 1; i >= 0; i--) {
		EntityId id = ordered[i];
		EntityCatalog::RowState *resident = _resident(id);
		if (resident) {
			EntityHandle handle = resident->handle;
			transforms.forget(handle.entity);
			_clear_resident(id);
			ecs.entity(handle.entity).destruct();
		}
		catalog.edit_record(id)->deleted = true;
		catalog.get_state_ptr(id)->tombstone = true;
		catalog._refresh_parent(id);
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
				for (EntityId child : catalog.get_children(pending[i])) {
					pending.push_back(child);
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
	const EntityCatalog::RowState *resident = _resident(p_reference.id);
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
	const EntityCatalog::RowState *resident = catalog.get_state_ptr(identity->row);
	return resident && resident->handle == p_handle;
}

EntityId EntityWorld::get_id(EntityHandle p_handle) const {
	ERR_FAIL_COND_V(!is_alive(p_handle), EntityId());
	return ecs.entity(p_handle.entity).get<Identity>().id;
}

uint64_t EntityWorld::get_revision(EntityHandle p_handle) const {
	ERR_FAIL_COND_V(!is_alive(p_handle), 0);
	const Identity &identity = ecs.entity(p_handle.entity).get<Identity>();
	const EntityCatalog::RowState *resident = catalog.get_state_ptr(identity.row);
	return resident ? resident->revision : 0;
}

Vector<EntityId> EntityWorld::drain_changed() {
	ERR_FAIL_COND_V(!_is_owner(), Vector<EntityId>());
	Vector<EntityId> result;
	result.reserve(changed_rows.size());
	for (const ChangedRow &changed : changed_rows) {
		EntityCatalog::RowState *state = catalog.get_state_ptr(changed.location);
		if (state && state->active && state->world_changed) {
			result.push_back(changed.id);
			state->world_changed = false;
			state->render_mask = 0;
		} else if (!state || !state->active) {
			result.push_back(changed.id);
		}
	}
	changed_rows.clear();
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
