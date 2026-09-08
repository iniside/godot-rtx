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
	if (ddgi_effect) {
		memdelete(ddgi_effect);
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
	Vector3 rt_origin;
	for (int axis = 0; axis < 3; axis++) {
		rt_origin[axis] = Math::floor(scene.cam_transform.origin[axis] / real_t(1024.0)) * real_t(1024.0);
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

bool RenderRaytracing::_prepare_ddgi(RTViewportState *p_state) {
	const RendererEnvironmentStorage::RaytracingSettings &settings = p_state->settings;
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
	if (!ddgi_effect->prepare(*p_state->ddgi, p_state->camera_transform.origin, p_state->rt_origin, p_state->ddgi_history_epoch, settings.ddgi_updates_per_frame)) {
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
					_release_cluster_blas(entry->ptr, false);
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(surface_chunks[i]);
		}
	}
	surface_chunks.clear();

	for (const RTPendingClusterBuild &pending : pending_cluster_builds) {
		if (pending.src_infos_buffer.is_valid()) {
			rd->free_rid(pending.src_infos_buffer);
		}
	}
	pending_cluster_builds.clear();

	for (const RTDeferredResourceFree &deferred : cluster_deferred_frees) {
		if (deferred.resource.is_valid()) {
			rd->free_rid(deferred.resource);
		}
	}
	cluster_deferred_frees.clear();

	if (clas_scratch_buffer.is_valid()) {
		rd->free_rid(clas_scratch_buffer);
		clas_scratch_buffer = RID();
	}
	clas_scratch_capacity = 0;

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

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RD *rd = RD::get_singleton();

	for (const RTPendingClusterBuild &pending : pending_cluster_builds) {
		if (pending.src_infos_buffer.is_valid()) {
			cluster_deferred_frees.push_back({ pending.src_infos_buffer, current_frame });
		}
	}
	pending_cluster_builds.clear();

	// Cluster build inputs stay alive until the graph that recorded them has been
	// submitted; freeing one in its own frame would destroy a tracker it still holds.
	for (uint32_t i = cluster_deferred_frees.size(); i > 0; i--) {
		RTDeferredResourceFree &deferred = cluster_deferred_frees[i - 1];
		if (deferred.frame == current_frame) {
			continue;
		}
		if (deferred.resource.is_valid()) {
			rd->free_rid(deferred.resource);
		}
		cluster_deferred_frees.remove_at_unordered(i - 1);
	}

	_sweep_dead_cluster_surfaces();

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

	if (!needs_refresh && entry->ptr->blas.is_valid()) {
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
		_release_cluster_blas(entry->ptr, false);
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

// Layout must match VkClusterAccelerationStructureBuildTriangleClusterInfoNV
// (thirdparty/vulkan/include/vulkan/vulkan_core.h:23292).
struct RTClusterTriangleInfo {
	uint32_t cluster_id = 0;
	uint32_t cluster_flags = 0;
	uint32_t packed_counts = 0;
	uint32_t base_geometry_index_and_flags = 0;
	uint16_t index_buffer_stride = 0;
	uint16_t vertex_buffer_stride = 0;
	uint16_t geometry_index_and_flags_buffer_stride = 0;
	uint16_t opacity_micromap_index_buffer_stride = 0;
	uint64_t index_buffer = 0;
	uint64_t vertex_buffer = 0;
	uint64_t geometry_index_and_flags_buffer = 0;
	uint64_t opacity_micromap_array = 0;
	uint64_t opacity_micromap_index_buffer = 0;
};
static_assert(sizeof(RTClusterTriangleInfo) == 64, "RTClusterTriangleInfo must match the Vulkan cluster triangle info layout");

enum : uint32_t {
	RT_CLUSTER_RECORD_SIZE = 16,
	RT_CLUSTER_INDEX_TYPE_8BIT = 1,
};

void RenderRaytracing::_release_cluster_blas(RTSurfaceData *p_surf_data, bool p_deferred) {
	if (!p_surf_data) {
		return;
	}

	RD *rd = RD::get_singleton();
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RID *owned[] = {
		&p_surf_data->blas,
		&p_surf_data->clas_buffer,
		&p_surf_data->clas_addresses_buffer,
		&p_surf_data->cluster_remap_buffer,
		&p_surf_data->clas_count_buffer,
	};
	for (RID *rid : owned) {
		if (!rid->is_valid()) {
			continue;
		}
		if (p_deferred) {
			cluster_deferred_frees.push_back({ *rid, current_frame });
		} else {
			rd->free_rid(*rid);
		}
		*rid = RID();
	}

	p_surf_data->cluster_count = 0;
	p_surf_data->is_clustered = false;
	p_surf_data->geometry.flags &= ~(uint32_t)RT_GEOM_FLAG_CLUSTERED;
	p_surf_data->geometry.cluster_count = 0;
	p_surf_data->geometry.cluster_remap_address_lo = 0;
	p_surf_data->geometry.cluster_remap_address_hi = 0;
}

// A cluster BLAS holds no RD dependency on its mesh, so a freed mesh leaves its buffers
// resident until this walk reaches them; one chunk per call keeps it off the frame budget.
void RenderRaytracing::_sweep_dead_cluster_surfaces() {
	if (surface_chunks.is_empty()) {
		return;
	}

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	cluster_sweep_chunk = (cluster_sweep_chunk + 1) % surface_chunks.size();
	RTCacheEntry *chunk = surface_chunks[cluster_sweep_chunk];
	if (!chunk) {
		return;
	}

	for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
		RTCacheEntry *entry = &chunk[i];
		if (!entry->ptr || !entry->owner_mesh.is_valid() || mesh_storage->owns_mesh(entry->owner_mesh)) {
			continue;
		}
		_release_cluster_blas(entry->ptr, true);
		memdelete(entry->ptr);
		*entry = RTCacheEntry();
	}
}

bool RenderRaytracing::_populate_cluster_blas(void *p_mesh_surface, uint32_t p_cache_key, RTSurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RD *rd = RD::get_singleton();

	const uint32_t cluster_count = mesh_storage->mesh_surface_get_cluster_count(p_mesh_surface);
	const RID cluster_buffer = mesh_storage->mesh_surface_get_cluster_buffer(p_mesh_surface);
	const RID cluster_position_buffer = mesh_storage->mesh_surface_get_cluster_position_buffer(p_mesh_surface);
	const Vector<uint8_t> &records = mesh_storage->mesh_surface_get_cluster_records(p_mesh_surface);

	if (cluster_count == 0 || !cluster_buffer.is_valid() || !cluster_position_buffer.is_valid() ||
			(uint64_t)records.size() < (uint64_t)cluster_count * RT_CLUSTER_RECORD_SIZE) {
		ERR_PRINT_ONCE("Ray tracing: a mesh surface carries no baked cluster data and will not be rendered. Re-import the mesh.");
		return false;
	}
	if (!rd->clas_is_supported()) {
		ERR_PRINT_ONCE("Ray tracing: the rendering device does not support cluster acceleration structures, so static geometry will not be rendered.");
		return false;
	}

	const uint64_t cluster_base_address = rd->buffer_get_device_address(cluster_buffer);
	const uint64_t position_base_address = rd->buffer_get_device_address(cluster_position_buffer);
	ERR_FAIL_COND_V_MSG(cluster_base_address == 0 || position_base_address == 0, false, "Ray tracing: cluster buffers have no device address.");

	const RD::ClusterAccelerationStructureLimits limits = rd->clas_get_limits();
	const uint32_t index_section_offset = mesh_storage->mesh_surface_get_cluster_index_section_offset(p_mesh_surface);
	const uint64_t cluster_buffer_size = mesh_storage->mesh_surface_get_cluster_buffer_size(p_mesh_surface);
	const uint64_t position_buffer_size = mesh_storage->mesh_surface_get_cluster_position_buffer_size(p_mesh_surface);
	const uint8_t *record_ptr = records.ptr();

	LocalVector<RTClusterTriangleInfo> src_infos;
	src_infos.resize(cluster_count);
	LocalVector<uint32_t> cluster_remap;
	cluster_remap.resize(cluster_count);

	uint32_t max_cluster_triangles = 0;
	uint32_t max_cluster_vertices = 0;
	uint32_t total_triangles = 0;
	uint32_t total_vertices = 0;

	for (uint32_t i = 0; i < cluster_count; i++) {
		const uint8_t *record = record_ptr + (uint64_t)i * RT_CLUSTER_RECORD_SIZE;
		const uint32_t position_offset = decode_uint32(record + 0);
		const uint32_t index_offset = decode_uint32(record + 4);
		const uint32_t vertex_count = record[8];
		const uint32_t triangle_count = record[9];

		ERR_FAIL_COND_V_MSG(vertex_count == 0 || triangle_count == 0, false, "Ray tracing: a cluster record carries no geometry.");
		ERR_FAIL_COND_V_MSG(vertex_count > limits.max_vertices_per_cluster || triangle_count > limits.max_triangles_per_cluster, false,
				"Ray tracing: a cluster exceeds the per-cluster vertex or triangle limit of this device.");
		ERR_FAIL_COND_V_MSG((uint64_t)position_offset + (uint64_t)vertex_count * 12 > position_buffer_size, false,
				"Ray tracing: a cluster's position range lies outside the surface's cluster position buffer.");
		// The local index section lives inside cluster_buffer, behind its header and per-cluster records.
		ERR_FAIL_COND_V_MSG((uint64_t)index_section_offset + (uint64_t)index_offset + (uint64_t)triangle_count * 3 > cluster_buffer_size, false,
				"Ray tracing: a cluster's index range lies outside the surface's cluster buffer.");

		RTClusterTriangleInfo &info = src_infos[i];
		info.cluster_id = i;
		info.packed_counts = (triangle_count & 0x1FFu) | ((vertex_count & 0x1FFu) << 9) | (RT_CLUSTER_INDEX_TYPE_8BIT << 24);
		info.vertex_buffer_stride = sizeof(float) * 3;
		info.index_buffer = cluster_base_address + index_section_offset + index_offset;
		info.vertex_buffer = position_base_address + position_offset;

		cluster_remap[i] = decode_uint32(record + 12);

		max_cluster_triangles = MAX(max_cluster_triangles, triangle_count);
		max_cluster_vertices = MAX(max_cluster_vertices, vertex_count);
		total_triangles += triangle_count;
		total_vertices += vertex_count;
	}

	RD::ClusterBuildInput input;
	input.max_acceleration_structure_count = cluster_count;
	input.flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
	input.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
	input.max_geometry_index_value = 0;
	input.max_cluster_unique_geometry_count = 1;
	input.max_cluster_triangle_count = max_cluster_triangles;
	input.max_cluster_vertex_count = max_cluster_vertices;
	input.max_total_triangle_count = total_triangles;
	input.max_total_vertex_count = total_vertices;
	input.min_position_truncate_bit_count = 0;

	RD::ClusterBuildSizes sizes;
	rd->clas_get_build_sizes(input, sizes);
	ERR_FAIL_COND_V_MSG(sizes.acceleration_structure_size == 0 || sizes.build_scratch_size == 0, false, "Ray tracing: failed to query the cluster build sizes.");
	ERR_FAIL_COND_V_MSG(sizes.acceleration_structure_size > UINT32_MAX, false, "Ray tracing: the cluster acceleration structure is too large to allocate.");

	const BitField<RD::BufferCreationBits> implicit_flags = RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_STORAGE_BIT | RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;
	const BitField<RD::BufferCreationBits> build_input_flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	RTPendingClusterBuild pending;
	pending.input = input;
	pending.cluster_count = cluster_count;
	pending.scratch_size = sizes.build_scratch_size;

	RID cluster_remap_buffer;
	auto abort_build = [&]() {
		RID owned[] = { pending.clas_buffer, pending.clas_addresses_buffer, pending.clas_count_buffer, pending.src_infos_buffer, cluster_remap_buffer };
		for (const RID &rid : owned) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
	};

	pending.clas_buffer = rd->storage_buffer_create((uint32_t)sizes.acceleration_structure_size, Span<uint8_t>(), 0, implicit_flags);
	if (!pending.clas_buffer.is_valid()) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: failed to allocate the cluster acceleration structure buffer.");
	}
	rd->set_resource_name(pending.clas_buffer, "RT CLAS [" + itos(p_cache_key) + "]");

	const uint32_t addresses_size = cluster_count * (uint32_t)sizeof(uint64_t);
	pending.clas_addresses_buffer = rd->storage_buffer_create(addresses_size, Span<uint8_t>(), 0, build_input_flags);
	if (!pending.clas_addresses_buffer.is_valid()) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: failed to allocate the cluster address buffer.");
	}
	rd->set_resource_name(pending.clas_addresses_buffer, "RT CLAS addresses [" + itos(p_cache_key) + "]");

	const uint32_t src_infos_count = cluster_count;
	pending.clas_count_buffer = rd->storage_buffer_create(sizeof(uint32_t), Span<uint8_t>((const uint8_t *)&src_infos_count, sizeof(uint32_t)), 0, build_input_flags);
	if (!pending.clas_count_buffer.is_valid()) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: failed to allocate the cluster count buffer.");
	}
	rd->set_resource_name(pending.clas_count_buffer, "RT CLAS count [" + itos(p_cache_key) + "]");

	const uint32_t src_infos_size = cluster_count * (uint32_t)sizeof(RTClusterTriangleInfo);
	pending.src_infos_buffer = rd->storage_buffer_create(src_infos_size, Span<uint8_t>((const uint8_t *)src_infos.ptr(), src_infos_size), 0, build_input_flags);
	if (!pending.src_infos_buffer.is_valid()) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: failed to allocate the cluster build input buffer.");
	}
	rd->set_resource_name(pending.src_infos_buffer, "RT CLAS build inputs [" + itos(p_cache_key) + "]");

	const uint32_t remap_size = cluster_count * (uint32_t)sizeof(uint32_t);
	cluster_remap_buffer = rd->storage_buffer_create(remap_size, Span<uint8_t>((const uint8_t *)cluster_remap.ptr(), remap_size), 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (!cluster_remap_buffer.is_valid()) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: failed to allocate the cluster remap buffer.");
	}
	rd->set_resource_name(cluster_remap_buffer, "RT cluster remap [" + itos(p_cache_key) + "]");

	const uint64_t remap_address = rd->buffer_get_device_address(cluster_remap_buffer);
	if (remap_address == 0) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: the cluster remap buffer has no device address.");
	}

	pending.blas = rd->blas_create_from_clusters(cluster_count, cluster_count);
	if (!pending.blas.is_valid()) {
		abort_build();
		ERR_FAIL_V_MSG(false, "Ray tracing: failed to create a cluster bottom level acceleration structure.");
	}
	rd->set_resource_name(pending.blas, "RT cluster BLAS [" + itos(p_cache_key) + "]");

	r_surf_data->blas = pending.blas;
	r_surf_data->clas_buffer = pending.clas_buffer;
	r_surf_data->clas_addresses_buffer = pending.clas_addresses_buffer;
	r_surf_data->clas_count_buffer = pending.clas_count_buffer;
	r_surf_data->cluster_remap_buffer = cluster_remap_buffer;
	r_surf_data->cluster_count = cluster_count;
	r_surf_data->is_clustered = true;

	RT_GeometryData &geom = r_surf_data->geometry;
	geom.cluster_remap_address_lo = uint32_t(remap_address & 0xFFFFFFFFULL);
	geom.cluster_remap_address_hi = uint32_t(remap_address >> 32);
	geom.cluster_count = cluster_count;
	geom.flags |= RT_GEOM_FLAG_CLUSTERED;

	pending.surf_data = r_surf_data;
	pending_cluster_builds.push_back(pending);

	return true;
}

