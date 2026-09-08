#ifndef DDGI_CAMERA_INC_SLANG
#define DDGI_CAMERA_INC_SLANG

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

bool ddgi_camera_surface(uint2 pixel, uint2 size, bool orthogonal, out SurfaceData surface, out float3 position, out float3 view) {
	int3 address = int3(pixel, 0);
	uint flags = surface_classification.Load(address).x;
	float depth = surface_depth.Load(address);
	if ((flags & 3u) != 1u || depth <= 0.0) {
		surface = (SurfaceData)0;
		position = 0.0;
		view = 0.0;
		return false;
	}
	float2 uv = (float2(pixel) + 0.5) / float2(size);
	float4 view_position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), scene.data.inv_projection_matrix);
	view_position /= view_position.w;
	float4x4 camera_to_rt = rt_camera_to_rt(rt_frame, false);
	position = mul(camera_to_rt, float4(view_position.xyz, 1.0)).xyz;
	float3 camera_direction = float3(camera_to_rt[0][2], camera_to_rt[1][2], camera_to_rt[2][2]);
	view = normalize(orthogonal ? camera_direction : -mul((float3x3)camera_to_rt, view_position.xyz));
	SurfaceBufferData packed;
	packed.base = surface_base.Load(address);
	packed.shading = surface_shading.Load(address);
	packed.emission = surface_emission.Load(address);
	packed.geometry = surface_geometry.Load(address);
	packed.classification = surface_classification.Load(address);
	packed.ambient_occlusion = surface_motion.Load(address).w;
	surface = decode_surface_buffers(packed, position, view);
	return true;
}

#endif
