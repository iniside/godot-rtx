/**************************************************************************/
/*  micro_geometry_builder.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef _3D_DISABLED
#include "micro_geometry_builder.h"

#include "thirdparty/meshoptimizer/meshoptimizer.h"

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/math/vector3i.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#define CLUSTERLOD_IMPLEMENTATION
#include "thirdparty/meshoptimizer/clusterlod.h"

namespace {
using Data = MicroGeometryData;

constexpr uint32_t MAX_VERTICES = Data::MAX_CLUSTER_VERTICES;
constexpr uint32_t MAX_TRIANGLES = Data::MAX_CLUSTER_TRIANGLES;

struct BitWriter {
	LocalVector<uint8_t> data;
	uint64_t partial = 0;
	uint32_t partial_bits = 0;

	void put(uint32_t p_value, uint32_t p_bits) {
		if (!p_bits) {
			return;
		}
		uint64_t mask = p_bits >= 32 ? 0xffffffffull : ((1ull << p_bits) - 1);
		partial |= (uint64_t(p_value) & mask) << partial_bits;
		partial_bits += p_bits;
		while (partial_bits >= 8) {
			data.push_back(uint8_t(partial));
			partial >>= 8;
			partial_bits -= 8;
		}
	}
	void align() {
		if (partial_bits) {
			data.push_back(uint8_t(partial));
			partial = 0;
			partial_bits = 0;
		}
	}
	void reset() {
		data.clear();
		partial = 0;
		partial_bits = 0;
	}
};

struct BitReader {
	const uint8_t *data = nullptr;
	uint64_t size = 0;
	uint64_t cursor = 0;
	bool overrun = false;

	uint32_t get(uint32_t p_bits) {
		uint32_t result = 0;
		for (uint32_t i = 0; i < p_bits; i++) {
			uint64_t bit = cursor + i;
			if ((bit >> 3) >= size) {
				overrun = true;
				return 0;
			}
			result |= uint32_t((data[bit >> 3] >> (bit & 7)) & 1) << i;
		}
		cursor += p_bits;
		return result;
	}
};

void append_bytes(LocalVector<uint8_t> &r_target, const uint8_t *p_data, uint32_t p_size) {
	if (!p_size) {
		return;
	}
	uint32_t offset = r_target.size();
	r_target.resize(offset + p_size);
	memcpy(r_target.ptr() + offset, p_data, p_size);
}

uint32_t bit_length(uint32_t p_value) {
	uint32_t bits = 0;
	while (p_value) {
		bits++;
		p_value >>= 1;
	}
	return bits;
}

uint32_t zigzag(int64_t p_value) {
	return uint32_t((p_value << 1) ^ (p_value >> 63));
}

void encode_octahedral(const Vector3 &p_normal, uint8_t *r_encoded) {
	double length = Math::abs(p_normal.x) + Math::abs(p_normal.y) + Math::abs(p_normal.z);
	double x = 0;
	double y = 0;
	if (length > 0) {
		x = double(p_normal.x) / length;
		y = double(p_normal.y) / length;
		if (p_normal.z < 0) {
			double folded_x = (1.0 - Math::abs(y)) * (x >= 0 ? 1.0 : -1.0);
			double folded_y = (1.0 - Math::abs(x)) * (y >= 0 ? 1.0 : -1.0);
			x = folded_x;
			y = folded_y;
		}
	}
	r_encoded[0] = uint8_t(CLAMP(Math::round((x * 0.5 + 0.5) * 255.0), 0.0, 255.0));
	r_encoded[1] = uint8_t(CLAMP(Math::round((y * 0.5 + 0.5) * 255.0), 0.0, 255.0));
}

Data::Bounds copy_bounds(const clodBounds &p_bounds) {
	Data::Bounds result;
	memcpy(result.center, p_bounds.center, sizeof(result.center));
	result.radius = p_bounds.radius;
	result.error = p_bounds.error;
	return result;
}

Vector3i triangle_key(uint32_t p_a, uint32_t p_b, uint32_t p_c) {
	Vector3i result(p_a, p_b, p_c);
	Vector3i second(p_b, p_c, p_a);
	Vector3i third(p_c, p_a, p_b);
	if (second < result) {
		result = second;
	}
	if (third < result) {
		result = third;
	}
	return result;
}

struct SourceSurface {
	Vector<int32_t> grid;
	Vector<uint8_t> normals;
	Vector<uint8_t> colors;
	Vector<uint16_t> uv;
	Vector<uint8_t> custom[4];
	uint32_t custom_size[4] = {};
	bool has_normals = false;
	bool has_colors = false;
	bool has_uv[2] = {};
};

struct Builder {
	Data::Build data;
	Vector<Data::Permutation> permutations;
	Vector<uint8_t> page;
	uint32_t memory_size = 0;
	SourceSurface source;
	Vector<uint32_t> vertex_ids;
	HashMap<Vector3i, uint32_t> triangle_heads;
	Vector<uint32_t> triangle_next;
	LocalVector<int32_t> edge_head;
	int32_t edge_next[MAX_TRIANGLES * 3] = {};
	uint32_t surface = 0;
	float position_step = 0;
	Error error = OK;

	static int output(void *p_context, clodGroup p_group, const clodCluster *p_clusters, size_t p_cluster_count);
};

int32_t find_continuation(const Builder &p_builder, const bool *p_visited, uint32_t p_from, uint32_t p_to) {
	int32_t occurrence = p_builder.edge_head[p_from * MAX_VERTICES + p_to];
	while (occurrence >= 0) {
		if (!p_visited[occurrence / 3]) {
			return occurrence;
		}
		occurrence = p_builder.edge_next[occurrence];
	}
	return -1;
}

uint32_t write_reference(BitWriter &r_writer, uint8_t *r_map, uint32_t &r_next, uint32_t p_local) {
	uint32_t index = r_map[p_local];
	if (index == 0xff) {
		r_writer.put(0, 1);
		index = r_next++;
		r_map[p_local] = uint8_t(index);
	} else {
		r_writer.put(1, 1);
		uint32_t delta = r_next - 1 - index;
		if (delta < Data::VERTEX_REFERENCE_ESCAPE) {
			r_writer.put(delta, 5);
		} else {
			r_writer.put(Data::VERTEX_REFERENCE_ESCAPE, 5);
			r_writer.put(index, 7);
		}
	}
	return index;
}

uint32_t read_reference(BitReader &r_reader, uint32_t &r_next) {
	if (!r_reader.get(1)) {
		return r_next++;
	}
	uint32_t delta = r_reader.get(5);
	if (delta < Data::VERTEX_REFERENCE_ESCAPE) {
		return r_next ? r_next - 1 - delta : 0;
	}
	return r_reader.get(7);
}

Error verify_topology(const BitWriter &p_writer, uint32_t p_vertex_count, uint32_t p_triangle_count, const uint8_t *p_expected) {
	BitReader reader{ p_writer.data.ptr(), p_writer.data.size(), 0, false };
	uint32_t next_vertex = 0;
	uint32_t previous[3] = {};
	for (uint32_t i = 0; i < p_triangle_count; i++) {
		uint32_t triangle[3];
		if (reader.get(1)) {
			for (uint32_t k = 0; k < 3; k++) {
				triangle[k] = read_reference(reader, next_vertex);
			}
		} else {
			uint32_t left = reader.get(1);
			triangle[0] = left ? previous[0] : previous[2];
			triangle[1] = left ? previous[2] : previous[1];
			triangle[2] = read_reference(reader, next_vertex);
		}
		for (uint32_t k = 0; k < 3; k++) {
			ERR_FAIL_COND_V(triangle[k] >= p_vertex_count || triangle[k] != p_expected[i * 3 + k], ERR_INVALID_DATA);
			previous[k] = triangle[k];
		}
	}
	ERR_FAIL_COND_V(reader.overrun || next_vertex != p_vertex_count, ERR_INVALID_DATA);
	return OK;
}

Error encode_topology(Builder &r_builder, const unsigned char *p_indices, uint32_t p_vertex_count, uint32_t p_triangle_count, uint8_t *r_strip_to_local, uint32_t *r_order, uint8_t *r_triangles, BitWriter &r_writer) {
	int32_t *head = r_builder.edge_head.ptr();
	int32_t *next = r_builder.edge_next;
	for (uint32_t i = 0; i < p_triangle_count * 3; i++) {
		uint32_t from = p_indices[i];
		uint32_t to = p_indices[i - i % 3 + (i + 1) % 3];
		uint32_t key = from * MAX_VERTICES + to;
		next[i] = head[key];
		head[key] = int32_t(i);
	}
	uint8_t map[MAX_VERTICES];
	memset(map, 0xff, sizeof(map));
	bool visited[MAX_TRIANGLES] = {};
	uint32_t next_vertex = 0;
	uint32_t cursor = 0;
	uint32_t previous[3] = {};
	bool have_previous = false;
	Error error = OK;
	for (uint32_t emitted = 0; emitted < p_triangle_count; emitted++) {
		int32_t occurrence = -1;
		uint32_t from = 0;
		uint32_t to = 0;
		uint32_t left = 0;
		if (have_previous) {
			occurrence = find_continuation(r_builder, visited, previous[2], previous[1]);
			if (occurrence >= 0) {
				from = previous[2];
				to = previous[1];
			} else {
				occurrence = find_continuation(r_builder, visited, previous[0], previous[2]);
				if (occurrence >= 0) {
					from = previous[0];
					to = previous[2];
					left = 1;
				}
			}
		}
		uint32_t local[3];
		uint32_t triangle[3];
		uint32_t index;
		if (occurrence >= 0) {
			index = uint32_t(occurrence) / 3;
			uint32_t corner = uint32_t(occurrence) % 3;
			r_writer.put(0, 1);
			r_writer.put(left, 1);
			local[0] = from;
			local[1] = to;
			local[2] = p_indices[index * 3 + (corner + 2) % 3];
			triangle[0] = map[local[0]];
			triangle[1] = map[local[1]];
			triangle[2] = write_reference(r_writer, map, next_vertex, local[2]);
		} else {
			while (cursor < p_triangle_count && visited[cursor]) {
				cursor++;
			}
			if (cursor >= p_triangle_count) {
				error = ERR_INVALID_DATA;
				break;
			}
			index = cursor;
			r_writer.put(1, 1);
			for (uint32_t k = 0; k < 3; k++) {
				local[k] = p_indices[index * 3 + k];
				triangle[k] = write_reference(r_writer, map, next_vertex, local[k]);
			}
		}
		visited[index] = true;
		have_previous = true;
		r_order[emitted] = index;
		for (uint32_t k = 0; k < 3; k++) {
			previous[k] = local[k];
			r_triangles[emitted * 3 + k] = uint8_t(triangle[k]);
		}
	}
	r_writer.align();
	for (uint32_t i = 0; i < p_triangle_count * 3; i++) {
		head[p_indices[i] * MAX_VERTICES + p_indices[i - i % 3 + (i + 1) % 3]] = -1;
	}
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(next_vertex != p_vertex_count, ERR_INVALID_DATA);
	for (uint32_t i = 0; i < p_vertex_count; i++) {
		r_strip_to_local[i] = 0;
	}
	for (uint32_t i = 0; i < p_vertex_count; i++) {
		ERR_FAIL_COND_V(map[i] >= p_vertex_count, ERR_INVALID_DATA);
		r_strip_to_local[map[i]] = uint8_t(i);
	}
	return verify_topology(r_writer, p_vertex_count, p_triangle_count, r_triangles);
}

void encode_range(const uint16_t *p_values, uint32_t p_count, uint16_t *r_range, uint32_t &r_bits, bool &r_split) {
	LocalVector<uint16_t> sorted;
	sorted.resize(p_count);
	memcpy(sorted.ptr(), p_values, p_count * sizeof(uint16_t));
	sorted.sort();
	uint32_t low_min = sorted[0];
	uint32_t high_max = sorted[p_count - 1];
	uint32_t single_bits = bit_length(high_max - low_min);
	uint32_t gap = 0;
	uint32_t gap_index = 0;
	for (uint32_t i = 1; i < p_count; i++) {
		uint32_t distance = uint32_t(sorted[i]) - uint32_t(sorted[i - 1]);
		if (distance > gap) {
			gap = distance;
			gap_index = i;
		}
	}
	r_split = false;
	r_bits = single_bits;
	r_range[0] = uint16_t(low_min);
	r_range[1] = uint16_t(high_max);
	r_range[2] = uint16_t(low_min);
	r_range[3] = uint16_t(high_max);
	if (gap > 1) {
		uint32_t low_max = sorted[gap_index - 1];
		uint32_t high_min = sorted[gap_index];
		uint32_t span = (low_max - low_min + 1) + (high_max - high_min + 1);
		uint32_t split_bits = bit_length(span - 1);
		if (split_bits < single_bits) {
			r_split = true;
			r_bits = split_bits;
			r_range[0] = uint16_t(low_min);
			r_range[1] = uint16_t(low_max);
			r_range[2] = uint16_t(high_min);
			r_range[3] = uint16_t(high_max);
		}
	}
}

uint32_t range_index(const uint16_t *p_range, bool p_split, uint32_t p_value) {
	if (p_split && p_value >= p_range[2]) {
		return (uint32_t(p_range[1]) - p_range[0] + 1) + (p_value - p_range[2]);
	}
	return p_value - p_range[0];
}

Error encode_cluster(Builder &r_builder, const clodCluster &p_input, const unsigned int *p_local_vertices, const unsigned char *p_local_indices, uint32_t p_vertex_count, uint32_t p_triangle_count, bool p_leaf, LocalVector<uint8_t> &r_payload, uint32_t &r_first_primitive) {
	const Data::Surface &surface = r_builder.data.surfaces[r_builder.surface];
	const SourceSurface &source = r_builder.source;
	Data::Permutation &permutation = r_builder.permutations.write[r_builder.surface];

	uint8_t strip_to_local[MAX_VERTICES];
	uint32_t order[MAX_TRIANGLES];
	uint8_t triangles[MAX_TRIANGLES * 3];
	BitWriter topology;
	Error err = encode_topology(r_builder, p_local_indices, p_vertex_count, p_triangle_count, strip_to_local, order, triangles, topology);
	ERR_FAIL_COND_V(err != OK, err);
	ERR_FAIL_COND_V(topology.data.size() > UINT16_MAX, ERR_INVALID_DATA);

	uint32_t vertices[MAX_VERTICES];
	uint32_t identities[MAX_VERTICES];
	int32_t minimum[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
	int32_t maximum[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
	for (uint32_t i = 0; i < p_vertex_count; i++) {
		uint32_t vertex = p_local_vertices[strip_to_local[i]];
		vertices[i] = vertex;
		if (r_builder.vertex_ids[vertex] == Data::INVALID_ID) {
			r_builder.vertex_ids.write[vertex] = permutation.vertices.size();
			permutation.vertices.push_back(vertex);
		}
		identities[i] = r_builder.vertex_ids[vertex];
		for (uint32_t axis = 0; axis < 3; axis++) {
			int32_t value = source.grid[uint64_t(vertex) * 3 + axis];
			minimum[axis] = MIN(minimum[axis], value);
			maximum[axis] = MAX(maximum[axis], value);
		}
	}
	uint8_t position_bits[3];
	for (uint32_t axis = 0; axis < 3; axis++) {
		position_bits[axis] = uint8_t(bit_length(uint32_t(int64_t(maximum[axis]) - minimum[axis])));
	}

	uint16_t uv_range[4][4] = {};
	uint8_t uv_bits[4] = {};
	bool uv_split[4] = {};
	for (uint32_t channel = 0; channel < 2; channel++) {
		if (!source.has_uv[channel]) {
			continue;
		}
		for (uint32_t component = 0; component < 2; component++) {
			uint16_t values[MAX_VERTICES];
			for (uint32_t i = 0; i < p_vertex_count; i++) {
				values[i] = source.uv[uint64_t(vertices[i]) * 4 + channel * 2 + component];
			}
			uint32_t slot = channel * 2 + component;
			uint32_t bits = 0;
			bool split = false;
			encode_range(values, p_vertex_count, uv_range[slot], bits, split);
			uv_bits[slot] = uint8_t(bits);
			uv_split[slot] = split;
		}
	}

	uint32_t identity_bits = 0;
	for (uint32_t i = 1; i < p_vertex_count; i++) {
		identity_bits = MAX(identity_bits, bit_length(zigzag(int64_t(identities[i]) - int64_t(identities[i - 1]))));
	}

	uint32_t header_size = Data::cluster_header_size(surface);
	r_payload.clear();
	r_payload.resize(header_size);
	uint8_t *header = r_payload.ptr();
	memset(header, 0, header_size);
	for (uint32_t axis = 0; axis < 3; axis++) {
		encode_uint32(uint32_t(minimum[axis]), header + axis * 4);
		header[12 + axis] = position_bits[axis];
	}
	header[15] = uint8_t(identity_bits);
	header[16] = uint8_t(p_vertex_count);
	header[17] = uint8_t(p_triangle_count);
	encode_uint16(uint16_t(topology.data.size()), header + 18);
	r_first_primitive = p_leaf ? uint32_t(permutation.triangles.size()) : Data::INVALID_ID;
	encode_uint32(r_first_primitive, header + 20);
	uint32_t uv_offset = Data::CLUSTER_HEADER_SIZE;
	for (uint32_t channel = 0; channel < 2; channel++) {
		if (!source.has_uv[channel]) {
			continue;
		}
		for (uint32_t component = 0; component < 2; component++) {
			uint32_t slot = channel * 2 + component;
			for (uint32_t i = 0; i < 4; i++) {
				encode_uint16(uv_range[slot][i], header + uv_offset + i * 2);
			}
			header[uv_offset + 8] = uint8_t(uv_bits[slot] | (uv_split[slot] ? Data::UV_SPLIT_FLAG : 0));
			uv_offset += Data::CLUSTER_UV_HEADER_SIZE;
		}
	}

	BitWriter stream;
	for (uint32_t i = 0; i < p_vertex_count; i++) {
		for (uint32_t axis = 0; axis < 3; axis++) {
			stream.put(uint32_t(int64_t(source.grid[uint64_t(vertices[i]) * 3 + axis]) - minimum[axis]), position_bits[axis]);
		}
	}
	stream.align();
	append_bytes(r_payload, stream.data.ptr(), stream.data.size());
	if (source.has_normals) {
		uint64_t offset = r_payload.size();
		r_payload.resize(offset + uint64_t(p_vertex_count) * 2);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			memcpy(r_payload.ptr() + offset + i * 2, source.normals.ptr() + uint64_t(vertices[i]) * 2, 2);
		}
	}
	if (source.has_colors) {
		uint64_t offset = r_payload.size();
		r_payload.resize(offset + uint64_t(p_vertex_count) * 4);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			memcpy(r_payload.ptr() + offset + i * 4, source.colors.ptr() + uint64_t(vertices[i]) * 4, 4);
		}
	}
	for (uint32_t channel = 0; channel < 2; channel++) {
		if (!source.has_uv[channel]) {
			continue;
		}
		stream.reset();
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			for (uint32_t component = 0; component < 2; component++) {
				uint32_t slot = channel * 2 + component;
				uint32_t value = source.uv[uint64_t(vertices[i]) * 4 + slot];
				stream.put(range_index(uv_range[slot], uv_split[slot], value), uv_bits[slot]);
			}
		}
		stream.align();
		append_bytes(r_payload, stream.data.ptr(), stream.data.size());
	}
	for (uint32_t channel = 0; channel < 4; channel++) {
		if (!source.custom_size[channel]) {
			continue;
		}
		uint32_t size = source.custom_size[channel];
		uint64_t offset = r_payload.size();
		r_payload.resize(offset + uint64_t(p_vertex_count) * size);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			memcpy(r_payload.ptr() + offset + uint64_t(i) * size, source.custom[channel].ptr() + uint64_t(vertices[i]) * size, size);
		}
	}
	append_bytes(r_payload, topology.data.ptr(), topology.data.size());
	stream.reset();
	stream.put(identities[0], 32);
	for (uint32_t i = 1; i < p_vertex_count; i++) {
		stream.put(zigzag(int64_t(identities[i]) - int64_t(identities[i - 1])), identity_bits);
	}
	stream.align();
	append_bytes(r_payload, stream.data.ptr(), stream.data.size());

	ERR_FAIL_COND_V(r_payload.size() != Data::cluster_disk_size(surface, p_vertex_count, position_bits, uv_bits, identity_bits, topology.data.size()), ERR_INVALID_DATA);

	if (p_leaf) {
		for (uint32_t i = 0; i < p_triangle_count; i++) {
			uint32_t triangle = order[i];
			Vector3i key = triangle_key(p_input.indices[triangle * 3], p_input.indices[triangle * 3 + 1], p_input.indices[triangle * 3 + 2]);
			uint32_t *head = r_builder.triangle_heads.getptr(key);
			ERR_FAIL_COND_V(!head || *head == Data::INVALID_ID, ERR_INVALID_DATA);
			uint32_t primitive = *head;
			*head = r_builder.triangle_next[primitive];
			permutation.triangles.push_back(primitive);
		}
	}
	return OK;
}

int Builder::output(void *p_context, clodGroup p_group, const clodCluster *p_clusters, size_t p_cluster_count) {
	Builder &builder = *static_cast<Builder *>(p_context);
	if (builder.error != OK) {
		return 0;
	}
	uint32_t group_id = builder.data.groups.size();
	Data::Group group;
	group.first_cluster = builder.data.clusters.size();
	group.cluster_count = p_cluster_count;
	group.depth = p_group.depth;
	group.bounds = copy_bounds(p_group.simplified);
	builder.data.groups.push_back(group);
	if (p_group.simplified.error == FLT_MAX) {
		builder.data.terminals.push_back(group_id);
	}
	uint32_t stride = builder.data.surfaces[builder.surface].vertex_stride;
	LocalVector<uint8_t> payload;
	for (size_t i = 0; i < p_cluster_count; i++) {
		const clodCluster &input = p_clusters[i];
		if (input.vertex_count > MAX_VERTICES || input.index_count > MAX_TRIANGLES * 3 || input.index_count % 3) {
			builder.error = ERR_INVALID_DATA;
			return group_id;
		}
		unsigned int local_vertices[MAX_VERTICES];
		unsigned char local_indices[MAX_TRIANGLES * 3];
		size_t vertex_count = clodLocalIndices(local_vertices, local_indices, input.indices, input.index_count);
		if (vertex_count != input.vertex_count) {
			builder.error = ERR_INVALID_DATA;
			return group_id;
		}
		uint32_t triangle_count = input.index_count / 3;
		uint32_t first_primitive = Data::INVALID_ID;
		builder.error = encode_cluster(builder, input, local_vertices, local_indices, vertex_count, triangle_count, input.refined < 0, payload, first_primitive);
		if (builder.error != OK) {
			return group_id;
		}
		uint32_t disk_offset = (builder.page.size() + 3) & ~3u;
		Data::ClusterLayout layout = Data::compute_cluster_layout(builder.memory_size, vertex_count, triangle_count, stride);
		if (disk_offset + payload.size() > Data::MAX_PAGE_SIZE || layout.end > Data::MAX_PAGE_SIZE) {
			builder.error = Data::append_page(builder.data, builder.page);
			if (builder.error != OK) {
				return group_id;
			}
			builder.page.clear();
			builder.memory_size = 0;
			disk_offset = 0;
			layout = Data::compute_cluster_layout(0, vertex_count, triangle_count, stride);
			if (payload.size() > Data::MAX_PAGE_SIZE || layout.end > Data::MAX_PAGE_SIZE) {
				builder.error = ERR_INVALID_DATA;
				return group_id;
			}
		}
		builder.error = builder.page.resize_initialized(disk_offset + payload.size());
		if (builder.error != OK) {
			return group_id;
		}
		memcpy(builder.page.ptrw() + disk_offset, payload.ptr(), payload.size());
		builder.memory_size = layout.end;
		Data::Cluster cluster;
		cluster.surface = builder.surface;
		cluster.group = group_id;
		cluster.refined_group = input.refined < 0 ? Data::INVALID_ID : uint32_t(input.refined);
		cluster.page = builder.data.pages.size();
		cluster.payload_offset = layout.vertices;
		cluster.disk_offset = disk_offset;
		cluster.vertex_count = vertex_count;
		cluster.triangle_count = triangle_count;
		cluster.first_primitive = first_primitive;
		cluster.bounds = copy_bounds(input.bounds);
		builder.data.clusters.push_back(cluster);
	}
	return group_id;
}

Error build_surface(Builder &r_builder, const Array &p_arrays, uint64_t p_format, uint32_t p_surface, const HashSet<Vector3> &p_material_seams) {
	PackedVector3Array positions = p_arrays[Mesh::ARRAY_VERTEX];
	PackedInt32Array source_indices = p_arrays[Mesh::ARRAY_INDEX];
	uint32_t vertex_count = positions.size();
	ERR_FAIL_COND_V(!vertex_count, ERR_INVALID_DATA);
	PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
	PackedColorArray colors = p_arrays[Mesh::ARRAY_COLOR];
	PackedVector2Array uv[2] = { p_arrays[Mesh::ARRAY_TEX_UV], p_arrays[Mesh::ARRAY_TEX_UV2] };
	ERR_FAIL_COND_V((!normals.is_empty() && normals.size() != vertex_count) || (!colors.is_empty() && colors.size() != vertex_count) || (!uv[0].is_empty() && uv[0].size() != vertex_count) || (!uv[1].is_empty() && uv[1].size() != vertex_count), ERR_INVALID_DATA);
	Vector<uint32_t> indices;
	uint32_t index_count = source_indices.is_empty() ? vertex_count : source_indices.size();
	ERR_FAIL_COND_V(index_count % 3 != 0, ERR_INVALID_DATA);
	indices.resize(index_count);
	r_builder.triangle_heads.clear();
	r_builder.triangle_next.resize(index_count / 3);
	for (uint32_t i = 0; i < index_count; i++) {
		uint32_t index = source_indices.is_empty() ? i : uint32_t(source_indices[i]);
		ERR_FAIL_COND_V(index >= vertex_count, ERR_INVALID_DATA);
		indices.write[i] = index;
	}
	for (uint32_t i = 0; i < index_count; i += 3) {
		Vector3i key = triangle_key(indices[i], indices[i + 1], indices[i + 2]);
		const uint32_t *previous = r_builder.triangle_heads.getptr(key);
		r_builder.triangle_next.write[i / 3] = previous ? *previous : Data::INVALID_ID;
		r_builder.triangle_heads[key] = i / 3;
	}
	Data::Surface surface;
	surface.source_surface = p_surface;
	surface.format = p_format;
	surface.source_vertex_count = vertex_count;
	surface.source_triangle_count = index_count / 3;
	memcpy(surface.frame_center, r_builder.data.frame_center, sizeof(surface.frame_center));
	surface.frame_scale = r_builder.data.frame_scale;

	SourceSurface &source = r_builder.source;
	source = SourceSurface();
	source.has_normals = !normals.is_empty();
	source.has_colors = !colors.is_empty();
	for (uint32_t channel = 0; channel < 2; channel++) {
		source.has_uv[channel] = !uv[channel].is_empty();
		if (!source.has_uv[channel]) {
			continue;
		}
		double range[2] = { 0, 0 };
		double minimum[2] = { FLT_MAX, FLT_MAX };
		double maximum[2] = { -FLT_MAX, -FLT_MAX };
		for (uint32_t i = 0; i < vertex_count; i++) {
			for (uint32_t component = 0; component < 2; component++) {
				double value = uv[channel][i][component];
				ERR_FAIL_COND_V(!Math::is_finite(value), ERR_INVALID_DATA);
				minimum[component] = MIN(minimum[component], value);
				maximum[component] = MAX(maximum[component], value);
			}
		}
		bool unorm = true;
		for (uint32_t component = 0; component < 2; component++) {
			range[component] = maximum[component] - minimum[component];
			unorm = unorm && range[component] <= 8.0;
		}
		surface.uv_mode[channel] = unorm ? Data::UV_MODE_UNORM16 : Data::UV_MODE_HALF;
		for (uint32_t component = 0; component < 2; component++) {
			surface.uv_min[channel * 2 + component] = unorm ? float(minimum[component]) : 0.0f;
			surface.uv_scale[channel * 2 + component] = unorm ? float(range[component]) : 0.0f;
		}
	}

	uint32_t components[6] = { 3, source.has_normals ? 3u : 0u, 0, source.has_colors ? 4u : 0u, source.has_uv[0] ? 2u : 0u, source.has_uv[1] ? 2u : 0u };
	uint32_t attribute_count = 0;
	for (uint32_t i = 1; i < 6; i++) {
		attribute_count += components[i];
	}
	surface.attribute_offsets[0] = 0;
	surface.attribute_formats[0] = Data::ATTRIBUTE_POSITION_SNORM16;
	uint32_t offset = 8;
	for (uint32_t i = 1; i < 10; i++) {
		surface.attribute_offsets[i] = Data::INVALID_ID;
		surface.attribute_formats[i] = Data::ATTRIBUTE_NONE;
	}
	if (source.has_normals) {
		surface.attribute_offsets[1] = 6;
		surface.attribute_formats[1] = Data::ATTRIBUTE_NORMAL_OCTAHEDRAL8;
	}
	if (source.has_colors) {
		surface.attribute_offsets[3] = offset;
		surface.attribute_formats[3] = Data::ATTRIBUTE_COLOR_UNORM8;
		offset += 4;
	}
	for (uint32_t channel = 0; channel < 2; channel++) {
		if (!source.has_uv[channel]) {
			continue;
		}
		surface.attribute_offsets[4 + channel] = offset;
		surface.attribute_formats[4 + channel] = surface.uv_mode[channel] == Data::UV_MODE_UNORM16 ? Data::ATTRIBUTE_UV_UNORM16 : Data::ATTRIBUTE_UV_HALF;
		offset += 4;
	}
	bool has_custom = false;
	for (uint32_t channel = 0; channel < 4; channel++) {
		source.custom_size[channel] = Data::custom_attribute_size(p_format, channel);
		if (!source.custom_size[channel]) {
			continue;
		}
		has_custom = true;
		uint32_t format = (p_format >> (RSE::ARRAY_FORMAT_CUSTOM0_SHIFT + RSE::ARRAY_FORMAT_CUSTOM_BITS * channel)) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
		if (format < RSE::ARRAY_CUSTOM_R_FLOAT) {
			source.custom[channel] = p_arrays[Mesh::ARRAY_CUSTOM0 + channel];
		} else {
			PackedFloat32Array input = p_arrays[Mesh::ARRAY_CUSTOM0 + channel];
			ERR_FAIL_COND_V(source.custom[channel].resize(input.size() * 4) != OK, ERR_OUT_OF_MEMORY);
			for (int64_t i = 0; i < input.size(); i++) {
				encode_float(input[i], source.custom[channel].ptrw() + i * 4);
			}
		}
		ERR_FAIL_COND_V(uint64_t(source.custom[channel].size()) != uint64_t(vertex_count) * source.custom_size[channel], ERR_INVALID_DATA);
		surface.attribute_offsets[6 + channel] = offset;
		surface.attribute_formats[6 + channel] = Data::ATTRIBUTE_CUSTOM;
		offset += source.custom_size[channel];
	}
	surface.vertex_stride = (offset + 3) & ~3u;
	ERR_FAIL_COND_V(surface.vertex_stride > 256, ERR_INVALID_DATA);

	Vector<float> position_data;
	Vector<float> attributes;
	Vector<float> weights;
	Vector<uint8_t> locks;
	Vector<uint32_t> position_remap;
	position_data.resize(uint64_t(vertex_count) * 3);
	attributes.resize(uint64_t(vertex_count) * attribute_count);
	weights.resize(attribute_count);
	weights.fill(1.0f);
	locks.resize_initialized(vertex_count);
	position_remap.resize(vertex_count);
	ERR_FAIL_COND_V(source.grid.resize(uint64_t(vertex_count) * 3) != OK, ERR_OUT_OF_MEMORY);
	if (source.has_normals) {
		ERR_FAIL_COND_V(source.normals.resize(uint64_t(vertex_count) * 2) != OK, ERR_OUT_OF_MEMORY);
	}
	if (source.has_colors) {
		ERR_FAIL_COND_V(source.colors.resize(uint64_t(vertex_count) * 4) != OK, ERR_OUT_OF_MEMORY);
	}
	if (source.has_uv[0] || source.has_uv[1]) {
		ERR_FAIL_COND_V(source.uv.resize_initialized(uint64_t(vertex_count) * 4) != OK, ERR_OUT_OF_MEMORY);
	}
	double step = r_builder.position_step;
	for (uint32_t i = 0; i < vertex_count; i++) {
		float values[12] = {};
		uint32_t count = 0;
		for (uint32_t axis = 0; axis < 3; axis++) {
			double value = positions[i][axis];
			ERR_FAIL_COND_V(!Math::is_finite(value), ERR_INVALID_DATA);
			position_data.write[i * 3 + axis] = float(value);
			double quantized = Math::round(value / step);
			ERR_FAIL_COND_V(quantized < double(INT32_MIN) || quantized > double(INT32_MAX), ERR_INVALID_DATA);
			source.grid.write[uint64_t(i) * 3 + axis] = int32_t(quantized);
		}
		if (source.has_normals) {
			encode_octahedral(normals[i], source.normals.ptrw() + uint64_t(i) * 2);
			for (uint32_t axis = 0; axis < 3; axis++) {
				values[count++] = normals[i][axis];
			}
		}
		if (source.has_colors) {
			for (uint32_t component = 0; component < 4; component++) {
				float value = colors[i][component];
				source.colors.write[uint64_t(i) * 4 + component] = uint8_t(CLAMP(Math::round(value * 255.0f), 0.0f, 255.0f));
				values[count++] = value;
			}
		}
		for (uint32_t channel = 0; channel < 2; channel++) {
			if (!source.has_uv[channel]) {
				continue;
			}
			for (uint32_t component = 0; component < 2; component++) {
				float value = uv[channel][i][component];
				uint32_t slot = channel * 2 + component;
				uint16_t code;
				if (surface.uv_mode[channel] == Data::UV_MODE_UNORM16) {
					float scale = surface.uv_scale[slot];
					float normalized = scale > 0 ? (value - surface.uv_min[slot]) / scale : 0.0f;
					code = uint16_t(CLAMP(Math::round(normalized * 65535.0f), 0.0f, 65535.0f));
				} else {
					code = Math::make_half_float(value);
				}
				source.uv.write[uint64_t(i) * 4 + slot] = code;
				values[count++] = value;
			}
		}
		for (uint32_t j = 0; j < count; j++) {
			ERR_FAIL_COND_V(!Math::is_finite(values[j]), ERR_INVALID_DATA);
			attributes.write[uint64_t(i) * attribute_count + j] = values[j];
		}
		if (has_custom || p_material_seams.has(positions[i])) {
			locks.write[i] = meshopt_SimplifyVertex_Lock;
		}
	}
	meshopt_generatePositionRemap(position_remap.ptrw(), position_data.ptr(), vertex_count, 12);
	for (uint32_t i = 0; i < vertex_count; i++) {
		if (position_remap[i] != i) {
			locks.write[position_remap[i]] = meshopt_SimplifyVertex_Lock;
		}
	}
	for (uint32_t i = 0; i < vertex_count; i++) {
		locks.write[i] |= locks[position_remap[i]];
	}
	r_builder.surface = r_builder.data.surfaces.size();
	r_builder.data.surfaces.push_back(surface);
	Data::Permutation permutation;
	permutation.surface = p_surface;
	r_builder.permutations.push_back(permutation);
	r_builder.vertex_ids.clear();
	ERR_FAIL_COND_V(r_builder.vertex_ids.resize(vertex_count) != OK, ERR_OUT_OF_MEMORY);
	r_builder.vertex_ids.fill(Data::INVALID_ID);
	clodMesh input = {};
	input.indices = indices.ptr();
	input.index_count = index_count;
	input.vertex_count = vertex_count;
	input.vertex_positions = position_data.ptr();
	input.vertex_positions_stride = 12;
	input.vertex_attributes = attributes.ptr();
	input.vertex_attributes_stride = attribute_count * 4;
	input.attribute_weights = weights.ptr();
	input.attribute_count = attribute_count;
	input.vertex_lock = locks.ptr();
	clodConfig config = clodDefaultConfig(MAX_TRIANGLES);
	config.max_vertices = MAX_VERTICES;
	config.cluster_spatial = true;
	config.simplify_permissive = false;
	config.simplify_fallback_permissive = false;
	config.simplify_fallback_sloppy = false;
	config.simplify_preserve_folds = true;
	clodBuild(config, input, &r_builder, Builder::output);
	ERR_FAIL_COND_V(r_builder.error != OK, r_builder.error);
	for (const KeyValue<Vector3i, uint32_t> &entry : r_builder.triangle_heads) {
		ERR_FAIL_COND_V(entry.value != Data::INVALID_ID, ERR_INVALID_DATA);
	}
	Data::Permutation &result = r_builder.permutations.write[r_builder.surface];
	ERR_FAIL_COND_V(uint32_t(result.triangles.size()) != surface.source_triangle_count, ERR_INVALID_DATA);
	for (uint32_t i = 0; i < vertex_count; i++) {
		if (r_builder.vertex_ids[i] == Data::INVALID_ID) {
			r_builder.vertex_ids.write[i] = result.vertices.size();
			result.vertices.push_back(i);
		}
	}
	ERR_FAIL_COND_V(uint32_t(result.vertices.size()) != vertex_count, ERR_INVALID_DATA);
	return OK;
}
}

Error build_micro_geometry(const ImporterMesh &p_mesh, float p_position_step, Ref<MicroGeometryData> &r_data, Vector<MicroGeometryData::Permutation> &r_permutations, String &r_error) {
	ERR_FAIL_COND_V(!Math::is_finite(p_position_step) || p_position_step <= 0, ERR_INVALID_PARAMETER);
	uint64_t start = OS::get_singleton()->get_ticks_msec();
	Builder builder;
	builder.position_step = p_position_step;
	builder.edge_head.resize(MAX_VERTICES * MAX_VERTICES);
	for (uint32_t i = 0; i < builder.edge_head.size(); i++) {
		builder.edge_head[i] = -1;
	}
	builder.data.position_step = p_position_step;
	HashSet<Vector3> material_seams;
	if (p_mesh.get_surface_count() > 1) {
		HashMap<Vector3, int> owners;
		for (int i = 0; i < p_mesh.get_surface_count(); i++) {
			PackedVector3Array positions = p_mesh.get_surface_arrays(i)[Mesh::ARRAY_VERTEX];
			for (const Vector3 &position : positions) {
				const int *owner = owners.getptr(position);
				if (owner && *owner != i) {
					material_seams.insert(position);
				} else {
					owners.insert(position, i);
				}
			}
		}
	}
	AABB frame;
	bool frame_valid = false;
	for (int i = 0; i < p_mesh.get_surface_count(); i++) {
		PackedVector3Array positions = p_mesh.get_surface_arrays(i)[Mesh::ARRAY_VERTEX];
		for (const Vector3 &position : positions) {
			ERR_FAIL_COND_V(!position.is_finite(), ERR_INVALID_DATA);
			if (frame_valid) {
				frame.expand_to(position);
			} else {
				frame.position = position;
				frame_valid = true;
			}
		}
	}
	ERR_FAIL_COND_V(!frame_valid, ERR_INVALID_DATA);
	Vector3 center = frame.get_center();
	Vector3 extent = frame.size * 0.5;
	for (int i = 0; i < 3; i++) {
		builder.data.frame_center[i] = float(center[i]);
	}
	builder.data.frame_scale = MAX(float(MAX(extent.x, MAX(extent.y, extent.z))), 0.000001f);
	for (int i = 0; i < p_mesh.get_surface_count(); i++) {
		Error err = build_surface(builder, p_mesh.get_surface_arrays(i), p_mesh.get_surface_format(i), i, material_seams);
		if (err != OK) {
			r_error = vformat("Mesh '%s', surface %d: microgeometry construction failed (%d).", p_mesh.get_name(), i, err);
			return err;
		}
	}
	if (!builder.page.is_empty()) {
		Error err = Data::append_page(builder.data, builder.page);
		ERR_FAIL_COND_V(err != OK, err);
	}
	Vector<clodGroup> groups;
	Vector<Vector<uint32_t>> parents;
	parents.resize(builder.data.groups.size());
	for (const Data::Cluster &cluster : builder.data.clusters) {
		if (cluster.refined_group != Data::INVALID_ID && !parents[cluster.refined_group].has(cluster.group)) {
			parents.write[cluster.refined_group].push_back(cluster.group);
		}
	}
	for (int i = 0; i < builder.data.groups.size(); i++) {
		Data::Group &group = builder.data.groups.write[i];
		group.first_parent = builder.data.parent_groups.size();
		group.parent_count = parents[i].size();
		builder.data.parent_groups.append_array(parents[i]);
	}
	for (uint32_t terminal : builder.data.terminals) {
		builder.data.coarse_cluster_count += builder.data.groups[terminal].cluster_count;
	}
	groups.resize(builder.data.groups.size());
	uint32_t levels = 0;
	for (int i = 0; i < groups.size(); i++) {
		const Data::Group &source = builder.data.groups[i];
		clodGroup &group = groups.write[i];
		group.depth = source.depth;
		memcpy(group.simplified.center, source.bounds.center, sizeof(source.bounds.center));
		group.simplified.radius = source.bounds.radius;
		group.simplified.error = source.bounds.error;
		levels = MAX(levels, source.depth + 1);
	}
	Vector<clodNode> nodes;
	nodes.resize(clodBuildHierarchyBound(groups.size(), 8, levels));
	size_t node_count = clodBuildHierarchy(nodes.ptrw(), groups.ptr(), groups.size(), 8, levels);
	for (uint32_t i = 0; i < levels; i++) {
		builder.data.roots.push_back(i);
	}
	for (size_t i = 0; i < node_count; i++) {
		Data::Node node;
		node.group = nodes[i].group < 0 ? Data::INVALID_ID : uint32_t(nodes[i].group);
		node.first_child = nodes[i].child_offset;
		node.child_count = nodes[i].child_count;
		node.bounds = copy_bounds(nodes[i].bounds);
		builder.data.nodes.push_back(node);
	}
	Error err = Data::create(builder.data, r_data);
	if (err != OK) {
		r_error = vformat("Mesh '%s': invalid microgeometry manifest (%d).", p_mesh.get_name(), err);
		return err;
	}
	r_permutations = builder.permutations;
	uint64_t bytes = 0;
	uint64_t decoded = 0;
	uint64_t leaves = 0;
	uint64_t triangles = 0;
	for (const Data::Page &page : builder.data.pages) {
		bytes += page.encoded_size;
		decoded += page.decoded_size;
	}
	for (const Data::Cluster &cluster : builder.data.clusters) {
		triangles += cluster.triangle_count;
		if (cluster.refined_group == Data::INVALID_ID) {
			leaves++;
		}
	}
	print_verbose(vformat("MicroGeometry '%s': %d leaf / %d total clusters, %d groups, %d levels, %d terminals, %d pages, %d encoded bytes (%.2f B/tri, %.2f B/tri decoded), %d ms", p_mesh.get_name(), leaves, builder.data.clusters.size(), groups.size(), levels, builder.data.terminals.size(), builder.data.pages.size(), bytes, triangles ? double(bytes) / double(triangles) : 0.0, triangles ? double(decoded) / double(triangles) : 0.0, OS::get_singleton()->get_ticks_msec() - start));
	return OK;
}

#endif
