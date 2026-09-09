/**************************************************************************/
/*  render_raytracing.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_rtxdi.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererSceneRenderImplementation;

static uint64_t _rt_scene_hash(const void *p_data, int p_size, uint64_t p_seed) {
	return uint64_t(hash_murmur3_buffer(p_data, p_size, uint32_t(p_seed))) |
			(uint64_t(hash_murmur3_buffer(p_data, p_size, uint32_t(p_seed >> 32))) << 32);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderRaytracing::initialize(RenderForwardClustered *p_owner) {
	owner = p_owner;
	bindless_block = memnew(BindlessBlock);

	// Initialize merged MultiMesh BLAS compute shader.
	Vector<String> merge_modes;
	merge_modes.push_back("\n");
	merge_modes.push_back("\n#define MODE_INDEXED\n");
	mm_merge_shader.shader.initialize(merge_modes);
	mm_merge_shader.version = mm_merge_shader.shader.version_create();
	for (int i = 0; i < MergeShader::MODE_MAX; i++) {
		mm_merge_shader.version_shader[i] = mm_merge_shader.shader.version_get_shader(mm_merge_shader.version, i);
		mm_merge_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(mm_merge_shader.version_shader[i]);
	}
}

RenderRaytracing::~RenderRaytracing() {
	for (KeyValue<RenderSceneBuffersRD *, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();
	if (!retired_micro_geometry.is_empty()) {
		RD::get_singleton()->flush_and_stall();
		for (auto *build : retired_micro_geometry) {
			_free_micro_geometry(build);
		}
		retired_micro_geometry.clear();
	}
	if (micro_selection) {
		memdelete(micro_selection);
		micro_rt_shader.version_free(micro_rt_version);
	}
	if (ddgi_effect) {
		memdelete(ddgi_effect);
	}
	if (pathtracing) {
		memdelete(pathtracing);
	}

	_free_persistent_buffers();
	if (geometry_positions_version.is_valid()) {
		RD::get_singleton()->flush_and_stall();
		geometry_positions_shader.version_free(geometry_positions_version);
	}
	cleanup_caches();

	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mat_ubo_pool_buffer);
		mat_ubo_pool_buffer = RID();
	}

	if (bindless_block) {
		memdelete(bindless_block);
		bindless_block = nullptr;
	}
	mm_merge_shader.shader.version_free(mm_merge_shader.version);
}

// ---------------------------------------------------------------------------
// Per-viewport state lifecycle
// ---------------------------------------------------------------------------

RTViewportState *RenderRaytracing::_get_or_create_viewport_state(const RenderDataRD *p_render_data) {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(key);
	if (it != viewport_states.end()) {
		return it->value;
	}
	RTViewportState *state = memnew(RTViewportState);
	viewport_states.insert(key, state);
	return state;
}

bool RenderRaytracing::update_viewport_settings(const RenderDataRD *p_render_data) {
	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	ERR_FAIL_NULL_V(state, false);
	const RenderSceneDataRD &scene = *p_render_data->scene_data;
	Vector3 rt_origin = state->rt_origin;
	for (int axis = 0; axis < 3; axis++) {
		if (!state->coordinates_initialized || Math::abs(scene.cam_transform.origin[axis] - rt_origin[axis]) > real_t(1024.0)) {
			rt_origin[axis] = Math::round(scene.cam_transform.origin[axis] / real_t(1024.0)) * real_t(1024.0);
		}
	}
	const bool origin_changed = !state->coordinates_initialized || state->rt_origin != rt_origin;
	const bool uses_jitter = scene.taa_jitter != Vector2();
	const bool camera_moved = !state->coordinates_initialized || state->camera_transform != scene.cam_transform || !state->camera_projection.is_same(scene.cam_projection) || state->camera_orthogonal != scene.cam_orthogonal || state->camera_uses_jitter != uses_jitter;
	state->rt_origin = rt_origin;
	state->camera_transform = scene.cam_transform;
	state->camera_projection = scene.cam_projection;
	state->camera_orthogonal = scene.cam_orthogonal;
	state->camera_uses_jitter = uses_jitter;
	state->coordinates_initialized = true;
	Transform3D camera_to_rt = scene.cam_transform;
	camera_to_rt.origin -= rt_origin;
	Transform3D previous_camera_to_rt = origin_changed ? scene.cam_transform : scene.prev_cam_transform;
	previous_camera_to_rt.origin -= rt_origin;
	RendererRD::MaterialStorage::store_transform_transposed_3x4(camera_to_rt, state->frame_constants.camera_to_rt);
	RendererRD::MaterialStorage::store_transform_transposed_3x4(previous_camera_to_rt, state->frame_constants.previous_camera_to_rt);
	if (!state->frame_constants_buffer.is_valid()) {
		state->frame_constants_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(RTFrameConstants));
	}
	ERR_FAIL_COND_V(!state->frame_constants_buffer.is_valid(), true);
	RD::get_singleton()->buffer_update(state->frame_constants_buffer, 0, sizeof(RTFrameConstants), &state->frame_constants);
	const RendererEnvironmentStorage::RaytracingSettings settings = RendererEnvironmentStorage::get_singleton()->environment_get_raytracing_settings(p_render_data->environment);
	const bool environment_changed = !state->settings_initialized || state->settings_environment != p_render_data->environment;
	const bool layers_changed = state->settings_visible_layers != p_render_data->scene_data->camera_visible_layers;
	const bool mode_changed = state->settings.raytracing_rendering_mode != settings.raytracing_rendering_mode;
	const bool camera_changed = state->settings_camera != p_render_data->scene_data->camera;
	const bool camera_settings_changed = environment_changed || layers_changed || state->settings.mode_generation != settings.mode_generation || state->settings.ddgi_generation != settings.ddgi_generation || state->settings.pathtracing_generation != settings.pathtracing_generation;
	if (environment_changed || layers_changed || mode_changed || state->settings.ddgi_layout_generation != settings.ddgi_layout_generation || state->settings.ddgi_enabled != settings.ddgi_enabled) {
		state->ddgi_history_epoch++;
	}
	if (camera_settings_changed || camera_changed || camera_moved || origin_changed) {
		state->pathtracing_history_epoch++;
	}
	if (camera_settings_changed || camera_changed || origin_changed) {
		state->camera_history_epoch++;
		state->light_history_valid = false;
	}
	state->settings = settings;
	state->settings_environment = p_render_data->environment;
	state->settings_camera = p_render_data->scene_data->camera;
	state->settings_visible_layers = p_render_data->scene_data->camera_visible_layers;
	state->settings_initialized = true;
	return camera_settings_changed || camera_changed || origin_changed;
}

bool RenderRaytracing::_prepare_ddgi(RTViewportState *p_state, bool p_freeze_anchor) {
	const RendererEnvironmentStorage::RaytracingSettings &settings = p_state->settings;
	if (p_state->pathtracing && settings.raytracing_rendering_mode != RSE::RAYTRACING_RENDERING_MODE_PATH_TRACED && owner->get_debug_draw_mode() != RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RT) {
		memdelete(p_state->pathtracing);
		p_state->pathtracing = nullptr;
	}
	const bool enabled = settings.ddgi_enabled && settings.raytracing_rendering_mode == RSE::RAYTRACING_RENDERING_MODE_HYBRID;
	if (p_state->ddgi && (!enabled || p_state->ddgi->cascade_count != uint32_t(settings.ddgi_cascade_count) || p_state->ddgi->rays_per_probe != uint32_t(settings.ddgi_rays_per_probe) || p_state->ddgi->base_spacing != settings.ddgi_probe_spacing)) {
		memdelete(p_state->ddgi);
		p_state->ddgi = nullptr;
	}
	if (!enabled) {
		return true;
	}
	if (!ddgi_effect) {
		ddgi_effect = memnew(RendererRD::DDGIEffect);
	}
	if (!p_state->ddgi) {
		p_state->ddgi = ddgi_effect->create_context(settings);
	}
	ERR_FAIL_NULL_V(p_state->ddgi, false);
	RendererRD::DDGIEffect::Context &context = *p_state->ddgi;
	if (!p_freeze_anchor || !context.debug_anchor_frozen) {
		context.debug_anchor = p_state->camera_transform.origin;
	}
	context.debug_anchor_frozen = p_freeze_anchor;
	if (!ddgi_effect->prepare(context, context.debug_anchor, p_state->rt_origin, p_state->ddgi_history_epoch, settings.ddgi_updates_per_frame)) {
		memdelete(p_state->ddgi);
		p_state->ddgi = nullptr;
		return false;
	}
	return true;
}

bool RenderRaytracing::_render_ddgi(RTViewportState *p_state, RID p_scene_data, RID p_sky) {
	if (!p_state->ddgi) {
		return true;
	}
	RD *rd = RD::get_singleton();
	RendererRD::DDGIEffect::Context &context = *p_state->ddgi;
	RID shader = ddgi_effect->get_trace_shader(owner->is_using_radiance_octmap_array());
	ERR_FAIL_COND_V(shader.is_null(), false);
	if (context.material_source_pipeline != p_state->material_pipeline || !rd->raytracing_pipeline_is_valid(context.trace_pipeline)) {
		if (rd->raytracing_pipeline_is_valid(context.trace_pipeline)) {
			rd->free_rid(context.trace_sbt);
			rd->free_rid(context.trace_pipeline);
		}
		context.trace_pipeline = RID();
		context.trace_sbt = RID();
		RD::PipelineShader entry;
		entry.shader = shader;
		ERR_FAIL_COND_V(!create_material_pipeline(p_state, { &entry, 1 }, { &entry, 1 }, 1, context.trace_pipeline, context.trace_sbt), false);
		context.material_source_pipeline = p_state->material_pipeline;
	}
	RID material_set = create_material_uniform_set(p_state, p_scene_data, shader);
	ERR_FAIL_COND_V(material_set.is_null(), false);
	RID bindless_set = get_bindless_uniform_set(shader);
	bool valid = bindless_set.is_valid();
	const RTLightSnapshot &lights = p_state->light_snapshots[p_state->current_light_snapshot];
	for (uint32_t cascade_index : context.updates) {
		if (!valid) {
			break;
		}
		RendererRD::DDGIEffect::Cascade &cascade = context.cascades[cascade_index];
		valid = ddgi_effect->begin_update(context, cascade_index);
		if (!valid) {
			break;
		}
		ddgi_effect->update_frame(context, cascade_index, p_state->settings_visible_layers);
		LocalVector<RD::Uniform> uniforms = ddgi_effect->get_grid_uniforms(context);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, { cascade.ray_data }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 8, { cascade.traced_data }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { lights.light_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, { lights.parameters_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, { p_sky }));
		RID grid_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 2, uniforms);
		if (grid_set.is_null()) {
			valid = false;
			break;
		}
		RENDER_TIMESTAMP(vformat("DDGI Cascade %d Trace", cascade_index));
		RD::RaytracingListID list = rd->raytracing_list_begin();
		rd->raytracing_list_bind_raytracing_pipeline(list, context.trace_pipeline);
		rd->raytracing_list_bind_uniform_set(list, material_set, 0);
		rd->raytracing_list_bind_uniform_set(list, bindless_set, 1);
		rd->raytracing_list_bind_uniform_set(list, grid_set, 2);
		register_raytracing_buffer_dependencies(list);
		rd->raytracing_list_trace_rays(list, 0, context.trace_sbt, context.rays_per_probe, RendererRD::DDGIEffect::PROBE_COUNT, 1);
		rd->raytracing_list_end();
		valid = ddgi_effect->finish_update(context, cascade_index);
	}
	rd->free_rid(material_set);
	if (!valid) {
		memdelete(p_state->ddgi);
		p_state->ddgi = nullptr;
	}
	return valid;
}

RTViewportState *RenderRaytracing::_get_viewport_state(const RenderDataRD *p_render_data) const {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::ConstIterator it = viewport_states.find(key);
	return (it != viewport_states.end()) ? it->value : nullptr;
}

void RenderRaytracing::_free_viewport_state_internal(RTViewportState *p_state) {
	if (!p_state) {
		return;
	}
	if (p_state->micro_geometry && p_state->tlas.is_valid()) {
		retired_tlas_memory.push_back({ RD::get_singleton()->acceleration_structure_get_memory_usage(p_state->tlas), RD::get_singleton()->get_pending_submission_serial() });
	}
	_retire_micro_geometry(p_state->micro_geometry);
	p_state->micro_geometry = nullptr;
	if (RD::get_singleton()->raytracing_pipeline_is_valid(p_state->material_pipeline)) {
		RD::get_singleton()->free_rid(p_state->material_sbt);
		RD::get_singleton()->free_rid(p_state->material_pipeline);
	}
	for (RID resource : { p_state->material_frame_buffer, p_state->decal_buffer, p_state->material_unused_buffer }) {
		if (resource.is_valid()) {
			RD::get_singleton()->free_rid(resource);
		}
	}
	if (p_state->tlas.is_valid()) {
		RD::get_singleton()->free_rid(p_state->tlas);
	}
	if (p_state->frame_constants_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->frame_constants_buffer);
	}
	if (p_state->geometry_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->geometry_buffer);
	}
	if (p_state->material_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->material_buffer);
	}
	if (p_state->motion_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_index_buffer);
	}
	if (p_state->motion_transform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_transform_buffer);
	}
	for (RTLightSnapshot &snapshot : p_state->light_snapshots) {
		if (snapshot.light_buffer.is_valid()) {
			RD::get_singleton()->free_rid(snapshot.light_buffer);
		}
		if (snapshot.current_to_previous_buffer.is_valid()) {
			RD::get_singleton()->free_rid(snapshot.current_to_previous_buffer);
		}
		if (snapshot.previous_to_current_buffer.is_valid()) {
			RD::get_singleton()->free_rid(snapshot.previous_to_current_buffer);
		}
		if (snapshot.parameters_buffer.is_valid()) {
			RD::get_singleton()->free_rid(snapshot.parameters_buffer);
		}
	}
	RenderRTXDI::free_viewport_resources(p_state);
	if (p_state->ddgi) {
		memdelete(p_state->ddgi);
	}
	if (p_state->pathtracing) {
		memdelete(p_state->pathtracing);
	}
	memdelete(p_state);
}

void RenderRaytracing::free_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(p_render_buffers);
	if (it == viewport_states.end()) {
		return;
	}
	_free_viewport_state_internal(it->value);
	viewport_states.remove(it);
}

// ---------------------------------------------------------------------------
// Material UBO sub-allocation pool
//
// Single device-address storage buffer of MAT_UBO_POOL_TOTAL_BYTES, divided
// into MAT_UBO_POOL_CAPACITY fixed-size slots. Allocate/release just bump a
// next-slot counter and a free-list. Per-material UBO writes are
// buffer_update at slot offset, no allocation. mat.uniform_address is
// pool_bda + slot * slot_size, so the closest-hit shader's
// CustomMaterialUniforms(addr) cast lands on the correct slot.
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t MAT_UBO_POOL_SLOT_SIZE = 512;
constexpr uint32_t MAT_UBO_POOL_CAPACITY = 100000;
constexpr uint64_t MAT_UBO_POOL_TOTAL_BYTES = uint64_t(MAT_UBO_POOL_SLOT_SIZE) * MAT_UBO_POOL_CAPACITY;
} // namespace

void RenderRaytracing::mat_ubo_pool_ensure_initialized() {
	if (mat_ubo_pool_buffer.is_valid()) {
		return;
	}
	Vector<uint8_t> init;
	init.resize(MAT_UBO_POOL_TOTAL_BYTES);
	memset(init.ptrw(), 0, MAT_UBO_POOL_TOTAL_BYTES);
	mat_ubo_pool_buffer = RD::get_singleton()->storage_buffer_create(
			MAT_UBO_POOL_TOTAL_BYTES, init, 0,
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(mat_ubo_pool_buffer, "RT Material UBO Pool");
		mat_ubo_pool_bda = RD::get_singleton()->buffer_get_device_address(mat_ubo_pool_buffer);
	}
}

uint32_t RenderRaytracing::mat_ubo_pool_allocate() {
	mat_ubo_pool_ensure_initialized();
	if (!mat_ubo_pool_buffer.is_valid()) {
		return UINT32_MAX;
	}
	if (mat_ubo_pool_free_count > 0) {
		--mat_ubo_pool_free_count;
		return mat_ubo_pool_free_slots[mat_ubo_pool_free_count];
	}
	if (mat_ubo_pool_next_slot >= MAT_UBO_POOL_CAPACITY) {
		ERR_PRINT_ONCE("RT Material UBO Pool exhausted; falling back to dedicated buffer.");
		return UINT32_MAX;
	}
	return mat_ubo_pool_next_slot++;
}

void RenderRaytracing::mat_ubo_pool_release(uint32_t p_slot) {
	if (p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	if (mat_ubo_pool_free_count < mat_ubo_pool_free_slots.size()) {
		mat_ubo_pool_free_slots[mat_ubo_pool_free_count] = p_slot;
	} else {
		mat_ubo_pool_free_slots.push_back(p_slot);
	}
	++mat_ubo_pool_free_count;
}

void RenderRaytracing::mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size) {
	if (!mat_ubo_pool_buffer.is_valid() || p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	uint32_t size = MIN(p_size, MAT_UBO_POOL_SLOT_SIZE);
	RD::get_singleton()->buffer_update(mat_ubo_pool_buffer, uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE, size, p_data);
}

uint64_t RenderRaytracing::mat_ubo_pool_get_address(uint32_t p_slot) const {
	return mat_ubo_pool_bda + uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void RenderRaytracing::cleanup_caches() {
	RD *rd = RD::get_singleton();

	for (uint32_t i = 0; i < surface_chunks.size(); i++) {
		if (surface_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTCacheEntry *entry = &surface_chunks[i][j];
				if (entry->ptr) {
					_release_surface_blas(entry->ptr, false);
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(surface_chunks[i]);
		}
	}
	surface_chunks.clear();

	for (const RTDeferredResourceFree &deferred : deferred_resource_frees) {
		if (deferred.resource.is_valid()) {
			rd->free_rid(deferred.resource);
		}
	}
	deferred_resource_frees.clear();

	// Free all cached deformed surface data.
	{
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTDeformedCacheEntry *e = deformed_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->ptr) {
				if (rd->acceleration_structure_is_valid(e->ptr->blas)) {
					rd->free_rid(e->ptr->blas);
				}
				memdelete(e->ptr);
				e->ptr = nullptr;
			}
			if (e->owned_vb_full.is_valid()) {
				rd->free_rid(e->owned_vb_full);
				e->owned_vb_full = RID();
			}
			if (e->prev_pos_vb.is_valid()) {
				rd->free_rid(e->prev_pos_vb);
				e->prev_pos_vb = RID();
			}
			deformed_pool.free(live[i]);
		}
	}

	// Free all merged MultiMesh BLAS data.
	{
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTMergedMMEntry *e = merged_mm_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->blas.is_valid()) {
				rd->free_rid(e->blas);
				e->blas = RID();
			}
			if (e->previous_position_buffer.is_valid()) {
				rd->free_rid(e->previous_position_buffer);
				e->previous_position_buffer = RID();
			}
			if (e->merged_vtx_buffer.is_valid()) {
				rd->free_rid(e->merged_vtx_buffer);
				e->merged_vtx_buffer = RID();
			}
			if (e->merged_attr_buffer.is_valid()) {
				rd->free_rid(e->merged_attr_buffer);
				e->merged_attr_buffer = RID();
			}
			if (e->replicated_idx_buffer.is_valid()) {
				rd->free_rid(e->replicated_idx_buffer);
				e->replicated_idx_buffer = RID();
			}
			merged_mm_pool.free(live[i]);
		}
	}
	mm_handles.clear();
	deformed_active_this_frame.clear();
	merged_mm_active_this_frame.clear();

	// Free all cached material data
	for (uint32_t i = 0; i < material_chunks.size(); i++) {
		if (material_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTMaterialCacheEntry *entry = &material_chunks[i][j];
				if (entry->ptr) {
					if (entry->ptr->uniform_buffer.is_valid()) {
						RD::get_singleton()->free_rid(entry->ptr->uniform_buffer);
					}
					if (entry->ptr->uniform_pool_slot != UINT32_MAX) {
						mat_ubo_pool_release(entry->ptr->uniform_pool_slot);
					}
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(material_chunks[i]);
		}
	}
	material_chunks.clear();

	// Reset counters
	material_free_slots.clear();
	next_material_slot = 0;
	vram_used = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ---------------------------------------------------------------------------
// RID helpers
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::get_rid_index(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() & 0xFFFFFFFFULL);
}

uint32_t RenderRaytracing::get_rid_version(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() >> 32);
}

RTCacheEntry *RenderRaytracing::get_surface_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= surface_chunks.size()) {
		surface_chunks.push_back(nullptr);
	}

	if (!surface_chunks[chunk_idx]) {
		surface_chunks.set(chunk_idx, memnew_arr(RTCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			surface_chunks[chunk_idx][i] = RTCacheEntry();
		}
	}

	return &surface_chunks[chunk_idx][entry_idx];
}

RTMaterialCacheEntry *RenderRaytracing::get_material_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= material_chunks.size()) {
		material_chunks.push_back(nullptr);
	}

	if (!material_chunks[chunk_idx]) {
		material_chunks.set(chunk_idx, memnew_arr(RTMaterialCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			material_chunks[chunk_idx][i] = RTMaterialCacheEntry();
		}
	}

	return &material_chunks[chunk_idx][entry_idx];
}

uint32_t RenderRaytracing::allocate_material_slot() {
	if (!material_free_slots.is_empty()) {
		uint32_t slot = material_free_slots[material_free_slots.size() - 1];
		material_free_slots.resize(material_free_slots.size() - 1);
		return slot;
	}
	return next_material_slot++;
}

// ---------------------------------------------------------------------------
// Slot-pool access helpers
// ---------------------------------------------------------------------------

RTDeformedCacheEntry *RenderRaytracing::_access_deformed_slot(RID &r_handle) {
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(r_handle);
	if (!entry) {
		// Stale or never-allocated handle -> fresh slot. RID_Owner default-
		// constructs the entry, so all members start at their cleared values.
		r_handle = deformed_pool.make_rid();
		entry = deformed_pool.get_or_null(r_handle);
	}
	deformed_active_this_frame.push_back(r_handle);
	return entry;
}

RTMergedMMEntry *RenderRaytracing::_access_merged_mm_slot(RID &r_handle) {
	RTMergedMMEntry *entry = merged_mm_pool.get_or_null(r_handle);
	if (!entry) {
		r_handle = merged_mm_pool.make_rid();
		entry = merged_mm_pool.get_or_null(r_handle);
	}
	merged_mm_active_this_frame.push_back(r_handle);
	return entry;
}

// ---------------------------------------------------------------------------
// Per-frame preparation
// ---------------------------------------------------------------------------

void RenderRaytracing::prepare_frame() {
	for (uint32_t index = 0; index < retired_tlas_memory.size();) {
		if (retired_tlas_memory[index].submission <= RD::get_singleton()->get_completed_submission_serial()) {
			retired_tlas_memory.remove_at(index);
		} else {
			index++;
		}
	}
	geometry_buffer_dependencies.clear();
	blass.clear();
	blas_transforms.clear();
	instance_flags.clear();
	instance_masks.clear();
	geometry_data.clear();
	material_data.clear();
	geometry_material_programs.clear();
	motion_indices.clear();
	motion_transforms.clear();
	emissive_sources.clear();
	deformed_active_this_frame.clear();
	merged_mm_active_this_frame.clear();

	for (uint32_t index = 0; index < uint32_t(retired_micro_geometry.size());) {
		auto *build = retired_micro_geometry[index];
		if (build->pending_feedback == 0 && build->retirement <= RD::get_singleton()->get_completed_submission_serial()) {
			_free_micro_geometry(build);
			retired_micro_geometry.remove_at(index);
		} else {
			index++;
		}
	}
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RD *rd = RD::get_singleton();

	for (uint32_t i = deferred_resource_frees.size(); i > 0; i--) {
		RTDeferredResourceFree &deferred = deferred_resource_frees[i - 1];
		if (deferred.submission > rd->get_completed_submission_serial()) {
			continue;
		}
		if (deferred.resource.is_valid()) {
			rd->free_rid(deferred.resource);
		}
		deferred_resource_frees.remove_at_unordered(i - 1);
	}

	_sweep_dead_surfaces();

	// TTL-evict stale deformed-surface entries.
	{
		static const uint32_t DEFORMED_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/raytracing/deformed_mesh_cache_ttl_frames");
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTDeformedCacheEntry *e = deformed_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->last_used_frame == 0 || current_frame - e->last_used_frame <= DEFORMED_CACHE_TTL) {
				continue;
			}
			if (e->ptr) {
				if (rd->acceleration_structure_is_valid(e->ptr->blas)) {
					rd->free_rid(e->ptr->blas);
				}
				memdelete(e->ptr);
				e->ptr = nullptr;
			}
			if (e->owned_vb_full.is_valid()) {
				rd->free_rid(e->owned_vb_full);
				e->owned_vb_full = RID();
			}
			if (e->prev_pos_vb.is_valid()) {
				rd->free_rid(e->prev_pos_vb);
				e->prev_pos_vb = RID();
			}
			deformed_pool.free(live[i]);
		}
	}

	// TTL-evict stale merged-MultiMesh entries.
	{
		static const uint32_t MM_BLAS_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/raytracing/multimesh_blas_cache_ttl_frames");
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTMergedMMEntry *e = merged_mm_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->last_used_frame == 0 || current_frame - e->last_used_frame <= MM_BLAS_CACHE_TTL) {
				continue;
			}
			if (e->blas.is_valid()) {
				rd->free_rid(e->blas);
				e->blas = RID();
			}
			if (e->previous_position_buffer.is_valid()) {
				rd->free_rid(e->previous_position_buffer);
				e->previous_position_buffer = RID();
			}
			if (e->merged_vtx_buffer.is_valid()) {
				rd->free_rid(e->merged_vtx_buffer);
				e->merged_vtx_buffer = RID();
			}
			if (e->merged_attr_buffer.is_valid()) {
				rd->free_rid(e->merged_attr_buffer);
				e->merged_attr_buffer = RID();
			}
			if (e->replicated_idx_buffer.is_valid()) {
				rd->free_rid(e->replicated_idx_buffer);
				e->replicated_idx_buffer = RID();
			}
			merged_mm_pool.free(live[i]);
		}
	}

	// Reset per-frame metrics
	cache_hits = 0;
	cache_misses = 0;

	if (!bindless_block->is_initialized()) {
		bindless_block->initialize(RD::get_singleton());
	}
	bindless_block->begin_frame();
}

// ---------------------------------------------------------------------------
// Surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_surface(
		const void *p_surf,
		void *p_mesh_surface,
		uint32_t p_surface_invalidation_counter,
		const Transform3D &p_transform,
		LocalVector<RID> &r_dirty_blas_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	RID mesh_rid = surf->owner->data->base;
	if (surf->owner->data->base_type == RSE::INSTANCE_MULTIMESH) {
		RID underlying = mesh_storage->multimesh_get_mesh(mesh_rid);
		if (underlying.is_valid()) {
			mesh_rid = underlying;
		}
	}

	uint32_t cache_key = (mesh_rid.get_local_index() << 8) | (surf->surface_index & 0xFF);
	uint32_t mesh_version = get_rid_version(mesh_rid);
	RTCacheEntry *entry = get_surface_cache_entry(cache_key);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mesh_version ||
			entry->cached_counter != p_surface_invalidation_counter;

	if (!needs_refresh && RD::get_singleton()->acceleration_structure_is_valid(entry->ptr->blas)) {
		cache_hits++;
		entry->last_used_frame = current_frame;
		return entry->ptr;
	}

	cache_misses++;

	// Allocate or reuse entry
	if (!entry->ptr) {
		entry->ptr = memnew(RTSurfaceData);
	} else {
		// Unconditional, including on a version mismatch.
		_release_surface_blas(entry->ptr, true);
	}

	entry->owner_mesh = mesh_storage->owns_mesh(mesh_rid) ? mesh_rid : RID();

	RTSurfaceData *surf_data = entry->ptr;

	_populate_surface_blas(p_mesh_surface, RID(), false, cache_key, surf_data, r_dirty_blas_list);

	surf->cached_final_transform_valid = false;

	if (!surf_data->blas.is_valid()) {
		return surf_data;
	}

	entry->cached_counter = p_surface_invalidation_counter;
	entry->cached_rid_version = mesh_version;
	entry->last_used_frame = current_frame;

	return surf_data;
}

// ---------------------------------------------------------------------------
// Deformed surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_deformed_surface(
		const void *p_surf,
		void *p_mesh_surface,
		const RTDeformedGeometrySource &p_source,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	if (!p_source.current_vb.is_valid()) {
		return nullptr;
	}

	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// Compute the deformed VB layout.
	uint64_t fmt = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	bool is_2d = fmt & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool has_n = fmt & RSE::ARRAY_FORMAT_NORMAL;
	bool has_t = fmt & RSE::ARRAY_FORMAT_TANGENT;
	uint32_t position_stride = is_2d ? (uint32_t)sizeof(float) * 2u : (uint32_t)sizeof(float) * 3u;
	uint32_t tbn_section_per_vertex = has_n ? (has_t ? 8u : 4u) : 0u;
	uint32_t per_vertex_bytes = position_stride + tbn_section_per_vertex;
	uint32_t full_size = per_vertex_bytes * vertex_count;
	uint32_t pos_size = position_stride * vertex_count;

	if (vertex_count == 0 || full_size == 0) {
		return nullptr;
	}

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	uint64_t buffer_id = p_source.current_vb.get_id();

	RTDeformedCacheEntry *entry_ptr = _access_deformed_slot(surf->rt_deformed_handle);
	ERR_FAIL_NULL_V(entry_ptr, nullptr);
	RTDeformedCacheEntry &entry = *entry_ptr;
	if (entry.ptr && !rd->acceleration_structure_is_valid(entry.ptr->blas)) {
		entry.ptr->blas = RID();
		entry.blas_built_once = false;
	}

	bool layout_changed = entry.cached_vertex_count != vertex_count || entry.cached_full_size != full_size;
	bool data_changed = layout_changed ||
			entry.cached_change_stamp != p_source.change_stamp ||
			entry.cached_buffer_id != buffer_id;
	bool needs_full_rebuild = !entry.ptr || !entry.blas_built_once || layout_changed ||
			entry.cached_key_version != p_source.cache_version ||
			entry.cached_surface_counter != p_source.surface_counter;

	BitField<RD::BufferCreationBits> owned_full_flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;
	BitField<RD::BufferCreationBits> prev_flags = RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;

	// Reallocate owned storage if the surface grew.
	if (entry.owned_vb_full_capacity < full_size) {
		if (entry.ptr && entry.ptr->blas.is_valid()) {
			rd->free_rid(entry.ptr->blas);
			entry.ptr->blas = RID();
			entry.blas_built_once = false;
		}
		if (entry.owned_vb_full.is_valid()) {
			rd->free_rid(entry.owned_vb_full);
		}
		entry.owned_vb_full = rd->vertex_buffer_create(full_size, Span<uint8_t>(), owned_full_flags);
		ERR_FAIL_COND_V(!entry.owned_vb_full.is_valid(), nullptr);
		entry.owned_vb_full_capacity = full_size;
		rd->set_resource_name(entry.owned_vb_full, "RT deformed owned VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
		needs_full_rebuild = true;
	}
	if (entry.prev_pos_vb_capacity < pos_size) {
		if (entry.prev_pos_vb.is_valid()) {
			rd->free_rid(entry.prev_pos_vb);
		}
		entry.prev_pos_vb = rd->storage_buffer_create(pos_size, Vector<uint8_t>(), 0, prev_flags);
		ERR_FAIL_COND_V(!entry.prev_pos_vb.is_valid(), nullptr);
		entry.prev_pos_vb_capacity = pos_size;
		rd->set_resource_name(entry.prev_pos_vb, "RT deformed prev pos VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
	}

	// Per-frame copies run exactly once per frame, even when the same surface is visible from multiple viewports.
	const bool first_touch_this_frame = entry.last_used_frame != current_frame;
	if (first_touch_this_frame) {
		if (entry.prev_pos_seeded) {
			rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
		}
		if (data_changed || !entry.prev_pos_seeded) {
			rd->buffer_copy(p_source.current_vb, entry.owned_vb_full, 0, 0, full_size);
		}
		if (!entry.prev_pos_seeded) {
			// Seed prev = current so motion vectors are zero on the first frame.
			rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
			entry.prev_pos_seeded = true;
		}
	}

	if (needs_full_rebuild) {
		cache_misses++;
		if (!entry.ptr) {
			entry.ptr = memnew(RTSurfaceData);
		}
		if (entry.ptr->blas.is_valid()) {
			rd->free_rid(entry.ptr->blas);
			entry.ptr->blas = RID();
		}
		_populate_surface_blas(p_mesh_surface, entry.owned_vb_full, true,
				static_cast<uint32_t>(p_source.cache_key), entry.ptr, r_dirty_blas_list);
		entry.blas_built_once = entry.ptr->blas.is_valid();
	} else if (data_changed) {
		cache_misses++;
		r_dirty_blas_update_list.push_back(entry.ptr->blas);
	} else {
		cache_hits++;
	}

	if (entry.ptr && entry.ptr->blas.is_valid()) {
		entry.ptr->geometry.flags |= RT_GEOM_FLAG_DEFORMED;
		uint64_t prev_addr = rd->buffer_get_device_address(entry.prev_pos_vb);
		entry.ptr->geometry.prev_vertex_buffer_address_lo = static_cast<uint32_t>(prev_addr & 0xFFFFFFFFULL);
		entry.ptr->geometry.prev_vertex_buffer_address_hi = static_cast<uint32_t>(prev_addr >> 32);
	}

	surf->cached_final_transform_valid = false;

	entry.cached_change_stamp = p_source.change_stamp;
	entry.cached_key_version = p_source.cache_version;
	entry.cached_surface_counter = p_source.surface_counter;
	entry.cached_buffer_id = buffer_id;
	entry.cached_vertex_count = vertex_count;
	entry.cached_full_size = full_size;
	entry.last_used_frame = current_frame;

	return entry.ptr;
}

// ---------------------------------------------------------------------------
// Surface BLAS helper (shared between static and deformed paths)
// ---------------------------------------------------------------------------

// Fills RTSurfaceData geometry metadata from the surface format.
// Returns the RIDs needed for BLAS creation so _populate_surface_blas can use them.
static void _fill_surface_geometry_data(
		void *p_mesh_surface,
		bool p_force_uncompressed,
		RTSurfaceData *r_surf_data,
		RID *r_vertex_buffer = nullptr,
		RID *r_attribute_buffer = nullptr,
		RID *r_index_buffer = nullptr) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	bool compressed = (surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) && !p_force_uncompressed;
	bool is_2d = surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	RT_GeometryData &geom = r_surf_data->geometry;
	memset(&geom, 0, sizeof(geom));
	geom.position_scale[0] = 1.0f;
	geom.position_scale[1] = 1.0f;
	geom.position_scale[2] = 1.0f;
	for (float &component : geom.instance_color) {
		component = 1.0f;
	}

	RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	RID attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);

	if (r_vertex_buffer) {
		*r_vertex_buffer = vertex_buffer;
	}
	if (r_attribute_buffer) {
		*r_attribute_buffer = attribute_buffer;
	}
	if (r_index_buffer) {
		*r_index_buffer = index_buffer;
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);

	geom.vertex_count = vertex_count;

	// Position stride
	uint32_t position_stride;
	if (is_2d) {
		position_stride = sizeof(float) * 2;
	} else if (compressed) {
		position_stride = sizeof(uint16_t) * 4;
	} else {
		position_stride = sizeof(float) * 3;
	}
	geom.position_stride = position_stride;
	if (compressed) {
		AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
		geom.position_offset[0] = surface_aabb.position.x;
		geom.position_offset[1] = surface_aabb.position.y;
		geom.position_offset[2] = surface_aabb.position.z;
		geom.position_scale[0] = surface_aabb.size.x;
		geom.position_scale[1] = surface_aabb.size.y;
		geom.position_scale[2] = surface_aabb.size.z;
	}

	// Normal/tangent layout
	uint32_t normal_stride;
	uint32_t tangent_stride = 0;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	uint32_t current_offset = position_stride * vertex_count;

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;

	if (compressed) {
		normal_stride = sizeof(uint16_t) * 2;
		if (has_normal) {
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		}
	} else {
		if (has_normal && has_tangent) {
			normal_stride = sizeof(uint16_t) * 4;
			tangent_stride = sizeof(uint16_t) * 4;
			geom.normal_byte_offset = current_offset;
			geom.tangent_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else if (has_normal) {
			normal_stride = sizeof(uint16_t) * 2;
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else {
			normal_stride = 0;
		}
	}
	geom.normal_stride = normal_stride;
	geom.tangent_stride = tangent_stride;
	geom.flags = compressed ? RT_GEOM_FLAG_COMPRESSED : 0;

	// Attribute buffer layout
	uint32_t attrib_offset = 0;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.uv2_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;

	if (surface_format & RSE::ARRAY_FORMAT_COLOR) {
		geom.color_byte_offset = attrib_offset;
		attrib_offset += sizeof(uint32_t);
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV) {
		geom.uv_byte_offset = attrib_offset;
		attrib_offset += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		geom.uv2_byte_offset = attrib_offset;
		attrib_offset += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	for (int ci = 0; ci < RSE::ARRAY_CUSTOM_COUNT; ci++) {
		geom.custom_byte_offsets[ci] = RT_OFFSET_NONE;
		const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
		if (surface_format & (1ULL << (RSE::ARRAY_CUSTOM0 + ci))) {
			uint32_t fmt = (surface_format >> fmt_shift[ci]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
			geom.custom_byte_offsets[ci] = attrib_offset;
			geom.custom_formats |= fmt << (ci * 3);
			const uint32_t fmtsize[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
			attrib_offset += fmtsize[fmt];
		}
	}
	geom.attribute_stride = attrib_offset;
	RID skin_buffer = mesh_storage->mesh_surface_get_skin_buffer(p_mesh_surface);
	if (skin_buffer.is_valid()) {
		geom.skin_address = RD::get_singleton()->buffer_get_device_address(skin_buffer);
		geom.skin_weight_offset = (surface_format & RSE::ARRAY_FLAG_USE_8_BONE_WEIGHTS) ? 16u : 8u;
		geom.skin_stride = geom.skin_weight_offset * 2u;
	}

	// UV scale (fp16 packed, matches GLSL unpackHalf2x16)
	Vector4 uv_scale = mesh_storage->mesh_surface_get_uv_scale(p_mesh_surface);
	geom.uv_scale_packed = (uint32_t(Math::make_half_float(uv_scale.y)) << 16) | Math::make_half_float(uv_scale.x);
	geom.uv2_scale_packed = (uint32_t(Math::make_half_float(uv_scale.w)) << 16) | Math::make_half_float(uv_scale.z);

	// Index format (no device address — caller fills those in)
	if (index_buffer.is_valid() && index_count > 0) {
		bool is_16bit = vertex_count <= 65536 && vertex_count > 0;
		geom.index_format = is_16bit ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}
}

void RenderRaytracing::_release_surface_blas(RTSurfaceData *p_surf_data, bool p_deferred) {
	if (!p_surf_data) {
		return;
	}

	RD *rd = RD::get_singleton();
	if (p_surf_data->blas.is_valid() && !rd->acceleration_structure_is_valid(p_surf_data->blas)) {
		p_surf_data->blas = RID();
	}
	RID *owned[] = { &p_surf_data->blas, &p_surf_data->position_buffer, &p_surf_data->position_parameters };
	for (RID *rid : owned) {
		if (!rid->is_valid()) {
			continue;
		}
		if (p_deferred) {
			deferred_resource_frees.push_back({ *rid, rd->get_pending_submission_serial() });
		} else {
			rd->free_rid(*rid);
		}
		*rid = RID();
	}
}

void RenderRaytracing::_sweep_dead_surfaces() {
	if (surface_chunks.is_empty()) {
		return;
	}

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	surface_sweep_chunk = (surface_sweep_chunk + 1) % surface_chunks.size();
	RTCacheEntry *chunk = surface_chunks[surface_sweep_chunk];
	if (!chunk) {
		return;
	}

	for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
		RTCacheEntry *entry = &chunk[i];
		if (!entry->ptr || !entry->owner_mesh.is_valid() || mesh_storage->owns_mesh(entry->owner_mesh)) {
			continue;
		}
		_release_surface_blas(entry->ptr, true);
		memdelete(entry->ptr);
		*entry = RTCacheEntry();
	}
}

void RenderRaytracing::_populate_surface_blas(
		void *p_mesh_surface,
		RID p_vertex_buffer_override,
		bool p_force_uncompressed,
		uint32_t p_cache_key,
		RTSurfaceData *r_surf_data,
		LocalVector<RID> &r_dirty_blas_list) {
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, p_force_uncompressed, r_surf_data,
			&vertex_buffer, &attribute_buffer, &index_buffer);

	if (p_vertex_buffer_override.is_valid()) {
		vertex_buffer = p_vertex_buffer_override;
	}

	RD *rd = RD::get_singleton();
	RT_GeometryData &geom = r_surf_data->geometry;

	if (vertex_buffer.is_valid()) {
		geom.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
	}
	if (attribute_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
	}
	if (index_buffer.is_valid() && geom.index_format != RT_INDEX_FORMAT_NONE) {
		geom.index_buffer_address = rd->buffer_get_device_address(index_buffer);
	}

	uint32_t index_count = (geom.index_format != RT_INDEX_FORMAT_NONE) ? geom.primitive_count * 3 : 0;
	bool is_2d = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	uint32_t position_stride = geom.position_stride;
	if (geom.flags & RT_GEOM_FLAG_COMPRESSED) {
		ERR_FAIL_COND(uint64_t(geom.vertex_count) * 12 > UINT32_MAX);
		if (geometry_positions_version.is_null()) {
			Vector<String> modes;
			modes.push_back("");
			geometry_positions_shader.initialize(modes);
			geometry_positions_version = geometry_positions_shader.version_create();
			geometry_positions_pipeline = rd->compute_pipeline_create(geometry_positions_shader.version_get_shader(geometry_positions_version, 0));
		}
		r_surf_data->position_buffer = rd->storage_buffer_create(uint64_t(geom.vertex_count) * 12, Span<uint8_t>(), 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		r_surf_data->position_parameters = rd->storage_buffer_create(sizeof(geom), Span<uint8_t>((const uint8_t *)&geom, sizeof(geom)));
		ERR_FAIL_COND(r_surf_data->position_buffer.is_null() || r_surf_data->position_parameters.is_null() || geometry_positions_pipeline.is_null());
		RID uniform = UniformSetCacheRD::get_singleton()->get_cache(geometry_positions_shader.version_get_shader(geometry_positions_version, 0), 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_surf_data->position_parameters),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_surf_data->position_buffer));
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, geometry_positions_pipeline);
		rd->compute_list_bind_uniform_set(list, uniform, 0);
		rd->compute_list_set_push_constant(list, &geom.vertex_count, sizeof(geom.vertex_count));
		rd->compute_list_add_buffer_dependency(list, vertex_buffer);
		uint32_t groups = (geom.vertex_count + 63) / 64;
		rd->compute_list_dispatch(list, MIN(groups, 1024u), (groups + 1023) / 1024, 1);
		rd->compute_list_end();
		vertex_buffer = r_surf_data->position_buffer;
		position_stride = 12;
		is_2d = false;
	}

	RD::AccelerationStructureGeometry as_geom;
	as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
	as_geom.geometry.triangles.vertex_buffer = vertex_buffer;
	as_geom.geometry.triangles.vertex_stride = position_stride;
	as_geom.geometry.triangles.vertex_count = geom.vertex_count;
	as_geom.geometry.triangles.vertex_format = is_2d ? RD::DATA_FORMAT_R32G32_SFLOAT : RD::DATA_FORMAT_R32G32B32_SFLOAT;

	if (index_buffer.is_valid() && index_count > 0) {
		as_geom.geometry.triangles.index_buffer = index_buffer;
		as_geom.geometry.triangles.index_count = index_count;
	}

	BitField<RD::AccelerationStructureFlagBits> as_flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT;
	as_flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);

	r_surf_data->blas = rd->blas_create({ &as_geom, 1 }, as_flags);
	if (!r_surf_data->blas.is_valid()) {
		return;
	}
	rd->set_resource_name(r_surf_data->blas, "RT BLAS deformed [" + itos(p_cache_key) + "]");
	r_dirty_blas_list.push_back(r_surf_data->blas);
}

void RenderRaytracing::update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list) {
	// Pack AABB data into a byte buffer.
	Vector<uint8_t> aabb_bytes;
	uint32_t aabb_count = 1;

	if (p_state->aabb_data.size() >= 6 && (p_state->aabb_data.size() % 6) == 0) {
		aabb_count = p_state->aabb_data.size() / 6;
		aabb_bytes.resize(p_state->aabb_data.size() * sizeof(float));
		memcpy(aabb_bytes.ptrw(), p_state->aabb_data.ptr(), aabb_bytes.size());
	} else {
		const AABB &a = p_state->culling_aabb;
		float single[6] = {
			(float)a.position.x, (float)a.position.y, (float)a.position.z,
			(float)(a.position.x + a.size.x), (float)(a.position.y + a.size.y), (float)(a.position.z + a.size.z)
		};
		aabb_bytes.resize(sizeof(single));
		memcpy(aabb_bytes.ptrw(), single, sizeof(single));
	}

	uint32_t required_bytes = aabb_bytes.size();
	bool needs_new_blas = false;

	// Grow-only: only recreate the buffer when capacity is exceeded or count changed.
	if (required_bytes > p_state->gpu_buffer_capacity || aabb_count != p_state->aabb_count) {
		if (p_state->blas.is_valid()) {
			RD::get_singleton()->free_rid(p_state->blas);
			p_state->blas = RID();
		}
		if (p_state->gpu_buffer.is_valid()) {
			RD::get_singleton()->free_rid(p_state->gpu_buffer);
		}
		p_state->gpu_buffer = RD::get_singleton()->storage_buffer_create(required_bytes, aabb_bytes,
				0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		p_state->gpu_buffer_capacity = required_bytes;
		p_state->aabb_count = aabb_count;
		needs_new_blas = true;
	} else {
		// Buffer is large enough -- just update contents.
		RD::get_singleton()->buffer_update(p_state->gpu_buffer, 0, required_bytes, aabb_bytes.ptr());
		needs_new_blas = !p_state->blas.is_valid();
	}

	if (needs_new_blas) {
		ERR_FAIL_COND(!p_state->gpu_buffer.is_valid());

		RD::AccelerationStructureGeometry geom;
		geom.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
		geom.geometry.aabbs.buffer = p_state->gpu_buffer;
		geom.geometry.aabbs.count = aabb_count;
		geom.geometry.aabbs.stride = 24; // VkAabbPositionsKHR: two float3 (min, max).
		p_state->blas = RD::get_singleton()->blas_create({ &geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}

	// BDA for shader access.
	if (p_state->expose_bounds && p_state->gpu_buffer.is_valid()) {
		p_state->gpu_buffer_address = RD::get_singleton()->buffer_get_device_address(p_state->gpu_buffer);
	} else {
		p_state->gpu_buffer_address = 0;
	}

	if (p_state->blas.is_valid()) {
		r_dirty_blas_list.push_back(p_state->blas);
	}
}

RTMaterialData *RenderRaytracing::process_material(RID p_material_rid, uint16_t p_material_invalidation_counter) {
	if (!bindless_block->is_initialized()) {
		bindless_block->initialize(RD::get_singleton());
	}

	// Static default material for invalid/null materials
	static RTMaterialData s_default_mat;
	static bool s_default_mat_initialized = false;
	if (!s_default_mat_initialized) {
		s_default_mat.data.albedo_color[0] = 1.0f;
		s_default_mat.data.albedo_color[1] = 1.0f;
		s_default_mat.data.albedo_color[2] = 1.0f;
		s_default_mat.data.albedo_color[3] = 1.0f;
		s_default_mat.data.emission_color[0] = 0.0f;
		s_default_mat.data.emission_color[1] = 0.0f;
		s_default_mat.data.emission_color[2] = 0.0f;
		s_default_mat.data.emission_strength = 0.0f;
		s_default_mat.data.roughness = 1.0f;
		s_default_mat.data.specular = 0.5f;
		s_default_mat.data.ao_strength = 1.0f;
		s_default_mat.data.uv1_scale[0] = 1.0f;
		s_default_mat.data.uv1_scale[1] = 1.0f;
		s_default_mat.data.uv1_offset[0] = 0.0f;
		s_default_mat.data.uv1_offset[1] = 0.0f;
		s_default_mat_initialized = true;
	}

	if (!p_material_rid.is_valid()) {
		p_material_rid = owner->scene_shader.default_material;
	}

	// Cache lookup
	uint32_t mat_idx = get_rid_index(p_material_rid);
	uint32_t mat_version = get_rid_version(p_material_rid);
	RTMaterialCacheEntry *entry = get_material_cache_entry(mat_idx);
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	SceneShaderForwardClustered::MaterialData *raster_material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(p_material_rid, RendererRD::MaterialStorage::SHADER_TYPE_3D));
	const RID hit_shader = raster_material && raster_material->shader_data ? raster_material->shader_data->get_hit_shader() : RID();
	const uint64_t shader_hash = material_storage->material_get_shader_code_rt_hash(p_material_rid);
	const uint64_t shader_hash_b = material_storage->material_get_shader_code_rt_hash_b(p_material_rid);
	const uint64_t content_generation = material_storage->material_get_rt_content_generation(p_material_rid);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	const bool needs_refresh = !entry->ptr ||
			entry->ptr->hit_shader != hit_shader ||
			entry->cached_rid_version != mat_version ||
			entry->cached_counter != p_material_invalidation_counter ||
			entry->cached_content_generation != content_generation ||
			entry->cached_shader_hash != shader_hash ||
			entry->cached_shader_hash_b != shader_hash_b;

	if (!needs_refresh) {
		entry->last_used_frame = current_frame;
		if (entry->ptr->uniform_buffer.is_valid()) {
			geometry_buffer_dependencies.insert(entry->ptr->uniform_buffer);
		}
		return entry->ptr;
	}

	// Cache miss - need to rebuild material
	if (!entry->ptr) {
		entry->ptr = memnew(RTMaterialData);
	}

	RTMaterialData *mat_data = entry->ptr;
	RT_MaterialData &mat = mat_data->data;

	// Initialize defaults
	mat.albedo_color[0] = 1.0f;
	mat.albedo_color[1] = 1.0f;
	mat.albedo_color[2] = 1.0f;
	mat.albedo_color[3] = 1.0f;
	mat.emission_color[0] = 0.0f;
	mat.emission_color[1] = 0.0f;
	mat.emission_color[2] = 0.0f;
	mat.emission_strength = 0.0f;
	mat.metallic = 0.0f;
	mat.roughness = 1.0f;
	mat.specular = 0.5f;
	mat.ao_strength = 1.0f;
	mat.flags = 0;
	mat.albedo_texture_idx = 0;
	mat.normal_texture_idx = 0;
	mat.orm_texture_idx = 0;
	mat.emission_texture_idx = 0;
	mat.uv1_scale[0] = 1.0f;
	mat.uv1_scale[1] = 1.0f;
	mat.uv1_offset[0] = 0.0f;
	mat.uv1_offset[1] = 0.0f;
	mat.normal_map_depth = 1.0f;
	mat.uniform_address = 0;

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	mat_data->uses_alpha_clip = false;
	mat_data->has_alpha_texture = false;

	mat.coverage_flags = 0;
	mat.coverage_sampler = 0;
	mat.alpha_scissor_threshold = 0.5f;
	mat.alpha_hash_scale = 1.0f;
	if (raster_material && raster_material->shader_data && raster_material->shader_data->generated_standard_material) {
		for (const ShaderCompiler::GeneratedCode::Texture &texture : raster_material->shader_data->texture_uniforms) {
			if (texture.name == SNAME("texture_albedo")) {
				DEV_ASSERT(texture.filter < ShaderLanguage::FILTER_DEFAULT && texture.repeat < ShaderLanguage::REPEAT_DEFAULT);
				mat.coverage_sampler = uint32_t(texture.filter) + (texture.repeat == ShaderLanguage::REPEAT_ENABLE ? 6u : 0u);
				break;
			}
		}
		const String &code = raster_material->shader_data->code;
		if (code.contains("ALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;")) {
			mat.coverage_flags |= 1u;
			mat.alpha_scissor_threshold = material_storage->material_get_param(p_material_rid, "alpha_scissor_threshold");
		}
		if (code.contains("ALPHA_HASH_SCALE = alpha_hash_scale;")) {
			mat.coverage_flags |= 2u;
			mat.alpha_hash_scale = material_storage->material_get_param(p_material_rid, "alpha_hash_scale");
		}
		if (code.contains("albedo_tex *= COLOR;")) {
			mat.coverage_flags |= 4u;
		}
	}

	// Helper lambda to get texture from material parameter
	// p_srgb should be true for color textures (albedo, emission) that need sRGB->linear conversion
	auto get_material_texture = [&](const StringName &p_param, bool p_srgb = false) -> RID {
		Variant tex_var = material_storage->material_get_param(p_material_rid, p_param);
		if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
			RID tex_rid = tex_var;
			if (tex_rid.is_valid()) {
				return texture_storage->texture_get_rd_texture(tex_rid, p_srgb);
			}
		}
		return RID();
	};

	// Textures
	// Albedo is a color texture - needs sRGB->linear conversion
	RID albedo_rd = get_material_texture("texture_albedo", true);
	if (albedo_rd.is_valid()) {
		mat.albedo_texture_idx = bindless_block->add_texture(albedo_rd);
	}

	RID normal_rd = get_material_texture("texture_normal");
	if (normal_rd.is_valid()) {
		mat.normal_texture_idx = bindless_block->add_texture(normal_rd);
		mat.flags |= RT_MAT_FLAG_HAS_NORMAL_MAP;

		Variant normal_scale_var = material_storage->material_get_param(p_material_rid, "normal_scale");
		if (normal_scale_var.get_type() == Variant::FLOAT) {
			mat.normal_map_depth = normal_scale_var;
		}
	}

	RID orm_rd = get_material_texture("texture_orm");
	if (orm_rd.is_valid()) {
		mat.orm_texture_idx = bindless_block->add_texture(orm_rd);
	} else {
		RID roughness_rd = get_material_texture("texture_roughness");
		if (roughness_rd.is_valid()) {
			mat.orm_texture_idx = bindless_block->add_texture(roughness_rd);
		}
	}

	// Emission is a color texture - needs sRGB->linear conversion
	RID emission_rd = get_material_texture("texture_emission", true);
	if (emission_rd.is_valid()) {
		mat.emission_texture_idx = bindless_block->add_texture(emission_rd);
		mat.flags |= RT_MAT_FLAG_HAS_EMISSION_TEX;
		// Set sensible defaults for emission when texture is present
		mat.emission_color[0] = 1.0f;
		mat.emission_color[1] = 1.0f;
		mat.emission_color[2] = 1.0f;
		mat.emission_strength = 1.0f;
	}

	// Material properties
	// Colors declared with source_color in Godot shaders are stored in sRGB;
	// material_get_param returns the raw sRGB value, so we convert to linear here.
	Variant albedo_var = material_storage->material_get_param(p_material_rid, "albedo");
	if (albedo_var.get_type() == Variant::COLOR) {
		Color c = ((Color)albedo_var).srgb_to_linear();
		mat.albedo_color[0] = c.r;
		mat.albedo_color[1] = c.g;
		mat.albedo_color[2] = c.b;
		mat.albedo_color[3] = c.a;
		mat_data->is_custom_shader = false;
	} else {
		mat_data->is_custom_shader = true;
	}
	if (raster_material && raster_material->shader_data) {
		const SceneShaderForwardClustered::ShaderData *shader_data = raster_material->shader_data;
		mat_data->uses_alpha_clip = shader_data->rt ? shader_data->rt->uses_alpha_clip : shader_data->uses_alpha_clip;
		const ShaderCompiler::GeneratedCode &generated = shader_data->hit_code;
		const uint32_t uniform_total_size = (generated.rt_uniform_total_size + 15u) & ~15u;
		Vector<uint8_t> ubo_data;
		ubo_data.resize(uniform_total_size);
		if (uniform_total_size > 0) {
			memset(ubo_data.ptrw(), 0, uniform_total_size);
		}
		for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &uniform : shader_data->hit_uniforms) {
			const ShaderLanguage::ShaderNode::Uniform &u = uniform.value;
			if (u.is_texture() || u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
				continue;
			}
			ERR_CONTINUE(u.order < 0 || u.order >= generated.uniform_offsets.size());
			uint8_t *destination = ubo_data.ptrw() + generated.uniform_offsets[u.order];
			if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
				uint32_t index = MAX(material_storage->global_shader_uniform_get_buffer_index(uniform.key), 0);
				memcpy(destination, &index, sizeof(index));
			} else {
				RendererRD::MaterialStorage::pack_uniform(u, material_storage->material_get_param(p_material_rid, uniform.key), destination);
			}
		}
		for (const ShaderCompiler::GeneratedCode::Texture &texture : generated.texture_uniforms) {
			Variant value = texture.global ? Variant(material_storage->global_shader_uniform_get_texture(texture.name)) : material_storage->material_get_param(p_material_rid, texture.name);
			Array array;
			if (value.get_type() == Variant::ARRAY) {
				array = value;
			}
			for (int element = 0; element < MAX(texture.array_size, 1); element++) {
				Variant selected = texture.array_size > 0 ? (element < array.size() ? array[element] : Variant()) : value;
				RID texture_rid;
				if (selected.get_type() == Variant::RID || selected.get_type() == Variant::OBJECT) {
					texture_rid = selected;
				}
				if (texture_rid.is_null()) {
					const HashMap<int, RID> *defaults = shader_data->default_texture_params.getptr(texture.name);
					if (defaults && defaults->has(element)) {
						texture_rid = (*defaults)[element];
					}
				}
				RID rd_texture = texture_rid.is_valid() ? texture_storage->texture_get_rd_texture(texture_rid, texture.use_color) : RID();
				if (rd_texture.is_null()) {
					rd_texture = raster_material->get_default_texture_id(texture.type, texture.hint);
				}
				uint32_t index = bindless_block->add_texture(rd_texture);
				memcpy(ubo_data.ptrw() + texture.rt_offset + element * sizeof(index), &index, sizeof(index));
			}
		}
		if (uniform_total_size > 0) {
			if (uniform_total_size <= MAT_UBO_POOL_SLOT_SIZE && mat_data->uniform_pool_slot == UINT32_MAX) {
				mat_data->uniform_pool_slot = mat_ubo_pool_allocate();
			}
			if (uniform_total_size <= MAT_UBO_POOL_SLOT_SIZE && mat_data->uniform_pool_slot != UINT32_MAX) {
				mat_ubo_pool_update(mat_data->uniform_pool_slot, ubo_data.ptr(), uniform_total_size);
				mat.uniform_address = mat_ubo_pool_get_address(mat_data->uniform_pool_slot);
				if (mat_data->uniform_buffer.is_valid()) {
					RD::get_singleton()->free_rid(mat_data->uniform_buffer);
					mat_data->uniform_buffer = RID();
				}
			} else {
				RID buffer = RD::get_singleton()->storage_buffer_create(uniform_total_size, ubo_data, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
				ERR_FAIL_COND_V(buffer.is_null(), &s_default_mat);
				if (mat_data->uniform_buffer.is_valid()) {
					RD::get_singleton()->free_rid(mat_data->uniform_buffer);
				}
				mat_data->uniform_buffer = buffer;
				RD::get_singleton()->set_resource_name(buffer, "RT Material UBO");
				mat.uniform_address = RD::get_singleton()->buffer_get_device_address(buffer);
				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					mat_ubo_pool_release(mat_data->uniform_pool_slot);
					mat_data->uniform_pool_slot = UINT32_MAX;
				}
			}
		}
	}
	if (mat.uniform_address == 0) {
		if (mat_data->uniform_buffer.is_valid()) {
			RD::get_singleton()->free_rid(mat_data->uniform_buffer);
			mat_data->uniform_buffer = RID();
		}
		if (mat_data->uniform_pool_slot != UINT32_MAX) {
			mat_ubo_pool_release(mat_data->uniform_pool_slot);
			mat_data->uniform_pool_slot = UINT32_MAX;
		}
	}

	Variant metallic_var = material_storage->material_get_param(p_material_rid, "metallic");
	if (metallic_var.get_type() == Variant::FLOAT) {
		mat.metallic = metallic_var;
	}

	Variant roughness_var = material_storage->material_get_param(p_material_rid, "roughness");
	if (roughness_var.get_type() == Variant::FLOAT) {
		mat.roughness = roughness_var;
	}

	Variant specular_var = material_storage->material_get_param(p_material_rid, "specular");
	if (specular_var.get_type() == Variant::FLOAT) {
		mat.specular = specular_var;
	}

	Variant emission_var = material_storage->material_get_param(p_material_rid, "emission");
	if (emission_var.get_type() == Variant::COLOR) {
		Color c = ((Color)emission_var).srgb_to_linear();
		mat.emission_color[0] = c.r;
		mat.emission_color[1] = c.g;
		mat.emission_color[2] = c.b;
	}

	Variant emission_energy_var = material_storage->material_get_param(p_material_rid, "emission_energy");
	if (emission_energy_var.get_type() == Variant::FLOAT) {
		mat.emission_strength = emission_energy_var;
	}

	// UV1 scale and offset (vec3 in Godot, we only use xy).
	Variant uv1_scale_var = material_storage->material_get_param(p_material_rid, "uv1_scale");
	if (uv1_scale_var.get_type() == Variant::VECTOR3) {
		Vector3 s = uv1_scale_var;
		mat.uv1_scale[0] = s.x;
		mat.uv1_scale[1] = s.y;
	}

	Variant uv1_offset_var = material_storage->material_get_param(p_material_rid, "uv1_offset");
	if (uv1_offset_var.get_type() == Variant::VECTOR3) {
		Vector3 o = uv1_offset_var;
		mat.uv1_offset[0] = o.x;
		mat.uv1_offset[1] = o.y;
	}

	// Point filtering: check if material requests nearest filtering (e.g. pixel art).
	// BaseMaterial3D exposes this as "texture_filter" int param (0=nearest, 1=linear, etc.).
	Variant filter_var = material_storage->material_get_param(p_material_rid, "texture_filter");
	if (filter_var.get_type() == Variant::INT) {
		int filter_mode = filter_var;
		// 0 = TEXTURE_FILTER_NEAREST, 2 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		// 4 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC
		if (filter_mode == 0 || filter_mode == 2 || filter_mode == 4) {
			mat.flags |= RT_MAT_FLAG_POINT_FILTER;
		}
	}

	// Update cache entry
	mat_data->hit_shader = hit_shader;
	entry->cached_counter = p_material_invalidation_counter;
	entry->cached_rid_version = mat_version;
	entry->cached_shader_hash = shader_hash;
	entry->cached_shader_hash_b = shader_hash_b;
	entry->cached_content_generation = content_generation;
	entry->last_used_frame = current_frame;

	if (mat_data->uniform_buffer.is_valid()) {
		geometry_buffer_dependencies.insert(mat_data->uniform_buffer);
	}
	_update_persistent_material(p_material_rid, mat_data, content_generation);
	return mat_data;
}

// ---------------------------------------------------------------------------
// Acceleration structure building
// ---------------------------------------------------------------------------

void RenderRaytracing::_retire_micro_geometry(RTMicroGeometryBuild *p_build) {
	if (p_build) {
		p_build->retirement = RD::get_singleton()->get_pending_submission_serial();
		retired_micro_geometry.push_back(p_build);
	}
}

void RenderRaytracing::_free_micro_geometry(RTMicroGeometryBuild *p_build) {
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	for (const auto &pin : p_build->pins) {
		storage->unpin_group(pin.asset, pin.group);
	}
	for (const auto &pin : p_build->candidate_pins) {
		storage->unpin_group(pin.asset, pin.group);
	}
	for (const auto &retired : p_build->retired_pins) {
		for (const auto &pin : retired.pins) {
			storage->unpin_group(pin.asset, pin.group);
		}
	}
	for (const auto &pin : p_build->finest_pins) {
		storage->unpin_group(pin.asset, pin.group);
	}
	for (RID blas : p_build->blas) {
		if (RD::get_singleton()->acceleration_structure_is_valid(blas)) {
			RD::get_singleton()->free_rid(blas);
		}
	}
	for (RID resource : p_build->resources) {
		RD::get_singleton()->free_rid(resource);
	}
	if (p_build->selection) {
		memdelete(p_build->selection);
	}
	for (RID asset : p_build->assets) {
		storage->release(asset);
	}
	memdelete(p_build);
}

void RenderRaytracing::_micro_group_feedback(const Vector<uint8_t> &p_bytes, uint64_t p_feedback) {
	RTMicroGeometryFeedback *feedback = reinterpret_cast<RTMicroGeometryFeedback *>(p_feedback);
	RTMicroGeometryBuild *build = feedback->build;
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	Vector<RTMicroGeometryPin> &pins = feedback->published ? build->pins : build->candidate_pins;
	if (p_bytes.size() < int64_t(build->selection->data.group_work) * 4) {
		pins.append_array(feedback->pins);
		build->pending_feedback--;
		memdelete(feedback);
		return;
	}
	Vector<RTMicroGeometryPin> used;
	for (const auto &task : build->selection_tasks) {
		RID asset = RID::from_uint64(task.asset);
		for (uint32_t group = 0; group < task.group_count; group++) {
			bool selected = false;
			for (uint32_t ordinal = 0; ordinal < task.multimesh_count; ordinal++) {
				uint64_t offset = uint64_t(task.group_offset + ordinal * task.group_count + group) * 4;
				selected |= offset + 4 <= uint64_t(p_bytes.size()) && decode_uint32(p_bytes.ptr() + offset) != 0;
			}
			RTMicroGeometryPin pin = { asset, group };
			if (selected && !used.has(pin)) {
				storage->pin_group(asset, group);
				used.push_back(pin);
			}
		}
	}
	if (feedback->cut_generation == build->cut_generation) {
		for (const auto &pin : pins) {
			storage->unpin_group(pin.asset, pin.group);
		}
		pins = used;
	} else {
		for (const auto &pin : used) {
			storage->unpin_group(pin.asset, pin.group);
		}
	}
	for (const auto &pin : feedback->pins) {
		storage->unpin_group(pin.asset, pin.group);
	}
	build->pending_feedback--;
	memdelete(feedback);
}

void RenderRaytracing::_micro_cut_feedback(const Vector<uint8_t> &p_bytes, uint64_t p_build, uint64_t p_generation, bool p_restore) {
	auto *build = reinterpret_cast<RTMicroGeometryBuild *>(p_build);
	if (p_restore) {
		if (p_bytes.size() >= 4) {
			build->completed_builds += decode_uint32(p_bytes.ptr());
		}
	} else if (build->cut_generation == p_generation) {
		if (build->candidate_pending) {
			build->candidate_changed = p_bytes.size() < 8 || decode_uint32(p_bytes.ptr() + 4) != 0;
		}
		if (p_bytes.size() >= 16) {
			build->candidate_builds = decode_uint32(p_bytes.ptr());
			build->candidate_clusters = decode_uint32(p_bytes.ptr() + 8);
			build->candidate_triangles = decode_uint32(p_bytes.ptr() + 12);
			if (!build->candidate_pending) {
				build->selected_clusters = build->candidate_clusters;
				build->selected_triangles = build->candidate_triangles;
				build->completed_builds += build->candidate_builds;
			}
		}
	}
	build->pending_feedback--;
}

bool RenderRaytracing::_prepare_micro_geometry(RTViewportState *p_state, const RenderDataRD *p_render_data, const Vector<MicroGeometrySelection::Task> &p_tasks, const Vector<RTMicroGeometryTask> &p_rt_tasks, uint32_t p_levels) {
	RD *rd = RD::get_singleton();
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	if (p_tasks.is_empty()) {
		_retire_micro_geometry(p_state->micro_geometry);
		p_state->micro_geometry = nullptr;
		return true;
	}
	uint64_t signature = _rt_scene_hash(p_tasks.ptr(), p_tasks.size() * sizeof(MicroGeometrySelection::Task), blass.size());
	signature = _rt_scene_hash(&p_state->settings.mode_generation, sizeof(p_state->settings.mode_generation), signature);
	const uint64_t environment = p_state->settings_environment.get_id();
	signature = _rt_scene_hash(&environment, sizeof(environment), signature);
	signature = _rt_scene_hash(&p_state->settings.geometry_error, sizeof(p_state->settings.geometry_error), signature);
	signature = _rt_scene_hash(&p_state->settings.geometry_offscreen_multiplier, sizeof(p_state->settings.geometry_offscreen_multiplier), signature);
	signature = _rt_scene_hash(&p_state->settings_visible_layers, sizeof(p_state->settings_visible_layers), signature);
	for (const auto &task : p_tasks) {
		const auto &surface = persistent_surfaces[uint32_t(task.surface) - 1].data;
		signature = _rt_scene_hash(&surface.material, sizeof(surface.material), signature);
		signature = _rt_scene_hash(&surface.material_generation, sizeof(surface.material_generation), signature);
	}
	for (auto task : p_rt_tasks) {
		task.motion_base = 0;
		signature = _rt_scene_hash(&task, sizeof(task), signature);
	}
	bool valid = p_state->micro_geometry && p_state->micro_geometry->signature == signature;
	if (valid) {
		for (RID blas : p_state->micro_geometry->blas) {
			valid &= rd->acceleration_structure_is_valid(blas);
		}
	}
	RTMicroGeometryBuild *previous = nullptr;
	if (!valid) {
		previous = p_state->micro_geometry;
		p_state->micro_geometry = nullptr;
		if (!micro_selection) {
			micro_selection = memnew(MicroGeometrySelection);
			Vector<String> modes;
			modes.push_back("#define PHASE_RESET\n");
			modes.push_back("#define PHASE_SCATTER\n");
			modes.push_back("#define PHASE_COMPARE\n");
			modes.push_back("#define PHASE_SCAN\n");
			modes.push_back("#define PHASE_ADD_OFFSETS\n");
			modes.push_back("#define PHASE_EMIT\n");
			modes.push_back("#define PHASE_FINALIZE\n");
			micro_rt_shader.initialize(modes);
			micro_rt_version = micro_rt_shader.version_create();
			for (uint32_t phase = 0; phase < MICRO_RT_PHASE_COUNT; phase++) {
				micro_rt_pipelines[phase] = rd->compute_pipeline_create(micro_rt_shader.version_get_shader(micro_rt_version, phase));
			}
		}
		RTMicroGeometryBuild *build = memnew(RTMicroGeometryBuild);
		build->signature = signature;
		build->selection_tasks = p_tasks;
		build->task_data = p_rt_tasks;
		uint64_t cluster_work = 0, group_work = 0, coarse_work = 0;
		uint32_t offset = 0;
		Vector<MicroGeometrySelection::Bin> bins;
		for (const auto &task : p_tasks) {
			cluster_work += uint64_t(task.cluster_count) * task.multimesh_count;
			group_work += uint64_t(task.group_count) * task.multimesh_count;
			coarse_work += uint64_t(task.coarse_count) * task.multimesh_count;
			uint32_t capacity = uint64_t(task.coarse_count) * task.multimesh_count + MIN(uint64_t(MicroGeometrySelection::EXTRA_SELECTED_CLUSTERS) / p_tasks.size(), uint64_t(task.cluster_count - task.coarse_count) * task.multimesh_count);
			bins.push_back({ offset, capacity });
			offset += capacity;
			build->input.max_acceleration_structure_count += task.multimesh_count;
			build->input.max_cluster_count_per_acceleration_structure = MAX(build->input.max_cluster_count_per_acceleration_structure, task.cluster_count);
			RID asset = RID::from_uint64(task.asset);
			if (!build->assets.has(asset)) {
				storage->acquire(storage->get_source(asset));
				build->assets.push_back(asset);
			}
			const auto &surface = persistent_surfaces[uint32_t(task.surface) - 1].data;
			if (surface.force_finest != 0) {
				const auto &metadata = storage->get_source(asset)->get_metadata();
				for (const auto &cluster : metadata.clusters) {
					RTMicroGeometryPin pin = { asset, cluster.group };
					if (cluster.refined_group == UINT32_MAX && metadata.surfaces[cluster.surface].source_surface == surface.source_surface && !build->finest_pins.has(pin)) {
						storage->pin_group(pin.asset, pin.group);
						build->finest_pins.push_back(pin);
					}
				}
			}
		}
		uint64_t tile_work = 0;
		uint64_t scan_work = 0;
		uint32_t max_tiles = 0;
		for (auto &task : build->task_data) {
			uint32_t tiles = (task.cluster_count + 255u) / 256u;
			task.tile_offset = tile_work;
			task.scan_offset = scan_work;
			for (uint32_t elements = tiles; elements != 0;) {
				task.scan_stride += elements;
				elements = elements > 256u ? (elements + 255u) / 256u : 0u;
			}
			tile_work += uint64_t(tiles) * task.instance_count;
			scan_work += uint64_t(task.scan_stride) * task.instance_count;
			max_tiles = MAX(max_tiles, tiles);
		}
		if (MAX(cluster_work, group_work) > UINT32_MAX / 8 || scan_work > UINT32_MAX / 4 || uint64_t(blass.size()) * sizeof(RD::AccelerationStructureGPUInstance) > UINT32_MAX) {
			_retire_micro_geometry(previous);
			_retire_micro_geometry(build);
			ERR_FAIL_V_MSG(false, "RT microgeometry preparation exceeds the device buffer range.");
		}
		build->tile_work = tile_work;
		build->selected_work = offset;
		Vector<RTMicroGeometryScanTask> scan_tasks;
		Vector<uint32_t> scan_offsets;
		scan_offsets.resize_initialized(p_tasks.size());
		for (uint64_t divisor = 1; divisor == 1 || divisor < max_tiles; divisor *= 256) {
			RTMicroGeometryBuild::ScanLevel level;
			level.task_offset = scan_tasks.size();
			for (uint32_t index = 0; index < uint32_t(build->task_data.size()); index++) {
				const auto &task = build->task_data[index];
				uint32_t tiles = (task.cluster_count + 255u) / 256u;
				if (divisor > 1 && tiles <= divisor) {
					continue;
				}
				RTMicroGeometryScanTask scan;
				scan.task = index;
				scan.group_offset = level.group_count;
				scan.element_count = (uint64_t(tiles) + divisor - 1) / divisor;
				scan.input_offset = scan_offsets[index];
				scan.output_offset = scan.input_offset + scan.element_count;
				scan_offsets.write[index] = scan.output_offset;
				level.group_count += ((scan.element_count + 255u) / 256u) * task.instance_count;
				scan_tasks.push_back(scan);
				level.task_count++;
			}
			build->scan_levels.push_back(level);
		}
		build->input.max_total_cluster_count = cluster_work;
		MicroGeometrySelection::Parameters parameters;
		parameters.group_work = group_work;
		parameters.cluster_work = cluster_work;
		parameters.coarse_work = coarse_work;
		get_persistent_buffer_dependencies(build->dependencies);
		for (RID asset : build->assets) {
			storage->get_dependencies(asset, build->dependencies);
		}
		build->selection = micro_selection->create(p_tasks, bins, parameters, p_levels, sizeof(RenderForwardClustered::SceneState::InstanceData), persistent_instance_buffer, persistent_surface_buffer, build->dependencies);
		auto allocate = [&](uint64_t p_size, const void *p_data = nullptr) {
			p_size = MAX(p_size, uint64_t(16));
			if (p_size > UINT32_MAX) {
				return RID();
			}
			Span<uint8_t> bytes;
			if (p_data) {
				bytes = { (const uint8_t *)p_data, p_size };
			}
			RID buffer = rd->storage_buffer_create(p_size, bytes, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
			if (buffer.is_valid()) {
				build->resources.push_back(buffer);
				build->memory_bytes += p_size;
			}
			return buffer;
		};
		build->tasks = allocate(uint64_t(build->task_data.size()) * sizeof(RTMicroGeometryTask), build->task_data.ptr());
		build->blas_addresses = allocate(uint64_t(blass.size()) * 8);
		build->membership = allocate(cluster_work * 4);
		build->cached_membership = allocate(cluster_work * 4);
		build->candidate_state = allocate(uint64_t(blass.size()) * 8);
		build->committed_state = allocate(uint64_t(blass.size()) * 8);
		build->scan_data = allocate(scan_work * 4);
		build->scan_tasks = allocate(uint64_t(scan_tasks.size()) * sizeof(RTMicroGeometryScanTask), scan_tasks.ptr());
		build->references = allocate(cluster_work * 8);
		build->dirty_infos = allocate(uint64_t(build->input.max_acceleration_structure_count) * sizeof(RD::ClusterBottomLevelBuildInfo));
		build->dirty_destinations = allocate(uint64_t(build->input.max_acceleration_structure_count) * 8);
		build->dirty_counts = allocate(16);
		build->tlas_instances = allocate(uint64_t(blass.size()) * sizeof(RD::AccelerationStructureGPUInstance));
		build->group_usage = allocate(group_work * 4);
		RD::ClusterBuildSizes sizes;
		rd->blas_get_cluster_build_sizes(build->input, sizes);
		build->scratch = allocate(sizes.build_scratch_size);
		build->as_memory_bytes += MAX(sizes.build_scratch_size, uint64_t(16));
		bool complete = build->selection != nullptr && sizes.build_scratch_size != 0;
		for (RID pipeline : micro_rt_pipelines) {
			complete &= pipeline.is_valid();
		}
		for (RID resource : { build->tasks, build->blas_addresses, build->membership, build->cached_membership, build->references, build->dirty_infos, build->dirty_destinations, build->dirty_counts, build->tlas_instances, build->group_usage, build->scratch, build->candidate_state, build->committed_state, build->scan_data, build->scan_tasks }) {
			complete &= resource.is_valid();
		}
		if (complete) {
			build->memory_bytes += build->selection->memory_bytes;
			rd->buffer_clear(build->cached_membership, 0, MAX(uint32_t(cluster_work * 4), 16u));
			rd->buffer_clear(build->committed_state, 0, MAX(blass.size() * 8, 16u));
			if (previous && previous->signature == signature && previous->has_committed_cut) {
				rd->buffer_copy(previous->cached_membership, build->cached_membership, 0, 0, cluster_work * 4);
				rd->buffer_copy(previous->committed_state, build->committed_state, 0, 0, blass.size() * 8);
				build->restore_committed_cut = true;
				build->selected_clusters = previous->selected_clusters;
				build->selected_triangles = previous->selected_triangles;
				build->completed_builds = previous->completed_builds;
				for (RID asset : build->assets) {
					const auto &metadata = storage->get_source(asset)->get_metadata();
					for (uint32_t group = 0; group < uint32_t(metadata.groups.size()); group++) {
						if (storage->is_group_ready(asset, group, true)) {
							storage->pin_group(asset, group);
							build->pins.push_back({ asset, group });
						}
					}
				}
			}
			for (uint32_t index = 0; index < build->input.max_acceleration_structure_count; index++) {
				RID blas = rd->blas_create_from_clusters(build->input.max_cluster_count_per_acceleration_structure, build->input.max_cluster_count_per_acceleration_structure);
				complete &= blas.is_valid();
				build->blas.push_back(blas);
				if (blas.is_valid()) {
					const uint64_t bytes = rd->acceleration_structure_get_memory_usage(blas);
					build->memory_bytes += bytes;
					build->as_memory_bytes += bytes;
				}
			}
		}
		_retire_micro_geometry(previous);
		if (!complete) {
			_retire_micro_geometry(build);
			ERR_FAIL_V_MSG(false, "Unable to allocate the resident RT microgeometry cut.");
		}
		p_state->micro_geometry = build;
	}
	RTMicroGeometryBuild *build = p_state->micro_geometry;
	build->frozen = p_render_data->render_buffers->is_micro_geometry_debug_freeze();
	for (uint32_t index = 0; index < uint32_t(p_rt_tasks.size()); index++) {
		build->task_data.write[index].motion_base = p_rt_tasks[index].motion_base;
	}
	uint32_t blas_index = 0;
	for (const auto &task : p_rt_tasks) {
		for (uint32_t ordinal = 0; ordinal < task.instance_count; ordinal++) {
			blass[task.geometry_base + ordinal] = build->blas[blas_index++];
		}
	}
	auto &parameters = build->selection->data;
	parameters.flags = 1 | 16 | 32;
	if (p_render_data->scene_data->view_count == 1) {
		parameters.flags |= 2;
	}
	if (parameters.group_work > MicroGeometrySelection::MAX_WORK_ITEMS || parameters.cluster_work > MicroGeometrySelection::MAX_WORK_ITEMS) {
		parameters.flags |= 8;
	}
	parameters.scenario = persistent_instances[uint32_t(p_tasks[0].instance) - 1].data.scenario;
	parameters.layer_mask = p_render_data->scene_data->camera_visible_layers;
	parameters.error = p_state->settings.geometry_error;
	parameters.offscreen_multiplier = p_state->settings.geometry_offscreen_multiplier;
	parameters.output_height = p_render_data->render_buffers->get_target_size().y;
	parameters.near_plane = p_render_data->scene_data->cam_projection.get_z_near();
	Projection correction;
	correction.set_depth_correction(p_render_data->scene_data->flip_y);
	RendererRD::MaterialStorage::store_camera(correction * p_render_data->scene_data->cam_projection, parameters.projection);
	RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(p_state->camera_transform.basis.inverse(), Vector3()), parameters.view_rotation);
	for (uint32_t axis = 0; axis < 3; axis++) {
#ifdef REAL_T_IS_DOUBLE
		RendererRD::MaterialStorage::split_double(p_state->camera_transform.origin[axis], &parameters.camera[axis], &parameters.camera_low[axis]);
#else
		parameters.camera[axis] = p_state->camera_transform.origin[axis];
#endif
	}
	build->selection->persistent_instances = persistent_instance_buffer;
	build->selection->persistent_surfaces = persistent_surface_buffer;
	RENDER_TIMESTAMP("Microgeometry RT Dependencies");
	build->dependencies.clear();
	build->clas_dependencies.clear();
	get_persistent_buffer_dependencies(build->dependencies);
	for (RID asset : build->assets) {
		storage->get_dependencies(asset, build->dependencies);
		storage->get_clas_dependencies(asset, build->clas_dependencies);
	}
	for (RID buffer : geometry_buffer_dependencies) {
		build->dependencies.push_back(buffer);
	}
	for (RID buffer : build->dependencies) {
		geometry_buffer_dependencies.insert(buffer);
	}
	build->selection->dependencies = build->dependencies;
	return true;
}

bool RenderRaytracing::_build_micro_geometry(RTViewportState *p_state) {
	RENDER_TIMESTAMP("Microgeometry RT Retire Pins");
	RTMicroGeometryBuild *build = p_state->micro_geometry;
	RD *rd = RD::get_singleton();
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	for (uint32_t index = 0; index < uint32_t(build->retired_pins.size());) {
		if (build->retired_pins[index].submission <= rd->get_completed_submission_serial()) {
			for (const auto &pin : build->retired_pins[index].pins) {
				storage->unpin_group(pin.asset, pin.group);
			}
			build->retired_pins.remove_at(index);
		} else {
			index++;
		}
	}
	RTMicroGeometryBuild::Mode mode = RTMicroGeometryBuild::UPDATE_TRANSFORMS;
	if (build->restore_committed_cut) {
		mode = RTMicroGeometryBuild::RESTORE_CUT;
	} else if (!build->has_committed_cut) {
		mode = RTMicroGeometryBuild::INITIAL_CUT;
	} else if (!build->frozen && build->pending_feedback == 0) {
		mode = build->candidate_pending ? RTMicroGeometryBuild::PUBLISH_CUT : RTMicroGeometryBuild::PREPARE_CUT;
	}
	const bool select = mode == RTMicroGeometryBuild::PREPARE_CUT || mode == RTMicroGeometryBuild::INITIAL_CUT;
	if (mode == RTMicroGeometryBuild::INITIAL_CUT || (mode == RTMicroGeometryBuild::PUBLISH_CUT && build->candidate_changed)) {
		p_state->scene_generation++;
		p_state->camera_history_epoch++;
		p_state->pathtracing_history_epoch++;
		p_state->ddgi_history_epoch++;
		p_state->light_history_valid = false;
	}
	RTMicroGeometryFeedback *feedback = nullptr;
	if (select) {
		RENDER_TIMESTAMP("Microgeometry RT Residency Pinning");
		feedback = memnew(RTMicroGeometryFeedback);
		feedback->build = build;
		feedback->cut_generation = ++build->cut_generation;
		feedback->published = mode == RTMicroGeometryBuild::INITIAL_CUT;
		for (RID asset : build->assets) {
			const auto &metadata = storage->get_source(asset)->get_metadata();
			for (uint32_t group = 0; group < uint32_t(metadata.groups.size()); group++) {
				if (storage->is_group_ready(asset, group, true)) {
					storage->pin_group(asset, group);
					feedback->pins.push_back({ asset, group });
				}
			}
		}
		micro_selection->select(build->selection, RID());
		micro_selection->submit_feedback(build->selection);
	}
	RENDER_TIMESTAMP("Microgeometry RT Instance Upload");
	LocalVector<RD::AccelerationStructureGPUInstance> instances;
	LocalVector<uint64_t> addresses;
	instances.resize(blass.size());
	addresses.resize(blass.size());
	Vector<RID> dependencies;
	for (uint32_t index = 0; index < blass.size(); index++) {
		Transform3D transform = blas_transforms[index];
		transform.origin -= p_state->rt_origin;
		RendererRD::MaterialStorage::store_transform_transposed_3x4(transform, instances[index].transform);
		instances[index].custom_index_and_mask = index | (uint32_t(instance_masks[index]) << 24);
		instances[index].sbt_offset_and_flags = index | (instance_flags[index] << 24);
		addresses[index] = rd->acceleration_structure_get_device_address(blass[index]);
		instances[index].acceleration_structure_reference = addresses[index];
		if (blass[index].is_valid() && !dependencies.has(blass[index])) {
			dependencies.push_back(blass[index]);
		}
	}
	rd->buffer_update(build->blas_addresses, 0, addresses.size() * 8, addresses.ptr());
	rd->buffer_update(build->tlas_instances, 0, instances.size() * sizeof(RD::AccelerationStructureGPUInstance), instances.ptr());
	for (auto &task : build->task_data) {
		task.mode = mode;
	}
	rd->buffer_update(build->tasks, 0, build->task_data.size() * sizeof(RTMicroGeometryTask), build->task_data.ptr());
	if (select || mode == RTMicroGeometryBuild::RESTORE_CUT) {
		rd->buffer_clear(build->dirty_counts, 0, 16);
	}
	if (select) {
		rd->buffer_clear(build->group_usage, 0, MAX(build->selection->data.group_work * 4, 16u));
	}
	LocalVector<RD::Uniform> uniforms;
	uint32_t binding = 0;
	for (RID buffer : { build->tasks, build->selection->selected, build->selection->bins, build->selection->counts, persistent_instance_buffer, build->blas_addresses, build->membership, build->cached_membership, build->references, build->dirty_infos, build->dirty_destinations, build->dirty_counts, build->tlas_instances, p_state->motion_transform_buffer, build->group_usage, build->candidate_state, build->committed_state, build->scan_data, build->scan_tasks }) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding++, buffer));
	}
	struct Parameters {
		float origin[4] = {};
		float origin_low[4] = {};
		uint32_t task_count = 0;
		uint32_t geometry_count = 0;
		uint64_t references = 0;
		uint32_t work_count = 0;
		uint32_t scan_task_offset = 0;
		uint32_t scan_task_count = 0;
		uint32_t padding = 0;
	} parameters;
	static_assert(sizeof(Parameters) == 64);
	for (uint32_t axis = 0; axis < 3; axis++) {
#ifdef REAL_T_IS_DOUBLE
		RendererRD::MaterialStorage::split_double(p_state->rt_origin[axis], &parameters.origin[axis], &parameters.origin_low[axis]);
#else
		parameters.origin[axis] = p_state->rt_origin[axis];
#endif
	}
	parameters.task_count = build->task_data.size();
	parameters.geometry_count = blass.size();
	parameters.references = rd->buffer_get_device_address(build->references);
	static const char *const mode_timestamps[] = {
		"Microgeometry RT Prepare Cut",
		"Microgeometry RT Publish Cut",
		"Microgeometry RT Update Transforms",
		"Microgeometry RT Restore Cut",
		"Microgeometry RT Initial Cut",
	};
	RENDER_TIMESTAMP(mode_timestamps[mode]);
	auto dispatch = [&](MicroRTPhase p_phase, uint32_t p_work_count, bool p_groups) {
		if (p_work_count == 0) {
			return;
		}
		parameters.work_count = p_work_count;
		RID shader = micro_rt_shader.version_get_shader(micro_rt_version, p_phase);
		RID uniform = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, micro_rt_pipelines[p_phase]);
		rd->compute_list_bind_uniform_set(list, uniform, 0);
		rd->compute_list_set_push_constant(list, &parameters, sizeof(parameters));
		for (RID buffer : build->dependencies) {
			rd->compute_list_add_buffer_dependency(list, buffer);
		}
		uint32_t groups = p_groups ? p_work_count : (p_work_count + 255u) / 256u;
		rd->compute_list_dispatch(list, MIN(groups, 65535u), (groups + 65534u) / 65535u, 1);
		rd->compute_list_end();
	};
	if (select || mode == RTMicroGeometryBuild::RESTORE_CUT) {
		dispatch(MICRO_RT_RESET, build->tile_work, true);
		if (select) {
			dispatch(MICRO_RT_SCATTER, build->selected_work, false);
		}
		dispatch(MICRO_RT_COMPARE, build->tile_work, true);
		for (const auto &level : build->scan_levels) {
			parameters.scan_task_offset = level.task_offset;
			parameters.scan_task_count = level.task_count;
			dispatch(MICRO_RT_SCAN, level.group_count, true);
		}
		for (int32_t index = build->scan_levels.size() - 2; index >= 0; index--) {
			const auto &level = build->scan_levels[index];
			parameters.scan_task_offset = level.task_offset;
			parameters.scan_task_count = level.task_count;
			dispatch(MICRO_RT_ADD_OFFSETS, level.group_count, true);
		}
		dispatch(MICRO_RT_EMIT, build->tile_work, true);
	}
	dispatch(MICRO_RT_FINALIZE, blass.size(), false);
	Vector<RID> address_dependencies = build->dependencies;
	address_dependencies.push_back(build->references);
	RD::ClusterAddressRegion destinations = { build->dirty_destinations, 0, 8, uint64_t(build->input.max_acceleration_structure_count) * 8 };
	RD::ClusterAddressRegion infos = { build->dirty_infos, 0, sizeof(RD::ClusterBottomLevelBuildInfo), uint64_t(build->input.max_acceleration_structure_count) * sizeof(RD::ClusterBottomLevelBuildInfo) };
	RD::ClusterAddressRegion count = { build->dirty_counts, 0, 4, 4 };
	Error error = OK;
	if (mode == RTMicroGeometryBuild::PUBLISH_CUT || mode == RTMicroGeometryBuild::RESTORE_CUT || mode == RTMicroGeometryBuild::INITIAL_CUT) {
		RENDER_TIMESTAMP("Microgeometry RT Cluster BLAS Build");
		error = rd->blas_build_from_clusters(build->input, build->blas, destinations, infos, count, build->scratch, address_dependencies, build->clas_dependencies);
		if (error == OK) {
			SWAP(build->membership, build->cached_membership);
			SWAP(build->candidate_state, build->committed_state);
			build->has_committed_cut = true;
			build->restore_committed_cut = false;
			if (mode == RTMicroGeometryBuild::PUBLISH_CUT) {
				if (!build->pins.is_empty()) {
					RTMicroGeometryBuild::RetiredPins retired;
					retired.pins = build->pins;
					retired.submission = rd->get_pending_submission_serial();
					build->retired_pins.push_back(retired);
				}
				build->pins = build->candidate_pins;
				build->candidate_pins.clear();
				build->candidate_pending = false;
				build->selected_clusters = build->candidate_clusters;
				build->selected_triangles = build->candidate_triangles;
				build->completed_builds += build->candidate_builds;
			}
		}
	}
	RENDER_TIMESTAMP("Microgeometry RT Cut Readbacks");
	if (select) {
		build->pending_feedback += 2;
		build->candidate_pending = mode == RTMicroGeometryBuild::PREPARE_CUT;
		build->candidate_changed = false;
		if (rd->buffer_get_data_async(build->group_usage, callable_mp_static(&RenderRaytracing::_micro_group_feedback).bind(uint64_t(feedback))) != OK) {
			_micro_group_feedback(Vector<uint8_t>(), uint64_t(feedback));
		}
		if (rd->buffer_get_data_async(build->dirty_counts, callable_mp_static(&RenderRaytracing::_micro_cut_feedback).bind(uint64_t(build), build->cut_generation, false)) != OK) {
			_micro_cut_feedback(Vector<uint8_t>(), uint64_t(build), build->cut_generation, false);
		}
	} else if (mode == RTMicroGeometryBuild::RESTORE_CUT && error == OK) {
		build->pending_feedback++;
		if (rd->buffer_get_data_async(build->dirty_counts, callable_mp_static(&RenderRaytracing::_micro_cut_feedback).bind(uint64_t(build), build->cut_generation, true)) != OK) {
			_micro_cut_feedback(Vector<uint8_t>(), uint64_t(build), build->cut_generation, true);
		}
	}
	ERR_FAIL_COND_V(error != OK, false);
	RENDER_TIMESTAMP("Microgeometry RT TLAS Build");
	const bool success = rd->tlas_build_from_buffer(p_state->tlas, build->tlas_instances, 0, blass.size(), dependencies) == OK;
	RENDER_TIMESTAMP("Microgeometry RT AS Complete");
	return success;
}

bool RenderRaytracing::build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list) {
	RENDER_TIMESTAMP("BLAS Build");

	for (const RID &blas_rid : p_dirty_blas_list) {
		if (blas_rid.is_valid()) {
			RD::get_singleton()->blas_build(blas_rid);
		}
	}

	for (const RID &blas_rid : p_dirty_blas_update_list) {
		if (blas_rid.is_valid()) {
			RD::get_singleton()->blas_update(blas_rid);
		}
	}

	RENDER_TIMESTAMP("TLAS Build");

	uint32_t needed = MAX(blass.size(), (uint32_t)1);
	if (!p_state->tlas.is_valid() || needed > p_state->tlas_max_instances) {
		if (p_state->tlas.is_valid()) {
			if (p_state->micro_geometry) {
				retired_tlas_memory.push_back({ RD::get_singleton()->acceleration_structure_get_memory_usage(p_state->tlas), RD::get_singleton()->get_pending_submission_serial() });
			}
			RD::get_singleton()->free_rid(p_state->tlas);
		}
		p_state->tlas_max_instances = needed * 2;
		p_state->tlas = RD::get_singleton()->tlas_create(p_state->tlas_max_instances, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(p_state->tlas, "RT TLAS");
	}

	if (p_state->micro_geometry) {
		return _build_micro_geometry(p_state);
	}

	LocalVector<RD::AccelerationStructureInstance> instances;
	instances.resize(blass.size());
	for (uint32_t i = 0; i < blass.size(); i++) {
		RD::AccelerationStructureInstance &inst = instances[i];
		inst.id = i;
		inst.transform = blas_transforms[i];
		inst.transform.origin -= p_state->rt_origin;
		inst.blas = blass[i];
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(instance_flags[i]);
		inst.mask = (i < instance_masks.size()) ? instance_masks[i] : 0xFF;
		inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | i);
	}

	return RD::get_singleton()->tlas_build(p_state->tlas, instances) == OK;
}

void RenderRaytracing::finalize_buffers(RTViewportState *p_state) {
	// Grow-only uploads. Callers must not free these in prepare_frame().
	auto update_or_grow = [](RID &p_buffer, uint32_t &p_capacity, const void *p_data, uint32_t p_size) {
		if (p_size == 0) {
			return;
		}
		if (!p_buffer.is_valid() || p_size > p_capacity) {
			if (p_buffer.is_valid()) {
				RD::get_singleton()->free_rid(p_buffer);
			}
			p_capacity = p_size;
			Vector<uint8_t> init;
			init.resize(p_size);
			memcpy(init.ptrw(), p_data, p_size);
			p_buffer = RD::get_singleton()->storage_buffer_create(p_size, init);
		} else {
			RD::get_singleton()->buffer_update(p_buffer, 0, p_size, p_data);
		}
	};

	RT_GeometryData empty_geometry = {};
	RT_MaterialData empty_material = {};
	const int32_t empty_motion_index = -1;
	const RT_InstanceMotionData empty_motion_transform = {};
	update_or_grow(p_state->geometry_buffer, p_state->geometry_buffer_capacity,
			geometry_data.is_empty() ? &empty_geometry : geometry_data.ptr(), MAX(geometry_data.size(), 1u) * sizeof(RT_GeometryData));
	update_or_grow(p_state->material_buffer, p_state->material_buffer_capacity,
			material_data.is_empty() ? &empty_material : material_data.ptr(), MAX(material_data.size(), 1u) * sizeof(RT_MaterialData));
	update_or_grow(p_state->motion_index_buffer, p_state->motion_index_buffer_capacity,
			motion_indices.is_empty() ? &empty_motion_index : motion_indices.ptr(), MAX(motion_indices.size(), 1u) * sizeof(int32_t));
	update_or_grow(p_state->motion_transform_buffer, p_state->motion_transform_buffer_capacity,
			motion_transforms.is_empty() ? &empty_motion_transform : motion_transforms.ptr(), MAX(motion_transforms.size(), 1u) * sizeof(RT_InstanceMotionData));
}

// ---------------------------------------------------------------------------
// Merged MultiMesh BLAS builder
// ---------------------------------------------------------------------------

bool RenderRaytracing::_build_merged_mm_blas(
		RID p_mm_rid,
		RID p_mm_gpu_buffer,
		void *p_mesh_surface,
		uint32_t p_mm_count,
		uint32_t p_surface_index,
		uint32_t p_surface_counter,
		RD::ComputeListID p_compute_list,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list,
		RTSurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	bool indexed = index_buffer.is_valid() && index_count > 0;
	uint32_t prim_count = indexed ? (index_count / 3) : (vertex_count / 3);

	// Skip compressed meshes: their positions are UNORM16x4, not float3.
	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	if (surface_format & (RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES | RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
		return false;
	}

	if (prim_count == 0 || vertex_count == 0) {
		return false;
	}
	static const uint32_t MM_MERGED_BLAS_MAX_TRIANGLES = (uint32_t)GLOBAL_GET("rendering/raytracing/multimesh_merged_blas_max_triangles");
	if ((uint64_t)p_mm_count * prim_count > MM_MERGED_BLAS_MAX_TRIANGLES) {
		return false; // Too large; fall back to expanded TLAS.
	}

	// Side table -> pool: dense direct index keyed by RID local_index, with a
	// validator stamp to detect RID local-index recycling for a different MM.
	const uint32_t li = p_mm_rid.get_local_index();
	const uint32_t mm_validator = static_cast<uint32_t>(p_mm_rid.get_id() >> 32);
	if (li >= mm_handles.size()) {
		mm_handles.resize(li + 1);
	}
	MMSurfaceHandles &mm_entry = mm_handles[li];
	if (mm_entry.mm_validator != mm_validator) {
		// Local index has been recycled (or first-ever access). Release any slots
		// the previous owner left behind so we don't leak until TTL.
		RD *rd_local = RD::get_singleton();
		for (uint32_t s = 0; s < mm_entry.per_surface.size(); s++) {
			RID &h = mm_entry.per_surface[s];
			RTMergedMMEntry *old = merged_mm_pool.get_or_null(h);
			if (!old) {
				h = RID();
				continue;
			}
			if (old->blas.is_valid()) {
				rd_local->free_rid(old->blas);
			}
			if (old->previous_position_buffer.is_valid()) {
				rd_local->free_rid(old->previous_position_buffer);
				old->previous_position_buffer = RID();
			}
			if (old->merged_vtx_buffer.is_valid()) {
				rd_local->free_rid(old->merged_vtx_buffer);
			}
			if (old->merged_attr_buffer.is_valid()) {
				rd_local->free_rid(old->merged_attr_buffer);
			}
			if (old->replicated_idx_buffer.is_valid()) {
				rd_local->free_rid(old->replicated_idx_buffer);
			}
			merged_mm_pool.free(h);
			h = RID();
		}
		mm_entry.per_surface.clear();
		mm_entry.mm_validator = mm_validator;
	}

	RTMergedMMEntry *entry_ptr = _access_merged_mm_slot(mm_entry.surface_handle(p_surface_index));
	ERR_FAIL_NULL_V(entry_ptr, false);
	RTMergedMMEntry &entry = *entry_ptr;
	const uint32_t cache_key = (li << 8) | (p_surface_index & 0xFF); // Used only for resource naming below.

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	// First viewport this frame does the merge dispatch + BLAS refit; later
	// viewports referencing the same MultiMesh-surface reuse the result.
	// `last_used_frame` doubles as the per-frame guard, so capture the gate
	// BEFORE bumping it.
	const bool first_touch_this_frame = entry.last_used_frame != current_frame;
	entry.last_used_frame = current_frame;

	// Detect whether instance transforms moved since our last bake. Without this
	// we'd re-merge + refit every frame even for fully-static MultiMeshes, which
	// is significant wasted compute for high-instance-count crowds/foliage.
	const uint64_t mm_last_change = mesh_storage->multimesh_get_last_change(p_mm_rid);
	const uint64_t mm_generation = mesh_storage->multimesh_get_rt_generation(p_mm_rid);
	const bool transforms_changed = (entry.cached_mm_generation != mm_generation);

	bool structure_changed = (entry.last_mm_count != p_mm_count ||
			entry.last_surface_counter != p_surface_counter);

	entry.indexed = indexed;

	if (structure_changed) {
		RD *rd = RD::get_singleton();
		if (entry.blas.is_valid()) {
			rd->free_rid(entry.blas);
			entry.blas = RID();
		}
		entry.blas_built_once = false;
		entry.last_mm_count = p_mm_count;
		entry.last_surface_counter = p_surface_counter;
	}

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	bool has_tbn = has_normal;

	// Layout of uncompressed vertex buffer: [float3 positions × V] + [packed TBN × V].
	// normal_stride = 8 when both normal+tangent present (two uint16x2 packed), 4 with normal only.
	uint32_t tbn_stride = 0;
	if (has_normal && has_tangent) {
		tbn_stride = 8; // 2 × uint32 (normal oct, tangent oct+sign)
	} else if (has_normal) {
		tbn_stride = 4; // 1 × uint32 (normal oct only)
	}
	// Byte offset of the TBN block in the source vertex buffer.
	uint32_t src_tbn_byte_offset = vertex_count * 12; // after all float3 positions

	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if TBN present).
	uint32_t merged_vtx_bytes = p_mm_count * vertex_count * 12 + (has_tbn ? p_mm_count * vertex_count * tbn_stride : 0);

	// Attribute buffer: attribute_stride bytes × V, replicated N times.
	RTSurfaceData meta_sd;
	_fill_surface_geometry_data(p_mesh_surface, false, &meta_sd);
	uint32_t attrib_stride = meta_sd.geometry.attribute_stride;
	bool has_attr = attrib_stride > 0;
	const uint32_t MIN_ATTR_BYTES = 16;
	uint32_t merged_attr_bytes = has_attr ? (p_mm_count * vertex_count * attrib_stride) : MIN_ATTR_BYTES;

	RD *rd = RD::get_singleton();
	BitField<RD::BufferCreationBits> gpu_buf_flags =
			RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	const uint32_t previous_position_bytes = p_mm_count * vertex_count * 12;
	if (!entry.previous_position_buffer.is_valid() || entry.previous_position_capacity_bytes < previous_position_bytes) {
		if (entry.previous_position_buffer.is_valid()) {
			rd->free_rid(entry.previous_position_buffer);
		}
		entry.previous_position_buffer = rd->storage_buffer_create(previous_position_bytes, {}, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
		ERR_FAIL_COND_V(!entry.previous_position_buffer.is_valid(), false);
		entry.previous_position_capacity_bytes = previous_position_bytes;
		entry.blas_built_once = false;
	}

	// --- Grow / allocate merged vertex buffer ---
	if (!entry.merged_vtx_buffer.is_valid() || entry.vtx_capacity_bytes < merged_vtx_bytes) {
		if (entry.merged_vtx_buffer.is_valid()) {
			rd->free_rid(entry.merged_vtx_buffer);
			entry.merged_vtx_buffer = RID();
		}
		entry.vtx_capacity_bytes = merged_vtx_bytes;
		entry.merged_vtx_buffer = rd->vertex_buffer_create(merged_vtx_bytes, {}, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_vtx_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_vtx_buffer, "RT MM merged vtx [" + itos(cache_key) + "]");
		entry.blas_built_once = false;
	}

	// --- Grow / allocate merged attribute buffer ---
	if (!entry.merged_attr_buffer.is_valid() || entry.attr_capacity_bytes < merged_attr_bytes) {
		if (entry.merged_attr_buffer.is_valid()) {
			rd->free_rid(entry.merged_attr_buffer);
			entry.merged_attr_buffer = RID();
		}
		entry.attr_capacity_bytes = merged_attr_bytes;
		entry.merged_attr_buffer = rd->storage_buffer_create(merged_attr_bytes, {}, 0, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_attr_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_attr_buffer, "RT MM merged attr [" + itos(cache_key) + "]");
	}

	// --- Grow / allocate replicated index buffer ---
	if (indexed) {
		uint32_t needed_idx = p_mm_count * index_count;
		if (!entry.replicated_idx_buffer.is_valid() || entry.idx_capacity < needed_idx) {
			if (entry.replicated_idx_buffer.is_valid()) {
				rd->free_rid(entry.replicated_idx_buffer);
				entry.replicated_idx_buffer = RID();
			}
			entry.idx_capacity = needed_idx;
			entry.replicated_idx_buffer = rd->index_buffer_create(
					needed_idx, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, gpu_buf_flags);
			ERR_FAIL_COND_V(!entry.replicated_idx_buffer.is_valid(), false);
			rd->set_resource_name(entry.replicated_idx_buffer, "RT MM replicated idx [" + itos(cache_key) + "]");
			entry.blas_built_once = false;
		}
	}

	RID src_attr_buf;
	if (has_attr) {
		src_attr_buf = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
		ERR_FAIL_COND_V(!src_attr_buf.is_valid(), false);
	} else {
		src_attr_buf = entry.merged_attr_buffer;
	}

	MergeShader::Mode merge_mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;

	// Pre-validate source vertex buffer before opening the compute list.
	RID vtx_buf = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	ERR_FAIL_COND_V(!vtx_buf.is_valid(), false);
	uint64_t vtx_bda = rd->buffer_get_device_address(vtx_buf);

	const bool needs_rebake = first_touch_this_frame &&
			(transforms_changed || !entry.blas_built_once);

	// --- Single merged dispatch: bake vertices + TBN + attributes + (optional) indices ---
	if (needs_rebake) {
		Vector<RD::Uniform> uniforms;
		{
			auto push_buf = [&](uint32_t binding, RID buf) {
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
				u.binding = binding;
				u.append_id(buf);
				uniforms.push_back(u);
			};
			push_buf(0, entry.merged_vtx_buffer);
			push_buf(1, p_mm_gpu_buffer);
			push_buf(2, entry.merged_attr_buffer);
			push_buf(3, src_attr_buf);
			push_buf(5, entry.previous_position_buffer);
			if (indexed) {
				push_buf(4, entry.replicated_idx_buffer);
			}
		}
		RID merge_uniform_set = rd->uniform_set_create(uniforms, mm_merge_shader.version_shader[merge_mode], 0, /*p_linear_pool=*/true);
		ERR_FAIL_COND_V(!merge_uniform_set.is_valid(), false);
		uint32_t mm_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
		uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
		uint32_t tbn_stride_words = tbn_stride / 4;
		// In the merged vertex buffer the TBN block starts after all N*V float3 positions.
		uint32_t dst_tbn_base_words = p_mm_count * vertex_count * 3;

		struct MergePC {
			uint32_t src_vtx_lo, src_vtx_hi;
			// MODE_INDEXED only -- present in struct (uploaded only when indexed):
			uint32_t src_idx_lo, src_idx_hi;
			uint32_t index_count, src_is_16bit;
			// Common tail:
			uint32_t vertex_count, instance_count;
			uint32_t pos_stride_words;
			uint32_t src_tbn_base_words;
			uint32_t src_tbn_stride_words;
			uint32_t dst_tbn_base_words;
			uint32_t mm_stride, mm_offset;
			uint32_t has_tbn;
			uint32_t attr_stride_words;
			uint32_t mm_previous_offset;
		} pc;

		pc.src_vtx_lo = uint32_t(vtx_bda);
		pc.src_vtx_hi = uint32_t(vtx_bda >> 32);

		if (indexed) {
			uint64_t src_idx_bda = rd->buffer_get_device_address(index_buffer);
			pc.src_idx_lo = uint32_t(src_idx_bda);
			pc.src_idx_hi = uint32_t(src_idx_bda >> 32);
			pc.index_count = index_count;
			pc.src_is_16bit = (vertex_count <= 65536) ? 1u : 0u;
		}

		pc.vertex_count = vertex_count;
		pc.instance_count = p_mm_count;
		pc.pos_stride_words = 3; // always float3 (uncompressed check at top)
		pc.src_tbn_base_words = src_tbn_byte_offset / 4;
		pc.src_tbn_stride_words = tbn_stride_words;
		pc.dst_tbn_base_words = dst_tbn_base_words;
		pc.mm_stride = mm_stride;
		pc.mm_offset = mm_cur_offset;
		pc.has_tbn = has_tbn ? 1u : 0u;
		pc.attr_stride_words = has_attr ? (attrib_stride / 4) : 0u;
		pc.mm_previous_offset = mesh_storage->multimesh_get_previous_instance_offset(p_mm_rid);

		MergeShader::Mode mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;
		rd->compute_list_bind_compute_pipeline(p_compute_list, mm_merge_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(p_compute_list, merge_uniform_set, 0);
		if (indexed) {
			rd->compute_list_set_push_constant(p_compute_list, &pc, sizeof(MergePC));
		} else {
			// Re-pack the non-indexed PC so that the common tail follows
			// src_vtx_lo/hi without the index gap.
			struct MergePCNonIndexed {
				uint32_t src_vtx_lo, src_vtx_hi;
				uint32_t vertex_count, instance_count;
				uint32_t pos_stride_words;
				uint32_t src_tbn_base_words;
				uint32_t src_tbn_stride_words;
				uint32_t dst_tbn_base_words;
				uint32_t mm_stride, mm_offset;
				uint32_t has_tbn;
				uint32_t attr_stride_words;
				uint32_t mm_previous_offset;
			} pc_ni;
			pc_ni.src_vtx_lo = pc.src_vtx_lo;
			pc_ni.src_vtx_hi = pc.src_vtx_hi;
			pc_ni.vertex_count = pc.vertex_count;
			pc_ni.instance_count = pc.instance_count;
			pc_ni.pos_stride_words = pc.pos_stride_words;
			pc_ni.src_tbn_base_words = pc.src_tbn_base_words;
			pc_ni.src_tbn_stride_words = pc.src_tbn_stride_words;
			pc_ni.dst_tbn_base_words = pc.dst_tbn_base_words;
			pc_ni.mm_stride = pc.mm_stride;
			pc_ni.mm_offset = pc.mm_offset;
			pc_ni.has_tbn = pc.has_tbn;
			pc_ni.attr_stride_words = pc.attr_stride_words;
			pc_ni.mm_previous_offset = pc.mm_previous_offset;
			rd->compute_list_set_push_constant(p_compute_list, &pc_ni, sizeof(MergePCNonIndexed));
		}

		// Single thread count: every thread processes one vertex (idx < N*V)
		// and -- for MODE_INDEXED -- one output index (idx < N*I) using disjoint
		// destination buffers, so no in-shader barrier is required.
		uint32_t thread_count = p_mm_count * vertex_count;
		if (indexed) {
			thread_count = MAX(thread_count, p_mm_count * index_count);
		}
		rd->compute_list_dispatch_threads(p_compute_list, thread_count, 1, 1);

		// Drop the RID now that the dispatch is recorded; the underlying Vulkan
		// descriptor set lives in the linear pool until frame end.
		rd->free_rid(merge_uniform_set);
	}

	// --- Build or refit the merged BLAS (uses merged_vtx_buffer for positions) ---
	if (!entry.blas.is_valid()) {
		RD::AccelerationStructureGeometry as_geom;
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = entry.merged_vtx_buffer;
		as_geom.geometry.triangles.vertex_stride = 12; // float3, positions section only
		as_geom.geometry.triangles.vertex_count = p_mm_count * vertex_count;
		as_geom.geometry.triangles.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;

		if (indexed) {
			as_geom.geometry.triangles.index_buffer = entry.replicated_idx_buffer;
			as_geom.geometry.triangles.index_count = p_mm_count * index_count;
		}

		BitField<RD::AccelerationStructureFlagBits> as_flags =
				RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT |
				RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		entry.blas = rd->blas_create({ &as_geom, 1 }, as_flags);
		ERR_FAIL_COND_V(!entry.blas.is_valid(), false);
		rd->set_resource_name(entry.blas, "RT MM merged BLAS [" + itos(cache_key) + "]");
	}

	if (needs_rebake) {
		if (!entry.blas_built_once) {
			r_dirty_blas_list.push_back(entry.blas);
			entry.blas_built_once = true;
		} else {
			r_dirty_blas_update_list.push_back(entry.blas);
		}
		entry.cached_mm_generation = mm_generation;
	}

	// --- Populate r_surf_data from the metadata already computed above, then override merged buffer addresses.
	*r_surf_data = meta_sd;
	r_surf_data->blas = entry.blas;

	RT_GeometryData &geom = r_surf_data->geometry;

	geom.source_vertex_address = geom.vertex_buffer_address;
	geom.vertex_buffer_address = rd->buffer_get_device_address(entry.merged_vtx_buffer);
	const uint64_t previous_address = rd->buffer_get_device_address(mm_last_change == current_frame ? entry.previous_position_buffer : entry.merged_vtx_buffer);
	geom.prev_vertex_buffer_address_lo = uint32_t(previous_address);
	geom.prev_vertex_buffer_address_hi = uint32_t(previous_address >> 32);
	geom.vertex_count = p_mm_count * vertex_count;
	geom.multimesh_address = rd->buffer_get_device_address(p_mm_gpu_buffer);
	geom.multimesh_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
	geom.multimesh_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
	geom.source_vertex_count = vertex_count;
	geom.multimesh_flags = uint32_t(mesh_storage->multimesh_uses_colors(p_mm_rid)) | (uint32_t(mesh_storage->multimesh_uses_custom_data(p_mm_rid)) << 1);
	geometry_buffer_dependencies.insert(p_mm_gpu_buffer);
	geometry_buffer_dependencies.insert(vtx_buf);
	RID skin_buffer = mesh_storage->mesh_surface_get_skin_buffer(p_mesh_surface);
	if (skin_buffer.is_valid()) {
		geometry_buffer_dependencies.insert(skin_buffer);
	}
	geom.position_stride = 12; // float3, uncompressed
	geom.flags &= ~RT_GEOM_FLAG_COMPRESSED;

	// TBN section starts after all positions in the merged vertex buffer.
	if (has_tbn) {
		uint32_t tbn_base = p_mm_count * vertex_count * 12;
		geom.normal_byte_offset = tbn_base;
		geom.normal_stride = tbn_stride;
		if (has_tangent) {
			geom.tangent_byte_offset = tbn_base; // tangent is at +4 from normal within the same stride pair
			geom.tangent_stride = tbn_stride;
		} else {
			geom.tangent_byte_offset = RT_OFFSET_NONE;
			geom.tangent_stride = 0;
		}
	}

	// Point attribute address at fully replicated attribute buffer.
	if (has_attr && entry.merged_attr_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(entry.merged_attr_buffer);
	}

	// Point index address at the replicated (uint32) index buffer.
	if (indexed && entry.replicated_idx_buffer.is_valid()) {
		geom.index_buffer_address = rd->buffer_get_device_address(entry.replicated_idx_buffer);
		geom.index_format = RT_INDEX_FORMAT_UINT32; // always uint32 in replicated buffer
		geom.primitive_count = p_mm_count * prim_count;
	} else {
		geom.index_buffer_address = 0;
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = p_mm_count * prim_count;
	}

	return true;
}

