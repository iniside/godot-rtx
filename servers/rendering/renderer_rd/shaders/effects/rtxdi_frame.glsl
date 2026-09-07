#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define MAX_VIEWS 2
#include "../scene_data_inc.glsl"
#include "../oct_inc.glsl"
#include "../raytracing/rtxdi_nrd_inc.glsl"

layout(set = 0, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData previous;
} scene;
layout(set = 0, binding = 1) uniform sampler2D surface_base;
layout(set = 0, binding = 2) uniform sampler2D surface_shading;
layout(set = 0, binding = 3) uniform sampler2D surface_emission;
layout(set = 0, binding = 4) uniform sampler2D surface_motion;
layout(set = 0, binding = 6) uniform usampler2D surface_classification;
layout(set = 0, binding = 7) uniform sampler2D surface_depth;

layout(push_constant, std430) uniform Params {
	uvec2 size;
	uint orthogonal;
	uint fog_enabled;
	float fog_inverse_length;
	float fog_spread;
	uint fog_legacy_blending;
	uint separate_specular;
	float environment_energy;
	uint pad0;
	uint pad1;
	uint pad2;
} params;

mat4 decode_inverse_view(SceneData data) {
	mat4 inverse_view = transpose(mat4(data.inv_view_matrix[0], data.inv_view_matrix[1], data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
#ifdef USE_DOUBLE_PRECISION
	inverse_view[3].xyz = -(inverse_view[3].xyz + data.inv_view_precision.xyz);
#endif
	return inverse_view;
}

#ifdef MODE_PREPARE
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D nrd_normal_roughness;
layout(r32f, set = 0, binding = 9) uniform restrict writeonly image2D nrd_view_depth;
layout(rg16f, set = 0, binding = 10) uniform restrict writeonly image2D velocity;
layout(rgba8, set = 0, binding = 11) uniform restrict writeonly image2D normal_roughness;
#else
layout(set = 0, binding = 8) uniform sampler2D diffuse_radiance;
layout(set = 0, binding = 9) uniform sampler2D specular_radiance;
layout(rgba16f, set = 0, binding = 10) uniform restrict writeonly image2D color;
layout(set = 0, binding = 11) uniform sampler3D volumetric_fog;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 12) uniform sampler2DArray radiance;
#else
layout(set = 0, binding = 12) uniform sampler2D radiance;
#endif
#include "../light_data_inc.glsl"
layout(set = 0, binding = 13, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
} directional_lights;
layout(rgba16f, set = 0, binding = 14) uniform restrict writeonly image2D separate_specular;

vec3 fog_get_directional_color(uint index) {
	return directional_lights.data[index].color * directional_lights.data[index].energy;
}

vec3 fog_get_directional_direction(uint index) {
	return directional_lights.data[index].direction;
}

vec3 fog_sample_radiance(vec3 view_direction, float mip_level) {
	vec3 sky_direction = scene.data.radiance_inverse_xform * view_direction;
	vec2 uv = vec3_to_oct_with_border(normalize(sky_direction), vec2(scene.data.radiance_border_size, 1.0 - 2.0 * scene.data.radiance_border_size));
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float layer = clamp(mip_level * MAX_ROUGHNESS_LOD, 0.0, MAX_ROUGHNESS_LOD);
	return mix(textureLod(radiance, vec3(uv, floor(layer)), 0.0).rgb, textureLod(radiance, vec3(uv, ceil(layer)), 0.0).rgb, fract(layer)) * params.environment_energy * scene.data.emissive_exposure_normalization;
#else
	return textureLod(radiance, uv, mip_level * MAX_ROUGHNESS_LOD).rgb * params.environment_energy * scene.data.emissive_exposure_normalization;
#endif
}

#define FOG_HAS_RADIANCE
#define RT
#define rt_decode_inv_view_matrix decode_inverse_view
#include "../fog_inc.glsl"
#endif

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(uvec2(pixel), params.size))) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.size);
	float depth = texelFetch(surface_depth, pixel, 0).x;
	uint flags = texelFetch(surface_classification, pixel, 0).x;
	bool surface_valid = (flags & 1u) != 0u && depth > 0.0;
	vec4 shading = texelFetch(surface_shading, pixel, 0);
	vec3 normal = surface_valid ? oct_to_vec3(shading.xy * 2.0 - 1.0) : vec3(0.0, 0.0, 1.0);
	vec4 view_position = scene.data.inv_projection_matrix * vec4(uv * 2.0 - 1.0, depth, 1.0);
	view_position /= view_position.w;

