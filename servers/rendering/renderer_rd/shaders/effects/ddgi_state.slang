#[compute]

#VERSION_DEFINES

#include "ddgi_sdk_inc.slang"
#include "rtxgi-sdk/shaders/ddgi/include/ProbeCommon.hlsl"

[[vk::binding(0, 0)]] StructuredBuffer<DDGIVolumeDescGPUPacked> volumes;
[[vk::binding(4, 0), vk::image_format("rgba32f")]] RWTexture2DArray<float4> probe_data;
[[vk::binding(5, 0), vk::image_format("rgba32f")]] RWTexture2DArray<float4> traced_data;
[[vk::binding(6, 0), vk::image_format("r32ui")]] RWTexture2DArray<uint> validity;

struct Params {
	uint cascade;
	uint frame;
	uint2 padding;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

#if defined(MODE_RESET) || defined(MODE_PREPARE)
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> irradiance;
[[vk::binding(3, 0), vk::image_format("rgba32f")]] RWTexture2DArray<float4> distance;

void clear_lighting(uint3 coordinate) {
	for (uint y = 0u; y < 16u; y++) {
		for (uint x = 0u; x < 16u; x++) {
			distance[uint3(coordinate.xy * 16u + uint2(x, y), coordinate.z)] = 0.0;
			if (x < 8u && y < 8u) {
				irradiance[uint3(coordinate.xy * 8u + uint2(x, y), coordinate.z)] = 0.0;
			}
		}
	}
}
#endif

#ifdef MODE_RESET
[[vk::binding(1, 0), vk::image_format("rgba32f")]] RWTexture2DArray<float4> ray_data;
[[vk::binding(7, 0)]] StructuredBuffer<uint> reset_mask;
#endif
#if defined(MODE_RESET) || defined(MODE_FINISH)
[[vk::binding(8, 0), vk::image_format("r32ui")]] RWTexture2DArray<uint> update_frame;
#endif

[numthreads(32, 1, 1)] void main(uint3 dispatch_id : SV_DispatchThreadID) {
	DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(volumes[params.cascade]);
	uint index = dispatch_id.x;
	if (index >= uint(volume.probeCounts.x * volume.probeCounts.y * volume.probeCounts.z)) {
		return;
	}
	uint3 coordinate = DDGIGetProbeTexelCoords(index, volume);
#ifdef MODE_RESET
	if (reset_mask[index] == 0u) {
		return;
	}
	clear_lighting(coordinate);
	probe_data[coordinate] = 0.0;
	traced_data[coordinate] = 0.0;
	validity[coordinate] = 0u;
	update_frame[coordinate] = 0u;
	for (int ray = 0; ray < volume.probeNumRays; ray++) {
		ray_data[DDGIGetRayDataTexelCoords(ray, index, volume)] = 0.0;
	}
#elif defined(MODE_PREPARE)
	if (validity[coordinate] == 0u) {
		clear_lighting(coordinate);
	}
	traced_data[coordinate] = probe_data[coordinate];
#else
	bool matched_position = all(traced_data[coordinate].xyz == probe_data[coordinate].xyz);
	validity[coordinate] = matched_position ? 1u : 0u;
	update_frame[coordinate] = matched_position ? params.frame : 0u;
#endif
}
