#ifndef DDGI_GRID_INC_SLANG
#define DDGI_GRID_INC_SLANG

#include "ddgi_query_inc.slang"

[[vk::binding(0, 2)]] StructuredBuffer<DDGIVolumeDescGPUPacked> ddgi_volumes;
[[vk::binding(1, 2)]] Texture2DArray<float4> ddgi_irradiance[6];
[[vk::binding(2, 2)]] Texture2DArray<float4> ddgi_distance[6];
[[vk::binding(3, 2)]] Texture2DArray<float4> ddgi_probe_data[6];
[[vk::binding(4, 2)]] Texture2DArray<uint> ddgi_validity[6];
[[vk::binding(5, 2)]] SamplerState ddgi_sampler;
[[vk::binding(7, 2)]] ConstantBuffer<uint4> ddgi_frame;

struct DDGIQueryWeights {
	float contribution[6];
	float confidence[6];
	float uncovered;
};

float3 ddgi_query(float3 position, float3 normal, float3 view_direction, out DDGIQueryWeights weights) {
	weights = (DDGIQueryWeights)0;
	float3 result = 0.0;
	float remaining = 1.0;
	for (uint cascade = 0u; cascade < ddgi_frame.y; cascade++) {
		DDGIVolumeResources resources;
		resources.probeIrradiance = ddgi_irradiance[cascade];
		resources.probeDistance = ddgi_distance[cascade];
		resources.probeData = ddgi_probe_data[cascade];
		resources.probeValidity = ddgi_validity[cascade];
		resources.bilinearSampler = ddgi_sampler;
		float4 sample = ddgi_sample_cascade(position, normal, view_direction, UnpackDDGIVolumeDescGPU(ddgi_volumes[cascade]), resources);
		float weight = remaining * sample.a;
		weights.contribution[cascade] = weight;
		weights.confidence[cascade] = sample.a;
		result += sample.rgb * weight;
		remaining *= 1.0 - sample.a;
	}
	weights.uncovered = remaining;
	return result;
}

float3 ddgi_query(float3 position, float3 normal, float3 view_direction) {
	DDGIQueryWeights weights;
	return ddgi_query(position, normal, view_direction, weights);
}

#endif
