#ifndef RTXDI_NRD_INC_SLANG
#define RTXDI_NRD_INC_SLANG

#include "thirdparty/nrd/Shaders/NRD.hlsli"

void rtxdi_nrd_material_factors(float3 normal, float3 view, float3 albedo, float3 f0, float roughness, out float3 diffuse, out float3 specular) {
	NRD_MaterialFactors(normal, view, albedo, f0, roughness, diffuse, specular);
}

float4 rtxdi_nrd_pack_radiance_distance(float3 radiance, float distance) {
	return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, distance, true);
}

#endif
