/**************************************************************************/
/*  renderer_scene_cull.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "renderer_scene_cull.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/math/geometry_3d.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "scene/entity/entity_render_system.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/rendering_light_culler.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

#ifndef XR_DISABLED
#include "servers/xr/xr_interface.h"
#include "servers/xr/xr_server.h"
#endif

//#define DEBUG_CULL_TIME

/* EVENT QUEUING */

void RendererSceneCull::tick() {
	if (_interpolation_data.interpolation_enabled) {
		update_interpolation_tick(true);
	}
}

void RendererSceneCull::pre_draw(bool p_will_draw) {
	_collect_retired_entity_assets();
	if (_interpolation_data.interpolation_enabled) {
		update_interpolation_frame(p_will_draw);
	}
}

/* CAMERA API */

RID RendererSceneCull::camera_allocate() {
	return camera_owner.allocate_rid();
}
void RendererSceneCull::camera_initialize(RID p_rid) {
	camera_owner.initialize_rid(p_rid);
}

void RendererSceneCull::camera_set_perspective(RID p_camera, float p_fovy_degrees, float p_z_near, float p_z_far) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->type = Camera::PERSPECTIVE;
	camera->fov = p_fovy_degrees;
	camera->znear = p_z_near;
	camera->zfar = p_z_far;
}

void RendererSceneCull::camera_set_orthogonal(RID p_camera, float p_size, float p_z_near, float p_z_far) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->type = Camera::ORTHOGONAL;
	camera->size = p_size;
	camera->znear = p_z_near;
	camera->zfar = p_z_far;
}

void RendererSceneCull::camera_set_frustum(RID p_camera, float p_size, Vector2 p_offset, float p_z_near, float p_z_far) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->type = Camera::FRUSTUM;
	camera->size = p_size;
	camera->offset = p_offset;
	camera->znear = p_z_near;
	camera->zfar = p_z_far;
}

void RendererSceneCull::camera_set_transform(RID p_camera, const Transform3D &p_transform) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);

	camera->transform = p_transform.orthonormalized();
	for (int axis = 0; axis < 3; axis++) {
		camera->origin[axis] = p_transform.origin[axis];
	}
}

void RendererSceneCull::camera_set_cull_mask(RID p_camera, uint32_t p_layers) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);

	camera->visible_layers = p_layers;
}

void RendererSceneCull::camera_set_environment(RID p_camera, RID p_env) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->env = p_env;
}

void RendererSceneCull::camera_set_camera_attributes(RID p_camera, RID p_attributes) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->attributes = p_attributes;
}

void RendererSceneCull::camera_set_compositor(RID p_camera, RID p_compositor) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->compositor = p_compositor;
}

void RendererSceneCull::camera_set_use_vertical_aspect(RID p_camera, bool p_enable) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	camera->vaspect = p_enable;
}

bool RendererSceneCull::is_camera(RID p_camera) const {
	return camera_owner.owns(p_camera);
}

/* OCCLUDER API */

RID RendererSceneCull::occluder_allocate() {
	return RendererSceneOcclusionCull::get_singleton()->occluder_allocate();
}

void RendererSceneCull::occluder_initialize(RID p_rid) {
	RendererSceneOcclusionCull::get_singleton()->occluder_initialize(p_rid);
}

void RendererSceneCull::occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) {
	RendererSceneOcclusionCull::get_singleton()->occluder_set_mesh(p_occluder, p_vertices, p_indices);
}

/* SCENARIO API */

void RendererSceneCull::_instance_pair(Instance *p_A, Instance *p_B) {
	RendererSceneCull *self = (RendererSceneCull *)singleton;
	Instance *A = p_A;
	Instance *B = p_B;

	//instance indices are designed so greater always contains lesser
	if (A->base_type > B->base_type) {
		SWAP(A, B); //lesser always first
	}

	if (B->base_type == RSE::INSTANCE_LIGHT && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightData *light = static_cast<InstanceLightData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		if (!(light->cull_mask & A->layer_mask)) {
			// Early return if the object's layer mask doesn't match the light's cull mask.
			return;
		}

		geom->lights.insert(B);
		light->geometries.insert(A);

		if (geom->can_cast_shadows) {
			light->make_shadow_dirty();
		}

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_LIGHTING_DIRTY;
		}

		if (light->uses_projector) {
			geom->projector_count++;
			if (geom->projector_count == 1) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

		if (light->uses_softshadow) {
			geom->softshadow_count++;
			if (geom->softshadow_count == 1) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

	} else if (self->geometry_instance_pair_mask & (1 << RSE::INSTANCE_REFLECTION_PROBE) && B->base_type == RSE::INSTANCE_REFLECTION_PROBE && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		if (!(A->layer_mask & RSG::light_storage->reflection_probe_get_reflection_mask(B->base))) {
			// Early return if the object's layer mask doesn't match the reflection mask.
			return;
		}

		InstanceReflectionProbeData *reflection_probe = static_cast<InstanceReflectionProbeData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->reflection_probes.insert(B);
		reflection_probe->geometries.insert(A);

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_REFLECTION_DIRTY;
		}

	} else if (self->geometry_instance_pair_mask & (1 << RSE::INSTANCE_DECAL) && B->base_type == RSE::INSTANCE_DECAL && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		if (!(decal->cull_mask & A->layer_mask)) {
			// Early return if the object's layer mask doesn't match the decal's cull mask.
			return;
		}

		geom->decals.insert(B);
		decal->geometries.insert(A);

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_DECAL_DIRTY;
		}

	} else if (B->base_type == RSE::INSTANCE_LIGHTMAP && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		if (A->dynamic_gi) {
			geom->lightmap_captures.insert(B);
			lightmap_data->geometries.insert(A);

			if (A->scenario && A->array_index >= 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_LIGHTMAP_CAPTURE;
			}
			((RendererSceneCull *)self)->_instance_queue_update(A, false, false); //need to update capture
		}

	} else if (self->geometry_instance_pair_mask & (1 << RSE::INSTANCE_VOXEL_GI) && B->base_type == RSE::INSTANCE_VOXEL_GI && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->voxel_gi_instances.insert(B);

		if (A->dynamic_gi) {
			voxel_gi->dynamic_geometries.insert(A);
		} else {
			voxel_gi->geometries.insert(A);
		}

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_VOXEL_GI_DIRTY;
		}

	} else if (B->base_type == RSE::INSTANCE_VOXEL_GI && A->base_type == RSE::INSTANCE_LIGHT) {
		InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(B->base_data);
		voxel_gi->lights.insert(A);
	} else if (B->base_type == RSE::INSTANCE_PARTICLES_COLLISION && A->base_type == RSE::INSTANCE_PARTICLES) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(B->base_data);

		if ((collision->cull_mask & A->layer_mask)) {
			RSG::particles_storage->particles_add_collision(A->base, collision->instance);
		}
	}
}

void RendererSceneCull::_instance_unpair(Instance *p_A, Instance *p_B) {
	RendererSceneCull *self = singleton;
	Instance *A = p_A;
	Instance *B = p_B;

	//instance indices are designed so greater always contains lesser
	if (A->base_type > B->base_type) {
		SWAP(A, B); //lesser always first
	}

	if (B->base_type == RSE::INSTANCE_LIGHT && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightData *light = static_cast<InstanceLightData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		if (!(light->cull_mask & A->layer_mask)) {
			// Early return if the object's layer mask doesn't match the light's cull mask.
			return;
		}

		geom->lights.erase(B);
		light->geometries.erase(A);

		if (geom->can_cast_shadows) {
			light->make_shadow_dirty();
		}

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_LIGHTING_DIRTY;
		}

		if (light->uses_projector) {
#ifdef DEBUG_ENABLED
			if (geom->projector_count == 0) {
				ERR_PRINT("geom->projector_count==0 - BUG!");
			}
#endif
			geom->projector_count--;
			if (geom->projector_count == 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

		if (light->uses_softshadow) {
#ifdef DEBUG_ENABLED
			if (geom->softshadow_count == 0) {
				ERR_PRINT("geom->softshadow_count==0 - BUG!");
			}
#endif
			geom->softshadow_count--;
			if (geom->softshadow_count == 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

	} else if (self->geometry_instance_pair_mask & (1 << RSE::INSTANCE_REFLECTION_PROBE) && B->base_type == RSE::INSTANCE_REFLECTION_PROBE && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceReflectionProbeData *reflection_probe = static_cast<InstanceReflectionProbeData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->reflection_probes.erase(B);
		reflection_probe->geometries.erase(A);

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_REFLECTION_DIRTY;
		}

	} else if (self->geometry_instance_pair_mask & (1 << RSE::INSTANCE_DECAL) && B->base_type == RSE::INSTANCE_DECAL && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		if (!(decal->cull_mask & A->layer_mask)) {
			// Early return if the object's layer mask doesn't match the decal's cull mask.
			return;
		}

		geom->decals.erase(B);
		decal->geometries.erase(A);

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_DECAL_DIRTY;
		}

	} else if (B->base_type == RSE::INSTANCE_LIGHTMAP && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);
		if (A->dynamic_gi) {
			geom->lightmap_captures.erase(B);

			if (geom->lightmap_captures.is_empty() && A->scenario && A->array_index >= 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags &= ~InstanceData::FLAG_LIGHTMAP_CAPTURE;
			}

			lightmap_data->geometries.erase(A);
			self->_instance_queue_update(A, false, false); //need to update capture
		}

	} else if (self->geometry_instance_pair_mask & (1 << RSE::INSTANCE_VOXEL_GI) && B->base_type == RSE::INSTANCE_VOXEL_GI && ((1 << A->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->voxel_gi_instances.erase(B);
		if (A->dynamic_gi) {
			voxel_gi->dynamic_geometries.erase(A);
		} else {
			voxel_gi->geometries.erase(A);
		}

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_VOXEL_GI_DIRTY;
		}

	} else if (B->base_type == RSE::INSTANCE_VOXEL_GI && A->base_type == RSE::INSTANCE_LIGHT) {
		InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(B->base_data);
		voxel_gi->lights.erase(A);
	} else if (B->base_type == RSE::INSTANCE_PARTICLES_COLLISION && A->base_type == RSE::INSTANCE_PARTICLES) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(B->base_data);

		if ((collision->cull_mask & A->layer_mask)) {
			RSG::particles_storage->particles_remove_collision(A->base, collision->instance);
		}
	}
}

void RendererSceneCull::Scenario::shadow_caster_dirty(const AABB &p_box) {
	const uint64_t generation = ++shadow_caster_generation;
	if (shadow_caster_log_size == SHADOW_CASTER_LOG_CAPACITY) {
		// The dropped entry is no longer scannable, so cascades older than it must refresh.
		shadow_caster_overflow_generation = MAX(shadow_caster_overflow_generation, shadow_caster_log[shadow_caster_log_first].generation);
		shadow_caster_log_first = (shadow_caster_log_first + 1) % SHADOW_CASTER_LOG_CAPACITY;
		shadow_caster_log_size--;
	}
	ShadowCasterLogEntry &entry = shadow_caster_log[(shadow_caster_log_first + shadow_caster_log_size) % SHADOW_CASTER_LOG_CAPACITY];
	entry.generation = generation;
	entry.box = p_box;
	shadow_caster_log_size++;
}

void RendererSceneCull::Scenario::shadow_caster_dirty_all() {
	shadow_caster_overflow_generation = ++shadow_caster_generation;
}

bool RendererSceneCull::Scenario::shadow_casters_intersect(uint64_t p_generation, const Basis &p_basis, const double *p_origin, const Vector3 &p_minimum, const Vector3 &p_maximum, real_t p_margin) const {
	if (p_generation < shadow_caster_overflow_generation) {
		return true;
	}

	const Basis inverse_basis = p_basis.transposed();
	for (uint32_t i = 0; i < shadow_caster_log_size; i++) {
		const ShadowCasterLogEntry &entry = shadow_caster_log[(shadow_caster_log_first + i) % SHADOW_CASTER_LOG_CAPACITY];
		if (entry.generation <= p_generation) {
			continue;
		}

		Vector3 box_minimum;
		Vector3 box_maximum;
		for (int corner = 0; corner < 8; corner++) {
			Vector3 point;
			for (int axis = 0; axis < 3; axis++) {
				const double extent = ((corner >> axis) & 1) ? double(entry.box.size[axis]) : 0.0;
				point[axis] = double(entry.box.position[axis]) + extent - p_origin[axis];
			}
			const Vector3 caster = inverse_basis.xform(point);
			if (corner == 0) {
				box_minimum = caster;
				box_maximum = caster;
			} else {
				for (int axis = 0; axis < 3; axis++) {
					box_minimum[axis] = MIN(box_minimum[axis], caster[axis]);
					box_maximum[axis] = MAX(box_maximum[axis], caster[axis]);
				}
			}
		}

		bool separated = false;
		for (int axis = 0; axis < 2; axis++) {
			if (box_maximum[axis] < p_minimum[axis] - p_margin || box_minimum[axis] > p_maximum[axis] + p_margin) {
				separated = true;
			}
		}
		// The cascade culls casters up to the light with an open far plane, so only the near side of z bounds them.
		if (box_maximum.z < p_minimum.z - p_margin) {
			separated = true;
		}
		if (!separated) {
			return true;
		}
	}

	return false;
}

RID RendererSceneCull::scenario_allocate() {
	return scenario_owner.allocate_rid();
}
void RendererSceneCull::scenario_initialize(RID p_rid) {
	scenario_owner.initialize_rid(p_rid);

	Scenario *scenario = scenario_owner.get_or_null(p_rid);
	scenario->self = p_rid;

	scenario->reflection_probe_shadow_atlas = RSG::light_storage->shadow_atlas_create();
	RSG::light_storage->shadow_atlas_set_size(scenario->reflection_probe_shadow_atlas, 1024); //make enough shadows for close distance, don't bother with rest
	RSG::light_storage->shadow_atlas_set_quadrant_subdivision(scenario->reflection_probe_shadow_atlas, 0, 4);
	RSG::light_storage->shadow_atlas_set_quadrant_subdivision(scenario->reflection_probe_shadow_atlas, 1, 4);
	RSG::light_storage->shadow_atlas_set_quadrant_subdivision(scenario->reflection_probe_shadow_atlas, 2, 4);
	RSG::light_storage->shadow_atlas_set_quadrant_subdivision(scenario->reflection_probe_shadow_atlas, 3, 8);

	scenario->reflection_atlas = RSG::light_storage->reflection_atlas_create();

	scenario->instance_aabbs.set_page_pool(&instance_aabb_page_pool);
	scenario->instance_data.set_page_pool(&instance_data_page_pool);
	scenario->instance_visibility.set_page_pool(&instance_visibility_data_page_pool);

	RendererSceneOcclusionCull::get_singleton()->add_scenario(p_rid);
}

void RendererSceneCull::scenario_set_environment(RID p_scenario, RID p_environment) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	scenario->environment = p_environment;
}

void RendererSceneCull::scenario_set_camera_attributes(RID p_scenario, RID p_camera_attributes) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	scenario->camera_attributes = p_camera_attributes;
}

void RendererSceneCull::scenario_set_compositor(RID p_scenario, RID p_compositor) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	scenario->compositor = p_compositor;
}

void RendererSceneCull::scenario_set_fallback_environment(RID p_scenario, RID p_environment) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	scenario->fallback_environment = p_environment;
}

void RendererSceneCull::scenario_set_reflection_atlas_size(RID p_scenario, int p_reflection_size, int p_reflection_count) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	RSG::light_storage->reflection_atlas_set_size(scenario->reflection_atlas, p_reflection_size, p_reflection_count);
}

bool RendererSceneCull::is_scenario(RID p_scenario) const {
	return scenario_owner.owns(p_scenario);
}

RID RendererSceneCull::scenario_get_environment(RID p_scenario) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL_V(scenario, RID());
	return scenario->environment;
}

void RendererSceneCull::scenario_remove_viewport_visibility_mask(RID p_scenario, RID p_viewport) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	if (!scenario->viewport_visibility_masks.has(p_viewport)) {
		return;
	}

	uint64_t mask = scenario->viewport_visibility_masks[p_viewport];
	scenario->used_viewport_visibility_bits &= ~mask;
	scenario->viewport_visibility_masks.erase(p_viewport);
}

void RendererSceneCull::scenario_add_viewport_visibility_mask(RID p_scenario, RID p_viewport) {
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	ERR_FAIL_COND(scenario->viewport_visibility_masks.has(p_viewport));

	uint64_t new_mask = 1;
	while (new_mask & scenario->used_viewport_visibility_bits) {
		new_mask <<= 1;
	}

	if (new_mask == 0) {
		ERR_PRINT("Only 64 viewports per scenario allowed when using visibility ranges.");
		new_mask = ((uint64_t)1) << 63;
	}

	scenario->viewport_visibility_masks[p_viewport] = new_mask;
	scenario->used_viewport_visibility_bits |= new_mask;
}

/* INSTANCING API */

void RendererSceneCull::_instance_queue_update(Instance *p_instance, bool p_update_aabb, bool p_update_dependencies) const {
	singleton->_instance_update_cull_domain(p_instance);
	if (p_update_aabb) {
		p_instance->update_aabb = true;
	}
	if (p_update_dependencies) {
		p_instance->update_dependencies = true;
	}

	if (p_instance->update_item.in_list()) {
		return;
	}

	_instance_update_list.add(&p_instance->update_item);
}

RID RendererSceneCull::_render_slot_allocate() {
	return instance_owner.allocate_rid();
}
void RendererSceneCull::_render_slot_initialize(RID p_rid) {
	instance_owner.initialize_rid(p_rid);
	Instance *instance = instance_owner.get_or_null(p_rid);
	instance->self = p_rid;
	instance->render_handle = p_rid;
}

RID RendererSceneCull::tool_render_allocate() {
	return instance_owner.allocate_rid();
}

void RendererSceneCull::tool_render_initialize(RID p_handle) {
	_render_slot_initialize(p_handle);
	instance_owner.get_or_null(p_handle)->tool = true;
}

void RendererSceneCull::tool_render_update(const ToolRenderData &p_data) {
	Instance *slot = instance_owner.get_or_null(p_data.handle);
	ERR_FAIL_COND(!slot || !slot->tool);
	const bool authored_changed = slot->base != p_data.base || slot->layer_mask != p_data.layers || slot->material_override != p_data.material_override || slot->cast_shadows != p_data.cast_shadows || slot->baked_light != p_data.baked_light || slot->dynamic_gi != p_data.dynamic_gi || slot->extra_margin != p_data.extra_margin || slot->sorting_offset != p_data.sorting_offset || slot->use_aabb_center != p_data.use_aabb_center || slot->ignore_occlusion_culling != p_data.ignore_occlusion_culling || slot->ignore_all_culling != p_data.ignore_all_culling || slot->skeleton != p_data.skeleton;
	const bool pose_changed = slot->transform != p_data.transform;
	if (slot->base != p_data.base) {
		_render_slot_replace_base(p_data.handle, p_data.base);
	}
	if (slot->dynamic_gi != p_data.dynamic_gi && slot->indexer_id.is_valid()) {
		_unpair_instance(slot);
	}
	slot->transform = p_data.transform;
	for (int i = 0; i < 3; i++) {
		slot->origin[i] = p_data.transform.origin[i];
	}
	slot->render_handle = p_data.handle;
	slot->layer_mask = p_data.layers;
	slot->material_override = p_data.material_override;
	slot->cast_shadows = p_data.cast_shadows;
	slot->baked_light = p_data.baked_light;
	slot->dynamic_gi = p_data.dynamic_gi;
	slot->extra_margin = p_data.extra_margin;
	slot->sorting_offset = p_data.sorting_offset;
	slot->use_aabb_center = p_data.use_aabb_center;
	slot->ignore_occlusion_culling = p_data.ignore_occlusion_culling;
	slot->ignore_all_culling = p_data.ignore_all_culling;
	if (slot->skeleton != p_data.skeleton) {
		slot->skeleton = p_data.skeleton;
		if (slot->skeleton.is_valid()) {
			RSG::mesh_storage->skeleton_update_dependency(slot->skeleton, &slot->dependency_tracker);
		}
		if (slot->base_type == RSE::INSTANCE_MESH) {
			_instance_update_mesh_instance(slot);
		}
	}
	if (slot->visible != p_data.visible) {
		_render_slot_change_visibility(p_data.handle, p_data.visible);
	}
	if ((slot->scenario ? slot->scenario->self : RID()) != p_data.scenario) {
		_render_slot_move_scenario(p_data.handle, p_data.scenario);
	}
	if (authored_changed && ((1 << slot->base_type) & RSE::INSTANCE_GEOMETRY_MASK)) {
		static_cast<InstanceGeometryData *>(slot->base_data)->geometry_instance->scene_data_changed();
	}
	_refresh_render_slot_flags(slot);
	if (authored_changed || pose_changed) {
		_instance_queue_update(slot, true, authored_changed);
	}
	_instance_update_scene_membership(slot);
	Vector<Ref<Resource>> assets = p_data.assets;
	if (p_data.base_asset.is_valid()) {
		assets.push_back(p_data.base_asset);
	}
	if (p_data.material_asset.is_valid()) {
		assets.push_back(p_data.material_asset);
	}
	if (slot->tool_assets != assets) {
		_retire_entity_assets(slot->tool_assets);
		slot->tool_assets = assets;
	}
}

void RendererSceneCull::_instance_update_mesh_instance(Instance *p_instance) const {
	bool needs_instance = RSG::mesh_storage->mesh_needs_instance(p_instance->base, p_instance->skeleton.is_valid());
	if (needs_instance != p_instance->mesh_instance.is_valid()) {
		if (needs_instance) {
			p_instance->mesh_instance = RSG::mesh_storage->mesh_instance_create(p_instance->base);

		} else {
			RSG::mesh_storage->mesh_instance_free(p_instance->mesh_instance);
			p_instance->mesh_instance = RID();
		}

		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
		geom->geometry_instance->scene_data_changed();

		if (p_instance->scenario && p_instance->array_index >= 0) {
			InstanceData &idata = p_instance->scenario->instance_data[p_instance->array_index];
			if (p_instance->mesh_instance.is_valid()) {
				idata.flags |= InstanceData::FLAG_USES_MESH_INSTANCE;
			} else {
				idata.flags &= ~InstanceData::FLAG_USES_MESH_INSTANCE;
			}
		}
	}

	if (p_instance->mesh_instance.is_valid()) {
		RSG::mesh_storage->mesh_instance_set_skeleton(p_instance->mesh_instance, p_instance->skeleton);
	}
}

void RendererSceneCull::_retire_entity_assets(Vector<Ref<Resource>> p_assets, Vector<RID> p_bases) {
	if (p_assets.is_empty() && p_bases.is_empty()) {
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device) {
		retired_entity_assets.push_back({ device->get_pending_submission_serial(), p_assets, p_bases });
	} else {
		for (RID base : p_bases) {
			RSG::utilities->free(base);
		}
	}
}

