#[compute]

#VERSION_DEFINES

#include "ddgi_camera_inc.slang"
#include "../rtxdi_resolution_inc.slang"

[[vk::binding(9, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> indirect_radiance;

struct Params {
	uint2 size;
	uint orthogonal;
	uint padding;
	uint2 interpolation_size;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	uint2 pixel = id.xy;
	if (any(pixel >= params.interpolation_size)) {
		return;
	}
	SurfaceData surface;
	float3 position, view;
	uint2 surface_pixel = rtxdi_surface_pixel(pixel, params.interpolation_size, params.size);
	if (!ddgi_camera_surface(surface_pixel, params.size, params.orthogonal != 0u, surface, position, view)) {
		indirect_radiance[pixel] = 0.0;
		return;
	}
	float3 indirect = ddgi_query(position, surface.normal, -view) * (1.0 / RTXGI_PI);
	indirect_radiance[pixel] = float4(indirect, 1.0);
}
