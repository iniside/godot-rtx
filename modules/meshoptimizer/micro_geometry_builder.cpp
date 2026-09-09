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
#include "core/math/vector3i.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#define CLUSTERLOD_IMPLEMENTATION
#include "thirdparty/meshoptimizer/clusterlod.h"

namespace {
using Data = MicroGeometryData;

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

struct Builder {
	Data::Build data;
	Vector<uint8_t> page;
	Vector<uint8_t> vertices;
	HashMap<Vector3i, uint32_t> triangle_heads;
	Vector<uint32_t> triangle_next;
	uint32_t surface = 0;
	Error error = OK;

	static int output(void *p_context, clodGroup p_group, const clodCluster *p_clusters, size_t p_cluster_count) {
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
		for (size_t i = 0; i < p_cluster_count; i++) {
			const clodCluster &input = p_clusters[i];
			if (input.vertex_count > 128 || input.index_count > 384 || input.index_count % 3) {
				builder.error = ERR_INVALID_DATA;
				return group_id;
			}
			unsigned int local_vertices[128];
			unsigned char local_indices[384];
			size_t vertex_count = clodLocalIndices(local_vertices, local_indices, input.indices, input.index_count);
			if (vertex_count != input.vertex_count) {
				builder.error = ERR_INVALID_DATA;
				return group_id;
			}
			uint32_t triangle_count = input.index_count / 3;
			uint32_t size = vertex_count * stride + triangle_count * 7;
			uint32_t offset = (builder.page.size() + 3) & ~3u;
			if (offset + size > Data::MAX_PAGE_SIZE) {
				builder.error = Data::append_page(builder.data, builder.page);
				if (builder.error != OK) {
					return group_id;
				}
				builder.page.clear();
				offset = 0;
			}
			builder.error = builder.page.resize_initialized(offset + size);
			if (builder.error != OK) {
				return group_id;
			}
			uint8_t *payload = builder.page.ptrw() + offset;
			for (uint32_t j = 0; j < vertex_count; j++) {
				memcpy(payload + j * stride, builder.vertices.ptr() + uint64_t(local_vertices[j]) * stride, stride);
			}
			memcpy(payload + vertex_count * stride, local_indices, input.index_count);
			uint8_t *identities = payload + vertex_count * stride + input.index_count;
			for (uint32_t j = 0; j < triangle_count; j++) {
				uint32_t primitive = Data::INVALID_ID;
				if (input.refined < 0) {
					Vector3i key = triangle_key(input.indices[j * 3], input.indices[j * 3 + 1], input.indices[j * 3 + 2]);
					uint32_t *head = builder.triangle_heads.getptr(key);
					if (!head || *head == Data::INVALID_ID) {
						builder.error = ERR_INVALID_DATA;
						return group_id;
					}
					primitive = *head;
					*head = builder.triangle_next[primitive];
				}
				encode_uint32(primitive, identities + j * 4);
			}
			Data::Cluster cluster;
			cluster.surface = builder.surface;
			cluster.group = group_id;
			cluster.refined_group = input.refined < 0 ? Data::INVALID_ID : uint32_t(input.refined);
			cluster.page = builder.data.pages.size();
			cluster.payload_offset = offset;
			cluster.vertex_count = vertex_count;
			cluster.triangle_count = triangle_count;
			cluster.bounds = copy_bounds(input.bounds);
			builder.data.clusters.push_back(cluster);
		}
		return group_id;
	}
};

Error build_surface(Builder &r_builder, const Array &p_arrays, uint64_t p_format, uint32_t p_surface, const HashSet<Vector3> &p_material_seams) {
	PackedVector3Array positions = p_arrays[Mesh::ARRAY_VERTEX];
	PackedInt32Array source_indices = p_arrays[Mesh::ARRAY_INDEX];
	uint32_t vertex_count = positions.size();
	ERR_FAIL_COND_V(!vertex_count, ERR_INVALID_DATA);
	PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
	PackedFloat32Array tangents = p_arrays[Mesh::ARRAY_TANGENT];
	PackedColorArray colors = p_arrays[Mesh::ARRAY_COLOR];
	PackedVector2Array uv = p_arrays[Mesh::ARRAY_TEX_UV];
	PackedVector2Array uv2 = p_arrays[Mesh::ARRAY_TEX_UV2];
	ERR_FAIL_COND_V((!normals.is_empty() && normals.size() != vertex_count) || (!tangents.is_empty() && tangents.size() != uint64_t(vertex_count) * 4) || (!colors.is_empty() && colors.size() != vertex_count) || (!uv.is_empty() && uv.size() != vertex_count) || (!uv2.is_empty() && uv2.size() != vertex_count), ERR_INVALID_DATA);
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
	uint32_t components[6] = { 3, normals.is_empty() ? 0u : 3u, tangents.is_empty() ? 0u : 4u, colors.is_empty() ? 0u : 4u, uv.is_empty() ? 0u : 2u, uv2.is_empty() ? 0u : 2u };
	for (int i = 0; i < 6; i++) {
		surface.attribute_offsets[i] = components[i] ? surface.vertex_stride : Data::INVALID_ID;
		surface.vertex_stride += components[i] * 4;
	}
	uint32_t attribute_count = surface.vertex_stride / 4 - 3;
	Vector<uint8_t> custom[4];
	uint32_t custom_size[4] = {};
	bool has_custom = false;
	for (int i = 0; i < 4; i++) {
		surface.attribute_offsets[6 + i] = Data::INVALID_ID;
		if (!(p_format & (uint64_t(Mesh::ARRAY_FORMAT_CUSTOM0) << i))) {
			continue;
		}
		has_custom = true;
		uint32_t format = (p_format >> (RSE::ARRAY_FORMAT_CUSTOM0_SHIFT + RSE::ARRAY_FORMAT_CUSTOM_BITS * i)) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
		const uint32_t sizes[8] = { 4, 4, 4, 8, 4, 8, 12, 16 };
		custom_size[i] = sizes[format];
		if (format < RSE::ARRAY_CUSTOM_R_FLOAT) {
			custom[i] = p_arrays[Mesh::ARRAY_CUSTOM0 + i];
		} else {
			PackedFloat32Array input = p_arrays[Mesh::ARRAY_CUSTOM0 + i];
			custom[i].resize(input.size() * 4);
			for (int64_t j = 0; j < input.size(); j++) {
				encode_float(input[j], custom[i].ptrw() + j * 4);
			}
		}
		ERR_FAIL_COND_V(uint64_t(custom[i].size()) != uint64_t(vertex_count) * custom_size[i], ERR_INVALID_DATA);
		surface.attribute_offsets[6 + i] = surface.vertex_stride;
		surface.vertex_stride += custom_size[i];
	}
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
	ERR_FAIL_COND_V(r_builder.vertices.resize(uint64_t(vertex_count) * surface.vertex_stride) != OK, ERR_OUT_OF_MEMORY);
	for (uint32_t i = 0; i < vertex_count; i++) {
		uint8_t *vertex = r_builder.vertices.ptrw() + uint64_t(i) * surface.vertex_stride;
		float values[18] = {};
		uint32_t count = 0;
		for (int j = 0; j < 3; j++) {
			float value = positions[i][j];
			ERR_FAIL_COND_V(!Math::is_finite(value), ERR_INVALID_DATA);
			position_data.write[i * 3 + j] = value;
			encode_float(value, vertex + j * 4);
		}
		if (!normals.is_empty()) {
			for (int j = 0; j < 3; j++) {
				values[count++] = normals[i][j];
			}
		}
		if (!tangents.is_empty()) {
			for (int j = 0; j < 4; j++) {
				values[count++] = tangents[i * 4 + j];
			}
		}
		if (!colors.is_empty()) {
			for (int j = 0; j < 4; j++) {
				values[count++] = colors[i][j];
			}
		}
		if (!uv.is_empty()) {
			for (int j = 0; j < 2; j++) {
				values[count++] = uv[i][j];
			}
		}
		if (!uv2.is_empty()) {
			for (int j = 0; j < 2; j++) {
				values[count++] = uv2[i][j];
			}
		}
		for (uint32_t j = 0; j < count; j++) {
			ERR_FAIL_COND_V(!Math::is_finite(values[j]), ERR_INVALID_DATA);
			attributes.write[uint64_t(i) * attribute_count + j] = values[j];
			encode_float(values[j], vertex + 12 + j * 4);
		}
		for (int j = 0; j < 4; j++) {
			if (custom_size[j]) {
				memcpy(vertex + surface.attribute_offsets[6 + j], custom[j].ptr() + uint64_t(i) * custom_size[j], custom_size[j]);
			}
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
	clodConfig config = clodDefaultConfig(128);
	config.max_vertices = 128;
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
	return OK;
}
}

Error build_micro_geometry(const ImporterMesh &p_mesh, Ref<MicroGeometryData> &r_data, String &r_error) {
	uint64_t start = OS::get_singleton()->get_ticks_msec();
	Builder builder;
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
	uint64_t bytes = 0;
	uint64_t leaves = 0;
	for (const Data::Page &page : builder.data.pages) {
		bytes += page.encoded_size;
	}
	for (const Data::Cluster &cluster : builder.data.clusters) {
		if (cluster.refined_group == Data::INVALID_ID) {
			leaves++;
		}
	}
	print_verbose(vformat("MicroGeometry '%s': %d leaf / %d total clusters, %d groups, %d levels, %d terminals, %d pages, %d encoded bytes, %d ms", p_mesh.get_name(), leaves, builder.data.clusters.size(), groups.size(), levels, builder.data.terminals.size(), builder.data.pages.size(), bytes, OS::get_singleton()->get_ticks_msec() - start));
	return OK;
}

#endif
