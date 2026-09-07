/**************************************************************************/
/*  render_raytracing.h                                                   */
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

#pragma once

#include "core/math/transform_3d.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {

class RenderForwardClustered;
struct RenderRTXDIViewportResources;

// Must match GLSL GeometryData (std430, 128 bytes).
struct alignas(16) RT_GeometryData {
	uint64_t vertex_buffer_address;
	uint64_t attribute_buffer_address;
	uint64_t index_buffer_address;
	uint32_t vertex_count;
	uint32_t position_stride;
	uint32_t normal_byte_offset;
	uint32_t normal_stride;
	uint32_t tangent_byte_offset;
	uint32_t tangent_stride;
	uint32_t attribute_stride;
	uint32_t uv_byte_offset;
	uint32_t uv_scale_packed;
	uint32_t index_format;
	uint32_t primitive_count;
	uint32_t flags;
	uint32_t color_byte_offset;
	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint32_t prev_vertex_buffer_address_lo;
	uint32_t prev_vertex_buffer_address_hi;
	// For clustered geometry: uint32 per cluster, mapping cluster index to its first global triangle.
	uint32_t cluster_remap_address_lo;
	uint32_t cluster_remap_address_hi;
	uint32_t cluster_count;
	float position_offset[3];
	uint32_t instance_layer_mask;
	float position_scale[3];
	float _pad1;
};
static_assert(sizeof(RT_GeometryData) == 128, "RT_GeometryData must be 128 bytes for std430");

/// Per-instance motion data for velocity computation (matches GLSL InstanceMotionData, 48 bytes).
struct RT_InstanceMotionData {
	float prev_object_to_world[12]; // Previous object-to-world (mat3x4, transposed 3x4).
};
static_assert(sizeof(RT_InstanceMotionData) == 48, "RT_InstanceMotionData must be 48 bytes");

// Must match GLSL MaterialData (std430, 112 bytes).
struct alignas(16) RT_MaterialData {
	uint32_t albedo_texture_idx;
	uint32_t normal_texture_idx;
	uint32_t orm_texture_idx;
	uint32_t emission_texture_idx;
	float albedo_color[4];
	float emission_color[3];
	float emission_strength;
	float metallic;
	float roughness;
	float ao_strength;
	uint32_t flags;
	float uv1_scale[2];
	float uv1_offset[2];
	float normal_map_depth; // Strength [0..N], default 1.0 (not Z-depth).
	float specular; // Dielectric specular [0..1], default 0.5 -> F0 = 0.04.
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none).
	float alpha_scissor_threshold = 0.5f;
	float alpha_hash_scale = 1.0f;
	uint32_t coverage_flags = 0;
	uint32_t coverage_sampler = 0;
};
static_assert(sizeof(RT_MaterialData) == 112, "RT_MaterialData must be 112 bytes for std430");

// Light types for raytracing (matches GLSL RT_LIGHT_TYPE_* defines).
enum RTLightType : uint32_t {
	RT_LIGHT_TYPE_OMNI = 0,
	RT_LIGHT_TYPE_DIRECTIONAL = 1,
	RT_LIGHT_TYPE_SPOT = 2,
	RT_LIGHT_TYPE_AREA = 3,
	RT_LIGHT_TYPE_EMISSIVE_TRIANGLE = 4,
	RT_LIGHT_TYPE_ENVIRONMENT = 5,
};

enum RTLightFlags : uint32_t {
	RT_LIGHT_FLAG_CASTS_SHADOW = 1u,
	RT_LIGHT_FLAG_TEXTURED = 2u,
};

// Must match GLSL RTLightData (std430, 192 bytes).
struct alignas(16) RT_LightData {
	float position[3];
	uint32_t type;
	float direction[3];
	uint32_t flags;
	float emission[3];
	float radius;
	float axis_u[3];
	float inv_area;
	float axis_v[3];
	float attenuation;
	float range;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float specular_amount;
	uint32_t texture_index;
	uint32_t geometry_index;
	uint32_t primitive_index;
	uint32_t receiver_mask;
	uint32_t caster_mask;
	uint32_t topology_generation;
	uint32_t _pad2;
	uint32_t _pad3;
	float uv_rect[4];
	float transform[12];
};
static_assert(sizeof(RT_LightData) == 192, "RT_LightData must be 192 bytes for std430");

