#[compute]

#VERSION_DEFINES

#include "ddgi_camera_inc.slang"

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
	SurfaceData surface;
	float3 position, view;
	if (!ddgi_camera_surface(pixel, params.size, params.orthogonal != 0u, surface, position, view)) {
		indirect_radiance[pixel] = 0.0;
		return;
	}
	float3 diffuse_factor, specular_factor;
	rtxdi_nrd_material_factors(surface.normal, view, surface.material.base_color * (1.0 - surface.material.metallic), lerp(float3(0.08 * surface.material.specular), surface.material.base_color, surface.material.metallic), surface.material.roughness, diffuse_factor, specular_factor);
	float3 indirect = ddgi_query(position, surface.normal, -view) * diffuse_factor * (1.0 / RTXGI_PI) * surface.material.ambient_occlusion;
	indirect_radiance[pixel] = float4(indirect, 1.0);
}
