#define M_PI 3.14159265359
#define M_TAU 6.28318530718
#define ROUGHNESS_MAX_LOD 5

#define MAX_VOXEL_GI_INSTANCES 8
#define MAX_VIEWS 2

#define CLUSTER_COUNTER_SHIFT 20
#define CLUSTER_POINTER_MASK ((1 << CLUSTER_COUNTER_SHIFT) - 1)
#define CLUSTER_COUNTER_MASK 0xfff
#include "../decal_data_inc.slang"
#include "../oct_inc.slang"
#include "../scene_data_inc.slang"
#include "../surface_data_inc.slang"
#ifdef MICRO_GEOMETRY_RASTER
#include "micro_geometry_inc.slang"
#endif

#if !defined(MODE_RENDER_DEPTH) || defined(MODE_RENDER_MATERIAL) || defined(MODE_RENDER_SDF) || defined(MODE_RENDER_NORMAL_ROUGHNESS) || defined(MODE_RENDER_VOXEL_GI) || defined(TANGENT_USED) || defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED)
#ifndef NORMAL_USED
#define NORMAL_USED
#endif
#endif

#if !defined(TANGENT_USED) && (defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED))
#define TANGENT_USED
#endif

struct DrawCall {
	uint instance_index;
	uint uv_offset;
#ifdef MICRO_GEOMETRY_RASTER
	uint micro_geometry_bin;
	uint micro_geometry_recovery;
#else
	uint multimesh_motion_vectors_current_offset;
	uint multimesh_motion_vectors_previous_offset;
#endif
#ifdef UBERSHADER
	uint sc_packed_0;
	uint sc_packed_1;
	uint sc_packed_2;
	uint uc_packed_0;
#endif
};
[[vk::push_constant]] ConstantBuffer<DrawCall> draw_call;

#ifdef UBERSHADER

#define POLYGON_CULL_DISABLED 0
#define POLYGON_CULL_FRONT 1
#define POLYGON_CULL_BACK 2

uint sc_packed_0() {
	return draw_call.sc_packed_0;
}

uint sc_packed_1() {
	return draw_call.sc_packed_1;
}

uint uc_cull_mode() {
	return (draw_call.uc_packed_0 >> 0) & 3U;
}

#else

[[vk::constant_id(0)]] const uint pso_sc_packed_0 = 0;
[[vk::constant_id(1)]] const uint pso_sc_packed_1 = 0;

uint sc_packed_0() {
	return pso_sc_packed_0;
}

uint sc_packed_1() {
	return pso_sc_packed_1;
}

#endif

bool sc_decal_use_mipmaps() {
	return ((sc_packed_0() >> 4) & 1U) != 0;
}

bool sc_multimesh() {
	return ((sc_packed_1() >> 0) & 1U) != 0;
}

bool sc_multimesh_format_2d() {
	return ((sc_packed_1() >> 1) & 1U) != 0;
}

bool sc_multimesh_has_color() {
	return ((sc_packed_1() >> 2) & 1U) != 0;
}

bool sc_multimesh_has_custom_data() {
	return ((sc_packed_1() >> 3) & 1U) != 0;
}

[[vk::constant_id(2)]] const bool sc_emulate_point_size = false;

#ifdef POINT_SIZE_USED

#define VERTEX_INDEX (sc_emulate_point_size ? instance_index_builtin : vertex_index)
#define INSTANCE_INDEX (sc_emulate_point_size ? (vertex_index / 6) : instance_index_builtin)

#else

#define VERTEX_INDEX vertex_index
#define INSTANCE_INDEX instance_index_builtin

#endif

#define REFLECTION_MULTIPLIER 1.0

#define SDFGI_MAX_CASCADES 8

#define INSTANCE_FLAGS_DYNAMIC (1 << 3)
#define INSTANCE_FLAGS_NON_UNIFORM_SCALE (1 << 4)
#define INSTANCE_FLAGS_USE_GI_BUFFERS (1 << 5)
#define INSTANCE_FLAGS_USE_SDFGI (1 << 6)
#define INSTANCE_FLAGS_USE_LIGHTMAP_CAPTURE (1 << 7)
#define INSTANCE_FLAGS_USE_LIGHTMAP (1 << 8)
#define INSTANCE_FLAGS_USE_SH_LIGHTMAP (1 << 9)
#define INSTANCE_FLAGS_USE_VOXEL_GI (1 << 10)
#define INSTANCE_FLAGS_PARTICLES (1 << 11)
#define INSTANCE_FLAGS_PARTICLE_TRAIL_SHIFT 16
#define INSTANCE_FLAGS_FADE_SHIFT 24

#define INSTANCE_FLAGS_PARTICLE_TRAIL_MASK 0xFF

#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSAO (1 << 0)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSIL (1 << 1)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSR (1 << 2)
#define SCREEN_SPACE_EFFECTS_FLAGS_RESOLVE_SSR (1 << 3)

[[vk::binding(10, 0)]] Texture2D<float4> decal_atlas;
[[vk::binding(11, 0)]] Texture2D<float4> decal_atlas_srgb;

[[vk::binding(12, 0)]] StructuredBuffer<DecalData> decals;

[[vk::binding(13, 0)]] StructuredBuffer<float4> global_shader_uniforms;

[[vk::binding(15, 0)]] SamplerState DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;

[[vk::binding(16, 0)]] Texture2D<float4> best_fit_normal_texture;

struct SceneDataBlock {
	SceneData data;
	SceneData prev_data;
};
[[vk::binding(0, 1)]] ConstantBuffer<SceneDataBlock> scene_data_block;

struct ImplementationData {
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;

