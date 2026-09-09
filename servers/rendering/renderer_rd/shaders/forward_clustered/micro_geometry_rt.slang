#[compute]
#VERSION_DEFINES
#include "micro_geometry_inc.slang"

struct MicroRTTask {
	uint64_t clas_addresses;
	uint64_t instance;
	uint64_t surface;
	uint selection_bin;
	uint geometry_base;
	uint motion_base;
	uint instance_count;
	uint cluster_count;
	uint source_surface;
	uint force_finest;
	uint group_count;
	uint instance_flags;
	uint instance_mask;
	uint64_t indirect_command;
};
struct Segment { uint task; uint ordinal; uint source_offset; uint capacity; uint offset; uint geometry; uint unit; uint pad; };
struct Cut { uint64_t asset; uint64_t records; uint64_t address; uint count; uint source_surface; uint flags; uint generation; uint hash; uint pad; };
struct State { uint count; uint representative; uint cursor; uint hash; uint invalid; uint triangles; uint page_count; uint pad; };
struct MicroRTTLASInstance { float transform[12]; uint custom_index_mask; uint sbt_offset_flags; uint64_t blas_address; };
struct MicroRTMotion { float previous_transform[12]; };
struct Parameters {
	float4 rt_origin;
	float4 rt_origin_low;
	uint unit_count;
	uint slot_count;
	uint bucket_mask;
	uint record_count;
	uint mode;
	uint step;
	uint width;
	uint range_begin;
	uint range_count;
	uint representative;
	uint bootstrap;
	uint layer_mask;
};
[[vk::binding(0, 0)]] StructuredBuffer<MicroRTTask> tasks;
[[vk::binding(1, 0)]] StructuredBuffer<Segment> segments;
[[vk::binding(2, 0)]] StructuredBuffer<uint2> source_records;
[[vk::binding(3, 0)]] StructuredBuffer<uint4> source_states;
[[vk::binding(4, 0)]] StructuredBuffer<MicroGeometryInstance> instances;
[[vk::binding(5, 0)]] StructuredBuffer<Cut> pool;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint2> records;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint2> pages;
[[vk::binding(8, 0)]] RWStructuredBuffer<State> states;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> buckets;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint> links;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> usage;
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> feedback;
[[vk::binding(13, 0)]] RWStructuredBuffer<MicroRTTLASInstance> tlas_instances;
[[vk::binding(14, 0)]] RWStructuredBuffer<MicroRTMotion> motion;
[[vk::binding(15, 0)]] RWStructuredBuffer<uint2> geometry_slots;
[[vk::binding(16, 0)]] StructuredBuffer<uint2> representative_slots;
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

static const uint INVALID = 0xffffffffu;
static const uint SLOT = 0x80000000u;
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


uint mix_integer(uint x) {
	x = (x ^ (x >> 16)) * 0x7feb352du;
	x = (x ^ (x >> 15)) * 0x846ca68bu;
	return x ^ (x >> 16);
}
uint segment_at(uint index) {
	uint begin = 0, end = parameters.unit_count;
	while (begin + 1 < end) {
		uint middle = (begin + end) / 2;
		if (segments[middle].offset <= index) { begin = middle; } else { end = middle; }
	}
	return begin;
}
bool resolve(Segment segment, out MicroGeometryInstance instance, out MicroGeometryAsset asset, out uint source_ordinal) {
	MicroRTTask task = tasks[segment.task];
	instance = instances[uint(task.instance) - 1];
	asset = (MicroGeometryAsset)0;
	source_ordinal = INVALID;
	if (instance.handle != task.instance || instance.asset_address == 0) { return false; }
	asset = *((MicroGeometryAsset*)instance.asset_address);
	if (asset.identity != instance.asset || (asset.ready & 3) != 3) { return false; }
	if (instance.visible == 0 || (instance.layer_mask & parameters.layer_mask) == 0 || instance.deformed != 0 || segment.ordinal >= task.instance_count) { return true; }
	uint ordinal = segment.ordinal;
	if (task.indirect_command != 0) {
		uint* command = (uint*)task.indirect_command;
		if (ordinal >= min(command[1], task.instance_count) || command[4] >= task.instance_count - ordinal) { return true; }
		ordinal += command[4];
	}
	if (instance.multimesh_address != 0 && ordinal >= instance.multimesh_count) { return true; }
	source_ordinal = ordinal;
	return true;
}
void feedback_record(uint kind, uint id, uint a, uint b, uint c, uint d) {
	uint destination;
	InterlockedAdd(feedback[0], 1, destination);
	if (destination >= 65536) { InterlockedOr(feedback[2], 1); return; }
	uint base = 8 + destination * 8;
	feedback[base] = kind;
	feedback[base + 1] = id;
	feedback[base + 2] = a;
	feedback[base + 3] = b;
	feedback[base + 4] = c;
	feedback[base + 5] = d;
	feedback[base + 6] = 0;
	feedback[base + 7] = 0;
}
groupshared uint values[128];
groupshared uint mismatch[128];
groupshared uint triangles[128];
groupshared uint candidate;

