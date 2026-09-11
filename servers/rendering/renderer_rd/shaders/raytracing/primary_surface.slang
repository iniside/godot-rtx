#[raygen]

#VERSION_DEFINES
#include "primary_surface_inc.slang"

struct PrimaryFrame {
	uint4 extent_layers;
	uint4 options;
};
[[vk::binding(0, 2)]] ConstantBuffer<PrimaryFrame> primary;
[[vk::binding(1, 2), vk::image_format("rgba8")]] RWTexture2D<float4> primary_base;
[[vk::binding(2, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> primary_shading;
[[vk::binding(3, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> primary_emission;
[[vk::binding(4, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> primary_motion;
[[vk::binding(5, 2), vk::image_format("r32ui")]] RWTexture2D<uint> primary_geometry;
[[vk::binding(6, 2), vk::image_format("rg32ui")]] RWTexture2D<uint2> primary_classification;
[[vk::binding(7, 2), vk::image_format("r32f")]] RWTexture2D<float> primary_depth;
[[vk::binding(8, 2)]] StructuredBuffer<int> primary_motion_indices;
[[vk::binding(9, 2)]] StructuredBuffer<InstanceMotionData> primary_instance_motion;
[[vk::binding(10, 2)]] RWStructuredBuffer<uint> primary_statistics;

[shader("raygeneration")]
void main() {
	uint2 pixel = DispatchRaysIndex().xy;
	float2 uv = (float2(pixel) + 0.5) / float2(primary.extent_layers.xy);
	float4 near_view = mul(float4(uv * 2.0 - 1.0, 1.0, 1.0), scene_data_block.data.inv_projection_matrix);
	near_view /= near_view.w;
	float4x4 camera_to_rt = rt_camera_to_rt(rt_frame, false);
	float3 view_direction = primary.extent_layers.w != 0u ? float3(0.0, 0.0, -1.0) : normalize(near_view.xyz);
	RayDesc ray;
	ray.Direction = normalize(mul((float3x3)camera_to_rt, view_direction));
	ray.Origin = mul(camera_to_rt, float4(primary.extent_layers.w != 0u ? float3(near_view.xy, 0.0) : float3(0.0), 1.0)).xyz;
	ray.TMin = scene_data_block.data.z_near / max(-view_direction.z, 1e-6);
	ray.TMax = scene_data_block.data.z_far / max(-view_direction.z, 1e-6);
	RTHitPayload hit = (RTHitPayload)0;
	hit.ray_layer_mask = primary.extent_layers.z;
	hit.ray_flags = 4u;
	TraceRay(scene_tlas, RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 0, 0, 0, ray, hit);
	PrimarySurfaceData output = reconstruct_primary_surface(hit, ray, primary.extent_layers.xy, primary_motion_indices, primary_instance_motion);
	primary_base[pixel] = output.surface.base;
	primary_shading[pixel] = output.surface.shading;
	primary_emission[pixel] = output.surface.emission;
	primary_motion[pixel] = output.motion;
	primary_geometry[pixel] = output.surface.geometry;
	primary_classification[pixel] = output.surface.classification;
	primary_depth[pixel] = output.depth;
	if (primary.options.x != 0u) {
		uint rays = WaveActiveCountBits(true);
		uint hits = WaveActiveCountBits(hit.hit != 0u);
		uint unsupported = WaveActiveCountBits(hit.hit != 0u && (hit.surface.material.flags & 2u) != 0u);
		if (WaveIsFirstLane()) {
			InterlockedAdd(primary_statistics[0], rays);
			InterlockedAdd(primary_statistics[1], hits);
			InterlockedAdd(primary_statistics[2], rays - hits);
			InterlockedAdd(primary_statistics[3], unsupported);
		}
	}
}

#[miss]

#VERSION_DEFINES
#include "rt_hit_context_inc.slang"

[shader("miss")]
void main(inout RTHitPayload hit) {
	hit.hit = 0u;
}