// ---------------------------------------------------------------------------
// TLAS creation (main entry point per frame)
// ---------------------------------------------------------------------------

_FORCE_INLINE_ static uint32_t _rt_indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}

RTViewportState *RenderRaytracing::build_tlas(const RenderDataRD *p_render_data) {
	if (!p_render_data || !p_render_data->rt_instances) {
		return nullptr;
	}

	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	if (!state) {
		return nullptr;
	}

	RENDER_TIMESTAMP("RT Frame Prepare");
	prepare_frame();
	RENDER_TIMESTAMP("RT Scene Gather");

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	LocalVector<RID> dirty_blas_list;
	LocalVector<RID> dirty_blas_update_list;
	Vector<MicroGeometrySelection::Task> micro_tasks;
	Vector<RTMicroGeometryTask> micro_rt_tasks;
	uint32_t micro_levels = 0;
	uint64_t micro_group_work = 0, micro_cluster_work = 0, micro_coarse_work = 0;
	uint64_t scene_signature = 0x9e3779b97f4a7c15ULL;
	auto hash_scene = [&](const auto &p_value) {
		scene_signature = _rt_scene_hash(&p_value, sizeof(p_value), scene_signature);
	};
	bool uses_time = false;
	bool uses_previous_time = false;

#ifdef TOOLS_ENABLED
	uint32_t tlas_instance_count = 0;
	uint32_t tlas_primitive_count = 0;
	uint32_t rt_blas_builds = 0;
	uint32_t rt_blas_refits = 0;
	uint32_t rt_triangles_built = 0;
	uint32_t rt_triangles_refit = 0;
	const bool collect_render_info = (p_render_data->render_info != nullptr);
#endif

	// -----------------------------------------------------------------------
	// Phase 1: CPU / buffer-update work
	// -----------------------------------------------------------------------
	struct PendingMMSurface {
		RID mm_rid;
		RID mm_gpu_buffer;
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf;
		void *mesh_surface;
		uint32_t mm_count;
		uint32_t surface_index;
		uint32_t surface_counter;
		Transform3D instance_transform;
		Transform3D prev_instance_transform;
		bool transform_moved;
		RTMaterialData *mat_data;
		uint32_t inst_flags;
	};
	LocalVector<PendingMMSurface> pending_mm_surfaces;

	auto register_emissive_source = [&](const RenderForwardClustered::GeometryInstanceForwardClustered *p_instance,
											RID p_resource, uint32_t p_surface_index, uint32_t p_surface_counter, uint32_t p_geometry_index,
											uint32_t p_key_primitive_offset, uint32_t p_primitive_count, const Transform3D &p_transform, RTMaterialData *p_material) {
		if (!p_instance || !p_instance->rt_visible_receiver || !p_material || p_material->is_custom_shader || p_primitive_count == 0) {
			return;
		}
		const RT_MaterialData &material = p_material->data;
		const bool textured = (material.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0;
		const bool colored = material.emission_color[0] != 0.0f || material.emission_color[1] != 0.0f || material.emission_color[2] != 0.0f;
		if (material.emission_strength == 0.0f || (!textured && !colored)) {
			return;
		}

		RTEmissiveSource source;
		source.instance_id = p_instance->get_instance_rid().get_id();
		source.resource_id = p_resource.get_id();
		source.surface_generation = (uint64_t(p_surface_counter) << 32) | uint64_t(p_surface_index);
		source.geometry_index = p_geometry_index;
		source.key_primitive_offset = p_key_primitive_offset;
		source.primitive_count = p_primitive_count;
		source.topology_generation = hash_murmur3_one_64(source.resource_id, hash_murmur3_one_64(source.surface_generation));
		source.transform = p_transform;
		source.material = p_material;
		emissive_sources.push_back(source);
	};
	auto instance_geometry = [](const RenderForwardClustered::GeometryInstanceForwardClustered *p_instance, const RT_GeometryData &p_geometry, const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surface) {
		RT_GeometryData geometry = p_geometry;
		geometry.instance_layer_mask = p_instance->layer_mask;
		geometry.instance_uniforms_offset = p_instance->shader_uniforms_offset;
		if (p_instance->rt_casts_shadows) {
			geometry.flags |= RT_GEOM_FLAG_CASTS_SHADOWS;
		}
		if (p_instance->rt_shadows_only) {
			geometry.flags |= RT_GEOM_FLAG_SHADOWS_ONLY;
		}
		if (p_surface && (!p_surface->shader || p_surface->shader->cull_mode != RSE::CULL_MODE_DISABLED) &&
				!(p_surface->flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS)) {
			geometry.flags |= RT_GEOM_FLAG_SHADOW_CULL_ENABLED;
		}
		return geometry;
	};

	auto append_micro_surface = [&](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surface) {
		const auto *instance = p_surface->owner;
		const auto *shader = p_surface->shader;
		if (!shader || !instance->persistent_instance || !p_surface->persistent_surface || instance->mesh_instance.is_valid() || instance->rt_procedural || instance->instance_count == 0 || p_surface->primitive != RSE::PRIMITIVE_TRIANGLES || shader->uses_alpha_pass() || shader->uses_vertex || shader->uses_position || shader->uses_vertex_time || shader->writes_modelview_or_projection || shader->uses_particle_trails || shader->uses_point_size || shader->uses_z_clip_scale) {
			return false;
		}
		const auto &record = persistent_instances[uint32_t(instance->persistent_instance) - 1].data;
		RID asset = RID::from_uint64(record.asset);
		auto *storage = mesh_storage->get_micro_geometry_storage();
		Ref<MicroGeometryData> source = storage->get_source(asset);
		if (source.is_null()) {
			return false;
		}
		uses_time |= shader->rt_uses_time();
		uses_previous_time |= shader->rt_uses_previous_time();
		if (!storage->is_ready(asset, true)) {
			return true;
		}
		const auto &metadata = source->get_metadata();
		uint32_t surface_index = UINT32_MAX;
		for (uint32_t index = 0; index < uint32_t(metadata.surfaces.size()); index++) {
			if (metadata.surfaces[index].source_surface == p_surface->surface_index) {
				surface_index = index;
				break;
			}
		}
		if (surface_index == UINT32_MAX) {
			return false;
		}
		const uint32_t count = record.multimesh_address != 0 ? record.multimesh_count : 1;
		if (!count) {
			return true;
		}
		RID material = p_surface->material_rid.is_valid() ? p_surface->material_rid : owner->scene_shader.default_material;
		RTMaterialData *native_material = process_material(material, material_storage->material_get_rt_invalidation_counter(material));
		hash_scene(material.get_id());
		hash_scene(material_storage->material_get_rt_content_generation(material, instance->shader_uniforms_offset));
		hash_scene(asset.get_id());
		const auto &surface_record = persistent_surfaces[uint32_t(p_surface->persistent_surface) - 1].data;
		const bool finest = surface_record.force_finest != 0;
		uint32_t flags = 0;
		if (shader->cull_mode == RSE::CULL_MODE_DISABLED) {
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
		}
		if (shader->cull_mode != RSE::CULL_MODE_FRONT) {
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
		}
		if (!shader->uses_alpha_clip && !shader->uses_alpha && !shader->uses_blend_alpha) {
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
		}
		MicroGeometrySelection::Task task;
		task.instance = record.handle;
		task.surface = p_surface->persistent_surface;
		task.asset = record.asset;
		task.group_offset = micro_group_work;
		task.cluster_offset = micro_cluster_work;
		task.coarse_offset = micro_coarse_work;
		task.group_count = metadata.groups.size();
		task.cluster_count = metadata.clusters.size();
		task.coarse_count = finest ? task.cluster_count : metadata.coarse_cluster_count;
		task.multimesh_count = count;
		task.bin = micro_tasks.size();
		task.flags = instance->store_transform_cache ? 0 : 1;
		if (p_render_data->scene_data->view_count != 1) {
			task.flags |= 2;
		}
		if (instance->base_flags & RenderForwardClustered::INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT) {
			RID commands = mesh_storage->_multimesh_get_command_buffer_rd_rid(instance->data->base);
			task.indirect_command = RD::get_singleton()->buffer_get_device_address(commands) + uint64_t(p_surface->surface_index) * sizeof(uint32_t) * RendererRD::MeshStorage::INDIRECT_MULTIMESH_COMMAND_STRIDE;
			geometry_buffer_dependencies.insert(commands);
		}
		RTMicroGeometryTask rt_task;
		rt_task.clas_addresses = RD::get_singleton()->buffer_get_device_address(storage->get_clas_addresses(asset));
		rt_task.instance = task.instance;
		rt_task.surface = task.surface;
		rt_task.selection_bin = task.bin;
		rt_task.geometry_base = geometry_data.size();
		rt_task.motion_base = motion_transforms.size();
		rt_task.instance_count = count;
		rt_task.cluster_count = task.cluster_count;
		rt_task.bitmap_offset = task.cluster_offset;
		rt_task.group_offset = task.group_offset;
		rt_task.group_count = task.group_count;
		rt_task.instance_flags = flags;
		micro_group_work += uint64_t(task.group_count) * count;
		micro_cluster_work += uint64_t(task.cluster_count) * count;
		micro_coarse_work += uint64_t(task.coarse_count) * count;
		ERR_FAIL_COND_V_MSG(MAX(micro_group_work, micro_cluster_work) > UINT32_MAX / 8, true, "RT microgeometry selection exceeds the device buffer range.");
		for (const auto &group : metadata.groups) {
			micro_levels = MAX(micro_levels, group.depth + 1);
		}
		micro_tasks.push_back(task);
		micro_rt_tasks.push_back(rt_task);
		for (uint32_t ordinal = 0; ordinal < count; ordinal++) {
			RT_GeometryData geometry = {};
			geometry.flags = RT_GEOM_FLAG_CLUSTERED;
			geometry.micro_asset_address = record.asset_address;
			geometry.micro_page_pool = RD::get_singleton()->buffer_get_device_address(storage->get_pool());
			geometry.micro_primitive_lookup = storage->get_primitive_lookup(asset, surface_index);
			geometry.micro_surface = surface_index;
			geometry.primitive_count = metadata.surfaces[surface_index].source_triangle_count;
			geometry.source_vertex_count = metadata.surfaces[surface_index].source_vertex_count;
			geometry.instance_index = ordinal;
			for (uint32_t channel = 0; channel < 4; channel++) {
				geometry.instance_color[channel] = 1;
			}
			geometry.multimesh_address = record.multimesh_address;
			geometry.multimesh_stride = record.multimesh_stride;
			geometry.multimesh_offset = record.multimesh_current_offset;
			geometry.micro_multimesh_previous_offset = record.multimesh_previous_offset;
			if (record.multimesh_address != 0) {
				geometry.multimesh_flags = (mesh_storage->multimesh_uses_colors(instance->data->base) ? 1u : 0u) | (mesh_storage->multimesh_uses_custom_data(instance->data->base) ? 2u : 0u) | ((record.flags & (1u << 13)) ? 4u : 0u);
			}
			geometry = instance_geometry(instance, geometry, p_surface);
			register_emissive_source(instance, instance->data->base, p_surface->surface_index, mesh_storage->mesh_surface_get_rt_invalidation_counter(p_surface->surface), geometry_data.size(), ordinal * geometry.primitive_count, geometry.primitive_count, instance->transform, native_material);
			geometry_data.push_back(geometry);
			material_data.push_back(native_material->data);
			geometry_material_programs.push_back(native_material->hit_shader);
			blass.push_back(RID());
			blas_transforms.push_back(instance->transform);
			instance_flags.push_back(flags);
			instance_masks.push_back(255);
			motion_indices.push_back(motion_transforms.size());
			motion_transforms.push_back(RT_InstanceMotionData());
		}
		return true;
	};

	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
		const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
				static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
		if (!inst || !inst->data) {
			continue;
		}
		const Transform3D &instance_transform = inst->transform;
		hash_scene(inst->get_instance_rid().get_id());
		hash_scene(inst->data->base.get_id());
		hash_scene(inst->mesh_instance.get_id());
		hash_scene(instance_transform);
		hash_scene(inst->layer_mask);
		hash_scene(inst->rt_visible_receiver);
		hash_scene(inst->rt_casts_shadows);
		hash_scene(inst->rt_shadows_only);

		// Determine previous-frame transform for motion vectors.
		const Transform3D &prev_instance_transform =
				(inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::TELEPORTED)
				? inst->transform
				: inst->prev_transform;

		if (inst->rt_procedural) {
			RTProceduralState *ps = inst->rt_procedural;

			if (!inst->data || !inst->data->material_override.is_valid()) {
				continue;
			}
			RID proc_material_rid = inst->data->material_override;
			const SceneShaderForwardClustered::MaterialData *proc_material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(proc_material_rid, RendererRD::MaterialStorage::SHADER_TYPE_3D));
			if (!proc_material || !proc_material->shader_data || proc_material->shader_data->version.is_null() || proc_material->shader_data->code.is_empty()) {
				continue;
			}

			if (ps->dirty) {
				ps->content_generation++;
#ifdef TOOLS_ENABLED
				uint32_t pre_proc_build_size = dirty_blas_list.size();
#endif
				update_procedural_blas(ps, dirty_blas_list);
				ps->dirty = false;
#ifdef TOOLS_ENABLED
				if (collect_render_info) {
					rt_blas_builds += dirty_blas_list.size() - pre_proc_build_size;
				}
#endif
			}

			if (ps->blas.is_valid()) {
				hash_scene(ps->content_generation);
				blass.push_back(ps->blas);
				blas_transforms.push_back(instance_transform);

				RT_GeometryData geom = {};
				geom.flags = RT_GEOM_FLAG_PROCEDURAL;
				geom.vertex_buffer_address = ps->gpu_buffer_address;
				geometry_data.push_back(instance_geometry(inst, geom, nullptr));

				if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					Transform3D previous_to_rt = prev_instance_transform;
					previous_to_rt.origin -= state->rt_origin;
					RendererRD::MaterialStorage::store_transform_transposed_3x4(previous_to_rt, motion.prev_object_to_rt);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				// Material for procedural geometry (already validated above).
				uint16_t proc_mat_counter = material_storage->material_get_rt_invalidation_counter(proc_material_rid);
				hash_scene(proc_material_rid.get_id());
				uses_time |= proc_material->shader_data->rt_uses_time();
				uses_previous_time |= proc_material->shader_data->rt_uses_previous_time();
				hash_scene(material_storage->material_get_rt_content_generation(proc_material_rid, inst->shader_uniforms_offset));
				RTMaterialData *proc_mat_data = process_material(proc_material_rid, proc_mat_counter);
				material_data.push_back(proc_mat_data->data);
				geometry_material_programs.push_back(proc_mat_data->hit_shader);

				// Procedural instances disable triangle culling and are opaque.
				uint32_t inst_flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT |
						RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				instance_flags.push_back(inst_flags);
				instance_masks.push_back(0xFF);
			}
			continue;
		}

		// MultiMesh: resolve materials and warm data cache now.
		// Compute dispatches and TLAS assembly are deferred to Phase 2.
		if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
			RID mm_rid = inst->data->base;

			if (mesh_storage->multimesh_get_transform_format(mm_rid) != RSE::MULTIMESH_TRANSFORM_3D) {
				continue;
			}

			uint32_t mm_count = mesh_storage->multimesh_get_instances_to_draw(mm_rid);
			hash_scene(mm_count);
			hash_scene(mesh_storage->multimesh_get_rt_generation(mm_rid));
			if (mm_count == 0) {
				continue;
			}

			RID mm_gpu_buffer = mesh_storage->multimesh_get_gpu_buffer(mm_rid);
			// Populate data cache now — first access triggers GPU readback, safe here.
			mesh_storage->multimesh_get_local_data_ptr(mm_rid);

			bool transform_moved = (inst->transform_status ==
					RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED);

			const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf = inst->surface_caches;
			while (mm_surf) {
				if (mm_surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) {
					mm_surf = mm_surf->next;
					continue;
				}

				if (append_micro_surface(mm_surf)) {
					mm_surf = mm_surf->next;
					continue;
				}

				void *mesh_surface = mm_surf->surface;
				uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);
				hash_scene(mm_surf->surface_index);
				hash_scene(surface_counter);
				uses_time |= mm_surf->shader && mm_surf->shader->rt_uses_time();
				uses_previous_time |= mm_surf->shader && mm_surf->shader->rt_uses_previous_time();

				RID material_rid;
				if (mm_surf->owner->data->material_override.is_valid()) {
					material_rid = mm_surf->owner->data->material_override;
				} else if (mm_surf->surface_index < mm_surf->owner->data->surface_materials.size() &&
						mm_surf->owner->data->surface_materials[mm_surf->surface_index].is_valid()) {
					material_rid = mm_surf->owner->data->surface_materials[mm_surf->surface_index];
				} else {
					RID mesh_rid = mesh_storage->multimesh_get_mesh(mm_rid);
					if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
						material_rid = mesh_storage->mesh_surface_get_material(mesh_rid, mm_surf->surface_index);
					}
				}

				uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
				hash_scene(material_rid.get_id());
				hash_scene(material_storage->material_get_rt_content_generation(material_rid, inst->shader_uniforms_offset));
				RTMaterialData *mat_data = process_material(material_rid, material_counter);

				uint32_t inst_flags = 0;
				if (mm_surf->shader) {
					switch (mm_surf->shader->cull_mode) {
						case RSE::CULL_MODE_DISABLED:
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
							break;
						case RSE::CULL_MODE_FRONT:
							break;
						case RSE::CULL_MODE_BACK:
						default:
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
							break;
					}
				} else {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
				}
				if (mat_data->is_custom_shader) {
					if (!mat_data->uses_alpha_clip && !mat_data->has_alpha_texture) {
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					}
				} else {
					bool is_alpha = mm_surf->shader &&
							(mm_surf->shader->uses_alpha_clip || mm_surf->shader->uses_blend_alpha || mm_surf->shader->uses_alpha);
					if (!is_alpha) {
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					}
				}

				PendingMMSurface pending;
				pending.mm_rid = mm_rid;
				pending.mm_gpu_buffer = mm_gpu_buffer;
				pending.mm_surf = mm_surf;
				pending.mesh_surface = mesh_surface;
				pending.mm_count = mm_count;
				pending.surface_index = mm_surf->surface_index;
				pending.surface_counter = surface_counter;
				pending.instance_transform = instance_transform;
				pending.prev_instance_transform = prev_instance_transform;
				pending.transform_moved = transform_moved;
				pending.mat_data = mat_data;
				pending.inst_flags = inst_flags;
				pending_mm_surfaces.push_back(pending);

				mm_surf = mm_surf->next;
			}
			continue;
		}

		// Walk the surface cache linked list.
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;
		bool instance_static = inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::NONE;
		while (surf) {
			// Skip surfaces routed to the raster alpha overlay
			if (surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) {
				surf = surf->next;
				continue;
			}

			if (append_micro_surface(surf)) {
				surf = surf->next;
				continue;
			}

			void *mesh_surface = surf->surface;
			uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);
			hash_scene(surf->surface_index);
			hash_scene(surface_counter);
			uses_time |= surf->shader && surf->shader->rt_uses_time();
			uses_previous_time |= surf->shader && surf->shader->rt_uses_previous_time();

