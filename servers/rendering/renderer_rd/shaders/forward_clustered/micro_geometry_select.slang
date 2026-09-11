#[compute]
#VERSION_DEFINES
#include "micro_geometry_inc.slang"

struct SelectionTask {
	uint64_t instance;
	uint64_t surface;
	uint group_count;
	uint cluster_count;
	uint coarse_count;
	uint multimesh_count;
	uint bin;
	uint flags;
	uint gi_offset;
	uint pad;
	uint64_t asset;
	uint64_t indirect_command;
};
struct SelectionBin { uint offset; uint capacity; };
struct SelectionUnit {
	uint task;
	uint ordinal;
	uint queue_offset;
	uint queue_capacity;
	uint record_offset;
	uint record_capacity;
	uint hash_offset;
	uint pad;
};
struct SelectionState {
	uint queue_count;
	uint candidate_count;
	uint committed_count;
	uint status;
	uint rejected_count;
	uint published_frontier_count;
	uint padding[2];
};
struct SelectionParameters {
	float projection[16];
	float previous_projection[16];
	float view_rotation[12];
	float previous_view_rotation[12];
	float camera[4];
	float camera_low[4];
	float previous_camera[4];
	float previous_camera_low[4];
	uint64_t scenario;
	uint layer_mask;
	uint task_count;
	uint queue_work;
	uint record_work;
	uint unit_count;
	uint bin_count;
	float error;
	float offscreen_multiplier;
	float near_plane;
	float output_height;
	uint flags;
	uint hzb_width;
	uint hzb_height;
	uint hzb_mips;
	uint64_t lightmaps[8];
	uint lightmap_sh;
	uint cull_plane_count;
	float cull_planes[96];
};
struct RasterInstance {
	float transform[12];
	float compressed_aabb_position[4];
	float compressed_aabb_size[4];
	float uv_scale[4];
	uint flags;
	uint instance_uniforms_ofs;
	uint gi_offset;
	uint layer_mask;
	uint rtxdi_material_flags;
	uint padding[3];
	float previous_transform[12];
	float lightmap_uv_scale[4];
	float model_precision[4];
	float previous_model_precision[4];
};
struct MeshDispatchArguments {
	uint x;
	uint y;
	uint z;
};
struct PushConstant {
	uint mode;
	uint level;
};
[[vk::push_constant]] ConstantBuffer<PushConstant> push;
[[vk::binding(0, 0)]] StructuredBuffer<SelectionParameters> parameter_buffer;
#define parameters parameter_buffer[0]
[[vk::binding(1, 0)]] StructuredBuffer<SelectionTask> tasks;
[[vk::binding(2, 0)]] StructuredBuffer<SelectionBin> bins;
[[vk::binding(3, 0)]] StructuredBuffer<MicroGeometryInstance> instances;
[[vk::binding(4, 0)]] StructuredBuffer<MicroGeometryInstanceSurface> surfaces;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint2> sparse_states;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint2> rejected;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> validity;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> counts;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> initial_counts;
[[vk::binding(10, 0)]] RWStructuredBuffer<SelectionState> unit_states;
[[vk::binding(11, 0)]] RWStructuredBuffer<MeshDispatchArguments> dispatch_arguments;
[[vk::binding(12, 0)]] RWStructuredBuffer<MicroGeometrySelectedCluster> selected;
[[vk::binding(13, 0)]] RWStructuredBuffer<RasterInstance> raster_instances;
[[vk::binding(14, 0)]] RWByteAddressBuffer requests;
[[vk::binding(15, 0)]] Texture2D<float> hzb;
[[vk::binding(16, 0)]] RWStructuredBuffer<uint> statistics;

[[vk::binding(17, 0)]] StructuredBuffer<SelectionUnit> units;
[[vk::binding(18, 0)]] RWStructuredBuffer<uint> frontier;
[[vk::binding(19, 0)]] RWStructuredBuffer<uint2> candidate;
[[vk::binding(20, 0)]] RWStructuredBuffer<uint2> committed;
[[vk::binding(21, 0)]] StructuredBuffer<uint> record_order;
[[vk::binding(22, 0)]] StructuredBuffer<uint> queue_order;

void add_statistic(uint p_index, uint p_value = 1) {
	if ((parameters.flags & 128) != 0) { InterlockedAdd(statistics[p_index], p_value); }
}

