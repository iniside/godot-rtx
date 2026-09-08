#ifndef RT_LIGHTING_RECONSTRUCTION_INC_SLANG
#define RT_LIGHTING_RECONSTRUCTION_INC_SLANG

#include "../rtxdi_resolution_inc.slang"
#ifdef RECONSTRUCT_DIRECT_LIGHTING
#include <Rtxdi/Utils/RandomSamplerState.hlsli>
#endif

void reconstruct_lighting(uint2 pixel, uint2 frame_size, uint2 lighting_size, SurfaceData surface, float3 view_position, float3 view_normal, bool use_direct, bool use_indirect, bool stochastic_direct_samples, uint frame_index, out float3 diffuse, out float3 specular, out float3 indirect) {
	diffuse = float3(0.0);
	specular = float3(0.0);
	indirect = float3(0.0);
	if (all(lighting_size == frame_size)) {
#ifdef RECONSTRUCT_DIRECT_LIGHTING
		if (use_direct) {
			diffuse = diffuse_radiance.Load(int3(pixel, 0)).rgb;
			specular = specular_radiance.Load(int3(pixel, 0)).rgb;
		}
#endif
		if (use_indirect) {
			indirect = indirect_diffuse.Load(int3(pixel, 0)).rgb;
		}
		return;
	}
	float2 lighting_position = rtxdi_lighting_position(float2(pixel) + 0.5, lighting_size, frame_size) - 0.5;
	int2 nearest = int2(floor(lighting_position + 0.5));
	float diffuse_weight = 0.0;
	float specular_weight = 0.0;
	float best_score = 0.0;
	float3 fallback_diffuse = float3(0.0);
	float3 fallback_specular = float3(0.0);
	float3 fallback_indirect = float3(0.0);
#ifdef RECONSTRUCT_DIRECT_LIGHTING
	RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, frame_index, 4u);
	float3 sampled_diffuse = float3(0.0);
	float3 sampled_specular = float3(0.0);
#endif
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			int2 tap = nearest + int2(x, y);
			if (any(tap < int2(0)) || any(tap >= int2(lighting_size))) {
				continue;
			}
			uint2 representative = rtxdi_surface_pixel(uint2(tap), lighting_size, frame_size);
			int3 address = int3(representative, 0);
			uint2 classification = surface_classification.Load(address);
			float depth = surface_depth.Load(address);
			if (classification.x != surface.material.flags || classification.y != surface.layer_mask || depth <= 0.0) {
				continue;
			}
			float4 shading = surface_shading.Load(address);
			float3 tap_normal = oct_to_vec3(shading.xy * 2.0 - 1.0);
			float normal_weight = pow(saturate(dot(surface.normal, tap_normal)), 32.0);
			float2 uv = (float2(representative) + 0.5) / float2(frame_size);
			float4 tap_view = mul(float4(uv * 2.0 - 1.0, depth, 1.0), scene.data.inv_projection_matrix);
			tap_view /= tap_view.w;
			float plane_distance = abs(dot(tap_view.xyz - view_position, view_normal));
			float depth_tolerance = max(0.005, abs(view_position.z) * 0.002);
			float2 offset = float2(tap) - lighting_position;
			float spatial_weight = exp2(-dot(offset, offset));
			float weight = spatial_weight * normal_weight * exp2(-plane_distance / depth_tolerance);
			float3 tap_diffuse = float3(0.0);
			float3 tap_specular = float3(0.0);
#ifdef RECONSTRUCT_DIRECT_LIGHTING
			if (use_direct) {
				float roughness_weight = exp2(-abs(shading.z - surface.material.roughness) * 32.0);
				tap_diffuse = diffuse_radiance.Load(int3(tap, 0)).rgb;
				tap_specular = specular_radiance.Load(int3(tap, 0)).rgb;
				diffuse += tap_diffuse * weight;
				specular += tap_specular * weight * roughness_weight;
				specular_weight += weight * roughness_weight;
				if (stochastic_direct_samples) {
					if (RTXDI_GetNextRandom(rng) * (diffuse_weight + weight) < weight) {
						sampled_diffuse = tap_diffuse;
					}
					if (RTXDI_GetNextRandom(rng) * specular_weight < weight * roughness_weight) {
						sampled_specular = tap_specular;
					}
				}
			}
#endif
			float3 tap_indirect = use_indirect ? indirect_diffuse.Load(int3(tap, 0)).rgb : float3(0.0);
			indirect += tap_indirect * weight;
			diffuse_weight += weight;
			float score = spatial_weight * normal_weight / (1.0 + plane_distance / depth_tolerance);
			if (score > best_score) {
				best_score = score;
				fallback_diffuse = tap_diffuse;
				fallback_specular = tap_specular;
				fallback_indirect = tap_indirect;
			}
		}
	}
	diffuse = diffuse_weight > 1e-6 ? diffuse / diffuse_weight : fallback_diffuse;
	specular = specular_weight > 1e-6 ? specular / specular_weight : fallback_specular;
#ifdef RECONSTRUCT_DIRECT_LIGHTING
	if (stochastic_direct_samples && use_direct) {
		diffuse = diffuse_weight > 1e-6 ? sampled_diffuse : fallback_diffuse;
		specular = specular_weight > 1e-6 ? sampled_specular : fallback_specular;
	}
#endif
	indirect = diffuse_weight > 1e-6 ? indirect / diffuse_weight : fallback_indirect;
}

#endif