	uint ss_effects_flags;
	float ssao_light_affect;
	float ssao_ao_affect;
	uint pad1;

	row_major float4x4 sdf_to_bounds;

	int3 sdf_offset;
	uint pad2;

	int3 sdf_size;
	bool gi_upscale_for_msaa;

	bool volumetric_fog_enabled;
	float volumetric_fog_inv_length;
	float volumetric_fog_detail_spread;
	uint volumetric_fog_pad;
};

struct ImplementationDataBlock {
	ImplementationData data;
};
[[vk::binding(1, 1)]] ConstantBuffer<ImplementationDataBlock> implementation_data_block;

#define implementation_data implementation_data_block.data

struct InstanceData {
	row_major float3x4 transform;
	float4 compressed_aabb_position_pad;
	float4 compressed_aabb_size_pad;
	float4 uv_scale;
	uint flags;
	uint instance_uniforms_ofs;
	uint gi_offset;
	uint layer_mask;
	uint rtxdi_material_flags;
	uint rtxdi_padding_0;
	uint rtxdi_padding_1;
	uint rtxdi_padding_2;
	row_major float3x4 prev_transform;
	float4 lightmap_uv_scale;
	float4 model_precision;
	float4 prev_model_precision;
};

#ifdef MICRO_GEOMETRY_RASTER
[[vk::binding(2, 1)]] StructuredBuffer<InstanceData> native_instances;
[[vk::binding(0, 4)]] StructuredBuffer<MicroGeometrySelectedCluster> micro_geometry_selected;
[[vk::binding(1, 4)]] StructuredBuffer<MicroGeometryInstance> micro_geometry_instances;
[[vk::binding(2, 4)]] StructuredBuffer<MicroGeometryInstanceSurface> micro_geometry_surfaces;
[[vk::binding(3, 4)]] ConstantBuffer<MicroGeometryRasterParameters> micro_geometry_parameters;
[[vk::binding(4, 4)]] StructuredBuffer<InstanceData> instances;
[[vk::binding(5, 4)]] StructuredBuffer<uint> micro_geometry_counts;
[[vk::binding(6, 4)]] StructuredBuffer<uint> micro_geometry_initial_counts;
#else
[[vk::binding(2, 1)]] StructuredBuffer<InstanceData> instances;
#endif

[[vk::binding(5, 1)]] Texture2D<float4> shadow_atlas;

[[vk::binding(9, 1)]] StructuredBuffer<uint> cluster_buffer;

[[vk::binding(10, 1)]] SamplerState decal_sampler;

[[vk::binding(12 + 0, 1)]] SamplerState SAMPLER_NEAREST_CLAMP;
[[vk::binding(12 + 1, 1)]] SamplerState SAMPLER_LINEAR_CLAMP;
[[vk::binding(12 + 2, 1)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
[[vk::binding(12 + 3, 1)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
[[vk::binding(12 + 4, 1)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
[[vk::binding(12 + 5, 1)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
[[vk::binding(12 + 6, 1)]] SamplerState SAMPLER_NEAREST_REPEAT;
[[vk::binding(12 + 7, 1)]] SamplerState SAMPLER_LINEAR_REPEAT;
[[vk::binding(12 + 8, 1)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
[[vk::binding(12 + 9, 1)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
[[vk::binding(12 + 10, 1)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
[[vk::binding(12 + 11, 1)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

#ifdef MODE_RENDER_SDF

[[vk::binding(24, 1)]] [[vk::image_format("r16ui")]] RWTexture3D<uint> albedo_volume_grid;
[[vk::binding(25, 1)]] [[vk::image_format("r32ui")]] RWTexture3D<uint> emission_grid;
[[vk::binding(26, 1)]] [[vk::image_format("r32ui")]] RWTexture3D<uint> emission_aniso_grid;
[[vk::binding(27, 1)]] [[vk::image_format("r32ui")]] RWTexture3D<uint> geom_facing_grid;

#define depth_buffer shadow_atlas
#define color_buffer shadow_atlas
#define normal_roughness_buffer shadow_atlas

typealias GodotMultiviewTexture = Texture2D<float4>;
#else

#ifdef USE_MULTIVIEW
[[vk::binding(24, 1)]] Texture2DArray<float4> depth_buffer;
[[vk::binding(25, 1)]] Texture2DArray<float4> color_buffer;
[[vk::binding(26, 1)]] Texture2DArray<float4> normal_roughness_buffer;
[[vk::binding(27, 1)]] Texture2DArray<float4> ao_buffer;
[[vk::binding(28, 1)]] Texture2DArray<float4> ambient_buffer;
[[vk::binding(29, 1)]] Texture2DArray<float4> reflection_buffer;
typealias GodotMultiviewTexture = Texture2DArray<float4>;
#else
[[vk::binding(24, 1)]] Texture2D<float4> depth_buffer;
[[vk::binding(25, 1)]] Texture2D<float4> color_buffer;
[[vk::binding(26, 1)]] Texture2D<float4> normal_roughness_buffer;
[[vk::binding(27, 1)]] Texture2D<float4> ao_buffer;
[[vk::binding(28, 1)]] Texture2D<float4> ambient_buffer;
[[vk::binding(29, 1)]] Texture2D<float4> reflection_buffer;
typealias GodotMultiviewTexture = Texture2D<float4>;
#endif
#endif

float4 normal_roughness_compatibility(float4 p_normal_roughness) {
	float roughness = p_normal_roughness.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);
	return float4(normalize(p_normal_roughness.xyz * 2.0 - 1.0) * 0.5 + 0.5, roughness);
}

[[vk::binding(0, 2)]] StructuredBuffer<float4> transforms;