uint unit_for_work(uint p_index, bool p_queue) {
	uint begin = 0, end = parameters.unit_count;
	while (begin + 1 < end) {
		uint middle = (begin + end) / 2;
		SelectionUnit unit = units[p_queue ? queue_order[middle] : record_order[middle]];
		uint offset = p_queue ? unit.queue_offset : unit.record_offset;
		if (offset <= p_index) { begin = middle; } else { end = middle; }
	}
	return p_queue ? queue_order[begin] : record_order[begin];
}

bool unit_deferred(uint p_unit_index) {
	return (units[p_unit_index].pad & 1) != 0 && (unit_states[p_unit_index].status & 8) == 0;
}

uint find_group(SelectionUnit p_unit, uint p_group) {
	uint count = p_unit.queue_capacity * 2;
	uint first = (p_group * 0x9e3779b9u) % count;
	for (uint probe = 0; probe < count; probe++) {
		uint slot = p_unit.hash_offset + (first + probe) % count;
		uint key = sparse_states[slot].x;
		if (key == p_group + 1) { return slot; }
		if (key == 0) { break; }
	}
	return 0xffffffff;
}

void enqueue_group(uint p_unit_index, uint p_group) {
	SelectionUnit unit = units[p_unit_index];
	uint count = unit.queue_capacity * 2;
	uint first = (p_group * 0x9e3779b9u) % count;
	for (uint probe = 0; probe < count; probe++) {
		uint slot = unit.hash_offset + (first + probe) % count;
		uint previous;
		InterlockedCompareExchange(sparse_states[slot].x, 0, p_group + 1, previous);
		if (previous == p_group + 1) { return; }
		if (previous != 0) { continue; }
		uint index;
		InterlockedAdd(unit_states[p_unit_index].queue_count, 1, index);
		if (index < unit.queue_capacity) {
			frontier[unit.queue_offset + index] = slot;
		} else {
			InterlockedOr(unit_states[p_unit_index].status, 1);
		}
		return;
	}
	InterlockedOr(unit_states[p_unit_index].status, 1);
	InterlockedMax(unit_states[p_unit_index].queue_count, unit.queue_capacity + 1);
}

bool resolve_multimesh(SelectionTask p_task, uint p_ordinal, out uint r_instance) {
	r_instance = p_ordinal;
	if (p_task.indirect_command == 0) { return p_ordinal < p_task.multimesh_count; }
	uint* command = (uint*)p_task.indirect_command;
	if (p_ordinal >= min(command[1], p_task.multimesh_count) || command[4] >= p_task.multimesh_count - p_ordinal) { return false; }
	r_instance += command[4];
	return true;
}

float3 transform_point(float transform[12], float3 p_point) {
	return float3(dot(float3(transform[0], transform[1], transform[2]), p_point) + transform[3], dot(float3(transform[4], transform[5], transform[6]), p_point) + transform[7], dot(float3(transform[8], transform[9], transform[10]), p_point) + transform[11]);
}

float transform_scale_bound(float3 p_row0, float3 p_row1, float3 p_row2) {
	float3 column0 = float3(p_row0.x, p_row1.x, p_row2.x);
	float3 column1 = float3(p_row0.y, p_row1.y, p_row2.y);
	float3 column2 = float3(p_row0.z, p_row1.z, p_row2.z);
	float3 diagonal = float3(dot(column0, column0), dot(column1, column1), dot(column2, column2));
	float3 off_diagonal = abs(float3(dot(column0, column1), dot(column0, column2), dot(column1, column2)));
	float3 row_sums = diagonal + float3(off_diagonal.x + off_diagonal.y, off_diagonal.x + off_diagonal.z, off_diagonal.y + off_diagonal.z);
	return sqrt(min(diagonal.x + diagonal.y + diagonal.z, max(row_sums.x, max(row_sums.y, row_sums.z)))) * 1.000001;
}

