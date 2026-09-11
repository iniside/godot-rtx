#ifndef RT_HIT_CONTEXT_INC_SLANG
#define RT_HIT_CONTEXT_INC_SLANG

#define MAX_VIEWS 2
#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0
#define OUTPUT_IS_MULTIVIEW false
#include "../scene_data_inc.slang"
#include "../surface_data_inc.slang"
#include "../decal_data_inc.slang"
#include "../scene_decal_inc.slang"
#include "geometry_decode_inc.slang"
#include "rt_frame_constants_inc.slang"

struct SceneDataBlock {
	SceneData data;
	SceneData prev_data;
};
struct RTMaterialFrame {
	float4 rt_origin;
	uint4 counts;
};
struct RTHitRay {
	float3 origin;
	float min_t;
	float3 direction;
	float max_t;
	float cone_width;
	float cone_spread;
	uint layer_mask;
	uint flags;
};
struct RTHitPayload {
	SurfaceData surface;
	float hit_distance;
	float cone_width;
	float cone_spread;
	uint geometry_index;
	uint primitive_index;
	float2 barycentrics;
	uint hit;
	uint front_face;
	uint ray_layer_mask;
	uint ray_flags;
	int cluster_id;
	uint local_primitive_index;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneDataBlock> scene_data_block;
[[vk::binding(1, 0)]] StructuredBuffer<float4> global_shader_uniforms;
[[vk::binding(2, 0)]] StructuredBuffer<GeometryData> geometries;
[[vk::binding(3, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(4, 0)]] RaytracingAccelerationStructure scene_tlas;
[[vk::binding(5, 0)]] ConstantBuffer<RTMaterialFrame> rt_material_frame;
[[vk::binding(6, 0)]] StructuredBuffer<DecalData> decals;
[[vk::binding(7, 0)]] Texture2D<float4> decal_atlas;
[[vk::binding(8, 0)]] Texture2D<float4> decal_atlas_srgb;
[[vk::binding(9, 0)]] SamplerState SAMPLER_NEAREST_CLAMP;
[[vk::binding(10, 0)]] SamplerState SAMPLER_LINEAR_CLAMP;
[[vk::binding(11, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
[[vk::binding(12, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
[[vk::binding(13, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
[[vk::binding(14, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
[[vk::binding(15, 0)]] SamplerState SAMPLER_NEAREST_REPEAT;
[[vk::binding(16, 0)]] SamplerState SAMPLER_LINEAR_REPEAT;
[[vk::binding(17, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
[[vk::binding(18, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
[[vk::binding(19, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
[[vk::binding(20, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
[[vk::binding(21, 0)]] ConstantBuffer<RTFrameConstants> rt_frame;
[[vk::binding(22, 0)]] StructuredBuffer<RTHitRay> rt_hit_rays;
[[vk::binding(23, 0)]] RWStructuredBuffer<RTHitPayload> rt_hit_results;
[[vk::binding(0, 1)]] Texture2D<float4> bindless_textures[];

#ifdef RT_MATERIAL_HIT_STAGE
#ifndef ALPHA_HASH_USED
#define ALPHA_HASH_USED
#define RT_UNDEF_ALPHA_HASH
#endif
#define ALPHA_HASH_COMPUTE
#include "../scene_forward_aa_inc.slang"
#ifdef RT_UNDEF_ALPHA_HASH
#undef ALPHA_HASH_USED
#undef RT_UNDEF_ALPHA_HASH
#endif
#include "material_coverage_inc.slang"

static SceneData scene_data;
static uint64_t rt_material_address;
static uint rt_instance_uniforms_offset;
static bool rt_material_discarded;
static float4x4 read_model_matrix;
static float3x3 model_normal_matrix;
static float4x4 read_view_matrix;
static float4x4 inv_view_matrix;
static float4x4 projection_matrix;
static float4x4 inv_projection_matrix;
static float4x4 modelview;
static float3x3 modelview_normal;
static float4x4 rt_world_to_view;
static float4x4 rt_view_to_world;
static float3 vertex;
static float3 normal_highp;
static float3 tangent;
static float3 binormal;
static float4 position;
static float2 uv_interp;
static float2 uv2_interp;
static float4 color_interp;
static float4 custom0_attrib;
static float4 custom1_attrib;
static float4 custom2_attrib;
static float4 custom3_attrib;
static float4 instance_custom;
static uint INSTANCE_INDEX;
static uint VERTEX_INDEX;
static uint ViewIndex;
static float2 read_viewport_size;
static float3 eye_offset;
static float global_time;
static float global_prev_time;
static bool front_facing_builtin;
static float3 view_highp;
static float3 albedo_highp;
static float alpha_highp;
static float roughness_highp;
static float metallic_highp;
static float specular;
static float3 emission;
static float ao;
static float ao_light_affect;
static float3 normal_map;
static float normal_map_depth;
static float3 bent_normal_map;
static float alpha_scissor_threshold;
static float alpha_hash_scale;
static float alpha_antialiasing_edge;
static float2 alpha_texture_coordinate;
static float premul_alpha;
static float rim;
static float rim_tint;
static float clearcoat;
static float clearcoat_roughness;
static float anisotropy;
static float2 anisotropy_flow;
static float sss_strength;
static float4 transmittance_color;
static float transmittance_depth;
static float transmittance_boost;
static float3 backlight;
static float4 fog;
static float4 custom_radiance;
static float4 custom_irradiance;
static float3 light_vertex;
static float point_size;
static float z_clip_scale;
static float4 fragment_coord;
static float2 screen_uv;
static float2 point_coord;
static float fragment_depth;
static uint4 bone_attrib;
static float4 weight_attrib;
static float3 rt_bary_dx;
static float3 rt_bary_dy;
static float3 rt_position_dx;
static float3 rt_position_dy;
static float rt_cone_radius;

struct RTVertexInterpolants {
	float2 uv;
	float2 uv2;
	float4 color;
	float3 normal;
	float3 tangent;
	float3 binormal;
	float4 custom0;
	float4 custom1;
	float4 custom2;
	float4 custom3;
};

static RTVertexInterpolants rt_interpolants_dx;
static RTVertexInterpolants rt_interpolants_dy;
static float rt_hit_distance;
static bool rt_primary_ray;

struct RTValue<T> {
	T value;
	T x;
	T y;
};

RTValue<T> rt_value<T>(T value) {
	RTValue<T> result;
	result.value = value;
	result.x = value;
	result.y = value;
	return result;
}

void rt_load_material_uniforms();
void rt_material_vertex();
void rt_material_fragment();
void rt_material_interpolate_vertices(GeometryHitInput hit, GeometryTriangle triangle, float3 bary);

void rt_ray_footprint(GeometryHitInput hit, GeometryTriangle triangle, float3 ray_direction, float radius,
		out float3 object_position, out float3 object_dx, out float3 object_dy, out float2 uv, out float2 uv_dx, out float2 uv_dy) {
	float3 bary = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
	float3 p0 = mul(hit.object_to_world, float4(triangle.p0, 1.0));
	float3 p1 = mul(hit.object_to_world, float4(triangle.p1, 1.0));
	float3 p2 = mul(hit.object_to_world, float4(triangle.p2, 1.0));
	float3 cross_normal = cross(p1 - p0, p2 - p0);
	float normal_length = max(length(cross_normal), 1e-20);
	float3 plane_normal = cross_normal / normal_length;
	float3 axis = abs(ray_direction.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
	float3 dx = normalize(cross(ray_direction, axis)) * radius;
	float3 dy = normalize(cross(ray_direction, dx)) * radius;
	float plane_dot = dot(plane_normal, ray_direction);
	float denominator = (plane_dot < 0.0 ? -1.0 : 1.0) * max(abs(plane_dot), 1e-4);
	rt_position_dx = dx - ray_direction * (dot(plane_normal, dx) / denominator);
	rt_position_dy = dy - ray_direction * (dot(plane_normal, dy) / denominator);
	if (rt_primary_ray) {
		GeometryFootprint footprint;
		footprint.world_to_clip = mul(transpose(scene_data_block.data.projection_matrix), geometry_inverse(rt_camera_to_rt(rt_frame, false)));
		footprint.pixel_size = 1.0 / scene_data_block.data.viewport_size;
		geometry_footprint_derivatives(hit, triangle, footprint, object_position, object_dx, object_dy, uv, uv_dx, uv_dy);
		rt_position_dx = mul((float3x3)hit.object_to_world, object_dx);
		rt_position_dy = mul((float3x3)hit.object_to_world, object_dy);
	}
	float3 bary_u = cross(p2 - p0, cross_normal) / max(dot(cross_normal, cross_normal), 1e-20);
	float3 bary_v = cross(cross_normal, p1 - p0) / max(dot(cross_normal, cross_normal), 1e-20);
	rt_bary_dx.yz = float2(dot(bary_u, rt_position_dx), dot(bary_v, rt_position_dx));
	rt_bary_dy.yz = float2(dot(bary_u, rt_position_dy), dot(bary_v, rt_position_dy));
	rt_bary_dx.x = -rt_bary_dx.y - rt_bary_dx.z;
	rt_bary_dy.x = -rt_bary_dy.y - rt_bary_dy.z;
	object_position = triangle.p0 * bary.x + triangle.p1 * bary.y + triangle.p2 * bary.z;
	object_dx = triangle.p0 * rt_bary_dx.x + triangle.p1 * rt_bary_dx.y + triangle.p2 * rt_bary_dx.z;
	object_dy = triangle.p0 * rt_bary_dy.x + triangle.p1 * rt_bary_dy.y + triangle.p2 * rt_bary_dy.z;
	uv = triangle.uv0 * bary.x + triangle.uv1 * bary.y + triangle.uv2 * bary.z;
	uv_dx = triangle.uv0 * rt_bary_dx.x + triangle.uv1 * rt_bary_dx.y + triangle.uv2 * rt_bary_dx.z;
	uv_dy = triangle.uv0 * rt_bary_dy.x + triangle.uv1 * rt_bary_dy.y + triangle.uv2 * rt_bary_dy.z;
}

float rt_texture_footprint(float3 dimensions, float3 dx, float3 dy) {
	dx *= dimensions;
	dy *= dimensions;
	return 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12));
}

float2 rt_cube_coordinate(float3 coordinate, uint face) {
	float3 projected = face == 0u ? coordinate.zyx : face == 1u ? coordinate.xzy : coordinate.xyz;
	return projected.xy / max(abs(projected.z), 1e-12) * 0.5;
}

float2 rt_texture_lod(Texture2D<float4> texture_value, float2 coordinate, float2 dx, float2 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, levels);
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(dx.xy, 0.0), float3(dy.xy, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture2D<int4> texture_value, float2 coordinate, float2 dx, float2 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, levels);
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(dx.xy, 0.0), float3(dy.xy, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture2D<uint4> texture_value, float2 coordinate, float2 dx, float2 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, levels);
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(dx.xy, 0.0), float3(dy.xy, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture2DArray<float4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(dx.xy, 0.0), float3(dy.xy, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture2DArray<int4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(dx.xy, 0.0), float3(dy.xy, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture2DArray<uint4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(dx.xy, 0.0), float3(dy.xy, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture3D<float4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float lod = rt_texture_footprint(float3(width, height, depth), dx, dy);
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture3D<int4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float lod = rt_texture_footprint(float3(width, height, depth), dx, dy);
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(Texture3D<uint4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float lod = rt_texture_footprint(float3(width, height, depth), dx, dy);
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(TextureCube<float4> texture_value, float3 coordinate, float3 dx, float3 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, levels);
	float3 direction = abs(coordinate.xyz);
	uint face = direction.x >= direction.y && direction.x >= direction.z ? 0u : direction.y >= direction.z ? 1u : 2u;
	float2 center = rt_cube_coordinate(coordinate.xyz, face);
	float2 gradient_x = rt_cube_coordinate(coordinate.xyz + dx.xyz, face) - center;
	float2 gradient_y = rt_cube_coordinate(coordinate.xyz + dy.xyz, face) - center;
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(gradient_x, 0.0), float3(gradient_y, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

float2 rt_texture_lod(TextureCubeArray<float4> texture_value, float4 coordinate, float4 dx, float4 dy) {
	uint width, height, depth, levels;
	texture_value.GetDimensions(0u, width, height, depth, levels);
	float3 direction = abs(coordinate.xyz);
	uint face = direction.x >= direction.y && direction.x >= direction.z ? 0u : direction.y >= direction.z ? 1u : 2u;
	float2 center = rt_cube_coordinate(coordinate.xyz, face);
	float2 gradient_x = rt_cube_coordinate(coordinate.xyz + dx.xyz, face) - center;
	float2 gradient_y = rt_cube_coordinate(coordinate.xyz + dy.xyz, face) - center;
	float lod = rt_texture_footprint(float3(width, height, 1u), float3(gradient_x, 0.0), float3(gradient_y, 0.0));
	return float2(clamp(lod, 0.0, float(max(levels, 1u) - 1u)), lod);
}

void rt_initialize_vertex(GeometryHitInput hit, GeometryTriangle triangle, uint index) {
	uint local_vertex = triangle.indices[index];
	VERTEX_INDEX = geometry_source_vertex_index(hit.geometry, local_vertex);
	geometry_fetch_bones(hit.geometry, local_vertex, bone_attrib, weight_attrib);
	vertex = geometry_fetch_position(hit.geometry, local_vertex);
	geometry_fetch_tbn(hit.geometry, triangle, local_vertex, normal_highp, tangent, binormal);
	float3 single_bary = float3(index == 0u ? 1.0 : 0.0, index == 1u ? 1.0 : 0.0, index == 2u ? 1.0 : 0.0);
	uv_interp = geometry_fetch_uv(hit.geometry, triangle.indices, single_bary);
	uv2_interp = geometry_fetch_uv2(hit.geometry, triangle.indices, single_bary);
	color_interp = geometry_fetch_color(hit.geometry, triangle.indices, single_bary);
	custom0_attrib = geometry_fetch_custom(hit.geometry, local_vertex, 0u);
	custom1_attrib = geometry_fetch_custom(hit.geometry, local_vertex, 1u);
	custom2_attrib = geometry_fetch_custom(hit.geometry, local_vertex, 2u);
	custom3_attrib = geometry_fetch_custom(hit.geometry, local_vertex, 3u);
	if (hit.geometry.multimesh_address != 0 && (hit.geometry.flags & FLAG_CLUSTERED) == 0u) {
		GeometryData source_geometry = hit.geometry;
		source_geometry.vertex_address = hit.geometry.source_vertex_address;
		source_geometry.vertex_count = hit.geometry.source_vertex_count;
		if (source_geometry.normal_byte_offset != OFFSET_NONE) {
			source_geometry.normal_byte_offset = source_geometry.vertex_count * source_geometry.position_stride;
		}
		if (source_geometry.tangent_byte_offset != OFFSET_NONE) {
			source_geometry.tangent_byte_offset = source_geometry.normal_byte_offset;
		}
		VERTEX_INDEX %= source_geometry.vertex_count;
		vertex = geometry_fetch_position(source_geometry, VERTEX_INDEX);
		geometry_fetch_tbn(source_geometry, triangle, VERTEX_INDEX, normal_highp, tangent, binormal);
	}
	roughness_highp = 1.0;
	point_size = 1.0;
	z_clip_scale = 1.0;
#ifdef VERTEX_WORLD_COORDS_USED
	vertex = mul(float4(vertex, 1.0), read_model_matrix).xyz;
	normal_highp = mul(normal_highp, model_normal_matrix);
	tangent = mul(tangent, (float3x3)read_model_matrix);
	binormal = mul(binormal, (float3x3)read_model_matrix);
#endif
}

void rt_accumulate_vertex(inout RTVertexInterpolants sums, float weight) {
	sums.uv += uv_interp * weight;
	sums.uv2 += uv2_interp * weight;
	sums.color += color_interp * weight;
	sums.custom0 += custom0_attrib * weight;
	sums.custom1 += custom1_attrib * weight;
	sums.custom2 += custom2_attrib * weight;
	sums.custom3 += custom3_attrib * weight;
#ifdef SKIP_TRANSFORM_USED
	sums.normal += normalize(normal_highp) * weight;
	sums.tangent += normalize(tangent) * weight;
	sums.binormal += normalize(binormal) * weight;
#elif defined(VERTEX_WORLD_COORDS_USED)
	sums.normal += normalize(mul(normal_highp, (float3x3)read_view_matrix)) * weight;
	sums.tangent += normalize(mul(tangent, (float3x3)read_view_matrix)) * weight;
	sums.binormal += normalize(mul(binormal, (float3x3)read_view_matrix)) * weight;
#else
	sums.normal += normalize(mul(normal_highp, modelview_normal)) * weight;
	sums.tangent += normalize(mul(tangent, (float3x3)modelview)) * weight;
	sums.binormal += normalize(mul(binormal, (float3x3)modelview)) * weight;
#endif
}

void rt_restore_vertex(RTVertexInterpolants sums) {
	uv_interp = sums.uv;
	uv2_interp = sums.uv2;
	color_interp = sums.color;
	custom0_attrib = sums.custom0;
	custom1_attrib = sums.custom1;
	custom2_attrib = sums.custom2;
	custom3_attrib = sums.custom3;
	normal_highp = sums.normal;
	tangent = sums.tangent;
	binormal = sums.binormal;
}

void rt_apply_decals(uint mask, float3 point, float3 geometry_normal, inout float3 normal) {
	uint filter = rt_material_frame.counts.w;
	for (uint index = 0u; index < rt_material_frame.counts.y; index++) {
		switch (filter) {
			case 0u:
				scene_apply_decal(decals[index], decal_atlas, decal_atlas_srgb, SAMPLER_NEAREST_CLAMP, false, mask, point, geometry_normal, rt_position_dx, rt_position_dy, albedo_highp, normal, ao, roughness_highp, metallic_highp, emission);
				break;
			case 1u:
				scene_apply_decal(decals[index], decal_atlas, decal_atlas_srgb, SAMPLER_LINEAR_CLAMP, false, mask, point, geometry_normal, rt_position_dx, rt_position_dy, albedo_highp, normal, ao, roughness_highp, metallic_highp, emission);
				break;
			case 2u:
				scene_apply_decal(decals[index], decal_atlas, decal_atlas_srgb, SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP, true, mask, point, geometry_normal, rt_position_dx, rt_position_dy, albedo_highp, normal, ao, roughness_highp, metallic_highp, emission);
				break;
			case 3u:
				scene_apply_decal(decals[index], decal_atlas, decal_atlas_srgb, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP, true, mask, point, geometry_normal, rt_position_dx, rt_position_dy, albedo_highp, normal, ao, roughness_highp, metallic_highp, emission);
				break;
			case 4u:
				scene_apply_decal(decals[index], decal_atlas, decal_atlas_srgb, SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP, true, mask, point, geometry_normal, rt_position_dx, rt_position_dy, albedo_highp, normal, ao, roughness_highp, metallic_highp, emission);
				break;
			case 5u:
				scene_apply_decal(decals[index], decal_atlas, decal_atlas_srgb, SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP, true, mask, point, geometry_normal, rt_position_dx, rt_position_dy, albedo_highp, normal, ao, roughness_highp, metallic_highp, emission);
				break;
		}
	}
}

SurfaceData rt_evaluate_material(GeometryHitInput hit, GeometryTriangle triangle, float3 ray_direction, float hit_t, bool native_front, float cone_width, float cone_spread) {
	rt_hit_distance = hit_t;
	scene_data = scene_data_block.data;
	rt_view_to_world = rt_camera_to_rt(rt_frame, false);
	rt_world_to_view = geometry_inverse(rt_view_to_world);
	float4x4 object_to_rt = float4x4(hit.object_to_world[0], hit.object_to_world[1], hit.object_to_world[2], float4(0.0, 0.0, 0.0, 1.0));
	object_to_rt = mul(object_to_rt, geometry_instance_transform(hit.geometry, triangle.indices[0]));
	modelview = transpose(mul(rt_world_to_view, object_to_rt));
	model_normal_matrix = (float3x3)geometry_inverse(object_to_rt);
	modelview_normal = mul(model_normal_matrix, transpose((float3x3)rt_world_to_view));
	read_model_matrix = transpose(object_to_rt);
	read_model_matrix[3].xyz += rt_material_frame.rt_origin.xyz;
	inv_view_matrix = transpose(rt_view_to_world);
	inv_view_matrix[3].xyz += rt_material_frame.rt_origin.xyz;
	read_view_matrix = transpose(rt_world_to_view);
	read_view_matrix[3].xyz = -mul(inv_view_matrix[3].xyz, (float3x3)read_view_matrix);
	projection_matrix = scene_data.projection_matrix;
	inv_projection_matrix = scene_data.inv_projection_matrix;
	read_viewport_size = scene_data.viewport_size;
	global_time = scene_data.time;
	global_prev_time = scene_data_block.prev_data.time;
	rt_material_address = materials[hit.geometry_index].uniform_address;
	rt_instance_uniforms_offset = hit.geometry.instance_uniforms_offset;
	INSTANCE_INDEX = geometry_instance_index(hit.geometry, triangle.indices[0]);
	instance_custom = geometry_instance_custom(hit.geometry, triangle.indices[0]);
	ViewIndex = 0u;
	eye_offset = float3(0.0);
	front_facing_builtin = RT_CULL_MODE == 1 ? !native_front : native_front;
	rt_material_discarded = false;
	rt_cone_radius = max(cone_width + hit_t * cone_spread, 1e-6);
	float3 object_position, object_dx, object_dy;
	float2 uv, uv_dx, uv_dy;
	rt_ray_footprint(hit, triangle, ray_direction, rt_cone_radius, object_position, object_dx, object_dy, uv, uv_dx, uv_dy);
	rt_load_material_uniforms();
	float3 bary = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
	rt_material_interpolate_vertices(hit, triangle, bary);
	float3 world_position = mul(hit.object_to_world, float4(object_position, 1.0));
	vertex = mul(rt_world_to_view, float4(world_position, 1.0)).xyz;
	float4 clip_position = mul(float4(vertex, 1.0), scene_data.projection_matrix);
	screen_uv = clip_position.xy / clip_position.w * 0.5 + 0.5;
	fragment_coord = float4(screen_uv * read_viewport_size, clip_position.z / clip_position.w, 1.0 / clip_position.w);
	fragment_depth = fragment_coord.z;
	light_vertex = vertex;
	view_highp = normalize(mul((float3x3)rt_world_to_view, -ray_direction));
#ifdef DO_SIDE_CHECK
	if (!front_facing_builtin) {
		normal_highp = -normal_highp;
		rt_interpolants_dx.normal = -rt_interpolants_dx.normal;
		rt_interpolants_dy.normal = -rt_interpolants_dy.normal;
	}
#endif
	albedo_highp = float3(1.0);
	alpha_highp = 1.0;
	roughness_highp = 1.0;
	metallic_highp = 0.0;
	specular = 0.5;
	emission = float3(0.0);
	ao = 1.0;
	ao_light_affect = 0.0;
	normal_map = float3(0.5);
	normal_map_depth = 1.0;
	bent_normal_map = float3(0.5);
	alpha_scissor_threshold = 1.0;
	alpha_hash_scale = 1.0;
	alpha_antialiasing_edge = 0.0;
	alpha_texture_coordinate = float2(0.0);
	premul_alpha = 1.0;
	rim = 0.0;
	rim_tint = 0.0;
	clearcoat = 0.0;
	clearcoat_roughness = 0.0;
	anisotropy = 0.0;
	anisotropy_flow = float2(1.0, 0.0);
	sss_strength = 0.0;
	transmittance_color = float4(0.0, 0.0, 0.0, 1.0);
	transmittance_depth = 0.0;
	transmittance_boost = 0.0;
	backlight = float3(0.0);
	fog = float4(0.0, 0.0, 0.0, 1.0);
	custom_radiance = float4(0.0);
	custom_irradiance = float4(0.0);
	rt_material_fragment();
	float3 decal_normal = normalize(normal_highp);
	float3 normal = normal_highp;
#ifdef NORMAL_MAP_USED
	normal_map.xy = normal_map.xy * 2.0 - 1.0;
	normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy)));
	normal = normalize(lerp(normal, tangent * normal_map.x + binormal * normal_map.y + normal * normal_map.z, normal_map_depth));
#endif
	normal = normalize(mul((float3x3)rt_view_to_world, normal));
	decal_normal = normalize(mul((float3x3)rt_view_to_world, decal_normal));
	rt_apply_decals(hit.geometry.instance_layer_mask, world_position, decal_normal, normal);
	SurfaceData surface = (SurfaceData)0;
	surface.world_position = world_position;
	surface.normal = normal;
	float3 edge1 = mul((float3x3)hit.object_to_world, triangle.p1 - triangle.p0);
	float3 edge2 = mul((float3x3)hit.object_to_world, triangle.p2 - triangle.p0);
	surface.geometry_normal = normalize(cross(edge1, edge2));
	surface.geometry_normal = faceforward(surface.geometry_normal, ray_direction, surface.geometry_normal);
	surface.view_direction = -ray_direction;
	surface.layer_mask = hit.geometry.instance_layer_mask;
	surface.material.base_color = albedo_highp;
	surface.material.opacity = alpha_highp;
	surface.material.roughness = roughness_highp;
	surface.material.metallic = metallic_highp;
	surface.material.specular = specular;
	surface.material.emission = emission;
	surface.material.ambient_occlusion = ao;
	surface.material.flags = RT_SURFACE_FLAGS;
	return surface;
}
#endif
#endif
