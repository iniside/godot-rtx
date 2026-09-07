#ifndef RTXDI_APPLICATION_BRIDGE_INC_GLSL
#define RTXDI_APPLICATION_BRIDGE_INC_GLSL

struct RAB_Material {
	vec3 base_color;
	float roughness;
	float metallic;
	float specular;
	uint material_flags;
	uint material_id;
};

struct RAB_Surface {
	vec3 world_pos;
	float linear_depth;
	vec3 normal;
	float _pad0;
	vec3 geometry_normal;
	float _pad1;
	vec3 view_dir;
	float _pad2;
	vec3 motion;
	uint layer_mask;
	RAB_Material material;
	bool valid;
};

#define RAB_LightInfo RTLightData

struct RAB_LightSample {
	vec3 direction;
	float distance;
	vec3 radiance;
	float solid_angle_pdf;
	vec3 position;
	uint light_type;
	uint flags;
	uint receiver_mask;
	uint caster_mask;
	uint geometry_index;
	uint primitive_index;
	uint valid;
	float specular_amount;
};

RAB_Material RAB_EmptyMaterial() {
	return RAB_Material(vec3(0.0), 1.0, 0.0, 0.5, 0u, 0u);
}

RAB_Surface RAB_EmptySurface() {
	return RAB_Surface(vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 0u, RAB_EmptyMaterial(), false);
}

RAB_LightInfo RAB_EmptyLightInfo() {
	RAB_LightInfo light;
	light.type = 0u;
	light.flags = 0u;
	light.emission = vec3(0.0);
	light.receiver_mask = 0u;
	light.caster_mask = 0u;
	return light;
}

RAB_LightSample RAB_EmptyLightSample() {
	return RAB_LightSample(vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1.0);
}

mat4 rtxdi_decode_inv_view(SceneData scene_data) {
	mat4 inv_view = transpose(mat4(scene_data.inv_view_matrix[0], scene_data.inv_view_matrix[1], scene_data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
#ifdef USE_DOUBLE_PRECISION
	inv_view[3].xyz = -(inv_view[3].xyz + scene_data.inv_view_precision.xyz);
#endif
	return inv_view;
}

vec3 rtxdi_reconstruct_world_position(ivec2 pixel, float device_depth, SceneData scene_data) {
	vec2 ndc = (vec2(pixel) + vec2(0.5)) * scene_data.screen_pixel_size * 2.0 - 1.0;
	vec4 view = scene_data.inv_projection_matrix * vec4(ndc, device_depth, 1.0);
	view /= view.w;
	return (rtxdi_decode_inv_view(scene_data) * vec4(view.xyz, 1.0)).xyz;
}

RAB_Surface rtxdi_load_surface(ivec2 pixel, bool previous_frame) {
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(rtxdi_params.extent_history.xy)))) {
		return RAB_EmptySurface();
	}
	if (previous_frame && rtxdi_params.extent_history.z == 0u) {
		return RAB_EmptySurface();
	}
	vec4 base = previous_frame ? texelFetch(sampler2D(previous_base, surface_sampler), pixel, 0) : texelFetch(sampler2D(current_base, surface_sampler), pixel, 0);
	vec4 shading = previous_frame ? texelFetch(sampler2D(previous_shading, surface_sampler), pixel, 0) : texelFetch(sampler2D(current_shading, surface_sampler), pixel, 0);
	vec4 emission_metallic = previous_frame ? texelFetch(sampler2D(previous_emission, surface_sampler), pixel, 0) : texelFetch(sampler2D(current_emission, surface_sampler), pixel, 0);
	vec4 motion = previous_frame ? texelFetch(sampler2D(previous_motion, surface_sampler), pixel, 0) : texelFetch(sampler2D(current_motion, surface_sampler), pixel, 0);
	uint geometry = previous_frame ? texelFetch(usampler2D(previous_geometry, surface_sampler), pixel, 0).x : texelFetch(usampler2D(current_geometry, surface_sampler), pixel, 0).x;
	uvec2 classification = previous_frame ? texelFetch(usampler2D(previous_classification, surface_sampler), pixel, 0).xy : texelFetch(usampler2D(current_classification, surface_sampler), pixel, 0).xy;
	float depth = previous_frame ? texelFetch(sampler2D(previous_depth, surface_sampler), pixel, 0).x : texelFetch(sampler2D(current_depth, surface_sampler), pixel, 0).x;
	if ((classification.x & 1u) == 0u || (classification.x & 2u) != 0u || depth <= 0.0) {
		return RAB_EmptySurface();
	}
	SceneData scene_data = previous_frame ? scene_data_block.prev_data : scene_data_block.data;
	vec3 world_pos = rtxdi_reconstruct_world_position(pixel, depth, scene_data);
	RAB_Material material = RAB_Material(base.rgb, shading.z, emission_metallic.a, shading.w, classification.x, 0u);
	RAB_Surface surface;
	surface.world_pos = world_pos;
	vec4 view_position = scene_data.inv_projection_matrix * vec4((vec2(pixel) + 0.5) * scene_data.screen_pixel_size * 2.0 - 1.0, depth, 1.0);
	surface.linear_depth = -view_position.z / view_position.w;
	surface.normal = oct_to_vec3(shading.xy * 2.0 - 1.0);
	surface.geometry_normal = oct_to_vec3(unpackUnorm2x16(geometry) * 2.0 - 1.0);
	mat4 inv_view = rtxdi_decode_inv_view(scene_data);
	surface.view_dir = rtxdi_params.extent_history.w != 0u ? normalize(inv_view[2].xyz) : normalize(inv_view[3].xyz - world_pos);
	surface.motion = motion.xyz;
	surface.layer_mask = classification.y;
	surface.material = material;
	surface.valid = true;
	return surface;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixel_position, bool previous_frame) {
	return rtxdi_load_surface(pixel_position, previous_frame);
}