void RenderRaytracing::_flush_pending_cluster_builds() {
	if (pending_cluster_builds.is_empty()) {
		return;
	}

	RD *rd = RD::get_singleton();
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	uint64_t required_scratch = 0;
	for (const RTPendingClusterBuild &pending : pending_cluster_builds) {
		required_scratch = MAX(required_scratch, pending.scratch_size);
	}

	if (clas_scratch_capacity < required_scratch) {
		if (clas_scratch_buffer.is_valid()) {
			cluster_deferred_frees.push_back({ clas_scratch_buffer, current_frame });
			clas_scratch_buffer = RID();
			clas_scratch_capacity = 0;
		}
		clas_scratch_buffer = rd->storage_buffer_create((uint32_t)required_scratch, Span<uint8_t>(), 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
		if (clas_scratch_buffer.is_valid()) {
			clas_scratch_capacity = required_scratch;
			rd->set_resource_name(clas_scratch_buffer, "RT CLAS scratch");
		}
	}

	for (const RTPendingClusterBuild &pending : pending_cluster_builds) {
		cluster_deferred_frees.push_back({ pending.src_infos_buffer, current_frame });

		RD::ClusterAddressRegion addresses;
		addresses.buffer = pending.clas_addresses_buffer;
		addresses.stride = sizeof(uint64_t);
		addresses.size = (uint64_t)pending.cluster_count * sizeof(uint64_t);

		RD::ClusterAddressRegion src_infos;
		src_infos.buffer = pending.src_infos_buffer;
		src_infos.stride = sizeof(RTClusterTriangleInfo);
		src_infos.size = (uint64_t)pending.cluster_count * sizeof(RTClusterTriangleInfo);

		Error err = clas_scratch_buffer.is_valid() ? OK : ERR_CANT_CREATE;
		if (err == OK) {
			err = rd->clas_build(pending.input, pending.clas_buffer, addresses, RD::ClusterAddressRegion(), clas_scratch_buffer, src_infos, pending.clas_count_buffer);
		}
		if (err == OK) {
			err = rd->blas_build_from_clusters(pending.blas, addresses, pending.clas_buffer);
		}
		if (err != OK) {
			_release_cluster_blas(pending.surf_data, true);
			// tlas_build() rejects the whole instance list over one unbuilt BLAS, while a null one
			// is written as an inactive instance.
			for (uint32_t i = 0; i < blass.size(); i++) {
				if (blass[i] == pending.blas) {
					blass[i] = RID();
					if (i < instance_masks.size()) {
						instance_masks[i] = 0;
					}
				}
			}
			ERR_PRINT_ONCE("Ray tracing: failed to build a cluster bottom level acceleration structure.");
		}
	}

	pending_cluster_builds.clear();
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

	if (!p_vertex_buffer_override.is_valid()) {
		_populate_cluster_blas(p_mesh_surface, p_cache_key, r_surf_data);
		return;
	}

	uint32_t index_count = (geom.index_format != RT_INDEX_FORMAT_NONE) ? geom.primitive_count * 3 : 0;
	bool is_2d = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	RD::AccelerationStructureGeometry as_geom;
	as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
	as_geom.geometry.triangles.vertex_buffer = vertex_buffer;
	as_geom.geometry.triangles.vertex_stride = geom.position_stride;
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
	const uint64_t shader_hash = material_storage->material_get_shader_code_rt_hash(p_material_rid);
	const uint64_t shader_hash_b = material_storage->material_get_shader_code_rt_hash_b(p_material_rid);
	const uint64_t content_generation = material_storage->material_get_rt_content_generation(p_material_rid);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	const bool needs_refresh = !entry->ptr ||
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
	SceneShaderForwardClustered::MaterialData *raster_material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(p_material_rid, RendererRD::MaterialStorage::SHADER_TYPE_3D));
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
		mat_data->hit_shader = shader_data->get_hit_shader();
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
	entry->cached_counter = p_material_invalidation_counter;
	entry->cached_rid_version = mat_version;
	entry->cached_shader_hash = shader_hash;
	entry->cached_shader_hash_b = shader_hash_b;
	entry->cached_content_generation = content_generation;
	entry->last_used_frame = current_frame;

	if (mat_data->uniform_buffer.is_valid()) {
		geometry_buffer_dependencies.insert(mat_data->uniform_buffer);
	}
	return mat_data;
}

