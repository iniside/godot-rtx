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
	uint tile_offset;
	uint mode;
	uint scan_offset;
	uint scan_stride;
};

struct MicroRTScanTask {
	uint task;
	uint group_offset;
	uint element_count;
	uint input_offset;
	uint output_offset;
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
	uint work_count;
	uint scan_task_offset;
	uint scan_task_count;
	uint pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<MicroRTTask> tasks;
[[vk::binding(1, 0)]] StructuredBuffer<MicroGeometrySelectedCluster> selected;
[[vk::binding(2, 0)]] StructuredBuffer<uint2> bins;
[[vk::binding(3, 0)]] StructuredBuffer<uint> selection_counts;
[[vk::binding(4, 0)]] StructuredBuffer<MicroGeometryInstance> instances;
[[vk::binding(5, 0)]] StructuredBuffer<uint64_t> blas_addresses;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> membership;
[[vk::binding(7, 0)]] StructuredBuffer<uint> cached_membership;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint64_t> references;
[[vk::binding(9, 0)]] RWStructuredBuffer<MicroRTBuildInfo> dirty_infos;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint64_t> dirty_destinations;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> dirty_counts;
[[vk::binding(12, 0)]] RWStructuredBuffer<MicroRTTLASInstance> tlas_instances;
[[vk::binding(13, 0)]] RWStructuredBuffer<MicroRTMotion> motion;
[[vk::binding(14, 0)]] RWStructuredBuffer<uint> group_usage;
[[vk::binding(15, 0)]] RWStructuredBuffer<uint2> candidate_state;
[[vk::binding(16, 0)]] StructuredBuffer<uint2> committed_state;
[[vk::binding(17, 0)]] RWStructuredBuffer<uint> scan_data;
[[vk::binding(18, 0)]] StructuredBuffer<MicroRTScanTask> scan_tasks;
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

static const uint TILE_SIZE = 256u;
static const uint INVALID_CUT = 1u;
static const uint REBUILD_CUT = 2u;

groupshared uint tile_invalid;
groupshared uint tile_values[TILE_SIZE];
groupshared uint tile_flags[TILE_SIZE];
groupshared uint tile_triangles[TILE_SIZE];

uint micro_rt_prefix(uint p_value, uint p_lane) {
	tile_values[p_lane] = p_value;
	GroupMemoryBarrierWithGroupSync();
	for (uint step = 1u; step < TILE_SIZE; step <<= 1u) {
		uint value = p_lane >= step ? tile_values[p_lane - step] : 0u;
		GroupMemoryBarrierWithGroupSync();
		tile_values[p_lane] += value;
		GroupMemoryBarrierWithGroupSync();
	}
	return tile_values[p_lane] - p_value;
}

bool micro_rt_instance(MicroRTTask p_task, uint p_ordinal, out MicroGeometryInstance r_instance, out MicroGeometryAsset r_asset) {
	r_instance = (MicroGeometryInstance)0;
	r_asset = (MicroGeometryAsset)0;
	uint count;
	uint stride;
	instances.GetDimensions(count, stride);
	uint slot = uint(p_task.instance);
	if (slot == 0u || slot > count || p_task.clas_addresses == 0 || p_task.geometry_base + p_ordinal > 0xffffffu) {
		return false;
	}
	r_instance = instances[slot - 1u];
	if (r_instance.handle != p_task.instance || r_instance.asset_address == 0 || (r_instance.multimesh_address != 0 && p_ordinal >= r_instance.multimesh_count)) {
		return false;
	}
	r_asset = *((MicroGeometryAsset*)r_instance.asset_address);
	return r_asset.identity == r_instance.asset && r_asset.ready != 0u && p_task.cluster_count == r_asset.cluster_count && p_task.group_count == r_asset.group_count;
}

[numthreads(256, 1, 1)]
void main(uint3 p_group : SV_GroupID, uint p_lane : SV_GroupIndex) {
	uint group = p_group.x + p_group.y * 65535u;
#if defined(PHASE_SCATTER) || defined(PHASE_FINALIZE)
	uint work = group * TILE_SIZE + p_lane;
#else
	uint work = group;
#endif
	if (work >= parameters.work_count || parameters.task_count == 0u) {
		return;
	}
#if defined(PHASE_SCAN) || defined(PHASE_ADD_OFFSETS)
	uint begin = parameters.scan_task_offset;
	uint end = begin + parameters.scan_task_count;
	while (begin + 1u < end) {
		uint middle = (begin + end) / 2u;
		if (scan_tasks[middle].group_offset <= work) {
			begin = middle;
		} else {
			end = middle;
		}
	}
	MicroRTScanTask scan = scan_tasks[begin];
	MicroRTTask task = tasks[scan.task];
	uint blocks = (scan.element_count + TILE_SIZE - 1u) / TILE_SIZE;
	uint ordinal = (work - scan.group_offset) / blocks;
	uint block = (work - scan.group_offset) % blocks;
	uint element = block * TILE_SIZE + p_lane;
	uint offset = task.scan_offset + ordinal * task.scan_stride;
#ifdef PHASE_SCAN
	uint value = element < scan.element_count ? scan_data[offset + scan.input_offset + element] : 0u;
	uint prefix = micro_rt_prefix(value, p_lane);
	if (element < scan.element_count) {
		scan_data[offset + scan.input_offset + element] = prefix;
	}
	if (p_lane == TILE_SIZE - 1u && blocks > 1u) {
		scan_data[offset + scan.output_offset + block] = prefix + value;
	}
#else
	if (element < scan.element_count && blocks > 1u) {
		scan_data[offset + scan.input_offset + element] += scan_data[offset + scan.output_offset + block];
	}
#endif
#else
	uint begin = 0u;
	uint end = parameters.task_count;
	while (begin + 1u < end) {
		uint middle = (begin + end) / 2u;
#ifdef PHASE_SCATTER
		uint base = bins[tasks[middle].selection_bin].x;
#elif defined(PHASE_FINALIZE)
		uint base = tasks[middle].geometry_base;
#else
		uint base = tasks[middle].tile_offset;
#endif
		if (base <= work) {
			begin = middle;
		} else {
			end = middle;
		}
	}
	MicroRTTask task = tasks[begin];
#ifdef PHASE_SCATTER
	uint2 bin = bins[task.selection_bin];
	uint selected_count = selection_counts[task.selection_bin];
	if (selected_count > bin.y || work < bin.x || work - bin.x >= selected_count) {
		return;
	}
	MicroGeometrySelectedCluster record = selected[work];
	uint ordinal = record.multimesh_instance;
	if (record.instance != task.instance || record.surface != task.surface || ordinal >= task.instance_count) {
		return;
	}
	uint geometry = task.geometry_base + ordinal;
	MicroGeometryInstance instance;
	MicroGeometryAsset asset;
	bool valid = micro_rt_instance(task, ordinal, instance, asset);
	valid = valid && record.asset == instance.asset && record.asset_address == instance.asset_address && record.bin == task.selection_bin && record.cluster < task.cluster_count;
	uint generation = 0u;
	if (valid) {
		MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[record.cluster];
		valid = cluster.page < asset.page_count && cluster.group < task.group_count;
		if (valid) {
			MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
			generation = page.generation;
			valid = (page.ready & 3u) == 3u && page.slot != 0xffffffffu && generation != 0u && generation == record.page_generation && ((uint64_t*)task.clas_addresses)[record.cluster] != 0;
		}
	}
	if (!valid) {
		InterlockedOr(candidate_state[geometry].y, INVALID_CUT);
	} else {
		InterlockedExchange(membership[task.bitmap_offset + ordinal * task.cluster_count + record.cluster], generation);
	}
#elif defined(PHASE_FINALIZE)
	uint geometry = work;
	if (geometry < task.geometry_base || geometry - task.geometry_base >= task.instance_count) {
		return;
	}
	uint ordinal = geometry - task.geometry_base;
	MicroRTTLASInstance output = (MicroRTTLASInstance)0;
	output.custom_index_mask = geometry | ((task.instance_mask & 255u) << 24u);
	output.sbt_offset_flags = geometry | ((task.instance_flags & 255u) << 24u);
	tlas_instances[geometry] = output;
	MicroGeometryInstance instance;
	MicroGeometryAsset asset;
	if (!micro_rt_instance(task, ordinal, instance, asset)) {
		return;
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
	output.sbt_offset_flags = geometry | ((flags & 255u) << 24u);
	uint active_count = task.mode == PREPARE_CUT || task.mode == UPDATE_TRANSFORMS ? committed_state[geometry].x : candidate_state[geometry].x;
	output.blas_address = active_count != 0u ? blas_addresses[geometry] : uint64_t(0);
	tlas_instances[geometry] = output;
	if (task.mode != UPDATE_TRANSFORMS && task.mode != PUBLISH_CUT && candidate_state[geometry].x != 0u && (candidate_state[geometry].y & REBUILD_CUT) != 0u) {
		uint destination;
		InterlockedAdd(dirty_counts[0], 1u, destination);
		MicroRTBuildInfo info;
		info.reference_count = candidate_state[geometry].x;
		info.reference_stride = 8u;
		info.references = parameters.references_address + uint64_t(task.bitmap_offset + ordinal * task.cluster_count) * 8u;
		dirty_infos[destination] = info;
		dirty_destinations[destination] = blas_addresses[geometry];
	}
#else
	uint tile_count = (task.cluster_count + TILE_SIZE - 1u) / TILE_SIZE;
	uint ordinal = (work - task.tile_offset) / tile_count;
	uint tile = (work - task.tile_offset) % tile_count;
	uint cluster_index = tile * TILE_SIZE + p_lane;
	uint geometry = task.geometry_base + ordinal;
	uint offset = task.bitmap_offset + ordinal * task.cluster_count;
#ifdef PHASE_RESET
	if (cluster_index < task.cluster_count) {
		membership[offset + cluster_index] = 0u;
	}
	if (tile == 0u && p_lane == 0u) {
		MicroGeometryInstance instance;
		MicroGeometryAsset asset;
		bool valid = micro_rt_instance(task, ordinal, instance, asset);
		if (task.mode != RESTORE_CUT) {
			valid = valid && selection_counts[task.selection_bin] <= bins[task.selection_bin].y;
		}
		candidate_state[geometry] = uint2(0u, valid ? 0u : INVALID_CUT);
	}
#elif defined(PHASE_COMPARE)
	if (p_lane == 0u) {
		InterlockedOr(candidate_state[geometry].y, 0u, tile_invalid);
	}
	GroupMemoryBarrierWithGroupSync();
	uint previous = 0u;
	uint current = 0u;
	uint triangles = 0u;
	if (cluster_index < task.cluster_count) {
		previous = cached_membership[offset + cluster_index];
		current = (tile_invalid & INVALID_CUT) != 0u ? 0u : (task.mode == RESTORE_CUT ? previous : membership[offset + cluster_index]);
		if (current != 0u) {
			MicroGeometryInstance instance = instances[uint(task.instance) - 1u];
			MicroGeometryAsset asset = *((MicroGeometryAsset*)instance.asset_address);
			MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_index];
			if (task.mode == RESTORE_CUT) {
				current = ((MicroGeometryPage*)asset.pages)[cluster.page].generation;
			}
			triangles = current != 0u ? cluster.triangle_count : 0u;
		}
		membership[offset + cluster_index] = current;
	}
	tile_values[p_lane] = current != 0u ? 1u : 0u;
	tile_flags[p_lane] = (current != previous ? REBUILD_CUT : 0u) | ((current != 0u) != (previous != 0u) ? 4u : 0u);
	tile_triangles[p_lane] = triangles;
	GroupMemoryBarrierWithGroupSync();
	for (uint step = TILE_SIZE / 2u; step != 0u; step >>= 1u) {
		if (p_lane < step) {
			tile_values[p_lane] += tile_values[p_lane + step];
			tile_flags[p_lane] |= tile_flags[p_lane + step];
			tile_triangles[p_lane] += tile_triangles[p_lane + step];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (p_lane == 0u) {
		scan_data[task.scan_offset + ordinal * task.scan_stride + tile] = tile_values[0];
		InterlockedAdd(candidate_state[geometry].x, tile_values[0]);
		uint flags = tile_flags[0] & REBUILD_CUT;
		if (task.mode == INITIAL_CUT || task.mode == RESTORE_CUT) {
			flags |= REBUILD_CUT;
		}
		InterlockedOr(candidate_state[geometry].y, flags);
		if (task.mode != RESTORE_CUT) {
			InterlockedOr(dirty_counts[1], (tile_flags[0] & 4u) != 0u ? 1u : 0u);
			InterlockedAdd(dirty_counts[2], tile_values[0]);
			InterlockedAdd(dirty_counts[3], tile_triangles[0]);
		}
	}
#elif defined(PHASE_EMIT)
	uint current = cluster_index < task.cluster_count ? membership[offset + cluster_index] : 0u;
	uint prefix = micro_rt_prefix(current != 0u ? 1u : 0u, p_lane);
	if (current != 0u) {
		uint tile_offset = scan_data[task.scan_offset + ordinal * task.scan_stride + tile];
		references[offset + tile_offset + prefix] = ((uint64_t*)task.clas_addresses)[cluster_index];
		if (task.mode != RESTORE_CUT) {
			MicroGeometryInstance instance = instances[uint(task.instance) - 1u];
			MicroGeometryAsset asset = *((MicroGeometryAsset*)instance.asset_address);
			MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_index];
			InterlockedOr(group_usage[task.group_offset + ordinal * task.group_count + cluster.group], 1u);
		}
	}
#endif
#endif
#endif
}