void view_bounds(SelectionTask p_task, MicroGeometryInstance p_instance, uint p_multimesh, float p_center[3], float p_radius, bool p_previous_camera, out float3 r_center, out float r_radius) {
	float3 local = float3(p_center[0], p_center[1], p_center[2]);
	float scale = 1;
	if (p_instance.multimesh_address != 0 && p_multimesh != 0xffffffff) {
		float* data = (float*)(p_instance.multimesh_address + uint64_t(p_instance.multimesh_stride) * (p_multimesh + p_instance.multimesh_current_offset) * 4);
		float3 rows[3];
		rows[0] = float3(data[0], data[1], data[2]);
		rows[1] = float3(data[4], data[5], data[6]);
		rows[2] = (p_instance.flags & (1 << 13)) != 0 ? float3(0, 0, 1) : float3(data[8], data[9], data[10]);
		local = float3(dot(rows[0], local) + data[3], dot(rows[1], local) + data[7], dot(rows[2], local) + ((p_instance.flags & (1 << 13)) != 0 ? 0 : data[11]));
		scale = transform_scale_bound(rows[0], rows[1], rows[2]);
	}
	float3 world;
	if ((p_task.flags & 1) != 0) {
		world = local;
		for (uint axis = 0; axis < 3; axis++) {
			world[axis] -= (p_previous_camera ? parameters.previous_camera[axis] : parameters.camera[axis]) + (p_previous_camera ? parameters.previous_camera_low[axis] : parameters.camera_low[axis]);
		}
	} else {
		float3 row0 = float3(p_instance.transform[0], p_instance.transform[1], p_instance.transform[2]);
		float3 row1 = float3(p_instance.transform[4], p_instance.transform[5], p_instance.transform[6]);
		float3 row2 = float3(p_instance.transform[8], p_instance.transform[9], p_instance.transform[10]);
		world = float3(dot(row0, local), dot(row1, local), dot(row2, local));
		scale *= transform_scale_bound(row0, row1, row2);
		for (uint axis = 0; axis < 3; axis++) {
			world[axis] += (p_instance.transform[axis * 4 + 3] - (p_previous_camera ? parameters.previous_camera[axis] : parameters.camera[axis])) + (p_instance.origin_low[axis] - (p_previous_camera ? parameters.previous_camera_low[axis] : parameters.camera_low[axis]));
		}
	}
	r_center = transform_point(p_previous_camera ? parameters.previous_view_rotation : parameters.view_rotation, world);
	r_radius = p_radius * scale;
}

float4 project_point(float3 p_point, bool p_previous) {
	float4 result;
	for (uint row = 0; row < 4; row++) {
		result[row] = p_previous ? parameters.previous_projection[row] * p_point.x + parameters.previous_projection[4 + row] * p_point.y + parameters.previous_projection[8 + row] * p_point.z + parameters.previous_projection[12 + row] : parameters.projection[row] * p_point.x + parameters.projection[4 + row] * p_point.y + parameters.projection[8 + row] * p_point.z + parameters.projection[12 + row];
	}
	return result;
}

bool projected_bounds(float3 p_center, float p_radius, bool p_previous, out float2 r_min, out float2 r_max, out float r_nearest) {
	r_min = float2(1);
	r_max = float2(0);
	r_nearest = 1;
	if (p_center.z + p_radius >= -parameters.near_plane) { return false; }
	r_min = float2(1e30);
	r_max = float2(-1e30);
	r_nearest = -1e30;
	for (uint corner = 0; corner < 8; corner++) {
		float3 offset = float3((corner & 1) != 0 ? 1 : -1, (corner & 2) != 0 ? 1 : -1, (corner & 4) != 0 ? 1 : -1) * p_radius;
		float4 clip = project_point(p_center + offset, p_previous);
		if (clip.w <= 0) { return false; }
		float3 ndc = clip.xyz / clip.w;
		float2 uv = ndc.xy * 0.5 + 0.5;
		r_min = min(r_min, uv);
		r_max = max(r_max, uv);
		r_nearest = max(r_nearest, ndc.z);
	}
	return true;
}

bool frustum_visible(SelectionTask p_task, MicroGeometryInstance p_instance, uint p_multimesh, float p_center[3], float p_radius) {
	if ((parameters.flags & 2) == 0) { return true; }
	float3 center;
	float radius, nearest;
	float2 low, high;
	view_bounds(p_task, p_instance, p_multimesh, p_center, p_radius, false, center, radius);
	if (parameters.cull_plane_count != 0) {
		for (uint index = 0; index < parameters.cull_plane_count; index++) {
			float3 normal = float3(parameters.cull_planes[index * 4], parameters.cull_planes[index * 4 + 1], parameters.cull_planes[index * 4 + 2]);
			if (dot(normal, center) - parameters.cull_planes[index * 4 + 3] > radius) { return false; }
		}
		return true;
	}
	if ((p_task.flags & 2) != 0) { return true; }
	if ((parameters.flags & 32) != 0 && center.z - radius >= 0) { return false; }
	if (!projected_bounds(center, radius, false, low, high, nearest)) { return true; }
	return all(high >= 0) && all(low <= 1) && nearest >= 0;
}