// ---------------------------------------------------------------------------
// Acceleration structure building
// ---------------------------------------------------------------------------

void RenderRaytracing::build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list) {
	RENDER_TIMESTAMP("BLAS Build");

	_flush_pending_cluster_builds();

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
			RD::get_singleton()->free_rid(p_state->tlas);
		}
		p_state->tlas_max_instances = needed * 2;
		p_state->tlas = RD::get_singleton()->tlas_create(p_state->tlas_max_instances, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(p_state->tlas, "RT TLAS");
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

	RD::get_singleton()->tlas_build(p_state->tlas, instances);
}

void RenderRaytracing::finalize_buffers(RTViewportState *p_state) {
	// Grow-only uploads. Callers must not free these in prepare_frame().
	auto update_or_grow = [](RID &p_buffer, uint32_t &p_capacity, const void *p_data, uint32_t p_size) {
		if (p_size == 0) {
			return;
		}
		if (p_size > p_capacity) {
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
	update_or_grow(p_state->geometry_buffer, p_state->geometry_buffer_capacity,
			geometry_data.is_empty() ? &empty_geometry : geometry_data.ptr(), MAX(geometry_data.size(), 1u) * sizeof(RT_GeometryData));
	update_or_grow(p_state->material_buffer, p_state->material_buffer_capacity,
			material_data.is_empty() ? &empty_material : material_data.ptr(), MAX(material_data.size(), 1u) * sizeof(RT_MaterialData));
	update_or_grow(p_state->motion_index_buffer, p_state->motion_index_buffer_capacity,
			motion_indices.ptr(), motion_indices.size() * sizeof(int32_t));
	update_or_grow(p_state->motion_transform_buffer, p_state->motion_transform_buffer_capacity,
			motion_transforms.ptr(), motion_transforms.size() * sizeof(RT_InstanceMotionData));
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

	prepare_frame();

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	LocalVector<RID> dirty_blas_list;
	LocalVector<RID> dirty_blas_update_list;
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
			for (RID buffer : { mesh_storage->mesh_surface_get_vertex_buffer(mesh_surface), mesh_storage->mesh_surface_get_attribute_buffer(mesh_surface), mesh_storage->mesh_surface_get_skin_buffer(mesh_surface), mesh_storage->mesh_surface_get_index_buffer(mesh_surface, 0), surf_data->cluster_remap_buffer }) {
				if (buffer.is_valid()) {
					geometry_buffer_dependencies.insert(buffer);
				}
			}
			register_emissive_source(inst, inst->data->base, surf->surface_index, surface_counter, geometry_index, 0,
					surf_data->geometry.primitive_count, final_transform, mat_data);

			if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
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
			if (pending.transform_moved) {
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

			RTSurfaceData *surf_data = process_surface(pending.mm_surf, pending.mesh_surface,
					pending.surface_counter, pending.instance_transform, dirty_blas_list);
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
				for (RID buffer : { mesh_storage->mesh_surface_get_vertex_buffer(pending.mesh_surface), mesh_storage->mesh_surface_get_attribute_buffer(pending.mesh_surface), mesh_storage->mesh_surface_get_skin_buffer(pending.mesh_surface), mesh_storage->mesh_surface_get_index_buffer(pending.mesh_surface, 0), surf_data->cluster_remap_buffer }) {
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

	ERR_FAIL_COND_V(!update_material_pipeline(state), nullptr);
	build_acceleration_structures(state, dirty_blas_list, dirty_blas_update_list);
	finalize_buffers(state);
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
	for (RID blas : blass) {
		hash_scene(blas.get_id());
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