#ifdef TOOLS_ENABLED
			uint32_t pre_build_size = dirty_blas_list.size();
			uint32_t pre_refit_size = dirty_blas_update_list.size();
#endif

			// MeshInstance skinning/blend shapes provide a deformed vertex buffer.
			RTSurfaceData *surf_data = nullptr;
			if (inst->mesh_instance.is_valid()) {
				RID curr_vb = mesh_storage->mesh_instance_get_vertex_buffer(inst->mesh_instance, surf->surface_index);
				if (curr_vb.is_valid()) {
					RTDeformedGeometrySource src;
					src.current_vb = curr_vb;
					src.prev_vb = mesh_storage->mesh_instance_get_prev_vertex_buffer(inst->mesh_instance, surf->surface_index);
					src.change_stamp = mesh_storage->mesh_instance_get_last_change(inst->mesh_instance, surf->surface_index);
					hash_scene(src.change_stamp);
					uint64_t mi_id = inst->mesh_instance.get_id();
					uint32_t mi_index = static_cast<uint32_t>(mi_id & 0xFFFFFFFFULL);
					src.cache_version = static_cast<uint32_t>(mi_id >> 32);
					src.cache_key = (static_cast<uint64_t>(mi_index) << 16) | (surf->surface_index & 0xFFFFu);
					src.surface_counter = surface_counter;
					surf_data = process_deformed_surface(surf, mesh_surface, src, dirty_blas_list, dirty_blas_update_list);
				}
			}
			if (!surf_data) {
				surf_data = process_surface(surf, mesh_surface, surface_counter, instance_transform, dirty_blas_list);
			}
			if (!surf_data || !surf_data->blas.is_valid()) {
				surf = surf->next;
				continue;
			}

			RID material_rid;
			if (surf->owner->data->material_override.is_valid()) {
				material_rid = surf->owner->data->material_override;
			} else if (surf->surface_index < surf->owner->data->surface_materials.size() &&
					surf->owner->data->surface_materials[surf->surface_index].is_valid()) {
				material_rid = surf->owner->data->surface_materials[surf->surface_index];
			} else {
				RID mesh_rid = surf->owner->data->base;
				if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
					material_rid = mesh_storage->mesh_surface_get_material(mesh_rid, surf->surface_index);
				}
			}

			uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
			hash_scene(material_rid.get_id());
			hash_scene(material_storage->material_get_rt_content_generation(material_rid, inst->shader_uniforms_offset));
			RTMaterialData *mat_data = process_material(material_rid, material_counter);

			Transform3D final_transform;
			if (instance_static && surf->cached_final_transform_valid) {
				final_transform = surf->cached_final_transform;
			} else {
				final_transform = instance_transform;
				surf->cached_final_transform = final_transform;
				surf->cached_final_transform_valid = true;
			}
			blas_transforms.push_back(final_transform);

			blass.push_back(surf_data->blas);
			const uint32_t geometry_index = geometry_data.size();
			geometry_data.push_back(instance_geometry(inst, surf_data->geometry, surf));
			for (RID buffer : { mesh_storage->mesh_surface_get_vertex_buffer(mesh_surface), mesh_storage->mesh_surface_get_attribute_buffer(mesh_surface), mesh_storage->mesh_surface_get_skin_buffer(mesh_surface), mesh_storage->mesh_surface_get_index_buffer(mesh_surface, 0) }) {
				if (buffer.is_valid()) {
					geometry_buffer_dependencies.insert(buffer);
				}
			}
			register_emissive_source(inst, inst->data->base, surf->surface_index, surface_counter, geometry_index, 0,
					surf_data->geometry.primitive_count, final_transform, mat_data);

			if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED ||
					(geometry_data[geometry_index].prev_vertex_buffer_address_lo | geometry_data[geometry_index].prev_vertex_buffer_address_hi) != 0) {
				motion_indices.push_back((int32_t)motion_transforms.size());
				RT_InstanceMotionData motion = {};
				Transform3D prev_final = prev_instance_transform;
				prev_final.origin -= state->rt_origin;
				RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_rt);
				motion_transforms.push_back(motion);
			} else {
				motion_indices.push_back(-1);
			}

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count++;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(surf->primitive, vertices);
				tlas_primitive_count += prim_count;
				uint32_t build_delta = dirty_blas_list.size() - pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif

			material_data.push_back(mat_data->data);
			geometry_material_programs.push_back(mat_data->hit_shader);

			// Determine per-instance TLAS flags from material properties.
			uint32_t inst_flags = 0;
			if (surf->shader) {
				switch (surf->shader->cull_mode) {
					case RSE::CULL_MODE_DISABLED:
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
						break;
					case RSE::CULL_MODE_FRONT:
						break;
					case RSE::CULL_MODE_BACK:
					default:
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
						break;
				}
			} else {
				inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
			}

			if (mat_data->is_custom_shader) {
				if (!mat_data->uses_alpha_clip && !mat_data->has_alpha_texture) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			} else {
				// Standard material: FORCE_OPAQUE if no alpha usage.
				bool is_alpha = surf->shader && (surf->shader->uses_alpha_clip || surf->shader->uses_blend_alpha || surf->shader->uses_alpha);
				if (!is_alpha) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			}
			instance_flags.push_back(inst_flags);
			instance_masks.push_back(0xFF);

			surf = surf->next;
		}
	}

	// -----------------------------------------------------------------------
	// Phase 2: GPU compute — merged MultiMesh BLAS dispatches.
	// -----------------------------------------------------------------------
	RENDER_TIMESTAMP("RT Merged Geometry Compute");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (const PendingMMSurface &pending : pending_mm_surfaces) {
#ifdef TOOLS_ENABLED
		uint32_t mm_pre_build_size = dirty_blas_list.size();
		uint32_t mm_pre_refit_size = dirty_blas_update_list.size();
#endif
		RTSurfaceData merged_sd;
		bool use_merged = pending.mm_gpu_buffer.is_valid() &&
				_build_merged_mm_blas(pending.mm_rid, pending.mm_gpu_buffer, pending.mesh_surface,
						pending.mm_count, pending.surface_index, pending.surface_counter,
						compute_list, dirty_blas_list, dirty_blas_update_list, &merged_sd);

		if (use_merged) {
			blass.push_back(merged_sd.blas);
			blas_transforms.push_back(pending.instance_transform);
			const uint32_t geometry_index = geometry_data.size();
			geometry_data.push_back(instance_geometry(pending.mm_surf->owner, merged_sd.geometry, pending.mm_surf));
			register_emissive_source(pending.mm_surf->owner, pending.mm_rid, pending.surface_index, pending.surface_counter,
					geometry_index, 0, merged_sd.geometry.primitive_count, pending.instance_transform, pending.mat_data);
			material_data.push_back(pending.mat_data->data);
			geometry_material_programs.push_back(pending.mat_data->hit_shader);
			if (pending.transform_moved || (merged_sd.geometry.prev_vertex_buffer_address_lo | merged_sd.geometry.prev_vertex_buffer_address_hi) != 0) {
				Transform3D previous_to_rt = pending.prev_instance_transform;
				previous_to_rt.origin -= state->rt_origin;
				motion_indices.push_back((int32_t)motion_transforms.size());
				RT_InstanceMotionData motion = {};
				RendererRD::MaterialStorage::store_transform_transposed_3x4(previous_to_rt, motion.prev_object_to_rt);
				motion_transforms.push_back(motion);
			} else {
				motion_indices.push_back(-1);
			}
			instance_flags.push_back(pending.inst_flags);
			instance_masks.push_back(0xFF);
#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count++;
				uint32_t prim_count = merged_sd.geometry.primitive_count;
				tlas_primitive_count += prim_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		} else {
			// Fallback: expanded TLAS — one entry per instance, shared BLAS.
			const float *mm_data = mesh_storage->multimesh_get_local_data_ptr(pending.mm_rid);
			if (!mm_data) {
				continue;
			}

			const uint32_t mm_stride = mesh_storage->multimesh_get_stride(pending.mm_rid);
			const uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(pending.mm_rid);
			const uint32_t mm_prev_offset = mesh_storage->multimesh_get_previous_instance_offset(pending.mm_rid);

			RD::get_singleton()->compute_list_end();
			RTSurfaceData *surf_data = process_surface(pending.mm_surf, pending.mesh_surface,
					pending.surface_counter, pending.instance_transform, dirty_blas_list);
			compute_list = RD::get_singleton()->compute_list_begin();
			if (!surf_data || !surf_data->blas.is_valid()) {
				continue;
			}

			for (uint32_t mi = 0; mi < pending.mm_count; mi++) {
				const float *d = mm_data + (mm_cur_offset + mi) * mm_stride;
				Transform3D mm_xform;
				mm_xform.basis.rows[0][0] = d[0];
				mm_xform.basis.rows[0][1] = d[1];
				mm_xform.basis.rows[0][2] = d[2];
				mm_xform.origin.x = d[3];
				mm_xform.basis.rows[1][0] = d[4];
				mm_xform.basis.rows[1][1] = d[5];
				mm_xform.basis.rows[1][2] = d[6];
				mm_xform.origin.y = d[7];
				mm_xform.basis.rows[2][0] = d[8];
				mm_xform.basis.rows[2][1] = d[9];
				mm_xform.basis.rows[2][2] = d[10];
				mm_xform.origin.z = d[11];

				Transform3D final_transform = pending.instance_transform * mm_xform;

				blass.push_back(surf_data->blas);
				blas_transforms.push_back(final_transform);
				const uint32_t geometry_index = geometry_data.size();
				RT_GeometryData instance_data = instance_geometry(pending.mm_surf->owner, surf_data->geometry, pending.mm_surf);
				instance_data.instance_index = mi;
				const bool has_color = mesh_storage->multimesh_uses_colors(pending.mm_rid);
				if (has_color) {
					memcpy(instance_data.instance_color, d + 12, sizeof(instance_data.instance_color));
				}
				if (mesh_storage->multimesh_uses_custom_data(pending.mm_rid)) {
					memcpy(instance_data.instance_custom, d + 12 + (has_color ? 4 : 0), sizeof(instance_data.instance_custom));
				}
				geometry_data.push_back(instance_data);
				for (RID buffer : { mesh_storage->mesh_surface_get_vertex_buffer(pending.mesh_surface), mesh_storage->mesh_surface_get_attribute_buffer(pending.mesh_surface), mesh_storage->mesh_surface_get_skin_buffer(pending.mesh_surface), mesh_storage->mesh_surface_get_index_buffer(pending.mesh_surface, 0) }) {
					if (buffer.is_valid()) {
						geometry_buffer_dependencies.insert(buffer);
					}
				}
				register_emissive_source(pending.mm_surf->owner, pending.mm_rid, pending.surface_index, pending.surface_counter,
						geometry_index, mi * surf_data->geometry.primitive_count, surf_data->geometry.primitive_count, final_transform, pending.mat_data);
				material_data.push_back(pending.mat_data->data);
				geometry_material_programs.push_back(pending.mat_data->hit_shader);

				if (pending.transform_moved || mm_prev_offset != mm_cur_offset) {
					const float *previous_data = mm_data + (mm_prev_offset + mi) * mm_stride;
					Transform3D previous_mm_transform;
					for (int row = 0; row < 3; row++) {
						for (int column = 0; column < 3; column++) {
							previous_mm_transform.basis.rows[row][column] = previous_data[row * 4 + column];
						}
						previous_mm_transform.origin[row] = previous_data[row * 4 + 3];
					}
					Transform3D prev_final = pending.prev_instance_transform * previous_mm_transform;
					prev_final.origin -= state->rt_origin;
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_rt);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				uint32_t inst_flags = pending.inst_flags;
				if (mm_xform.basis.determinant() < 0.0) {
					inst_flags ^= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
				}
				instance_flags.push_back(inst_flags);
				instance_masks.push_back(0xFF);
			}

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count += pending.mm_count;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(pending.mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(pending.mm_surf->primitive, vertices);
				tlas_primitive_count += prim_count * pending.mm_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		}
	}

	// Phase 3: BLAS / TLAS build.
#ifdef TOOLS_ENABLED
	if (collect_render_info) {
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += tlas_primitive_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS] += rt_blas_builds;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS] += rt_blas_refits;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT] += rt_triangles_built;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT] += rt_triangles_refit;
	}