void RendererSceneCull::_collect_retired_entity_assets() {
	RenderingDevice *device = RenderingDevice::get_singleton();
	const uint64_t completed = device ? device->get_completed_submission_serial() : UINT64_MAX;
	for (uint32_t i = 0; i < retired_entity_assets.size();) {
		if (retired_entity_assets[i].submission <= completed) {
			for (RID base : retired_entity_assets[i].bases) {
				RSG::utilities->free(base);
			}
			retired_entity_assets.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

void RendererSceneCull::finalize_entities() {
	update_dirty_instances();
	releasing_entity_batch = true;
	for (RID rid : scenario_owner.get_owned_list()) {
		Scenario *scenario = scenario_owner.get_or_null(rid);
		for (KeyValue<EntityId, NativeEntity> &entry : scenario->native_entities) {
			_release_native_entity(entry.value);
		}
		scenario->native_entities.clear();
		scenario->native_cameras.clear();
		scenario->native_environments.clear();
		scenario->native_released = true;
	}
	releasing_entity_batch = false;
	update_dirty_instances();
	if (RenderingDevice *device = RenderingDevice::get_singleton()) {
		device->flush_and_stall();
	}
	for (RetiredEntityAssets &retired : retired_entity_assets) {
		for (RID base : retired.bases) {
			RSG::utilities->free(base);
		}
	}
	retired_entity_assets.clear();
	if (procedural_geometry_base.is_valid()) {
		RSG::mesh_storage->mesh_free(procedural_geometry_base);
		procedural_geometry_base = RID();
	}
}

void RendererSceneCull::_release_native_entity(NativeEntity &r_entity) {
	for (RID &slot : r_entity.slots) {
		if (slot.is_valid()) {
			free(slot);
			slot = RID();
		}
	}
	Vector<RID> bases;
	for (RID &base : r_entity.owned_bases) {
		if (base.is_valid()) {
			bases.push_back(base);
			base = RID();
		}
	}
	if (r_entity.skeleton.is_valid()) {
		bases.push_back(r_entity.skeleton);
		r_entity.skeleton = RID();
	}
	if (r_entity.camera.is_valid()) {
		free(r_entity.camera);
		r_entity.camera = RID();
	}
	_retire_entity_assets(Vector<Ref<Resource>>(), bases);
	for (Vector<Ref<Resource>> &assets : r_entity.assets) {
		_retire_entity_assets(assets);
		assets.clear();
	}
}

void RendererSceneCull::_remove_entity_references(Instance *p_instance) {
	if (!p_instance->scenario) {
		return;
	}
	for (EntityId target : { p_instance->visibility_target, p_instance->skeleton_target, p_instance->lightmap_target, p_instance->subemitter_target }) {
		HashSet<Instance *> *dependents = p_instance->scenario->entity_dependents.getptr(target);
		if (dependents) {
			dependents->erase(p_instance);
			if (dependents->is_empty()) {
				p_instance->scenario->entity_dependents.erase(target);
			}
		}
	}
}

void RendererSceneCull::_refresh_entity_references(Instance *p_instance) {
	if (!p_instance->scenario) {
		return;
	}
	Scenario *scenario = p_instance->scenario;
	if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
		NativeEntity *skeleton_entity = scenario->native_entities.getptr(p_instance->skeleton_target);
		RID skeleton = skeleton_entity ? skeleton_entity->skeleton : RID();
		if (p_instance->skeleton != skeleton) {
			p_instance->skeleton = skeleton;
			if (skeleton.is_valid()) {
				RSG::mesh_storage->skeleton_update_dependency(skeleton, &p_instance->dependency_tracker);
			}
			if (p_instance->base_type == RSE::INSTANCE_MESH) {
				_instance_update_mesh_instance(p_instance);
			}
			static_cast<InstanceGeometryData *>(p_instance->base_data)->geometry_instance->scene_data_changed();
			_instance_queue_update(p_instance, true, true);
		}
		NativeEntity *lightmap = scenario->native_entities.getptr(p_instance->lightmap_target);
		_render_slot_link_lightmap(p_instance->self, lightmap ? lightmap->slots[11] : RID(), p_instance->lightmap_uv_scale, p_instance->lightmap_slice_index);
		NativeEntity *parent = scenario->native_entities.getptr(p_instance->visibility_target);
		RID parent_slot;
		if (parent) {
			for (int component : { 0, 5, 9 }) {
				if (parent->slots[component].is_valid()) {
					parent_slot = parent->slots[component];
					break;
				}
			}
		}
		_render_slot_link_visibility(p_instance->self, parent_slot);
	}
	if (p_instance->base_type == RSE::INSTANCE_PARTICLES) {
		NativeEntity *emitter = scenario->native_entities.getptr(p_instance->subemitter_target);
		RID target = emitter ? emitter->owned_bases[9] : RID();
		RSG::particles_storage->particles_set_subemitter(p_instance->base, target == p_instance->base ? RID() : target);
	}
}

void RendererSceneCull::_apply_entity_geometry(Instance *p_instance, const EntityRenderUpdate &p_update) {
	const EntityGeometry &geometry = p_update.geometry;
	if (p_instance->dynamic_gi != geometry.use_dynamic_gi && p_instance->indexer_id.is_valid()) {
		_unpair_instance(p_instance);
	}
	p_instance->baked_light = geometry.use_baked_light;
	p_instance->dynamic_gi = geometry.use_dynamic_gi;
	p_instance->cast_shadows = RSE::ShadowCastingSetting(geometry.cast_shadows);
	p_instance->material_overlay = geometry.material_overlay.is_valid() ? geometry.material_overlay->get_rid() : RID();
	p_instance->transparency = geometry.transparency;
	p_instance->sorting_offset = geometry.sorting_offset;
	p_instance->use_aabb_center = geometry.use_aabb_center;
	p_instance->lod_bias = geometry.lod_bias;
	p_instance->extra_margin = geometry.extra_cull_margin;
	p_instance->ignore_occlusion_culling = geometry.ignore_occlusion_culling;
	p_instance->ignore_all_culling = geometry.ignore_all_culling;
	const AABB custom_aabb = geometry.custom_aabb != AABB() ? geometry.custom_aabb : geometry.rt_procedural ? geometry.procedural_aabb : AABB();
	if (custom_aabb != AABB()) {
		if (!p_instance->custom_aabb) {
			p_instance->custom_aabb = memnew(AABB);
		}
		*p_instance->custom_aabb = custom_aabb;
	} else if (p_instance->custom_aabb) {
		memdelete(p_instance->custom_aabb);
		p_instance->custom_aabb = nullptr;
	}
	p_instance->visibility_range_begin = geometry.visibility_range_begin;
	p_instance->visibility_range_end = geometry.visibility_range_end;
	p_instance->visibility_range_begin_margin = geometry.visibility_range_begin_margin;
	p_instance->visibility_range_end_margin = geometry.visibility_range_end_margin;
	p_instance->visibility_range_fade_mode = RSE::VisibilityRangeFadeMode(geometry.visibility_range_fade_mode);
	_update_instance_visibility_dependencies(p_instance);
	if (p_instance->scenario && p_instance->visibility_index >= 0) {
		InstanceVisibilityData &visibility = p_instance->scenario->instance_visibility[p_instance->visibility_index];
		visibility.range_begin = p_instance->visibility_range_begin;
		visibility.range_end = p_instance->visibility_range_end;
		visibility.range_begin_margin = p_instance->visibility_range_begin_margin;
		visibility.range_end_margin = p_instance->visibility_range_end_margin;
		visibility.fade_mode = p_instance->visibility_range_fade_mode;
	}
	HashSet<StringName> uniforms;
	for (const EntityShaderUniform &uniform : geometry.shader_uniforms) {
		StringName name(uniform.name);
		uniforms.insert(name);
		p_instance->instance_uniforms.set(p_instance->self, name, uniform.value);
	}
	for (const StringName &name : p_instance->published_uniforms) {
		if (!uniforms.has(name)) {
			p_instance->instance_uniforms.set(p_instance->self, name, Variant());
		}
	}
	p_instance->published_uniforms = uniforms;
	if (p_instance->mesh_instance.is_valid()) {
		int count = RSG::mesh_storage->mesh_get_blend_shape_count(p_instance->base);
		for (int i = 0; i < count; i++) {
			RSG::mesh_storage->mesh_instance_set_blend_shape_weight(p_instance->mesh_instance, i, i < geometry.blend_shape_weights.size() ? geometry.blend_shape_weights[i] : 0.0);
		}
	}
	InstanceGeometryData *data = static_cast<InstanceGeometryData *>(p_instance->base_data);
	data->geometry_instance->set_rt_procedural(geometry.rt_procedural, geometry.procedural_aabb);
	if (geometry.rt_procedural) {
		Vector<float> bounds;
		bounds.resize(geometry.procedural_bounds.size());
		for (int i = 0; i < bounds.size(); i++) {
			bounds.write[i] = geometry.procedural_bounds[i];
		}
		data->geometry_instance->set_rt_procedural_bounds(bounds, geometry.expose_procedural_bounds);
	}
	data->geometry_instance->scene_data_changed();
	_refresh_render_slot_flags(p_instance);
	_instance_update_cull_domain(p_instance);
}

void RendererSceneCull::_refresh_render_slot_flags(Instance *p_instance) {
	if (p_instance->scenario && p_instance->array_index >= 0) {
		InstanceData &cached = p_instance->scenario->instance_data[p_instance->array_index];
		cached.layer_mask = p_instance->layer_mask;
		cached.flags &= ~(InstanceData::FLAG_USES_BAKED_LIGHT | InstanceData::FLAG_CAST_SHADOWS | InstanceData::FLAG_CAST_SHADOWS_ONLY | InstanceData::FLAG_IGNORE_OCCLUSION_CULLING);
		cached.flags |= p_instance->baked_light ? InstanceData::FLAG_USES_BAKED_LIGHT : 0;
		cached.flags |= p_instance->cast_shadows != RSE::SHADOW_CASTING_SETTING_OFF ? InstanceData::FLAG_CAST_SHADOWS : 0;
		cached.flags |= p_instance->cast_shadows == RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY ? InstanceData::FLAG_CAST_SHADOWS_ONLY : 0;
		cached.flags |= p_instance->ignore_occlusion_culling ? InstanceData::FLAG_IGNORE_OCCLUSION_CULLING : 0;
	}
}

void RendererSceneCull::_apply_entity_pose(NativeEntity &r_entity, const EntityRenderPoseUpdate &p_update) {
	const EntityPose &pose = p_update.pose;
	Transform3D transform(pose.basis, Vector3(pose.translation.x, pose.translation.y, pose.translation.z));
	for (RID handle : r_entity.slots) {
		Instance *slot = instance_owner.get_or_null(handle);
		if (!slot) {
			continue;
		}
		const bool changed = slot->transform != transform || slot->origin[0] != pose.translation.x || slot->origin[1] != pose.translation.y || slot->origin[2] != pose.translation.z || slot->reset_revision != p_update.reset_revision;
		if (changed) {
			slot->transform = transform;
			slot->origin[0] = pose.translation.x;
			slot->origin[1] = pose.translation.y;
			slot->origin[2] = pose.translation.z;
			slot->teleported |= slot->reset_revision != p_update.reset_revision;
			slot->reset_revision = p_update.reset_revision;
			_instance_queue_update(slot, true, false);
		}
		const bool visible = p_update.visible && slot->component_visible;
		if (slot->visible != visible) {
			_render_slot_change_visibility(handle, visible);
		}
	}
	if (Camera *camera = camera_owner.get_or_null(r_entity.camera)) {
		camera->transform = transform.orthonormalized();
		const Vector3 offset = camera->transform.basis.get_column(0) * camera->position_offset.x + camera->transform.basis.get_column(1) * camera->position_offset.y;
		camera->origin[0] = pose.translation.x + double(offset.x);
		camera->origin[1] = pose.translation.y + double(offset.y);
		camera->origin[2] = pose.translation.z + double(offset.z);
		camera->transform.origin = Vector3(camera->origin[0], camera->origin[1], camera->origin[2]);
	}
}

void RendererSceneCull::scene_publish_entities(const EntityRenderPacket &p_packet) {
	const uint64_t publish_begin = OS::get_singleton()->get_ticks_usec();
	_collect_retired_entity_assets();
	Scenario *scenario = scenario_owner.get_or_null(p_packet.scenario);
	if (!scenario || scenario->native_released || (scenario->world_generation && scenario->world_generation != p_packet.world_generation) || p_packet.sequence <= scenario->publication_sequence) {
		return;
	}
	scenario->world_generation = p_packet.world_generation;
	scenario->publication_sequence = p_packet.sequence;
	update_dirty_instances();
	const uint64_t prepare_end = OS::get_singleton()->get_ticks_usec();
	releasing_entity_batch = true;
	if (p_packet.release) {
		for (KeyValue<EntityId, NativeEntity> &entry : scenario->native_entities) {
			_release_native_entity(entry.value);
		}
		scenario->native_entities.clear();
		scenario->native_cameras.clear();
		scenario->native_environments.clear();
		scenario->native_released = true;
		scenario->environment = RID();
		scenario->camera_attributes = RID();
		scenario->compositor = RID();
		releasing_entity_batch = false;
		update_dirty_instances();
		return;
	}
	for (const EntityRenderUpdate &update : p_packet.updates) {
		NativeEntity *existing = scenario->native_entities.getptr(update.id);
		const bool renderable = (update.components & ~EntityRenderUpdate::GEOMETRY) != 0 || update.procedural;
		if (existing && (!update.handle.is_valid() || !(existing->handle == update.handle) || !renderable)) {
			_release_native_entity(*existing);
			scenario->native_entities.erase(update.id);
			scenario->native_cameras.erase(update.id);
			scenario->native_environments.erase(update.id);
		}
		if (!renderable || !update.handle.is_valid() || update.handle.world_generation != p_packet.world_generation) {
			continue;
		}
		NativeEntity &entity = scenario->native_entities[update.id];
		entity.handle = update.handle;
		Vector<Ref<Resource>> assets[EntityRenderUpdate::COMPONENT_COUNT];
		int asset_component = 0;
		auto asset_rid = [&](const auto &p_asset) -> RID {
			if (p_asset.is_null()) {
				return RID();
			}
			assets[asset_component].push_back(p_asset);
			return p_asset->get_rid();
		};
		const EntityPose &pose = update.pose;
		Transform3D transform(pose.basis, Vector3(pose.translation.x, pose.translation.y, pose.translation.z));
		RID bases[EntityRenderUpdate::COMPONENT_COUNT];
		for (int component = 0; component < EntityRenderUpdate::COMPONENT_COUNT; component++) {
			if (Instance *slot = instance_owner.get_or_null(entity.slots[component])) {
				bases[component] = slot->base;
			}
		}
		if (update.changed_components & EntityRenderUpdate::MESH) {
			bases[0] = update.components & EntityRenderUpdate::MESH ? asset_rid(update.mesh.mesh) : RID();
			if (bases[0].is_null() && update.procedural) {
				if (procedural_geometry_base.is_null()) {
					procedural_geometry_base = RSG::mesh_storage->mesh_allocate();
					RSG::mesh_storage->mesh_initialize(procedural_geometry_base);
				}
				bases[0] = procedural_geometry_base;
			}
		}
		if (update.changed_components & EntityRenderUpdate::MULTIMESH) {
			asset_component = 5;
			bases[5] = update.components & EntityRenderUpdate::MULTIMESH ? asset_rid(update.multimesh.multimesh) : RID();
		}
		if (update.changed_components & EntityRenderUpdate::VOXEL_GI) {
			asset_component = 10;
			bases[10] = update.components & EntityRenderUpdate::VOXEL_GI ? asset_rid(update.voxel_gi.data) : RID();
		}
		if (update.changed_components & EntityRenderUpdate::LIGHTMAP) {
			asset_component = 11;
			bases[11] = update.components & EntityRenderUpdate::LIGHTMAP ? asset_rid(update.lightmap.data) : RID();
		}
		if (update.changed_components & EntityRenderUpdate::SKINNING_POSE) {
			if (update.components & EntityRenderUpdate::SKINNING_POSE) {
				if (entity.skeleton.is_null()) {
					entity.skeleton = RSG::mesh_storage->skeleton_allocate();
					RSG::mesh_storage->skeleton_initialize(entity.skeleton);
				}
				RSG::mesh_storage->skeleton_allocate_data(entity.skeleton, update.skinning_pose.bones.size(), false);
				for (int i = 0; i < update.skinning_pose.bones.size(); i++) {
					const EntityPose &bone = update.skinning_pose.bones[i];
					RSG::mesh_storage->skeleton_bone_set_transform(entity.skeleton, i, Transform3D(bone.basis, Vector3(bone.translation.x, bone.translation.y, bone.translation.z)));
				}
			} else if (entity.skeleton.is_valid()) {
				Vector<RID> retired;
				retired.push_back(entity.skeleton);
				_retire_entity_assets(Vector<Ref<Resource>>(), retired);
				entity.skeleton = RID();
			}
		}
		for (int component : { 3, 6, 7, 8, 9, 13 }) {
			if (!(update.changed_components & (1 << component))) {
				continue;
			}
			RID &base = entity.owned_bases[component];
			bool replace_light = component == 3 && base.is_valid() && RSG::light_storage->light_get_type(base) != RSE::LightType(update.light.type);
			if (!(update.components & (1 << component)) || replace_light) {
				if (entity.slots[component].is_valid()) {
					free(entity.slots[component]);
					entity.slots[component] = RID();
				}
				if (base.is_valid()) {
					Vector<RID> retired;
					retired.push_back(base);
					_retire_entity_assets(Vector<Ref<Resource>>(), retired);
					base = RID();
					bases[component] = RID();
				}
			}
			if (!(update.components & (1 << component))) {
				if (component == 9) {
					entity.emitter_initialized = false;
				}
				continue;
			}
			if (base.is_null()) {
				switch (component) {
					case 3:
						switch (RSE::LightType(update.light.type)) {
							case RSE::LIGHT_DIRECTIONAL: base = RSG::light_storage->directional_light_allocate(); RSG::light_storage->directional_light_initialize(base); break;
							case RSE::LIGHT_OMNI: base = RSG::light_storage->omni_light_allocate(); RSG::light_storage->omni_light_initialize(base); break;
							case RSE::LIGHT_SPOT: base = RSG::light_storage->spot_light_allocate(); RSG::light_storage->spot_light_initialize(base); break;
							case RSE::LIGHT_AREA: base = RSG::light_storage->area_light_allocate(); RSG::light_storage->area_light_initialize(base); break;
							default: break;
						}
						break;
					case 6: base = RSG::texture_storage->decal_allocate(); RSG::texture_storage->decal_initialize(base); break;
					case 7: base = RSG::fog->fog_volume_allocate(); RSG::fog->fog_volume_initialize(base); break;
					case 8: base = RSG::light_storage->reflection_probe_allocate(); RSG::light_storage->reflection_probe_initialize(base); break;
					case 9: base = RSG::particles_storage->particles_allocate(); RSG::particles_storage->particles_initialize(base); break;
					case 13: base = RSG::particles_storage->particles_collision_allocate(); RSG::particles_storage->particles_collision_initialize(base); break;
				}
			}
			bases[component] = base;
		}
		if ((update.changed_components & (1 << 3)) && bases[3].is_valid()) {
			asset_component = 3;
			RID base = bases[3];
			const EntityLight &light = update.light;
			const float parameters[RSE::LIGHT_PARAM_MAX] = {
				float(light.energy), float(light.indirect_energy), float(light.volumetric_fog_energy), float(light.specular), float(light.range), float(light.size), float(light.attenuation), float(light.spot_angle), float(light.spot_attenuation),
				float(light.shadow_max_distance), float(light.shadow_split_offsets.x), float(light.shadow_split_offsets.y), float(light.shadow_split_offsets.z), float(light.shadow_fade_start), float(light.shadow_normal_bias), float(light.shadow_bias), float(light.shadow_pancake_size), float(light.shadow_opacity), float(light.shadow_blur), float(light.transmittance_bias), float(light.intensity)
			};
			for (int i = 0; i < RSE::LIGHT_PARAM_MAX; i++) {
				RSG::light_storage->light_set_param(base, RSE::LightParam(i), parameters[i]);
			}
			Color color = light.color;
			if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
				color = (color.srgb_to_linear() * render_light_color_from_temperature(light.temperature).srgb_to_linear()).linear_to_srgb();
			}
			RSG::light_storage->light_set_color(base, color);
			RSG::light_storage->light_set_shadow(base, light.shadow);
			RSG::light_storage->light_set_negative(base, light.negative);
			RSG::light_storage->light_set_reverse_cull_face_mode(base, light.shadow_reverse_cull);
			RSG::light_storage->light_set_cull_mask(base, light.cull_mask);
			RSG::light_storage->light_set_shadow_caster_mask(base, light.shadow_caster_mask);
			RSG::light_storage->light_set_bake_mode(base, RSE::LightBakeMode(light.bake_mode));
			RSG::light_storage->light_set_projector(base, asset_rid(light.projector));
			RSG::light_storage->light_set_distance_fade(base, light.distance_fade_enabled, light.distance_fade_begin, light.distance_fade_shadow, light.distance_fade_length);
			if (light.type == RSE::LIGHT_DIRECTIONAL) {
				RSG::light_storage->light_directional_set_shadow_mode(base, RSE::LightDirectionalShadowMode(light.directional_shadow_mode));
				RSG::light_storage->light_directional_set_blend_splits(base, light.directional_blend_splits);
				RSG::light_storage->light_directional_set_sky_mode(base, RSE::LightDirectionalSkyMode(light.directional_sky_mode));
			} else if (light.type == RSE::LIGHT_OMNI) {
				RSG::light_storage->light_omni_set_shadow_mode(base, RSE::LightOmniShadowMode(light.omni_shadow_mode));
			} else if (light.type == RSE::LIGHT_AREA) {
				RSG::light_storage->light_area_set_size(base, light.area_size);
				RSG::light_storage->light_area_set_normalize_energy(base, light.area_normalize_energy);
				RSG::light_storage->light_area_set_texture(base, asset_rid(light.area_texture));
			}
		}
		if ((update.changed_components & (1 << 6)) && bases[6].is_valid()) {
			asset_component = 6;
			RID base = bases[6];
			const EntityDecal &decal = update.decal;
			RSG::texture_storage->decal_set_size(base, decal.size);
			RSG::texture_storage->decal_set_texture(base, RSE::DECAL_TEXTURE_ALBEDO, asset_rid(decal.albedo));
			RSG::texture_storage->decal_set_texture(base, RSE::DECAL_TEXTURE_NORMAL, asset_rid(decal.normal));
			RSG::texture_storage->decal_set_texture(base, RSE::DECAL_TEXTURE_ORM, asset_rid(decal.orm));
			RSG::texture_storage->decal_set_texture(base, RSE::DECAL_TEXTURE_EMISSION, asset_rid(decal.emission));
			RSG::texture_storage->decal_set_emission_energy(base, decal.emission_energy);
			RSG::texture_storage->decal_set_albedo_mix(base, decal.albedo_mix);
			RSG::texture_storage->decal_set_modulate(base, decal.modulate);
			RSG::texture_storage->decal_set_cull_mask(base, decal.cull_mask);
			RSG::texture_storage->decal_set_normal_fade(base, decal.normal_fade);
			RSG::texture_storage->decal_set_fade(base, decal.upper_fade, decal.lower_fade);
			RSG::texture_storage->decal_set_distance_fade(base, decal.distance_fade_enabled, decal.distance_fade_begin, decal.distance_fade_length);
		}
		if ((update.changed_components & (1 << 7)) && bases[7].is_valid()) {
			asset_component = 7;
			RSG::fog->fog_volume_set_size(bases[7], update.fog_volume.size);
			RSG::fog->fog_volume_set_shape(bases[7], RSE::FogVolumeShape(update.fog_volume.shape));
			RSG::fog->fog_volume_set_material(bases[7], asset_rid(update.fog_volume.material));
		}
		if ((update.changed_components & (1 << 8)) && bases[8].is_valid()) {
			asset_component = 8;
			RID base = bases[8];
			const EntityReflectionProbe &probe = update.reflection_probe;
			RSG::light_storage->reflection_probe_set_size(base, probe.size);
			RSG::light_storage->reflection_probe_set_origin_offset(base, probe.origin_offset);
			RSG::light_storage->reflection_probe_set_intensity(base, probe.intensity);
			RSG::light_storage->reflection_probe_set_blend_distance(base, probe.blend_distance);
			RSG::light_storage->reflection_probe_set_max_distance(base, probe.max_distance);
			RSG::light_storage->reflection_probe_set_enable_box_projection(base, probe.box_projection);
			RSG::light_storage->reflection_probe_set_enable_shadows(base, probe.enable_shadows);
			RSG::light_storage->reflection_probe_set_as_interior(base, probe.interior);
			RSG::light_storage->reflection_probe_set_ambient_mode(base, RSE::ReflectionProbeAmbientMode(probe.ambient_mode));
			RSG::light_storage->reflection_probe_set_ambient_color(base, probe.ambient_color);
			RSG::light_storage->reflection_probe_set_ambient_energy(base, probe.ambient_energy);
			RSG::light_storage->reflection_probe_set_mesh_lod_threshold(base, probe.mesh_lod_threshold);
			RSG::light_storage->reflection_probe_set_cull_mask(base, probe.cull_mask);
			RSG::light_storage->reflection_probe_set_reflection_mask(base, probe.reflection_mask);
			RSG::light_storage->reflection_probe_set_update_mode(base, RSE::ReflectionProbeUpdateMode(probe.update_mode));
		}
		if ((update.changed_components & (1 << 9)) && bases[9].is_valid()) {
			asset_component = 9;
			RID base = bases[9];
			const EntityParticles &particles = update.particles;
			RSG::particles_storage->particles_set_mode(base, RSE::PARTICLES_MODE_3D);
			RSG::particles_storage->particles_set_amount(base, particles.amount);
			RSG::particles_storage->particles_set_amount_ratio(base, particles.amount_ratio);
			RSG::particles_storage->particles_set_lifetime(base, particles.lifetime);
			RSG::particles_storage->particles_set_one_shot(base, particles.one_shot);
			RSG::particles_storage->particles_set_pre_process_time(base, particles.preprocess);
			RSG::particles_storage->particles_set_explosiveness_ratio(base, particles.explosiveness);
			RSG::particles_storage->particles_set_randomness_ratio(base, particles.randomness);
			RSG::particles_storage->particles_set_speed_scale(base, particles.speed_scale);
			RSG::particles_storage->particles_set_custom_aabb(base, particles.visibility_aabb);
			RSG::particles_storage->particles_set_use_local_coordinates(base, particles.local_coords);
			RSG::particles_storage->particles_set_fixed_fps(base, particles.fixed_fps);
			RSG::particles_storage->particles_set_fractional_delta(base, particles.fractional_delta);
			RSG::particles_storage->particles_set_interpolate(base, particles.interpolate);
			RSG::particles_storage->particles_set_collision_base_size(base, particles.collision_base_size);
			const bool restart = !entity.emitter_initialized || entity.emitter_restart_revision != particles.restart_revision;
			const bool start = particles.emitting && (restart || !entity.emitter_requested);
			if (particles.use_fixed_seed || start) {
				RSG::particles_storage->particles_set_seed(base, particles.use_fixed_seed ? particles.seed : Math::rand());
			}
			RSG::particles_storage->particles_set_trails(base, particles.trail_enabled, particles.trail_lifetime);
			RSG::particles_storage->particles_set_transform_align(base, RSE::ParticlesTransformAlign(particles.transform_align));
			RSG::particles_storage->particles_set_transform_align_channel_filter(base, RSE::ParticlesTransformAlignCustomSrc(particles.transform_align_channel_filter));
			RSG::particles_storage->particles_set_transform_align_axis(base, RSE::ParticlesTransformAlignAxis(particles.transform_align_axis));
			RSG::particles_storage->particles_set_process_material(base, asset_rid(particles.process_material));
			RSG::particles_storage->particles_set_draw_order(base, RSE::ParticlesDrawOrder(particles.draw_order));
			RSG::particles_storage->particles_set_draw_passes(base, particles.draw_passes.size());
			for (int i = 0; i < particles.draw_passes.size(); i++) {
				RSG::particles_storage->particles_set_draw_pass_mesh(base, i, asset_rid(particles.draw_passes[i]));
			}
			Vector<Transform3D> bind_poses;
			if (particles.skin.is_valid()) {
				assets[9].push_back(particles.skin);
				bind_poses.resize(particles.skin->get_bind_count());
				for (int i = 0; i < bind_poses.size(); i++) {
					bind_poses.write[i] = particles.skin->get_bind_pose(i);
				}
			}
			RSG::particles_storage->particles_set_trail_bind_poses(base, bind_poses);
			RSG::particles_storage->particles_set_interp_to_end(base, particles.interp_to_end);
			RSG::particles_storage->particles_set_emitter_velocity(base, particles.emitter_velocity);
			if (restart) {
				RSG::particles_storage->particles_restart(base);
			}
			if (restart || entity.emitter_requested != particles.emitting) {
				RSG::particles_storage->particles_set_emitting(base, particles.emitting);
			}
			entity.emitter_initialized = true;
			entity.emitter_requested = particles.emitting;
			entity.emitter_restart_revision = particles.restart_revision;
		}
		if ((update.changed_components & EntityRenderUpdate::PARTICLES_COLLISION) && bases[13].is_valid()) {
			asset_component = 13;
			RID base = bases[13];
			const EntityParticlesCollision &collision = update.particles_collision;
			RSG::particles_storage->particles_collision_set_collision_type(base, RSE::ParticlesCollisionType(collision.type));
			RSG::particles_storage->particles_collision_set_cull_mask(base, collision.cull_mask);
			RSG::particles_storage->particles_collision_set_sphere_radius(base, collision.radius);
			RSG::particles_storage->particles_collision_set_box_extents(base, collision.size * 0.5);
			RSG::particles_storage->particles_collision_set_attractor_strength(base, collision.strength);
			RSG::particles_storage->particles_collision_set_attractor_directionality(base, collision.directionality);
			RSG::particles_storage->particles_collision_set_attractor_attenuation(base, collision.attenuation);
			RSG::particles_storage->particles_collision_set_field_texture(base, asset_rid(collision.field));
			RSG::particles_storage->particles_collision_set_height_field_resolution(base, RSE::ParticlesCollisionHeightfieldResolution(collision.heightfield_resolution));
			RSG::particles_storage->particles_collision_set_height_field_mask(base, collision.heightfield_mask);
		}
		if (update.changed_components & EntityRenderUpdate::GEOMETRY) {
			asset_component = 1;
			asset_rid(update.geometry.material_overlay);
		}
		for (int component = 0; component < EntityRenderUpdate::COMPONENT_COUNT; component++) {
			const bool component_changed = update.changed_components & (1 << component);
			if (!component_changed && !((update.changed_components & EntityRenderUpdate::GEOMETRY) && (component == 0 || component == 5 || component == 9))) {
				continue;
			}
			asset_component = component;
			RID &handle = entity.slots[component];
			if (bases[component].is_null()) {
				if (handle.is_valid()) {
					free(handle);
					handle = RID();
				}
				continue;
			}
			if (handle.is_null()) {
				handle = _render_slot_allocate();
				_render_slot_initialize(handle);
			}
			Instance *slot = instance_owner.get_or_null(handle);
			while (slot->pairs.first()) {
				InstancePair *pair = slot->pairs.first()->self();
				_instance_unpair(slot, slot == pair->a ? pair->b : pair->a);
				pair_allocator.free(pair);
			}
			if (slot->base != bases[component]) {
				_render_slot_replace_base(handle, bases[component]);
			}
			slot->entity_handle = update.handle;
			slot->entity_id = update.id;
			slot->render_handle = handle;
			if (component_changed && component == 13) {
				InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(slot->base_data);
				collision->heightfield_follow_camera = update.particles_collision.heightfield_follow_camera;
				collision->heightfield_update_always = update.particles_collision.heightfield_update_mode == 1;
			}
			slot->transform = transform;
			slot->origin[0] = pose.translation.x;
			slot->origin[1] = pose.translation.y;
			slot->origin[2] = pose.translation.z;
			slot->teleported |= slot->reset_revision != update.reset_revision;
			slot->reset_revision = update.reset_revision;
			bool visible = update.visible && slot->component_visible;
			const Vector<Ref<Material>> *materials = nullptr;
			if (component_changed && component == 0) {
				slot->component_visible = update.mesh.visible;
				visible = update.visible && slot->component_visible;
				slot->layer_mask = update.mesh.layers;
				slot->material_override = asset_rid(update.mesh.material_override);
				materials = &update.mesh.surface_materials;
			} else if (component_changed && component == 5) {
				slot->component_visible = update.multimesh.visible;
				visible = update.visible && slot->component_visible;
				slot->layer_mask = update.multimesh.layers;
				slot->material_override = asset_rid(update.multimesh.material_override);
				materials = &update.multimesh.surface_materials;
			} else if (component_changed && component == 9) {
				slot->layer_mask = update.particles.layers;
				slot->material_override = asset_rid(update.particles.material_override);
			}
			if (materials) {
				slot->materials.resize(materials->size());
				for (int i = 0; i < materials->size(); i++) {
					slot->materials.write[i] = asset_rid((*materials)[i]);
				}
			}
			if (slot->visible != visible) {
				_render_slot_change_visibility(handle, visible);
			}
			if (slot->scenario != scenario) {
				_render_slot_move_scenario(handle, p_packet.scenario);
			}
			_remove_entity_references(slot);
			slot->visibility_target = update.geometry.visibility_parent.id;
			slot->skeleton_target = update.geometry.skeleton.id;
			slot->lightmap_target = update.geometry.lightmap.id;
			slot->lightmap_uv_scale = update.geometry.lightmap_uv_scale;
			slot->lightmap_slice_index = update.geometry.lightmap_slice;
			if (component_changed && component == 9) {
				slot->subemitter_target = update.particles.sub_emitter.id;
			}
			for (EntityId target : { slot->visibility_target, slot->skeleton_target, slot->lightmap_target, slot->subemitter_target }) {
				if (target.is_valid()) {
					scenario->entity_dependents[target].insert(slot);
				}
			}
			if ((1 << slot->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
				_apply_entity_geometry(slot, update);
			}
			_instance_queue_update(slot, true, true);
			_instance_update_scene_membership(slot);
		}
		if (update.changed_components & EntityRenderUpdate::CAMERA) {
			asset_component = 2;
			if (update.components & EntityRenderUpdate::CAMERA) {
				if (entity.camera.is_null()) {
					entity.camera = camera_allocate();
					camera_initialize(entity.camera);
				}
				Camera *camera = camera_owner.get_or_null(entity.camera);
				camera->type = Camera::Type(update.camera.projection);
				camera->fov = update.camera.fov;
				camera->size = update.camera.size;
				camera->znear = update.camera.near_distance;
				camera->zfar = update.camera.far_distance;
				camera->offset = update.camera.frustum_offset;
				camera->vaspect = update.camera.keep_width;
				camera->visible_layers = update.camera.layers;
				camera->position_offset = update.camera.offset;
				camera->transform = transform.orthonormalized();
				camera->transform.origin += camera->transform.basis.get_column(0) * update.camera.offset.x + camera->transform.basis.get_column(1) * update.camera.offset.y;
				camera->env = asset_rid(update.camera.environment);
				camera->attributes = asset_rid(update.camera.attributes);
				camera->compositor = asset_rid(update.camera.compositor);
				entity.current_camera = update.camera.current;
				if (entity.current_camera) {
					scenario->native_cameras.insert(update.id, entity.camera);
				} else {
					scenario->native_cameras.erase(update.id);
				}
			} else if (entity.camera.is_valid()) {
				free(entity.camera);
				entity.camera = RID();
				scenario->native_cameras.erase(update.id);
			}
		}
		if (update.changed_components & EntityRenderUpdate::ENVIRONMENT) {
			asset_component = 4;
			if (update.components & EntityRenderUpdate::ENVIRONMENT) {
				Vector<RID> environment;
				environment.push_back(asset_rid(update.environment.environment));
				environment.push_back(asset_rid(update.environment.attributes));
				environment.push_back(asset_rid(update.environment.compositor));
				scenario->native_environments.insert(update.id, environment);
			} else {
				scenario->native_environments.erase(update.id);
			}
		}
		for (int component = 0; component < EntityRenderUpdate::COMPONENT_COUNT; component++) {
			if ((update.changed_components & (1 << component)) && entity.assets[component] != assets[component]) {
				_retire_entity_assets(entity.assets[component]);
				entity.assets[component] = assets[component];
			}
		}
		_apply_entity_pose(entity, update);
	}
	const uint64_t updates_end = OS::get_singleton()->get_ticks_usec();
	for (const EntityRenderPoseUpdate &update : p_packet.poses) {
		NativeEntity *entity = scenario->native_entities.getptr(update.id);
		if (entity && entity->handle == update.handle) {
			_apply_entity_pose(*entity, update);
		}
	}
	const uint64_t poses_end = OS::get_singleton()->get_ticks_usec();
	HashSet<Instance *> references_to_refresh;
	for (const EntityRenderUpdate &update : p_packet.updates) {
		if (NativeEntity *entity = scenario->native_entities.getptr(update.id)) {
			for (RID handle : entity->slots) {
				if (Instance *slot = instance_owner.get_or_null(handle)) {
					references_to_refresh.insert(slot);
				}
			}
		}
		if (const HashSet<Instance *> *dependents = scenario->entity_dependents.getptr(update.id)) {
			for (Instance *slot : *dependents) {
				references_to_refresh.insert(slot);
			}
		}
	}
	for (Instance *slot : references_to_refresh) {
		_refresh_entity_references(slot);
	}
	EntityId selected;
	for (const KeyValue<EntityId, RID> &entry : scenario->native_cameras) {
		if (!selected.is_valid() || entry.key.high < selected.high || (entry.key.high == selected.high && entry.key.low < selected.low)) {
			selected = entry.key;
		}
	}
	if (scenario->sampling_camera.is_null()) {
		scenario->sampling_camera = p_packet.camera;
	}
	if (Camera *camera = camera_owner.get_or_null(p_packet.camera)) {
		*camera = selected.is_valid() ? *camera_owner.get_or_null(scenario->native_cameras[selected]) : Camera();
	}
	selected = EntityId();
	for (const KeyValue<EntityId, Vector<RID>> &entry : scenario->native_environments) {
		if (!selected.is_valid() || entry.key.high < selected.high || (entry.key.high == selected.high && entry.key.low < selected.low)) {
			selected = entry.key;
		}
	}
	scenario->environment = selected.is_valid() ? scenario->native_environments[selected][0] : RID();
	scenario->camera_attributes = selected.is_valid() ? scenario->native_environments[selected][1] : RID();
	scenario->compositor = selected.is_valid() ? scenario->native_environments[selected][2] : RID();
	releasing_entity_batch = false;
	const uint64_t references_end = OS::get_singleton()->get_ticks_usec();
	update_dirty_instances();
	const uint64_t dirty_end = OS::get_singleton()->get_ticks_usec();
	if (OS::get_singleton()->is_use_benchmark_set() && p_packet.updates.size() > 0) {
		const double to_ms = 1.0 / 1000.0;
		print_line(vformat("RendererSceneCull scene_publish_entities: updates=%d poses=%d prepare=%.2fms apply=%.2fms pose_apply=%.2fms references=%.2fms dirty=%.2fms",
				p_packet.updates.size(),
				p_packet.poses.size(),
				double(prepare_end - publish_begin) * to_ms,
				double(updates_end - prepare_end) * to_ms,
				double(poses_end - updates_end) * to_ms,
				double(references_end - poses_end) * to_ms,
				double(dirty_end - references_end) * to_ms));
	}
}

void RendererSceneCull::_render_slot_replace_base(RID p_instance, RID p_base) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_NULL(instance);

	Scenario *scenario = instance->scenario;

	if (instance->base_type != RSE::INSTANCE_NONE) {
		//free anything related to that base

		if (scenario && instance->indexer_id.is_valid()) {
			_unpair_instance(instance);
		}

		if (instance->mesh_instance.is_valid()) {
			RSG::mesh_storage->mesh_instance_free(instance->mesh_instance);
			instance->mesh_instance = RID();
			// no need to set instance data flag here, as it was freed above
		}

		switch (instance->base_type) {
			case RSE::INSTANCE_MESH:
			case RSE::INSTANCE_MULTIMESH:
			case RSE::INSTANCE_PARTICLES: {
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
				scene_render->geometry_instance_free(geom->geometry_instance);
			} break;
			case RSE::INSTANCE_LIGHT: {
				InstanceLightData *light = static_cast<InstanceLightData *>(instance->base_data);

				if (scenario && instance->visible && RSG::light_storage->light_get_type(instance->base) != RSE::LIGHT_DIRECTIONAL && light->bake_mode == RSE::LIGHT_BAKE_DYNAMIC) {
					scenario->dynamic_lights.erase(light->instance);
				}

#ifdef DEBUG_ENABLED
				if (light->geometries.size()) {
					ERR_PRINT("BUG, indexing did not unpair geometries from light.");
				}
#endif
				if (scenario && light->D) {
					scenario->directional_lights.erase(light->D);
					light->D = nullptr;
				}
				RSG::light_storage->light_instance_free(light->instance);
			} break;
			case RSE::INSTANCE_PARTICLES_COLLISION: {
				heightfield_particle_colliders_update_list.erase(instance);
				continuous_heightfield_particle_colliders.erase(instance);
				InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(instance->base_data);
				RSG::utilities->free(collision->instance);
			} break;
			case RSE::INSTANCE_FOG_VOLUME: {
				InstanceFogVolumeData *volume = static_cast<InstanceFogVolumeData *>(instance->base_data);
				scene_render->free(volume->instance);
			} break;
			case RSE::INSTANCE_VISIBLITY_NOTIFIER: {
				//none
			} break;
			case RSE::INSTANCE_REFLECTION_PROBE: {
				InstanceReflectionProbeData *reflection_probe = static_cast<InstanceReflectionProbeData *>(instance->base_data);
				RSG::light_storage->reflection_probe_instance_free(reflection_probe->instance);
			} break;
			case RSE::INSTANCE_DECAL: {
				InstanceDecalData *decal = static_cast<InstanceDecalData *>(instance->base_data);
				RSG::texture_storage->decal_instance_free(decal->instance);

			} break;
			case RSE::INSTANCE_LIGHTMAP: {
				InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(instance->base_data);
				//erase dependencies, since no longer a lightmap
				while (lightmap_data->users.begin()) {
					_render_slot_link_lightmap((*lightmap_data->users.begin())->self, RID(), Rect2(), 0);
				}
				RSG::light_storage->lightmap_instance_free(lightmap_data->instance);
			} break;
			case RSE::INSTANCE_VOXEL_GI: {
				InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(instance->base_data);
#ifdef DEBUG_ENABLED
				if (voxel_gi->geometries.size()) {
					ERR_PRINT("BUG, indexing did not unpair geometries from VoxelGI.");
				}
#endif
#ifdef DEBUG_ENABLED
				if (voxel_gi->lights.size()) {
					ERR_PRINT("BUG, indexing did not unpair lights from VoxelGI.");
				}
#endif

				scene_render->free(voxel_gi->probe_instance);

			} break;
			case RSE::INSTANCE_OCCLUDER: {
				if (scenario && instance->visible) {
					RendererSceneOcclusionCull::get_singleton()->scenario_remove_instance(instance->scenario->self, p_instance);
				}
			} break;
			default: {
			}
		}

		if (instance->base_data) {
			memdelete(instance->base_data);
			instance->base_data = nullptr;
		}

		instance->materials.clear();
	}

	instance->base_type = RSE::INSTANCE_NONE;
	instance->base = RID();

	if (p_base.is_valid()) {
		instance->base_type = RSG::utilities->get_base_type(p_base);

		// fix up a specific malfunctioning case before the switch, so it can be handled
		if (instance->base_type == RSE::INSTANCE_NONE && RendererSceneOcclusionCull::get_singleton()->is_occluder(p_base)) {
			instance->base_type = RSE::INSTANCE_OCCLUDER;
		}

		switch (instance->base_type) {
			case RSE::INSTANCE_NONE: {
				ERR_PRINT_ONCE("unimplemented base type encountered in renderer scene cull");
				return;
			}
			case RSE::INSTANCE_LIGHT: {
				InstanceLightData *light = memnew(InstanceLightData);

				if (scenario && RSG::light_storage->light_get_type(p_base) == RSE::LIGHT_DIRECTIONAL) {
					light->D = scenario->directional_lights.push_back(instance);
				}

				light->instance = RSG::light_storage->light_instance_create(p_base);

				instance->base_data = light;
			} break;
			case RSE::INSTANCE_MESH:
			case RSE::INSTANCE_MULTIMESH:
			case RSE::INSTANCE_PARTICLES: {
				InstanceGeometryData *geom = memnew(InstanceGeometryData);
				instance->base_data = geom;
				geom->geometry_instance = scene_render->geometry_instance_create(p_base, instance);

				ERR_FAIL_NULL(geom->geometry_instance);
				geom->geometry_instance->scene_data_changed();
				geom->geometry_instance->micro_geometry_routing_data = instance;
				geom->geometry_instance->micro_geometry_routing_changed = _instance_micro_geometry_routing_changed;
				geom->geometry_instance->set_transform(instance->transform, instance->aabb, instance->transformed_aabb);
				geom->geometry_instance->set_use_lightmap(RID(), instance->lightmap_uv_scale, instance->lightmap_slice_index);
				geom->geometry_instance->set_instance_shader_uniforms_offset(instance->instance_uniforms.location());
				if (instance->lightmap_sh.size() == 9) {
					geom->geometry_instance->set_lightmap_capture(instance->lightmap_sh.ptr());
				}

				for (Instance *E : instance->visibility_dependencies) {
					Instance *dep_instance = E;
					ERR_CONTINUE(dep_instance->array_index == -1);
					ERR_CONTINUE(dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index != -1);
					dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = instance->array_index;
				}
			} break;
			case RSE::INSTANCE_PARTICLES_COLLISION: {
				InstanceParticlesCollisionData *collision = memnew(InstanceParticlesCollisionData);
				collision->instance = RSG::particles_storage->particles_collision_instance_create(p_base);
				RSG::particles_storage->particles_collision_instance_set_active(collision->instance, instance->visible);
				instance->base_data = collision;
			} break;
			case RSE::INSTANCE_FOG_VOLUME: {
				InstanceFogVolumeData *volume = memnew(InstanceFogVolumeData);
				volume->instance = scene_render->fog_volume_instance_create(p_base);
				scene_render->fog_volume_instance_set_active(volume->instance, instance->visible);
				instance->base_data = volume;
			} break;
			case RSE::INSTANCE_VISIBLITY_NOTIFIER: {
				InstanceVisibilityNotifierData *vnd = memnew(InstanceVisibilityNotifierData);
				vnd->base = p_base;
				instance->base_data = vnd;
			} break;
			case RSE::INSTANCE_REFLECTION_PROBE: {
				InstanceReflectionProbeData *reflection_probe = memnew(InstanceReflectionProbeData);
				reflection_probe->owner = instance;
				instance->base_data = reflection_probe;

				reflection_probe->instance = RSG::light_storage->reflection_probe_instance_create(p_base);
			} break;
			case RSE::INSTANCE_DECAL: {
				InstanceDecalData *decal = memnew(InstanceDecalData);
				decal->owner = instance;
				instance->base_data = decal;

				decal->instance = RSG::texture_storage->decal_instance_create(p_base);
				RSG::texture_storage->decal_instance_set_sorting_offset(decal->instance, instance->sorting_offset);
			} break;
			case RSE::INSTANCE_LIGHTMAP: {
				InstanceLightmapData *lightmap_data = memnew(InstanceLightmapData);
				instance->base_data = lightmap_data;
				lightmap_data->instance = RSG::light_storage->lightmap_instance_create(p_base);
			} break;
			case RSE::INSTANCE_VOXEL_GI: {
				InstanceVoxelGIData *voxel_gi = memnew(InstanceVoxelGIData);
				instance->base_data = voxel_gi;
				voxel_gi->owner = instance;

				voxel_gi->probe_instance = scene_render->voxel_gi_instance_create(p_base);

			} break;
			case RSE::INSTANCE_OCCLUDER: {
				if (scenario) {
					RendererSceneOcclusionCull::get_singleton()->scenario_set_instance(scenario->self, p_instance, p_base, instance->transform, instance->visible);
				}
			} break;
			default: {
			}
		}

		instance->base = p_base;

		if (instance->base_type == RSE::INSTANCE_MESH) {
			_instance_update_mesh_instance(instance);
		}

		//forcefully update the dependency now, so if for some reason it gets removed, we can immediately clear it
		RSG::utilities->base_update_dependency(p_base, &instance->dependency_tracker);
	}

	_instance_queue_update(instance, true, true);
	_instance_update_scene_membership(instance);
}

void RendererSceneCull::_render_slot_move_scenario(RID p_instance, RID p_scenario) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_NULL(instance);

	if (instance->scenario) {
		instance->scenario->instances.remove(&instance->scenario_item);

		if (instance->indexer_id.is_valid()) {
			_unpair_instance(instance);
		}

		switch (instance->base_type) {
			case RSE::INSTANCE_LIGHT: {
				InstanceLightData *light = static_cast<InstanceLightData *>(instance->base_data);
				if (instance->visible && RSG::light_storage->light_get_type(instance->base) != RSE::LIGHT_DIRECTIONAL && light->bake_mode == RSE::LIGHT_BAKE_DYNAMIC) {
					instance->scenario->dynamic_lights.erase(light->instance);
				}

#ifdef DEBUG_ENABLED
				if (light->geometries.size()) {
					ERR_PRINT("BUG, indexing did not unpair geometries from light.");
				}
#endif
				if (light->D) {
					instance->scenario->directional_lights.erase(light->D);
					light->D = nullptr;
				}
			} break;
			case RSE::INSTANCE_REFLECTION_PROBE: {
				InstanceReflectionProbeData *reflection_probe = static_cast<InstanceReflectionProbeData *>(instance->base_data);
				RSG::light_storage->reflection_probe_release_atlas_index(reflection_probe->instance);

			} break;
			case RSE::INSTANCE_PARTICLES_COLLISION: {
				heightfield_particle_colliders_update_list.erase(instance);
				continuous_heightfield_particle_colliders.erase(instance);
			} break;
			case RSE::INSTANCE_VOXEL_GI: {
				InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(instance->base_data);

#ifdef DEBUG_ENABLED
				if (voxel_gi->geometries.size()) {
					ERR_PRINT("BUG, indexing did not unpair geometries from VoxelGI.");
				}
#endif
#ifdef DEBUG_ENABLED
				if (voxel_gi->lights.size()) {
					ERR_PRINT("BUG, indexing did not unpair lights from VoxelGI.");
				}
#endif
			} break;
			case RSE::INSTANCE_OCCLUDER: {
				if (instance->visible) {
					RendererSceneOcclusionCull::get_singleton()->scenario_remove_instance(instance->scenario->self, p_instance);
				}
			} break;
			default: {
			}
		}

		instance->scenario = nullptr;
		_instance_update_scene_membership(instance);
	}

	if (p_scenario.is_valid()) {
		Scenario *scenario = scenario_owner.get_or_null(p_scenario);
		ERR_FAIL_NULL(scenario);

		instance->scenario = scenario;

		scenario->instances.add(&instance->scenario_item);

		switch (instance->base_type) {
			case RSE::INSTANCE_LIGHT: {
				InstanceLightData *light = static_cast<InstanceLightData *>(instance->base_data);

				if (RSG::light_storage->light_get_type(instance->base) == RSE::LIGHT_DIRECTIONAL) {
					light->D = scenario->directional_lights.push_back(instance);
				}
			} break;
			case RSE::INSTANCE_OCCLUDER: {
				RendererSceneOcclusionCull::get_singleton()->scenario_set_instance(scenario->self, p_instance, instance->base, instance->transform, instance->visible);
			} break;
			default: {
			}
		}

		_instance_queue_update(instance, true, true);
	}
	_instance_update_scene_membership(instance);
}

