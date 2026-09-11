#[compute]
#VERSION_DEFINES
#include "micro_geometry_inc.slang"

struct PageParameters {
	uint64_t asset;
	uint64_t surface_offsets;
	uint first_cluster;
	uint cluster_count;
	uint pool_slot;
	uint staging_slot;
	float position_step;
	uint pad;
};
[[vk::push_constant]] ConstantBuffer<PageParameters> parameters;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint2> primitive_lookup;
[[vk::binding(1, 0)]] StructuredBuffer<uint> staging;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> pool;
[[vk::binding(3, 0)]] StructuredBuffer<uint4> transcode_infos;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> transcode_faults;

static const uint SLOT_WORDS = 65536 / 4;
static const uint INVALID = 0xffffffff;
static const uint CLUSTER_HEADER_SIZE = 24;
static const uint CLUSTER_UV_HEADER_SIZE = 10;
static const uint VERTEX_REFERENCE_ESCAPE = 31;
static const uint UV_SPLIT_FLAG = 0x80;

groupshared uint index_words[96];

uint disk_word(uint p_base, uint p_byte) {
	uint word = staging[p_base + min(p_byte >> 2, SLOT_WORDS - 1)];
	uint shift = (p_byte & 3) * 8;
	if (shift == 0) {
		return word;
	}
	uint next = staging[p_base + min((p_byte >> 2) + 1, SLOT_WORDS - 1)];
	return (word >> shift) | (next << (32 - shift));
}

uint disk_byte(uint p_base, uint p_byte) {
	return (staging[p_base + min(p_byte >> 2, SLOT_WORDS - 1)] >> ((p_byte & 3) * 8)) & 255;
}

uint disk_bits(uint p_base, uint p_offset, inout uint r_cursor, uint p_bits) {
	if (p_bits == 0) {
		return 0;
	}
	uint byte = p_offset + (r_cursor >> 3);
	uint shift = r_cursor & 7;
	uint value = disk_word(p_base, byte) >> shift;
	if (shift != 0 && p_bits + shift > 32) {
		value |= disk_byte(p_base, byte + 4) << (32 - shift);
	}
	r_cursor += p_bits;
	return p_bits >= 32 ? value : (value & ((1u << p_bits) - 1));
}

void pool_store(uint p_base, uint p_byte, uint p_value) {
	pool[p_base + min(p_byte >> 2, SLOT_WORDS - 1)] = p_value;
}

uint custom_size(uint64_t p_format, uint p_channel) {
	if ((p_format & (uint64_t(1) << (6 + p_channel))) == 0) {
		return 0;
	}
	const uint sizes[8] = { 4, 4, 4, 8, 4, 8, 12, 16 };
	return sizes[uint(p_format >> (13 + p_channel * 3)) & 7];
}

uint read_reference(uint p_base, uint p_offset, inout uint r_cursor, inout uint r_next) {
	if (disk_bits(p_base, p_offset, r_cursor, 1) == 0) {
		uint value = r_next;
		r_next += 1;
		return value;
	}
	uint delta = disk_bits(p_base, p_offset, r_cursor, 5);
	if (delta < VERTEX_REFERENCE_ESCAPE) {
		return r_next != 0 ? r_next - 1 - delta : 0;
	}
	return disk_bits(p_base, p_offset, r_cursor, 7);
}

