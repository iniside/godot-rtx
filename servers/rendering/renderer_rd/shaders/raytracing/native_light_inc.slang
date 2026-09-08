#ifndef NATIVE_LIGHT_INC_SLANG
#define NATIVE_LIGHT_INC_SLANG
float3 native_environment(float3 direction) {
	if (native_light_params.environment_present == 0u) { return 0.0; }
	SceneData scene = scene_data_block.data;
	float3x3 world_to_radiance = mul(transpose(scene.radiance_inverse_xform), transpose((float3x3)scene.inv_view_matrix));
	float2 uv = vec3_to_oct_with_border(mul(world_to_radiance, direction), float2(scene.radiance_border_size, 1.0 - scene.radiance_border_size * 2.0));
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float3 radiance = native_sky.SampleLevel(SAMPLER_LINEAR_CLAMP, float3(uv, 0.0), 0.0).rgb;
#else
	float3 radiance = native_sky.SampleLevel(SAMPLER_LINEAR_CLAMP, uv, 0.0).rgb;
#endif
	return max(radiance * native_lights[native_light_params.environment_index].emission, 0.0);
}

RTHitPayload native_trace(RayDesc ray, uint mask, uint flags, float width, float spread) {
	RTHitPayload hit = (RTHitPayload)0;
	hit.ray_layer_mask = mask; hit.ray_flags = flags; hit.cone_width = width; hit.cone_spread = spread;
	TraceRay(scene_tlas, RAY_FLAG_FORCE_NON_OPAQUE | ((flags & 1u) != 0u ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : 0u), 0xFF, 0, 0, 0, ray, hit);
	return hit;
}
bool native_light_proposal(RTHitPayload hit, RTLightData light, float2 random, float max_distance, out RTLightProposal result) {
	result = (RTLightProposal)0;
	RTLightProposal proposal;
	if (light.type == RT_LIGHT_TYPE_ENVIRONMENT) {
		proposal = light_sample_environment(random, 1.0);
		proposal.radiance = native_environment(proposal.direction);
	} else { proposal = light_sample_analytic_light(light, hit.surface.world_position, random); }
	if (proposal.valid == 0u || dot(hit.surface.geometry_normal, proposal.direction) <= 0.0) { return false; }
	RayDesc ray;
	ray.Origin = hit.surface.world_position; ray.Direction = proposal.direction;
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		ray.TMin = max(proposal.distance - 0.002, 0.001); ray.TMax = proposal.distance + 0.002;
		RTHitPayload emitter = native_trace(ray, 0xFFFFFFFFu, 2u, hit.cone_width, hit.cone_spread);
		if (emitter.hit == 0u || emitter.geometry_index != light.geometry_index || emitter.primitive_index != light.primitive_index) { return false; }
		proposal.radiance = emitter.surface.material.emission;
	}
	ray.TMin = 0.001;
	ray.TMax = light.type == RT_LIGHT_TYPE_ENVIRONMENT || light.type == RT_LIGHT_TYPE_DIRECTIONAL ? max_distance : max(proposal.distance - 0.002, 0.001);
	if ((light.flags & RT_LIGHT_FLAG_CASTS_SHADOW) != 0u) {
		RTHitPayload shadow = native_trace(ray, light.caster_mask, 1u, hit.cone_width, hit.cone_spread);
		if (shadow.hit != 0u) { return false; }
	}
	result = proposal;
	return true;
}
#endif
