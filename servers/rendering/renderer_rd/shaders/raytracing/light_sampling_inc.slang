#ifndef LIGHT_SAMPLING_INC_SLANG
#define LIGHT_SAMPLING_INC_SLANG

#include "light_data_inc.slang"

float light_target_pdf(float3 contribution) {
	return dot(abs(contribution), float3(0.2126, 0.7152, 0.0722));
}

uint light_sample_uniform_light(uint first, uint count, float random, out float selection_pdf) {
	if (count == 0u) {
		selection_pdf = 0.0;
		return 0xFFFFFFFFu;
	}
	selection_pdf = 1.0 / float(count);
	return first + min(uint(random * float(count)), count - 1u);
}

float light_uniform_light_pdf(uint index, uint first, uint count) {
	return count > 0u && index >= first && index < first + count ? 1.0 / float(count) : 0.0;
}

float4x4 light_decode_light_transform(RTLightData light) {
	return float4x4(
			float4(light.transform[0], light.transform[1], light.transform[2], light.transform[3]),
			float4(light.transform[4], light.transform[5], light.transform[6], light.transform[7]),
			float4(light.transform[8], light.transform[9], light.transform[10], light.transform[11]),
			float4(0.0, 0.0, 0.0, 1.0));
}

float3 light_evaluate_emission_texture(RTLightData light, float2 uv) {
	if ((light.flags & RT_LIGHT_FLAG_TEXTURED) == 0u) {
		return light.emission;
	}
	float2 material_uv = uv * light.uv_rect.xy + light.uv_rect.zw;
	return light.emission * bindless_textures[NonUniformResourceIndex(light.texture_index)].SampleLevel(bindless_linear_mip_repeat, material_uv, 0.0).rgb;
}

GeometryData light_geometry(RTLightData light) {
	GeometryData geometry = geometries[light.geometry_index];
	return light._pad0.x != 0u ? geometry_previous_positions(geometry) : geometry;
}

RTLightProposal light_sample_emissive_triangle(RTLightData light, float3 receiver_position, float2 random) {
	RTLightProposal proposal = (RTLightProposal)0;
	GeometryData geometry = light_geometry(light);
	uint3 indices;
	if (!geometry_triangle_indices(geometry, light.primitive_index, indices)) {
		return proposal;
	}
	float4x4 object_to_world = light_decode_light_transform(light);
	float3 p0 = mul(object_to_world, float4(geometry_fetch_position(geometry, indices[0]), 1.0)).xyz;
	float3 p1 = mul(object_to_world, float4(geometry_fetch_position(geometry, indices[1]), 1.0)).xyz;
	float3 p2 = mul(object_to_world, float4(geometry_fetch_position(geometry, indices[2]), 1.0)).xyz;
	float root = sqrt(random.x);
	float3 barycentrics = float3(1.0 - root, root * (1.0 - random.y), root * random.y);
	proposal.position = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
	float3 to_light = proposal.position - receiver_position;
	float distance_squared = dot(to_light, to_light);
	proposal.distance = sqrt(distance_squared);
	proposal.direction = to_light / max(proposal.distance, 1e-10);
	float3 normal_cross = cross(p1 - p0, p2 - p0);
	float twice_area = length(normal_cross);
	float projected_cosine = abs(dot(normal_cross / max(twice_area, 1e-10), -proposal.direction));
	proposal.solid_angle_pdf = distance_squared * 2.0 / max(twice_area * projected_cosine, 1e-10);
	float2 uv = geometry_fetch_uv(geometry, indices, barycentrics);
	proposal.radiance = light_evaluate_emission_texture(light, uv);
	proposal.valid = twice_area > 0.0 && projected_cosine > 0.0 ? 1u : 0u;
	return proposal;
}

