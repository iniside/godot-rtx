#ifndef DDGI_PROBE_INC_SLANG
#define DDGI_PROBE_INC_SLANG

#include "ddgi_sdk_inc.slang"
#include "rtxgi-sdk/shaders/ddgi/include/ProbeCommon.hlsl"

float3 ddgi_probe_position(uint physical_index, DDGIVolumeDescGPU volume, Texture2DArray<float4> traced_data) {
	int3 physical = DDGIGetProbeCoords(physical_index, volume);
	int3 logical = (physical - volume.probeScrollOffsets + volume.probeCounts) % volume.probeCounts;
	return DDGIGetProbeWorldPosition(logical, volume, traced_data);
}

float ddgi_probe_ray_spread(DDGIVolumeDescGPU volume) {
	return 2.0 / sqrt(float(max(1, volume.probeNumRays - RTXGI_DDGI_NUM_FIXED_RAYS)));
}

#endif