bool RAB_IsSurfaceValid(RAB_Surface surface) {
	return surface.valid;
}

float RAB_GetSurfaceLinearDepth(RAB_Surface surface) {
	return surface.linear_depth;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) {
	return surface.world_pos;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface) {
	return surface.normal;
}

RAB_Material RAB_GetMaterial(RAB_Surface surface) {
	return surface.material;
}

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b) {
	return a.material_flags == b.material_flags && distance(a.base_color, b.base_color) <= 0.1 && abs(a.roughness - b.roughness) <= 0.1 && abs(a.metallic - b.metallic) <= 0.1;
}

int2 RAB_ClampSamplePositionIntoView(int2 pixel_position, bool previous_frame) {
	return clamp(pixel_position, int2(0), int2(rtxdi_params.extent_history.xy) - int2(1));
}

RAB_LightInfo RAB_LoadLightInfo(uint light_index, bool previous_frame) {
	uint count = previous_frame ? rt_light_params.previous_count : rt_light_params.total_count;
	if (light_index >= count) {
		return RAB_EmptyLightInfo();
	}
	RAB_LightInfo light = previous_frame ? previous_lights[light_index] : current_lights[light_index];
	if (previous_frame && light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		uint current_index = previous_to_current_light[light_index];
		if (current_index == 0xFFFFFFFFu) {
			return RAB_EmptyLightInfo();
		}
		light.geometry_index = current_lights[current_index].geometry_index;
		light._pad0.x = 1u;
	}
	return light;
}

int RAB_TranslateLightIndex(uint light_index, bool current_to_previous) {
	if (current_to_previous) {
		if (light_index >= rt_light_params.total_count) {
			return -1;
		}
		uint mapped = current_to_previous_light[light_index];
		return mapped == 0xFFFFFFFFu ? -1 : int(mapped);
	}
	if (light_index >= rt_light_params.previous_count) {
		return -1;
	}
	uint mapped = previous_to_current_light[light_index];
	return mapped == 0xFFFFFFFFu ? -1 : int(mapped);
}

vec3 rtxdi_sample_environment_radiance(vec3 world_direction) {
	vec3 view_direction = mat3(scene_data_block.data.inv_view_matrix) * world_direction;
	vec3 sky_direction = scene_data_block.data.radiance_inverse_xform * view_direction;
	vec2 border = vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 uv = vec3_to_oct_with_border(sky_direction, border);
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	return textureLod(sampler2DArray(radiance_octmap, environment_sampler), vec3(uv, 0.0), 0.0).rgb;
#else
	return textureLod(sampler2D(radiance_octmap, environment_sampler), uv, 0.0).rgb;
#endif
}