bool occluded(SelectionTask p_task, MicroGeometryInstance p_instance, uint p_multimesh, MicroGeometryCluster p_cluster, bool p_previous) {
	uint statistic_offset = p_previous ? 4 : 16;
	if ((parameters.flags & 4) == 0 || parameters.hzb_mips == 0 || parameters.hzb_width == 0 || parameters.hzb_height == 0) { add_statistic(statistic_offset + 2); return false; }
	if ((p_task.flags & 18) != 0) { add_statistic(statistic_offset + 3); return false; }
	float3 center;
	float radius, nearest;
	float2 low, high;
	view_bounds(p_task, p_instance, p_multimesh, p_cluster.center, p_cluster.radius, p_previous, center, radius);
	if (!projected_bounds(center, radius, p_previous, low, high, nearest) || !all(isfinite(low)) || !all(isfinite(high)) || !isfinite(nearest)) { add_statistic(statistic_offset + 4); return false; }
	if (p_previous && (any(low < 0) || any(high > 1))) { add_statistic(statistic_offset + 5); return false; }
	low = max(low, float2(0));
	high = min(high, float2(1));
	if (any(low >= high)) { add_statistic(statistic_offset + 4); return false; }
	float2 pixel_low = low * float2(parameters.hzb_width, parameters.hzb_height);
	float2 pixel_high = high * float2(parameters.hzb_width, parameters.hzb_height);
	uint2 rectangle_low = uint2(floor(pixel_low));
	uint2 rectangle_end = uint2(ceil(pixel_high));
	if (any(rectangle_low >= rectangle_end)) { add_statistic(statistic_offset + 4); return false; }
	uint2 rectangle_high = rectangle_end - 1;
	uint2 extent = rectangle_end - rectangle_low;
	uint root_level = min(uint(ceil(log2(float(max(extent.x, extent.y))))), parameters.hzb_mips - 1);
	uint2 start = rectangle_low >> root_level;
	uint2 finish = rectangle_high >> root_level;
	uint width, height, levels;
	hzb.GetDimensions(root_level, width, height, levels);
	if (any(finish >= uint2(width, height)) || any(finish - start > 1)) { add_statistic(statistic_offset + 4); return false; }
	uint query_offset = p_previous ? 68 : 76;
	uint2 node = start;
	uint level = root_level;
	uint fetches = 0;
	uint boundary_descents = 0;
	bool advance = false;
	bool complete = false;
	bool hidden = false;
	bool zero_depth = false;
	bool zero_padding = false;
	uint cancellation_index = (p_previous ? parameters.previous_projection[15] : parameters.projection[15]) == 0.0 ? 10 : 14;
	float cancellation_term = abs(p_previous ? parameters.previous_projection[cancellation_index] : parameters.projection[cancellation_index]);
	float threshold = max(nearest, 0.0) + 32.0 * 1.1920929e-7 * (abs(nearest) + 2.0 * cancellation_term);
	for (uint step = 0; step < 128; step++) {
		if (advance) {
			if (level == root_level) {
				if (node.x < finish.x) {
					node.x++;
				} else if (node.y < finish.y) {
					node = uint2(start.x, node.y + 1);
				} else {
					complete = true;
					hidden = true;
					break;
				}
			} else {
				uint2 parent = node >> 1;
				uint2 sibling_start = max(parent << 1, rectangle_low >> level);
				uint2 sibling_finish = min((parent << 1) + 1, rectangle_high >> level);
				if (node.x < sibling_finish.x) {
					node.x++;
				} else if (node.y < sibling_finish.y) {
					node = uint2(sibling_start.x, node.y + 1);
				} else {
					node = parent;
					level++;
					continue;
				}
			}
			advance = false;
			continue;
		}
		if (fetches == 32) { break; }
		float depth = hzb.Load(int3(node, level));
		fetches++;
		uint2 node_low = node << level;
		uint2 node_end = (node + 1) << level;
		if (depth == 0) {
			zero_depth = true;
			zero_padding = zero_padding || any(node_end > uint2(parameters.hzb_width, parameters.hzb_height));
		}
		if (threshold < depth) {
			advance = true;
		} else if (all(node_low >= rectangle_low) && all(node_end <= rectangle_end)) {
			add_statistic(query_offset + (depth == 0 ? 2 : 3));
			complete = true;
			break;
		} else if (level == 0) {
			complete = true;
			break;
		} else {
			level--;
			node = max(node << 1, rectangle_low >> level);
			boundary_descents++;
		}
	}
	add_statistic(statistic_offset + 6);
	if (zero_depth) {
		add_statistic(statistic_offset + 7);
		if (zero_padding) { add_statistic(p_previous ? 37 : 38); }
	}
	if (boundary_descents != 0) {
		add_statistic(query_offset);
		if (hidden) { add_statistic(query_offset + 1); }
	}
	if (!complete) { add_statistic(query_offset + 4); }
	add_statistic(query_offset + 5, fetches);
	if ((parameters.flags & 128) != 0) { InterlockedMax(statistics[query_offset + 6], fetches); }
	add_statistic(query_offset + 7, boundary_descents);
	return hidden;
}

