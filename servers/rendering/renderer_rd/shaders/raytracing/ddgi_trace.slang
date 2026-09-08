#[raygen]

#VERSION_DEFINES
#include "ddgi_trace_inc.slang"

[shader("raygeneration")]
void main() {
	uint3 index = DispatchRaysIndex();
	DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(ddgi_volumes[ddgi_frame.x]);
	uint probe = index.y;
	uint ray_index = index.x;
	RayDesc ray;
	ray.Origin = ddgi_probe_position(probe, volume, ddgi_traced_data);
	ray.Direction = DDGIGetProbeRayDirection(ray_index, volume);
	ray.TMin = 0.001;
	ray.TMax = volume.probeMaxRayDistance;
	RTHitPayload hit = (RTHitPayload)0;
	hit.ray_layer_mask = ddgi_frame.w;
	hit.ray_flags = 2u;
	hit.cone_spread = ddgi_probe_ray_spread(volume);
	TraceRay(scene_tlas, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, 0, 0, 0, ray, hit);
	uint3 texel = DDGIGetRayDataTexelCoords(ray_index, probe, volume);
	if (hit.hit == 0u) {
		DDGIStoreProbeRayMiss(ddgi_ray_data, texel, volume, ddgi_environment(ray.Direction));
		return;
	}
	if (hit.front_face == 0u) {
		ddgi_ray_data[texel] = 0.0;
		DDGIStoreProbeRayBackfaceHit(ddgi_ray_data, texel, volume, hit.hit_distance);
		return;
	}
	uint rng = ddgi_hash(probe * uint(volume.probeNumRays) + ray_index + ddgi_frame.z * 1664525u);
	float3 radiance = hit.surface.material.emission;
	if ((hit.surface.material.flags & 2u) == 0u) {
		radiance += ddgi_direct(hit, rng);
		float3 diffuse_factor, specular_factor;
		rtxdi_nrd_material_factors(hit.surface.normal, hit.surface.view_direction, hit.surface.material.base_color * (1.0 - hit.surface.material.metallic), lerp(float3(0.08 * hit.surface.material.specular), hit.surface.material.base_color, hit.surface.material.metallic), hit.surface.material.roughness, diffuse_factor, specular_factor);
		radiance += ddgi_query(hit.surface.world_position, hit.surface.normal, -hit.surface.view_direction) * diffuse_factor * (1.0 / RTXGI_PI) * hit.surface.material.ambient_occlusion;
	}
	DDGIStoreProbeRayFrontfaceHit(ddgi_ray_data, texel, volume, max(radiance, 0.0), hit.hit_distance);
}

#[miss]

#VERSION_DEFINES
#include "ddgi_trace_inc.slang"

[shader("miss")]
void main(inout RTHitPayload hit) {
	hit.hit = 0u;
}
