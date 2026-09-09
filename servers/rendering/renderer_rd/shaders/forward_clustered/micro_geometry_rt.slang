#[compute]
#VERSION_DEFINES
#include "micro_geometry_inc.slang"

static const uint PREPARE_CUT = 0u;
static const uint PUBLISH_CUT = 1u;
static const uint UPDATE_TRANSFORMS = 2u;
static const uint RESTORE_CUT = 3u;
static const uint INITIAL_CUT = 4u;

struct MicroRTTask {
	uint64_t clas_addresses;
	uint64_t instance;
	uint64_t surface;
	uint selection_bin;
	uint geometry_base;
	uint motion_base;
	uint instance_count;
	uint cluster_count;
	uint bitmap_offset;
	uint group_offset;
	uint group_count;
	uint instance_flags;
	uint instance_mask;
	uint pad0;
	uint pad1;
};

struct MicroRTBuildInfo {
	uint reference_count;
	uint reference_stride;
	uint64_t references;
};

struct MicroRTTLASInstance {
	float transform[12];
	uint custom_index_mask;
	uint sbt_offset_flags;
	uint64_t blas_address;
};

struct MicroRTMotion {
	float previous_transform[12];
};

struct MicroRTParameters {
	float4 rt_origin;
	float4 rt_origin_low;
	uint task_count;
	uint geometry_count;
	uint64_t references_address;
};

[[vk::binding(0, 0)]] StructuredBuffer<MicroRTTask> tasks;
[[vk::binding(1, 0)]] StructuredBuffer<MicroGeometrySelectedCluster> selected;
[[vk::binding(2, 0)]] StructuredBuffer<uint2> bins;
[[vk::binding(3, 0)]] StructuredBuffer<uint> selection_counts;
[[vk::binding(4, 0)]] StructuredBuffer<MicroGeometryInstance> instances;
[[vk::binding(5, 0)]] StructuredBuffer<uint64_t> blas_addresses;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> membership;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> cached_membership;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint64_t> references;
[[vk::binding(9, 0)]] RWStructuredBuffer<MicroRTBuildInfo> dirty_infos;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint64_t> dirty_destinations;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> dirty_counts;
[[vk::binding(12, 0)]] RWStructuredBuffer<MicroRTTLASInstance> tlas_instances;
[[vk::binding(13, 0)]] RWStructuredBuffer<MicroRTMotion> motion;
[[vk::binding(14, 0)]] RWStructuredBuffer<uint> group_usage;
[[vk::push_constant]] ConstantBuffer<MicroRTParameters> parameters;

