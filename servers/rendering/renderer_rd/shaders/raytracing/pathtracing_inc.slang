#ifndef PATH_TRACING_INC_SLANG
#define PATH_TRACING_INC_SLANG
#include "primary_surface_inc.slang"
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
[[vk::binding(8, 2), vk::image_format("rgba8")]] RWTexture2D<float4> pt_base;
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
void pt_write_primary(uint2 pixel, RTHitPayload hit, RayDesc ray) {
	PrimarySurfaceData primary = reconstruct_primary_surface(hit, ray, pt.extent_frame.xy, pt_motion_indices, pt_instance_motion);
	pt_base[pixel] = primary.surface.base;
	pt_shading[pixel] = primary.surface.shading;
	pt_emission[pixel] = primary.surface.emission;
	pt_motion[pixel] = primary.motion;
	pt_geometry[pixel] = primary.surface.geometry;
	pt_classification[pixel] = primary.surface.classification;
	pt_depth[pixel] = primary.depth;
}
#endif
