#[compute]
#VERSION_DEFINES
#include "geometry_decode_inc.slang"

struct GeometryPositionsParameters {
	uint vertex_count;
};

[[vk::binding(0, 0)]] StructuredBuffer<GeometryData> input_geometry;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> positions;
[[vk::push_constant]] ConstantBuffer<GeometryPositionsParameters> parameters;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	uint vertex = id.x + id.y * 65536u;
	if (vertex >= parameters.vertex_count) {
		return;
	}
	float3 position = geometry_fetch_position(input_geometry[0], vertex);
	positions[vertex * 3u] = position.x;
	positions[vertex * 3u + 1u] = position.y;
	positions[vertex * 3u + 2u] = position.z;
}
