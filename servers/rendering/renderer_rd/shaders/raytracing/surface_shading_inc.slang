#ifndef SURFACE_SHADING_INC_SLANG
#define SURFACE_SHADING_INC_SLANG

struct SurfaceLightSample {
	float3 direction;
	float distance;
	float3 radiance;
	float solid_angle_pdf;
	float3 position;
	uint light_type;
	uint flags;
	uint receiver_mask;
	uint caster_mask;
	uint geometry_index;
	uint primitive_index;
	uint valid;
	float specular_amount;
};

float surface_light_measure(SurfaceLightSample light_sample) {
	return light_sample.light_type == RT_LIGHT_TYPE_AREA || light_sample.light_type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE || light_sample.light_type == RT_LIGHT_TYPE_ENVIRONMENT ? max(light_sample.solid_angle_pdf, 1e-20) : 1.0;
}

float surface_light_target_pdf(SurfaceLightSample light_sample, SurfaceData surface) {
	if (light_sample.valid == 0u || (light_sample.receiver_mask & surface.layer_mask) == 0u || dot(surface.geometry_normal, light_sample.direction) <= 0.0 || dot(surface.normal, surface.view_direction) <= 0.0) {
		return 0.0;
	}
	float3 diffuse;
	float3 specular;
	surface_evaluate_brdf(surface, light_sample.direction, diffuse, specular);
	return light_target_pdf((diffuse + specular * light_sample.specular_amount) * light_sample.radiance / surface_light_measure(light_sample));
}

#include "material_coverage_inc.slang"

bool geometry_alpha_covered(GeometryHitInput hit, MaterialData material, GeometryFootprint footprint, bool shadow_ray, uint caster_mask) {
	GeometryTriangle triangle;
	if (!geometry_decode_triangle(hit, triangle)) {
		return false;
	}
	float3 object_position;
	float3 position_dx;
	float3 position_dy;
	float2 uv;
	float2 uv_dx;
	float2 uv_dy;
	geometry_footprint_derivatives(hit, triangle, footprint, object_position, position_dx, position_dy, uv, uv_dx, uv_dy);
	return geometry_material_covered(hit, material, object_position, position_dx, position_dy, uv, uv_dx, uv_dy, shadow_ray, caster_mask);
}

float3 sample_environment_radiance(float3 world_direction, float3x3 world_to_radiance, float2 border) {
	float2 uv = vec3_to_oct_with_border(mul(world_to_radiance, world_direction), border);
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	return radiance_octmap.SampleLevel(environment_sampler, float3(uv, 0.0), 0.0).rgb;
#else
	return radiance_octmap.SampleLevel(environment_sampler, uv, 0.0).rgb;
#endif
}

SurfaceLightSample sample_surface_light(RTLightData light, SurfaceData surface, float2 random, GeometryFootprint footprint, float3x3 world_to_radiance, float2 border) {
	SurfaceLightSample light_sample = (SurfaceLightSample)0;
	light_sample.specular_amount = 1.0;
	if (light.receiver_mask == 0u) {
		return light_sample;
	}
	RTLightProposal proposal;
	if (light.type == RT_LIGHT_TYPE_ENVIRONMENT) {
		proposal = light_sample_environment(random, float3(1.0));
		proposal.radiance = sample_environment_radiance(proposal.direction, world_to_radiance, border) * light.emission;
	} else {
		if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
			uint geometry_count;
			uint stride;
			geometries.GetDimensions(geometry_count, stride);
			if (light.geometry_index >= geometry_count) {
				return light_sample;
			}
		}
		proposal = light_sample_analytic_light(light, surface.world_position, random);
	}
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		float root = sqrt(random.x);
		float2 barycentrics = float2(root * (1.0 - random.y), root * random.y);
		GeometryHitInput hit;
		MaterialData material;
		float3x4 object_to_world = (float3x4)light_decode_light_transform(light);
		if (!load_geometry_hit(light.geometry_index, light.primitive_index, -1, barycentrics, object_to_world, hit, material)) {
			return light_sample;
		}
		hit.geometry = light_geometry(light);
		if (!geometry_alpha_covered(hit, material, footprint, false, 0xFFFFFFFFu)) {
			proposal.radiance = float3(0.0);
		}
	}
	light_sample.direction = proposal.direction;
	light_sample.distance = proposal.distance;
	light_sample.radiance = proposal.radiance;
	light_sample.solid_angle_pdf = proposal.solid_angle_pdf;
	light_sample.position = proposal.position;
	light_sample.light_type = light.type;
	light_sample.flags = light.flags;
	light_sample.receiver_mask = light.receiver_mask;
	light_sample.caster_mask = light.caster_mask;
	light_sample.geometry_index = light.geometry_index;
	light_sample.primitive_index = light.primitive_index;
	light_sample.valid = proposal.valid;
	light_sample.specular_amount = light.specular_amount;
	return light_sample;
}

bool trace_light_visibility(SurfaceData surface, SurfaceLightSample light_sample, GeometryFootprint footprint) {
	if ((light_sample.flags & RT_LIGHT_FLAG_CASTS_SHADOW) == 0u) {
		return true;
	}
	RayDesc ray;
	ray.Origin = surface.world_position;
	ray.Direction = light_sample.direction;
	ray.TMin = 0.001;
	ray.TMax = light_sample.light_type == RT_LIGHT_TYPE_ENVIRONMENT || light_sample.light_type == RT_LIGHT_TYPE_DIRECTIONAL ? 3.402823466e+38 : max(light_sample.distance - 0.002, 0.001);
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE> query;
	query.TraceRayInline(scene_tlas, RAY_FLAG_NONE, 0xFF, ray);
	while (query.Proceed()) {
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
			GeometryHitInput hit;
			MaterialData material;
			if (load_geometry_hit(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateClusterID(), query.CandidateTriangleBarycentrics(), query.CandidateObjectToWorld3x4(), hit, material)) {
				bool shadow_facing = (hit.geometry.flags & FLAG_SHADOW_CULL_ENABLED) == 0u || !query.CandidateTriangleFrontFace();
				if (shadow_facing && geometry_alpha_covered(hit, material, footprint, true, light_sample.caster_mask)) {
					query.CommitNonOpaqueTriangleHit();
				}
			}
		}
	}
	return query.CommittedStatus() == COMMITTED_NOTHING;
}

bool trace_local_geometry(float3 origin, float3 direction, float min_t, float max_t, GeometryFootprint footprint, out uint geometry_index, out uint primitive_index, out float2 barycentrics) {
	geometry_index = OFFSET_NONE;
	primitive_index = OFFSET_NONE;
	barycentrics = float2(0.0);
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = min_t;
	ray.TMax = max_t;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
	query.TraceRayInline(scene_tlas, RAY_FLAG_NONE, 0xFF, ray);
	while (query.Proceed()) {
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
			GeometryHitInput hit;
			MaterialData material;
			if (load_geometry_hit(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateClusterID(), query.CandidateTriangleBarycentrics(), query.CandidateObjectToWorld3x4(), hit, material) && geometry_alpha_covered(hit, material, footprint, false, 0xFFFFFFFFu)) {
				query.CommitNonOpaqueTriangleHit();
			}
		}
	}
	if (query.CommittedStatus() == COMMITTED_NOTHING) {
		return false;
	}
	geometry_index = query.CommittedInstanceID();
	if (!geometry_resolve_primitive(geometries[geometry_index], query.CommittedClusterID(), query.CommittedPrimitiveIndex(), primitive_index)) {
		return false;
	}
	barycentrics = query.CommittedTriangleBarycentrics();
	return true;
}

#endif