float rtxdi_sample_coverage(MaterialData material, vec2 uv, vec2 uv_dx, vec2 uv_dy) {
	switch (material.coverage_sampler) {
		case 0u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_NEAREST_CLAMP), uv, uv_dx, uv_dy).a;
		case 1u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_LINEAR_CLAMP), uv, uv_dx, uv_dy).a;
		case 2u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP), uv, uv_dx, uv_dy).a;
		case 3u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, uv_dx, uv_dy).a;
		case 4u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP), uv, uv_dx, uv_dy).a;
		case 5u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), uv, uv_dx, uv_dy).a;
		case 6u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_NEAREST_REPEAT), uv, uv_dx, uv_dy).a;
		case 7u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_LINEAR_REPEAT), uv, uv_dx, uv_dy).a;
		case 8u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT), uv, uv_dx, uv_dy).a;
		case 9u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, uv_dx, uv_dy).a;
		case 10u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT), uv, uv_dx, uv_dy).a;
		case 11u:
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(material.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT), uv, uv_dx, uv_dy).a;
	}
	return 0.0;
}

bool rtxdi_alpha_covered(uint geometry_index, uint primitive_id, int cluster_id, vec2 barycentrics, mat4x3 object_to_world, bool shadow_ray);

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 random) {
	if (light.receiver_mask == 0u) {
		return RAB_EmptyLightSample();
	}
	RTLightProposal proposal;
	if (light.type == RT_LIGHT_TYPE_ENVIRONMENT) {
		proposal = rtxdi_sample_environment(random, vec3(1.0));
		proposal.radiance = rtxdi_sample_environment_radiance(proposal.direction) * light.emission;
	} else {
		proposal = rtxdi_sample_analytic_light(light, surface.world_pos, random);
	}
	if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		float root = sqrt(random.x);
		vec2 barycentrics = vec2(root * (1.0 - random.y), root * random.y);
		if (!rtxdi_alpha_covered(light.geometry_index, light.primitive_index, -1, barycentrics, mat4x3(rtxdi_decode_light_transform(light)), false)) {
			proposal.radiance = vec3(0.0);
		}
	}
	RAB_LightSample light_sample;
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

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample light_sample, out float3 direction, out float distance) {
	direction = light_sample.direction;
	distance = light_sample.distance;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample light_sample) {
	return light_sample.solid_angle_pdf;
}

bool RAB_IsAnalyticLightSample(RAB_LightSample light_sample) {
	return light_sample.light_type != RT_LIGHT_TYPE_EMISSIVE_TRIANGLE && light_sample.light_type != RT_LIGHT_TYPE_ENVIRONMENT;
}

MaterialProperties rtxdi_material_properties(RAB_Surface surface) {
	MaterialProperties material;
	material.baseColor = surface.material.base_color;
	material.metalness = surface.material.metallic;
	material.emissive = vec3(0.0);
	material.roughness = max(surface.material.roughness, 0.02);
	material.dielectricF0 = 0.08 * surface.material.specular;
	material.transmissivness = 0.0;
	material.opacity = 1.0;
	return material;
}

void rtxdi_evaluate_brdf(RAB_Surface surface, vec3 direction, out vec3 diffuse, out vec3 specular) {
	evalCombinedBRDFSeparate(surface.normal, direction, surface.view_dir, rtxdi_material_properties(surface), diffuse, specular);
}

float rtxdi_light_sample_measure(RAB_LightSample light_sample) {
	return light_sample.light_type == RT_LIGHT_TYPE_AREA || light_sample.light_type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE || light_sample.light_type == RT_LIGHT_TYPE_ENVIRONMENT ? max(light_sample.solid_angle_pdf, 1e-20) : 1.0;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample light_sample, RAB_Surface surface) {
	if (light_sample.valid == 0u || (light_sample.receiver_mask & surface.layer_mask) == 0u || dot(surface.geometry_normal, light_sample.direction) <= 0.0 || dot(surface.normal, surface.view_dir) <= 0.0) {
		return 0.0;
	}
	vec3 diffuse;
	vec3 specular;
	rtxdi_evaluate_brdf(surface, light_sample.direction, diffuse, specular);
	return rtxdi_target_pdf((diffuse + specular * light_sample.specular_amount) * light_sample.radiance / rtxdi_light_sample_measure(light_sample));
}

float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 direction) {
	if (dot(surface.geometry_normal, direction) <= 0.0 || dot(surface.normal, direction) <= 0.0 || dot(surface.normal, surface.view_dir) <= 0.0) {
		return 0.0;
	}
	BrdfData data = prepareBRDFData(surface.normal, direction, surface.view_dir, rtxdi_material_properties(surface));
	return 0.5 * (diffusePdf(data.NdotL) + specularPdf(data.alpha, data.alphaSquared, data.NdotH, data.NdotV, data.LdotH));
}

bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction) {
	int type = RTXDI_GetNextRandom(rng) < 0.5 ? DIFFUSE_TYPE : SPECULAR_TYPE;
	vec3 weight;
	return evalIndirectCombinedBRDF(vec2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng)), surface.normal, surface.geometry_normal, surface.view_dir, rtxdi_material_properties(surface), type, direction, weight, vec4(0.0));
}

float RAB_EvaluateLocalLightSourcePdf(uint light_index) {
	return rtxdi_uniform_light_pdf(light_index, rt_light_params.local_first, rt_light_params.local_count);
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction) {
	float phi = atan(direction.y, direction.x);
	return vec2(clamp((1.0 - direction.z) * 0.5, 0.0, 1.0), fract(phi / (2.0 * PI)));
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction) {
	return 1.0;
}

uint rtxdi_resolve_primitive(GeometryData geometry, int cluster_id, uint primitive_id) {
	if ((geometry.flags & FLAG_CLUSTERED) == 0u || cluster_id < 0) {
		return primitive_id;
	}
	uint64_t address = (uint64_t(geometry.cluster_remap_address_hi) << 32u) | uint64_t(geometry.cluster_remap_address_lo);
	return Uint32Buffer(address).v[uint(cluster_id)] + primitive_id;
}

bool rtxdi_alpha_covered(uint geometry_index, uint primitive_id, int cluster_id, vec2 barycentrics, mat4x3 object_to_world, bool shadow_ray) {
	GeometryData geometry = geometries[geometry_index];
	if (shadow_ray && ((geometry.flags & FLAG_CASTS_SHADOWS) == 0u || (geometry.instance_layer_mask & rtxdi_visibility_caster_mask) == 0u)) {
		return false;
	}
	if (!shadow_ray && (geometry.flags & FLAG_SHADOWS_ONLY) != 0u) {
		return false;
	}
	MaterialData material = materials[geometry_index];
	if ((material.coverage_flags & 3u) == 0u) {
		return true;
	}
	uint primitive = rtxdi_resolve_primitive(geometry, cluster_id, primitive_id);
	uint i0;
	uint i1;
	uint i2;
	rtxdi_get_triangle_indices(geometry, primitive, i0, i1, i2);
	vec3 bary = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
	vec3 p0 = rtxdi_fetch_position(geometry, i0);
	vec3 p1 = rtxdi_fetch_position(geometry, i1);
	vec3 p2 = rtxdi_fetch_position(geometry, i2);
	vec3 object_position = p0 * bary.x + p1 * bary.y + p2 * bary.z;
	vec3 normal = cross(p1 - p0, p2 - p0);
	mat4 object_to_clip = scene_data_block.data.projection_matrix * inverse(rtxdi_decode_inv_view(scene_data_block.data)) * mat4(object_to_world);
	mat4 clip_to_object = inverse(object_to_clip);
	vec4 clip = object_to_clip * vec4(object_position, 1.0);
	vec4 clip_plane = transpose(clip_to_object) * vec4(normal, -dot(normal, p0));
	float plane_denominator = (clip_plane.z < 0.0 ? -1.0 : 1.0) * max(abs(clip_plane.z), 1e-20);
	vec3 derivatives[2];
	for (uint axis = 0u; axis < 2u; axis++) {
		vec4 homogeneous_derivative = clip_to_object[axis] - clip_to_object[2] * (clip_plane[axis] / plane_denominator);
		derivatives[axis] = (homogeneous_derivative.xyz - object_position * homogeneous_derivative.w) * clip.w * 2.0 * scene_data_block.data.screen_pixel_size[axis];
	}
	vec2 uv0 = rtxdi_fetch_uv(geometry, uvec3(i0, i1, i2), vec3(1.0, 0.0, 0.0));
	vec2 uv1 = rtxdi_fetch_uv(geometry, uvec3(i0, i1, i2), vec3(0.0, 1.0, 0.0));
	vec2 uv2 = rtxdi_fetch_uv(geometry, uvec3(i0, i1, i2), vec3(0.0, 0.0, 1.0));
	vec3 barycentric_u = cross(p2 - p0, normal) / max(dot(normal, normal), 1e-20);
	vec3 barycentric_v = cross(normal, p1 - p0) / max(dot(normal, normal), 1e-20);
	vec2 uv_derivatives[2];
	for (uint axis = 0u; axis < 2u; axis++) {
		uv_derivatives[axis] = ((uv1 - uv0) * dot(barycentric_u, derivatives[axis]) + (uv2 - uv0) * dot(barycentric_v, derivatives[axis])) * material.uv1_scale;
	}
	vec2 uv = (uv0 * bary.x + uv1 * bary.y + uv2 * bary.z) * material.uv1_scale + material.uv1_offset;
	float alpha = rtxdi_sample_coverage(material, uv, uv_derivatives[0], uv_derivatives[1]) * material.albedo_color.a;
	if ((material.coverage_flags & 4u) != 0u && geometry.color_byte_offset != OFFSET_NONE) {
		Uint32Buffer attributes = Uint32Buffer(geometry.attribute_address);
		uvec3 offsets = (uvec3(i0, i1, i2) * geometry.attribute_stride + geometry.color_byte_offset) >> 2u;
		alpha *= dot(bary, vec3(unpackUnorm4x8(attributes.v[offsets.x]).a, unpackUnorm4x8(attributes.v[offsets.y]).a, unpackUnorm4x8(attributes.v[offsets.z]).a));
	}
	if ((material.coverage_flags & 1u) != 0u && alpha < material.alpha_scissor_threshold) {
		return false;
	}
	if ((material.coverage_flags & 2u) != 0u) {
		if (alpha < compute_alpha_hash_threshold(object_position, max(material.alpha_hash_scale, 1e-6), derivatives[0], derivatives[1])) {
			return false;
		}
	}
	return true;
}