struct alignas(16) RT_LightBufferParameters {
	uint32_t local_first = 0;
	uint32_t local_count = 0;
	uint32_t infinite_first = 0;
	uint32_t infinite_count = 0;
	uint32_t environment_index = 0;
	uint32_t environment_present = 0;
	uint32_t total_count = 0;
	uint32_t previous_count = 0;
};
static_assert(sizeof(RT_LightBufferParameters) == 32, "RT_LightBufferParameters must be 32 bytes for std430");

struct RTLightKey {
	uint64_t instance_id = 0;
	uint64_t resource_id = 0;
	uint64_t surface_generation = 0;
	uint32_t primitive_index = 0;
	uint32_t type = 0;

	_FORCE_INLINE_ bool operator==(const RTLightKey &p_other) const {
		return instance_id == p_other.instance_id && resource_id == p_other.resource_id &&
				surface_generation == p_other.surface_generation && primitive_index == p_other.primitive_index && type == p_other.type;
	}

	_FORCE_INLINE_ uint32_t hash() const {
		uint32_t h = hash_murmur3_one_64(instance_id);
		h = hash_murmur3_one_64(resource_id, h);
		h = hash_murmur3_one_64(surface_generation, h);
		h = hash_murmur3_one_32(primitive_index, h);
		return hash_murmur3_one_32(type, h);
	}
};

struct RTLightSnapshot {
	LocalVector<RTLightKey> keys;
	LocalVector<RT_LightData> lights;
	RT_LightBufferParameters parameters;
	RID light_buffer;
	uint32_t light_buffer_capacity = 0;
	RID current_to_previous_buffer;
	uint32_t current_to_previous_capacity = 0;
	RID previous_to_current_buffer;
	uint32_t previous_to_current_capacity = 0;
	RID parameters_buffer;
};

enum {
	RT_OFFSET_NONE = 0xFFFFFFFFu,
	RT_CACHE_CHUNK_SIZE = 256,
	RT_CACHE_CHUNK_SHIFT = 8,
	RT_CACHE_CHUNK_MASK = 255,
};

// Material flags for RT (matches GLSL mat_flags bit layout).
enum {
	RT_MAT_FLAG_HAS_NORMAL_MAP = 1u,
	RT_MAT_FLAG_HAS_EMISSION_TEX = 2u,
	RT_MAT_FLAG_POINT_FILTER = 4u,
};

// Index format for RT geometry (matches GLSL fetch_indices).
enum {
	RT_INDEX_FORMAT_UINT16 = 0,
	RT_INDEX_FORMAT_UINT32 = 1,
	RT_INDEX_FORMAT_NONE = 2,
};

enum {
	RT_GEOM_FLAG_COMPRESSED = 1u,
	RT_GEOM_FLAG_PROCEDURAL = 2u,
	// Set when the BLAS uses a per-frame-deformed vertex buffer.
	RT_GEOM_FLAG_DEFORMED = 4u,
	// Set when the BLAS is built from clusters, so gl_PrimitiveID is cluster-relative.
	RT_GEOM_FLAG_CLUSTERED = 8u,
	RT_GEOM_FLAG_CASTS_SHADOWS = 16u,
	RT_GEOM_FLAG_SHADOWS_ONLY = 32u,
	RT_GEOM_FLAG_SHADOW_CULL_ENABLED = 64u,
};

/// Per-instance state for procedural RT geometry. Heap-allocated, only exists for procedural instances.
struct RTProceduralState {
	AABB culling_aabb;
	PackedFloat32Array aabb_data; // N * 6 floats (min/max per AABB). Empty = single AABB.
	bool expose_bounds = false;
	bool dirty = true;
	RID blas;
	RID gpu_buffer;
	uint32_t gpu_buffer_capacity = 0; // Bytes, grow-only.
	uint64_t gpu_buffer_address = 0; // BDA (0 = not exposed).
	uint32_t aabb_count = 0;
};

struct RTSurfaceData {
	RID blas;
	RT_GeometryData geometry = {};
	uint64_t blas_size = 0;

	// Cluster BLAS resources. blas_create_from_clusters() registers no dependency on
	// the mesh buffers, so nothing cascade-frees these; every one is freed explicitly.
	RID clas_buffer;
	RID clas_addresses_buffer;
	RID cluster_remap_buffer;
	RID clas_count_buffer;
	uint32_t cluster_count = 0;
	bool is_clustered = false;
};

