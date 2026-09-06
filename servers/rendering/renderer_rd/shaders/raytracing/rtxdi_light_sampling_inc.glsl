// Shared light registry, proposal sampling, and PDF evaluation for RTXDI.
// Requires raytracing_inc.glsl, raytracing_data_inc.glsl, geometry/material buffers, bindless textures, and samplers.

#ifndef RTXDI_LIGHT_SAMPLING_INC_GLSL
#define RTXDI_LIGHT_SAMPLING_INC_GLSL

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_SPOT 2 // Spot light with cone falloff
#define RT_LIGHT_TYPE_AREA 3
#define RT_LIGHT_TYPE_EMISSIVE_TRIANGLE 4
#define RT_LIGHT_TYPE_ENVIRONMENT 5

#define RT_LIGHT_FLAG_CASTS_SHADOW 1u
#define RT_LIGHT_FLAG_TEXTURED 2u

// Reservoir sampling batch size for stochastic light selection.
#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

// ============================================================================
// Light Data (matches C++ RT_LightData, 192 bytes, std430)
// ============================================================================

struct RTLightData {
	vec3 position;
	uint type;
	vec3 direction;
	uint flags;
	vec3 emission;
	float radius;
	vec3 axis_u;
	float inv_area;
	vec3 axis_v;
	float attenuation;
	float range;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float specular_amount;
	uint texture_index;
	uint geometry_index;
	uint primitive_index;
	uint receiver_mask;
	uint caster_mask;
	uint topology_generation;
	uvec2 _pad0;
	vec4 uv_rect;
	float transform[12];
};

struct RTLightBufferParameters {
	uint local_first;
	uint local_count;
	uint infinite_first;
	uint infinite_count;
	uint environment_index;
	uint environment_present;
	uint total_count;
	uint previous_count;
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightData rt_lights[];
};

#ifndef RT_LIGHT_PARAMETERS_BINDING
#define RT_LIGHT_PARAMETERS_BINDING 14
#endif
#ifndef RT_CURRENT_TO_PREVIOUS_BINDING
#define RT_CURRENT_TO_PREVIOUS_BINDING 15
#endif
#ifndef RT_PREVIOUS_TO_CURRENT_BINDING
#define RT_PREVIOUS_TO_CURRENT_BINDING 16
#endif

layout(set = 0, binding = RT_LIGHT_PARAMETERS_BINDING, std140) uniform LightParameters {
	RTLightBufferParameters rt_light_parameters;
};

layout(set = 0, binding = RT_CURRENT_TO_PREVIOUS_BINDING, std430) readonly buffer CurrentToPreviousLightMap {
	uint rt_current_to_previous[];
};

layout(set = 0, binding = RT_PREVIOUS_TO_CURRENT_BINDING, std430) readonly buffer PreviousToCurrentLightMap {
	uint rt_previous_to_current[];
};

int rtxdi_translate_current_to_previous(uint light_index) {
	return light_index < rt_light_parameters.total_count ? int(rt_current_to_previous[light_index]) : -1;
}

int rtxdi_translate_previous_to_current(uint light_index) {
	return light_index < rt_light_parameters.previous_count ? int(rt_previous_to_current[light_index]) : -1;
}

struct RTLightProposal {
	vec3 direction;
	float distance;
	vec3 radiance;
	float solid_angle_pdf;
	vec3 position;
	uint valid;
};

float rtxdi_target_pdf(vec3 contribution) {
	return dot(abs(contribution), vec3(0.2126, 0.7152, 0.0722));
}

uint rtxdi_sample_uniform_light(uint first, uint count, float random, out float selection_pdf) {
	if (count == 0u) {
		selection_pdf = 0.0;
		return 0xFFFFFFFFu;
	}
	selection_pdf = 1.0 / float(count);
	return first + min(uint(random * float(count)), count - 1u);
}

float rtxdi_uniform_light_pdf(uint index, uint first, uint count) {
	return count > 0u && index >= first && index < first + count ? 1.0 / float(count) : 0.0;
}

