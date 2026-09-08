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

float3 ddgi_environment(float3 direction) {
	if (ddgi_light_params.environment_present == 0u) {
		return 0.0;
	}
	SceneData scene = scene_data_block.data;
	float3x3 world_to_radiance = mul(transpose(scene.radiance_inverse_xform), transpose((float3x3)scene.inv_view_matrix));
	float2 uv = vec3_to_oct_with_border(mul(world_to_radiance, direction), float2(scene.radiance_border_size, 1.0 - scene.radiance_border_size * 2.0));
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float3 radiance = ddgi_sky.SampleLevel(ddgi_sampler, float3(uv, 0.0), 0.0).rgb;
#else
	float3 radiance = ddgi_sky.SampleLevel(ddgi_sampler, uv, 0.0).rgb;
#endif
	return max(radiance * ddgi_lights[ddgi_light_params.environment_index].emission, 0.0);
}

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
	if (light.type == RT_LIGHT_TYPE_ENVIRONMENT) {
		proposal = light_sample_environment(random, 1.0);
		proposal.radiance = ddgi_environment(proposal.direction);
	} else {
		proposal = light_sample_analytic_light(light, hit.surface.world_position, random);
	}
	if (proposal.valid == 0u || dot(hit.surface.geometry_normal, proposal.direction) <= 0.0) {
		return 0.0;
	}
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		RayDesc emission_ray;
		emission_ray.Origin = hit.surface.world_position;
		emission_ray.Direction = proposal.direction;
		emission_ray.TMin = max(proposal.distance - 0.002, 0.001);
		emission_ray.TMax = proposal.distance + 0.002;
		RTHitPayload emitter = (RTHitPayload)0;
		emitter.ray_flags = 2u;
		emitter.ray_layer_mask = 0xFFFFFFFFu;
		emitter.cone_width = hit.cone_width;
		emitter.cone_spread = hit.cone_spread;
		TraceRay(scene_tlas, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, 0, 0, 0, emission_ray, emitter);
		if (emitter.hit == 0u || emitter.geometry_index != light.geometry_index || emitter.primitive_index != light.primitive_index) {
			return 0.0;
		}
		proposal.radiance = emitter.surface.material.emission;
	}
	RayDesc ray;
	ray.Origin = hit.surface.world_position;
	ray.Direction = proposal.direction;
	ray.TMin = 0.001;
	ray.TMax = light.type == RT_LIGHT_TYPE_ENVIRONMENT || light.type == RT_LIGHT_TYPE_DIRECTIONAL ? 3.402823466e+38 : max(proposal.distance - 0.002, 0.001);
	if ((light.flags & RT_LIGHT_FLAG_CASTS_SHADOW) != 0u) {
		RTHitPayload shadow = (RTHitPayload)0;
		shadow.ray_flags = 1u;
		shadow.ray_layer_mask = light.caster_mask;
		shadow.cone_width = hit.cone_width;
		shadow.cone_spread = hit.cone_spread;
		TraceRay(scene_tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, 0, 0, 0, ray, shadow);
		if (shadow.hit != 0u) {
			return 0.0;
		}
	}
	float3 diffuse, specular;
	surface_evaluate_brdf(hit.surface, proposal.direction, diffuse, specular);
	float measure = light.type == RT_LIGHT_TYPE_AREA || light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE || light.type == RT_LIGHT_TYPE_ENVIRONMENT ? max(proposal.solid_angle_pdf, 1e-20) : 1.0;
	return (diffuse + specular * light.specular_amount) * proposal.radiance * float(count) / measure;
}

#endif