#endif

	RD::get_singleton()->compute_list_end();

	RENDER_TIMESTAMP("Microgeometry RT Prepare");
	ERR_FAIL_COND_V(!_prepare_micro_geometry(state, p_render_data, micro_tasks, micro_rt_tasks, micro_levels), nullptr);
	RENDER_TIMESTAMP("RT Material Pipeline");
	ERR_FAIL_COND_V(!update_material_pipeline(state), nullptr);
	RENDER_TIMESTAMP("RT Finalize Buffers");
	finalize_buffers(state);
	ERR_FAIL_COND_V(!build_acceleration_structures(state, dirty_blas_list, dirty_blas_update_list), nullptr);
	RENDER_TIMESTAMP("RT Decals and Lights");
	state->decal_count = 0;
	state->decal_generation = 0;
	if (p_render_data->rt_decals && p_render_data->decals) {
		RendererRD::TextureStorage::RTDecalSnapshot snapshot = RendererRD::TextureStorage::get_singleton()->build_rt_decal_snapshot(*p_render_data->rt_decals, *p_render_data->decals, p_render_data->scene_data->cam_transform, state->rt_origin);
		state->decal_count = snapshot.count;
		state->decal_generation = snapshot.generation;
		hash_scene(snapshot.generation);
		uint32_t size = MAX(uint32_t(snapshot.data.size()), 16u);
		if (size > state->decal_buffer_capacity) {
			if (state->decal_buffer.is_valid()) {
				RD::get_singleton()->free_rid(state->decal_buffer);
			}
			state->decal_buffer = RD::get_singleton()->storage_buffer_create(size);
			state->decal_buffer_capacity = state->decal_buffer.is_valid() ? size : 0;
			ERR_FAIL_COND_V(state->decal_buffer.is_null(), nullptr);
		}
		if (!snapshot.data.is_empty()) {
			RD::get_singleton()->buffer_update(state->decal_buffer, 0, snapshot.data.size(), snapshot.data.ptr());
		}
	}
	build_light_registry(state, p_render_data, scene_signature);
	hash_scene(blass.size());
	for (uint32_t index = 0; index < blass.size(); index++) {
		if (!(geometry_data[index].flags & RT_GEOM_FLAG_CLUSTERED)) {
			hash_scene(blass[index].get_id());
		}
	}
	hash_scene(RendererEnvironmentStorage::get_singleton()->environment_get_rt_generation(p_render_data->environment));
	if (p_render_data->environment.is_valid()) {
		hash_scene(owner->get_sky()->sky_get_content_generation(owner->environment_get_sky(p_render_data->environment)));
	}
	if (uses_time) {
		hash_scene(p_render_data->scene_data->time);
	}
	if (uses_previous_time) {
		const float previous_time = p_render_data->scene_data->calculate_motion_vectors ? p_render_data->scene_data->time - p_render_data->scene_data->time_step : 0.0f;
		hash_scene(previous_time);
	}
	if (state->scene_generation == 0 || state->scene_signature != scene_signature) {
		state->scene_signature = scene_signature;
		state->scene_generation++;
		state->pathtracing_history_epoch++;
	}

	return state;
}

