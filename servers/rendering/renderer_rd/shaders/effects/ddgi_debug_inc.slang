#ifndef DDGI_DEBUG_INC_SLANG
#define DDGI_DEBUG_INC_SLANG

#include "ddgi_camera_inc.slang"
#include "ddgi_probe_inc.slang"

[[vk::binding(9, 0)]] Texture2D<float4> camera_indirect;
[[vk::binding(0, 1)]] Texture2DArray<uint> probe_update_frame;
[[vk::binding(1, 1)]] StructuredBuffer<uint> probe_reset_mask;

struct DebugParams {
	uint2 output_size;
	uint2 internal_size;
	uint mode;
	uint cascade;
	uint reset_cascades;
	uint orthogonal;
};
[[vk::push_constant]] ConstantBuffer<DebugParams> params;

struct DebugVertex {
	float4 position : SV_Position;
	float2 disc : TEXCOORD0;
	nointerpolation float3 color : TEXCOORD1;
};

float3 ddgi_debug_color(uint cascade) {
	const float3 colors[6] = { float3(1.0, 0.15, 0.1), float3(0.1, 1.0, 0.15), float3(0.15, 0.35, 1.0), float3(1.0, 1.0, 0.1), float3(0.1, 1.0, 1.0), float3(1.0, 0.5, 0.1) };
	return colors[min(cascade, 5u)];
}

float4 ddgi_debug_project(float3 position) {
	float4x4 camera = rt_camera_to_rt(rt_frame, false);
	float3 view_position = mul(transpose((float3x3)camera), position - float3(camera[0][3], camera[1][3], camera[2][3]));
	float4 clip_position = mul(float4(view_position, 1.0), scene.data.projection_matrix);
	clip_position.xy -= scene.data.taa_jitter * clip_position.w;
	return clip_position;
}

bool ddgi_debug_clip(inout float4 a, inout float4 b, float da, float db) {
	if (da < 0.0 && db < 0.0) {
		return false;
	}
	if (da < 0.0) {
		a = lerp(a, b, da / (da - db));
	} else if (db < 0.0) {
		b = lerp(a, b, da / (da - db));
	}
	return true;
}

#endif