mat4 rtxdi_decode_light_transform(RTLightData light) {
	return transpose(mat4(
			vec4(light.transform[0], light.transform[1], light.transform[2], light.transform[3]),
			vec4(light.transform[4], light.transform[5], light.transform[6], light.transform[7]),
			vec4(light.transform[8], light.transform[9], light.transform[10], light.transform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

void rtxdi_get_triangle_indices(GeometryData geometry, uint primitive, out uint i0, out uint i1, out uint i2) {
	if (geometry.index_format == 2u) {
		i0 = primitive * 3u;
		i1 = i0 + 1u;
		i2 = i0 + 2u;
		return;
	}
	Uint32Buffer indices = Uint32Buffer(geometry.index_address);
	if (geometry.index_format == 0u) {
		uint byte_offset = primitive * 6u;
		uint word0 = indices.v[byte_offset >> 2u];
		uint word1 = indices.v[(byte_offset >> 2u) + 1u];
		if ((byte_offset & 3u) == 0u) {
			i0 = word0 & 0xFFFFu;
			i1 = word0 >> 16u;
			i2 = word1 & 0xFFFFu;
		} else {
			i0 = word0 >> 16u;
			i1 = word1 & 0xFFFFu;
			i2 = word1 >> 16u;
		}
	} else {
		i0 = indices.v[primitive * 3u];
		i1 = indices.v[primitive * 3u + 1u];
		i2 = indices.v[primitive * 3u + 2u];
	}
}

vec3 rtxdi_fetch_position(GeometryData geometry, uint vertex) {
	if ((geometry.flags & FLAG_COMPRESSED) != 0u) {
		Uint32Buffer positions = Uint32Buffer(geometry.vertex_address);
		uint word0 = positions.v[vertex * 2u];
		uint word1 = positions.v[vertex * 2u + 1u];
		vec3 unorm_position = vec3(word0 & 0xFFFFu, word0 >> 16u, word1 & 0xFFFFu) / 65535.0;
		return geometry.position_offset + unorm_position * geometry.position_scale;
	}
	FloatBuffer positions = FloatBuffer(geometry.vertex_address);
	uint stride = geometry.position_stride >> 2u;
	return vec3(positions.v[vertex * stride], positions.v[vertex * stride + 1u], positions.v[vertex * stride + 2u]);
}

vec2 rtxdi_fetch_uv(GeometryData geometry, uvec3 indices, vec3 barycentrics) {
	if (geometry.uv_byte_offset == OFFSET_NONE || geometry.attribute_address == 0ul) {
		return vec2(0.0);
	}
	uint stride = geometry.attribute_stride >> 2u;
	uint offset = geometry.uv_byte_offset >> 2u;
	vec2 uv[3];
	if ((geometry.flags & FLAG_COMPRESSED) != 0u) {
		Uint32Buffer attributes = Uint32Buffer(geometry.attribute_address);
		for (uint i = 0u; i < 3u; i++) {
			uint packed_uv = attributes.v[indices[i] * stride + offset];
			uv[i] = vec2(packed_uv & 0xFFFFu, packed_uv >> 16u) / 65535.0;
		}
		vec2 scale = unpackHalf2x16(geometry.uv_scale_packed);
		if (scale.x != 0.0 || scale.y != 0.0) {
			uv[0] = (uv[0] - 0.5) * scale;
			uv[1] = (uv[1] - 0.5) * scale;
			uv[2] = (uv[2] - 0.5) * scale;
		}
	} else {
		FloatBuffer attributes = FloatBuffer(geometry.attribute_address);
		for (uint i = 0u; i < 3u; i++) {
			uv[i] = vec2(attributes.v[indices[i] * stride + offset], attributes.v[indices[i] * stride + offset + 1u]);
		}
	}
	return uv[0] * barycentrics.x + uv[1] * barycentrics.y + uv[2] * barycentrics.z;
}

vec3 rtxdi_evaluate_emission_texture(RTLightData light, vec2 uv) {
	if ((light.flags & RT_LIGHT_FLAG_TEXTURED) == 0u) {
		return light.emission;
	}
	vec2 material_uv = uv * light.uv_rect.xy + light.uv_rect.zw;
	return light.emission * texture(sampler2D(bindless_textures[nonuniformEXT(light.texture_index)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), material_uv).rgb;
}

RTLightProposal rtxdi_sample_emissive_triangle(RTLightData light, vec3 receiver_position, vec2 random) {
	RTLightProposal sample = RTLightProposal(vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 0u);
	GeometryData geometry = geometries[light.geometry_index];
	uint i0, i1, i2;
	rtxdi_get_triangle_indices(geometry, light.primitive_index, i0, i1, i2);
	mat4 object_to_world = rtxdi_decode_light_transform(light);
	vec3 p0 = (object_to_world * vec4(rtxdi_fetch_position(geometry, i0), 1.0)).xyz;
	vec3 p1 = (object_to_world * vec4(rtxdi_fetch_position(geometry, i1), 1.0)).xyz;
	vec3 p2 = (object_to_world * vec4(rtxdi_fetch_position(geometry, i2), 1.0)).xyz;
	float root = sqrt(random.x);
	vec3 barycentrics = vec3(1.0 - root, root * (1.0 - random.y), root * random.y);
	sample.position = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
	vec3 to_light = sample.position - receiver_position;
	float distance_squared = dot(to_light, to_light);
	sample.distance = sqrt(distance_squared);
	sample.direction = to_light / max(sample.distance, 1e-10);
	vec3 normal_cross = cross(p1 - p0, p2 - p0);
	float twice_area = length(normal_cross);
	float projected_cosine = abs(dot(normal_cross / max(twice_area, 1e-10), -sample.direction));
	sample.solid_angle_pdf = distance_squared * 2.0 / max(twice_area * projected_cosine, 1e-10);
	vec2 uv = rtxdi_fetch_uv(geometry, uvec3(i0, i1, i2), barycentrics);
	sample.radiance = rtxdi_evaluate_emission_texture(light, uv);
	sample.valid = twice_area > 0.0 && projected_cosine > 0.0 ? 1u : 0u;
	return sample;
}

RTLightProposal rtxdi_sample_area_light(RTLightData light, vec3 receiver_position, vec2 random) {
	RTLightProposal sample = RTLightProposal(vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 0u);
	sample.position = light.position + (random.x - 0.5) * light.axis_u + (random.y - 0.5) * light.axis_v;
	vec3 to_light = sample.position - receiver_position;
	float distance_squared = dot(to_light, to_light);
	sample.distance = sqrt(distance_squared);
	sample.direction = to_light / max(sample.distance, 1e-10);
	float projected_cosine = max(dot(normalize(light.direction), -sample.direction), 0.0);
	sample.solid_angle_pdf = light.inv_area * distance_squared / max(projected_cosine, 1e-10);
	sample.radiance = light.emission;
	if ((light.flags & RT_LIGHT_FLAG_TEXTURED) != 0u) {
		vec2 atlas_uv = light.uv_rect.xy + random * light.uv_rect.zw;
		sample.radiance *= texture(sampler2D(bindless_textures[nonuniformEXT(light.texture_index)], SAMPLER_LINEAR_CLAMP), atlas_uv).rgb;
	}
	sample.valid = light.inv_area > 0.0 && projected_cosine > 0.0 ? 1u : 0u;
	return sample;
}

// ============================================================================
// Unified Cone Sampling (for sphere, directional, spot lights)
// ============================================================================

struct LightSample {
	vec3 cone_axis; // Direction to sample around (normalized).
	float cos_theta_max; // Cone half-angle cosine (1.0 = point, 0.0 = hemisphere).
	vec3 emission; // Light radiance.
	float distance_sq; // Squared distance to light (0 = directional).
	float max_distance; // Max shadow ray distance.
};

// Build orthonormal basis from a single direction.
void lights_build_basis(vec3 dir, out vec3 tangent, out vec3 bitangent) {
	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

// Transform local direction to world space around axis.
vec3 lights_local_to_world(vec3 local_dir, vec3 axis) {
	vec3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

// Prepare unified cone sample from any light type.
LightSample lights_prepare_sample(vec3 hit_pos, RTLightData light) {
	LightSample s;

	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = inversesqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;

	// Cone axis.
	vec3 sphere_axis = to_light * inv_dist;
	vec3 dir_axis = -normalize(light.direction);
	s.cone_axis = mix(sphere_axis, dir_axis, is_directional);

	// Distance (0 for directional = no falloff).
	s.distance_sq = mix(dist_sq, 0.0, is_directional);

	// Cone angle from subtended solid angle.
	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = mix(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	// Max shadow ray distance.
	float sphere_max = dist + light.radius;
	float dir_max = 10000.0;
	s.max_distance = mix(sphere_max, dir_max, is_directional);

	return s;
}

// Sample a direction within the light's cone.
vec3 lights_sample_cone(LightSample ls, vec2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

vec3 rtxdi_evaluate_projector(RTLightData light, vec3 receiver_position) {
	if ((light.flags & RT_LIGHT_FLAG_TEXTURED) == 0u) {
		return vec3(1.0);
	}
	vec3 local_position = (rtxdi_decode_light_transform(light) * vec4(receiver_position, 1.0)).xyz;
	vec2 projector_uv;
	if (light.type == RT_LIGHT_TYPE_OMNI) {
		vec3 local_direction = normalize(local_position);
		vec4 atlas_rect = light.uv_rect;
		if (local_direction.z >= 0.0) {
			atlas_rect.y += atlas_rect.w;
		}
		local_direction.z = 1.0 + abs(local_direction.z);
		projector_uv = (local_direction.xy / local_direction.z * 0.5 + 0.5) * atlas_rect.zw + atlas_rect.xy;
	} else {
		float tangent = sqrt(max(1.0 - light.cos_spot_angle * light.cos_spot_angle, 0.0)) / max(light.cos_spot_angle, 1e-6);
		projector_uv = (local_position.xy / max(-local_position.z * tangent, 1e-6) * 0.5 + 0.5) * light.uv_rect.zw + light.uv_rect.xy;
	}
	vec4 projected = textureLod(sampler2D(bindless_textures[nonuniformEXT(light.texture_index)], SAMPLER_LINEAR_CLAMP), projector_uv, 0.0);
	return projected.rgb * projected.a;
}

RTLightProposal rtxdi_sample_analytic_light(RTLightData light, vec3 receiver_position, vec2 random) {
	if (light.type == RT_LIGHT_TYPE_AREA) {
		return rtxdi_sample_area_light(light, receiver_position, random);
	}
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		return rtxdi_sample_emissive_triangle(light, receiver_position, random);
	}
	RTLightProposal sample = RTLightProposal(vec3(0.0), 0.0, vec3(0.0), 1.0, vec3(0.0), 0u);
	LightSample cone = lights_prepare_sample(receiver_position, light);
	if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		sample.direction = light.radius > 0.0 ? lights_sample_cone(cone, random, sample.solid_angle_pdf) : -normalize(light.direction);
		sample.distance = cone.max_distance;
		sample.radiance = light.emission;
		sample.valid = 1u;
		return sample;
	}
	vec3 to_light = light.position - receiver_position;
	float distance_squared = dot(to_light, to_light);
	float center_distance = sqrt(distance_squared);
	if (light.range > 0.0 && center_distance > light.range) {
		return sample;
	}
	if (light.radius > 0.0) {
		sample.direction = lights_sample_cone(cone, random, sample.solid_angle_pdf);
		float center_t = dot(to_light, sample.direction);
		vec3 perpendicular = to_light - center_t * sample.direction;
		sample.distance = max(center_t - sqrt(max(light.radius * light.radius - dot(perpendicular, perpendicular), 0.0)), 0.0);
	} else {
		sample.direction = to_light / max(center_distance, 1e-10);
		sample.distance = center_distance;
	}
	sample.position = receiver_position + sample.direction * sample.distance;
	float attenuation = min(pow(max(center_distance, 0.0001), -light.attenuation), 1.0);
	if (light.range > 0.0) {
		float window = center_distance / light.range;
		window *= window;
		window *= window;
		window = max(1.0 - window, 0.0);
		attenuation *= window * window;
	}
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		float spot_cosine = dot(-sample.direction, light.direction);
		if (spot_cosine <= light.cos_spot_angle) {
			return sample;
		}
		float rim = max(1e-4, (1.0 - spot_cosine) / (1.0 - light.cos_spot_angle));
		attenuation *= 1.0 - pow(rim, light.inv_spot_attenuation);
	}
	sample.radiance = light.emission * attenuation * rtxdi_evaluate_projector(light, receiver_position);
	sample.valid = 1u;
	return sample;
}

RTLightProposal rtxdi_sample_light(uint light_index, vec3 receiver_position, vec2 random) {
	return rtxdi_sample_analytic_light(rt_lights[light_index], receiver_position, random);
}

float rtxdi_evaluate_light_solid_angle_pdf(RTLightData light, vec3 receiver_position, vec3 sampled_position, vec3 sampled_direction) {
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
		GeometryData geometry = geometries[light.geometry_index];
		uint i0, i1, i2;
		rtxdi_get_triangle_indices(geometry, light.primitive_index, i0, i1, i2);
		mat4 object_to_world = rtxdi_decode_light_transform(light);
		vec3 p0 = (object_to_world * vec4(rtxdi_fetch_position(geometry, i0), 1.0)).xyz;
		vec3 p1 = (object_to_world * vec4(rtxdi_fetch_position(geometry, i1), 1.0)).xyz;
		vec3 p2 = (object_to_world * vec4(rtxdi_fetch_position(geometry, i2), 1.0)).xyz;
		vec3 normal_cross = cross(p1 - p0, p2 - p0);
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

RTLightProposal rtxdi_sample_environment(vec2 random, vec3 radiance) {
	RTLightProposal sample = RTLightProposal(vec3(0.0), 1e10, radiance, 1.0 / (4.0 * PI), vec3(0.0), 1u);
	float z = 1.0 - 2.0 * random.x;
	float radius = sqrt(max(1.0 - z * z, 0.0));
	float phi = 2.0 * PI * random.y;
	sample.direction = vec3(radius * cos(phi), radius * sin(phi), z);
	return sample;
}

#endif