float4x4 micro_rt_multimesh_transform(MicroGeometryInstance p_instance, uint p_ordinal, bool p_previous) {
	if (p_instance.multimesh_address == 0) {
		return float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
	}
	uint offset = p_previous ? p_instance.multimesh_previous_offset : p_instance.multimesh_current_offset;
	float* data = (float*)(p_instance.multimesh_address + uint64_t(offset + p_ordinal) * p_instance.multimesh_stride * 4u);
	if ((p_instance.flags & (1u << 13u)) != 0u) {
		return float4x4(float4(data[0], data[1], 0.0, data[3]), float4(data[4], data[5], 0.0, data[7]), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
	}
	return float4x4(float4(data[0], data[1], data[2], data[3]), float4(data[4], data[5], data[6], data[7]), float4(data[8], data[9], data[10], data[11]), float4(0.0, 0.0, 0.0, 1.0));
}

void micro_rt_transform(MicroGeometryInstance p_instance, float4x4 p_multimesh, bool p_previous, out float r_transform[12]) {
	for (uint row = 0u; row < 3u; row++) {
		float3 basis = p_previous ? float3(p_instance.previous_transform[row * 4u], p_instance.previous_transform[row * 4u + 1u], p_instance.previous_transform[row * 4u + 2u]) : float3(p_instance.transform[row * 4u], p_instance.transform[row * 4u + 1u], p_instance.transform[row * 4u + 2u]);
		for (uint column = 0u; column < 3u; column++) {
			r_transform[row * 4u + column] = dot(basis, float3(p_multimesh[0][column], p_multimesh[1][column], p_multimesh[2][column]));
		}
		float high = p_previous ? p_instance.previous_transform[row * 4u + 3u] : p_instance.transform[row * 4u + 3u];
		float low = p_previous ? p_instance.previous_origin_low[row] : p_instance.origin_low[row];
		r_transform[row * 4u + 3u] = (high - parameters.rt_origin[row]) + (low - parameters.rt_origin_low[row]) + dot(basis, float3(p_multimesh[0][3], p_multimesh[1][3], p_multimesh[2][3]));
	}
}

[numthreads(64, 1, 1)]
void main(uint3 p_dispatch : SV_DispatchThreadID) {
	uint geometry = p_dispatch.x + p_dispatch.y * 65536u;
	if (geometry >= parameters.geometry_count || parameters.task_count == 0u) {
		return;
	}
	uint begin = 0u;
	uint end = parameters.task_count;
	while (begin + 1u < end) {
		uint middle = (begin + end) / 2u;
		if (tasks[middle].geometry_base <= geometry) {
			begin = middle;
		} else {
			end = middle;
		}
	}
	MicroRTTask task = tasks[begin];
	uint mode = task.pad1;
	if (geometry < task.geometry_base || geometry - task.geometry_base >= task.instance_count) {
		return;
	}
	uint ordinal = geometry - task.geometry_base;
	uint offset = task.bitmap_offset + ordinal * task.cluster_count;
	uint group_offset = task.group_offset + ordinal * task.group_count;
	uint count;
	uint stride;
	instances.GetDimensions(count, stride);
	uint instance_slot = uint(task.instance);
	MicroRTTLASInstance output = (MicroRTTLASInstance)0;
	output.custom_index_mask = geometry | ((task.instance_mask & 255u) << 24u);
	output.sbt_offset_flags = geometry | ((task.instance_flags & 255u) << 24u);
	tlas_instances[geometry] = output;
	if (instance_slot == 0u || instance_slot > count || task.clas_addresses == 0 || geometry > 0xffffffu) {
		return;
	}
	MicroGeometryInstance instance = instances[instance_slot - 1u];
	if (instance.handle != task.instance || instance.asset_address == 0 || (instance.multimesh_address != 0 && ordinal >= instance.multimesh_count)) {
		return;
	}
	MicroGeometryAsset asset = *((MicroGeometryAsset*)instance.asset_address);
	if (asset.identity != instance.asset || asset.ready == 0u || task.cluster_count != asset.cluster_count || task.group_count != asset.group_count) {
		return;
	}
	bool prepare = mode == PREPARE_CUT || mode == INITIAL_CUT;
	bool valid = true;
	if (prepare) {
		for (uint cluster_index = 0u; cluster_index < task.cluster_count; cluster_index++) {
			membership[offset + cluster_index] = 0u;
		}
		uint2 bin = bins[task.selection_bin];
		uint selected_count = selection_counts[task.selection_bin];
		valid = selected_count <= bin.y;
		for (uint selected_index = 0u; selected_index < min(selected_count, bin.y); selected_index++) {
			MicroGeometrySelectedCluster record = selected[bin.x + selected_index];
			if (record.instance != task.instance || record.surface != task.surface || record.multimesh_instance != ordinal) {
				continue;
			}
			if (record.asset != instance.asset || record.asset_address != instance.asset_address || record.bin != task.selection_bin || record.cluster >= task.cluster_count) {
				valid = false;
				continue;
			}
			MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[record.cluster];
			if (cluster.page >= asset.page_count || cluster.group >= task.group_count) {
				valid = false;
				continue;
			}
			MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
			if ((page.ready & 3u) != 3u || page.slot == 0xffffffffu || page.generation == 0u || page.generation != record.page_generation || ((uint64_t*)task.clas_addresses)[record.cluster] == 0) {
				valid = false;
				continue;
			}
			membership[offset + record.cluster] = page.generation;
		}
	}
	uint reference_count = 0u;
	uint active_count = 0u;
	bool rebuild = mode == RESTORE_CUT || mode == INITIAL_CUT;
	bool changed = false;
	for (uint cluster_index = 0u; cluster_index < task.cluster_count; cluster_index++) {
		uint previous = cached_membership[offset + cluster_index];
		uint current = mode == UPDATE_TRANSFORMS || mode == RESTORE_CUT ? previous : (valid ? membership[offset + cluster_index] : 0u);
		if (mode == RESTORE_CUT && current != 0u) {
			MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_index];
			current = ((MicroGeometryPage*)asset.pages)[cluster.page].generation;
		}
		rebuild = rebuild || current != previous;
		changed = changed || ((current != 0u) != (previous != 0u));
		if (prepare) {
			membership[offset + cluster_index] = current;
		}
		if (mode == PUBLISH_CUT || mode == RESTORE_CUT || mode == INITIAL_CUT) {
			cached_membership[offset + cluster_index] = current;
		}
		active_count += (mode == PREPARE_CUT ? previous : current) != 0u ? 1u : 0u;
		if (current != 0u && (prepare || mode == RESTORE_CUT)) {
			references[offset + reference_count] = ((uint64_t*)task.clas_addresses)[cluster_index];
			reference_count++;
			if (prepare) {
				MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_index];
				group_usage[group_offset + cluster.group] = 1u;
			}
		}
	}
	if (prepare && changed) {
		InterlockedOr(dirty_counts[1], 1u);
	}
	float4x4 multimesh = micro_rt_multimesh_transform(instance, ordinal, false);
	micro_rt_transform(instance, multimesh, false, output.transform);
	MicroRTMotion previous_motion;
	micro_rt_transform(instance, micro_rt_multimesh_transform(instance, ordinal, true), true, previous_motion.previous_transform);
	motion[task.motion_base + ordinal] = previous_motion;
	uint flags = task.instance_flags;
	if (determinant((float3x3)multimesh) < 0.0) {
		flags ^= 2u;
	}
	output.custom_index_mask = geometry | ((task.instance_mask & 255u) << 24u);
	output.sbt_offset_flags = geometry | ((flags & 255u) << 24u);
	output.blas_address = active_count != 0u ? blas_addresses[geometry] : uint64_t(0);
	tlas_instances[geometry] = output;
	if ((prepare || mode == RESTORE_CUT) && rebuild && reference_count != 0u) {
		uint destination;
		InterlockedAdd(dirty_counts[0], 1u, destination);
		MicroRTBuildInfo info;
		info.reference_count = reference_count;
		info.reference_stride = 8u;
		info.references = parameters.references_address + uint64_t(offset) * 8u;
		dirty_infos[destination] = info;
		dirty_destinations[destination] = output.blas_address;
	}
}