/// A cluster BLAS build queued during surface processing and issued once the frame's
/// compute list is closed; RenderingDevice forbids cluster builds inside a list.
struct RTPendingClusterBuild {
	RD::ClusterBuildInput input;
	RTSurfaceData *surf_data = nullptr;
	RID blas;
	RID clas_buffer;
	RID clas_addresses_buffer;
	RID clas_count_buffer;
	RID src_infos_buffer;
	uint32_t cluster_count = 0;
	uint64_t scratch_size = 0;
};

/// A resource the render graph still references this frame; freed on a later frame.
struct RTDeferredResourceFree {
	RID resource;
	uint32_t frame = 0;
};

/// Inputs for a surface backed by a per-frame-deformed vertex buffer.
struct RTDeformedGeometrySource {
	RID current_vb; ///< Vertex buffer (object-space positions) the BLAS is built / refit against.
	RID prev_vb; ///< Previous-frame positions for motion vectors. Optional.
	uint64_t change_stamp = 0; ///< Changes when `current_vb` contents change.
	uint64_t cache_key = 0; ///< Caller-defined cache identity.
	uint32_t cache_version = 0; ///< Changes when the resource behind `cache_key` is recycled.
	uint32_t surface_counter = 0; ///< Changes when the underlying mesh surface changes.
};

struct RTMaterialData {
	alignas(16) RT_MaterialData data = {};
	uint32_t global_buffer_index = UINT32_MAX;
	bool is_custom_shader = false;
	bool uses_alpha_clip = false;
	bool has_alpha_texture = false;
	RID uniform_buffer; // Buffer pointer for mats > 512 bytes.
	uint32_t uniform_pool_slot = UINT32_MAX; // Index into the material UBO pool, or UINT32_MAX (unused)
	RID albedo_texture_rd;
	RID normal_texture_rd;
	RID orm_texture_rd;
	RID emission_texture_rd;
};

struct RTEmissiveSource {
	uint64_t instance_id = 0;
	uint64_t resource_id = 0;
	uint64_t surface_generation = 0;
	uint32_t geometry_index = 0;
	uint32_t key_primitive_offset = 0;
	uint32_t primitive_count = 0;
	uint32_t topology_generation = 0;
	Transform3D transform;
	RTMaterialData *material = nullptr;
};

struct RTCacheEntry {
	RTSurfaceData *ptr = nullptr;
	RID owner_mesh;
	uint32_t last_used_frame = 0;
	uint32_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
	uint8_t failed_attempts = 0;
	uint64_t size_bytes = 0;
};

/// BLAS cache entry for a surface driven by a per-frame-deformed vertex buffer.
///
/// The skinned vertex buffer is owned by the engine's MeshInstance system and may
/// be freed/reallocated outside our control. To decouple BLAS lifetime from that
/// (and keep last-frame positions alive for motion vectors) we copy the skinned VB
/// into our own buffers each frame:
///   * owned_vb_full mirrors the skinned VB layout (positions + N + T) and is what
///     the BLAS and the hit shader read from this frame.
///   * prev_pos_vb stores the previous-frame positions only (float3 packed) for the
///     motion-vector path. Updated from owned_vb_full's position section each frame
///     before owned_vb_full is overwritten with the new skinned data.
struct RTDeformedCacheEntry {
	RTSurfaceData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint64_t cached_change_stamp = 0;
	uint32_t cached_key_version = 0;
	uint32_t cached_surface_counter = 0;
	uint64_t cached_buffer_id = 0; // RID id of the deformed vertex buffer at the time of build.
	bool blas_built_once = false; // True once the BLAS has been fully built; subsequent ticks can refit.

	RID owned_vb_full;
	uint32_t owned_vb_full_capacity = 0; // Bytes; grow-only.
	RID prev_pos_vb;
	uint32_t prev_pos_vb_capacity = 0; // Bytes; grow-only.
	uint32_t cached_vertex_count = 0;
	uint32_t cached_full_size = 0;
	bool prev_pos_seeded = false; // False until the first owned->prev copy has run.
};