// ---------------------------------------------------------------------------
// Light registry
// ---------------------------------------------------------------------------

void RenderRaytracing::build_light_registry(RTViewportState *p_state, const RenderDataRD *p_render_data, uint64_t &r_scene_signature) {
	ERR_FAIL_NULL(p_state);
	ERR_FAIL_NULL(p_render_data);

	LocalVector<RTLightKey> local_keys;
	LocalVector<RT_LightData> local_lights;
	LocalVector<RTLightKey> infinite_keys;
	LocalVector<RT_LightData> infinite_lights;
	LocalVector<RTLightKey> environment_keys;
	LocalVector<RT_LightData> environment_lights;

	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();

	auto compute_light_energy = [&](RID p_base, RSE::LightType p_type) {
		float sign = ls->light_is_negative(p_base) ? -1.0f : 1.0f;
		float e = sign * ls->light_get_param(p_base, RSE::LIGHT_PARAM_ENERGY);
		if (owner->is_using_physical_light_units()) {
			e *= ls->light_get_param(p_base, RSE::LIGHT_PARAM_INTENSITY);
			if (p_type == RSE::LIGHT_OMNI) {
				e *= 1.0f / (Math::PI * 4.0f);
			} else if (p_type == RSE::LIGHT_AREA) {
				e *= 1.0f / (Math::PI * 2.0f);
			} else {
				e *= 1.0f / Math::PI;
			}
		} else {
			e *= Math::PI;
		}
		return e;
	};

	if (p_render_data->rt_lights) {
		const PagedArray<RID> &lights = *p_render_data->rt_lights;
		for (uint32_t li = 0; li < uint32_t(lights.size()); li++) {
			RID light_instance = lights[li];
			if (!ls->owns_light_instance(light_instance)) {
				continue;
			}
			RID base = ls->light_instance_get_base_light(light_instance);
			if (!base.is_valid()) {
				continue;
			}
			RSE::LightType type = ls->light_get_type(base);
			if (type == RSE::LIGHT_DIRECTIONAL && ls->light_directional_get_sky_mode(base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
				continue;
			}
			RT_LightData ld = {};
			Transform3D xform = ls->light_instance_get_base_transform(light_instance);
			const uint64_t light_generation = ls->light_get_rt_generation(base);
			const uint64_t light_id = light_instance.get_id();
			r_scene_signature = _rt_scene_hash(&light_id, sizeof(light_id), r_scene_signature);
			r_scene_signature = _rt_scene_hash(&light_generation, sizeof(light_generation), r_scene_signature);
			r_scene_signature = _rt_scene_hash(&xform, sizeof(xform), r_scene_signature);
			xform.origin -= p_state->rt_origin;
			Vector3 direction = -xform.basis.get_column(2).normalized();
			ld.position[0] = xform.origin.x;
			ld.position[1] = xform.origin.y;
			ld.position[2] = xform.origin.z;
			ld.direction[0] = direction.x;
			ld.direction[1] = direction.y;
			ld.direction[2] = direction.z;
			switch (type) {
				case RSE::LIGHT_DIRECTIONAL:
					ld.type = RT_LIGHT_TYPE_DIRECTIONAL;
					break;
				case RSE::LIGHT_OMNI:
					ld.type = RT_LIGHT_TYPE_OMNI;
					break;
				case RSE::LIGHT_SPOT:
					ld.type = RT_LIGHT_TYPE_SPOT;
					break;
				case RSE::LIGHT_AREA:
					ld.type = RT_LIGHT_TYPE_AREA;
					break;
			}

			Color linear_col = ls->light_get_color(base).srgb_to_linear();
			float energy = compute_light_energy(base, type);
			ld.emission[0] = linear_col.r * energy;
			ld.emission[1] = linear_col.g * energy;
			ld.emission[2] = linear_col.b * energy;
			ld.radius = type == RSE::LIGHT_DIRECTIONAL ? Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE) * 0.5f) : ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
			ld.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
			ld.range = type == RSE::LIGHT_DIRECTIONAL ? 0.0f : ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
			ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f;
			ld.receiver_mask = ls->light_get_cull_mask(base);
			ld.caster_mask = ls->light_get_shadow_caster_mask(base);
			if (ls->light_has_shadow(base)) {
				ld.flags |= RT_LIGHT_FLAG_CASTS_SHADOW;
			}

			if (type == RSE::LIGHT_SPOT) {
				ld.inv_spot_attenuation = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
				ld.cos_spot_angle = Math::cos(Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE)));
			}

			if (type == RSE::LIGHT_AREA) {
				Vector2 area_size = ls->light_area_get_size(base);
				Vector3 axis_u = xform.basis.xform(Vector3(1.0f, 0.0f, 0.0f)).normalized() * area_size.x;
				Vector3 axis_v = xform.basis.xform(Vector3(0.0f, 1.0f, 0.0f)).normalized() * area_size.y;
				ld.axis_u[0] = axis_u.x;
				ld.axis_u[1] = axis_u.y;
				ld.axis_u[2] = axis_u.z;
				ld.axis_v[0] = axis_v.x;
				ld.axis_v[1] = axis_v.y;
				ld.axis_v[2] = axis_v.z;
				float area = axis_u.cross(axis_v).length();
				ld.inv_area = area > 0.0f ? 1.0f / area : 0.0f;
				if (ls->light_area_get_normalize_energy(base) && area > 0.0f) {
					ld.emission[0] /= area;
					ld.emission[1] /= area;
					ld.emission[2] /= area;
				}
			}

			RID projected_texture = type == RSE::LIGHT_AREA ? ls->light_area_get_texture(base) : ls->light_get_projector(base);
			const uint64_t texture_generation = ts->texture_get_content_generation(projected_texture);
			r_scene_signature = _rt_scene_hash(&texture_generation, sizeof(texture_generation), r_scene_signature);
			if (projected_texture.is_valid()) {
				Rect2 rect;
				RID atlas_texture;
				if (type == RSE::LIGHT_AREA) {
					rect = ts->area_light_atlas_get_texture_rect(projected_texture);
					atlas_texture = ts->area_light_atlas_get_texture();
				} else {
					rect = ts->decal_atlas_get_texture_rect(projected_texture);
					atlas_texture = ts->decal_atlas_get_texture_srgb();
					if (type == RSE::LIGHT_SPOT) {
						rect.position.y += rect.size.y;
						rect.size.y = -rect.size.y;
					} else if (type == RSE::LIGHT_OMNI) {
						rect.size.y *= 0.5f;
					}
				}
				if (atlas_texture.is_valid()) {
					ld.texture_index = bindless_block->add_texture(atlas_texture);
					ld.flags |= RT_LIGHT_FLAG_TEXTURED;
					ld.uv_rect[0] = rect.position.x;
					ld.uv_rect[1] = rect.position.y;
					ld.uv_rect[2] = rect.size.x;
					ld.uv_rect[3] = rect.size.y;
				}
			}
			RendererRD::MaterialStorage::store_transform_transposed_3x4(xform.affine_inverse(), ld.transform);

			RTLightKey key;
			key.instance_id = light_instance.get_id();
			key.resource_id = base.get_id();
			key.type = ld.type;
			if (type == RSE::LIGHT_DIRECTIONAL) {
				infinite_keys.push_back(key);
				infinite_lights.push_back(ld);
			} else {
				local_keys.push_back(key);
				local_lights.push_back(ld);
			}
		}
	}

	for (const RTEmissiveSource &source : emissive_sources) {
		for (uint32_t primitive = 0; primitive < source.primitive_count; primitive++) {
			RTLightKey key;
			key.instance_id = source.instance_id;
			key.resource_id = source.resource_id;
			key.surface_generation = source.surface_generation;
			key.primitive_index = source.key_primitive_offset + primitive;
			key.type = RT_LIGHT_TYPE_EMISSIVE_TRIANGLE;

			RT_LightData light = {};
			light.type = RT_LIGHT_TYPE_EMISSIVE_TRIANGLE;
			light.flags = RT_LIGHT_FLAG_CASTS_SHADOW;
			light.specular_amount = 1.0f;
			light.geometry_index = source.geometry_index;
			light.primitive_index = primitive;
			light.receiver_mask = UINT32_MAX;
			light.caster_mask = UINT32_MAX;
			light.topology_generation = source.topology_generation;
			light.texture_index = source.material->data.emission_texture_idx;
			light.emission[0] = source.material->data.emission_color[0] * source.material->data.emission_strength;
			light.emission[1] = source.material->data.emission_color[1] * source.material->data.emission_strength;
			light.emission[2] = source.material->data.emission_color[2] * source.material->data.emission_strength;
			if ((source.material->data.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0) {
				light.flags |= RT_LIGHT_FLAG_TEXTURED;
			}
			light.uv_rect[0] = source.material->data.uv1_scale[0];
			light.uv_rect[1] = source.material->data.uv1_scale[1];
			light.uv_rect[2] = source.material->data.uv1_offset[0];
			light.uv_rect[3] = source.material->data.uv1_offset[1];
			Transform3D emitter_to_rt = source.transform;
			emitter_to_rt.origin -= p_state->rt_origin;
			RendererRD::MaterialStorage::store_transform_transposed_3x4(emitter_to_rt, light.transform);
			local_keys.push_back(key);
			local_lights.push_back(light);
		}
	}

	p_state->environment_texture = RID();
	if (p_render_data->environment.is_valid()) {
		RID sky_rid = owner->environment_get_sky(p_render_data->environment);
		if (sky_rid.is_valid()) {
			RID environment_texture = owner->get_sky()->sky_get_radiance_texture_rd(sky_rid);
			if (environment_texture.is_valid()) {
				RTLightKey key;
				key.instance_id = p_render_data->environment.get_id();
				key.resource_id = sky_rid.get_id();
				key.type = RT_LIGHT_TYPE_ENVIRONMENT;
				RT_LightData light = {};
				light.type = RT_LIGHT_TYPE_ENVIRONMENT;
				const float environment_energy = owner->environment_get_bg_energy_multiplier(p_render_data->environment) * owner->environment_get_bg_intensity(p_render_data->environment);
				light.emission[0] = environment_energy;
				light.emission[1] = environment_energy;
				light.emission[2] = environment_energy;
				light.flags = RT_LIGHT_FLAG_CASTS_SHADOW;
				light.specular_amount = 1.0f;
				light.receiver_mask = UINT32_MAX;
				light.caster_mask = UINT32_MAX;
				environment_keys.push_back(key);
				environment_lights.push_back(light);
				p_state->environment_texture = environment_texture;
			}
		}
	}

	LocalVector<RTLightKey> current_keys;
	LocalVector<RT_LightData> current_lights;
	for (uint32_t i = 0; i < local_keys.size(); i++) {
		current_keys.push_back(local_keys[i]);
		current_lights.push_back(local_lights[i]);
	}
	for (uint32_t i = 0; i < infinite_keys.size(); i++) {
		current_keys.push_back(infinite_keys[i]);
		current_lights.push_back(infinite_lights[i]);
	}
	for (uint32_t i = 0; i < environment_keys.size(); i++) {
		current_keys.push_back(environment_keys[i]);
		current_lights.push_back(environment_lights[i]);
	}

	ERR_FAIL_COND_MSG(current_lights.size() > 0x7FFFFFFFu, "The RTXDI light registry exceeds the reservoir light-index range.");
	ERR_FAIL_COND_MSG(uint64_t(current_lights.size()) * sizeof(RT_LightData) > UINT32_MAX, "The RTXDI light registry exceeds RenderingDevice buffer limits.");

	const uint32_t previous_index = p_state->current_light_snapshot;
	const uint32_t current_index = p_state->light_history_valid ? (previous_index ^ 1u) : previous_index;
	RTLightSnapshot &previous = p_state->light_snapshots[previous_index];
	RTLightSnapshot &current = p_state->light_snapshots[current_index];

	LocalVector<uint32_t> current_to_previous;
	LocalVector<uint32_t> previous_to_current;
	current_to_previous.resize(current_keys.size());
	previous_to_current.resize(p_state->light_history_valid ? previous.keys.size() : 0);
	for (uint32_t i = 0; i < current_to_previous.size(); i++) {
		current_to_previous[i] = UINT32_MAX;
	}
	for (uint32_t i = 0; i < previous_to_current.size(); i++) {
		previous_to_current[i] = UINT32_MAX;
	}

	if (p_state->light_history_valid) {
		HashMap<RTLightKey, uint32_t> previous_indices;
		for (uint32_t i = 0; i < previous.keys.size(); i++) {
			previous_indices.insert(previous.keys[i], i);
		}
		for (uint32_t i = 0; i < current_keys.size(); i++) {
			const uint32_t *previous_light = previous_indices.getptr(current_keys[i]);
			if (previous_light) {
				current_to_previous[i] = *previous_light;
				previous_to_current[*previous_light] = i;
			}
		}
	}

	current.keys = current_keys;
	current.lights = current_lights;
	current.parameters.local_first = 0;
	current.parameters.local_count = local_lights.size();
	current.parameters.infinite_first = local_lights.size();
	current.parameters.infinite_count = infinite_lights.size();
	current.parameters.environment_index = local_lights.size() + infinite_lights.size();
	current.parameters.environment_present = environment_lights.is_empty() ? 0 : 1;
	current.parameters.total_count = current_lights.size();
	current.parameters.previous_count = p_state->light_history_valid ? previous.keys.size() : 0;

	auto update_or_grow = [](RID &p_buffer, uint32_t &p_capacity, const void *p_data, uint32_t p_size, const String &p_name) {
		uint32_t required_size = MAX(p_size, 4u);
		if (required_size > p_capacity) {
			if (p_buffer.is_valid()) {
				RD::get_singleton()->free_rid(p_buffer);
			}
			Vector<uint8_t> initial_data;
			initial_data.resize(required_size);
			memset(initial_data.ptrw(), 0xFF, required_size);
			if (p_size > 0) {
				memcpy(initial_data.ptrw(), p_data, p_size);
			}
			p_buffer = RD::get_singleton()->storage_buffer_create(required_size, initial_data);
			p_capacity = required_size;
			RD::get_singleton()->set_resource_name(p_buffer, p_name);
		} else if (p_size > 0) {
			RD::get_singleton()->buffer_update(p_buffer, 0, p_size, p_data);
		}
	};

	update_or_grow(current.light_buffer, current.light_buffer_capacity, current_lights.ptr(), current_lights.size() * sizeof(RT_LightData), "RTXDI Light Snapshot");
	update_or_grow(current.current_to_previous_buffer, current.current_to_previous_capacity, current_to_previous.ptr(), current_to_previous.size() * sizeof(uint32_t), "RTXDI Current To Previous Light Map");
	update_or_grow(current.previous_to_current_buffer, current.previous_to_current_capacity, previous_to_current.ptr(), previous_to_current.size() * sizeof(uint32_t), "RTXDI Previous To Current Light Map");
	if (!current.parameters_buffer.is_valid()) {
		current.parameters_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(RT_LightBufferParameters));
		RD::get_singleton()->set_resource_name(current.parameters_buffer, "RTXDI Light Parameters");
	}
	RD::get_singleton()->buffer_update(current.parameters_buffer, 0, sizeof(RT_LightBufferParameters), &current.parameters);

	p_state->current_light_snapshot = current_index;
	p_state->light_history_valid = true;
}