void request_group(uint64_t p_asset, uint p_group) {
	uint index;
	requests.InterlockedAdd(0, 1, index);
	if (index < requests.Load(8)) {
		uint address = 16 + index * 16;
		requests.Store4(address, uint4(uint(p_asset), uint(p_asset >> 32), p_group, 0));
	} else {
		requests.InterlockedOr(4, 1);
	}
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	uint index = id.x + id.y * 65536u;
	if (push.mode == 12) {
		if (index < parameters.bin_count) { counts[index] += initial_counts[index]; }
		return;
	}
	if (push.mode == 13) {
		if (index >= parameters.bin_count) { return; }
		uint count = min(counts[index], bins[index].capacity);
		MeshDispatchArguments arguments;
		arguments.x = min(count, push.level);
		arguments.y = (count + push.level - 1) / push.level;
		arguments.z = 1;
		dispatch_arguments[index] = arguments;
		return;
	}
	if (push.mode == 0) {
		if (index >= parameters.task_count) { return; }
		if (index == 0 && (parameters.flags & 128) != 0) {
			statistics[2] = 1;
			statistics[3] = parameters.flags;
		}
		SelectionTask task = tasks[index];
		MicroGeometryInstance instance = instances[uint(task.instance) - 1];
		MicroGeometryInstanceSurface surface = surfaces[uint(task.surface) - 1];
		bool valid = instance.handle == task.instance && surface.handle == task.surface && surface.instance == task.instance && instance.asset == task.asset && instance.asset_address != 0 && instance.deformed == 0;
		if ((parameters.flags & 1) != 0) { valid = valid && instance.scenario == parameters.scenario && instance.visible != 0 && (instance.layer_mask & parameters.layer_mask) != 0 && ((parameters.flags & 64) != 0 ? instance.shadows != 0 : ((parameters.flags & 32) != 0 || instance.shadows != 3)); }
		if (valid) {
			MicroGeometryAsset asset = *((MicroGeometryAsset*)instance.asset_address);
			valid = asset.identity == instance.asset && (asset.ready & 1) != 0 && asset.group_count == task.group_count && asset.cluster_count == task.cluster_count;
		}
		validity[index] = valid ? 1 : 0;
		RasterInstance raster = {};
		for (uint element = 0; element < 12; element++) {
			raster.transform[element] = (task.flags & 1) != 0 ? (element == 0 || element == 5 || element == 10 ? 1 : 0) : instance.transform[element];
			raster.previous_transform[element] = (task.flags & 1) != 0 ? raster.transform[element] : instance.previous_transform[element];
		}
		for (uint axis = 0; axis < 3; axis++) { raster.compressed_aabb_size[axis] = 1; }
		raster.flags = (instance.flags | task.flags | (1 << 4) | 0xff000000) & ~3u;
		raster.flags &= ~(1u << 3);
		for (uint element = 0; element < 12; element++) {
			if (raster.transform[element] != raster.previous_transform[element]) { raster.flags |= 1u << 3; }
		}
		for (uint axis = 0; axis < 3; axis++) {
			if (instance.origin_low[axis] != instance.previous_origin_low[axis]) { raster.flags |= 1u << 3; }
		}
		if (instance.multimesh_address != 0) { raster.flags |= 1u << 3; }
		raster.instance_uniforms_ofs = instance.instance_uniforms_offset;
		raster.gi_offset = task.gi_offset;
		raster.layer_mask = instance.layer_mask;
		raster.rtxdi_material_flags = surface.material_flags;
		for (uint element = 0; element < 4; element++) {
			raster.lightmap_uv_scale[element] = instance.lightmap_uv_scale[element];
			raster.model_precision[element] = (task.flags & 1) != 0 ? 0 : instance.origin_low[element];
			raster.previous_model_precision[element] = (task.flags & 1) != 0 ? 0 : instance.previous_origin_low[element];
		}
		for (uint lightmap = 0; lightmap < 8; lightmap++) {
			if (instance.lightmap != 0 && instance.lightmap == parameters.lightmaps[lightmap]) {
				raster.gi_offset = (instance.lightmap_slice << 16) | lightmap;
				raster.flags |= (1 << 8) | (((parameters.lightmap_sh >> lightmap) & 1) << 9);
			}
		}
		raster_instances[index] = raster;
		return;
	}
	if (push.mode == 1) {
		if (index >= parameters.unit_count) { return; }
		uint previous_count = unit_states[index].committed_count;
		SelectionState state = {};
		state.committed_count = previous_count;
		state.status = previous_count == 0 ? 8 : 0;
		unit_states[index] = state;
		return;
	}
	if (push.mode == 14) {
		if (index < parameters.unit_count) { unit_states[index].published_frontier_count = min(unit_states[index].queue_count, units[index].queue_capacity); }
		return;
	}
	if (push.mode == 3 || push.mode == 9) {
		if (index >= parameters.unit_count) { return; }
		if (unit_deferred(index)) { return; }
		SelectionUnit unit = units[index];
		SelectionTask task = tasks[unit.task];
		uint source_multimesh;
		if (validity[unit.task] == 0 || !resolve_multimesh(task, unit.ordinal, source_multimesh)) {
			unit_states[index].committed_count = 0;
			return;
		}
		if (push.mode == 9) {
			if ((unit_states[index].status & 7) == 0) {
				unit_states[index].committed_count = unit_states[index].candidate_count;
			} else {
				add_statistic(30);
			}
			add_statistic(28, unit_states[index].candidate_count);
			add_statistic(29, unit_states[index].committed_count);
			add_statistic(31);
			if ((parameters.flags & 128) != 0 && surfaces[uint(task.surface) - 1].force_finest != 0) { add_statistic(35); }
			return;
		}
		MicroGeometryInstance instance = instances[uint(task.instance) - 1];
		MicroGeometryInstanceSurface surface = surfaces[uint(task.surface) - 1];
		MicroGeometryAsset asset = *((MicroGeometryAsset*)instance.asset_address);
		if ((parameters.flags & 32) == 0) {
			float center[3];
			for (uint axis = 0; axis < 3; axis++) { center[axis] = instance.aabb_position[axis] + instance.aabb_size[axis] * 0.5; }
			float radius = length(float3(instance.aabb_size[0], instance.aabb_size[1], instance.aabb_size[2])) * 0.5;
			if ((parameters.flags & 128) != 0) {
				float3 view_center;
				float view_radius;
				view_bounds(task, instance, source_multimesh, center, radius, false, view_center, view_radius);
				bool perspective = parameters.projection[15] == 0;
				float distance = -view_center.z - view_radius;
				float diameter = view_radius * abs(parameters.projection[5]) * parameters.output_height / (perspective ? max(0.00001, distance) : 1);
				uint bucket = diameter <= 16 ? 0 : diameter <= 32 ? 1 : diameter <= 64 ? 2 : diameter <= 128 ? 3 : diameter <= 256 ? 4 : 5;
				unit_states[index].padding[0] = bucket;
				add_statistic(40 + bucket * 4);
				if (perspective && distance <= parameters.near_plane) { add_statistic(64); }
				if (perspective && distance <= 0.00001) { add_statistic(65); }
			}
			if (!frustum_visible(task, instance, source_multimesh, center, radius)) {
				unit_states[index].committed_count = 0;
				return;
			}
		}

		bool bootstrap = (unit_states[index].status & 8) != 0;
		bool finest = (parameters.flags & 32) != 0 && surface.force_finest != 0;
		if (bootstrap) { unit_states[index].committed_count = 0; }
		for (uint terminal = 0; terminal < asset.terminal_count; terminal++) {
			uint group_id = ((uint*)asset.terminals)[terminal];
			MicroGeometryGroup group = ((MicroGeometryGroup*)asset.groups)[group_id];
			enqueue_group(index, group_id);
			if (!bootstrap || finest) { continue; }
			for (uint local = 0; local < group.cluster_count; local++) {
				uint cluster_id = group.first_cluster + local;
				MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_id];
				if (((MicroGeometrySurface*)asset.surfaces)[cluster.surface].source_surface != surface.source_surface) { continue; }
				MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
				if ((page.ready & 1) == 0) { continue; }
				uint destination = unit_states[index].committed_count++;
				if (destination < unit.record_capacity) { committed[unit.record_offset + destination] = uint2(cluster_id, page.generation); }
			}
		}
		return;
	}
	bool queue_work = push.mode == 4 || push.mode == 5;
	if (index >= (queue_work ? parameters.queue_work : parameters.record_work)) { return; }
	uint unit_index = unit_for_work(index, queue_work);
	SelectionUnit unit = units[unit_index];
	uint relative = index - (queue_work ? unit.queue_offset : unit.record_offset);
	if (push.mode != 2 && unit_deferred(unit_index)) { return; }
	if (queue_work) {
		uint published_count = push.mode == 4 ? unit_states[unit_index].published_frontier_count : min(unit_states[unit_index].queue_count, unit.queue_capacity);
		if (relative >= published_count) { return; }
	}
	SelectionTask task = tasks[unit.task];
	uint source_multimesh;
	if (validity[unit.task] == 0 || !resolve_multimesh(task, unit.ordinal, source_multimesh)) { return; }
	MicroGeometryInstance instance = instances[uint(task.instance) - 1];
	MicroGeometryInstanceSurface surface = surfaces[uint(task.surface) - 1];
	MicroGeometryAsset asset = *((MicroGeometryAsset*)instance.asset_address);
	bool force_finest = surface.force_finest != 0;
	bool finest = (parameters.flags & 32) != 0 && force_finest;
	if (push.mode == 2) {
		if (relative >= min(unit_states[unit_index].committed_count, unit.record_capacity)) { return; }
		uint2 record = committed[index];
		MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[record.x];
		MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
		uint ready_mask = (parameters.flags & 16) != 0 ? 3 : 1;
		if (page.generation != record.y || ((page.ready & 1) == 0 || (((uint*)asset.group_states)[cluster.group] & ready_mask) != ready_mask)) {
			InterlockedOr(unit_states[unit_index].status, 8);
		}
		return;
	}
	if (queue_work) {
		uint slot = frontier[index];
		uint group_id = sparse_states[slot].x - 1;
		MicroGeometryGroup group = ((MicroGeometryGroup*)asset.groups)[group_id];
		if (push.mode == 4) {
			if (group.depth != push.level) { return; }
			if ((parameters.flags & 32) == 0 && !frustum_visible(task, instance, source_multimesh, group.center, group.radius)) { return; }
			float3 center;
			float scale;
			view_bounds(task, instance, source_multimesh, group.center, 1, false, center, scale);
			float depth = (parameters.flags & 32) != 0 ? length(center) : -center.z;
			float distance = parameters.projection[15] != 0 ? 1 : max(0.00001, depth - group.radius * scale);
			float error = group.error * scale * abs(parameters.projection[5]) * parameters.output_height * 0.5 / distance;
			if ((parameters.flags & 128) != 0 && group.parent_count != 0 && parameters.projection[15] == 0) {
				if (depth - group.radius * scale <= parameters.near_plane) { add_statistic(66); }
				if (depth - group.radius * scale <= 0.00001) { add_statistic(67); }
			}
			float target_error = parameters.error;
			float instance_center[3];
			for (uint axis = 0; axis < 3; axis++) { instance_center[axis] = instance.aabb_position[axis] + instance.aabb_size[axis] * 0.5; }
			float instance_radius = length(float3(instance.aabb_size[0], instance.aabb_size[1], instance.aabb_size[2])) * 0.5;
			if ((parameters.flags & 32) != 0 && !frustum_visible(task, instance, source_multimesh, instance_center, instance_radius)) { target_error *= parameters.offscreen_multiplier; }
			bool wanted = group.parent_count == 0 || (task.flags & 2) != 0 || force_finest || error > target_error;
			uint ready_mask = (parameters.flags & 16) != 0 ? 3 : 1;
			bool ready = (((uint*)asset.group_states)[group_id] & ready_mask) == ready_mask;
			bool parents_active = true;
			bool parents_ready = true;
			for (uint parent = 0; parent < group.parent_count; parent++) {
				uint parent_id = ((uint*)asset.parent_groups)[group.first_parent + parent];
				uint parent_slot = find_group(unit, parent_id);
				if (parent_slot == 0xffffffff) { parents_active = false; } else { parents_active = parents_active && sparse_states[parent_slot].y != 0; }
				parents_ready = parents_ready && ((((uint*)asset.group_states)[parent_id] & ready_mask) == ready_mask);
			}
			bool active = wanted && ready && parents_active;
			if (wanted && !ready) { add_statistic(32); }
			if (wanted && ready && !parents_active) { add_statistic(33); }
			if (active && group.parent_count != 0) { add_statistic(34); }
			if (!wanted) { add_statistic(39); }
			sparse_states[slot].y = active ? 1 : 0;
			bool request = wanted && !ready && parents_ready;
			if (finest) {
				request = false;
				for (uint local = 0; local < group.cluster_count; local++) {
					MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[group.first_cluster + local];
					request = request || (!ready && cluster.refined_group == 0xffffffff && ((MicroGeometrySurface*)asset.surfaces)[cluster.surface].source_surface == surface.source_surface);
				}
			}
			if (request) { request_group(asset.identity, group_id); }
			if (active || finest) {
				for (uint local = 0; local < group.cluster_count; local++) {
					MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[group.first_cluster + local];
					if (cluster.refined_group != 0xffffffff) { enqueue_group(unit_index, cluster.refined_group); }
				}
			}
			return;
		}
		if (!finest && sparse_states[slot].y == 0) { return; }
		for (uint local = 0; local < group.cluster_count; local++) {
			uint cluster_id = group.first_cluster + local;
			MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_id];
			if (((MicroGeometrySurface*)asset.surfaces)[cluster.surface].source_surface != surface.source_surface) { continue; }
			uint child_slot = cluster.refined_group == 0xffffffff ? 0xffffffff : find_group(unit, cluster.refined_group);
			if (finest && cluster.refined_group != 0xffffffff) { continue; }
			if (!finest && child_slot != 0xffffffff) {
				if (sparse_states[child_slot].y != 0) { continue; }
			}
			MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
			uint ready_mask = (parameters.flags & 16) != 0 ? 3 : 1;
			if (((page.ready & 1) == 0 || (((uint*)asset.group_states)[cluster.group] & ready_mask) != ready_mask)) { InterlockedOr(unit_states[unit_index].status, 4); continue; }
			uint destination;
			InterlockedAdd(unit_states[unit_index].candidate_count, 1, destination);
			if (destination < unit.record_capacity) {
				candidate[unit.record_offset + destination] = uint2(cluster_id, page.generation);
			} else {
				InterlockedOr(unit_states[unit_index].status, 2);
			}
		}
		return;
	}
	if (push.mode == 6) {
		if ((unit_states[unit_index].status & 7) == 0 && relative < unit_states[unit_index].candidate_count) { committed[index] = candidate[index]; }
		return;
	}
	bool recovery = push.mode == 11;
	uint count = recovery ? unit_states[unit_index].rejected_count : unit_states[unit_index].committed_count;
	if (relative >= min(count, unit.record_capacity)) { return; }
	uint2 cut = recovery ? rejected[index] : committed[index];
	uint cluster_id = cut.x;
	MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_id];
	uint statistic_offset = recovery ? 16 : 4;
	add_statistic(statistic_offset);
	if ((parameters.flags & 32) == 0 && !frustum_visible(task, instance, source_multimesh, cluster.center, cluster.radius)) { add_statistic(statistic_offset + 1); return; }
	if (occluded(task, instance, source_multimesh, cluster, !recovery)) {
		add_statistic(statistic_offset + 8);
		if (!recovery) {
			uint rejected_index;
			InterlockedAdd(unit_states[unit_index].rejected_count, 1, rejected_index);
			rejected[unit.record_offset + rejected_index] = cut;
		}
		return;
	}
	MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
	if ((page.ready & 1) == 0 || page.generation != cut.y) { add_statistic(statistic_offset + 9); return; }
	uint destination;
	InterlockedAdd(counts[task.bin], 1, destination);
	uint selected_offset = bins[task.bin].offset + destination + (recovery ? initial_counts[task.bin] : 0);
	if (!recovery) { InterlockedAdd(initial_counts[task.bin], 1); }
	MicroGeometrySelectedCluster record;
	record.instance = task.instance;
	record.surface = task.surface;
	record.asset = asset.identity;
	record.asset_address = instance.asset_address;
	record.material_generation = surface.material_generation;
	record.cluster = cluster_id;
	record.page_generation = page.generation;
	record.multimesh_instance = source_multimesh;
	record.bin = task.bin;
	record.material_slot = surface.material_slot;
	record.raster_instance = unit.task;
	selected[selected_offset] = record;
	InterlockedAdd(statistics[0], 1);
	InterlockedAdd(statistics[1], cluster.triangle_count);
	add_statistic(statistic_offset + 10);
	add_statistic(statistic_offset + 11, cluster.triangle_count);
	add_statistic(84, cluster.vertex_count);
	if ((parameters.flags & 128) != 0) {
		uint bucket = unit_states[unit_index].padding[0];
		add_statistic(41 + bucket * 4);
		add_statistic(42 + bucket * 4, cluster.triangle_count);
		if (cluster.refined_group == 0xffffffff) { add_statistic(43 + bucket * 4); }
	}
	if ((parameters.flags & 128) != 0 && cluster.refined_group != 0xffffffff) {
		uint child_slot = find_group(unit, cluster.refined_group);
		if (child_slot == 0xffffffff || sparse_states[child_slot].y == 0) { add_statistic(36); }
	}

}