RTLightProposal light_sample_area_light(RTLightData light, float3 receiver_position, float2 random) {
	RTLightProposal proposal = (RTLightProposal)0;
	proposal.position = light.position + (random.x - 0.5) * light.axis_u + (random.y - 0.5) * light.axis_v;
	float3 to_light = proposal.position - receiver_position;
	float distance_squared = dot(to_light, to_light);
	proposal.distance = sqrt(distance_squared);
	proposal.direction = to_light / max(proposal.distance, 1e-10);
	float projected_cosine = max(dot(normalize(light.direction), -proposal.direction), 0.0);
	proposal.solid_angle_pdf = light.inv_area * distance_squared / max(projected_cosine, 1e-10);
	float axis_u_length = length(light.axis_u);
	float axis_v_length = length(light.axis_v);
	float3 center_to_receiver = receiver_position - light.position;
	float3 local_receiver = float3(dot(center_to_receiver, light.axis_u / max(axis_u_length, 1e-10)), dot(center_to_receiver, light.axis_v / max(axis_v_length, 1e-10)), dot(center_to_receiver, -normalize(light.direction)));
	float3 closest_local = float3(clamp(local_receiver.x, -0.5 * axis_u_length, 0.5 * axis_u_length), clamp(local_receiver.y, -0.5 * axis_v_length, 0.5 * axis_v_length), 0.0);
	float closest_distance = length(closest_local - local_receiver);
	float normalized_distance = closest_distance / max(light.range, 0.0001);
	normalized_distance *= normalized_distance;
	normalized_distance *= normalized_distance;
	float range_window = max(1.0 - normalized_distance, 0.0);
	range_window *= range_window;
	float attenuation = range_window * pow(max(closest_distance, 0.0001), -light.attenuation) * closest_distance * closest_distance;
	proposal.radiance = light.emission * attenuation;
	if ((light.flags & RT_LIGHT_FLAG_TEXTURED) != 0u) {
		float2 atlas_uv = light.uv_rect.xy + random * light.uv_rect.zw;
		proposal.radiance *= bindless_textures[NonUniformResourceIndex(light.texture_index)].SampleLevel(bindless_linear_clamp, atlas_uv, 0.0).rgb;
	}
	proposal.valid = light.inv_area > 0.0 && projected_cosine > 0.0 ? 1u : 0u;
	return proposal;
}

struct LightSample {
	float3 cone_axis;
	float cos_theta_max;
	float3 emission;
	float distance_sq;
	float max_distance;
};