void RendererSceneCull::_render_slot_change_visibility(RID p_instance, bool p_visible) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_NULL(instance);

	if (instance->visible == p_visible) {
		return;
	}

	instance->visible = p_visible;

	if (p_visible) {
		if (instance->scenario != nullptr) {
			_instance_queue_update(instance, true, false);
		}
	} else if (instance->indexer_id.is_valid()) {
		_unpair_instance(instance);
	}

	if (instance->base_type == RSE::INSTANCE_LIGHT) {
		InstanceLightData *light = static_cast<InstanceLightData *>(instance->base_data);
		if (instance->scenario && RSG::light_storage->light_get_type(instance->base) != RSE::LIGHT_DIRECTIONAL && light->bake_mode == RSE::LIGHT_BAKE_DYNAMIC) {
			if (p_visible) {
				instance->scenario->dynamic_lights.push_back(light->instance);
			} else {
				instance->scenario->dynamic_lights.erase(light->instance);
			}
		}
	}

	if (instance->base_type == RSE::INSTANCE_PARTICLES_COLLISION) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(instance->base_data);
		RSG::particles_storage->particles_collision_instance_set_active(collision->instance, p_visible);
	}

	if (instance->base_type == RSE::INSTANCE_FOG_VOLUME) {
		InstanceFogVolumeData *volume = static_cast<InstanceFogVolumeData *>(instance->base_data);
		scene_render->fog_volume_instance_set_active(volume->instance, p_visible);
	}

	if (instance->base_type == RSE::INSTANCE_OCCLUDER) {
		if (instance->scenario) {
			RendererSceneOcclusionCull::get_singleton()->scenario_set_instance(instance->scenario->self, p_instance, instance->base, instance->transform, p_visible);
		}
	}
	_instance_update_scene_membership(instance);
}

Vector<EntityHandle> RendererSceneCull::scene_entities_cull_aabb(const AABB &p_aabb, RID p_scenario) const {
	Vector<EntityHandle> instances;
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL_V(scenario, instances);

	update_dirty_instances(); // check dirty instances before culling

	struct CullAABB {
		Vector<EntityHandle> instances;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			Instance *p_instance = (Instance *)p_data;
			if (p_instance->entity_handle.is_valid()) {
				instances.push_back(p_instance->entity_handle);
			}
			return false;
		}
	};

	CullAABB cull_aabb;
	scenario->indexers[Scenario::INDEXER_GEOMETRY].aabb_query(p_aabb, cull_aabb);
	scenario->indexers[Scenario::INDEXER_VOLUMES].aabb_query(p_aabb, cull_aabb);
	return cull_aabb.instances;
}

Vector<EntityHandle> RendererSceneCull::scene_entities_cull_ray(const Vector3 &p_from, const Vector3 &p_to, RID p_scenario) const {
	Vector<EntityHandle> instances;
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL_V(scenario, instances);
	update_dirty_instances(); // check dirty instances before culling

	struct CullRay {
		Vector<EntityHandle> instances;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			Instance *p_instance = (Instance *)p_data;
			if (p_instance->entity_handle.is_valid()) {
				instances.push_back(p_instance->entity_handle);
			}
			return false;
		}
	};

	CullRay cull_ray;
	scenario->indexers[Scenario::INDEXER_GEOMETRY].ray_query(p_from, p_to, cull_ray);
	scenario->indexers[Scenario::INDEXER_VOLUMES].ray_query(p_from, p_to, cull_ray);
	return cull_ray.instances;
}

Vector<EntityHandle> RendererSceneCull::scene_entities_cull_convex(const Vector<Plane> &p_convex, RID p_scenario) const {
	Vector<EntityHandle> instances;
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL_V(scenario, instances);
	update_dirty_instances(); // check dirty instances before culling

	Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(&p_convex[0], p_convex.size());

	struct CullConvex {
		Vector<EntityHandle> instances;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			Instance *p_instance = (Instance *)p_data;
			if (p_instance->entity_handle.is_valid()) {
				instances.push_back(p_instance->entity_handle);
			}
			return false;
		}
	};

	CullConvex cull_convex;
	scenario->indexers[Scenario::INDEXER_GEOMETRY].convex_query(p_convex.ptr(), p_convex.size(), points.ptr(), points.size(), cull_convex);
	scenario->indexers[Scenario::INDEXER_VOLUMES].convex_query(p_convex.ptr(), p_convex.size(), points.ptr(), points.size(), cull_convex);
	return cull_convex.instances;
}

void RendererSceneCull::_render_slot_link_visibility(RID p_instance, RID p_parent_instance) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_NULL(instance);

	Instance *old_parent = instance->visibility_parent;
	if (old_parent) {
		old_parent->visibility_dependencies.erase(instance);
		instance->visibility_parent = nullptr;
		_update_instance_visibility_depth(old_parent);
	}

	Instance *parent = instance_owner.get_or_null(p_parent_instance);
	ERR_FAIL_COND(p_parent_instance.is_valid() && !parent);

	if (parent) {
		parent->visibility_dependencies.insert(instance);
		instance->visibility_parent = parent;

		bool cycle_detected = _update_instance_visibility_depth(parent);
		if (cycle_detected) {
			ERR_PRINT("Cycle detected in the visibility dependencies tree. The latest change to visibility_parent will have no effect.");
			parent->visibility_dependencies.erase(instance);
			instance->visibility_parent = nullptr;
		}
	}

	_update_instance_visibility_dependencies(instance);
}

bool RendererSceneCull::_update_instance_visibility_depth(Instance *p_instance) {
	bool cycle_detected = false;
	HashSet<Instance *> traversed_nodes;

	{
		Instance *instance = p_instance;
		while (instance) {
			if (!instance->visibility_dependencies.is_empty()) {
				uint32_t depth = 0;
				for (const Instance *E : instance->visibility_dependencies) {
					depth = MAX(depth, E->visibility_dependencies_depth);
				}
				instance->visibility_dependencies_depth = depth + 1;
			} else {
				instance->visibility_dependencies_depth = 0;
			}

			if (instance->scenario && instance->visibility_index != -1) {
				instance->scenario->instance_visibility.move(instance->visibility_index, instance->visibility_dependencies_depth);
			}

			traversed_nodes.insert(instance);

			instance = instance->visibility_parent;
			if (traversed_nodes.has(instance)) {
				cycle_detected = true;
				break;
			}
		}
	}

	return cycle_detected;
}

void RendererSceneCull::_update_instance_visibility_dependencies(Instance *p_instance) const {
	_instance_update_cull_domain(p_instance);
	bool is_geometry_instance = ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) && p_instance->base_data;
	bool has_visibility_range = p_instance->visibility_range_begin > 0.0 || p_instance->visibility_range_end > 0.0;
	bool needs_visibility_cull = has_visibility_range && is_geometry_instance && p_instance->array_index != -1;

	if (!needs_visibility_cull && p_instance->visibility_index != -1) {
		p_instance->scenario->instance_visibility.remove_at(p_instance->visibility_index);
		p_instance->visibility_index = -1;
	} else if (needs_visibility_cull && p_instance->visibility_index == -1) {
		InstanceVisibilityData vd;
		vd.instance = p_instance;
		vd.range_begin = p_instance->visibility_range_begin;
		vd.range_end = p_instance->visibility_range_end;
		vd.range_begin_margin = p_instance->visibility_range_begin_margin;
		vd.range_end_margin = p_instance->visibility_range_end_margin;
		const AABB local = Transform3D(p_instance->transform.basis, Vector3()).xform(p_instance->aabb);
		for (int axis = 0; axis < 3; axis++) {
			vd.position[axis] = p_instance->origin[axis] + double(local.position[axis]) + double(local.size[axis]) * 0.5;
		}
		vd.array_index = p_instance->array_index;
		vd.fade_mode = p_instance->visibility_range_fade_mode;

		p_instance->scenario->instance_visibility.insert(vd, p_instance->visibility_dependencies_depth);
	}

	if (p_instance->scenario && p_instance->array_index != -1) {
		InstanceData &idata = p_instance->scenario->instance_data[p_instance->array_index];
		idata.visibility_index = p_instance->visibility_index;

		if (is_geometry_instance) {
			if (has_visibility_range && p_instance->visibility_range_fade_mode == RSE::VISIBILITY_RANGE_FADE_SELF) {
				bool begin_enabled = p_instance->visibility_range_begin > 0.0f;
				float begin_min = p_instance->visibility_range_begin - p_instance->visibility_range_begin_margin;
				float begin_max = p_instance->visibility_range_begin + p_instance->visibility_range_begin_margin;
				bool end_enabled = p_instance->visibility_range_end > 0.0f;
				float end_min = p_instance->visibility_range_end - p_instance->visibility_range_end_margin;
				float end_max = p_instance->visibility_range_end + p_instance->visibility_range_end_margin;
				idata.instance_geometry->set_fade_range(begin_enabled, begin_min, begin_max, end_enabled, end_min, end_max);
			} else {
				idata.instance_geometry->set_fade_range(false, 0.0f, 0.0f, false, 0.0f, 0.0f);
			}
		}

		if ((has_visibility_range || p_instance->visibility_parent) && (p_instance->visibility_index == -1 || p_instance->visibility_dependencies_depth == 0)) {
			idata.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK;
		} else {
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK;
		}

		if (p_instance->visibility_parent) {
			idata.parent_array_index = p_instance->visibility_parent->array_index;
		} else {
			idata.parent_array_index = -1;
			if (is_geometry_instance) {
				idata.instance_geometry->set_parent_fade_alpha(1.0f);
			}
		}
	}
}