#ifdef MODE_PREPARE
	vec3 encoded_normal = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	float encoded_roughness = max(shading.z, 1.5 / 512.0);
	vec3 packed_normal;
	packed_normal.y = encoded_normal.y * 0.5 + 0.5;
	packed_normal.x = encoded_normal.x * 0.5 + packed_normal.y;
	packed_normal.y -= encoded_normal.x * 0.5;
	packed_normal.z = (encoded_normal.z < 0.0 ? -encoded_roughness : encoded_roughness) * 0.5 + 0.5;
	imageStore(nrd_normal_roughness, pixel, vec4(packed_normal, 0.0));
	imageStore(nrd_view_depth, pixel, vec4(surface_valid && (flags & 2u) == 0u ? -view_position.z : 1e6));
	vec2 motion;
	if (surface_valid) {
		motion = texelFetch(surface_motion, pixel, 0).xy / vec2(params.size);
	} else {
		vec4 view_direction = scene.data.inv_projection_matrix * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
		vec3 world_direction = mat3(decode_inverse_view(scene.data)) * normalize(view_direction.xyz);
		vec3 previous_view = transpose(mat3(decode_inverse_view(scene.previous))) * world_direction;
		vec4 previous_clip = scene.previous.projection_matrix * vec4(previous_view, 0.0);
		motion = abs(previous_clip.w) > 1e-6 ? ((previous_clip.xy / previous_clip.w - scene.previous.taa_jitter) - (uv * 2.0 - 1.0 - scene.data.taa_jitter)) * 0.5 : vec2(0.0);
	}
	imageStore(velocity, pixel, vec4(motion, 0.0, 0.0));
	vec3 view_normal = transpose(mat3(decode_inverse_view(scene.data))) * normal;
	imageStore(normal_roughness, pixel, vec4(view_normal * 0.5 + 0.5, shading.z * (127.0 / 255.0)));
#else
	if (!surface_valid) {
		if (params.separate_specular != 0u) {
			imageStore(separate_specular, pixel, vec4(0.0));
		}
		return;
	}
	vec4 emission = texelFetch(surface_emission, pixel, 0);
	vec3 output_color = emission.rgb;
	vec3 specular_color = vec3(0.0);
	if ((flags & 2u) == 0u) {
		vec3 base = texelFetch(surface_base, pixel, 0).rgb;
		mat4 inverse_view = decode_inverse_view(scene.data);
		vec3 view_direction = params.orthogonal != 0u ? normalize(inverse_view[2].xyz) : normalize(-(mat3(inverse_view) * view_position.xyz));
		vec3 diffuse_factor;
		vec3 specular_factor;
		rtxdi_nrd_material_factors(normal, view_direction, base * (1.0 - emission.a), mix(vec3(0.08 * shading.w), base, emission.a), shading.z, diffuse_factor, specular_factor);
		output_color += texelFetch(diffuse_radiance, pixel, 0).rgb * diffuse_factor * scene.data.emissive_exposure_normalization;
		specular_color = texelFetch(specular_radiance, pixel, 0).rgb * specular_factor * scene.data.emissive_exposure_normalization;
	}
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
	bool fixed_fog = (scene.data.flags & SCENE_DATA_FLAGS_USE_FOG) != 0u;
	if (fixed_fog) {
		fog = fog_process(scene.data, view_position.xyz);
		fog.rgb *= fog.a;
		fog.a = 1.0 - fog.a;
	}
	if (params.fog_enabled != 0u) {
		float fog_z = -view_position.z * params.fog_inverse_length;
		fog_z = fog_z < 1.0 ? pow(max(fog_z, 0.0), params.fog_spread) : fog_z;
		vec4 volume = textureLod(volumetric_fog, vec3(uv, fog_z), 0.0);
		if (fixed_fog) {
			fog.rgb = params.fog_legacy_blending != 0u ? mix(volume.rgb, fog.rgb, volume.a) : fog.rgb * volume.a + volume.rgb;
			fog.a *= volume.a;
		} else {
			fog = volume;
			if (params.fog_legacy_blending != 0u) {
				fog.rgb *= 1.0 - fog.a;
			}
		}
	}
	output_color = output_color * fog.a + fog.rgb;
	specular_color *= fog.a;
	specular_color = any(isnan(specular_color)) || any(isinf(specular_color)) ? vec3(0.0) : clamp(specular_color, 0.0, 65504.0);
	if (params.separate_specular != 0u) {
		imageStore(separate_specular, pixel, vec4(clamp(specular_color, 0.0, 65504.0), emission.a));
	} else {
		output_color += specular_color;
	}
	output_color = any(isnan(output_color)) || any(isinf(output_color)) ? vec3(0.0) : clamp(output_color, 0.0, 65504.0);
	imageStore(color, pixel, vec4(output_color, 1.0));
#endif
}
