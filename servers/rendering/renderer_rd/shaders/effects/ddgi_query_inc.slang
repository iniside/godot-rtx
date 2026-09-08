#ifndef DDGI_QUERY_INC_SLANG
#define DDGI_QUERY_INC_SLANG

#include "ddgi_sdk_inc.slang"
#include "rtxgi-sdk/shaders/ddgi/Irradiance.hlsl"

float4 ddgi_sample_cascade(float3 position, float3 normal, float3 view_direction, DDGIVolumeDescGPU volume, DDGIVolumeResources resources) {
	float3 center = volume.origin + volume.probeScrollOffsets * volume.probeSpacing;
	float3 extent = (volume.probeCounts - 1) * volume.probeSpacing * 0.5;
	float3 edge_distance = extent - abs(position - center);
	if (any(edge_distance < 0.0)) {
		return 0.0;
	}
	float3 bias = DDGIGetSurfaceBias(normal, view_direction, volume);
	float support;
	float3 irradiance = DDGIGetVolumeIrradiance(position, bias, normal, volume, resources, support);
	float3 edge_weight = saturate(edge_distance / volume.probeSpacing);
	float confidence = saturate(support) * min(edge_weight.x, min(edge_weight.y, edge_weight.z));
	return float4(irradiance, confidence);
}

#endif