void RendererSceneCull::_render_slot_link_lightmap(RID p_instance, RID p_lightmap, const Rect2 &p_lightmap_uv_scale, int p_slice_index) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_NULL(instance);

	if (instance->lightmap) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(((Instance *)instance->lightmap)->base_data);
		lightmap_data->users.erase(instance);
		instance->lightmap = nullptr;
	}

	Instance *lightmap_instance = instance_owner.get_or_null(p_lightmap);

	instance->lightmap = lightmap_instance;
	instance->lightmap_uv_scale = p_lightmap_uv_scale;
	instance->lightmap_slice_index = p_slice_index;

	RID lightmap_instance_rid;

	if (lightmap_instance) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(lightmap_instance->base_data);
		lightmap_data->users.insert(instance);
		lightmap_instance_rid = lightmap_data->instance;
	}

	if ((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_use_lightmap(lightmap_instance_rid, p_lightmap_uv_scale, p_slice_index);
	}
}

void RendererSceneCull::mesh_generate_pipelines(RID p_mesh, bool p_background_compilation) {
	scene_render->mesh_generate_pipelines(p_mesh, p_background_compilation);
}

uint32_t RendererSceneCull::get_pipeline_compilations(RSE::PipelineSource p_source) {
	return scene_render->get_pipeline_compilations(p_source);
}

void RendererSceneCull::_update_instance(Instance *p_instance) const {
	p_instance->version++;

	// When not using interpolation the transform is used straight.
	const Transform3D *instance_xform = &p_instance->transform;
	const double *instance_origin = p_instance->origin;
	if (p_instance->base_type == RSE::INSTANCE_PARTICLES_COLLISION) {
		_update_particle_collider_sampling(p_instance);
		instance_origin = static_cast<InstanceParticlesCollisionData *>(p_instance->base_data)->sampling_origin;
	}

	// Can possibly use the most up to date current transform here when using physics interpolation ...
	// uncomment the next line for this..
	//if (_interpolation_data.interpolation_enabled && p_instance->interpolated) {
	//    instance_xform = &p_instance->transform_curr;
	//}
	// However it does seem that using the interpolated transform (transform) works for keeping AABBs
	// up to date to avoid culling errors.

	if (p_instance->base_type == RSE::INSTANCE_LIGHT) {
		InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);

		RSG::light_storage->light_instance_set_transform(light->instance, *instance_xform, p_instance->origin);
		RSG::light_storage->light_instance_set_aabb(light->instance, instance_xform->xform(p_instance->aabb));
		light->make_shadow_dirty();

		RSE::LightBakeMode bake_mode = RSG::light_storage->light_get_bake_mode(p_instance->base);
		if (RSG::light_storage->light_get_type(p_instance->base) != RSE::LIGHT_DIRECTIONAL && bake_mode != light->bake_mode) {
			if (p_instance->visible && p_instance->scenario && light->bake_mode == RSE::LIGHT_BAKE_DYNAMIC) {
				p_instance->scenario->dynamic_lights.erase(light->instance);
			}

			light->bake_mode = bake_mode;

			if (p_instance->visible && p_instance->scenario && light->bake_mode == RSE::LIGHT_BAKE_DYNAMIC) {
				p_instance->scenario->dynamic_lights.push_back(light->instance);
			}
		}

		uint32_t max_sdfgi_cascade = RSG::light_storage->light_get_max_sdfgi_cascade(p_instance->base);
		if (light->max_sdfgi_cascade != max_sdfgi_cascade) {
			light->max_sdfgi_cascade = max_sdfgi_cascade; //should most likely make sdfgi dirty in scenario
		}
		light->cull_mask = RSG::light_storage->light_get_cull_mask(p_instance->base);
	} else if (p_instance->base_type == RSE::INSTANCE_REFLECTION_PROBE) {
		InstanceReflectionProbeData *reflection_probe = static_cast<InstanceReflectionProbeData *>(p_instance->base_data);

		RSG::light_storage->reflection_probe_instance_set_transform(reflection_probe->instance, *instance_xform, p_instance->origin);

		if (p_instance->scenario && p_instance->array_index >= 0) {
			InstanceData &idata = p_instance->scenario->instance_data[p_instance->array_index];
			idata.flags |= InstanceData::FLAG_REFLECTION_PROBE_DIRTY;
		}
	} else if (p_instance->base_type == RSE::INSTANCE_DECAL) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(p_instance->base_data);

		RSG::texture_storage->decal_instance_set_transform(decal->instance, *instance_xform, p_instance->origin);
		decal->cull_mask = RSG::texture_storage->decal_get_cull_mask(p_instance->base);
	} else if (p_instance->base_type == RSE::INSTANCE_LIGHTMAP) {
		InstanceLightmapData *lightmap = static_cast<InstanceLightmapData *>(p_instance->base_data);

		RSG::light_storage->lightmap_instance_set_transform(lightmap->instance, *instance_xform, p_instance->origin);
	} else if (p_instance->base_type == RSE::INSTANCE_VOXEL_GI) {
		InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(p_instance->base_data);

		scene_render->voxel_gi_instance_set_transform_to_data(voxel_gi->probe_instance, *instance_xform, p_instance->origin);
	} else if (p_instance->base_type == RSE::INSTANCE_PARTICLES) {
		RSG::particles_storage->particles_set_emission_transform(p_instance->base, *instance_xform, instance_origin);
	} else if (p_instance->base_type == RSE::INSTANCE_PARTICLES_COLLISION) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(p_instance->base_data);

		//remove materials no longer used and un-own them
		if (RSG::particles_storage->particles_collision_is_heightfield(p_instance->base)) {
			heightfield_particle_colliders_update_list.insert(p_instance);
		}
		RSG::particles_storage->particles_collision_instance_set_transform(collision->instance, *instance_xform, instance_origin);
		collision->cull_mask = RSG::particles_storage->particles_collision_get_cull_mask(p_instance->base);
	} else if (p_instance->base_type == RSE::INSTANCE_FOG_VOLUME) {
		InstanceFogVolumeData *volume = static_cast<InstanceFogVolumeData *>(p_instance->base_data);
		scene_render->fog_volume_instance_set_transform(volume->instance, *instance_xform, p_instance->origin);
	} else if (p_instance->base_type == RSE::INSTANCE_OCCLUDER) {
		if (p_instance->scenario) {
			RendererSceneOcclusionCull::get_singleton()->scenario_set_instance(p_instance->scenario->self, p_instance->self, p_instance->base, *instance_xform, p_instance->visible);
		}
	} else if (p_instance->base_type == RSE::INSTANCE_NONE) {
		return;
	}

	if (!p_instance->aabb.has_surface()) {
		return;
	}

	if (p_instance->base_type == RSE::INSTANCE_LIGHTMAP) {
		//if this moved, update the captured objects
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(p_instance->base_data);
		//erase dependencies, since no longer a lightmap

		for (Instance *E : lightmap_data->geometries) {
			Instance *geom = E;
			_instance_queue_update(geom, true, false);
		}
	}

	const InstanceBounds instance_bounds(p_instance->aabb, instance_xform->basis, instance_origin);
	const AABB new_aabb = instance_bounds.get_aabb();
	p_instance->transformed_aabb = new_aabb;

	if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
		//make sure lights are updated if it casts shadow

		if (geom->can_cast_shadows) {
			for (const Instance *E : geom->lights) {
				InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);
				light->make_shadow_dirty();
			}
			if (p_instance->scenario) {
				const AABB &previous_aabb = p_instance->prev_transformed_aabb;
				p_instance->scenario->shadow_caster_dirty(previous_aabb == AABB() ? new_aabb : new_aabb.merge(previous_aabb));
			}
		}

		if (!p_instance->lightmap && geom->lightmap_captures.size()) {
			//affected by lightmap captures, must update capture info!
			_update_instance_lightmap_captures(p_instance);
		} else {
			if (!p_instance->lightmap_sh.is_empty()) {
				p_instance->lightmap_sh.clear(); //don't need SH
				p_instance->lightmap_target_sh.clear(); //don't need SH
				ERR_FAIL_NULL(geom->geometry_instance);
				geom->geometry_instance->set_lightmap_capture(nullptr);
			}
		}

		ERR_FAIL_NULL(geom->geometry_instance);

		_instance_update_cull_domain(p_instance);
		geom->geometry_instance->set_transform(*instance_xform, p_instance->aabb, p_instance->transformed_aabb);
		if (p_instance->teleported) {
			geom->geometry_instance->reset_motion_vectors();
		}
	}

	// note: we had to remove is equal approx check here, it meant that det == 0.000004 won't work, which is the case for some of our scenes.
	if (p_instance->scenario == nullptr || !p_instance->visible || instance_xform->basis.determinant() == 0) {
		p_instance->prev_transformed_aabb = p_instance->transformed_aabb;
		return;
	}

	//quantize to improve moving object performance
	AABB bvh_aabb = p_instance->transformed_aabb;

	if (p_instance->indexer_id.is_valid() && bvh_aabb != p_instance->prev_transformed_aabb) {
		//assume motion, see if bounds need to be quantized
		AABB motion_aabb = bvh_aabb.merge(p_instance->prev_transformed_aabb);
		float motion_longest_axis = motion_aabb.get_longest_axis_size();
		float longest_axis = p_instance->transformed_aabb.get_longest_axis_size();

		if (motion_longest_axis < longest_axis * 2) {
			//moved but not a lot, use motion aabb quantizing
			float quantize_size = Math::pow(2.0, Math::ceil(Math::log(motion_longest_axis) / Math::log(2.0))) * 0.5; //one fifth
			bvh_aabb.quantize(quantize_size);
		}
	}

	if (!p_instance->indexer_id.is_valid()) {
		if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
			p_instance->indexer_id = p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY].insert(bvh_aabb, p_instance);
		} else {
			p_instance->indexer_id = p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES].insert(bvh_aabb, p_instance);
		}

		p_instance->array_index = p_instance->scenario->instance_data.size();
		InstanceData idata;
		idata.instance = p_instance;
		idata.layer_mask = p_instance->layer_mask;
		idata.flags = p_instance->base_type; //changing it means de-indexing, so this never needs to be changed later
		idata.base_rid = p_instance->base;
		idata.parent_array_index = p_instance->visibility_parent ? p_instance->visibility_parent->array_index : -1;
		idata.visibility_index = p_instance->visibility_index;
		idata.occlusion_timeout = 0;

		for (Instance *E : p_instance->visibility_dependencies) {
			Instance *dep_instance = E;
			if (dep_instance->array_index != -1) {
				dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = p_instance->array_index;
			}
		}

		switch (p_instance->base_type) {
			case RSE::INSTANCE_MESH:
			case RSE::INSTANCE_MULTIMESH:
			case RSE::INSTANCE_PARTICLES: {
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
				idata.instance_geometry = geom->geometry_instance;
			} break;
			case RSE::INSTANCE_LIGHT: {
				InstanceLightData *light_data = static_cast<InstanceLightData *>(p_instance->base_data);
				idata.instance_data_rid = light_data->instance.get_id();
				light_data->uses_projector = RSG::light_storage->light_has_projector(p_instance->base);
				light_data->uses_softshadow = RSG::light_storage->light_get_type(p_instance->base) == RSE::LIGHT_AREA || RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_SIZE) > CMP_EPSILON;
			} break;
			case RSE::INSTANCE_REFLECTION_PROBE: {
				idata.instance_data_rid = static_cast<InstanceReflectionProbeData *>(p_instance->base_data)->instance.get_id();
			} break;
			case RSE::INSTANCE_DECAL: {
				idata.instance_data_rid = static_cast<InstanceDecalData *>(p_instance->base_data)->instance.get_id();
			} break;
			case RSE::INSTANCE_LIGHTMAP: {
				idata.instance_data_rid = static_cast<InstanceLightmapData *>(p_instance->base_data)->instance.get_id();
			} break;
			case RSE::INSTANCE_VOXEL_GI: {
				idata.instance_data_rid = static_cast<InstanceVoxelGIData *>(p_instance->base_data)->probe_instance.get_id();
			} break;
			case RSE::INSTANCE_FOG_VOLUME: {
				idata.instance_data_rid = static_cast<InstanceFogVolumeData *>(p_instance->base_data)->instance.get_id();
			} break;
			case RSE::INSTANCE_VISIBLITY_NOTIFIER: {
				idata.visibility_notifier = static_cast<InstanceVisibilityNotifierData *>(p_instance->base_data);
			} break;
			default: {
			}
		}

		if (p_instance->base_type == RSE::INSTANCE_REFLECTION_PROBE) {
			//always dirty when added
			idata.flags |= InstanceData::FLAG_REFLECTION_PROBE_DIRTY;
		}
		if (p_instance->cast_shadows != RSE::SHADOW_CASTING_SETTING_OFF) {
			idata.flags |= InstanceData::FLAG_CAST_SHADOWS;
		}
		if (p_instance->cast_shadows == RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY) {
			idata.flags |= InstanceData::FLAG_CAST_SHADOWS_ONLY;
		}
		if (p_instance->redraw_if_visible) {
			idata.flags |= InstanceData::FLAG_REDRAW_IF_VISIBLE;
		}
		// dirty flags should not be set here, since no pairing has happened
		if (p_instance->baked_light) {
			idata.flags |= InstanceData::FLAG_USES_BAKED_LIGHT;
		}
		if (p_instance->mesh_instance.is_valid()) {
			idata.flags |= InstanceData::FLAG_USES_MESH_INSTANCE;
		}
		if (p_instance->ignore_occlusion_culling) {
			idata.flags |= InstanceData::FLAG_IGNORE_OCCLUSION_CULLING;
		}
		if (p_instance->ignore_all_culling) {
			idata.flags |= InstanceData::FLAG_IGNORE_ALL_CULLING;
		}

		p_instance->scenario->instance_data.push_back(idata);
		_instance_update_cull_domain(p_instance);
		p_instance->scenario->instance_aabbs.push_back(instance_bounds);
		_update_instance_visibility_dependencies(p_instance);
	} else {
		if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
			p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY].update(p_instance->indexer_id, bvh_aabb);
			if (p_instance->conventional_indexer_id.is_valid()) {
				p_instance->scenario->indexers[Scenario::INDEXER_CONVENTIONAL_GEOMETRY].update(p_instance->conventional_indexer_id, bvh_aabb);
			}
		} else {
			p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES].update(p_instance->indexer_id, bvh_aabb);
		}
		p_instance->scenario->instance_aabbs[p_instance->array_index] = instance_bounds;
	}

	if (p_instance->visibility_index != -1) {
		for (int axis = 0; axis < 3; axis++) {
			p_instance->scenario->instance_visibility[p_instance->visibility_index].position[axis] = (instance_bounds.precise_bounds[axis] + instance_bounds.precise_bounds[axis + 3]) * 0.5;
		}
	}

	//move instance and repair
	pair_pass++;

	PairInstances pair;

	pair.instance = p_instance;
	pair.pair_allocator = &pair_allocator;
	pair.pair_pass = pair_pass;
	pair.pair_mask = 0;

	if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
		pair.pair_mask |= 1 << RSE::INSTANCE_LIGHT;
		pair.pair_mask |= 1 << RSE::INSTANCE_VOXEL_GI;
		pair.pair_mask |= 1 << RSE::INSTANCE_LIGHTMAP;
		if (p_instance->base_type == RSE::INSTANCE_PARTICLES) {
			pair.pair_mask |= 1 << RSE::INSTANCE_PARTICLES_COLLISION;
		}

		pair.pair_mask |= geometry_instance_pair_mask;

		pair.bvh2 = &p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES];
	} else if (p_instance->base_type == RSE::INSTANCE_LIGHT) {
		pair.pair_mask |= RSE::INSTANCE_GEOMETRY_MASK;
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];

		RSE::LightBakeMode bake_mode = RSG::light_storage->light_get_bake_mode(p_instance->base);
		if (bake_mode == RSE::LIGHT_BAKE_STATIC || bake_mode == RSE::LIGHT_BAKE_DYNAMIC) {
			pair.pair_mask |= (1 << RSE::INSTANCE_VOXEL_GI);
			pair.bvh2 = &p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES];
		}
	} else if (p_instance->base_type == RSE::INSTANCE_LIGHTMAP) {
		pair.pair_mask = RSE::INSTANCE_GEOMETRY_MASK;
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
	} else if (geometry_instance_pair_mask & (1 << RSE::INSTANCE_REFLECTION_PROBE) && (p_instance->base_type == RSE::INSTANCE_REFLECTION_PROBE)) {
		pair.pair_mask = RSE::INSTANCE_GEOMETRY_MASK;
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
	} else if (geometry_instance_pair_mask & (1 << RSE::INSTANCE_DECAL) && (p_instance->base_type == RSE::INSTANCE_DECAL)) {
		pair.pair_mask = RSE::INSTANCE_GEOMETRY_MASK;
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
	} else if (p_instance->base_type == RSE::INSTANCE_PARTICLES_COLLISION) {
		pair.pair_mask = (1 << RSE::INSTANCE_PARTICLES);
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
	} else if (p_instance->base_type == RSE::INSTANCE_VOXEL_GI) {
		//lights and geometries
		pair.pair_mask = RSE::INSTANCE_GEOMETRY_MASK | (1 << RSE::INSTANCE_LIGHT);
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
		pair.bvh2 = &p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES];
	}

	pair.pair();

	p_instance->prev_transformed_aabb = p_instance->transformed_aabb;
}

void RendererSceneCull::_unpair_instance(Instance *p_instance) {
	if (!p_instance->indexer_id.is_valid()) {
		return; //nothing to do
	}

	while (p_instance->pairs.first()) {
		InstancePair *pair = p_instance->pairs.first()->self();
		Instance *other_instance = p_instance == pair->a ? pair->b : pair->a;
		_instance_unpair(p_instance, other_instance);
		pair_allocator.free(pair);
	}

	if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
		p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY].remove(p_instance->indexer_id);
		if (static_cast<InstanceGeometryData *>(p_instance->base_data)->can_cast_shadows) {
			p_instance->scenario->shadow_caster_dirty(p_instance->transformed_aabb);
		}
	} else {
		p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES].remove(p_instance->indexer_id);
	}

	p_instance->indexer_id = DynamicBVH::ID();

	_instance_update_cull_domain(p_instance, true);
	//replace this by last
	int32_t swap_with_index = p_instance->scenario->instance_data.size() - 1;
	if (swap_with_index != p_instance->array_index) {
		Instance *swapped_instance = p_instance->scenario->instance_data[swap_with_index].instance;
		swapped_instance->array_index = p_instance->array_index; //swap
		p_instance->scenario->instance_data[p_instance->array_index] = p_instance->scenario->instance_data[swap_with_index];
		p_instance->scenario->instance_aabbs[p_instance->array_index] = p_instance->scenario->instance_aabbs[swap_with_index];

		if (swapped_instance->visibility_index != -1) {
			swapped_instance->scenario->instance_visibility[swapped_instance->visibility_index].array_index = swapped_instance->array_index;
		}

		for (Instance *E : swapped_instance->visibility_dependencies) {
			Instance *dep_instance = E;
			if (dep_instance != p_instance && dep_instance->array_index != -1) {
				dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = swapped_instance->array_index;
			}
		}
	}

	// pop last
	p_instance->scenario->instance_data.pop_back();
	p_instance->scenario->instance_aabbs.pop_back();

	//uninitialize
	p_instance->array_index = -1;
	if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
		// Clear these now because the InstanceData containing the dirty flags is gone
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);

		geom->geometry_instance->clear_light_instances();
		geom->geometry_instance->pair_reflection_probe_instances(nullptr, 0);
		geom->geometry_instance->pair_decal_instances(nullptr, 0);
		geom->geometry_instance->pair_voxel_gi_instances(nullptr, 0);
	}

	for (Instance *E : p_instance->visibility_dependencies) {
		Instance *dep_instance = E;
		if (dep_instance->array_index != -1) {
			dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = -1;
			if ((1 << dep_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
				dep_instance->scenario->instance_data[dep_instance->array_index].instance_geometry->set_parent_fade_alpha(1.0f);
			}
		}
	}

	_update_instance_visibility_dependencies(p_instance);
}

void RendererSceneCull::_update_instance_aabb(Instance *p_instance) const {
	AABB new_aabb;

	ERR_FAIL_COND(p_instance->base_type != RSE::INSTANCE_NONE && !p_instance->base.is_valid());

	switch (p_instance->base_type) {
		case RSE::INSTANCE_NONE: {
			// do nothing
		} break;
		case RSE::INSTANCE_MESH: {
			if (p_instance->custom_aabb) {
				new_aabb = *p_instance->custom_aabb;
			} else {
				new_aabb = RSG::mesh_storage->mesh_get_aabb(p_instance->base, p_instance->skeleton);
			}

		} break;

		case RSE::INSTANCE_MULTIMESH: {
			if (p_instance->custom_aabb) {
				new_aabb = *p_instance->custom_aabb;
			} else {
				new_aabb = RSG::mesh_storage->multimesh_get_aabb(p_instance->base);
			}

		} break;
		case RSE::INSTANCE_PARTICLES: {
			if (p_instance->custom_aabb) {
				new_aabb = *p_instance->custom_aabb;
			} else {
				new_aabb = RSG::particles_storage->particles_get_aabb(p_instance->base);
			}

		} break;
		case RSE::INSTANCE_PARTICLES_COLLISION: {
			new_aabb = RSG::particles_storage->particles_collision_get_aabb(p_instance->base);

		} break;
		case RSE::INSTANCE_FOG_VOLUME: {
			new_aabb = RSG::fog->fog_volume_get_aabb(p_instance->base);
		} break;
		case RSE::INSTANCE_VISIBLITY_NOTIFIER: {
			new_aabb = RSG::utilities->visibility_notifier_get_aabb(p_instance->base);
		} break;
		case RSE::INSTANCE_LIGHT: {
			new_aabb = RSG::light_storage->light_get_aabb(p_instance->base);

		} break;
		case RSE::INSTANCE_REFLECTION_PROBE: {
			new_aabb = RSG::light_storage->reflection_probe_get_aabb(p_instance->base);

		} break;
		case RSE::INSTANCE_DECAL: {
			new_aabb = RSG::texture_storage->decal_get_aabb(p_instance->base);

		} break;
		case RSE::INSTANCE_VOXEL_GI: {
			new_aabb = RSG::gi->voxel_gi_get_bounds(p_instance->base);

		} break;
		case RSE::INSTANCE_LIGHTMAP: {
			new_aabb = RSG::light_storage->lightmap_get_aabb(p_instance->base);

		} break;
		default: {
		}
	}

	if (p_instance->extra_margin) {
		new_aabb.grow_by(p_instance->extra_margin);
	}

	p_instance->aabb = new_aabb;
}

void RendererSceneCull::_update_instance_lightmap_captures(Instance *p_instance) const {
	bool first_set = p_instance->lightmap_sh.is_empty();
	p_instance->lightmap_sh.resize(9); //using SH
	p_instance->lightmap_target_sh.resize(9); //using SH
	Color *instance_sh = p_instance->lightmap_target_sh.ptrw();
	bool inside = false;
	Color accum_sh[9];
	float accum_blend = 0.0;

	InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
	// Don't blend if only one lightmap affects the dynamic object.
	// This prevents ambient light from being pure black until the object has fully entered the LightmapGI's AABB.
	const bool use_blending = geom->lightmap_captures.size() >= 2;

	for (Instance *E : geom->lightmap_captures) {
		Instance *lightmap = E;

		bool interior = RSG::light_storage->lightmap_is_interior(lightmap->base);

		if (inside && !interior) {
			continue; //we are inside, ignore exteriors
		}

		Vector3 center = p_instance->transform.basis.xform(p_instance->aabb.get_center());
		for (int axis = 0; axis < 3; axis++) {
			center[axis] = p_instance->origin[axis] - lightmap->origin[axis] + double(center[axis]);
		}
		Vector3 lm_pos = lightmap->transform.basis.inverse().xform(center);

		AABB bounds = RSG::light_storage->lightmap_get_aabb(lightmap->base);

		Color sh[9];
		RSG::light_storage->lightmap_tap_sh_light(lightmap->base, lm_pos, sh);

		//rotate it
		Basis rot = lightmap->transform.basis.orthonormalized();
		for (int i = 0; i < 3; i++) {
			real_t csh[9];
			for (int j = 0; j < 9; j++) {
				csh[j] = sh[j][i];
			}
			rot.rotate_sh(csh);
			for (int j = 0; j < 9; j++) {
				sh[j][i] = csh[j];
			}
		}

		Vector3 inner_pos = ((lm_pos - bounds.position) / bounds.size) * 2.0 - Vector3(1.0, 1.0, 1.0);

		real_t blend = 1.0;
		if (use_blending) {
			blend = MAX(Math::abs(inner_pos.x), MAX(Math::abs(inner_pos.y), Math::abs(inner_pos.z)));
			// Make blend more rounded.
			blend = Math::lerp(inner_pos.length(), blend, blend);
			blend *= blend;
			blend = MAX(0.0, 1.0 - blend);
		}

		if (interior && !inside) {
			//do not blend, just replace
			for (int j = 0; j < 9; j++) {
				accum_sh[j] = sh[j] * blend;
			}
			accum_blend = blend;
			inside = true;
		} else {
			for (int j = 0; j < 9; j++) {
				accum_sh[j] += sh[j] * blend;
			}
			accum_blend += blend;
		}
	}

	if (accum_blend > 0.0) {
		for (int j = 0; j < 9; j++) {
			instance_sh[j] = accum_sh[j] / accum_blend;
			if (first_set) {
				p_instance->lightmap_sh.write[j] = instance_sh[j];
			}
		}
	}

	ERR_FAIL_NULL(geom->geometry_instance);
	geom->geometry_instance->set_lightmap_capture(p_instance->lightmap_sh.ptr());
}

void RendererSceneCull::_cull_shadow_geometry(Scenario *p_scenario, const Vector<Plane> &p_planes, const double *p_origin, PagedArray<Instance *> &r_instances) {
	const Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(p_planes.ptr(), p_planes.size());
	if (points.is_empty()) {
		return;
	}
	AABB local(points[0], Vector3());
	for (int i = 1; i < points.size(); i++) {
		local.expand_to(points[i]);
	}
	const Frustum frustum(p_planes, p_origin);
	auto gather = [&](void *p_data) {
		Instance *instance = static_cast<Instance *>(p_data);
		if (p_scenario->instance_aabbs[instance->array_index].in_frustum(frustum)) {
			r_instances.push_back(instance);
		}
		return false;
	};
	p_scenario->indexers[Scenario::INDEXER_CONVENTIONAL_GEOMETRY].aabb_query(InstanceBounds(local, Basis(), p_origin).get_aabb(), gather);
}

