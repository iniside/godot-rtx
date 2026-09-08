#[compute]

#VERSION_DEFINES

#define MAX_VIEWS 2
#include "../scene_data_inc.slang"
#include "../surface_data_inc.slang"
#include "../raytracing/rt_frame_constants_inc.slang"
#include "../raytracing/rtxdi_nrd_inc.slang"
#include "ddgi_grid_inc.slang"

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
[[vk::binding(8, 0)]] ConstantBuffer<RTFrameConstants> rt_frame;
[[vk::binding(9, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> indirect_radiance;

struct Params {
	uint2 size;
	uint orthogonal;
	uint padding;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	uint2 pixel = id.xy;
	if (any(pixel >= params.size)) {
		return;
	}
	int3 address = int3(pixel, 0);
	uint flags = surface_classification.Load(address).x;
	float depth = surface_depth.Load(address);
	if ((flags & 3u) != 1u || depth <= 0.0) {
		indirect_radiance[pixel] = 0.0;
		return;
	}
	float2 uv = (float2(pixel) + 0.5) / float2(params.size);
	float4 view_position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), scene.data.inv_projection_matrix);
	view_position /= view_position.w;
	float4x4 camera_to_rt = rt_camera_to_rt(rt_frame, false);
	float3 position = mul(camera_to_rt, float4(view_position.xyz, 1.0)).xyz;
	float3 camera_direction = float3(camera_to_rt[0][2], camera_to_rt[1][2], camera_to_rt[2][2]);
	float3 view = normalize(params.orthogonal != 0u ? camera_direction : -mul((float3x3)camera_to_rt, view_position.xyz));
	SurfaceBufferData packed;
	packed.base = surface_base.Load(address);
	packed.shading = surface_shading.Load(address);
	packed.emission = surface_emission.Load(address);
	packed.geometry = surface_geometry.Load(address);
	packed.classification = surface_classification.Load(address);
	packed.ambient_occlusion = surface_motion.Load(address).w;
	SurfaceData surface = decode_surface_buffers(packed, position, view);
	float3 diffuse_factor, specular_factor;
	rtxdi_nrd_material_factors(surface.normal, view, surface.material.base_color * (1.0 - surface.material.metallic), lerp(float3(0.08 * surface.material.specular), surface.material.base_color, surface.material.metallic), surface.material.roughness, diffuse_factor, specular_factor);
	float3 indirect = ddgi_query(position, surface.normal, -view) * diffuse_factor * (1.0 / RTXGI_PI) * surface.material.ambient_occlusion;
	indirect_radiance[pixel] = float4(indirect, 1.0);
}