/// Cache entry for a per-(MultiMesh, surface) merged BLAS.
/// All vertex data (positions, normals, tangents, UVs, colors) is fully baked per-instance
/// so the hit shader uses the standard code path — no special per-instance lookups.
struct RTMergedMMEntry {
	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if mesh has normals).
	// The BLAS reads only the position section; the hit shader reads TBN via normal_byte_offset.
	RID merged_vtx_buffer;
	uint32_t vtx_capacity_bytes = 0;

	// Merged attribute buffer: [UV + color × N*V] replicated per instance.
	RID merged_attr_buffer;
	uint32_t attr_capacity_bytes = 0;

	// Replicated index buffer: uint32 N*I entries (invalid if non-indexed).
	RID replicated_idx_buffer;
	uint32_t idx_capacity = 0;

	RID blas;
	uint32_t last_mm_count = 0;
	uint32_t last_surface_counter = 0;
	uint32_t last_used_frame = 0;
	uint64_t cached_mm_last_change = 0;
	bool blas_built_once = false;
	bool indexed = false; // selects MODE_INDEXED vs MODE_NON_INDEXED variant
};

struct RTMaterialCacheEntry {
	RTMaterialData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint16_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
	uint64_t cached_shader_hash = 0;
	uint64_t cached_shader_hash_b = 0;
};

/// Per-viewport raytracing state.
///
/// Each viewport has its own visibility set (frustum/LOD/visibility ranges), so
/// the TLAS instance composition and per-instance SSBO contents differ across
/// viewports. Sharing them caused the wrong `gl_InstanceCustomIndexEXT` to
/// resolve to the wrong `geometries[]` / `materials[]` entries, dereferencing
/// stale BDAs and faulting the GPU.
///
/// Lifetime is tied to a `RenderSceneBuffersRD`: created lazily on first
/// `build_tlas` for that viewport, freed via `RenderRaytracing::free_viewport_state`
/// from `RenderBufferDataForwardClustered::free_data()`.
struct RTViewportState {
	RID tlas;
	uint32_t tlas_max_instances = 0;

	RID geometry_buffer;
	uint32_t geometry_buffer_capacity = 0;
	RID material_buffer;
	uint32_t material_buffer_capacity = 0;
	RID motion_index_buffer;
	uint32_t motion_index_buffer_capacity = 0;
	RID motion_transform_buffer;
	uint32_t motion_transform_buffer_capacity = 0;

	RTLightSnapshot light_snapshots[2];
	uint32_t current_light_snapshot = 0;
	bool light_history_valid = false;
	RID environment_texture;
	RenderRTXDIViewportResources *rtxdi_di = nullptr;

};

class RenderRaytracing {
	friend class RenderForwardClustered;

	RenderForwardClustered *owner = nullptr;
	BindlessBlock *bindless_block = nullptr;

	// Caching (chunked sparse caches indexed by RID low bits / 256).
	Vector<RTCacheEntry *> surface_chunks;
	Vector<RTMaterialCacheEntry *> material_chunks;

	// Merged MultiMesh BLAS cache and compute shader.
	struct MergeShader {
		enum Mode {
			MODE_NON_INDEXED = 0,
			MODE_INDEXED = 1,
			MODE_MAX,
		};
		MultimeshMergeShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} mm_merge_shader;

	struct MMSurfaceHandles {
		LocalVector<RID> per_surface; // Grown on demand; entry per touched surface index.
		uint32_t mm_validator = 0; // High 32 bits of MM RID id at last access.

		// Resolve / grow storage for `p_surface_index`.
		RID &surface_handle(uint32_t p_surface_index) {
			if (p_surface_index >= per_surface.size()) {
				per_surface.resize(p_surface_index + 1);
			}
			return per_surface[p_surface_index];
		}
	};
	LocalVector<MMSurfaceHandles> mm_handles;

	RID_Owner<RTDeformedCacheEntry> deformed_pool;
	LocalVector<RID> deformed_active_this_frame;

	RID_Owner<RTMergedMMEntry> merged_mm_pool;
	LocalVector<RID> merged_mm_active_this_frame;

	RTDeformedCacheEntry *_access_deformed_slot(RID &r_handle);
	RTMergedMMEntry *_access_merged_mm_slot(RID &r_handle);

	// Cluster BLAS build state, all owned by this class.
	LocalVector<RTPendingClusterBuild> pending_cluster_builds;
	LocalVector<RTDeferredResourceFree> cluster_deferred_frees;
	RID clas_scratch_buffer;
	uint64_t clas_scratch_capacity = 0;
	uint32_t cluster_sweep_chunk = 0;

	LocalVector<uint32_t> material_free_slots;
	uint32_t next_material_slot = 0;
	uint64_t vram_used = 0;
	uint32_t cache_hits = 0;
	uint32_t cache_misses = 0;