void lights_build_basis(float3 dir, out float3 tangent, out float3 bitangent) {
	float3 up = abs(dir.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

float3 lights_local_to_world(float3 local_dir, float3 axis) {
	float3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

LightSample lights_prepare_sample(float3 hit_pos, RTLightData light) {
	LightSample s;

	float3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = rsqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;

	float3 sphere_axis = to_light * inv_dist;
	float3 dir_axis = -normalize(light.direction);
	s.cone_axis = lerp(sphere_axis, dir_axis, is_directional);

	s.distance_sq = lerp(dist_sq, 0.0, is_directional);

	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = lerp(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	float sphere_max = dist + light.radius;
	float dir_max = 3.402823466e+38;
	s.max_distance = lerp(sphere_max, dir_max, is_directional);

	return s;
}

float3 lights_sample_cone(LightSample ls, float2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	float3 local_dir = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	float3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

float3 light_evaluate_projector(RTLightData light, float3 receiver_position) {
	if ((light.flags & RT_LIGHT_FLAG_TEXTURED) == 0u) {
		return float3(1.0);
	}
	float3 local_position = mul(light_decode_light_transform(light), float4(receiver_position, 1.0)).xyz;
	float2 projector_uv;
	if (light.type == RT_LIGHT_TYPE_OMNI) {
		float3 local_direction = normalize(local_position);
		float4 atlas_rect = light.uv_rect;
		if (local_direction.z >= 0.0) {
			atlas_rect.y += atlas_rect.w;
		}
		local_direction.z = 1.0 + abs(local_direction.z);
		projector_uv = (local_direction.xy / local_direction.z * 0.5 + 0.5) * atlas_rect.zw + atlas_rect.xy;
	} else {
		float tangent = sqrt(max(1.0 - light.cos_spot_angle * light.cos_spot_angle, 0.0)) / max(light.cos_spot_angle, 1e-6);
		projector_uv = (local_position.xy / max(-local_position.z * tangent, 1e-6) * 0.5 + 0.5) * light.uv_rect.zw + light.uv_rect.xy;
	}
	float4 projected = bindless_textures[NonUniformResourceIndex(light.texture_index)].SampleLevel(bindless_linear_clamp, projector_uv, 0.0);
	return projected.rgb * projected.a;
}

RTLightProposal light_sample_analytic_light(RTLightData light, float3 receiver_position, float2 random) {
	if (light.type == RT_LIGHT_TYPE_AREA) {
		return light_sample_area_light(light, receiver_position, random);
	}
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		return light_sample_emissive_triangle(light, receiver_position, random);
	}
	RTLightProposal proposal = (RTLightProposal)0;
	proposal.solid_angle_pdf = 1.0;
	LightSample cone = lights_prepare_sample(receiver_position, light);
	if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		proposal.direction = light.radius > 0.0 ? lights_sample_cone(cone, random, proposal.solid_angle_pdf) : -normalize(light.direction);
		proposal.distance = cone.max_distance;
		proposal.radiance = light.emission;
		proposal.valid = 1u;
		return proposal;
	}
	float3 to_light = light.position - receiver_position;
	float distance_squared = dot(to_light, to_light);
	float center_distance = sqrt(distance_squared);
	if (light.range > 0.0 && center_distance > light.range) {
		return proposal;
	}
	if (light.radius > 0.0) {
		proposal.direction = lights_sample_cone(cone, random, proposal.solid_angle_pdf);
		float center_t = dot(to_light, proposal.direction);
		float3 perpendicular = to_light - center_t * proposal.direction;
		proposal.distance = max(center_t - sqrt(max(light.radius * light.radius - dot(perpendicular, perpendicular), 0.0)), 0.0);
	} else {
		proposal.direction = to_light / max(center_distance, 1e-10);
		proposal.distance = center_distance;
	}
	proposal.position = receiver_position + proposal.direction * proposal.distance;
	float attenuation = min(pow(max(center_distance, 0.0001), -light.attenuation), 1.0);
	if (light.range > 0.0) {
		float window = center_distance / light.range;
		window *= window;
		window *= window;
		window = max(1.0 - window, 0.0);
		attenuation *= window * window;
	}
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		float spot_cosine = dot(-proposal.direction, light.direction);
		if (spot_cosine <= light.cos_spot_angle) {
			return proposal;
		}
		float rim = max(1e-4, (1.0 - spot_cosine) / (1.0 - light.cos_spot_angle));
		attenuation *= 1.0 - pow(rim, light.inv_spot_attenuation);
	}
	proposal.radiance = light.emission * attenuation * light_evaluate_projector(light, receiver_position);
	proposal.valid = 1u;
	return proposal;
}

float light_evaluate_light_solid_angle_pdf(RTLightData light, float3 receiver_position, float3 sampled_position, float3 sampled_direction) {
	if (light.type == RT_LIGHT_TYPE_ENVIRONMENT) {
		return 1.0 / (4.0 * PI);
	}
	if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		float solid_angle = 2.0 * PI * (1.0 - cos(light.radius));
		return light.radius > 0.0 ? 1.0 / max(solid_angle, 1e-10) : 1.0;
	}
	if (light.type == RT_LIGHT_TYPE_AREA) {
		float distance_squared = dot(sampled_position - receiver_position, sampled_position - receiver_position);
		float projected_cosine = max(dot(normalize(light.direction), -sampled_direction), 0.0);
		return light.inv_area * distance_squared / max(projected_cosine, 1e-10);
	}
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		GeometryData geometry = light_geometry(light);
		uint3 indices;
		if (!geometry_triangle_indices(geometry, light.primitive_index, indices)) {
			return 0.0;
		}
		float4x4 object_to_world = light_decode_light_transform(light);
		float3 p0 = mul(object_to_world, float4(geometry_fetch_position(geometry, indices[0]), 1.0)).xyz;
		float3 p1 = mul(object_to_world, float4(geometry_fetch_position(geometry, indices[1]), 1.0)).xyz;
		float3 p2 = mul(object_to_world, float4(geometry_fetch_position(geometry, indices[2]), 1.0)).xyz;
		float3 normal_cross = cross(p1 - p0, p2 - p0);
		float twice_area = length(normal_cross);
		float distance_squared = dot(sampled_position - receiver_position, sampled_position - receiver_position);
		float projected_cosine = abs(dot(normal_cross / max(twice_area, 1e-10), -sampled_direction));
		return distance_squared * 2.0 / max(twice_area * projected_cosine, 1e-10);
	}
	if (light.radius > 0.0) {
		float distance = length(light.position - receiver_position);
		float sine = clamp(light.radius / max(distance, 1e-10), 0.0, 1.0);
		float solid_angle = 2.0 * PI * (1.0 - sqrt(max(1.0 - sine * sine, 0.0)));
		return 1.0 / max(solid_angle, 1e-10);
	}
	return 1.0;
}

RTLightProposal light_sample_environment(float2 random, float3 radiance) {
	RTLightProposal proposal = (RTLightProposal)0;
	proposal.distance = 1e10;
	proposal.radiance = radiance;
	proposal.solid_angle_pdf = 1.0 / (4.0 * PI);
	proposal.valid = 1u;
	float z = 1.0 - 2.0 * random.x;
	float radius = sqrt(max(1.0 - z * z, 0.0));
	float phi = 2.0 * PI * random.y;
	proposal.direction = float3(radius * cos(phi), radius * sin(phi), z);
	return proposal;
}

#endif
