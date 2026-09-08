#ifndef DDGI_TRACE_INC_SLANG
#define DDGI_TRACE_INC_SLANG

#include "rt_hit_context_inc.slang"
#include "surface_brdf_inc.slang"
#include "rtxdi_nrd_inc.slang"
#include "../effects/ddgi_grid_inc.slang"
#include "../effects/ddgi_probe_inc.slang"
#define bindless_linear_clamp SAMPLER_LINEAR_CLAMP
#define bindless_linear_mip_repeat SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT
#include "light_sampling_inc.slang"

[[vk::binding(6, 2), vk::image_format("rgba32f")]] RWTexture2DArray<float4> ddgi_ray_data;
[[vk::binding(8, 2)]] Texture2DArray<float4> ddgi_traced_data;
[[vk::binding(9, 2)]] StructuredBuffer<RTLightData> ddgi_lights;
[[vk::binding(10, 2)]] ConstantBuffer<RTLightBufferParameters> ddgi_light_params;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
[[vk::binding(11, 2)]] Texture2DArray<float4> ddgi_sky;
#else
[[vk::binding(11, 2)]] Texture2D<float4> ddgi_sky;
#endif

uint ddgi_hash(uint value) {
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}

float ddgi_random(inout uint state) {
	state = ddgi_hash(state + 0x9e3779b9u);
	return float(state >> 8u) * (1.0 / 16777216.0);
}

#define native_light_params ddgi_light_params
#define native_lights ddgi_lights
#define native_sky ddgi_sky
#include "native_light_inc.slang"
#define ddgi_environment native_environment

float3 ddgi_direct(RTHitPayload hit, inout uint rng) {
	uint count = ddgi_light_params.total_count;
	if (count == 0u) {
		return 0.0;
	}
	RTLightData light = ddgi_lights[min(uint(ddgi_random(rng) * count), count - 1u)];
	if ((light.receiver_mask & hit.surface.layer_mask) == 0u) {
		return 0.0;
	}
	float2 random = float2(ddgi_random(rng), ddgi_random(rng));
	RTLightProposal proposal;
	if (!native_light_proposal(hit, light, random, 3.402823466e+38, proposal)) { return 0.0; }
	float3 diffuse, specular;
	surface_evaluate_brdf(hit.surface, proposal.direction, diffuse, specular);
	float measure = light.type == RT_LIGHT_TYPE_AREA || light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE || light.type == RT_LIGHT_TYPE_ENVIRONMENT ? max(proposal.solid_angle_pdf, 1e-20) : 1.0;
	return (diffuse + specular * light.specular_amount) * proposal.radiance * float(count) / measure;
}

#endif