void RendererSceneCull::_light_instance_setup_directional_shadow(int p_shadow_index, Instance *p_instance, const Transform3D p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, bool p_cam_vaspect, const double *p_cam_origin, bool p_cache_shadows) {
	// For later tight culling, the light culler needs to know the details of the directional light.
	light_culler->prepare_directional_light(p_instance, p_shadow_index);

	InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);

	Transform3D light_transform = p_instance->transform;
	light_transform.orthonormalize(); //scale does not count on lights

	real_t max_distance = p_cam_projection.get_z_far();
	real_t shadow_max = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
	if (shadow_max > 0 && !p_cam_orthogonal) { //its impractical (and leads to unwanted behaviors) to set max distance in orthogonal camera
		max_distance = MIN(shadow_max, max_distance);
	}
	max_distance = MAX(max_distance, p_cam_projection.get_z_near() + 0.001);
	real_t min_distance = MIN(p_cam_projection.get_z_near(), max_distance);

	real_t pancake_size = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE);

	real_t range = max_distance - min_distance;

	int splits = 0;
	switch (RSG::light_storage->light_directional_get_shadow_mode(p_instance->base)) {
		case RSE::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL:
			splits = 1;
			break;
		case RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS:
			splits = 2;
			break;
		case RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS:
			splits = 4;
			break;
	}

	real_t distances[5];

	distances[0] = min_distance;
	for (int i = 0; i < splits; i++) {
		distances[i + 1] = min_distance + RSG::light_storage->light_get_param(p_instance->base, RSE::LightParam(RSE::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET + i)) * range;
	};

	distances[splits] = max_distance;

	real_t texture_size = RSG::light_storage->get_directional_light_shadow_size(light->instance);

	bool overlap = RSG::light_storage->light_directional_get_blend_splits(p_instance->base);

	cull.shadow_count = p_shadow_index + 1;
	cull.shadows[p_shadow_index].cascade_count = splits;
	cull.shadows[p_shadow_index].light_instance = light->instance;
	cull.shadows[p_shadow_index].light_data = p_cache_shadows ? light : nullptr;
	cull.shadows[p_shadow_index].caster_mask = RSG::light_storage->light_get_shadow_caster_mask(p_instance->base);

	const uint64_t caster_generation = p_instance->scenario ? p_instance->scenario->shadow_caster_generation : 0;

	for (int i = 0; i < splits; i++) {
		Cull::Shadow::Cascade &cascade = cull.shadows[p_shadow_index].cascades[i];
		cascade.refresh = false;
		cascade.caster_generation = caster_generation;
		cascade.full_coverage = p_cache_shadows && i > 0;
		RENDER_TIMESTAMP("Cull DirectionalLight3D, Split " + itos(i));

		// setup a camera matrix for that range!
		Projection camera_matrix;

		real_t aspect = p_cam_projection.get_aspect();

		if (p_cam_orthogonal) {
			Vector2 vp_he = p_cam_projection.get_viewport_half_extents();

			camera_matrix.set_orthogonal(vp_he.y * 2.0, aspect, distances[(i == 0 || !overlap) ? i : i - 1], distances[i + 1], false);
		} else {
			real_t fov = p_cam_projection.get_fov(); //this is actually yfov, because set aspect tries to keep it
			camera_matrix.set_perspective(fov, aspect, distances[(i == 0 || !overlap) ? i : i - 1], distances[i + 1], true);
		}

		//obtain the frustum endpoints

		Vector3 endpoints[8]; // frustum plane endpoints
		bool res = camera_matrix.get_endpoints(Transform3D(p_cam_transform.basis, Vector3()), endpoints);
		ERR_CONTINUE(!res);

		if (p_cache_shadows) {
			auto &cached = light->directional_shadow_cache.cascades[i];
			const uint64_t frame = RSG::rasterizer->get_frame_number();
			const uint64_t age = frame - cached.frame;
			const uint32_t period = 1u << i;
			const uint32_t phase = i < 2 ? 0 : (1u << (i - 2)) * 2 - 1;
			bool casters_unchanged = p_instance->scenario != nullptr && cached.caster_generation == caster_generation;
			bool casters_scanned = false;
			if (cached.valid) {
				cached.max_age = MAX(cached.max_age, age);
				const real_t texel = (cached.maximum.x - cached.minimum.x) / MAX(texture_size, real_t(1));
				const real_t margin = MAX(cached.maximum.x - cached.minimum.x, cached.maximum.y - cached.minimum.y) * 2.0 / MAX(texture_size, real_t(1));
				const real_t depth_range = MAX(cached.maximum.z - cached.minimum.z, texel);
				const real_t sun_angle_max = MIN(Math::deg_to_rad(shadow_sun_max_angle_degrees), shadow_sun_texel_error * texel / depth_range);
				if (cached.basis.get_column(2).dot(light_transform.basis.get_column(2)) < Math::cos(sun_angle_max)) {
					cached.force |= 1 << InstanceLightData::DirectionalShadowCache::SUN;
				}
				if (!casters_unchanged && p_instance->scenario != nullptr && !p_instance->scenario->shadow_casters_intersect(cached.caster_generation, cached.basis, cached.origin, cached.minimum, cached.maximum, margin)) {
					casters_unchanged = true;
					casters_scanned = true;
				}
				if (i == 0) {
					bool pose_changed = cached.camera_basis != p_cam_transform.basis;
					for (int axis = 0; axis < 3; axis++) {
						pose_changed = pose_changed || cached.origin[axis] != p_cam_origin[axis];
					}
					if (pose_changed) {
						cached.force |= 1 << InstanceLightData::DirectionalShadowCache::COVERAGE;
					}
				} else if (cascade.full_coverage) {
					Vector3 offset;
					for (int axis = 0; axis < 3; axis++) {
						offset[axis] = p_cam_origin[axis] - cached.origin[axis];
					}
					const Basis inverse_basis = cached.basis.transposed();
					for (const Vector3 &endpoint : endpoints) {
						const Vector3 receiver = inverse_basis.xform(endpoint + offset);
						for (int axis = 0; axis < 3; axis++) {
							if (receiver[axis] < cached.minimum[axis] + margin || receiver[axis] > cached.maximum[axis] - margin) {
								cached.force |= 1 << InstanceLightData::DirectionalShadowCache::COVERAGE;
							}
						}
					}
				}
			}
			if (cached.valid && cached.force == 0 && (casters_unchanged || (frame % period != phase && age < period))) {
				if (casters_scanned) {
					cached.caster_generation = caster_generation;
				}
				cached.reused++;
				continue;
			}
		}
		cascade.refresh = true;

		// obtain the light frustum ranges (given endpoints)

		Transform3D transform = light_transform; //discard scale and stabilize light

		Vector3 x_vec = transform.basis.get_column(Vector3::AXIS_X).normalized();
		Vector3 y_vec = transform.basis.get_column(Vector3::AXIS_Y).normalized();
		Vector3 z_vec = transform.basis.get_column(Vector3::AXIS_Z).normalized();
		//z_vec points against the camera, like in default opengl

		real_t x_min = 0.f, x_max = 0.f;
		real_t y_min = 0.f, y_max = 0.f;
		real_t z_min = 0.f, z_max = 0.f;

		// FIXME: z_max_cam is defined, computed, but not used below when setting up
		// ortho_camera. Commented out for now to fix warnings but should be investigated.
		real_t x_min_cam = 0.f, x_max_cam = 0.f;
		real_t y_min_cam = 0.f, y_max_cam = 0.f;
		real_t z_min_cam = 0.f;
		//real_t z_max_cam = 0.f;

		//real_t bias_scale = 1.0;
		//real_t aspect_bias_scale = 1.0;

		//used for culling

		for (int j = 0; j < 8; j++) {
			real_t d_x = x_vec.dot(endpoints[j]);
			real_t d_y = y_vec.dot(endpoints[j]);
			real_t d_z = z_vec.dot(endpoints[j]);

			if (j == 0 || d_x < x_min) {
				x_min = d_x;
			}
			if (j == 0 || d_x > x_max) {
				x_max = d_x;
			}

			if (j == 0 || d_y < y_min) {
				y_min = d_y;
			}
			if (j == 0 || d_y > y_max) {
				y_max = d_y;
			}

			if (j == 0 || d_z < z_min) {
				z_min = d_z;
			}
			if (j == 0 || d_z > z_max) {
				z_max = d_z;
			}
		}

		real_t radius = 0;
		real_t soft_shadow_expand = 0;
		Vector3 center;

		{
			//camera viewport stuff

			// Far cached cascades stay centered on the camera origin so a yaw cannot move the cached box.
			const bool camera_centered = cascade.full_coverage && i >= 2;

			if (!camera_centered) {
				for (int j = 0; j < 8; j++) {
					center += endpoints[j];
				}
				center /= 8.0;
			}

			//center=x_vec*(x_max-x_min)*0.5 + y_vec*(y_max-y_min)*0.5 + z_vec*(z_max-z_min)*0.5;

			for (int j = 0; j < 8; j++) {
				real_t d = center.distance_to(endpoints[j]);
				if (d > radius) {
					radius = d;
				}
			}

			radius *= texture_size / (texture_size - 2.0); //add a texel by each side
			if (cascade.full_coverage) {
				radius *= 1.1;
			}

			z_min_cam = z_vec.dot(center) - radius;

			{
				float soft_shadow_angle = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_SIZE);

				if (soft_shadow_angle > 0.0) {
					float z_range = (z_vec.dot(center) + radius + pancake_size) - z_min_cam;
					soft_shadow_expand = Math::tan(Math::deg_to_rad(soft_shadow_angle)) * z_range;

					x_max += soft_shadow_expand;
					y_max += soft_shadow_expand;

					x_min -= soft_shadow_expand;
					y_min -= soft_shadow_expand;
				}
			}

			// This trick here is what stabilizes the shadow (make potential jaggies to not move)
			// at the cost of some wasted resolution. Still, the quality increase is very well worth it.
			const real_t unit = (radius + soft_shadow_expand) * 4.0 / texture_size;
			double camera_x = 0.0;
			double camera_y = 0.0;
			for (int axis = 0; axis < 3; axis++) {
				camera_x += double(x_vec[axis]) * p_cam_origin[axis];
				camera_y += double(y_vec[axis]) * p_cam_origin[axis];
			}
			x_max_cam = Math::snapped(camera_x + double(x_vec.dot(center) + radius + soft_shadow_expand), double(unit)) - camera_x;
			x_min_cam = Math::snapped(camera_x + double(x_vec.dot(center) - radius - soft_shadow_expand), double(unit)) - camera_x;
			y_max_cam = Math::snapped(camera_y + double(y_vec.dot(center) + radius + soft_shadow_expand), double(unit)) - camera_y;
			y_min_cam = Math::snapped(camera_y + double(y_vec.dot(center) - radius - soft_shadow_expand), double(unit)) - camera_y;
		}

		if (cascade.full_coverage) {
			x_min = x_min_cam;
			x_max = x_max_cam;
			y_min = y_min_cam;
			y_max = y_max_cam;
			z_min = z_min_cam;
		}

		//now that we know all ranges, we can proceed to make the light frustum planes, for culling octree

		Vector<Plane> light_frustum_planes;
		light_frustum_planes.resize(6);

		//right/left
		light_frustum_planes.write[0] = Plane(x_vec, x_max);
		light_frustum_planes.write[1] = Plane(-x_vec, -x_min);
		//top/bottom
		light_frustum_planes.write[2] = Plane(y_vec, y_max);
		light_frustum_planes.write[3] = Plane(-y_vec, -y_min);
		//near/far
		light_frustum_planes.write[4] = Plane(z_vec, z_max + 1e6);
		light_frustum_planes.write[5] = Plane(-z_vec, -z_min); // z_min is ok, since casters further than far-light plane are not needed

		// a pre pass will need to be needed to determine the actual z-near to be used

		z_max = z_vec.dot(center) + radius + pancake_size;
		cascade.coverage_minimum = Vector3(x_min_cam, y_min_cam, z_min_cam);
		cascade.coverage_maximum = Vector3(x_max_cam, y_max_cam, z_max);

		{
			Projection ortho_camera;
			real_t half_x = (x_max_cam - x_min_cam) * 0.5;
			real_t half_y = (y_max_cam - y_min_cam) * 0.5;

			ortho_camera.set_orthogonal(-half_x, half_x, -half_y, half_y, 0, (z_max - z_min_cam));

			Vector2 uv_scale(1.0 / (x_max_cam - x_min_cam), 1.0 / (y_max_cam - y_min_cam));

			Transform3D ortho_transform;
			ortho_transform.basis = transform.basis;
			ortho_transform.origin = x_vec * (x_min_cam + half_x) + y_vec * (y_min_cam + half_y) + z_vec * z_max;

			for (int axis = 0; axis < 3; axis++) {
				cull.shadows[p_shadow_index].cascades[i].origin[axis] = p_cam_origin[axis] + double(ortho_transform.origin[axis]);
				ortho_transform.origin[axis] = cull.shadows[p_shadow_index].cascades[i].origin[axis];
			}
			cull.shadows[p_shadow_index].cascades[i].frustum = Frustum(light_frustum_planes, p_cam_origin);
			cull.shadows[p_shadow_index].cascades[i].camera_basis = p_cam_transform.basis;
			cull.shadows[p_shadow_index].cascades[i].projection = ortho_camera;
			cull.shadows[p_shadow_index].cascades[i].transform = ortho_transform;
			cull.shadows[p_shadow_index].cascades[i].zfar = z_max - z_min_cam;
			cull.shadows[p_shadow_index].cascades[i].split = distances[i + 1];
			cull.shadows[p_shadow_index].cascades[i].shadow_texel_size = radius * 2.0 / texture_size;
			cull.shadows[p_shadow_index].cascades[i].bias_scale = (z_max - z_min_cam);
			double range_begin = z_max;
			for (int axis = 0; axis < 3; axis++) {
				range_begin += double(z_vec[axis]) * p_cam_origin[axis];
			}
			cull.shadows[p_shadow_index].cascades[i].range_begin = range_begin;
			cull.shadows[p_shadow_index].cascades[i].uv_scale = uv_scale;
		}
	}
}

bool RendererSceneCull::_light_instance_update_shadow(Instance *p_instance, const Transform3D p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, bool p_cam_vaspect, RID p_shadow_atlas, Scenario *p_scenario, float p_screen_mesh_lod_threshold, uint32_t p_visible_layers) {
	InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);

	struct ShadowTransform {
		Projection projection;
		Transform3D transform;
		real_t radius;
		int pass;
	};
	LocalVector<ShadowTransform> transforms;
	HashSet<RID> mesh_updates;
	uint32_t shadow_count = max_shadows_used;
	const bool can_render_cube = RSG::light_storage->light_instances_can_render_shadow_cube();
	bool animated = false;
	auto prepare = [&](uint32_t) {
		auto gather = [&]() {
			Transform3D light_transform = p_instance->transform;
			light_transform.origin = Vector3();
			light_transform.orthonormalize(); //scale does not count on lights

			bool animated_material_found = false;

			switch (RSG::light_storage->light_get_type(p_instance->base)) {
				case RSE::LIGHT_DIRECTIONAL: {
				} break;
				case RSE::LIGHT_OMNI: {
					RSE::LightOmniShadowMode shadow_mode = RSG::light_storage->light_omni_get_shadow_mode(p_instance->base);

					if (shadow_mode == RSE::LIGHT_OMNI_SHADOW_DUAL_PARABOLOID || !can_render_cube) {
						if (shadow_count + 2 > MAX_UPDATE_SHADOWS) {
							return true;
						}
						for (int i = 0; i < 2; i++) {
							//using this one ensures that raster deferred will have it

							real_t radius = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_RANGE);

							real_t z = i == 0 ? -1 : 1;
							Vector<Plane> planes;
							planes.resize(6);
							planes.write[0] = light_transform.xform(Plane(Vector3(0, 0, z), radius));
							planes.write[1] = light_transform.xform(Plane(Vector3(1, 0, z).normalized(), radius));
							planes.write[2] = light_transform.xform(Plane(Vector3(-1, 0, z).normalized(), radius));
							planes.write[3] = light_transform.xform(Plane(Vector3(0, 1, z).normalized(), radius));
							planes.write[4] = light_transform.xform(Plane(Vector3(0, -1, z).normalized(), radius));
							planes.write[5] = light_transform.xform(Plane(Vector3(0, 0, -z), 0));

							instance_shadow_cull_result.clear();

							_cull_shadow_geometry(p_scenario, planes, p_instance->origin, instance_shadow_cull_result);

							RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[shadow_count++];
							shadow_data.cull_planes = planes;
							for (int axis = 0; axis < 3; axis++) {
								shadow_data.cull_origin[axis] = p_instance->origin[axis];
							}
							if (!light->is_shadow_update_full()) {
								light_culler->append_caster_planes(shadow_data.cull_planes, -1, 0, shadow_data.cull_origin);
							}

							if (!light->is_shadow_update_full()) {
								light_culler->cull_regular_light(instance_shadow_cull_result);
							}

							for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
								Instance *instance = instance_shadow_cull_result[j];
								const bool is_inactive_particle = (instance->base_type == RSE::INSTANCE_PARTICLES) && RSG::particles_storage->particles_is_inactive(instance->base);
								if (!instance->visible || !((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask & RSG::light_storage->light_get_shadow_caster_mask(p_instance->base)) || is_inactive_particle) {
									continue;
								} else {
									if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
										animated_material_found = true;
									}

									if (instance->mesh_instance.is_valid()) {
										mesh_updates.insert(instance->mesh_instance);
									}
								}

								shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
							}

							transforms.push_back({ Projection(), light_transform, radius, i });
							shadow_data.light = light->instance;
							shadow_data.pass = i;
						}
					} else { //shadow cube

						if (shadow_count + 6 > MAX_UPDATE_SHADOWS) {
							return true;
						}

						real_t radius = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_RANGE);
						real_t z_near = MIN(0.025f, radius);
						Projection cm;
						cm.set_perspective(90, 1, z_near, radius);

						for (int i = 0; i < 6; i++) {
							//using this one ensures that raster deferred will have it

							static const Vector3 view_normals[6] = {
								Vector3(+1, 0, 0),
								Vector3(-1, 0, 0),
								Vector3(0, -1, 0),
								Vector3(0, +1, 0),
								Vector3(0, 0, +1),
								Vector3(0, 0, -1)
							};
							static const Vector3 view_up[6] = {
								Vector3(0, -1, 0),
								Vector3(0, -1, 0),
								Vector3(0, 0, -1),
								Vector3(0, 0, +1),
								Vector3(0, -1, 0),
								Vector3(0, -1, 0)
							};

							Transform3D xform = light_transform * Transform3D().looking_at(view_normals[i], view_up[i]);

							Vector<Plane> planes = cm.get_projection_planes(xform);

							instance_shadow_cull_result.clear();

							_cull_shadow_geometry(p_scenario, planes, p_instance->origin, instance_shadow_cull_result);

							RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[shadow_count++];
							shadow_data.cull_planes = planes;
							for (int axis = 0; axis < 3; axis++) {
								shadow_data.cull_origin[axis] = p_instance->origin[axis];
							}
							if (!light->is_shadow_update_full()) {
								light_culler->append_caster_planes(shadow_data.cull_planes, -1, 0, shadow_data.cull_origin);
							}

							if (!light->is_shadow_update_full()) {
								light_culler->cull_regular_light(instance_shadow_cull_result);
							}

							for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
								Instance *instance = instance_shadow_cull_result[j];
								const bool is_inactive_particle = (instance->base_type == RSE::INSTANCE_PARTICLES) && RSG::particles_storage->particles_is_inactive(instance->base);
								if (!instance->visible || !((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask & RSG::light_storage->light_get_shadow_caster_mask(p_instance->base)) || is_inactive_particle) {
									continue;
								} else {
									if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
										animated_material_found = true;
									}
									if (instance->mesh_instance.is_valid()) {
										mesh_updates.insert(instance->mesh_instance);
									}
								}

								shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
							}

							transforms.push_back({ cm, xform, radius, i });

							shadow_data.light = light->instance;
							shadow_data.pass = i;
						}

						//restore the regular DP matrix
					}

				} break;
				case RSE::LIGHT_SPOT: {
					if (shadow_count + 1 > MAX_UPDATE_SHADOWS) {
						return true;
					}

					real_t radius = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_RANGE);
					real_t angle = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_SPOT_ANGLE);
					real_t z_near = MIN(0.025f, radius);

					Projection cm;
					cm.set_perspective(angle * 2.0, 1.0, z_near, radius);

					Vector<Plane> planes = cm.get_projection_planes(light_transform);

					instance_shadow_cull_result.clear();

					_cull_shadow_geometry(p_scenario, planes, p_instance->origin, instance_shadow_cull_result);

					RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[shadow_count++];
					shadow_data.cull_planes = planes;
					for (int axis = 0; axis < 3; axis++) {
						shadow_data.cull_origin[axis] = p_instance->origin[axis];
					}
					if (!light->is_shadow_update_full()) {
						light_culler->append_caster_planes(shadow_data.cull_planes, -1, 0, shadow_data.cull_origin);
					}

					if (!light->is_shadow_update_full()) {
						light_culler->cull_regular_light(instance_shadow_cull_result);
					}

					for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
						Instance *instance = instance_shadow_cull_result[j];
						const bool is_inactive_particle = (instance->base_type == RSE::INSTANCE_PARTICLES) && RSG::particles_storage->particles_is_inactive(instance->base);
						if (!instance->visible || !((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask & RSG::light_storage->light_get_shadow_caster_mask(p_instance->base)) || is_inactive_particle) {
							continue;
						} else {
							if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
								animated_material_found = true;
							}

							if (instance->mesh_instance.is_valid()) {
								mesh_updates.insert(instance->mesh_instance);
							}
						}
						shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
					}

					transforms.push_back({ cm, light_transform, radius, 0 });
					shadow_data.light = light->instance;
					shadow_data.pass = 0;

				} break;
				case RSE::LIGHT_AREA: {
					if (shadow_count + 1 > MAX_UPDATE_SHADOWS) {
						return true;
					}

					real_t radius = RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_RANGE);
					Vector2 half_size = RSG::light_storage->light_area_get_size(p_instance->base) / 2.0;

					real_t z = -1;
					Vector<Plane> planes;
					planes.resize(6);
					planes.write[0] = light_transform.xform(Plane(Vector3(0, 0, z), radius));
					planes.write[1] = light_transform.xform(Plane(Vector3(1, 0, 0).normalized(), radius + half_size.x));
					planes.write[2] = light_transform.xform(Plane(Vector3(-1, 0, 0).normalized(), radius + half_size.x));
					planes.write[3] = light_transform.xform(Plane(Vector3(0, 1, 0).normalized(), radius + half_size.y));
					planes.write[4] = light_transform.xform(Plane(Vector3(0, -1, 0).normalized(), radius + half_size.y));
					planes.write[5] = light_transform.xform(Plane(Vector3(0, 0, -z), 0));

					instance_shadow_cull_result.clear();

					_cull_shadow_geometry(p_scenario, planes, p_instance->origin, instance_shadow_cull_result);

					RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[shadow_count++];
					shadow_data.cull_planes = planes;
					for (int axis = 0; axis < 3; axis++) {
						shadow_data.cull_origin[axis] = p_instance->origin[axis];
					}
					if (!light->is_shadow_update_full()) {
						light_culler->append_caster_planes(shadow_data.cull_planes, -1, 0, shadow_data.cull_origin);
					}

					if (!light->is_shadow_update_full()) {
						light_culler->cull_regular_light(instance_shadow_cull_result);
					}

					for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
						Instance *instance = instance_shadow_cull_result[j];
						const bool is_inactive_particle = (instance->base_type == RSE::INSTANCE_PARTICLES) && RSG::particles_storage->particles_is_inactive(instance->base);
						if (!instance->visible || !((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask & RSG::light_storage->light_get_shadow_caster_mask(p_instance->base)) || is_inactive_particle) {
							continue;
						} else {
							if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
								animated_material_found = true;
							}

							if (instance->mesh_instance.is_valid()) {
								mesh_updates.insert(instance->mesh_instance);
							}
						}

						shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
					}

					transforms.push_back({ Projection(), light_transform, radius, 0 });
					shadow_data.light = light->instance;
					shadow_data.pass = 0;
				}
			}

			return animated_material_found;
		};
		animated = gather();
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("PositionalShadowCull");
		(*static_cast<decltype(prepare) *>(p_data))(p_index);
	},
			&prepare, 1, 1, true, SNAME("PositionalShadowCull"));
	pool->wait_for_group_task_completion(job);
	max_shadows_used = shadow_count;
	for (RID mesh : mesh_updates) {
		RSG::mesh_storage->mesh_instance_check_for_update(mesh);
	}
	if (!transforms.is_empty()) {
		RSG::mesh_storage->update_mesh_instances();
	}
	for (const ShadowTransform &transform : transforms) {
		Transform3D world_transform = transform.transform;
		double origin[3];
		for (int axis = 0; axis < 3; axis++) {
			origin[axis] = p_instance->origin[axis] + double(world_transform.origin[axis]);
			world_transform.origin[axis] = origin[axis];
		}
		RSG::light_storage->light_instance_set_shadow_transform(light->instance, transform.projection, world_transform, transform.radius, 0, transform.pass, 0, 1.0, 0.0, Vector2(), origin);
	}
	return animated;
}
void RendererSceneCull::render_camera(const Ref<RenderSceneBuffers> &p_render_buffers, RID p_camera, RID p_scenario, RID p_viewport, Size2 p_viewport_size, uint32_t p_jitter_phase_count, float p_screen_mesh_lod_threshold, RID p_shadow_atlas, Ref<XRInterface> &p_xr_interface, float p_window_output_max_value, RenderingServerTypes::RenderInfo *r_render_info) {
#ifndef _3D_DISABLED

	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_NULL(camera);
	if (Scenario *scenario = scenario_owner.get_or_null(p_scenario)) {
		scenario->sampling_camera = p_camera;
	}

	Vector2 jitter;
	float taa_frame_count = 0.0f;
	if (p_jitter_phase_count > 0) {
		constexpr double plastic_number = 1.32471795724474602596;
		constexpr double alpha_x = 1.0 / plastic_number;
		constexpr double alpha_y = 1.0 / (plastic_number * plastic_number);
		const double frame = double(RSG::rasterizer->get_frame_number());
		const double offset_x = Math::fract(0.5 + alpha_x * frame) * 2.0 - 1.0;
		const double offset_y = Math::fract(0.5 + alpha_y * frame) * 2.0 - 1.0;
		jitter = Vector2(real_t(offset_x), real_t(offset_y)) / p_viewport_size;
		taa_frame_count = float(RSG::rasterizer->get_frame_number() % p_jitter_phase_count);
	}

	RendererSceneRender::CameraData camera_data;

	// Setup Camera(s)
	if (p_xr_interface.is_null()) {
		// Normal camera
		Transform3D transform = camera->transform;
		Projection projection;
		bool vaspect = camera->vaspect;
		bool is_orthogonal = false;

		switch (camera->type) {
			case Camera::ORTHOGONAL: {
				projection.set_orthogonal(
						camera->size,
						p_viewport_size.width / (float)p_viewport_size.height,
						camera->znear,
						camera->zfar,
						camera->vaspect);
				is_orthogonal = true;
			} break;
			case Camera::PERSPECTIVE: {
				projection.set_perspective(
						camera->fov,
						p_viewport_size.width / (float)p_viewport_size.height,
						camera->znear,
						camera->zfar,
						camera->vaspect);

			} break;
			case Camera::FRUSTUM: {
				projection.set_frustum(
						camera->size,
						p_viewport_size.width / (float)p_viewport_size.height,
						camera->offset,
						camera->znear,
						camera->zfar,
						camera->vaspect);
			} break;
		}

		camera_data.set_camera(transform, projection, is_orthogonal, vaspect, jitter, taa_frame_count, camera->visible_layers, camera->origin);
#ifndef XR_DISABLED
	} else {
		XRServer *xr_server = XRServer::get_singleton();

		// Setup our camera for our XR interface.
		// We can support multiple views here each with their own camera
		Transform3D transforms[RendererSceneRender::MAX_RENDER_VIEWS];
		Projection projections[RendererSceneRender::MAX_RENDER_VIEWS];

		uint32_t view_count = p_xr_interface->get_view_count();
		ERR_FAIL_COND_MSG(view_count == 0 || view_count > RendererSceneRender::MAX_RENDER_VIEWS, "Requested view count is not supported");

		float aspect = p_viewport_size.width / (float)p_viewport_size.height;

		Transform3D world_origin = xr_server->get_world_origin();

		// We ignore our camera position, it will have been positioned with a slightly old tracking position.
		// Instead we take our origin point and have our XR interface add fresh tracking data! Whoohoo!
		for (uint32_t v = 0; v < view_count; v++) {
			transforms[v] = p_xr_interface->get_transform_for_view(v, world_origin);
			projections[v] = p_xr_interface->get_projection_for_view(v, aspect, camera->znear, camera->zfar);
		}

		// If requested, we move the views to be rendered as if the HMD is at the XROrigin.
		if (unlikely(xr_server->is_camera_locked_to_origin())) {
			Transform3D camera_reset = p_xr_interface->get_camera_transform().affine_inverse() * xr_server->get_reference_frame().affine_inverse();
			for (uint32_t v = 0; v < view_count; v++) {
				transforms[v] *= camera_reset;
			}
		}

		if (view_count == 1) {
			camera_data.set_camera(transforms[0], projections[0], false, camera->vaspect, jitter, p_jitter_phase_count, camera->visible_layers);
		} else if (view_count == 2) {
			camera_data.set_multiview_camera(view_count, transforms, projections, false, camera->vaspect, camera->visible_layers);
		} else {
			// this won't be called (see fail check above) but keeping this comment to indicate we may support more then 2 views in the future...
		}
#endif // XR_DISABLED
	}

	RID environment = _render_get_environment(p_camera, p_scenario);
	RID compositor = _render_get_compositor(p_camera, p_scenario);

	RENDER_TIMESTAMP("Update Occlusion Buffer")
	// For now just cull on the first camera
	RendererSceneOcclusionCull::get_singleton()->buffer_update(p_viewport, camera_data.main_transform, camera_data.main_projection, camera_data.is_orthogonal);

	_render_scene(p_camera, &camera_data, p_render_buffers, environment, camera->attributes, compositor, camera->visible_layers, p_scenario, p_viewport, p_shadow_atlas, RID(), -1, p_screen_mesh_lod_threshold, p_window_output_max_value, true, r_render_info);
#endif
}