bool rtxdi_trace_visibility(RAB_Surface surface, RAB_LightSample light_sample) {
	if ((light_sample.flags & RT_LIGHT_FLAG_CASTS_SHADOW) == 0u) {
		return true;
	}
	rtxdi_visibility_caster_mask = light_sample.caster_mask;
	rayQueryEXT query;
	float max_distance = light_sample.light_type == RT_LIGHT_TYPE_ENVIRONMENT || light_sample.light_type == RT_LIGHT_TYPE_DIRECTIONAL ? 3.402823466e+38 : max(light_sample.distance - 0.002, 0.001);
	rayQueryInitializeEXT(query, scene_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsNoOpaqueEXT, 0xFF, surface.world_pos, 0.001, light_sample.direction, max_distance);
	while (rayQueryProceedEXT(query)) {
		if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			uint geometry_index = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
			bool shadow_facing = (geometries[geometry_index].flags & FLAG_SHADOW_CULL_ENABLED) == 0u || !rayQueryGetIntersectionFrontFaceEXT(query, false);
			if (shadow_facing && rtxdi_alpha_covered(geometry_index, rayQueryGetIntersectionPrimitiveIndexEXT(query, false), rayQueryGetIntersectionClusterIdNV(query, false), rayQueryGetIntersectionBarycentricsEXT(query, false), rayQueryGetIntersectionObjectToWorldEXT(query, false), true)) {
				rayQueryConfirmIntersectionEXT(query);
			}
		}
	}
	return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample light_sample) {
	return rtxdi_trace_visibility(surface, light_sample);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface current_surface, RAB_Surface previous_surface, RAB_LightSample light_sample) {
	return rtxdi_trace_visibility(previous_surface, light_sample);
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float min_t, float max_t, out uint light_index, out float2 random) {
	light_index = RTXDI_InvalidLightIndex;
	random = vec2(0.0);
	rtxdi_visibility_caster_mask = 0xFFFFFFFFu;
	rayQueryEXT query;
	rayQueryInitializeEXT(query, scene_tlas, gl_RayFlagsNoOpaqueEXT, 0xFF, origin, min_t, direction, max_t);
	while (rayQueryProceedEXT(query)) {
		if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT && rtxdi_alpha_covered(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false), rayQueryGetIntersectionPrimitiveIndexEXT(query, false), rayQueryGetIntersectionClusterIdNV(query, false), rayQueryGetIntersectionBarycentricsEXT(query, false), rayQueryGetIntersectionObjectToWorldEXT(query, false), false)) {
			rayQueryConfirmIntersectionEXT(query);
		}
	}
	if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		return false;
	}
	uint geometry_index = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
	GeometryData geometry = geometries[geometry_index];
	uint primitive_index = rtxdi_resolve_primitive(geometry, rayQueryGetIntersectionClusterIdNV(query, true), rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
	for (uint i = 0u; i < rt_light_params.local_count; i++) {
		uint candidate_index = rt_light_params.local_first + i;
		RTLightData light = current_lights[candidate_index];
		if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE && light.geometry_index == geometry_index && light.primitive_index == primitive_index) {
			light_index = candidate_index;
			vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
			float root = max(barycentrics.x + barycentrics.y, 1e-6);
			random = vec2(root * root, barycentrics.y / root);
			break;
		}
	}
	return true;
}

#endif
