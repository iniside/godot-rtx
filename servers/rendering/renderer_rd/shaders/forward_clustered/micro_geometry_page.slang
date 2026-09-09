#[compute]
#VERSION_DEFINES
#include "micro_geometry_inc.slang"

struct PageParameters {
	uint64_t asset;
	uint64_t pool;
	uint64_t lookup;
	uint64_t surface_offsets;
	uint first_cluster;
	uint cluster_count;
	uint slot;
	uint pad;
};
[[vk::push_constant]] ConstantBuffer<PageParameters> parameters;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint2> primitive_lookup;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= parameters.cluster_count) { return; }
	MicroGeometryAsset asset = *((MicroGeometryAsset*)parameters.asset);
	uint cluster_id = parameters.first_cluster + id.x;
	MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_id];
	if (cluster.refined_group != 0xffffffff) { return; }
	MicroGeometrySurface surface = ((MicroGeometrySurface*)asset.surfaces)[cluster.surface];
	uint64_t primitives = parameters.pool + uint64_t(parameters.slot) * 65536 + cluster.payload_offset + uint64_t(cluster.vertex_count) * surface.vertex_stride + cluster.triangle_count * 3;
	uint offset = ((uint*)parameters.surface_offsets)[cluster.surface];
	for (uint triangle = 0; triangle < cluster.triangle_count; triangle++) {
		uint primitive = micro_geometry_load_word(primitives + triangle * 4);
		if (primitive < surface.source_triangle_count) {
			primitive_lookup[offset + primitive] = uint2(cluster_id, triangle);
		}
	}
}