void RendererSceneCull::_visibility_cull_threaded(uint32_t p_thread, VisibilityCullData *cull_data) {
	uint32_t total_threads = cull_data->job_count;
	uint32_t bin_from = p_thread * cull_data->cull_count / total_threads;
	uint32_t bin_to = (p_thread + 1 == total_threads) ? cull_data->cull_count : ((p_thread + 1) * cull_data->cull_count / total_threads);

	_visibility_cull(*cull_data, cull_data->cull_offset + bin_from, cull_data->cull_offset + bin_to);
}

void RendererSceneCull::_visibility_cull(VisibilityCullData &cull_data, uint64_t p_from, uint64_t p_to) {
	Scenario *scenario = cull_data.scenario;
	for (unsigned int i = p_from; i < p_to; i++) {
		VisibilityCullData::Result &result = cull_data.results[i - cull_data.cull_offset];
		result = {};
		result.visibility = scenario->instance_visibility[i];
		InstanceVisibilityData &vd = result.visibility;
		const InstanceData &idata = scenario->instance_data[vd.array_index];
		result.flags = idata.flags;

		const uint32_t hidden_mask = InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN | InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
		const bool was_hidden = (result.flags & hidden_mask) != 0;
		bool hidden_by_parent = false;

		if (idata.parent_array_index >= 0) {
			uint32_t parent_flags = scenario->instance_data[idata.parent_array_index].flags;

			if ((parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN) || !(parent_flags & (InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE | InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN))) {
				result.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
				hidden_by_parent = true;
			}
		}

		if (!hidden_by_parent) {
			int range_check = _visibility_range_check<true>(vd, cull_data.camera_position, cull_data.viewport_mask);

			if (range_check == -1) {
				result.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
			} else if (range_check == 1) {
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
				result.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
			} else {
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
				result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
				if (range_check == 2) {
					result.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
				} else {
					result.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
				}

				if (was_hidden) {
					const uint32_t base_type = result.flags & InstanceData::FLAG_BASE_TYPE_MASK;
					if ((1u << base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
						result.reset_motion = true;
					}
				}
			}
		}

		const bool is_hidden = (result.flags & hidden_mask) != 0;
		if (is_hidden != was_hidden && (result.flags & InstanceData::FLAG_CAST_SHADOWS)) {
			result.shadow_caster_changed = true;
		}
	}
}

template <bool p_fade_check>
int RendererSceneCull::_visibility_range_check(InstanceVisibilityData &r_vis_data, const double *p_camera_pos, uint64_t p_viewport_mask) {
	double distance_squared = 0.0;
	for (int axis = 0; axis < 3; axis++) {
		const double delta = p_camera_pos[axis] - r_vis_data.position[axis];
		distance_squared += delta * delta;
	}
	const float dist = Math::sqrt(distance_squared);
	const RSE::VisibilityRangeFadeMode &fade_mode = r_vis_data.fade_mode;

	float begin_offset = -r_vis_data.range_begin_margin;
	float end_offset = r_vis_data.range_end_margin;

	if (fade_mode == RSE::VISIBILITY_RANGE_FADE_DISABLED && !(p_viewport_mask & r_vis_data.viewport_state)) {
		begin_offset = -begin_offset;
		end_offset = -end_offset;
	}

	if (r_vis_data.range_end > 0.0f && dist > r_vis_data.range_end + end_offset) {
		r_vis_data.viewport_state &= ~p_viewport_mask;
		return -1;
	} else if (r_vis_data.range_begin > 0.0f && dist < r_vis_data.range_begin + begin_offset) {
		r_vis_data.viewport_state &= ~p_viewport_mask;
		return 1;
	} else {
		r_vis_data.viewport_state |= p_viewport_mask;
		if (p_fade_check) {
			if (fade_mode != RSE::VISIBILITY_RANGE_FADE_DISABLED) {
				r_vis_data.children_fade_alpha = 1.0f;
				if (r_vis_data.range_end > 0.0f && dist > r_vis_data.range_end - end_offset) {
					if (fade_mode == RSE::VISIBILITY_RANGE_FADE_DEPENDENCIES) {
						r_vis_data.children_fade_alpha = MIN(1.0f, (dist - (r_vis_data.range_end - end_offset)) / (2.0f * r_vis_data.range_end_margin));
					}
					return 2;
				} else if (r_vis_data.range_begin > 0.0f && dist < r_vis_data.range_begin - begin_offset) {
					if (fade_mode == RSE::VISIBILITY_RANGE_FADE_DEPENDENCIES) {
						r_vis_data.children_fade_alpha = MIN(1.0f, 1.0 - (dist - (r_vis_data.range_begin + begin_offset)) / (2.0f * r_vis_data.range_begin_margin));
					}
					return 2;
				}
			}
		}
		return 0;
	}
}

bool RendererSceneCull::_visibility_parent_check(const CullData &p_cull_data, const InstanceData &p_instance_data) {
	if (p_instance_data.parent_array_index == -1) {
		return true;
	}
	const uint32_t &parent_flags = p_cull_data.scenario->instance_data[p_instance_data.parent_array_index].flags;
	return ((parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK) == InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE) || (parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN);
}

void RendererSceneCull::_scene_cull_threaded(uint32_t p_thread, CullData *cull_data) {
	GodotProfileZone("RenderCullInstances");
	auto &result = scene_cull_result_threads[p_thread];
	if (cull_data->profile) {
		result.worker = Thread::get_caller_id();
		result.begin_usec = OS::get_singleton()->get_ticks_usec();
	}
	uint32_t cull_total = cull_data->scenario->conventional_instances.size();
	uint32_t total_threads = scene_cull_result_threads.size();
	uint32_t cull_from = p_thread * cull_total / total_threads;
	uint32_t cull_to = (p_thread + 1 == total_threads) ? cull_total : ((p_thread + 1) * cull_total / total_threads);

	_scene_cull(*cull_data, result, cull_from, cull_to);

	if (cull_data->profile) {
		result.end_usec = OS::get_singleton()->get_ticks_usec();
	}
}

void RendererSceneCull::_scene_cull_geometry_updates(CullData &cull_data, const InstanceCullResult &p_result) {
	uint64_t frame_number = RSG::rasterizer->get_frame_number();
	float lightmap_probe_update_speed = RSG::light_storage->lightmap_get_probe_capture_update_speed() * RSG::rasterizer->get_frame_delta_time();

	RID instance_pair_buffer[MAX_INSTANCE_PAIRS];

	// Minimize allocations when picking the most relevant lights per mesh.
	// We need to track the score and current index of the best N lights.
	thread_local LocalVector<Pair<float, uint32_t>> omni_score_idx, spot_score_idx, area_score_idx;
	omni_score_idx.clear();
	spot_score_idx.clear();
	area_score_idx.clear();
	uint32_t max_lights_per_mesh = scene_render->get_max_lights_per_mesh();
	uint32_t max_lights_total = scene_render->get_max_lights_total();

	for (const auto &change : p_result.state_changes) {
		InstanceData &idata = cull_data.scenario->instance_data[change.index];
		idata.occlusion_timeout = change.occlusion_timeout;
		if (idata.visibility_index >= 0) {
			cull_data.scenario->instance_visibility[idata.visibility_index].viewport_state = change.viewport_state;
		}
	}
	for (const auto &visibility : p_result.rt_visibility) {
		visibility.instance->set_rt_visibility(visibility.receiver, visibility.caster, visibility.shadows_only);
	}
	for (uint64_t index : p_result.geometry_updates) {
		InstanceData &idata = cull_data.scenario->instance_data[index];
		if (idata.parent_array_index != -1) {
			float fade = 1.0f;
			const uint32_t &parent_flags = cull_data.scenario->instance_data[idata.parent_array_index].flags;
			if (parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN) {
				const int32_t &parent_idx = cull_data.scenario->instance_data[idata.parent_array_index].visibility_index;
				fade = cull_data.scenario->instance_visibility[parent_idx].children_fade_alpha;
			}
			idata.instance_geometry->set_parent_fade_alpha(fade);
		}

		if (geometry_instance_pair_mask & (1 << RSE::INSTANCE_LIGHT) && (idata.flags & InstanceData::FLAG_GEOM_LIGHTING_DIRTY)) {
			InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
			ERR_FAIL_NULL(geom->geometry_instance);
			// Clear any existing light instances for this mesh and find the max count per-mesh, and total (per-scene).
			geom->geometry_instance->clear_light_instances();
			if ((max_lights_per_mesh > 0) && (max_lights_total > 0)) {
				// For the top N lights, track the score and the index into the internal light storage array.
				uint32_t total_omni_count = 0, total_spot_count = 0, total_area_count = 0;
				bool omni_needs_heap = true, spot_needs_heap = true, area_needs_heap = true;
				uint32_t omni_count = 0, spot_count = 0, area_count = 0;
				omni_score_idx.clear();
				spot_score_idx.clear();
				area_score_idx.clear();
				SortArray<Pair<float, uint32_t>> heapify; // SortArray has heap functions, but no local storage.
				// Iterate over the lights (possibly > max_renderable_lights), keeping the closest to the mesh center.
				Vector3 mesh_center = idata.instance->transformed_aabb.get_center();
				for (const Instance *E : geom->lights) {
					RSE::LightType light_type = RSG::light_storage->light_get_type(E->base);
					if (((RSE::LIGHT_OMNI == light_type) && (total_omni_count++ < max_lights_total)) ||
							((RSE::LIGHT_SPOT == light_type) && (total_spot_count++ < max_lights_total)) ||
							((RSE::LIGHT_AREA == light_type) && (total_area_count++ < max_lights_total))) {
						// Perform culling.
						if (!(RSG::light_storage->light_get_cull_mask(E->base) & idata.layer_mask)) {
							continue;
						}
						if ((RSG::light_storage->light_get_bake_mode(E->base) == RSE::LIGHT_BAKE_STATIC) && idata.instance->lightmap) {
							continue;
						}

						InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);
						// Large scores are worse, so linear with distance, inverse with energy and range.
						Vector3 light_center = E->transformed_aabb.get_center();
						float light_range_energy =
								RSG::light_storage->light_get_param(E->base, RSE::LightParam::LIGHT_PARAM_RANGE) *
								RSG::light_storage->light_get_param(E->base, RSE::LightParam::LIGHT_PARAM_ENERGY);
						float light_inst_score = mesh_center.distance_to(light_center) / MAX(0.01f, light_range_energy);
						// Of the N lights (on a per-light-type basis, Omni or Spot) keep only the M "best" lights.
						// If N <= M, we can simply store the lights, but once we exceed M, we need check each new
						// light and see if it's score is better than the worst light stored to date.  If the new
						// light is better, we can replace the current worst light with the new one.  In order to
						// efficiently track our currently worst light we use a "max heap".  This loosely orders
						// the elements in an array as a binary-tree structure, and has the properties that finding
						// the worst score element is O(1) (it will always be stored in element [0]), and removing
						// the old max and inserting a new value is O(log M).
#define VERIFY_RELEVANT_LIGHT_HEAP 0
#if VERIFY_RELEVANT_LIGHT_HEAP
									WARN_PRINT_ONCE("VERIFY_RELEVANT_LIGHT_HEAP is True");
#endif
									switch (light_type) {
										case RSE::LIGHT_OMNI: {
											if (omni_count < max_lights_per_mesh) {
												// We have room to just add it, and track the score and where it goes.
												omni_score_idx.push_back(Pair(light_inst_score, omni_count));
												geom->geometry_instance->pair_light_instance(light->instance, light_type, omni_count++);
											} else {
												if (omni_needs_heap) {
													// We need to make this a heap one time.
													heapify.make_heap(0, omni_count, &omni_score_idx[0]);
													omni_needs_heap = false;
												}
												if (light_inst_score < omni_score_idx[0].first) {
#if VERIFY_RELEVANT_LIGHT_HEAP
													// The [0] element should have the max score.
													for (uint32_t vi = 1; vi < max_lights_per_mesh; ++vi) {
														if (omni_score_idx[vi].first > omni_score_idx[0].first) {
															ERR_PRINT_ONCE("Relevant Omni Light Heap Error");
														}
													}
#endif
													uint32_t replace_index = omni_score_idx[0].second;
													geom->geometry_instance->pair_light_instance(light->instance, light_type, replace_index);
													heapify.adjust_heap(0, 0, omni_count, Pair(light_inst_score, replace_index), &omni_score_idx[0]);
												}
											}
										} break;
										case RSE::LIGHT_SPOT: {
											if (spot_count < max_lights_per_mesh) {
												// We have room to just add it, and track the score and where it goes.
												spot_score_idx.push_back(Pair(light_inst_score, spot_count));
												geom->geometry_instance->pair_light_instance(light->instance, light_type, spot_count++);
											} else {
												if (spot_needs_heap) {
													// We need to make this a heap one time.
													heapify.make_heap(0, spot_count, &spot_score_idx[0]);
													spot_needs_heap = false;
												}
												if (light_inst_score < spot_score_idx[0].first) {
#if VERIFY_RELEVANT_LIGHT_HEAP
													// The [0] element should have the max score.
													for (uint32_t vi = 1; vi < max_lights_per_mesh; ++vi) {
														if (spot_score_idx[vi].first > spot_score_idx[0].first) {
															ERR_PRINT_ONCE("Relevant Spot Light Heap Error");
														}
													}
#endif
													uint32_t replace_index = spot_score_idx[0].second;
													geom->geometry_instance->pair_light_instance(light->instance, light_type, replace_index);
													heapify.adjust_heap(0, 0, spot_count, Pair(light_inst_score, replace_index), &spot_score_idx[0]);
												}
											}
										} break;
										case RSE::LIGHT_AREA: {
											if (area_count < max_lights_per_mesh) {
												// We have room to just add it, and track the score and where it goes.
												area_score_idx.push_back(Pair(light_inst_score, area_count));
												geom->geometry_instance->pair_light_instance(light->instance, light_type, area_count++);
											} else {
												if (area_needs_heap) {
													// We need to make this a heap one time.
													heapify.make_heap(0, area_count, &area_score_idx[0]);
													area_needs_heap = false;
												}
												if (light_inst_score < area_score_idx[0].first) {
#if VERIFY_RELEVANT_LIGHT_HEAP
													// The [0] element should have the max score.
													for (uint32_t vi = 1; vi < max_lights_per_mesh; ++vi) {
														if (area_score_idx[vi].first > area_score_idx[0].first) {
															ERR_PRINT_ONCE("Relevant Area Light Heap Error");
														}
													}
#endif
													uint32_t replace_index = area_score_idx[0].second;
													geom->geometry_instance->pair_light_instance(light->instance, light_type, replace_index);
													heapify.adjust_heap(0, 0, area_count, Pair(light_inst_score, replace_index), &area_score_idx[0]);
												}
											}
										} break;
										default:
											break;
									}
								}
							}
						}
						idata.flags &= ~InstanceData::FLAG_GEOM_LIGHTING_DIRTY;
					}

					if (idata.flags & InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY) {
						InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);

						ERR_FAIL_NULL(geom->geometry_instance);
						geom->geometry_instance->set_softshadow_projector_pairing(geom->softshadow_count > 0, geom->projector_count > 0);
						idata.flags &= ~InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
					}

					if (geometry_instance_pair_mask & (1 << RSE::INSTANCE_REFLECTION_PROBE) && (idata.flags & InstanceData::FLAG_GEOM_REFLECTION_DIRTY)) {
						InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
						uint32_t idx = 0;

						for (const Instance *E : geom->reflection_probes) {
							InstanceReflectionProbeData *reflection_probe = static_cast<InstanceReflectionProbeData *>(E->base_data);

							instance_pair_buffer[idx++] = reflection_probe->instance;
							if (idx == MAX_INSTANCE_PAIRS) {
								break;
							}
						}

						ERR_FAIL_NULL(geom->geometry_instance);
						geom->geometry_instance->pair_reflection_probe_instances(instance_pair_buffer, idx);
						idata.flags &= ~InstanceData::FLAG_GEOM_REFLECTION_DIRTY;
					}

					if (geometry_instance_pair_mask & (1 << RSE::INSTANCE_DECAL) && (idata.flags & InstanceData::FLAG_GEOM_DECAL_DIRTY)) {
						InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
						uint32_t idx = 0;

						for (const Instance *E : geom->decals) {
							InstanceDecalData *decal = static_cast<InstanceDecalData *>(E->base_data);

							instance_pair_buffer[idx++] = decal->instance;
							if (idx == MAX_INSTANCE_PAIRS) {
								break;
							}
						}

						ERR_FAIL_NULL(geom->geometry_instance);
						geom->geometry_instance->pair_decal_instances(instance_pair_buffer, idx);

						idata.flags &= ~InstanceData::FLAG_GEOM_DECAL_DIRTY;
					}

					if (idata.flags & InstanceData::FLAG_GEOM_VOXEL_GI_DIRTY) {
						InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
						uint32_t idx = 0;
						for (const Instance *E : geom->voxel_gi_instances) {
							InstanceVoxelGIData *voxel_gi = static_cast<InstanceVoxelGIData *>(E->base_data);

							instance_pair_buffer[idx++] = voxel_gi->probe_instance;
							if (idx == MAX_INSTANCE_PAIRS) {
								break;
							}
						}

						ERR_FAIL_NULL(geom->geometry_instance);
						geom->geometry_instance->pair_voxel_gi_instances(instance_pair_buffer, idx);

						idata.flags &= ~InstanceData::FLAG_GEOM_VOXEL_GI_DIRTY;
					}

					if ((idata.flags & InstanceData::FLAG_LIGHTMAP_CAPTURE) && idata.instance->last_frame_pass != frame_number && !idata.instance->lightmap_target_sh.is_empty() && !idata.instance->lightmap_sh.is_empty()) {
						InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
						Color *sh = idata.instance->lightmap_sh.ptrw();
						const Color *target_sh = idata.instance->lightmap_target_sh.ptr();
						for (uint32_t j = 0; j < 9; j++) {
							sh[j] = sh[j].lerp(target_sh[j], MIN(1.0, lightmap_probe_update_speed));
						}
						ERR_FAIL_NULL(geom->geometry_instance);
						geom->geometry_instance->set_lightmap_capture(sh);
						idata.instance->last_frame_pass = frame_number;
					}
	}
}

