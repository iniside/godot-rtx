#ifndef GEOMETRY_DATA_INC_SLANG
#define GEOMETRY_DATA_INC_SLANG

static const uint OFFSET_NONE = 0xFFFFFFFFu;
static const uint FLAG_COMPRESSED = 1u;
static const uint FLAG_PROCEDURAL = 2u;
static const uint FLAG_DEFORMED = 4u;
static const uint FLAG_CLUSTERED = 8u;
static const uint FLAG_CASTS_SHADOWS = 16u;
static const uint FLAG_SHADOWS_ONLY = 32u;
static const uint FLAG_SHADOW_CULL_ENABLED = 64u;

struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;

	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;

	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed;
	uint index_format;
	uint primitive_count;
	uint flags;

	uint color_byte_offset;
	uint prev_vertex_address_lo;
	uint prev_vertex_address_hi;
	uint cluster_remap_address_lo;
	uint cluster_remap_address_hi;
	uint cluster_count;

	float3 position_offset;
	uint instance_layer_mask;
	float3 position_scale;
	uint uv2_byte_offset;
	uint uv2_scale_packed;
	uint custom_byte_offsets[4];
	uint custom_formats;
	uint instance_uniforms_offset;
	uint instance_index;
	float4 instance_custom;
	float4 instance_color;
	uint64_t multimesh_address;
	uint multimesh_stride;
	uint multimesh_offset;
	uint source_vertex_count;
	uint multimesh_flags;
	uint2 _padding;
	uint64_t skin_address;
	uint skin_stride;
	uint skin_weight_offset;
};

struct InstanceMotionData {
	float prev_object_to_rt[12];
};

struct MaterialData {
	uint albedo_texture_idx;
	uint normal_texture_idx;
	uint orm_texture_idx;
	uint emission_texture_idx;

	float4 albedo_color;
	float3 emission_color;
	float emission_strength;

	float metallic;
	float roughness;
	float ao_strength;
	uint flags;

	float2 uv1_scale;
	float2 uv1_offset;

	float normal_map_depth;
	float specular;
	uint64_t uniform_address;
	float alpha_scissor_threshold;
	float alpha_hash_scale;
	uint coverage_flags;
	uint coverage_sampler;
};

#endif