// ---------------------------------------------------------------------------
// Trace-time buffer dependencies
// ---------------------------------------------------------------------------

void RenderRaytracing::register_compute_buffer_dependencies(RD::ComputeListID p_list) {
	RD *rd = RD::get_singleton();
	for (RID buffer : geometry_buffer_dependencies) {
		rd->compute_list_add_buffer_dependency(p_list, buffer);
	}

	if (mat_ubo_pool_buffer.is_valid()) {
		rd->compute_list_add_buffer_dependency(p_list, mat_ubo_pool_buffer);
	}

	for (uint32_t i = 0; i < deformed_active_this_frame.size(); i++) {
		RTDeformedCacheEntry *e = deformed_pool.get_or_null(deformed_active_this_frame[i]);
		if (!e) {
			continue;
		}
		if (e->owned_vb_full.is_valid()) {
			rd->compute_list_add_buffer_dependency(p_list, e->owned_vb_full);
		}
		if (e->prev_pos_vb.is_valid()) {
			rd->compute_list_add_buffer_dependency(p_list, e->prev_pos_vb);
		}
	}

	for (uint32_t i = 0; i < merged_mm_active_this_frame.size(); i++) {
		RTMergedMMEntry *e = merged_mm_pool.get_or_null(merged_mm_active_this_frame[i]);
		if (!e) {
			continue;
		}
		if (e->previous_position_buffer.is_valid()) {
			rd->compute_list_add_buffer_dependency(p_list, e->previous_position_buffer);
		}
		if (e->merged_vtx_buffer.is_valid()) {
			rd->compute_list_add_buffer_dependency(p_list, e->merged_vtx_buffer);
		}
		if (e->merged_attr_buffer.is_valid()) {
			rd->compute_list_add_buffer_dependency(p_list, e->merged_attr_buffer);
		}
		if (e->replicated_idx_buffer.is_valid()) {
			rd->compute_list_add_buffer_dependency(p_list, e->replicated_idx_buffer);
		}
	}
}