void RendererSceneCull::_scene_cull(CullData &cull_data, InstanceCullResult &cull_result, uint64_t p_from, uint64_t p_to) {
	uint32_t sdfgi_last_light_index = 0xFFFFFFFF;
	uint32_t sdfgi_last_light_cascade = 0xFFFFFFFF;
	const Transform3D inv_cam_transform(cull_data.cam_transform.basis.inverse(), Vector3());
	float z_near = cull_data.camera_matrix->get_z_near();
	bool is_orthogonal = cull_data.camera_matrix->is_orthogonal();
	auto is_occluded = [&](const InstanceBounds &p_bounds, uint64_t &r_timeout) {
		real_t relative_bounds[6];
		for (int axis = 0; axis < 3; axis++) {
			for (int side = 0; side < 2; side++) {
				const int index = axis + side * 3;
				const double value = p_bounds.precise_bounds[index] - cull_data.camera_origin[axis];
				relative_bounds[index] = real_t(value);
				if (side ? double(relative_bounds[index]) < value : double(relative_bounds[index]) > value) {
					relative_bounds[index] = std::nextafter(relative_bounds[index], side ? std::numeric_limits<real_t>::infinity() : -std::numeric_limits<real_t>::infinity());
				}
			}
		}
		return cull_data.occlusion_buffer->is_occluded(relative_bounds, Vector3(), inv_cam_transform, *cull_data.camera_matrix, z_near, is_orthogonal, r_timeout);
	};

	for (uint64_t domain_index = p_from; domain_index < p_to; domain_index++) {
		const uint64_t i = cull_data.scenario->conventional_instances[domain_index]->array_index;
		bool mesh_visible = false;
		bool in_frustum = false;

		InstanceData idata = cull_data.scenario->instance_data[i];
		InstanceVisibilityData visibility;
		if (idata.visibility_index >= 0) {
			visibility = cull_data.scenario->instance_visibility[idata.visibility_index];
		}
		uint32_t visibility_flags = idata.flags & (InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE | InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN | InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN);
		int32_t visibility_check = -1;

#define HIDDEN_BY_VISIBILITY_CHECKS (visibility_flags == InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE || visibility_flags == InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN)
#define LAYER_CHECK (cull_data.visible_layers & idata.layer_mask)
#define IN_FRUSTUM(f) (cull_data.scenario->instance_aabbs[i].in_frustum(f))
#define VIS_RANGE_CHECK ((idata.visibility_index == -1) || _visibility_range_check<false>(visibility, cull_data.camera_origin, cull_data.visibility_viewport_mask) == 0)
#define VIS_PARENT_CHECK (_visibility_parent_check(cull_data, idata))
#define VIS_CHECK (visibility_check < 0 ? (visibility_check = (visibility_flags != InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK || (VIS_RANGE_CHECK && VIS_PARENT_CHECK))) : visibility_check)
#define OCCLUSION_CULLED (cull_data.occlusion_buffer != nullptr && (cull_data.scenario->instance_data[i].flags & InstanceData::FLAG_IGNORE_OCCLUSION_CULLING) == 0 && is_occluded(cull_data.scenario->instance_aabbs[i], idata.occlusion_timeout))

		if (!HIDDEN_BY_VISIBILITY_CHECKS) {
			if ((LAYER_CHECK && IN_FRUSTUM(cull_data.cull->frustum) && VIS_CHECK && !OCCLUSION_CULLED) || (cull_data.scenario->instance_data[i].flags & InstanceData::FLAG_IGNORE_ALL_CULLING)) {
				in_frustum = true;
				uint32_t base_type = idata.flags & InstanceData::FLAG_BASE_TYPE_MASK;
				if (base_type == RSE::INSTANCE_LIGHT) {
					cull_result.lights.push_back(idata.instance);
					cull_result.light_instances.push_back(RID::from_uint64(idata.instance_data_rid));
					if (cull_data.shadow_atlas.is_valid() && RSG::light_storage->light_has_shadow(idata.base_rid)) {
						cull_result.visible_lights.push_back(RID::from_uint64(idata.instance_data_rid));
					}

				} else if (base_type == RSE::INSTANCE_DECAL) {
					cull_result.decals.push_back(RID::from_uint64(idata.instance_data_rid));

				} else if (base_type == RSE::INSTANCE_FOG_VOLUME) {
					cull_result.fog_volumes.push_back(RID::from_uint64(idata.instance_data_rid));
				} else if (base_type == RSE::INSTANCE_VISIBLITY_NOTIFIER) {
					cull_result.notifiers.push_back(idata.visibility_notifier);
				} else if (((1 << base_type) & RSE::INSTANCE_GEOMETRY_MASK) && !(idata.flags & InstanceData::FLAG_CAST_SHADOWS_ONLY)) {
					bool keep = true;

					if (idata.flags & InstanceData::FLAG_REDRAW_IF_VISIBLE) {
						cull_result.redraw = true;
					}

					if (base_type == RSE::INSTANCE_MESH) {
						mesh_visible = true;
					} else if (base_type == RSE::INSTANCE_PARTICLES) {
						//particles visible? process them
						if (RSG::particles_storage->particles_is_inactive(idata.base_rid)) {
							//but if nothing is going on, don't do it.
							keep = false;
						} else {
							cull_result.particles.push_back(idata.base_rid);
							//particles visible? request redraw
							cull_result.redraw = true;
						}
					}

					cull_result.geometry_updates.push_back(i);

					if (keep) {
						cull_result.geometry_instances.push_back(idata.instance_geometry);
					}
				}
			}

			for (uint32_t j = 0; j < cull_data.cull->shadow_count; j++) {
				for (uint32_t k = 0; k < cull_data.cull->shadows[j].cascade_count; k++) {
					const Cull::Shadow::Cascade &cascade = cull_data.cull->shadows[j].cascades[k];
					if (!cascade.refresh || (!cascade.full_coverage && !light_culler->cull_directional_light(cull_data.scenario->instance_aabbs[i], j, k))) {
						continue;
					}
					if (IN_FRUSTUM(cull_data.cull->shadows[j].cascades[k].frustum) && VIS_CHECK) {
						uint32_t base_type = idata.flags & InstanceData::FLAG_BASE_TYPE_MASK;

						const bool is_inactive_particle = (base_type == RSE::INSTANCE_PARTICLES) && RSG::particles_storage->particles_is_inactive(idata.base_rid);
						if (((1 << base_type) & RSE::INSTANCE_GEOMETRY_MASK) && idata.flags & InstanceData::FLAG_CAST_SHADOWS && (LAYER_CHECK & cull_data.cull->shadows[j].caster_mask) && !is_inactive_particle) {
							cull_result.directional_shadows[j].cascade_geometry_instances[k].push_back(idata.instance_geometry);
							mesh_visible = true;
						}
					}
				}
			}
		}

		if (idata.instance != nullptr && idata.instance->visible) {
			uint32_t base_type = idata.flags & InstanceData::FLAG_BASE_TYPE_MASK;
			if (base_type == RSE::INSTANCE_LIGHT) {
				cull_result.rt_light_instances.push_back(RID::from_uint64(idata.instance_data_rid));
			} else if (base_type == RSE::INSTANCE_DECAL && LAYER_CHECK && !HIDDEN_BY_VISIBILITY_CHECKS && VIS_CHECK) {
				cull_result.rt_decals.push_back(RID::from_uint64(idata.instance_data_rid));
			} else if (base_type == RSE::INSTANCE_MESH || base_type == RSE::INSTANCE_MULTIMESH) {
				const bool visible_receiver = (LAYER_CHECK != 0) && !(idata.flags & InstanceData::FLAG_CAST_SHADOWS_ONLY);
				const bool shadow_caster = (idata.flags & InstanceData::FLAG_CAST_SHADOWS) != 0;
				if (visible_receiver || shadow_caster) {
					cull_result.rt_visibility.push_back({ idata.instance_geometry, visible_receiver, shadow_caster, (idata.flags & InstanceData::FLAG_CAST_SHADOWS_ONLY) != 0 });
					cull_result.rt_geometry_instances.push_back(idata.instance_geometry);
					mesh_visible = true;
				}
			}
		}

#undef HIDDEN_BY_VISIBILITY_CHECKS
#undef LAYER_CHECK
#undef IN_FRUSTUM
#undef VIS_RANGE_CHECK
#undef VIS_PARENT_CHECK
#undef VIS_CHECK
#undef OCCLUSION_CULLED

		for (uint32_t j = 0; j < cull_data.cull->sdfgi.region_count; j++) {
			if (cull_data.scenario->instance_aabbs[i].in_aabb(cull_data.cull->sdfgi.region_aabb[j])) {
				uint32_t base_type = idata.flags & InstanceData::FLAG_BASE_TYPE_MASK;

				if (base_type == RSE::INSTANCE_LIGHT) {
					InstanceLightData *instance_light = (InstanceLightData *)idata.instance->base_data;
					if (instance_light->bake_mode == RSE::LIGHT_BAKE_STATIC && cull_data.cull->sdfgi.region_cascade[j] <= instance_light->max_sdfgi_cascade) {
						if (sdfgi_last_light_index != i || sdfgi_last_light_cascade != cull_data.cull->sdfgi.region_cascade[j]) {
							sdfgi_last_light_index = i;
							sdfgi_last_light_cascade = cull_data.cull->sdfgi.region_cascade[j];
							cull_result.sdfgi_cascade_lights[sdfgi_last_light_cascade].push_back(instance_light->instance);
						}
					}
				} else if ((1 << base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
					if ((idata.flags & InstanceData::FLAG_USES_BAKED_LIGHT) && (cull_data.visible_layers & idata.layer_mask)) {
						cull_result.sdfgi_region_geometry_instances[j].push_back(idata.instance_geometry);
						mesh_visible = true;
					}
				}
			}
		}

		if (idata.occlusion_timeout != cull_data.scenario->instance_data[i].occlusion_timeout || (idata.visibility_index >= 0 && visibility.viewport_state != cull_data.scenario->instance_visibility[idata.visibility_index].viewport_state)) {
			cull_result.state_changes.push_back({ i, idata.occlusion_timeout, visibility.viewport_state });
		}

		if (mesh_visible && cull_data.scenario->instance_data[i].flags & InstanceData::FLAG_USES_MESH_INSTANCE) {
			cull_result.mesh_instances.push_back(cull_data.scenario->instance_data[i].instance->mesh_instance);
		}
	}
}

void RendererSceneCull::_scene_particles_set_view_axis(RID p_particles, const Vector3 &p_axis, const Vector3 &p_up_axis) {
	RSG::particles_storage->particles_set_view_axis(p_particles, p_axis, p_up_axis);
}

void RendererSceneCull::_render_scene(RID p_camera, const RendererSceneRender::CameraData *p_camera_data, const Ref<RenderSceneBuffers> &p_render_buffers, RID p_environment, RID p_force_camera_attributes, RID p_compositor, uint32_t p_visible_layers, RID p_scenario, RID p_viewport, RID p_shadow_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, float p_window_output_max_value, bool p_using_shadows, RenderingServerTypes::RenderInfo *r_render_info) {
	Instance *render_reflection_probe = instance_owner.get_or_null(p_reflection_probe); //if null, not rendering to it

	// Prepare the light - camera volume culling system.
	light_culler->prepare_camera(p_camera_data->main_transform, p_camera_data->main_projection, p_camera_data->main_origin);

	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	Vector3 camera_position = p_camera_data->main_transform.origin;

	ERR_FAIL_COND(p_render_buffers.is_null());

	render_pass++;

	scene_render->set_scene_pass(render_pass);

	if (p_reflection_probe.is_null()) {
		//no rendering code here, this is only to set up what needs to be done, request regions, etc.
		scene_render->sdfgi_update(p_render_buffers, p_environment, camera_position); //update conditions for SDFGI (whether its used or not)
	}

	RENDER_TIMESTAMP("Update Visibility Dependencies");

	if (scenario->instance_visibility.get_bin_count() > 0) {
		if (!scenario->viewport_visibility_masks.has(p_viewport)) {
			scenario_add_viewport_visibility_mask(scenario->self, p_viewport);
		}

		VisibilityCullData visibility_cull_data;
		visibility_cull_data.scenario = scenario;
		visibility_cull_data.viewport_mask = scenario->viewport_visibility_masks[p_viewport];
		for (int axis = 0; axis < 3; axis++) {
			visibility_cull_data.camera_position[axis] = p_camera_data->main_origin[axis];
		}

		LocalVector<AABB> shadow_caster_boxes;

		for (int i = scenario->instance_visibility.get_bin_count() - 1; i > 0; i--) { // We skip bin 0
			visibility_cull_data.cull_offset = scenario->instance_visibility.get_bin_start(i);
			visibility_cull_data.cull_count = scenario->instance_visibility.get_bin_size(i);

			if (visibility_cull_data.cull_count == 0) {
				continue;
			}

			WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
			visibility_cull_data.results.resize(visibility_cull_data.cull_count);
			visibility_cull_data.job_count = MIN(uint32_t(MAX(1, pool->get_thread_count())), MAX(1u, visibility_cull_data.cull_count / thread_cull_threshold));
			WorkerThreadPool::GroupID group_task = pool->add_template_group_task(this, &RendererSceneCull::_visibility_cull_threaded, &visibility_cull_data, visibility_cull_data.job_count, -1, true, SNAME("VisibilityCullInstances"));
			pool->wait_for_group_task_completion(group_task);
			auto publish = [&](uint32_t) {
				for (uint32_t index = 0; index < visibility_cull_data.results.size(); index++) {
					const auto &result = visibility_cull_data.results[index];
					scenario->instance_visibility[visibility_cull_data.cull_offset + index] = result.visibility;
					InstanceData &idata = scenario->instance_data[result.visibility.array_index];
					idata.flags = result.flags;
					if (result.reset_motion) {
						idata.instance_geometry->reset_motion_vectors();
					}
					if (result.shadow_caster_changed && result.visibility.instance) {
						shadow_caster_boxes.push_back(result.visibility.instance->transformed_aabb);
					}
				}
			};
			WorkerThreadPool::GroupID publish_job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
				GodotProfileZone("VisibilityCullPublish");
				(*static_cast<decltype(publish) *>(p_data))(p_index);
			},
					&publish, 1, 1, true, SNAME("VisibilityCullPublish"));
			pool->wait_for_group_task_completion(publish_job);
		}

		for (const AABB &box : shadow_caster_boxes) {
			scenario->shadow_caster_dirty(box);
		}
	}

	RENDER_TIMESTAMP("Cull 3D Scene");

	//rasterizer->set_camera(p_camera_data->main_transform, p_camera_data.main_projection, p_camera_data.is_orthogonal);

	/* STEP 2 - CULL */

	Vector<Plane> planes = p_camera_data->main_projection.get_projection_planes(Transform3D(p_camera_data->main_transform.basis, Vector3()));
	cull.frustum = Frustum(planes, p_camera_data->main_origin);

	Vector<RID> directional_lights;
	// directional lights
	{
		cull.shadow_count = 0;

		Vector<Instance *> lights_with_shadow;

		for (Instance *E : scenario->directional_lights) {
			if (!E->visible || !(E->layer_mask & p_visible_layers)) {
				continue;
			}

			if (directional_lights.size() >= RendererSceneRender::MAX_DIRECTIONAL_LIGHTS) {
				break;
			}

			InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);

			//check shadow..

			if (light) {
				if (p_using_shadows && p_shadow_atlas.is_valid() && RSG::light_storage->light_has_shadow(E->base) && !(RSG::light_storage->light_get_type(E->base) == RSE::LIGHT_DIRECTIONAL && RSG::light_storage->light_directional_get_sky_mode(E->base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY)) {
					lights_with_shadow.push_back(E);
				}
				//add to list
				directional_lights.push_back(light->instance);
			}
		}

		RSG::light_storage->set_directional_shadow_count(lights_with_shadow.size());
		const bool cache_shadows = scene_render == RendererSceneRenderImplementation::RenderForwardClustered::get_singleton();
		if (cache_shadows) {
			Vector<RID> layout;
			for (Instance *light_instance : lights_with_shadow) {
				layout.push_back(static_cast<InstanceLightData *>(light_instance->base_data)->instance);
			}
			if (layout != directional_shadow_layout) {
				directional_shadow_layout = layout;
				directional_shadow_layout_generation++;
			}
		}

		for (int i = 0; i < lights_with_shadow.size(); i++) {
			if (cache_shadows) {
				using Cache = InstanceLightData::DirectionalShadowCache;
				Instance *light_instance = lights_with_shadow[i];
				auto &cache = static_cast<InstanceLightData *>(light_instance->base_data)->directional_shadow_cache;
				const uint64_t generation = RendererRD::LightStorage::get_singleton()->directional_shadow_get_generation();
				const uint64_t version = RSG::light_storage->light_get_version(light_instance->base);
				const float size = RSG::light_storage->light_get_param(light_instance->base, RSE::LIGHT_PARAM_SIZE);
				uint32_t force = 0;
				if (cache.atlas_generation != generation) {
					force |= 1 << Cache::ATLAS;
				}
				if (cache.layout_generation != directional_shadow_layout_generation) {
					force |= 1 << Cache::LAYOUT;
				}
				if (cache.scenario != p_scenario || cache.camera != p_camera || cache.viewport != p_viewport || cache.render_buffers != p_render_buffers->get_instance_id() || cache.projection != p_camera_data->main_projection || cache.layers != p_visible_layers) {
					force |= 1 << Cache::CAMERA;
				}
				if (cache.light_version != version || cache.light_size != size) {
					force |= 1 << Cache::PARAMETERS;
				}
				for (auto &cascade : cache.cascades) {
					cascade.force |= force;
					if (force) {
						cascade.valid = false;
					}
				}
				cache.atlas_generation = generation;
				cache.layout_generation = directional_shadow_layout_generation;
				cache.light_version = version;
				cache.light_size = size;
				cache.scenario = p_scenario;
				cache.camera = p_camera;
				cache.viewport = p_viewport;
				cache.render_buffers = p_render_buffers->get_instance_id();
				cache.projection = p_camera_data->main_projection;
				cache.layers = p_visible_layers;
			}
			_light_instance_setup_directional_shadow(i, lights_with_shadow[i], p_camera_data->main_transform, p_camera_data->main_projection, p_camera_data->is_orthogonal, p_camera_data->vaspect, p_camera_data->main_origin, cache_shadows);
		}
	}

	{ //sdfgi
		cull.sdfgi.region_count = 0;

		if (p_reflection_probe.is_null()) {
			cull.sdfgi.cascade_light_count = 0;

			uint32_t prev_cascade = 0xFFFFFFFF;
			uint32_t pending_region_count = scene_render->sdfgi_get_pending_region_count(p_render_buffers);

			for (uint32_t i = 0; i < pending_region_count; i++) {
				cull.sdfgi.region_aabb[i] = scene_render->sdfgi_get_pending_region_bounds(p_render_buffers, i);
				uint32_t region_cascade = scene_render->sdfgi_get_pending_region_cascade(p_render_buffers, i);
				cull.sdfgi.region_cascade[i] = region_cascade;

				if (region_cascade != prev_cascade) {
					cull.sdfgi.cascade_light_index[cull.sdfgi.cascade_light_count] = region_cascade;
					cull.sdfgi.cascade_light_count++;
					prev_cascade = region_cascade;
				}
			}

			cull.sdfgi.region_count = pending_region_count;
		}
	}

	scene_cull_result.clear();

	{
		CullData cull_data;

		//prepare for eventual thread usage
		cull_data.cull = &cull;
		cull_data.scenario = scenario;
		cull_data.shadow_atlas = p_shadow_atlas;
		cull_data.cam_transform = p_camera_data->main_transform;
		for (int axis = 0; axis < 3; axis++) {
			cull_data.camera_origin[axis] = p_camera_data->main_origin[axis];
		}
		cull_data.visible_layers = p_visible_layers;
		cull_data.render_reflection_probe = render_reflection_probe;
		cull_data.occlusion_buffer = RendererSceneOcclusionCull::get_singleton()->buffer_get_ptr(p_viewport);
		cull_data.camera_matrix = &p_camera_data->main_projection;
		cull_data.visibility_viewport_mask = scenario->viewport_visibility_masks.has(p_viewport) ? scenario->viewport_visibility_masks[p_viewport] : 0;
#ifdef DEBUG_CULL_TIME
		uint64_t time_from = OS::get_singleton()->get_ticks_usec();
#endif

		for (InstanceCullResult &thread : scene_cull_result_threads) {
			thread.clear();
		}
		WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		const uint64_t frame = RSG::rasterizer->get_frame_number();
		cull_data.profile = RSG::utilities->capturing_timestamps && frame % 120 == 0;
		const uint64_t coordinator = cull_data.profile ? Thread::get_caller_id() : 0;
		const uint64_t queued = cull_data.profile ? OS::get_singleton()->get_ticks_usec() : 0;
		WorkerThreadPool::GroupID group_task = pool->add_template_group_task(this, &RendererSceneCull::_scene_cull_threaded, &cull_data, scene_cull_result_threads.size(), -1, true, SNAME("RenderCullInstances"));
		pool->wait_for_group_task_completion(group_task);
		if (cull_data.profile) {
			const uint64_t joined = OS::get_singleton()->get_ticks_usec();
			String rows;
			for (uint32_t index = 0; index < scene_cull_result_threads.size(); index++) {
				const auto &result = scene_cull_result_threads[index];
				const uint32_t total = scenario->conventional_instances.size();
				const uint32_t count = scene_cull_result_threads.size();
				rows += vformat("RenderPrep stage=RenderCullInstances frame=%d chunk=%d coordinator=%d queued_usec=%d joined_usec=%d worker=%d begin_usec=%d end_usec=%d work=%d conventional=%d micro_geometry=%d", frame, index, coordinator, queued, joined, result.worker, result.begin_usec, result.end_usec, (index + 1) * total / count - index * total / count, total, scenario->micro_geometry_instances.size()) + "\n";
			}
			print_line(rows);
		}
		auto merge = [&](uint32_t) {
			for (InstanceCullResult &thread : scene_cull_result_threads) {
				scene_cull_result.append_from(thread);
				_scene_cull_geometry_updates(cull_data, thread);
			}
		};
		WorkerThreadPool::GroupID merge_job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
			GodotProfileZone("RenderCullMerge");
			(*static_cast<decltype(merge) *>(p_data))(p_index);
		},
				&merge, 1, 1, true, SNAME("RenderCullMerge"));
		pool->wait_for_group_task_completion(merge_job);
		for (const InstanceCullResult &thread : scene_cull_result_threads) {
			for (RID light : thread.visible_lights) {
				RSG::light_storage->light_instance_mark_visible(light);
			}
			for (InstanceVisibilityNotifierData *notifier : thread.notifiers) {
				if (!notifier->list_element.in_list()) {
					visible_notifier_list.add(&notifier->list_element);
					notifier->just_visible = true;
				}
				notifier->visible_in_frame = RSG::rasterizer->get_frame_number();
			}
			for (RID particles : thread.particles) {
				RSG::particles_storage->particles_request_process(particles);
				_scene_particles_set_view_axis(particles, -cull_data.cam_transform.basis.get_column(2).normalized(), cull_data.cam_transform.basis.get_column(1).normalized());
			}
			if (thread.redraw) {
				RenderingServerDefault::redraw_request();
			}
		}

#ifdef DEBUG_CULL_TIME
		static float time_avg = 0;
		static uint32_t time_count = 0;
		time_avg += double(OS::get_singleton()->get_ticks_usec() - time_from) / 1000.0;
		time_count++;
		print_line("time taken: " + rtos(time_avg / time_count));
#endif

		if (scene_cull_result.mesh_instances.size()) {
			for (uint64_t i = 0; i < scene_cull_result.mesh_instances.size(); i++) {
				RSG::mesh_storage->mesh_instance_check_for_update(scene_cull_result.mesh_instances[i]);
			}
			RSG::mesh_storage->update_mesh_instances();
		}
	}

	//render shadows

	max_shadows_used = 0;

	if (p_using_shadows) { //setup shadow maps

		// Directional Shadows

		for (uint32_t i = 0; i < cull.shadow_count; i++) {
			for (uint32_t j = 0; j < cull.shadows[i].cascade_count; j++) {
				const Cull::Shadow::Cascade &c = cull.shadows[i].cascades[j];
				if (!c.refresh || max_shadows_used == MAX_UPDATE_SHADOWS) {
					continue;
				}
				RSG::light_storage->light_instance_set_shadow_transform(cull.shadows[i].light_instance, c.projection, c.transform, c.zfar, c.split, j, c.shadow_texel_size, c.bias_scale, c.range_begin, c.uv_scale, c.origin);
				render_shadow_data[max_shadows_used].light = cull.shadows[i].light_instance;
				render_shadow_data[max_shadows_used].pass = j;
				render_shadow_data[max_shadows_used].cull_planes = cull.shadows[i].cascades[j].frustum.planes;
				for (int axis = 0; axis < 3; axis++) {
					render_shadow_data[max_shadows_used].cull_origin[axis] = cull.shadows[i].cascades[j].frustum.origin[axis];
				}
				if (!c.full_coverage) {
					light_culler->append_caster_planes(render_shadow_data[max_shadows_used].cull_planes, i, j, render_shadow_data[max_shadows_used].cull_origin);
				}
				render_shadow_data[max_shadows_used].instances.merge_unordered(scene_cull_result.directional_shadows[i].cascade_geometry_instances[j]);
				max_shadows_used++;
				if (cull.shadows[i].light_data) {
					auto &cached = cull.shadows[i].light_data->directional_shadow_cache.cascades[j];
					cached.valid = true;
					cached.frame = RSG::rasterizer->get_frame_number();
					cached.basis = c.transform.basis;
					cached.camera_basis = c.camera_basis;
					for (int axis = 0; axis < 3; axis++) {
						cached.origin[axis] = c.frustum.origin[axis];
					}
					cached.minimum = c.coverage_minimum;
					cached.maximum = c.coverage_maximum;
					cached.caster_generation = c.caster_generation;
					cached.refreshed++;
					for (uint32_t reason = 0; reason < InstanceLightData::DirectionalShadowCache::FORCE_REASON_COUNT; reason++) {
						cached.forced[reason] += (cached.force >> reason) & 1;
					}
					cached.force = 0;
				}
			}
			const uint64_t frame = RSG::rasterizer->get_frame_number();
			if (cull.shadows[i].light_data && RSG::utilities->capturing_timestamps && frame % 120 == 0) {
				for (uint32_t j = 0; j < cull.shadows[i].cascade_count; j++) {
					auto &cached = cull.shadows[i].light_data->directional_shadow_cache.cascades[j];
					print_line(vformat("ShadowCadence frame=%d light=%d cascade=%d period=%d refreshed=%d reused=%d age=%d max_age=%d casters=%d first=%d atlas=%d layout=%d camera=%d parameters=%d sun=%d coverage=%d", frame, i, j, 1u << j, cached.refreshed, cached.reused, frame - cached.frame, cached.max_age, cached.caster_generation, cached.forced[0], cached.forced[1], cached.forced[2], cached.forced[3], cached.forced[4], cached.forced[5], cached.forced[6]));
					cached.refreshed = 0;
					cached.reused = 0;
					cached.max_age = 0;
					for (uint32_t &count : cached.forced) {
						count = 0;
					}
				}
			}
		}

		// Positional Shadows
		for (uint32_t i = 0; i < (uint32_t)scene_cull_result.lights.size(); i++) {
			Instance *ins = scene_cull_result.lights[i];

			if (!p_shadow_atlas.is_valid()) {
				continue;
			}

			InstanceLightData *light = static_cast<InstanceLightData *>(ins->base_data);

			if (!RSG::light_storage->light_instance_is_shadow_visible_at_position(light->instance, camera_position)) {
				continue;
			}

			float coverage = 0.f;

			{ //compute coverage

				Transform3D cam_xf = p_camera_data->main_transform;
				float zn = p_camera_data->main_projection.get_z_near();
				Plane p(-cam_xf.basis.get_column(2), cam_xf.origin + cam_xf.basis.get_column(2) * -zn); //camera near plane

				// near plane half width and height
				Vector2 vp_half_extents = p_camera_data->main_projection.get_viewport_half_extents();

				switch (RSG::light_storage->light_get_type(ins->base)) {
					case RSE::LIGHT_OMNI: {
						float radius = RSG::light_storage->light_get_param(ins->base, RSE::LIGHT_PARAM_RANGE);

						//get two points parallel to near plane
						Vector3 points[2] = {
							ins->transform.origin,
							ins->transform.origin + cam_xf.basis.get_column(0) * radius
						};

						if (!p_camera_data->is_orthogonal) {
							//if using perspetive, map them to near plane
							for (int j = 0; j < 2; j++) {
								if (p.distance_to(points[j]) < 0) {
									points[j].z = -zn; //small hack to keep size constant when hitting the screen
								}

								p.intersects_segment(cam_xf.origin, points[j], &points[j]); //map to plane
							}
						}

						float screen_diameter = points[0].distance_to(points[1]) * 2;
						coverage = screen_diameter / (vp_half_extents.x + vp_half_extents.y);
					} break;
					case RSE::LIGHT_SPOT: {
						float radius = RSG::light_storage->light_get_param(ins->base, RSE::LIGHT_PARAM_RANGE);
						float angle = RSG::light_storage->light_get_param(ins->base, RSE::LIGHT_PARAM_SPOT_ANGLE);

						float w = radius * Math::sin(Math::deg_to_rad(angle));
						float d = radius * Math::cos(Math::deg_to_rad(angle));

						Vector3 base = ins->transform.origin - ins->transform.basis.get_column(2).normalized() * d;

						Vector3 points[2] = {
							base,
							base + cam_xf.basis.get_column(0) * w
						};

						if (!p_camera_data->is_orthogonal) {
							//if using perspetive, map them to near plane
							for (int j = 0; j < 2; j++) {
								if (p.distance_to(points[j]) < 0) {
									points[j].z = -zn; //small hack to keep size constant when hitting the screen
								}

								p.intersects_segment(cam_xf.origin, points[j], &points[j]); //map to plane
							}
						}

						float screen_diameter = points[0].distance_to(points[1]) * 2;
						coverage = screen_diameter / (vp_half_extents.x + vp_half_extents.y);

					} break;
					case RSE::LIGHT_AREA: {
						float diagonal = RSG::light_storage->light_area_get_size(ins->base).length();
						float radius = RSG::light_storage->light_get_param(ins->base, RSE::LIGHT_PARAM_RANGE) + diagonal;

						//get two points parallel to near plane
						Vector3 points[2] = {
							ins->transform.origin,
							ins->transform.origin + cam_xf.basis.get_column(0) * radius
						};

						if (!p_camera_data->is_orthogonal) {
							//if using perspetive, map them to near plane
							for (int j = 0; j < 2; j++) {
								if (p.distance_to(points[j]) < 0) {
									points[j].z = -zn; //small hack to keep size constant when hitting the screen
								}

								p.intersects_segment(cam_xf.origin, points[j], &points[j]); //map to plane
							}
						}

						float screen_diameter = points[0].distance_to(points[1]) * 2;
						coverage = screen_diameter / (vp_half_extents.x + vp_half_extents.y);
					} break;
					default: {
						ERR_PRINT("Invalid Light Type");
					}
				}
			}

			// We can detect whether multiple cameras are hitting this light, whether or not the shadow is dirty,
			// so that we can turn off tighter caster culling.
			light->detect_light_intersects_multiple_cameras(RSG::frame.frames_drawn);

			if (light->is_shadow_dirty()) {
				// Dirty shadows have no need to be drawn if
				// the light volume doesn't intersect the camera frustum.

				// Returns false if the entire light can be culled.
				bool allow_redraw = light_culler->prepare_regular_light(*ins);

				// Directional lights aren't handled here, _light_instance_update_shadow is called from elsewhere.
				// Checking for this in case this changes, as this is assumed.
				DEV_CHECK_ONCE(RSG::light_storage->light_get_type(ins->base) != RSE::LIGHT_DIRECTIONAL);

				// Tighter caster culling to the camera frustum should work correctly with multiple viewports + cameras.
				// The first camera will cull tightly, but if the light is present on more than 1 camera, the second will
				// do a full render, and mark the light as non-dirty.
				// There is however a cost to tighter shadow culling in this situation (2 shadow updates in 1 frame),
				// so we should detect this and switch off tighter caster culling automatically.
				// This is done in the logic for `decrement_shadow_dirty()`.
				if (allow_redraw) {
					light->last_version++;
					light->decrement_shadow_dirty();
				}
			}

			bool redraw = RSG::light_storage->shadow_atlas_update_light(p_shadow_atlas, light->instance, coverage, light->last_version);

			if (redraw && max_shadows_used < MAX_UPDATE_SHADOWS) {
				//must redraw!
				RENDER_TIMESTAMP("> Render Light3D " + itos(i));
				if (_light_instance_update_shadow(ins, p_camera_data->main_transform, p_camera_data->main_projection, p_camera_data->is_orthogonal, p_camera_data->vaspect, p_shadow_atlas, scenario, p_screen_mesh_lod_threshold, p_visible_layers)) {
					light->make_shadow_dirty();
				}
				RENDER_TIMESTAMP("< Render Light3D " + itos(i));
			} else {
				if (redraw) {
					light->make_shadow_dirty();
				}
			}
		}
	}

	//render SDFGI

	{
		// Q: Should this whole block be skipped if we're rendering our reflection probe?

		sdfgi_update_data.update_static = false;

		if (cull.sdfgi.region_count > 0) {
			//update regions
			for (uint32_t i = 0; i < cull.sdfgi.region_count; i++) {
				render_sdfgi_data[i].instances.merge_unordered(scene_cull_result.sdfgi_region_geometry_instances[i]);
				render_sdfgi_data[i].region = i;
			}
			//check if static lights were culled
			bool static_lights_culled = false;
			for (uint32_t i = 0; i < cull.sdfgi.cascade_light_count; i++) {
				if (scene_cull_result.sdfgi_cascade_lights[i].size()) {
					static_lights_culled = true;
					break;
				}
			}

			if (static_lights_culled) {
				sdfgi_update_data.static_cascade_count = cull.sdfgi.cascade_light_count;
				sdfgi_update_data.static_cascade_indices = cull.sdfgi.cascade_light_index;
				sdfgi_update_data.static_positional_lights = scene_cull_result.sdfgi_cascade_lights;
				sdfgi_update_data.update_static = true;
			}
		}

		if (p_reflection_probe.is_null()) {
			sdfgi_update_data.directional_lights = &directional_lights;
			sdfgi_update_data.positional_light_instances = scenario->dynamic_lights.ptr();
			sdfgi_update_data.positional_light_count = scenario->dynamic_lights.size();
		}
	}

	//append the directional lights to the lights culled
	for (int i = 0; i < directional_lights.size(); i++) {
		scene_cull_result.light_instances.push_back(directional_lights[i]);
	}
	for (Instance *E : scenario->directional_lights) {
		if (!E->visible || !(E->layer_mask & p_visible_layers)) {
			continue;
		}
		InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);
		if (light) {
			scene_cull_result.rt_light_instances.push_back(light->instance);
		}
	}

	RID camera_attributes;
	if (p_force_camera_attributes.is_valid()) {
		camera_attributes = p_force_camera_attributes;
	} else {
		camera_attributes = scenario->camera_attributes;
	}

	/* PROCESS GEOMETRY AND DRAW SCENE */

	RID occluders_tex;
	RID prev_camera = p_camera;
	const RendererSceneRender::CameraData *prev_camera_data = p_camera_data;
	if (p_viewport.is_valid()) {
		occluders_tex = RSG::viewport->viewport_get_occluder_debug_texture(p_viewport);
		prev_camera = RSG::viewport->viewport_get_prev_camera(p_viewport);
		prev_camera_data = RSG::viewport->viewport_get_prev_camera_data(p_viewport);
	}

	RENDER_TIMESTAMP("Render 3D Scene");
	scene_render->render_scene(p_render_buffers, p_scenario, p_camera_data, prev_camera_data, p_camera, prev_camera, scene_cull_result.geometry_instances, scene_cull_result.light_instances, scene_cull_result.reflections, scene_cull_result.voxel_gi_instances, scene_cull_result.decals, scene_cull_result.lightmaps, scene_cull_result.fog_volumes, p_environment, camera_attributes, p_compositor, p_shadow_atlas, occluders_tex, p_reflection_probe.is_valid() ? RID() : scenario->reflection_atlas, p_reflection_probe, p_reflection_probe_pass, p_screen_mesh_lod_threshold, render_shadow_data, max_shadows_used, render_sdfgi_data, cull.sdfgi.region_count, p_window_output_max_value, &sdfgi_update_data, r_render_info, &scene_cull_result.rt_geometry_instances, &scene_cull_result.rt_light_instances, &scene_cull_result.rt_decals);

	if (p_viewport.is_valid()) {
		RSG::viewport->viewport_set_prev_camera_data(p_viewport, p_camera, p_camera_data);
	}

	for (uint32_t i = 0; i < max_shadows_used; i++) {
		render_shadow_data[i].instances.clear();
	}
	max_shadows_used = 0;

	for (uint32_t i = 0; i < cull.sdfgi.region_count; i++) {
		render_sdfgi_data[i].instances.clear();
	}
}