[numthreads(128, 1, 1)]
void main(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex) {
	uint group = group_id.x + group_id.y * 65535u;
	uint index = group * 128u + lane;
	uint mode = parameters.mode;
	if (mode == 0) {
		if (index >= parameters.record_count) { return; }
		records[index] = uint2(INVALID, INVALID);
		pages[index] = uint2(INVALID, INVALID);
		return;
	}
	if (mode == 1) {
		if (group >= parameters.unit_count) { return; }
		Segment segment = segments[group];
		MicroRTTask task = tasks[segment.task];
		MicroGeometryInstance instance;
		MicroGeometryAsset asset;
		uint source_ordinal;
		bool valid = resolve(segment, instance, asset, source_ordinal);
		uint count = parameters.bootstrap != 0 || source_ordinal == INVALID ? 0 : source_states[segment.unit * 2].z;
		if (lane == 0) {
			State state = (State)0;
			state.representative = INVALID;
			state.cursor = INVALID;
			state.count = count;
			state.invalid = !valid || count > segment.capacity ? 1 : 0;
			states[group] = state;
		}
		AllMemoryBarrierWithGroupSync();
		if (!valid || source_ordinal == INVALID || count > segment.capacity) { return; }
		if (parameters.bootstrap != 0) {
			if (task.force_finest != 0) { return; }
			for (uint terminal = 0; terminal < asset.terminal_count; terminal++) {
				MicroGeometryGroup terminal_group = ((MicroGeometryGroup*)asset.groups)[((uint*)asset.terminals)[terminal]];
				for (uint local = lane; local < terminal_group.cluster_count; local += 128) {
					uint cluster_id = terminal_group.first_cluster + local;
					MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_id];
					if (((MicroGeometrySurface*)asset.surfaces)[cluster.surface].source_surface != task.source_surface) { continue; }
					MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
					uint destination;
					InterlockedAdd(states[group].count, 1, destination);
					if (destination < segment.capacity && (page.ready & 3) == 3) {
						records[segment.offset + destination] = uint2(cluster_id, page.generation);
					} else { InterlockedOr(states[group].invalid, 1); }
				}
			}
		} else {
			for (uint local = lane; local < count; local += 128) {
				uint2 record = source_records[segment.source_offset + local];
				bool ready = record.x < asset.cluster_count;
				if (ready) {
					MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[record.x];
					MicroGeometryPage page = ((MicroGeometryPage*)asset.pages)[cluster.page];
					ready = record.y == page.generation && (page.ready & 3) == 3 && ((MicroGeometrySurface*)asset.surfaces)[cluster.surface].source_surface == task.source_surface;
				}
				if (ready) { records[segment.offset + local] = record; } else { InterlockedOr(states[group].invalid, 1); }
			}
		}
		return;
	}
	if (mode == 2 || mode == 11) {
		if (index >= parameters.record_count) { return; }
		Segment segment = segments[segment_at(index)];
		uint relative = index - segment.offset;
		uint other = relative ^ parameters.step;
		if (other <= relative || other >= segment.capacity || parameters.width > segment.capacity) { return; }
		uint2 a = mode == 2 ? records[index] : pages[index];
		uint2 b = mode == 2 ? records[segment.offset + other] : pages[segment.offset + other];
		bool greater = a.x > b.x || (a.x == b.x && a.y > b.y);
		if (greater == ((relative & parameters.width) == 0)) {
			if (mode == 2) { records[index] = b; records[segment.offset + other] = a; }
			else { pages[index] = b; pages[segment.offset + other] = a; }
		}
		return;
	}
	if (mode == 3) {
		if (group >= parameters.unit_count) { return; }
		Segment segment = segments[group];
		MicroRTTask task = tasks[segment.task];
		MicroGeometryInstance instance;
		MicroGeometryAsset asset;
		uint source_ordinal;
		bool valid = resolve(segment, instance, asset, source_ordinal) && states[group].invalid == 0;
		uint hash = 0, triangle_count = 0;
		for (uint local = lane; valid && local < states[group].count; local += 128) {
			uint2 record = records[segment.offset + local];
			MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[record.x];
			hash ^= mix_integer(record.x ^ mix_integer(record.y) ^ mix_integer(local));
			triangle_count += cluster.triangle_count;
			pages[segment.offset + local] = uint2(cluster.page, record.y);
		}
		values[lane] = hash;
		triangles[lane] = triangle_count;
		GroupMemoryBarrierWithGroupSync();
		for (uint stride = 64; stride != 0; stride >>= 1) {
			if (lane < stride) { values[lane] ^= values[lane + stride]; triangles[lane] += triangles[lane + stride]; }
			GroupMemoryBarrierWithGroupSync();
		}
		if (lane == 0) {
			states[group].hash = mix_integer(values[0] ^ mix_integer(states[group].count) ^ uint(instance.asset) ^ uint(instance.asset >> 32) ^ mix_integer(task.source_surface));
			states[group].triangles = triangles[0];
			if (!valid) { states[group].representative = SLOT | geometry_slots[segment.geometry].x; states[group].invalid = 1; }
			else if (states[group].count == 0) { states[group].representative = SLOT; }
		}
		return;
	}
	if (mode == 4) {
		if (index > parameters.bucket_mask) { return; }
		buckets[index] = INVALID;
		return;
	}
	if (mode == 5) {
		if (index == 0 || index >= parameters.slot_count || pool[index].count == 0) { return; }
		InterlockedExchange(buckets[pool[index].hash & parameters.bucket_mask], index, links[index]);
		return;
	}
	if (mode == 6) {
		if (group >= parameters.unit_count) { return; }
		Segment segment = segments[group];
		State state = states[group];
		if (state.representative != INVALID) { return; }
		if (lane == 0) { candidate = parameters.step == 0 ? buckets[state.hash & parameters.bucket_mask] : state.cursor; }
		GroupMemoryBarrierWithGroupSync();
		for (uint round = 0; round < 8 && candidate != INVALID; round++) {
			Cut cut = pool[candidate];
			MicroRTTask task = tasks[segment.task];
			MicroGeometryInstance instance = instances[uint(task.instance) - 1];
			bool equal = cut.count == state.count && cut.hash == state.hash && cut.asset == instance.asset && cut.source_surface == task.source_surface && cut.flags == 0;
			uint different = equal ? 0 : 1;
			for (uint local = lane; equal && local < state.count; local += 128) {
				different |= any(records[segment.offset + local] != ((uint2*)cut.records)[local]) ? 1 : 0;
			}
			mismatch[lane] = different;
			GroupMemoryBarrierWithGroupSync();
			for (uint stride = 64; stride != 0; stride >>= 1) {
				if (lane < stride) { mismatch[lane] |= mismatch[lane + stride]; }
				GroupMemoryBarrierWithGroupSync();
			}
			if (lane == 0) {
				if (mismatch[0] == 0) { states[group].representative = SLOT | candidate; candidate = INVALID; }
				else { candidate = links[candidate]; }
			}
			GroupMemoryBarrierWithGroupSync();
		}
		if (lane == 0) { states[group].cursor = candidate; }
		return;
	}
	if (mode == 7) {
		if (index < parameters.unit_count && states[index].representative == INVALID) { InterlockedMin(buckets[states[index].hash & parameters.bucket_mask], index); }
		return;
	}
	if (mode == 8) {
		if (group >= parameters.unit_count) { return; }
		Segment segment = segments[group];
		State state = states[group];
		if (state.representative != INVALID) { return; }
		uint selected = buckets[state.hash & parameters.bucket_mask];
		if (selected == INVALID) { return; }
		Segment other = segments[selected];
		MicroRTTask task = tasks[segment.task], other_task = tasks[other.task];
		bool equal = states[selected].count == state.count && states[selected].hash == state.hash && instances[uint(task.instance) - 1].asset == instances[uint(other_task.instance) - 1].asset && task.source_surface == other_task.source_surface;
		uint different = equal ? 0 : 1;
		for (uint local = lane; equal && local < state.count; local += 128) { different |= any(records[segment.offset + local] != records[other.offset + local]) ? 1 : 0; }
		mismatch[lane] = different;
		GroupMemoryBarrierWithGroupSync();
		for (uint stride = 64; stride != 0; stride >>= 1) {
			if (lane < stride) { mismatch[lane] |= mismatch[lane + stride]; }
			GroupMemoryBarrierWithGroupSync();
		}
		if (lane == 0 && mismatch[0] == 0) { states[group].representative = selected; }
		return;
	}
	if (mode == 9) {
		if (index >= parameters.unit_count) { return; }
		State state = states[index];
		bool unresolved = parameters.step == 0 ? state.representative == INVALID && state.cursor != INVALID : state.representative == INVALID;
		if (unresolved) { InterlockedAdd(feedback[1], 1); }
		InterlockedOr(feedback[2], state.invalid);
		return;
	}
	if (mode == 10) {
		if (index >= parameters.unit_count) { return; }
		State state = states[index];
		Segment segment = segments[index];
		if ((state.representative & SLOT) != 0 && state.representative != INVALID) { InterlockedAdd(usage[state.representative & ~SLOT], 1); }
		else if (state.representative != INVALID) { InterlockedAdd(states[state.representative].pad, 1); }
		InterlockedAdd(feedback[4], state.count);
		InterlockedAdd(feedback[5], state.triangles);
		uint2 previous = geometry_slots[segment.geometry];
		uint rep_slot = state.representative & ~SLOT;
		bool same = (state.representative & SLOT) != 0 && previous.x == rep_slot && (rep_slot == 0 || previous.y == pool[rep_slot].generation);
		if (!same) { InterlockedOr(feedback[3], 1); }
		return;
	}
	if (mode == 12) {
		if (index >= parameters.range_count) { return; }
		uint item = parameters.range_begin + index;
		if (item < parameters.unit_count) {
			State state = states[item];
			if (state.representative == item && state.count != 0) { feedback_record(1, item, state.count, state.hash, state.pad, 0); }
		} else {
			uint slot = item - parameters.unit_count;
			if (slot < parameters.slot_count && usage[slot] != 0) { feedback_record(2, slot, usage[slot], 0, 0, 0); }
		}
		return;
	}
	if (mode == 13) {
		Segment segment = segments[parameters.representative];
		if (index >= parameters.range_count) { return; }
		uint local = parameters.range_begin + index;
		if (local >= states[parameters.representative].count) { return; }
		uint2 page = pages[segment.offset + local];
		if (local == 0 || any(page != pages[segment.offset + local - 1])) { feedback_record(3, parameters.representative, page.x, page.y, 0, 0); }
		return;
	}
	if (mode == 14) {
		if (index >= parameters.unit_count) { return; }
		Segment segment = segments[index];
		State state = states[index];
		uint2 slot = (state.representative & SLOT) != 0 ? uint2(state.representative & ~SLOT, 0) : representative_slots[state.representative];
		if (slot.x != 0) { slot.y = pool[slot.x].generation; }
		geometry_slots[segment.geometry] = slot;
		return;
	}
	if (mode == 15) {
		if (index >= parameters.unit_count) { return; }
		Segment segment = segments[index];
		MicroRTTask task = tasks[segment.task];
		MicroRTTLASInstance output = (MicroRTTLASInstance)0;
		MicroGeometryInstance instance;
		MicroGeometryAsset asset;
		uint source_ordinal;
		if (resolve(segment, instance, asset, source_ordinal) && source_ordinal != INVALID) {
			uint geometry = task.geometry_base + source_ordinal;
			output.custom_index_mask = geometry | ((task.instance_mask & 255) << 24);
			float4x4 multimesh = micro_rt_multimesh_transform(instance, source_ordinal, false);
			micro_rt_transform(instance, multimesh, false, output.transform);
			MicroRTMotion previous;
			micro_rt_transform(instance, micro_rt_multimesh_transform(instance, source_ordinal, true), true, previous.previous_transform);
			motion[task.motion_base + source_ordinal] = previous;
			uint flags = task.instance_flags;
			if (determinant((float3x3)multimesh) < 0) { flags ^= 2; }
			output.sbt_offset_flags = geometry | ((flags & 255) << 24);
			uint2 slot = geometry_slots[segment.geometry];
			if (slot.x != 0 && pool[slot.x].generation == slot.y) { output.blas_address = pool[slot.x].address; }
		}
		tlas_instances[segment.geometry] = output;
		return;
	}
	if (mode == 16) {
		if (index >= parameters.range_count) { return; }
		Segment segment = segments[parameters.representative];
		Cut cut = pool[representative_slots[parameters.representative].x];
		uint cluster = ((uint2*)cut.records)[index].x;
		((uint64_t*)(cut.records + uint64_t(cut.count) * 8))[index] = ((uint64_t*)tasks[segment.task].clas_addresses)[cluster];
	}

}