bool RenderRaytracing::create_material_pipeline(const RTViewportState *p_state, Span<RD::PipelineShader> p_raygen_shaders, Span<RD::PipelineShader> p_miss_shaders, uint32_t p_recursion_depth, RID &r_pipeline, RID &r_sbt) const {
	ERR_FAIL_NULL_V(p_state, false);
	ERR_FAIL_COND_V(r_pipeline.is_valid() || r_sbt.is_valid(), false);
	Vector<RD::HitGroup> groups;
	for (RID shader : p_state->hit_programs) {
		ERR_FAIL_COND_V(shader.is_null(), false);
		RD::HitGroup group;
		group.closest_hit_shader.shader = shader;
		group.any_hit_shader.shader = shader;
		groups.push_back(group);
	}
	RD *rd = RD::get_singleton();
	RID pipeline = rd->raytracing_pipeline_create(p_raygen_shaders, p_miss_shaders, groups, p_recursion_depth);
	ERR_FAIL_COND_V(pipeline.is_null(), false);
	const uint32_t count = MAX(p_state->geometry_hit_groups.size(), 1);
	RID sbt = rd->hit_sbt_create(pipeline, count);
	if (sbt.is_null()) {
		rd->free_rid(pipeline);
		return false;
	}
	RD::HitShaderBindingTableRange range = rd->hit_sbt_range_alloc(sbt, count);
	Vector<uint32_t> indices = p_state->geometry_hit_groups;
	if (indices.is_empty()) {
		indices.push_back(0);
	}
	if (uint32_t(range) != 0 || rd->hit_sbt_range_update(sbt, range, 0, indices) != OK) {
		rd->free_rid(sbt);
		rd->free_rid(pipeline);
		return false;
	}
	r_pipeline = pipeline;
	r_sbt = sbt;
	print_verbose(vformat("Native RT material pipeline created: %d hit programs, %d geometry records, pipeline %d, SBT %d.", groups.size(), count, pipeline.get_id(), sbt.get_id()));
	return true;
}

