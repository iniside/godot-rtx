#[raygen]

#VERSION_DEFINES
#include "pathtracing_inc.slang"

[shader("raygeneration")]
void main() {
	uint2 pixel = DispatchRaysIndex().xy;
	uint spp = pt.options.x != 0.0 ? max(pt.settings.x, 1u) : 1u;
	uint rng = pt_hash(pixel.x + pixel.y * pt.extent_frame.x + pt.extent_frame.z * 1664525u);
	float3 sum = 0.0, diffuse_sum = 0.0, specular_sum = 0.0;
	float diffuse_distance = 0.0, specular_distance = 0.0;
	for (uint sample = 0u; sample < spp; sample++) {
		float2 subpixel = pt.options.x != 0.0 ? float2(pt_random(rng), pt_random(rng)) : float2(0.5);
		float2 uv = (float2(pixel) + subpixel) / float2(pt.extent_frame.xy);
		float4 near_view = mul(float4(uv * 2.0 - 1.0, 1.0, 1.0), scene_data_block.data.inv_projection_matrix);
		near_view /= near_view.w;
		float4x4 camera_to_rt = rt_camera_to_rt(rt_frame, false);
		RayDesc ray;
		float3 view_direction = pt.options.z != 0.0 ? float3(0.0, 0.0, -1.0) : normalize(near_view.xyz);
		ray.Direction = normalize(mul((float3x3)camera_to_rt, view_direction));
		ray.Origin = mul(camera_to_rt, float4(pt.options.z != 0.0 ? float3(near_view.xy, 0.0) : float3(0.0), 1.0)).xyz;
		ray.TMin = scene_data_block.data.z_near / max(-view_direction.z, 1e-6);
		ray.TMax = scene_data_block.data.z_far / max(-view_direction.z, 1e-6);
		float spread = pt.options.z != 0.0 ? 0.0 : 2.0 * abs(scene_data_block.data.inv_projection_matrix[1][1]) / float(pt.extent_frame.y);
		float width = pt.options.z != 0.0 ? 2.0 * abs(scene_data_block.data.inv_projection_matrix[1][1]) / float(pt.extent_frame.y) : 0.0;
		float3 throughput = 1.0, primary_diffuse_weight = 0.0, primary_specular_weight = 0.0;
		float3 radiance = 0.0, diffuse = 0.0, specular = 0.0;
		float3 diffuse_factor = 1.0, specular_factor = 1.0;
		float previous_pdf = 0.0;
		uint previous_mask = pt.settings.w;
		for (uint bounce = 0u; bounce < max(pt.settings.y, 1u); bounce++) {
			RTHitPayload hit = pt_trace(ray, pt.settings.w, 0u, width, spread);
			if (bounce == 0u && sample == 0u) { pt_write_primary(pixel, hit, ray); }
			if (bounce == 1u) {
				float distance = hit.hit != 0u ? hit.hit_distance : NRD_FP16_MAX;
				diffuse_distance += distance; specular_distance += distance;
			}
			float3 emission = hit.hit != 0u ? encode_surface_buffers(hit.surface).emission.rgb : (bounce == 0u && pt.options.w == 0.0 ? pt.background.rgb : pt_environment(ray.Direction));
			float weight = bounce > 0u && any(emission != float3(0.0)) ? pt_mis(previous_pdf, pt_emitter_pdf(hit, ray.Origin, ray.Direction, previous_mask)) : 1.0;
			float3 incoming = emission * weight;
			if (bounce == 0u) { radiance += incoming; }
			else { diffuse += primary_diffuse_weight * throughput * incoming; specular += primary_specular_weight * throughput * incoming; }
			if (hit.hit == 0u || (hit.surface.material.flags & 2u) != 0u) { break; }
			float3 direct_diffuse, direct_specular;
			pt_direct(hit, bounce + 1u < max(pt.settings.y, 1u), rng, direct_diffuse, direct_specular);
			if (bounce == 0u) {
				diffuse += direct_diffuse; specular += direct_specular;
				rtxdi_nrd_material_factors(hit.surface.normal, hit.surface.view_direction, hit.surface.material.base_color * (1.0 - hit.surface.material.metallic), lerp(float3(0.08 * hit.surface.material.specular), hit.surface.material.base_color, hit.surface.material.metallic), hit.surface.material.roughness, diffuse_factor, specular_factor);
			} else {
				incoming = direct_diffuse + direct_specular;
				diffuse += primary_diffuse_weight * throughput * incoming; specular += primary_specular_weight * throughput * incoming;
			}
			float3 direction;
			if (!surface_sample_brdf(hit.surface, float3(pt_random(rng), pt_random(rng), pt_random(rng)), direction)) { break; }
			float pdf = surface_brdf_pdf(hit.surface, direction);
			if (pdf <= 0.0) { break; }
			float3 brdf_diffuse, brdf_specular;
			surface_evaluate_brdf(hit.surface, direction, brdf_diffuse, brdf_specular);
			if (bounce == 0u) { primary_diffuse_weight = brdf_diffuse / pdf; primary_specular_weight = brdf_specular / pdf; }
			else { throughput *= (brdf_diffuse + brdf_specular) / pdf; }
			if (bounce >= 2u) {
				float3 total_weight = throughput * (primary_diffuse_weight + primary_specular_weight);
				float survival = clamp(max(total_weight.x, max(total_weight.y, total_weight.z)), 0.05, 0.95);
				if (pt_random(rng) >= survival) { break; }
				throughput /= survival;
			}
			previous_pdf = pdf; previous_mask = hit.surface.layer_mask;
			ray.Origin = hit.surface.world_position; ray.Direction = direction; ray.TMin = 0.001; ray.TMax = pt.options.y;
			width = hit.cone_width; spread = max(spread, hit.surface.material.roughness * hit.surface.material.roughness * 0.25);
		}
		sum += radiance + diffuse + specular;
		diffuse_sum += diffuse / max(diffuse_factor, float3(1e-6));
		specular_sum += specular / max(specular_factor, float3(1e-6));
	}
	float3 value = sum / float(spp);
	if (pt.options.x != 0.0 && pt.settings.z != 0u) {
		uint history = pt.extent_frame.w;
		float3 previous = history > 0u ? pt_accumulation[pixel].rgb : float3(0.0);
		value = previous + (value - previous) * (float(spp) / (float(history) + float(spp)));
		pt_accumulation[pixel] = float4(value, float(history) + float(spp));
	} else { pt_accumulation[pixel] = float4(value, float(spp)); }
	pt_radiance[pixel] = float4(value, 1.0);
	pt_diffuse[pixel] = rtxdi_nrd_pack_radiance_distance(diffuse_sum / float(spp), diffuse_distance / float(spp));
	pt_specular[pixel] = rtxdi_nrd_pack_radiance_distance(specular_sum / float(spp), specular_distance / float(spp));
}

#[miss]

#VERSION_DEFINES
#include "pathtracing_inc.slang"

[shader("miss")]
void main(inout RTHitPayload hit) {
	hit.hit = 0u;
}
