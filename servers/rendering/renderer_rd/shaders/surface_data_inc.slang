#ifndef SURFACE_DATA_INC_SLANG
#define SURFACE_DATA_INC_SLANG

#include "oct_inc.slang"

struct SurfaceMaterial {
	float3 base_color;
	float opacity;
	float roughness;
	float metallic;
	float specular;
	float3 emission;
	float ambient_occlusion;
	uint flags;
};

struct SurfaceData {
	float3 world_position;
	float3 normal;
	float3 geometry_normal;
	float3 view_direction;
	uint layer_mask;
	SurfaceMaterial material;
};

struct SurfaceBufferData {
	float4 base;
	float4 shading;
	float4 emission;
	uint geometry;
	uint2 classification;
	float ambient_occlusion;
};

uint pack_unorm_2x16(float2 value) {
	uint2 packed = uint2(round(saturate(value) * 65535.0));
	return packed.x | (packed.y << 16);
}

SurfaceBufferData encode_surface_buffers(SurfaceData surface) {
	bool unsupported = (surface.material.flags & 2u) != 0u;
	SurfaceBufferData result;
	result.base = unsupported ? float4(1.0, 0.0, 1.0, 1.0) : float4(surface.material.base_color, surface.material.opacity);
	result.shading = float4(vec3_to_oct(surface.normal), surface.material.roughness, surface.material.specular);
	result.emission = unsupported ? float4(8.0, 0.0, 8.0, surface.material.metallic) : float4(surface.material.emission, surface.material.metallic);
	result.geometry = pack_unorm_2x16(vec3_to_oct(surface.geometry_normal));
	result.classification = uint2(surface.material.flags, surface.layer_mask);
	result.ambient_occlusion = surface.material.ambient_occlusion;
	return result;
}

#endif