bool RenderRaytracing::update_material_pipeline(RTViewportState *p_state) {
	Vector<RID> programs;
	Vector<uint32_t> indices;
	HashMap<RID, uint32_t> program_indices;
	for (RID shader : geometry_material_programs) {
		ERR_FAIL_COND_V_MSG(shader.is_null(), false, "The RT scene has no valid native material hit program.");
		if (!program_indices.has(shader)) {
			program_indices[shader] = programs.size();
			programs.push_back(shader);
		}
		indices.push_back(program_indices[shader]);
	}
	RID default_program = owner->scene_shader.default_material_shader_ptr->get_hit_shader();
	ERR_FAIL_COND_V(default_program.is_null(), false);
	if (programs.is_empty()) {
		programs.push_back(default_program);
	}
	RD *rd = RD::get_singleton();
	if (p_state->hit_programs == programs && p_state->geometry_hit_groups == indices && rd->raytracing_pipeline_is_valid(p_state->material_pipeline)) {
		return true;
	}
	p_state->hit_programs = programs;
	p_state->geometry_hit_groups = indices;
	if (rd->raytracing_pipeline_is_valid(p_state->material_pipeline)) {
		rd->free_rid(p_state->material_sbt);
		rd->free_rid(p_state->material_pipeline);
	}
	p_state->material_sbt = RID();
	p_state->material_pipeline = RID();
	RD::PipelineShader entry;
	entry.shader = default_program;
	return create_material_pipeline(p_state, { &entry, 1 }, { &entry, 1 }, 1, p_state->material_pipeline, p_state->material_sbt);
}

void RenderRaytracing::register_raytracing_buffer_dependencies(RD::RaytracingListID p_list) {
	RD *rd = RD::get_singleton();
	for (RID buffer : geometry_buffer_dependencies) {
		rd->raytracing_list_add_buffer_dependency(p_list, buffer);
	}
	if (mat_ubo_pool_buffer.is_valid()) {
		rd->raytracing_list_add_buffer_dependency(p_list, mat_ubo_pool_buffer);
	}
	for (RID handle : deformed_active_this_frame) {
		RTDeformedCacheEntry *entry = deformed_pool.get_or_null(handle);
		if (!entry) {
			continue;
		}
		for (RID buffer : { entry->owned_vb_full, entry->prev_pos_vb }) {
			if (buffer.is_valid()) {
				rd->raytracing_list_add_buffer_dependency(p_list, buffer);
			}
		}
	}
	for (RID handle : merged_mm_active_this_frame) {
		RTMergedMMEntry *entry = merged_mm_pool.get_or_null(handle);
		if (!entry) {
			continue;
		}
		for (RID buffer : { entry->previous_position_buffer, entry->merged_vtx_buffer, entry->merged_attr_buffer, entry->replicated_idx_buffer }) {
			if (buffer.is_valid()) {
				rd->raytracing_list_add_buffer_dependency(p_list, buffer);
			}
		}
	}
}

RID RenderRaytracing::create_material_uniform_set(RTViewportState *p_state, RID p_scene_data_buffer, RID p_shader, RID p_ray_buffer, RID p_result_buffer, uint32_t p_ray_count) {
	ERR_FAIL_NULL_V(p_state, RID());
	RD *rd = RD::get_singleton();
	struct alignas(16) MaterialFrame {
		float origin[4] = {};
		uint32_t counts[4] = {};
	} frame;
	for (uint32_t axis = 0; axis < 3; axis++) {
		frame.origin[axis] = p_state->rt_origin[axis];
	}
	frame.counts[0] = p_state->geometry_hit_groups.size();
	frame.counts[1] = p_state->decal_count;
	frame.counts[2] = p_ray_count;
	frame.counts[3] = owner->decals_get_filter();
	if (p_state->material_frame_buffer.is_null()) {
		p_state->material_frame_buffer = rd->uniform_buffer_create(sizeof(frame));
	}
	ERR_FAIL_COND_V(p_state->material_frame_buffer.is_null(), RID());
	rd->buffer_update(p_state->material_frame_buffer, 0, sizeof(frame), &frame);
	if (p_state->decal_buffer.is_null()) {
		p_state->decal_buffer = rd->storage_buffer_create(16);
		p_state->decal_buffer_capacity = 16;
	}
	RendererRD::MaterialStorage *materials = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *textures = RendererRD::TextureStorage::get_singleton();
	Vector<RD::Uniform> uniforms;
	auto append = [&](RD::UniformType p_type, uint32_t p_binding, RID p_resource) {
		RD::Uniform uniform;
		uniform.uniform_type = p_type;
		uniform.binding = p_binding;
		uniform.append_id(p_resource);
		uniforms.push_back(uniform);
	};
	append(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_scene_data_buffer);
	append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, materials->global_shader_uniforms_get_storage_buffer());
	append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_state->geometry_buffer);
	append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_state->material_buffer);
	append(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 4, p_state->tlas);
	append(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, p_state->material_frame_buffer);
	append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_state->decal_buffer);
	append(RD::UNIFORM_TYPE_TEXTURE, 7, textures->decal_atlas_get_texture());
	append(RD::UNIFORM_TYPE_TEXTURE, 8, textures->decal_atlas_get_texture_srgb());
	const RSE::CanvasItemTextureFilter filters[] = {
		RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST,
		RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR,
		RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
		RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC,
		RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC,
	};
	for (uint32_t repeat = 0; repeat < 2; repeat++) {
		for (uint32_t filter = 0; filter < 6; filter++) {
			append(RD::UNIFORM_TYPE_SAMPLER, 9 + repeat * 6 + filter, materials->sampler_rd_get_default(filters[filter], repeat ? RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED : RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		}
	}
	append(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 21, p_state->frame_constants_buffer);
	if ((p_ray_buffer.is_null() || p_result_buffer.is_null()) && p_state->material_unused_buffer.is_null()) {
		p_state->material_unused_buffer = rd->storage_buffer_create(16);
	}
	append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, p_ray_buffer.is_valid() ? p_ray_buffer : p_state->material_unused_buffer);
	append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, p_result_buffer.is_valid() ? p_result_buffer : p_state->material_unused_buffer);
	return rd->uniform_set_create(uniforms, p_shader, 0, true);
}

bool RenderRaytracing::trace_material_rays(RTViewportState *p_state, RID p_scene_data_buffer, RID p_ray_buffer, RID p_result_buffer, uint32_t p_ray_count) {
	ERR_FAIL_NULL_V(p_state, false);
	RD *rd = RD::get_singleton();
	ERR_FAIL_COND_V(!rd->raytracing_pipeline_is_valid(p_state->material_pipeline) || !p_state->material_sbt.is_valid(), false);
	ERR_FAIL_COND_V(!p_scene_data_buffer.is_valid() || !p_ray_buffer.is_valid() || !p_result_buffer.is_valid(), false);
	if (p_ray_count == 0) {
		return true;
	}
	RID shader = owner->scene_shader.default_material_shader_ptr->get_hit_shader();
	RID uniform_set = create_material_uniform_set(p_state, p_scene_data_buffer, shader, p_ray_buffer, p_result_buffer, p_ray_count);
	ERR_FAIL_COND_V(uniform_set.is_null(), false);
	RID bindless_set = get_bindless_uniform_set(shader);
	if (bindless_set.is_null()) {
		rd->free_rid(uniform_set);
		return false;
	}
	RD::RaytracingListID list = rd->raytracing_list_begin();
	rd->raytracing_list_bind_raytracing_pipeline(list, p_state->material_pipeline);
	rd->raytracing_list_bind_uniform_set(list, uniform_set, 0);
	rd->raytracing_list_bind_uniform_set(list, bindless_set, 1);
	register_raytracing_buffer_dependencies(list);
	rd->raytracing_list_trace_rays(list, 0, p_state->material_sbt, p_ray_count, 1, 1);
	rd->raytracing_list_end();
	rd->free_rid(uniform_set);
	return true;
}

void RenderRaytracing::_upload_persistent_record(RID &r_buffer, uint32_t &r_capacity, uint32_t p_stride, uint32_t p_index, const void *p_data) {
	if (p_index >= r_capacity || r_buffer.is_null()) {
		uint32_t capacity = 64;
		while (capacity <= p_index) {
			ERR_FAIL_COND(capacity > UINT32_MAX / 2);
			capacity *= 2;
		}
		ERR_FAIL_COND(uint64_t(capacity) * p_stride > UINT32_MAX);
		Vector<uint8_t> initial;
		initial.resize(capacity * p_stride);
		memset(initial.ptrw(), 0, initial.size());
		RID buffer = RD::get_singleton()->storage_buffer_create(initial.size(), initial, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
		ERR_FAIL_COND(buffer.is_null());
		if (r_buffer.is_valid()) {
			RD::get_singleton()->buffer_copy(r_buffer, buffer, 0, 0, r_capacity * p_stride);
			RD::get_singleton()->free_rid(r_buffer);
		}
		r_buffer = buffer;
		r_capacity = capacity;
	}
	RD::get_singleton()->buffer_update(r_buffer, p_index * p_stride, p_stride, p_data);
}

void RenderRaytracing::_update_persistent_material(RID p_material, RTMaterialData *p_data, uint64_t p_generation) {
	if (p_data->global_buffer_index == UINT32_MAX) {
		p_data->global_buffer_index = allocate_material_slot();
	}
	const uint32_t index = p_data->global_buffer_index;
	if (index >= persistent_materials.size()) {
		persistent_materials.resize(index + 1);
		persistent_material_uniform_buffers.resize(index + 1);
	}
	_reference_persistent_buffer(persistent_material_uniform_buffers[index], false);
	persistent_material_uniform_buffers[index] = p_data->uniform_buffer;
	_reference_persistent_buffer(p_data->uniform_buffer, true);
	RTPersistentMaterialData &material = persistent_materials[index];
	material.identity = p_material.get_id();
	material.generation = p_generation;
	material.data = p_data->data;
	_upload_persistent_record(persistent_material_buffer, persistent_material_capacity, sizeof(RTPersistentMaterialData), index, &material);
	persistent_scene_generation++;
}

void RenderRaytracing::update_persistent_instance(RenderGeometryInstance *p_instance) {
	RenderForwardClustered::GeometryInstanceForwardClustered *instance = static_cast<RenderForwardClustered::GeometryInstanceForwardClustered *>(p_instance);
	if (!instance->data) {
		return;
	}
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	auto allocate = [&](auto &r_slots, LocalVector<uint32_t> &r_free_slots) -> uint64_t {
		uint32_t index = r_slots.size();
		for (uint32_t i = 0; i < r_free_slots.size(); i++) {
			if (r_slots[r_free_slots[i]].retirement <= completed) {
				index = r_free_slots[i];
				r_free_slots.remove_at_unordered(i);
				break;
			}
		}
		if (index == r_slots.size()) {
			r_slots.resize(index + 1);
		}
		r_slots[index].generation++;
		r_slots[index].retirement = 0;
		return (uint64_t(r_slots[index].generation) << 32) | uint64_t(index + 1);
	};
	if (instance->persistent_instance == 0) {
		instance->persistent_instance = allocate(persistent_instances, persistent_instance_free_slots);
	}
	const uint32_t instance_index = uint32_t(instance->persistent_instance) - 1;
	RTPersistentInstanceData data;
	data.handle = instance->persistent_instance;
	data.identity = instance->instance_rid.get_id();
	data.scenario = instance->scenario_rid.get_id();
	RID mesh;
	if (instance->data->base_type == RSE::INSTANCE_MESH) {
		mesh = instance->data->base;
	} else if (instance->data->base_type == RSE::INSTANCE_MULTIMESH) {
		mesh = mesh_storage->multimesh_get_mesh(instance->data->base);
		RID buffer = mesh_storage->multimesh_get_gpu_buffer(instance->data->base);
		if (buffer.is_valid()) {
			data.multimesh_address = RD::get_singleton()->buffer_get_device_address(buffer);
		}
		data.multimesh_generation = mesh_storage->multimesh_get_rt_generation(instance->data->base);
		data.multimesh_stride = mesh_storage->multimesh_get_stride(instance->data->base);
		data.multimesh_current_offset = mesh_storage->multimesh_get_current_instance_offset(instance->data->base);
		data.multimesh_previous_offset = mesh_storage->multimesh_get_previous_instance_offset(instance->data->base);
		data.multimesh_count = mesh_storage->multimesh_get_instances_to_draw(instance->data->base);
	}
	PersistentInstanceSlot &instance_slot = persistent_instances[instance_index];
	for (RID buffer : instance_slot.dependencies) {
		_reference_persistent_buffer(buffer, false);
	}
	instance_slot.dependencies.clear();
	if (instance->data->base_type == RSE::INSTANCE_MULTIMESH) {
		RID buffer = mesh_storage->multimesh_get_gpu_buffer(instance->data->base);
		if (buffer.is_valid()) {
			instance_slot.dependencies.push_back(buffer);
		}
	}
	RID asset = mesh_storage->mesh_get_micro_geometry_asset(mesh);
	data.asset = asset.get_id();
	if (asset.is_valid()) {
		RID descriptor = mesh_storage->get_micro_geometry_storage()->get_asset_buffer(asset);
		data.asset_address = RD::get_singleton()->buffer_get_device_address(descriptor);
		mesh_storage->get_micro_geometry_storage()->get_dependencies(asset, instance_slot.dependencies);
	}
	for (RID buffer : instance_slot.dependencies) {
		_reference_persistent_buffer(buffer, true);
	}
	const Transform3D &previous = instance->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TELEPORTED ? instance->transform : instance->prev_transform;
	RendererRD::MaterialStorage::store_transform_transposed_3x4(instance->transform, data.transform);
	RendererRD::MaterialStorage::store_transform_transposed_3x4(previous, data.previous_transform);
#ifdef REAL_T_IS_DOUBLE
	for (int axis = 0; axis < 3; axis++) {
		RendererRD::MaterialStorage::split_double(instance->transform.origin[axis], &data.transform[axis * 4 + 3], &data.origin_low[axis]);
		RendererRD::MaterialStorage::split_double(previous.origin[axis], &data.previous_transform[axis * 4 + 3], &data.previous_origin_low[axis]);
	}
#endif
	for (int axis = 0; axis < 3; axis++) {
		data.aabb_position[axis] = instance->data->aabb.position[axis];
		data.aabb_size[axis] = instance->data->aabb.size[axis];
	}
	data.flags = instance->base_flags;
	data.layer_mask = instance->layer_mask;
	data.instance_uniforms_offset = uint32_t(instance->shader_uniforms_offset);
	data.visible = instance->scene_visible && instance->scenario_rid.is_valid();
	data.shadows = instance->scene_shadows;
	data.deformed = instance->mesh_instance.is_valid() || instance->rt_procedural != nullptr;
	data.fade_near_begin = instance->fade_near ? instance->fade_near_begin : 0;
	data.fade_near_end = instance->fade_near ? instance->fade_near_end : 0;
	data.fade_far_begin = instance->fade_far ? instance->fade_far_begin : 0;
	data.fade_far_end = instance->fade_far ? instance->fade_far_end : 0;
	data.force_alpha = instance->force_alpha;
	data.parent_fade_alpha = instance->parent_fade_alpha;
	data.lod_bias = instance->lod_bias;
	data.model_scale = instance->lod_model_scale;
	data.lightmap = instance->lightmap_instance.get_id();
	data.lightmap_slice = instance->lightmap_slice_index;
	data.lightmap_uv_scale[0] = instance->lightmap_uv_scale.position.x;
	data.lightmap_uv_scale[1] = instance->lightmap_uv_scale.position.y;
	data.lightmap_uv_scale[2] = instance->lightmap_uv_scale.size.x;
	data.lightmap_uv_scale[3] = instance->lightmap_uv_scale.size.y;
	if (instance->lightmap_sh) {
		memcpy(data.lightmap_sh, instance->lightmap_sh->sh, sizeof(data.lightmap_sh));
	}
	if (instance->persistent_surfaces_dirty) {
		Vector<uint64_t> previous_handles = instance->persistent_surfaces;
		LocalVector<bool> used;
		used.resize_initialized(previous_handles.size());
		Vector<uint64_t> handles;
		LocalVector<uint32_t> pass_indices;
		HashMap<uint32_t, uint32_t> passes;
		for (auto *surface = instance->surface_caches; surface; surface = surface->next) {
			const uint32_t pass = passes[surface->surface_index]++;
			uint64_t handle = 0;
			for (uint32_t i = 0; i < previous_handles.size(); i++) {
				const RTPersistentSurfaceData &record = persistent_surfaces[uint32_t(previous_handles[i]) - 1].data;
				if (!used[i] && record.source_surface == surface->surface_index && record.pass_index == pass) {
					handle = previous_handles[i];
					used[i] = true;
					break;
				}
			}
			if (handle == 0) {
				handle = allocate(persistent_surfaces, persistent_surface_free_slots);
			}
			handles.push_back(handle);
			pass_indices.push_back(pass);
		}
		for (uint32_t i = 0; i < previous_handles.size(); i++) {
			if (!used[i]) {
				release_persistent_instance(0, Vector<uint64_t>({ previous_handles[i] }));
			}
		}
		instance->persistent_surfaces = handles;
		const uint32_t surface_count = handles.size();
		uint32_t ordinal = 0;
		for (auto *surface = instance->surface_caches; surface; surface = surface->next, ordinal++) {
			RTPersistentSurfaceData record;
			record.handle = instance->persistent_surfaces[ordinal];
			record.instance = data.handle;
			record.next_surface = ordinal + 1 < surface_count ? instance->persistent_surfaces[ordinal + 1] : 0;
			RID material = surface->material_rid.is_valid() ? surface->material_rid : owner->scene_shader.default_material;
			RTMaterialData *native_material = process_material(material, material_storage->material_get_rt_invalidation_counter(material));
			record.material = material.get_id();
			record.material_generation = material_storage->material_get_rt_content_generation(material);
			record.material_slot = native_material->global_buffer_index;
			record.source_surface = surface->surface_index;
			record.pass_index = pass_indices[ordinal];
			record.flags = surface->flags;
			record.rt_pass_flags = surface->rt_pass_flags;
			record.material_flags = surface->rtxdi_material_flags;
			record.force_finest = native_material->is_custom_shader || (surface->shader && surface->shader->uses_emission);
			surface->persistent_surface = record.handle;
			const uint32_t index = uint32_t(record.handle) - 1;
			if (memcmp(&persistent_surfaces[index].data, &record, sizeof(record)) != 0) {
				persistent_surfaces[index].data = record;
				_upload_persistent_record(persistent_surface_buffer, persistent_surface_capacity, sizeof(record), index, &record);
			}
		}
		instance->persistent_surfaces_dirty = false;
	}
	data.first_surface = instance->persistent_surfaces.is_empty() ? 0 : instance->persistent_surfaces[0];
	data.surface_count = instance->persistent_surfaces.size();
	persistent_instances[instance_index].data = data;
	_upload_persistent_record(persistent_instance_buffer, persistent_instance_capacity, sizeof(data), instance_index, &data);
	persistent_scene_generation++;
}

void RenderRaytracing::release_persistent_instance(uint64_t p_handle, const Vector<uint64_t> &p_surfaces) {
	for (uint64_t handle : p_surfaces) {
		const uint32_t index = uint32_t(handle) - 1;
		ERR_CONTINUE(index >= persistent_surfaces.size() || persistent_surfaces[index].data.handle != handle);
		auto &slot = persistent_surfaces[index];
		slot.data = RTPersistentSurfaceData();
		_upload_persistent_record(persistent_surface_buffer, persistent_surface_capacity, sizeof(slot.data), index, &slot.data);
		slot.retirement = RD::get_singleton()->get_pending_submission_serial();
		persistent_surface_free_slots.push_back(index);
	}
	if (p_handle != 0) {
		const uint32_t index = uint32_t(p_handle) - 1;
		ERR_FAIL_COND(index >= persistent_instances.size() || persistent_instances[index].data.handle != p_handle);
		auto &slot = persistent_instances[index];
		for (RID buffer : slot.dependencies) {
			_reference_persistent_buffer(buffer, false);
		}
		slot.dependencies.clear();
		slot.data = RTPersistentInstanceData();
		_upload_persistent_record(persistent_instance_buffer, persistent_instance_capacity, sizeof(slot.data), index, &slot.data);
		slot.retirement = RD::get_singleton()->get_pending_submission_serial();
		persistent_instance_free_slots.push_back(index);
	}
	persistent_scene_generation++;
}

uint64_t RenderRaytracing::get_persistent_memory_bytes() const {
	return uint64_t(persistent_instance_capacity) * sizeof(RTPersistentInstanceData) + uint64_t(persistent_surface_capacity) * sizeof(RTPersistentSurfaceData) + uint64_t(persistent_material_capacity) * sizeof(RTPersistentMaterialData);
}

uint64_t RenderRaytracing::get_micro_geometry_memory_bytes() const {
	uint64_t bytes = 0;
	for (const auto &entry : viewport_states) {
		if (entry.value->micro_geometry) {
			bytes += entry.value->micro_geometry->memory_bytes;
			if (entry.value->tlas.is_valid()) {
				bytes += RD::get_singleton()->acceleration_structure_get_memory_usage(entry.value->tlas);
			}
		}
	}
	for (const auto *build : retired_micro_geometry) {
		bytes += build->memory_bytes;
	}
	for (const auto &retired : retired_tlas_memory) {
		if (retired.submission > RD::get_singleton()->get_completed_submission_serial()) {
			bytes += retired.bytes;
		}
	}
	return bytes;
}

uint64_t RenderRaytracing::get_micro_geometry_as_memory_bytes() const {
	uint64_t bytes = 0;
	for (const auto &entry : viewport_states) {
		if (entry.value->micro_geometry) {
			bytes += entry.value->micro_geometry->as_memory_bytes;
			if (entry.value->tlas.is_valid()) {
				bytes += RD::get_singleton()->acceleration_structure_get_memory_usage(entry.value->tlas);
			}
		}
	}
	for (const auto *build : retired_micro_geometry) {
		bytes += build->as_memory_bytes;
	}
	for (const auto &retired : retired_tlas_memory) {
		if (retired.submission > RD::get_singleton()->get_completed_submission_serial()) {
			bytes += retired.bytes;
		}
	}
	return bytes;
}

void RenderRaytracing::_free_persistent_buffers() {
	for (RID buffer : { persistent_instance_buffer, persistent_surface_buffer, persistent_material_buffer }) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
}

void RenderRaytracing::_reference_persistent_buffer(RID p_buffer, bool p_add) {
	if (p_buffer.is_null()) {
		return;
	}
	if (p_add) {
		persistent_buffer_references[p_buffer]++;
	} else {
		uint32_t *count = persistent_buffer_references.getptr(p_buffer);
		if (count && --*count == 0) {
			persistent_buffer_references.erase(p_buffer);
		}
	}
}

void RenderRaytracing::get_persistent_buffer_dependencies(Vector<RID> &r_buffers) const {
	for (RID buffer : { persistent_instance_buffer, persistent_surface_buffer, persistent_material_buffer, mat_ubo_pool_buffer }) {
		if (buffer.is_valid()) {
			r_buffers.push_back(buffer);
		}
	}
	for (const KeyValue<RID, uint32_t> &entry : persistent_buffer_references) {
		r_buffers.push_back(entry.key);
	}
}