	// Per-frame scratch arrays.
	HashSet<RID> geometry_buffer_dependencies;
	LocalVector<RT_GeometryData> geometry_data;
	LocalVector<RT_MaterialData> material_data;
	LocalVector<int32_t> motion_indices; ///< Per-instance: index into motion_transforms[], or -1.
	LocalVector<RT_InstanceMotionData> motion_transforms; ///< Compact: only moving instances.
	LocalVector<RID> blass;
	LocalVector<Transform3D> blas_transforms;
	LocalVector<uint32_t> instance_flags;
	LocalVector<uint8_t> instance_masks; // Per-instance ray mask (0x00 = invisible to rays, 0xFF = normal)
	LocalVector<RTEmissiveSource> emissive_sources;

	HashMap<RenderSceneBuffersRD *, RTViewportState *> viewport_states;

	RTViewportState *_get_or_create_viewport_state(const RenderDataRD *p_render_data);
	RTViewportState *_get_viewport_state(const RenderDataRD *p_render_data) const;
	void _free_viewport_state_internal(RTViewportState *p_state);

	// Material UBO sub-allocation pool. One large device-address buffer divided
	// into fixed-size slots for performance reasons, and easier to debug.
	RID mat_ubo_pool_buffer;
	uint64_t mat_ubo_pool_bda = 0;
	LocalVector<uint32_t> mat_ubo_pool_free_slots;
	uint32_t mat_ubo_pool_free_count = 0;
	uint32_t mat_ubo_pool_next_slot = 0;

	void mat_ubo_pool_ensure_initialized();
	uint32_t mat_ubo_pool_allocate(); // Returns slot index or UINT32_MAX if pool is exhausted.
	void mat_ubo_pool_release(uint32_t p_slot);
	void mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size);
	uint64_t mat_ubo_pool_get_address(uint32_t p_slot) const;

	// Cache helpers.
	static uint32_t get_rid_index(RID p_rid);
	static uint32_t get_rid_version(RID p_rid);
	RTCacheEntry *get_surface_cache_entry(uint32_t p_index);
	RTMaterialCacheEntry *get_material_cache_entry(uint32_t p_index);
	uint32_t allocate_material_slot();

	// Internal methods.
	RTSurfaceData *process_surface(
			const void *p_surf,
			void *p_mesh_surface,
			uint32_t p_surface_invalidation_counter,
			const Transform3D &p_transform,
			LocalVector<RID> &r_dirty_blas_list);
	RTSurfaceData *process_deformed_surface(
			const void *p_surf,
			void *p_mesh_surface,
			const struct RTDeformedGeometrySource &p_source,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list);
	void _populate_surface_blas(
			void *p_mesh_surface,
			RID p_vertex_buffer_override,
			bool p_force_uncompressed,
			uint32_t p_cache_key,
			RTSurfaceData *r_surf_data,
			LocalVector<RID> &r_dirty_blas_list);
	bool _populate_cluster_blas(void *p_mesh_surface, uint32_t p_cache_key, RTSurfaceData *r_surf_data);
	void _release_cluster_blas(RTSurfaceData *p_surf_data, bool p_deferred);
	void _sweep_dead_cluster_surfaces();
	void _flush_pending_cluster_builds();
	RTMaterialData *process_material(RID p_material_rid, uint16_t p_material_invalidation_counter);
	bool _build_merged_mm_blas(
			RID p_mm_rid,
			RID p_mm_gpu_buffer,
			void *p_mesh_surface,
			uint32_t p_mm_count,
			uint32_t p_surface_index,
			uint32_t p_surface_counter,
			RD::ComputeListID p_compute_list,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list,
			RTSurfaceData *r_surf_data);
	void update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list);
	void build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list);
	void finalize_buffers(RTViewportState *p_state);
	void build_light_registry(RTViewportState *p_state, const RenderDataRD *p_render_data);
	void prepare_frame();

public:
	void initialize(RenderForwardClustered *p_owner);

	void cleanup_caches();

	RTViewportState *build_tlas(const RenderDataRD *p_render_data);
	void free_viewport_state(RenderSceneBuffersRD *p_render_buffers);

	void register_compute_buffer_dependencies(RD::ComputeListID p_list);

	RID get_bindless_uniform_set(RID p_shader) const {
		bindless_block->finalize(p_shader, 1);
		return bindless_block->get_uniform_set();
	}
	RID get_mat_ubo_pool_buffer() const { return mat_ubo_pool_buffer; }

	~RenderRaytracing();
};

} // namespace RendererSceneRenderImplementation