[shader("compute")]
[numthreads(32, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
	if (group.x >= parameters.cluster_count) { return; }
	uint lane = thread.x;
	MicroGeometryAsset asset = *((MicroGeometryAsset*)parameters.asset);
	uint cluster_id = parameters.first_cluster + group.x;
	MicroGeometryCluster cluster = ((MicroGeometryCluster*)asset.clusters)[cluster_id];
	MicroGeometrySurface surface = ((MicroGeometrySurface*)asset.surfaces)[cluster.surface];
	uint4 info = transcode_infos[group.x];
	uint pool_base = parameters.pool_slot * SLOT_WORDS;
	uint disk_base = parameters.staging_slot * SLOT_WORDS;
	uint header = info.w;
	uint vertex_count = cluster.vertex_count;
	uint triangle_count = cluster.triangle_count;

	int3 grid_minimum = int3(asint(disk_word(disk_base, header)), asint(disk_word(disk_base, header + 4)), asint(disk_word(disk_base, header + 8)));
	uint packed_bits = disk_word(disk_base, header + 12);
	uint3 position_bits = uint3(packed_bits & 255, (packed_bits >> 8) & 255, (packed_bits >> 16) & 255);
	uint vertex_id_bits = packed_bits >> 24;
	uint topology_bytes = disk_word(disk_base, header + 16) >> 16;

	uint uv_low_min[4];
	uint uv_low_max[4];
	uint uv_high_min[4];
	uint uv_high_max[4];
	uint uv_bits[4];
	uint uv_split[4];
	uint uv_header = header + CLUSTER_HEADER_SIZE;
	for (uint slot = 0; slot < 4; slot++) {
		uv_low_min[slot] = 0;
		uv_low_max[slot] = 0;
		uv_high_min[slot] = 0;
		uv_high_max[slot] = 0;
		uv_bits[slot] = 0;
		uv_split[slot] = 0;
	}
	for (uint channel = 0; channel < 2; channel++) {
		if (surface.uv_mode[channel] == 0) { continue; }
		for (uint component = 0; component < 2; component++) {
			uint slot = channel * 2 + component;
			uv_low_min[slot] = disk_word(disk_base, uv_header) & 0xffff;
			uv_low_max[slot] = disk_word(disk_base, uv_header + 2) & 0xffff;
			uv_high_min[slot] = disk_word(disk_base, uv_header + 4) & 0xffff;
			uv_high_max[slot] = disk_word(disk_base, uv_header + 6) & 0xffff;
			uint code = disk_byte(disk_base, uv_header + 8);
			uv_bits[slot] = code & ~UV_SPLIT_FLAG;
			uv_split[slot] = code & UV_SPLIT_FLAG;
			uv_header += CLUSTER_UV_HEADER_SIZE;
		}
	}

	uint section = uv_header;
	uint positions_offset = section;
	section += (vertex_count * (position_bits.x + position_bits.y + position_bits.z) + 7) / 8;
	uint normals_offset = section;
	if (surface.attribute_offsets[1] != INVALID) {
		section += vertex_count * 2;
	}
	uint colors_offset = section;
	if (surface.attribute_offsets[3] != INVALID) {
		section += vertex_count * 4;
	}
	uint uv_offset[2];
	for (uint channel = 0; channel < 2; channel++) {
		uv_offset[channel] = section;
		if (surface.uv_mode[channel] != 0) {
			section += (vertex_count * (uv_bits[channel * 2] + uv_bits[channel * 2 + 1]) + 7) / 8;
		}
	}
	uint custom_offset[4];
	for (uint channel = 0; channel < 4; channel++) {
		custom_offset[channel] = section;
		section += vertex_count * custom_size(surface.format, channel);
	}
	uint topology_offset = section;
	uint identity_offset = section + topology_bytes;

	float3 frame_center = float3(surface.frame_center[0], surface.frame_center[1], surface.frame_center[2]);
	float frame_scale = surface.frame_scale > 0.0 ? surface.frame_scale : 1.0;
	uint position_stride = position_bits.x + position_bits.y + position_bits.z;
	for (uint vertex = lane; vertex < vertex_count; vertex += 32) {
		uint vertex_byte = info.x + vertex * surface.vertex_stride;
		uint cursor = vertex * position_stride;
		int3 grid;
		grid.x = grid_minimum.x + asint(disk_bits(disk_base, positions_offset, cursor, position_bits.x));
		grid.y = grid_minimum.y + asint(disk_bits(disk_base, positions_offset, cursor, position_bits.y));
		grid.z = grid_minimum.z + asint(disk_bits(disk_base, positions_offset, cursor, position_bits.z));
		float3 local = (float3(grid) * parameters.position_step - frame_center) / frame_scale;
		int3 quantized = int3(round(clamp(local, float3(-1.0), float3(1.0)) * 32767.0));
		uint normal = 0;
		if (surface.attribute_offsets[1] != INVALID) {
			normal = disk_byte(disk_base, normals_offset + vertex * 2) | (disk_byte(disk_base, normals_offset + vertex * 2 + 1) << 8);
		}
		pool_store(pool_base, vertex_byte, (asuint(quantized.x) & 0xffff) | ((asuint(quantized.y) & 0xffff) << 16));
		pool_store(pool_base, vertex_byte + 4, (asuint(quantized.z) & 0xffff) | (normal << 16));
		if (surface.attribute_offsets[3] != INVALID) {
			pool_store(pool_base, vertex_byte + surface.attribute_offsets[3], disk_word(disk_base, colors_offset + vertex * 4));
		}
		for (uint channel = 0; channel < 2; channel++) {
			if (surface.uv_mode[channel] == 0 || surface.attribute_offsets[4 + channel] == INVALID) { continue; }
			uint slot = channel * 2;
			uint uv_cursor = vertex * (uv_bits[slot] + uv_bits[slot + 1]);
			uint codes[2];
			for (uint component = 0; component < 2; component++) {
				uint index = disk_bits(disk_base, uv_offset[channel], uv_cursor, uv_bits[slot + component]);
				uint span = uv_low_max[slot + component] - uv_low_min[slot + component] + 1;
				codes[component] = (uv_split[slot + component] != 0 && index >= span) ? uv_high_min[slot + component] + index - span : uv_low_min[slot + component] + index;
			}
			pool_store(pool_base, vertex_byte + surface.attribute_offsets[4 + channel], (codes[0] & 0xffff) | ((codes[1] & 0xffff) << 16));
		}
		for (uint channel = 0; channel < 4; channel++) {
			uint size = custom_size(surface.format, channel);
			if (size == 0 || surface.attribute_offsets[6 + channel] == INVALID) { continue; }
			for (uint word = 0; word < size / 4; word++) {
				pool_store(pool_base, vertex_byte + surface.attribute_offsets[6 + channel] + word * 4, disk_word(disk_base, custom_offset[channel] + vertex * size + word * 4));
			}
		}
	}

	if (lane == 0) {
		uint fault_count = 0;
		for (uint word = 0; word < 96; word++) {
			index_words[word] = 0;
		}
		uint cursor = 0;
		uint next_vertex = 0;
		uint3 previous = uint3(0, 0, 0);
		for (uint triangle = 0; triangle < triangle_count; triangle++) {
			uint3 indices;
			if (disk_bits(disk_base, topology_offset, cursor, 1) != 0) {
				indices.x = read_reference(disk_base, topology_offset, cursor, next_vertex);
				indices.y = read_reference(disk_base, topology_offset, cursor, next_vertex);
				indices.z = read_reference(disk_base, topology_offset, cursor, next_vertex);
			} else {
				uint left = disk_bits(disk_base, topology_offset, cursor, 1);
				indices.x = left != 0 ? previous.x : previous.z;
				indices.y = left != 0 ? previous.z : previous.y;
				indices.z = read_reference(disk_base, topology_offset, cursor, next_vertex);
			}
			previous = indices;
			if (indices.x >= vertex_count || indices.y >= vertex_count || indices.z >= vertex_count) {
				indices = uint3(0, 0, 0);
				fault_count += 1;
			}
			for (uint corner = 0; corner < 3; corner++) {
				uint byte = triangle * 3 + corner;
				index_words[byte >> 2] |= (indices[corner] & 255) << ((byte & 3) * 8);
			}
		}
		uint identity_cursor = 0;
		uint identity = disk_bits(disk_base, identity_offset, identity_cursor, 32);
		for (uint vertex = 0; vertex < vertex_count; vertex++) {
			if (vertex != 0) {
				uint zigzag = disk_bits(disk_base, identity_offset, identity_cursor, vertex_id_bits);
				identity += (zigzag >> 1) ^ (0 - (zigzag & 1));
			}
			pool_store(pool_base, info.z + vertex * 4, identity);
		}
		if (fault_count != 0) {
			InterlockedAdd(transcode_faults[0], fault_count);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	for (uint word = lane; word < (triangle_count * 3 + 3) / 4; word += 32) {
		pool_store(pool_base, info.y + word * 4, index_words[word]);
	}

	if (cluster.refined_group != INVALID) { return; }
	uint offset = ((uint*)parameters.surface_offsets)[cluster.surface];
	for (uint triangle = lane; triangle < triangle_count; triangle += 32) {
		uint primitive = cluster.first_primitive + triangle;
		if (primitive < surface.source_triangle_count) {
			primitive_lookup[offset + primitive] = uint2(cluster_id, triangle);
		}
	}
}
