#[compute]

#VERSION_DEFINES

#define MAX_VIEWS 2
#include "../scene_data_inc.slang"
#include "../surface_data_inc.slang"
#include "../rtxdi_resolution_inc.slang"
#include "../raytracing/rtxdi_nrd_inc.slang"

struct SceneDataBlock {
	SceneData data;
	SceneData previous;
};
[[vk::binding(0, 0)]] ConstantBuffer<SceneDataBlock> scene;
[[vk::binding(1, 0)]] Texture2D<float4> surface_base;
[[vk::binding(2, 0)]] Texture2D<float4> surface_shading;
[[vk::binding(3, 0)]] Texture2D<float4> surface_emission;
[[vk::binding(4, 0)]] Texture2D<float4> surface_motion;
[[vk::binding(5, 0)]] Texture2D<uint> surface_geometry;
[[vk::binding(6, 0)]] Texture2D<uint2> surface_classification;
[[vk::binding(7, 0)]] Texture2D<float> surface_depth;

struct Params {
	uint2 size;
	uint orthogonal;
	uint fog_enabled;
	float fog_inverse_length;
	float fog_spread;
	uint fog_legacy_blending;
	uint separate_specular;
	float environment_energy;
	uint ddgi_enabled;
	uint camera_radiance_enabled;
	uint camera_radiance_full;
	uint reflection_hit_distance_enabled;
	uint pad;
	uint2 lighting_size;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

float4x4 decode_inverse_view(SceneData data) {
	float4x4 inverse_view = float4x4(data.inv_view_matrix[0], data.inv_view_matrix[1], data.inv_view_matrix[2], float4(0.0, 0.0, 0.0, 1.0));
#ifdef USE_DOUBLE_PRECISION
	inverse_view[0][3] = -(inverse_view[0][3] + data.inv_view_precision.x);
	inverse_view[1][3] = -(inverse_view[1][3] + data.inv_view_precision.y);
	inverse_view[2][3] = -(inverse_view[2][3] + data.inv_view_precision.z);
#endif
	return inverse_view;
}

#ifdef MODE_PREPARE
[[vk::binding(8, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> nrd_normal_roughness;
[[vk::binding(9, 0), vk::image_format("r32f")]] RWTexture2D<float> nrd_view_depth;
[[vk::binding(10, 0), vk::image_format("rg16f")]] RWTexture2D<float2> velocity;
[[vk::binding(11, 0), vk::image_format("rgba8")]] RWTexture2D<float4> normal_roughness;
[[vk::binding(12, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> rr_diffuse_albedo;
[[vk::binding(13, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> rr_specular_albedo;
[[vk::binding(14, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> rr_normal_roughness;
[[vk::binding(15, 0), vk::image_format("r16f")]] RWTexture2D<float> rr_specular_hit_distance;
[[vk::binding(16, 0)]] Texture2D<float> reflection_hit_distance;
[[vk::binding(17, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> nrd_motion;
#else
[[vk::binding(8, 0)]] Texture2D<float4> diffuse_radiance;
[[vk::binding(9, 0)]] Texture2D<float4> specular_radiance;
[[vk::binding(10, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> color;
[[vk::binding(11, 0)]] Texture3D<float4> volumetric_fog;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
[[vk::binding(12, 0)]] Texture2DArray<float4> radiance;
#else
[[vk::binding(12, 0)]] Texture2D<float4> radiance;
#endif

struct DirectionalLightData {
	float3 direction;
	float energy;
	float3 color;
	float size;
	float specular;
	uint mask;
	float softshadow_angle;
	float soft_shadow_scale;
	uint blend_splits;
	float shadow_opacity;
	float fade_from;
	float fade_to;
	uint2 pad;
	uint bake_mode;
	float volumetric_fog_energy;
	float4 shadow_bias;
	float4 shadow_normal_bias;
	float4 shadow_transmittance_bias;
	float4 shadow_z_range;
	float4 shadow_range_begin;
	float4 shadow_split_offsets;
	row_major float4x4 shadow_matrix1;
	row_major float4x4 shadow_matrix2;
	row_major float4x4 shadow_matrix3;
	row_major float4x4 shadow_matrix4;
	float2 uv_scale1;
	float2 uv_scale2;
	float2 uv_scale3;
	float2 uv_scale4;
};
struct DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
};
[[vk::binding(13, 0)]] ConstantBuffer<DirectionalLights> directional_lights;
[[vk::binding(14, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> separate_specular;
[[vk::binding(15, 0)]] SamplerState environment_sampler;
[[vk::binding(16, 0)]] Texture2D<float4> indirect_diffuse;
[[vk::binding(17, 0)]] Texture2D<float4> camera_radiance;

void reconstruct_radiance(uint2 pixel, SurfaceData surface, float3 view_position, out float3 diffuse, out float3 specular) {
	if (all(params.lighting_size == params.size)) {
		diffuse = diffuse_radiance.Load(int3(pixel, 0)).rgb;
		specular = specular_radiance.Load(int3(pixel, 0)).rgb;
		return;
	}
	float2 lighting_position = rtxdi_lighting_position(float2(pixel) + 0.5, params.lighting_size, params.size) - 0.5;
	int2 nearest = int2(floor(lighting_position + 0.5));
	float3 view_normal = mul(surface.geometry_normal, (float3x3)decode_inverse_view(scene.data));
	diffuse = float3(0.0);
	specular = float3(0.0);
	float diffuse_weight = 0.0;
	float specular_weight = 0.0;
	float best_score = 0.0;
	float3 fallback_diffuse = float3(0.0);
	float3 fallback_specular = float3(0.0);
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			int2 tap = nearest + int2(x, y);
			if (any(tap < int2(0)) || any(tap >= int2(params.lighting_size))) {
				continue;
			}
			uint2 representative = rtxdi_surface_pixel(uint2(tap), params.lighting_size, params.size);
			int3 address = int3(representative, 0);
			uint2 classification = surface_classification.Load(address);
			float depth = surface_depth.Load(address);
			if (classification.x != surface.material.flags || classification.y != surface.layer_mask || depth <= 0.0) {
				continue;
			}
			float4 shading = surface_shading.Load(address);
			float3 tap_normal = oct_to_vec3(shading.xy * 2.0 - 1.0);
			float normal_weight = pow(saturate(dot(surface.normal, tap_normal)), 32.0);
			float2 uv = (float2(representative) + 0.5) / float2(params.size);
			float4 tap_view = mul(float4(uv * 2.0 - 1.0, depth, 1.0), scene.data.inv_projection_matrix);
			tap_view /= tap_view.w;
			float plane_distance = abs(dot(tap_view.xyz - view_position, view_normal));
			float depth_tolerance = max(0.005, abs(view_position.z) * 0.002);
			float2 offset = float2(tap) - lighting_position;
			float spatial_weight = exp2(-dot(offset, offset));
			float weight = spatial_weight * normal_weight * exp2(-plane_distance / depth_tolerance);
			float roughness_weight = exp2(-abs(shading.z - surface.material.roughness) * 32.0);
			float3 tap_diffuse = diffuse_radiance.Load(int3(tap, 0)).rgb;
			float3 tap_specular = specular_radiance.Load(int3(tap, 0)).rgb;
			diffuse += tap_diffuse * weight;
			specular += tap_specular * weight * roughness_weight;
			diffuse_weight += weight;
			specular_weight += weight * roughness_weight;
			float score = spatial_weight * normal_weight / (1.0 + plane_distance / depth_tolerance);
			if (score > best_score) {
				best_score = score;
				fallback_diffuse = tap_diffuse;
				fallback_specular = tap_specular;
			}
		}
	}
	diffuse = diffuse_weight > 1e-6 ? diffuse / diffuse_weight : fallback_diffuse;
	specular = specular_weight > 1e-6 ? specular / specular_weight : fallback_specular;
}

float3 fog_sample_radiance(float3 view_direction, float mip_level) {
	float3 sky_direction = mul(view_direction, scene.data.radiance_inverse_xform);
	float2 uv = vec3_to_oct_with_border(normalize(sky_direction), float2(scene.data.radiance_border_size, 1.0 - 2.0 * scene.data.radiance_border_size));
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float layer = clamp(mip_level * MAX_ROUGHNESS_LOD, 0.0, MAX_ROUGHNESS_LOD);
	return lerp(radiance.SampleLevel(environment_sampler, float3(uv, floor(layer)), 0.0).rgb, radiance.SampleLevel(environment_sampler, float3(uv, ceil(layer)), 0.0).rgb, frac(layer)) * params.environment_energy * scene.data.emissive_exposure_normalization;
#else
	return radiance.SampleLevel(environment_sampler, uv, mip_level * MAX_ROUGHNESS_LOD).rgb * params.environment_energy * scene.data.emissive_exposure_normalization;
#endif
}

float4 fog_process(float3 vertex) {
	float3 fog_color = scene.data.fog_light_color;
	if (scene.data.fog_aerial_perspective > 0.0) {
		float mip_level = lerp(1.0 / MAX_ROUGHNESS_LOD, 1.0, 1.0 - (abs(vertex.z) - scene.data.z_near) / (scene.data.z_far - scene.data.z_near));
		fog_color = lerp(fog_color, fog_sample_radiance(vertex, mip_level), scene.data.fog_aerial_perspective);
	}
	if (scene.data.fog_sun_scatter > 0.001) {
		float3 view = normalize(vertex);
		for (uint i = 0u; i < scene.data.directional_light_count; i++) {
			DirectionalLightData light = directional_lights.data[i];
			float light_amount = pow(max(dot(view, light.direction), 0.0), 8.0);
			fog_color += light.color * light.energy * light_amount * scene.data.fog_sun_scatter;
		}
	}
	float fog_amount;
	if ((scene.data.flags & SCENE_DATA_FLAGS_USE_DEPTH_FOG) != 0u) {
		float fog_z = smoothstep(scene.data.fog_depth_begin, scene.data.fog_depth_end, length(vertex));
		fog_amount = pow(fog_z, scene.data.fog_depth_curve) * scene.data.fog_density;
	} else {
		fog_amount = 1.0 - exp(min(0.0, -length(vertex) * scene.data.fog_density));
	}
	if (abs(scene.data.fog_height_density) >= 0.0001) {
		float y = mul(decode_inverse_view(scene.data), float4(vertex, 1.0)).y;
		float height_amount = 1.0 - exp(min(0.0, (y - scene.data.fog_height) * scene.data.fog_height_density));
		fog_amount = max(height_amount, fog_amount);
	}
	return float4(fog_color, fog_amount);
}
#endif

[numthreads(8, 8, 1)]
void main(uint3 invocation : SV_DispatchThreadID) {
	uint2 pixel = invocation.xy;
#ifdef MODE_PREPARE_LIGHTING
	if (any(pixel >= params.lighting_size)) {
		return;
	}
	uint2 surface_pixel = rtxdi_surface_pixel(pixel, params.lighting_size, params.size);
#else
	if (any(pixel >= params.size)) {
		return;
	}
	uint2 surface_pixel = pixel;
#endif
	int3 address = int3(surface_pixel, 0);
	float2 uv = (float2(surface_pixel) + 0.5) / float2(params.size);
	float depth = surface_depth.Load(address);
	SurfaceBufferData packed;
	packed.base = surface_base.Load(address);
	packed.shading = surface_shading.Load(address);
	packed.emission = surface_emission.Load(address);
	packed.geometry = surface_geometry.Load(address);
	packed.classification = surface_classification.Load(address);
	packed.ambient_occlusion = surface_motion.Load(address).w;
	uint flags = packed.classification.x;
	bool surface_valid = (flags & 1u) != 0u && depth > 0.0;
	float4 view_position = mul(float4(uv * 2.0 - 1.0, surface_valid ? depth : 0.5, 1.0), scene.data.inv_projection_matrix);
	view_position /= view_position.w;
	if (!surface_valid) {
		if (params.orthogonal != 0u) {
			view_position.z = -scene.data.z_far;
		} else {
			view_position.xyz *= scene.data.z_far / max(-view_position.z, 1e-6);
		}
	}
	float4x4 inverse_view = decode_inverse_view(scene.data);
	float3 camera_direction = float3(inverse_view[0][2], inverse_view[1][2], inverse_view[2][2]);
	float3 view_direction = normalize(params.orthogonal != 0u ? camera_direction : -mul((float3x3)inverse_view, view_position.xyz));
	SurfaceData surface = decode_surface_buffers(packed, mul(inverse_view, float4(view_position.xyz, 1.0)).xyz, view_direction);
	float3 normal = surface_valid ? surface.normal : float3(0.0, 0.0, 1.0);

#ifdef MODE_PREPARE
#ifdef MODE_PREPARE_LIGHTING
	float4 packed_motion = surface_motion.Load(address);
	float2 jitter_delta = (scene.previous.taa_jitter - scene.data.taa_jitter) * 0.5;
	float2 previous_position = float2(surface_pixel) + 0.5 + packed_motion.xy + jitter_delta * float2(params.size);
	packed_motion.xy = rtxdi_lighting_position(previous_position, params.lighting_size, params.size) - (float2(pixel) + 0.5) - jitter_delta * float2(params.lighting_size);
	nrd_motion[pixel] = packed_motion;
#else
	if (all(params.lighting_size == params.size)) {
#endif
		nrd_normal_roughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, surface.material.roughness, 0.0);
		nrd_view_depth[pixel] = surface_valid && (flags & 2u) == 0u ? -view_position.z : 1e6;
#ifndef MODE_PREPARE_LIGHTING
	}
	float2 motion;
	if (surface_valid) {
		motion = surface_motion.Load(address).xy / float2(params.size);
	} else {
		float4 direction = mul(float4(uv * 2.0 - 1.0, 0.0, 1.0), scene.data.inv_projection_matrix);
		float3 world_direction = mul((float3x3)inverse_view, normalize(direction.xyz));
		float3 previous_view = mul(world_direction, (float3x3)decode_inverse_view(scene.previous));
		float4 previous_clip = mul(float4(previous_view, 0.0), scene.previous.projection_matrix);
		motion = abs(previous_clip.w) > 1e-6 ? ((previous_clip.xy / previous_clip.w - scene.previous.taa_jitter) - (uv * 2.0 - 1.0 - scene.data.taa_jitter)) * 0.5 : float2(0.0);
	}
	velocity[pixel] = motion;
	float3 view_normal = mul(normal, (float3x3)inverse_view);
	normal_roughness[pixel] = float4(view_normal * 0.5 + 0.5, surface.material.roughness * (127.0 / 255.0));
	bool lit_surface = surface_valid && (flags & 2u) == 0u;
	rr_diffuse_albedo[pixel] = float4(lit_surface ? surface.material.base_color * (1.0 - surface.material.metallic) : float3(0.0), 1.0);
	rr_specular_albedo[pixel] = float4(lit_surface ? lerp(float3(0.08 * surface.material.specular), surface.material.base_color, surface.material.metallic) : float3(0.0), 1.0);
	rr_normal_roughness[pixel] = float4(normalize(normal), surface_valid ? saturate(surface.material.roughness) : 1.0);
	float reflection_distance = lit_surface && params.reflection_hit_distance_enabled != 0u ? reflection_hit_distance.Load(address) : 0.0;
	rr_specular_hit_distance[pixel] = isnan(reflection_distance) || isinf(reflection_distance) ? 0.0 : clamp(reflection_distance, 0.0, 65504.0);
#endif
#else
	if (!surface_valid && params.camera_radiance_enabled == 0u) {
		if (params.separate_specular != 0u) {
			separate_specular[pixel] = float4(0.0);
		}
		return;
	}
	float3 output_color = surface.material.emission;
	float3 specular_color = float3(0.0);
	bool use_camera_radiance = params.camera_radiance_full != 0u || (!surface_valid && params.camera_radiance_enabled != 0u);
	if (use_camera_radiance) {
		output_color = camera_radiance.Load(address).rgb * scene.data.emissive_exposure_normalization;
	} else if ((flags & 2u) == 0u) {
		float3 diffuse_factor;
		float3 specular_factor;
		rtxdi_nrd_material_factors(normal, surface.view_direction, surface.material.base_color * (1.0 - surface.material.metallic), lerp(float3(0.08 * surface.material.specular), surface.material.base_color, surface.material.metallic), surface.material.roughness, diffuse_factor, specular_factor);
		float3 diffuse_lighting;
		float3 specular_lighting;
		reconstruct_radiance(pixel, surface, view_position.xyz, diffuse_lighting, specular_lighting);
		output_color += diffuse_lighting * diffuse_factor * scene.data.emissive_exposure_normalization;
		if (params.ddgi_enabled != 0u) {
			output_color += indirect_diffuse.Load(address).rgb * scene.data.emissive_exposure_normalization;
		}
		specular_color = specular_lighting * specular_factor * scene.data.emissive_exposure_normalization;
	}
	float4 fog = float4(0.0, 0.0, 0.0, 1.0);
	bool fixed_fog = (scene.data.flags & SCENE_DATA_FLAGS_USE_FOG) != 0u;
	if (fixed_fog) {
		fog = fog_process(view_position.xyz);
		fog.rgb *= fog.a;
		fog.a = 1.0 - fog.a;
	}
	if (params.fog_enabled != 0u) {
		float fog_z = -view_position.z * params.fog_inverse_length;
		fog_z = fog_z < 1.0 ? pow(max(fog_z, 0.0), params.fog_spread) : fog_z;
		float4 volume = volumetric_fog.SampleLevel(environment_sampler, float3(uv, fog_z), 0.0);
		if (fixed_fog) {
			fog.rgb = params.fog_legacy_blending != 0u ? lerp(volume.rgb, fog.rgb, volume.a) : fog.rgb * volume.a + volume.rgb;
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
	specular_color = any(isnan(specular_color)) || any(isinf(specular_color)) ? float3(0.0) : clamp(specular_color, 0.0, 65504.0);
	if (params.separate_specular != 0u) {
		separate_specular[pixel] = float4(specular_color, use_camera_radiance ? 0.0 : surface.material.metallic);
	} else {
		output_color += specular_color;
	}
	output_color = any(isnan(output_color)) || any(isinf(output_color)) ? float3(0.0) : clamp(output_color, 0.0, 65504.0);
	color[pixel] = float4(output_color, 1.0);
#endif
}