RID RendererSceneCull::_render_get_environment(RID p_camera, RID p_scenario) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	if (camera && scene_render->is_environment(camera->env)) {
		return camera->env;
	}

	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	if (!scenario) {
		return RID();
	}
	if (scene_render->is_environment(scenario->environment)) {
		return scenario->environment;
	}

	if (scene_render->is_environment(scenario->fallback_environment)) {
		return scenario->fallback_environment;
	}

	return RID();
}

RID RendererSceneCull::_render_get_compositor(RID p_camera, RID p_scenario) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	if (camera && scene_render->is_compositor(camera->compositor)) {
		return camera->compositor;
	}

	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	if (scenario && scene_render->is_compositor(scenario->compositor)) {
		return scenario->compositor;
	}

	return RID();
}

void RendererSceneCull::render_empty_scene(const Ref<RenderSceneBuffers> &p_render_buffers, RID p_scenario, RID p_shadow_atlas, float p_window_output_max_value) {
#ifndef _3D_DISABLED
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);

	RID environment;
	if (scenario->environment.is_valid()) {
		environment = scenario->environment;
	} else {
		environment = scenario->fallback_environment;
	}
	RID compositor = scenario->compositor;
	RENDER_TIMESTAMP("Render Empty 3D Scene");

	RendererSceneRender::CameraData camera_data;
	camera_data.set_camera(Transform3D(), Projection(), true, false);

	scene_render->render_scene(p_render_buffers, RID(), &camera_data, &camera_data, RID(), RID(), PagedArray<RenderGeometryInstance *>(), PagedArray<RID>(), PagedArray<RID>(), PagedArray<RID>(), PagedArray<RID>(), PagedArray<RID>(), PagedArray<RID>(), environment, RID(), compositor, p_shadow_atlas, RID(), scenario->reflection_atlas, RID(), 0, 0, nullptr, 0, nullptr, 0, p_window_output_max_value, nullptr);
#endif
}

void RendererSceneCull::render_probes() {
}

bool RendererSceneCull::_update_particle_collider_sampling(Instance *p_instance) const {
	InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(p_instance->base_data);
	double origin[3] = { p_instance->origin[0], p_instance->origin[1], p_instance->origin[2] };
	Camera *camera = p_instance->scenario ? camera_owner.get_or_null(p_instance->scenario->sampling_camera) : nullptr;
	if (camera && collision->heightfield_follow_camera && RSG::particles_storage->particles_collision_is_heightfield(p_instance->base)) {
		for (int axis : { 0, 2 }) {
			const Vector3 direction = p_instance->transform.basis.get_column(axis).normalized();
			const double step = p_instance->transform.basis.get_column(axis).length();
			if (step <= CMP_EPSILON) {
				continue;
			}
			double distance = 0.0;
			for (int coordinate = 0; coordinate < 3; coordinate++) {
				distance += double(direction[coordinate]) * (camera->origin[coordinate] - origin[coordinate]);
			}
			const double offset = (distance < 0.0 ? -1.0 : 1.0) * MAX(0.0, Math::ceil(Math::abs(distance) / step) - 1.0) * step;
			for (int coordinate = 0; coordinate < 3; coordinate++) {
				origin[coordinate] += double(direction[coordinate]) * offset;
			}
		}
	}
	bool changed = false;
	for (int axis = 0; axis < 3; axis++) {
		changed |= collision->sampling_origin[axis] != origin[axis];
		collision->sampling_origin[axis] = origin[axis];
	}
	return changed;
}

void RendererSceneCull::render_particle_colliders() {
	while (heightfield_particle_colliders_update_list.begin()) {
		Instance *hfpc = *heightfield_particle_colliders_update_list.begin();

		if (hfpc->scenario && hfpc->visible && hfpc->base_type == RSE::INSTANCE_PARTICLES_COLLISION && RSG::particles_storage->particles_collision_is_heightfield(hfpc->base)) {
			//update heightfield
			instance_cull_result.clear();
			scene_cull_result.geometry_instances.clear();

			struct CullAABB {
				PagedArray<Instance *> *result;
				uint32_t heightfield_mask;
				_FORCE_INLINE_ bool operator()(void *p_data) {
					Instance *p_instance = (Instance *)p_data;
					if (p_instance->layer_mask & heightfield_mask) {
						result->push_back(p_instance);
					}
					return false;
				}
			};

			CullAABB cull_aabb;
			cull_aabb.result = &instance_cull_result;
			cull_aabb.heightfield_mask = RSG::particles_storage->particles_collision_get_height_field_mask(hfpc->base);
			hfpc->scenario->indexers[Scenario::INDEXER_GEOMETRY].aabb_query(hfpc->transformed_aabb, cull_aabb);
			hfpc->scenario->indexers[Scenario::INDEXER_VOLUMES].aabb_query(hfpc->transformed_aabb, cull_aabb);

			for (int i = 0; i < (int)instance_cull_result.size(); i++) {
				Instance *instance = instance_cull_result[i];
				if (!instance || !((1 << instance->base_type) & (RSE::INSTANCE_GEOMETRY_MASK & (~(1 << RSE::INSTANCE_PARTICLES))))) { //all but particles to avoid self collision
					continue;
				}
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
				ERR_FAIL_NULL(geom->geometry_instance);
				scene_cull_result.geometry_instances.push_back(geom->geometry_instance);
			}

			scene_render->render_particle_collider_heightfield(hfpc->base, hfpc->transform, scene_cull_result.geometry_instances, static_cast<InstanceParticlesCollisionData *>(hfpc->base_data)->sampling_origin, hfpc->scenario->self, cull_aabb.heightfield_mask);
		}
		heightfield_particle_colliders_update_list.remove(heightfield_particle_colliders_update_list.begin());
	}
}

void RendererSceneCull::_update_dirty_instance(Instance *p_instance) const {
	if (p_instance->update_aabb) {
		_update_instance_aabb(p_instance);
	}

	if (p_instance->update_dependencies) {
		p_instance->dependency_tracker.update_begin();

		if (p_instance->base.is_valid()) {
			RSG::utilities->base_update_dependency(p_instance->base, &p_instance->dependency_tracker);
		}

		if (p_instance->material_override.is_valid()) {
			RSG::material_storage->material_update_dependency(p_instance->material_override, &p_instance->dependency_tracker);
		}

		if (p_instance->material_overlay.is_valid()) {
			RSG::material_storage->material_update_dependency(p_instance->material_overlay, &p_instance->dependency_tracker);
		}

		if (p_instance->base_type == RSE::INSTANCE_MESH) {
			//remove materials no longer used and un-own them

			int new_mat_count = RSG::mesh_storage->mesh_get_surface_count(p_instance->base);
			p_instance->materials.resize(new_mat_count);

			_instance_update_mesh_instance(p_instance);
		}

		if (p_instance->base_type == RSE::INSTANCE_PARTICLES) {
			// update the process material dependency

			RID particle_material = RSG::particles_storage->particles_get_process_material(p_instance->base);
			if (particle_material.is_valid()) {
				RSG::material_storage->material_update_dependency(particle_material, &p_instance->dependency_tracker);
			}
		}

		if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
			InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);

			bool can_cast_shadows = true;
			bool is_animated = false;

			p_instance->instance_uniforms.materials_start();

			if (p_instance->cast_shadows == RSE::SHADOW_CASTING_SETTING_OFF) {
				can_cast_shadows = false;
			}

			if (p_instance->material_override.is_valid()) {
				if (!RSG::material_storage->material_casts_shadows(p_instance->material_override)) {
					can_cast_shadows = false;
				}
				is_animated = RSG::material_storage->material_is_animated(p_instance->material_override);
				p_instance->instance_uniforms.materials_append(p_instance->material_override);
			} else {
				if (p_instance->base_type == RSE::INSTANCE_MESH) {
					RID mesh = p_instance->base;

					if (mesh.is_valid()) {
						bool cast_shadows = false;

						for (int i = 0; i < p_instance->materials.size(); i++) {
							RID mat = p_instance->materials[i].is_valid() ? p_instance->materials[i] : RSG::mesh_storage->mesh_surface_get_material(mesh, i);

							if (!mat.is_valid()) {
								cast_shadows = true;
							} else {
								if (RSG::material_storage->material_casts_shadows(mat)) {
									cast_shadows = true;
								}

								if (RSG::material_storage->material_is_animated(mat)) {
									is_animated = true;
								}

								p_instance->instance_uniforms.materials_append(mat);

								RSG::material_storage->material_update_dependency(mat, &p_instance->dependency_tracker);
							}
						}

						if (!cast_shadows) {
							can_cast_shadows = false;
						}
					}

				} else if (p_instance->base_type == RSE::INSTANCE_MULTIMESH) {
					RID mesh = RSG::mesh_storage->multimesh_get_mesh(p_instance->base);
					if (mesh.is_valid()) {
						bool cast_shadows = false;

						int sc = RSG::mesh_storage->mesh_get_surface_count(mesh);
						for (int i = 0; i < sc; i++) {
							RID mat = RSG::mesh_storage->mesh_surface_get_material(mesh, i);

							if (!mat.is_valid()) {
								cast_shadows = true;

							} else {
								if (RSG::material_storage->material_casts_shadows(mat)) {
									cast_shadows = true;
								}
								if (RSG::material_storage->material_is_animated(mat)) {
									is_animated = true;
								}

								p_instance->instance_uniforms.materials_append(mat);

								RSG::material_storage->material_update_dependency(mat, &p_instance->dependency_tracker);
							}
						}

						if (!cast_shadows) {
							can_cast_shadows = false;
						}

						RSG::utilities->base_update_dependency(mesh, &p_instance->dependency_tracker);
					}
				} else if (p_instance->base_type == RSE::INSTANCE_PARTICLES) {
					bool cast_shadows = false;

					int dp = RSG::particles_storage->particles_get_draw_passes(p_instance->base);

					for (int i = 0; i < dp; i++) {
						RID mesh = RSG::particles_storage->particles_get_draw_pass_mesh(p_instance->base, i);
						if (!mesh.is_valid()) {
							continue;
						}

						int sc = RSG::mesh_storage->mesh_get_surface_count(mesh);
						for (int j = 0; j < sc; j++) {
							RID mat = RSG::mesh_storage->mesh_surface_get_material(mesh, j);

							if (!mat.is_valid()) {
								cast_shadows = true;
							} else {
								if (RSG::material_storage->material_casts_shadows(mat)) {
									cast_shadows = true;
								}

								if (RSG::material_storage->material_is_animated(mat)) {
									is_animated = true;
								}

								p_instance->instance_uniforms.materials_append(mat);

								RSG::material_storage->material_update_dependency(mat, &p_instance->dependency_tracker);
							}
						}
					}

					if (!cast_shadows) {
						can_cast_shadows = false;
					}
				}
			}

			if (p_instance->material_overlay.is_valid()) {
				can_cast_shadows = can_cast_shadows && RSG::material_storage->material_casts_shadows(p_instance->material_overlay);
				is_animated = is_animated || RSG::material_storage->material_is_animated(p_instance->material_overlay);
				p_instance->instance_uniforms.materials_append(p_instance->material_overlay);
			}

			if (can_cast_shadows != geom->can_cast_shadows) {
				//ability to cast shadows change, let lights now
				for (const Instance *E : geom->lights) {
					InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);
					light->make_shadow_dirty();
				}

				geom->can_cast_shadows = can_cast_shadows;

				if (p_instance->scenario) {
					p_instance->scenario->shadow_caster_dirty(p_instance->transformed_aabb);
				}
			}

			geom->material_is_animated = is_animated;

			if (p_instance->instance_uniforms.materials_finish(p_instance->self)) {
				geom->geometry_instance->set_instance_shader_uniforms_offset(p_instance->instance_uniforms.location());
			}
		}

		if (p_instance->skeleton.is_valid()) {
			RSG::mesh_storage->skeleton_update_dependency(p_instance->skeleton, &p_instance->dependency_tracker);
		}

		p_instance->dependency_tracker.update_end();

		if ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) {
			InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
			ERR_FAIL_NULL(geom->geometry_instance);
			geom->geometry_instance->scene_data_changed();
		}
	}

	_instance_update_list.remove(&p_instance->update_item);

	_update_instance(p_instance);

	p_instance->teleported = false;
	p_instance->update_aabb = false;
	p_instance->update_dependencies = false;
}

void RendererSceneCull::update_dirty_instances() const {
	while (_instance_update_list.first()) {
		_update_dirty_instance(_instance_update_list.first()->self());
	}

	// Update dirty resources after dirty instances as instance updates may affect resources.
	RSG::utilities->update_dirty_resources();
}

void RendererSceneCull::update() {
	//optimize bvhs

	uint32_t rid_count = scenario_owner.get_rid_count();
	RID *rids = (RID *)alloca(sizeof(RID) * rid_count);
	scenario_owner.fill_owned_buffer(rids);
	for (uint32_t i = 0; i < rid_count; i++) {
		Scenario *s = scenario_owner.get_or_null(rids[i]);
		s->indexers[Scenario::INDEXER_GEOMETRY].optimize_incremental(indexer_update_iterations);
		s->indexers[Scenario::INDEXER_VOLUMES].optimize_incremental(indexer_update_iterations);
		s->indexers[Scenario::INDEXER_CONVENTIONAL_GEOMETRY].optimize_incremental(indexer_update_iterations);
	}
	for (Instance *collider : continuous_heightfield_particle_colliders) {
		if (_update_particle_collider_sampling(collider)) {
			_instance_queue_update(collider, true, false);
		}
		if (static_cast<InstanceParticlesCollisionData *>(collider->base_data)->heightfield_update_always) {
			heightfield_particle_colliders_update_list.insert(collider);
		}
	}
	scene_render->update();
	update_dirty_instances();
	render_particle_colliders();
}

bool RendererSceneCull::free(RID p_rid) {
	if (p_rid.is_null()) {
		return true;
	}

	if (scene_render->free(p_rid)) {
		return true;
	}

	if (camera_owner.owns(p_rid)) {
		camera_owner.free(p_rid);

	} else if (scenario_owner.owns(p_rid)) {
		Scenario *scenario = scenario_owner.get_or_null(p_rid);
		releasing_entity_batch = true;
		for (KeyValue<EntityId, NativeEntity> &entry : scenario->native_entities) {
			_release_native_entity(entry.value);
		}
		scenario->native_entities.clear();
		releasing_entity_batch = false;

		while (scenario->instances.first()) {
			_render_slot_move_scenario(scenario->instances.first()->self()->self, RID());
		}
		scenario->instance_aabbs.reset();
		scenario->instance_data.reset();
		scenario->instance_visibility.reset();

		RSG::light_storage->shadow_atlas_free(scenario->reflection_probe_shadow_atlas);
		RSG::light_storage->reflection_atlas_free(scenario->reflection_atlas);
		scenario_owner.free(p_rid);
		RendererSceneOcclusionCull::get_singleton()->remove_scenario(p_rid);

	} else if (RendererSceneOcclusionCull::get_singleton() && RendererSceneOcclusionCull::get_singleton()->is_occluder(p_rid)) {
		RendererSceneOcclusionCull::get_singleton()->free_occluder(p_rid);
	} else if (instance_owner.owns(p_rid)) {
		// delete the instance

		if (!releasing_entity_batch) {
			update_dirty_instances();
		}

		Instance *instance = instance_owner.get_or_null(p_rid);

		_remove_entity_references(instance);
		while (!instance->visibility_dependencies.is_empty()) {
			_render_slot_link_visibility((*instance->visibility_dependencies.begin())->self, RID());
		}
		_render_slot_link_visibility(p_rid, RID());
		_render_slot_link_lightmap(p_rid, RID(), Rect2(), 0);
		_render_slot_move_scenario(p_rid, RID());
		_render_slot_replace_base(p_rid, RID());

		instance->instance_uniforms.free(instance->self);
		_retire_entity_assets(instance->tool_assets);
		if (instance->update_item.in_list()) {
			_instance_update_list.remove(&instance->update_item);
		}
		if (!releasing_entity_batch) {
			update_dirty_instances();
		}

		instance_owner.free(p_rid);
	} else {
		return false;
	}

	return true;
}

TypedArray<Image> RendererSceneCull::bake_render_uv2(RID p_base, const TypedArray<RID> &p_material_overrides, const Size2i &p_image_size) {
	return scene_render->bake_render_uv2(p_base, p_material_overrides, p_image_size);
}

PackedByteArray RendererSceneCull::bake_render_area_light_atlas(const TypedArray<RID> &p_area_light_textures, const TypedArray<Rect2> &p_area_light_atlas_texture_rects, const Size2i &p_size, int p_mipmaps) {
	return scene_render->bake_render_area_light_atlas(p_area_light_textures, p_area_light_atlas_texture_rects, p_size, p_mipmaps);
}

void RendererSceneCull::update_visibility_notifiers() {
	SelfList<InstanceVisibilityNotifierData> *E = visible_notifier_list.first();
	while (E) {
		SelfList<InstanceVisibilityNotifierData> *N = E->next();

		InstanceVisibilityNotifierData *visibility_notifier = E->self();
		if (visibility_notifier->just_visible) {
			visibility_notifier->just_visible = false;

			RSG::utilities->visibility_notifier_call(visibility_notifier->base, true, RSG::threaded);
		} else {
			if (visibility_notifier->visible_in_frame != RSG::rasterizer->get_frame_number()) {
				visible_notifier_list.remove(E);

				RSG::utilities->visibility_notifier_call(visibility_notifier->base, false, RSG::threaded);
			}
		}

		E = N;
	}
}

/*******************************/
/* Passthrough to Scene Render */
/*******************************/

/* ENVIRONMENT API */

RendererSceneCull *RendererSceneCull::singleton = nullptr;

void RendererSceneCull::set_scene_render(RendererSceneRender *p_scene_render) {
	scene_render = p_scene_render;
	geometry_instance_pair_mask = scene_render->geometry_instance_get_pair_mask();
}

/* INTERPOLATION API */

void RendererSceneCull::update_interpolation_tick(bool p_process) {
	// MultiMesh: Update interpolation in storage.
	RSG::mesh_storage->update_interpolation_tick(p_process);
}

void RendererSceneCull::update_interpolation_frame(bool p_process) {
	// MultiMesh: Update interpolation in storage.
	RSG::mesh_storage->update_interpolation_frame(p_process);
}

void RendererSceneCull::set_physics_interpolation_enabled(bool p_enabled) {
	_interpolation_data.interpolation_enabled = p_enabled;
}

RendererSceneCull::RendererSceneCull() {
	render_pass = 1;
	singleton = this;

	instance_cull_result.set_page_pool(&instance_cull_page_pool);
	instance_shadow_cull_result.set_page_pool(&instance_cull_page_pool);

	for (uint32_t i = 0; i < MAX_UPDATE_SHADOWS; i++) {
		render_shadow_data[i].instances.set_page_pool(&geometry_instance_cull_page_pool);
	}
	for (uint32_t i = 0; i < SDFGI_MAX_CASCADES * SDFGI_MAX_REGIONS_PER_CASCADE; i++) {
		render_sdfgi_data[i].instances.set_page_pool(&geometry_instance_cull_page_pool);
	}

	scene_cull_result.init(&rid_cull_page_pool, &geometry_instance_cull_page_pool, &instance_cull_page_pool);
	scene_cull_result_threads.resize(MAX(1, WorkerThreadPool::get_singleton()->get_thread_count()));
	for (InstanceCullResult &thread : scene_cull_result_threads) {
		thread.init(&rid_cull_page_pool, &geometry_instance_cull_page_pool, &instance_cull_page_pool);
	}

	indexer_update_iterations = GLOBAL_GET("rendering/limits/spatial_indexer/update_iterations_per_frame");
	thread_cull_threshold = GLOBAL_GET("rendering/limits/spatial_indexer/threaded_cull_minimum_instances");
	thread_cull_threshold = MAX(thread_cull_threshold, (uint32_t)WorkerThreadPool::get_singleton()->get_thread_count()); //make sure there is at least one thread per CPU
	RendererSceneOcclusionCull::HZBuffer::occlusion_jitter_enabled = GLOBAL_GET("rendering/occlusion_culling/jitter_projection");

	dummy_occlusion_culling = memnew(RendererSceneOcclusionCull);

	light_culler = memnew(RenderingLightCuller);

	bool tighter_caster_culling = GLOBAL_DEF("rendering/lights_and_shadows/tighter_shadow_caster_culling", true);
	light_culler->set_caster_culling_active(tighter_caster_culling);
	light_culler->set_light_culling_active(tighter_caster_culling);
}

RendererSceneCull::~RendererSceneCull() {
	instance_cull_result.reset();
	instance_shadow_cull_result.reset();

	for (uint32_t i = 0; i < MAX_UPDATE_SHADOWS; i++) {
		render_shadow_data[i].instances.reset();
	}
	for (uint32_t i = 0; i < SDFGI_MAX_CASCADES * SDFGI_MAX_REGIONS_PER_CASCADE; i++) {
		render_sdfgi_data[i].instances.reset();
	}

	scene_cull_result.reset();
	for (InstanceCullResult &thread : scene_cull_result_threads) {
		thread.reset();
	}
	scene_cull_result_threads.clear();

	if (dummy_occlusion_culling) {
		memdelete(dummy_occlusion_culling);
	}

	if (light_culler) {
		memdelete(light_culler);
		light_culler = nullptr;
	}
}

void RendererSceneCull::_instance_micro_geometry_routing_changed(void *p_data, bool p_enabled) {
	singleton->_instance_update_cull_domain(static_cast<Instance *>(p_data));
}

void RendererSceneCull::_instance_update_cull_domain(Instance *p_instance, bool p_remove) const {
	bool micro_geometry = false;
	const bool geometry_instance = ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) && p_instance->base_data;
	if (!p_remove && geometry_instance) {
		auto *geometry = static_cast<InstanceGeometryData *>(p_instance->base_data)->geometry_instance;
		if (geometry) {
			const bool cpu_culling = p_instance->visibility_parent || p_instance->visibility_range_begin != 0 || p_instance->visibility_range_end != 0 || !p_instance->lightmap_sh.is_empty() || p_instance->ignore_all_culling || p_instance->ignore_occlusion_culling || p_instance->custom_aabb || p_instance->extra_margin != 0 || p_instance->redraw_if_visible;
			if (geometry->micro_geometry_cpu_culling != cpu_culling) {
				geometry->micro_geometry_cpu_culling = cpu_culling;
				geometry->_mark_instance_data_dirty();
			}
			micro_geometry = geometry->micro_geometry_raster_only && !cpu_culling;
		}
	}
	if (!p_instance->scenario || p_instance->array_index < 0) {
		return;
	}
	auto *scenario = p_instance->scenario;
	if (p_instance->conventional_indexer_id.is_valid() && (p_remove || micro_geometry)) {
		scenario->indexers[Scenario::INDEXER_CONVENTIONAL_GEOMETRY].remove(p_instance->conventional_indexer_id);
		p_instance->conventional_indexer_id = DynamicBVH::ID();
	} else if (!p_remove && !micro_geometry && geometry_instance && !p_instance->conventional_indexer_id.is_valid()) {
		p_instance->conventional_indexer_id = scenario->indexers[Scenario::INDEXER_CONVENTIONAL_GEOMETRY].insert(p_instance->transformed_aabb, p_instance);
	}
	if (p_instance->cull_domain_index != UINT32_MAX) {
		if (!p_remove && p_instance->micro_geometry_domain == micro_geometry) {
			return;
		}
		auto &previous = p_instance->micro_geometry_domain ? scenario->micro_geometry_instances : scenario->conventional_instances;
		previous[p_instance->cull_domain_index] = previous[previous.size() - 1];
		previous[p_instance->cull_domain_index]->cull_domain_index = p_instance->cull_domain_index;
		previous.resize(previous.size() - 1);
		p_instance->cull_domain_index = UINT32_MAX;
	}
	if (!p_remove) {
		auto &domain = micro_geometry ? scenario->micro_geometry_instances : scenario->conventional_instances;
		p_instance->micro_geometry_domain = micro_geometry;
		p_instance->cull_domain_index = domain.size();
		domain.push_back(p_instance);
	}
}

void RendererSceneCull::_instance_update_scene_membership(Instance *p_instance) {
	if (p_instance->base_type == RSE::INSTANCE_PARTICLES_COLLISION && p_instance->base_data) {
		const InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(p_instance->base_data);
		if (p_instance->scenario && p_instance->visible && RSG::particles_storage->particles_collision_is_heightfield(p_instance->base) && (collision->heightfield_follow_camera || collision->heightfield_update_always)) {
			continuous_heightfield_particle_colliders.insert(p_instance);
		} else {
			continuous_heightfield_particle_colliders.erase(p_instance);
		}
	}
	p_instance->scenario_rid = p_instance->scenario ? p_instance->scenario->self : RID();
	_instance_update_cull_domain(p_instance);
	if (((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) && p_instance->base_data) {
		InstanceGeometryData *geometry = static_cast<InstanceGeometryData *>(p_instance->base_data);
		if (geometry->geometry_instance) {
			geometry->geometry_instance->scene_membership_changed();
		}
	}
}
