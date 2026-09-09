#ifndef PATH_TRACING_INC_SLANG
#define PATH_TRACING_INC_SLANG
#include "rt_hit_context_inc.slang"
#include "surface_brdf_inc.slang"
#include "rtxdi_nrd_inc.slang"
#define bindless_linear_clamp SAMPLER_LINEAR_CLAMP
#define bindless_linear_mip_repeat SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT
#include "light_sampling_inc.slang"
struct PTFrame {
	uint4 extent_frame;
	uint4 settings;
	float4 options;
	float4 background;
};
[[vk::binding(0, 2)]] ConstantBuffer<PTFrame> pt;
[[vk::binding(1, 2)]] StructuredBuffer<RTLightData> pt_lights;
[[vk::binding(2, 2)]] ConstantBuffer<RTLightBufferParameters> pt_light_params;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
[[vk::binding(3, 2)]] Texture2DArray<float4> pt_sky;
#else
[[vk::binding(3, 2)]] Texture2D<float4> pt_sky;
#endif
[[vk::binding(4, 2), vk::image_format("rgba32f")]] RWTexture2D<float4> pt_accumulation;
[[vk::binding(5, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_radiance;
[[vk::binding(6, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_diffuse;
[[vk::binding(7, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_specular;
[[vk::binding(8, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_base;
[[vk::binding(9, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_shading;
[[vk::binding(10, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_emission;
[[vk::binding(11, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> pt_motion;
[[vk::binding(12, 2), vk::image_format("r32ui")]] RWTexture2D<uint> pt_geometry;
[[vk::binding(13, 2), vk::image_format("rg32ui")]] RWTexture2D<uint2> pt_classification;
[[vk::binding(14, 2), vk::image_format("r32f")]] RWTexture2D<float> pt_depth;
[[vk::binding(15, 2)]] StructuredBuffer<int> pt_motion_indices;
[[vk::binding(16, 2)]] StructuredBuffer<InstanceMotionData> pt_instance_motion;

uint pt_hash(uint x) {
	x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; return x ^ (x >> 16u);
}
float pt_random(inout uint rng) {
	rng = pt_hash(rng + 0x9e3779b9u); return float(rng >> 8u) / 16777216.0;
}
float pt_mis(float a, float b) {
	float scale = max(max(a, b), 1e-20); a /= scale; b /= scale;
	return a * a / max(a * a + b * b, 1e-20);
}
#define native_light_params pt_light_params
#define native_lights pt_lights
#define native_sky pt_sky
#include "native_light_inc.slang"
#define pt_environment native_environment
#define pt_trace native_trace
void pt_direct(RTHitPayload hit, bool continue_path, inout uint rng, out float3 diffuse, out float3 specular) {
	diffuse = 0.0; specular = 0.0;
	uint count = pt_light_params.total_count;
	if (count == 0u) { return; }
	RTLightData light = pt_lights[min(uint(pt_random(rng) * count), count - 1u)];
	if ((light.receiver_mask & hit.surface.layer_mask) == 0u) { return; }
	float2 random = float2(pt_random(rng), pt_random(rng));
	RTLightProposal proposal;
	if (!native_light_proposal(hit, light, random, pt.options.y, proposal)) { return; }
	bool area = light.type == RT_LIGHT_TYPE_AREA || light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE || light.type == RT_LIGHT_TYPE_ENVIRONMENT;
	float pdf = (area ? max(proposal.solid_angle_pdf, 1e-20) : 1.0) / float(count);
	// Analytic lights have no intersectable emitter in the shared TLAS.
	bool competing = light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE || light.type == RT_LIGHT_TYPE_ENVIRONMENT;
	float weight = competing && continue_path ? pt_mis(pdf, surface_brdf_pdf(hit.surface, proposal.direction)) : 1.0;
	surface_evaluate_brdf(hit.surface, proposal.direction, diffuse, specular);
	diffuse *= proposal.radiance * (weight / pdf);
	specular *= proposal.radiance * (weight / pdf) * light.specular_amount;
}
float pt_emitter_pdf(RTHitPayload hit, float3 origin, float3 direction, uint receiver_mask) {
	float pdf = 0.0;
	for (uint i = 0u; i < pt_light_params.total_count; i++) {
		RTLightData light = pt_lights[i];
		if ((light.receiver_mask & receiver_mask) == 0u) { continue; }
		bool match = hit.hit == 0u ? light.type == RT_LIGHT_TYPE_ENVIRONMENT :
			light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE && light.geometry_index == hit.geometry_index && light.primitive_index == hit.primitive_index;
		if (match) { pdf += light_evaluate_light_solid_angle_pdf(light, origin, hit.surface.world_position, direction); }
	}
	return pdf / float(max(pt_light_params.total_count, 1u));
}
float3 pt_previous_position(RTHitPayload hit) {
	int motion_index = pt_motion_indices[hit.geometry_index];
	if (motion_index < 0) { return hit.surface.world_position; }
	GeometryData geometry = geometry_previous_positions(geometries[hit.geometry_index]);
	uint3 indices;
	uint primitive;
	if (!geometry_select_triangle(geometry, hit.cluster_id, hit.local_primitive_index, primitive) || !geometry_triangle_indices(geometry, primitive, indices)) { return hit.surface.world_position; }
	float3 bary = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
	float3 position = geometry_fetch_position(geometry, indices.x) * bary.x + geometry_fetch_position(geometry, indices.y) * bary.y + geometry_fetch_position(geometry, indices.z) * bary.z;
	InstanceMotionData motion = pt_instance_motion[motion_index];
	return float3(dot(float4(position, 1.0), float4(motion.prev_object_to_rt[0], motion.prev_object_to_rt[1], motion.prev_object_to_rt[2], motion.prev_object_to_rt[3])),
		dot(float4(position, 1.0), float4(motion.prev_object_to_rt[4], motion.prev_object_to_rt[5], motion.prev_object_to_rt[6], motion.prev_object_to_rt[7])),
		dot(float4(position, 1.0), float4(motion.prev_object_to_rt[8], motion.prev_object_to_rt[9], motion.prev_object_to_rt[10], motion.prev_object_to_rt[11])));
}
void pt_write_primary(uint2 pixel, RTHitPayload hit, RayDesc ray) {
	float4x4 camera_to_rt = rt_camera_to_rt(rt_frame, false);
	float4x4 previous_to_rt = rt_camera_to_rt(rt_frame, true);
	float3 current_view, previous_view;
	SurfaceBufferData packed = (SurfaceBufferData)0;
	if (hit.hit != 0u) {
		packed = encode_surface_buffers(hit.surface);
		packed.emission.rgb *= scene_data_block.data.emissive_exposure_normalization;
		current_view = mul(geometry_inverse(camera_to_rt), float4(hit.surface.world_position, 1.0)).xyz;
		previous_view = mul(geometry_inverse(previous_to_rt), float4(pt_previous_position(hit), 1.0)).xyz;
	} else {
		current_view = mul(transpose((float3x3)camera_to_rt), ray.Direction);
		previous_view = mul(transpose((float3x3)previous_to_rt), ray.Direction);
	}
	float4 current_clip = mul(float4(current_view, hit.hit != 0u ? 1.0 : 0.0), scene_data_block.data.projection_matrix);
	float4 previous_clip = mul(float4(previous_view, hit.hit != 0u ? 1.0 : 0.0), scene_data_block.prev_data.projection_matrix);
	float2 motion = 0.0;
	if (abs(current_clip.w) > 1e-6 && abs(previous_clip.w) > 1e-6) {
		motion = ((previous_clip.xy / previous_clip.w - scene_data_block.prev_data.taa_jitter) - (current_clip.xy / current_clip.w - scene_data_block.data.taa_jitter)) * 0.5 * float2(pt.extent_frame.xy);
	}
	pt_base[pixel] = packed.base; pt_shading[pixel] = packed.shading; pt_emission[pixel] = packed.emission;
	pt_geometry[pixel] = packed.geometry; pt_classification[pixel] = packed.classification;
	pt_motion[pixel] = float4(motion, hit.hit != 0u ? current_view.z - previous_view.z : 0.0, packed.ambient_occlusion);
	pt_depth[pixel] = hit.hit != 0u ? current_clip.z / current_clip.w : 0.0;
}
#endif
