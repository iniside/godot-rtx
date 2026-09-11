/**************************************************************************/
/*  micro_geometry_data.cpp                                               */
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

#include "micro_geometry_data.h"

#include "core/crypto/crypto_core.h"
#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cfloat>

namespace {
constexpr uint32_t MAGIC = 0x4144474d;
constexpr uint32_t HEADER_SIZE = 96;
constexpr uint32_t MANIFEST_PROLOGUE_SIZE = 56;
constexpr uint32_t UV_SPLIT_FLAG = MicroGeometryData::UV_SPLIT_FLAG;

struct ManifestWriter {
	Vector<uint8_t> data;
	void u32(uint32_t p_value) {
		int64_t offset = data.size();
		data.resize(offset + 4);
		encode_uint32(p_value, data.ptrw() + offset);
	}
	void u64(uint64_t p_value) {
		u32(p_value);
		u32(p_value >> 32);
	}
	void f32(float p_value) {
		uint32_t bits;
		memcpy(&bits, &p_value, 4);
		u32(bits);
	}
	void bounds(const MicroGeometryData::Bounds &p_bounds) {
		for (float value : p_bounds.center) {
			f32(value);
		}
		f32(p_bounds.radius);
		f32(p_bounds.error);
	}
};

struct ManifestReader {
	const uint8_t *data;
	uint64_t offset = 0;
	uint32_t u32() {
		uint32_t result = decode_uint32(data + offset);
		offset += 4;
		return result;
	}
	uint64_t u64() {
		uint64_t lo = u32();
		return lo | (uint64_t(u32()) << 32);
	}
	float f32() {
		uint32_t bits = u32();
		float result;
		memcpy(&result, &bits, 4);
		return result;
	}
	MicroGeometryData::Bounds bounds() {
		MicroGeometryData::Bounds result;
		for (float &value : result.center) {
			value = f32();
		}
		result.radius = f32();
		result.error = f32();
		return result;
	}
};

bool valid_bounds(const MicroGeometryData::Bounds &p_bounds) {
	for (float value : p_bounds.center) {
		if (!Math::is_finite(value)) {
			return false;
		}
	}
	return Math::is_finite(p_bounds.radius) && p_bounds.radius >= 0 && Math::is_finite(p_bounds.error) && p_bounds.error >= 0;
}
}

uint32_t MicroGeometryData::custom_attribute_size(uint64_t p_format, uint32_t p_index) {
	if (p_index >= 4 || !(p_format & (uint64_t(RSE::ARRAY_FORMAT_CUSTOM0) << p_index))) {
		return 0;
	}
	const uint32_t sizes[8] = { 4, 4, 4, 8, 4, 8, 12, 16 };
	return sizes[(p_format >> (RSE::ARRAY_FORMAT_CUSTOM0_SHIFT + RSE::ARRAY_FORMAT_CUSTOM_BITS * p_index)) & RSE::ARRAY_FORMAT_CUSTOM_MASK];
}

MicroGeometryData::ClusterLayout MicroGeometryData::compute_cluster_layout(uint32_t p_payload_offset, uint32_t p_vertex_count, uint32_t p_triangle_count, uint32_t p_vertex_stride) {
	ClusterLayout layout;
	layout.vertices = (p_payload_offset + 3) & ~3u;
	layout.indices = (layout.vertices + p_vertex_count * p_vertex_stride + 3) & ~3u;
	layout.vertex_ids = (layout.indices + p_triangle_count * 3 + 3) & ~3u;
	layout.end = layout.vertex_ids + p_vertex_count * 4;
	return layout;
}

MicroGeometryData::ClusterLayout MicroGeometryData::get_cluster_layout(uint32_t p_cluster) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_cluster, uint32_t(metadata.clusters.size()), ClusterLayout());
	const Cluster &cluster = metadata.clusters[p_cluster];
	return compute_cluster_layout(cluster.payload_offset, cluster.vertex_count, cluster.triangle_count, metadata.surfaces[cluster.surface].vertex_stride);
}

uint32_t MicroGeometryData::cluster_header_size(const Surface &p_surface) {
	uint32_t size = CLUSTER_HEADER_SIZE;
	for (uint32_t mode : p_surface.uv_mode) {
		if (mode != UV_MODE_NONE) {
			size += CLUSTER_UV_HEADER_SIZE * 2;
		}
	}
	return size;
}

uint32_t MicroGeometryData::cluster_disk_size(const Surface &p_surface, uint32_t p_vertex_count, const uint8_t p_position_bits[3], const uint8_t p_uv_bits[4], uint32_t p_vertex_id_bits, uint32_t p_topology_bytes) {
	uint64_t size = cluster_header_size(p_surface);
	size += (uint64_t(p_vertex_count) * (uint32_t(p_position_bits[0]) + p_position_bits[1] + p_position_bits[2]) + 7) / 8;
	if (p_surface.attribute_offsets[1] != INVALID_ID) {
		size += uint64_t(p_vertex_count) * 2;
	}
	if (p_surface.attribute_offsets[3] != INVALID_ID) {
		size += uint64_t(p_vertex_count) * 4;
	}
	for (uint32_t channel = 0; channel < 2; channel++) {
		if (p_surface.uv_mode[channel] != UV_MODE_NONE) {
			size += (uint64_t(p_vertex_count) * (uint32_t(p_uv_bits[channel * 2]) + p_uv_bits[channel * 2 + 1]) + 7) / 8;
		}
	}
	for (uint32_t channel = 0; channel < 4; channel++) {
		size += uint64_t(p_vertex_count) * custom_attribute_size(p_surface.format, channel);
	}
	size += p_topology_bytes;
	if (p_vertex_count) {
		size += (32 + uint64_t(p_vertex_count - 1) * p_vertex_id_bits + 7) / 8;
	}
	return uint32_t(size);
}

Vector<uint8_t> MicroGeometryData::encode_manifest() const {
	ManifestWriter w;
	w.u32(metadata.surfaces.size());
	w.u32(metadata.clusters.size());
	w.u32(metadata.groups.size());
	w.u32(metadata.terminals.size());
	w.u32(metadata.pages.size());
	w.u32(metadata.nodes.size());
	w.u32(metadata.roots.size());
	w.u32(metadata.parent_groups.size());
	w.u32(metadata.coarse_cluster_count);
	for (float value : metadata.frame_center) {
		w.f32(value);
	}
	w.f32(metadata.frame_scale);
	w.f32(metadata.position_step);
	for (const Surface &s : metadata.surfaces) {
		w.u32(s.source_surface);
		w.u64(s.format);
		w.u32(s.vertex_stride);
		for (uint32_t offset : s.attribute_offsets) {
			w.u32(offset);
		}
		for (uint32_t format : s.attribute_formats) {
			w.u32(format);
		}
		w.u32(s.source_vertex_count);
		w.u32(s.source_triangle_count);
		for (float value : s.frame_center) {
			w.f32(value);
		}
		w.f32(s.frame_scale);
		for (uint32_t mode : s.uv_mode) {
			w.u32(mode);
		}
		for (float value : s.uv_min) {
			w.f32(value);
		}
		for (float value : s.uv_scale) {
			w.f32(value);
		}
	}
	for (const Cluster &c : metadata.clusters) {
		w.u32(c.surface);
		w.u32(c.group);
		w.u32(c.refined_group);
		w.u32(c.page);
		w.u32(c.payload_offset);
		w.u32(c.disk_offset);
		w.u32(c.vertex_count);
		w.u32(c.triangle_count);
		w.u32(c.first_primitive);
		w.bounds(c.bounds);
	}
	for (const Group &g : metadata.groups) {
		w.u32(g.first_cluster);
		w.u32(g.cluster_count);
		w.u32(g.depth);
		w.u32(g.first_parent);
		w.u32(g.parent_count);
		w.bounds(g.bounds);
	}
	for (uint32_t terminal : metadata.terminals) {
		w.u32(terminal);
	}
	for (const Page &p : metadata.pages) {
		w.u64(p.offset);
		w.u32(p.encoded_size);
		w.u32(p.decoded_size);
		w.u32(p.first_cluster);
		w.u32(p.cluster_count);
		for (int i = 0; i < 8; i++) {
			w.u32(decode_uint32(p.digest + i * 4));
		}
	}
	for (const Node &n : metadata.nodes) {
		w.u32(n.group);
		w.u32(n.first_child);
		w.u32(n.child_count);
		w.bounds(n.bounds);
	}
	for (uint32_t root : metadata.roots) {
		w.u32(root);
	}
	for (uint32_t parent : metadata.parent_groups) {
		w.u32(parent);
	}
	return w.data;
}

Error MicroGeometryData::decode_manifest(const Vector<uint8_t> &p_manifest) {
	ERR_FAIL_COND_V(p_manifest.size() < MANIFEST_PROLOGUE_SIZE, ERR_FILE_CORRUPT);
	ManifestReader r{ p_manifest.ptr() };
	uint32_t sizes[8];
	for (uint32_t &size : sizes) {
		size = r.u32();
	}
	metadata.coarse_cluster_count = r.u32();
	for (float &value : metadata.frame_center) {
		value = r.f32();
	}
	metadata.frame_scale = r.f32();
	metadata.position_step = r.f32();
	uint64_t expected = MANIFEST_PROLOGUE_SIZE + uint64_t(sizes[0]) * 160 + uint64_t(sizes[1]) * 56 + uint64_t(sizes[2]) * 40 + uint64_t(sizes[3]) * 4 + uint64_t(sizes[4]) * 56 + uint64_t(sizes[5]) * 32 + uint64_t(sizes[6]) * 4 + uint64_t(sizes[7]) * 4;
	ERR_FAIL_COND_V(expected != uint64_t(p_manifest.size()), ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(metadata.surfaces.resize(sizes[0]) != OK || metadata.clusters.resize(sizes[1]) != OK || metadata.groups.resize(sizes[2]) != OK || metadata.terminals.resize(sizes[3]) != OK || metadata.pages.resize(sizes[4]) != OK || metadata.nodes.resize(sizes[5]) != OK || metadata.roots.resize(sizes[6]) != OK, ERR_OUT_OF_MEMORY);
	ERR_FAIL_COND_V(metadata.parent_groups.resize(sizes[7]) != OK, ERR_OUT_OF_MEMORY);
	for (Surface &s : metadata.surfaces) {
		s.source_surface = r.u32();
		s.format = r.u64();
		s.vertex_stride = r.u32();
		for (uint32_t &offset : s.attribute_offsets) {
			offset = r.u32();
		}
		for (uint32_t &format : s.attribute_formats) {
			format = r.u32();
		}
		s.source_vertex_count = r.u32();
		s.source_triangle_count = r.u32();
		for (float &value : s.frame_center) {
			value = r.f32();
		}
		s.frame_scale = r.f32();
		for (uint32_t &mode : s.uv_mode) {
			mode = r.u32();
		}
		for (float &value : s.uv_min) {
			value = r.f32();
		}
		for (float &value : s.uv_scale) {
			value = r.f32();
		}
	}
	for (Cluster &c : metadata.clusters) {
		c.surface = r.u32();
		c.group = r.u32();
		c.refined_group = r.u32();
		c.page = r.u32();
		c.payload_offset = r.u32();
		c.disk_offset = r.u32();
		c.vertex_count = r.u32();
		c.triangle_count = r.u32();
		c.first_primitive = r.u32();
		c.bounds = r.bounds();
	}
	for (Group &g : metadata.groups) {
		g.first_cluster = r.u32();
		g.cluster_count = r.u32();
		g.depth = r.u32();
		g.first_parent = r.u32();
		g.parent_count = r.u32();
		g.bounds = r.bounds();
	}
	for (uint32_t &terminal : metadata.terminals) {
		terminal = r.u32();
	}
	for (Page &p : metadata.pages) {
		p.offset = r.u64();
		p.encoded_size = r.u32();
		p.decoded_size = r.u32();
		p.first_cluster = r.u32();
		p.cluster_count = r.u32();
		for (int i = 0; i < 8; i++) {
			encode_uint32(r.u32(), p.digest + i * 4);
		}
	}
	for (Node &n : metadata.nodes) {
		n.group = r.u32();
		n.first_child = r.u32();
		n.child_count = r.u32();
		n.bounds = r.bounds();
	}
	for (uint32_t &root : metadata.roots) {
		root = r.u32();
	}
	for (uint32_t &parent : metadata.parent_groups) {
		parent = r.u32();
	}
	return validate();
}

Error MicroGeometryData::validate() const {
	ERR_FAIL_COND_V(metadata.surfaces.is_empty() || metadata.groups.is_empty() || metadata.terminals.is_empty() || metadata.roots.is_empty(), ERR_INVALID_DATA);
	Vector<uint8_t> refined;
	refined.resize_initialized(metadata.groups.size());
	ERR_FAIL_COND_V(!Math::is_finite(metadata.position_step) || metadata.position_step <= 0 || !Math::is_finite(metadata.frame_scale) || metadata.frame_scale <= 0, ERR_INVALID_DATA);
	for (float value : metadata.frame_center) {
		ERR_FAIL_COND_V(!Math::is_finite(value), ERR_INVALID_DATA);
	}
	for (const Surface &s : metadata.surfaces) {
		ERR_FAIL_COND_V(s.vertex_stride < 8 || s.vertex_stride > 256 || (s.vertex_stride & 3) || s.source_vertex_count == 0 || s.source_triangle_count == 0, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(s.attribute_offsets[0] != 0 || s.attribute_formats[0] != ATTRIBUTE_POSITION_SNORM16, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(s.attribute_offsets[2] != INVALID_ID || s.attribute_formats[2] != ATTRIBUTE_NONE, ERR_INVALID_DATA);
		for (int i = 0; i < 10; i++) {
			ERR_FAIL_COND_V(s.attribute_offsets[i] != INVALID_ID && s.attribute_offsets[i] >= s.vertex_stride, ERR_INVALID_DATA);
			ERR_FAIL_COND_V((s.attribute_offsets[i] == INVALID_ID) != (s.attribute_formats[i] == ATTRIBUTE_NONE), ERR_INVALID_DATA);
		}
		ERR_FAIL_COND_V(s.frame_scale != metadata.frame_scale || memcmp(s.frame_center, metadata.frame_center, sizeof(s.frame_center)) != 0, ERR_INVALID_DATA);
		for (int i = 0; i < 2; i++) {
			uint32_t uv_format = s.uv_mode[i] == UV_MODE_UNORM16 ? uint32_t(ATTRIBUTE_UV_UNORM16) : (s.uv_mode[i] == UV_MODE_HALF ? uint32_t(ATTRIBUTE_UV_HALF) : uint32_t(ATTRIBUTE_NONE));
			ERR_FAIL_COND_V(s.uv_mode[i] > UV_MODE_HALF || s.attribute_formats[4 + i] != uv_format, ERR_INVALID_DATA);
			for (int j = 0; j < 2; j++) {
				ERR_FAIL_COND_V(!Math::is_finite(s.uv_min[i * 2 + j]) || !Math::is_finite(s.uv_scale[i * 2 + j]) || s.uv_scale[i * 2 + j] < 0, ERR_INVALID_DATA);
			}
		}
	}
	Vector<uint32_t> primitive_end;
	ERR_FAIL_COND_V(primitive_end.resize_initialized(metadata.surfaces.size()) != OK, ERR_OUT_OF_MEMORY);
	uint64_t coarse_clusters = 0;
	for (uint32_t terminal : metadata.terminals) {
		ERR_FAIL_COND_V(terminal >= uint32_t(metadata.groups.size()), ERR_INVALID_DATA);
		coarse_clusters += metadata.groups[terminal].cluster_count;
	}
	ERR_FAIL_COND_V(coarse_clusters != metadata.coarse_cluster_count, ERR_INVALID_DATA);
	uint64_t cluster_end = 0;
	uint64_t parent_end = 0;
	for (int i = 0; i < metadata.groups.size(); i++) {
		const Group &g = metadata.groups[i];
		ERR_FAIL_COND_V(g.first_parent != parent_end || g.depth >= uint32_t(metadata.roots.size()), ERR_INVALID_DATA);
		parent_end += g.parent_count;
		ERR_FAIL_COND_V(parent_end > uint64_t(metadata.parent_groups.size()), ERR_INVALID_DATA);
		for (uint64_t j = g.first_parent; j < parent_end; j++) {
			uint32_t parent = metadata.parent_groups[j];
			ERR_FAIL_COND_V(parent <= uint32_t(i) || parent >= uint32_t(metadata.groups.size()) || metadata.groups[parent].depth <= g.depth, ERR_INVALID_DATA);
			for (uint64_t k = g.first_parent; k < j; k++) {
				ERR_FAIL_COND_V(metadata.parent_groups[k] == parent, ERR_INVALID_DATA);
			}
			const Group &parent_group = metadata.groups[parent];
			ERR_FAIL_COND_V(uint64_t(parent_group.first_cluster) + parent_group.cluster_count > uint64_t(metadata.clusters.size()), ERR_INVALID_DATA);
			bool found = false;
			for (uint32_t k = 0; k < parent_group.cluster_count; k++) {
				found |= metadata.clusters[parent_group.first_cluster + k].refined_group == uint32_t(i);
			}
			ERR_FAIL_COND_V(!found, ERR_INVALID_DATA);
		}
		ERR_FAIL_COND_V(g.first_cluster != cluster_end || !g.cluster_count || !valid_bounds(g.bounds), ERR_INVALID_DATA);
		cluster_end += g.cluster_count;
		ERR_FAIL_COND_V(cluster_end > uint64_t(metadata.clusters.size()), ERR_INVALID_DATA);
		for (uint64_t j = g.first_cluster; j < cluster_end; j++) {
			const Cluster &c = metadata.clusters[j];
			ERR_FAIL_COND_V(c.group != uint32_t(i) || c.surface >= uint32_t(metadata.surfaces.size()) || c.page >= uint32_t(metadata.pages.size()) || !valid_bounds(c.bounds), ERR_INVALID_DATA);
			ERR_FAIL_COND_V(!c.vertex_count || c.vertex_count > MAX_CLUSTER_VERTICES || !c.triangle_count || c.triangle_count > MAX_CLUSTER_TRIANGLES, ERR_INVALID_DATA);
			const Surface &surface = metadata.surfaces[c.surface];
			ClusterLayout layout = compute_cluster_layout(c.payload_offset, c.vertex_count, c.triangle_count, surface.vertex_stride);
			ERR_FAIL_COND_V(layout.vertices != c.payload_offset || layout.end > MAX_PAGE_SIZE, ERR_INVALID_DATA);
			ERR_FAIL_COND_V((c.disk_offset & 3) || uint64_t(c.disk_offset) + cluster_header_size(surface) > metadata.pages[c.page].decoded_size, ERR_INVALID_DATA);
			if (c.refined_group == INVALID_ID) {
				ERR_FAIL_COND_V(c.first_primitive != primitive_end[c.surface], ERR_INVALID_DATA);
				primitive_end.write[c.surface] += c.triangle_count;
				ERR_FAIL_COND_V(primitive_end[c.surface] > surface.source_triangle_count, ERR_INVALID_DATA);
			} else {
				ERR_FAIL_COND_V(c.first_primitive != INVALID_ID, ERR_INVALID_DATA);
			}
			if (c.refined_group != INVALID_ID) {
				ERR_FAIL_COND_V(c.refined_group >= uint32_t(i), ERR_INVALID_DATA);
				const Group &child = metadata.groups[c.refined_group];
				bool found = false;
				for (uint32_t k = 0; k < child.parent_count; k++) {
					found |= metadata.parent_groups[child.first_parent + k] == uint32_t(i);
				}
				ERR_FAIL_COND_V(!found, ERR_INVALID_DATA);
				refined.write[c.refined_group] = 1;
			}
		}
	}
	ERR_FAIL_COND_V(cluster_end != uint64_t(metadata.clusters.size()), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(parent_end != uint64_t(metadata.parent_groups.size()), ERR_INVALID_DATA);
	for (int i = 0; i < metadata.surfaces.size(); i++) {
		ERR_FAIL_COND_V(primitive_end[i] != metadata.surfaces[i].source_triangle_count, ERR_INVALID_DATA);
	}
	for (uint32_t terminal : metadata.terminals) {
		ERR_FAIL_COND_V(terminal >= uint32_t(refined.size()) || refined[terminal] != 0 || metadata.groups[terminal].bounds.error != FLT_MAX, ERR_INVALID_DATA);
		refined.write[terminal] = 2;
	}
	for (uint8_t used : refined) {
		ERR_FAIL_COND_V(used == 0, ERR_INVALID_DATA);
	}
	uint64_t page_end = 0;
	uint64_t page_cluster_end = 0;
	for (const Page &page : metadata.pages) {
		ERR_FAIL_COND_V(page.offset != page_end || !page.encoded_size || !page.decoded_size || page.decoded_size > MAX_PAGE_SIZE || page.encoded_size > MAX_PAGE_SIZE * 2, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(page.first_cluster != page_cluster_end || !page.cluster_count, ERR_INVALID_DATA);
		page_cluster_end += page.cluster_count;
		ERR_FAIL_COND_V(page_cluster_end > uint64_t(metadata.clusters.size()), ERR_INVALID_DATA);
		for (uint64_t i = page.first_cluster; i < page_cluster_end; i++) {
			ERR_FAIL_COND_V(metadata.pages[metadata.clusters[i].page].offset != page.offset, ERR_INVALID_DATA);
		}
		page_end += page.encoded_size;
	}
	ERR_FAIL_COND_V(page_cluster_end != uint64_t(metadata.clusters.size()), ERR_INVALID_DATA);
	for (int i = 0; i < metadata.nodes.size(); i++) {
		const Node &n = metadata.nodes[i];
		ERR_FAIL_COND_V(!valid_bounds(n.bounds), ERR_INVALID_DATA);
		if (n.group != INVALID_ID) {
			ERR_FAIL_COND_V(n.group >= uint32_t(metadata.groups.size()) || n.child_count, ERR_INVALID_DATA);
		} else {
			ERR_FAIL_COND_V(!n.child_count || n.first_child == uint32_t(i) || uint64_t(n.first_child) + n.child_count > uint64_t(metadata.nodes.size()), ERR_INVALID_DATA);
		}
	}
	Vector<uint8_t> visited;
	visited.resize_initialized(metadata.nodes.size());
	Vector<uint32_t> queue = metadata.roots;
	for (int i = 0; i < queue.size(); i++) {
		uint32_t node = queue[i];
		ERR_FAIL_COND_V(node >= uint32_t(visited.size()) || visited[node], ERR_INVALID_DATA);
		visited.write[node] = 1;
		for (uint32_t j = 0; j < metadata.nodes[node].child_count; j++) {
			queue.push_back(metadata.nodes[node].first_child + j);
		}
	}
	for (uint8_t seen : visited) {
		ERR_FAIL_COND_V(!seen, ERR_INVALID_DATA);
	}
	return OK;
}

Error MicroGeometryData::append_page(Build &r_build, const Vector<uint8_t> &p_data) {
	ERR_FAIL_COND_V(p_data.is_empty() || p_data.size() > MAX_PAGE_SIZE, ERR_INVALID_PARAMETER);
	Vector<uint8_t> encoded;
	ERR_FAIL_COND_V(encoded.resize(Compression::get_max_compressed_buffer_size(p_data.size(), Compression::MODE_ZSTD)) != OK, ERR_OUT_OF_MEMORY);
	int64_t size = Compression::compress(encoded.ptrw(), p_data.ptr(), p_data.size(), Compression::MODE_ZSTD);
	ERR_FAIL_COND_V(size <= 0, ERR_CANT_CREATE);
	encoded.resize(size);
	Page page;
	if (!r_build.pages.is_empty()) {
		const Page &previous = r_build.pages[r_build.pages.size() - 1];
		page.offset = previous.offset + previous.encoded_size;
	}
	page.first_cluster = r_build.pages.is_empty() ? 0 : r_build.pages[r_build.pages.size() - 1].first_cluster + r_build.pages[r_build.pages.size() - 1].cluster_count;
	page.cluster_count = r_build.clusters.size() - page.first_cluster;
	page.encoded_size = size;
	page.decoded_size = p_data.size();
	ERR_FAIL_COND_V(CryptoCore::sha256(encoded.ptr(), encoded.size(), page.digest) != OK, FAILED);
	r_build.pages.push_back(page);
	r_build.encoded_pages.push_back(encoded);
	return OK;
}

Error MicroGeometryData::create(const Build &p_build, Ref<MicroGeometryData> &r_data) {
	Ref<MicroGeometryData> result;
	result.instantiate();
	result->metadata = p_build;
	ERR_FAIL_COND_V(p_build.pages.size() != p_build.encoded_pages.size(), ERR_INVALID_DATA);
	Error err = result->validate();
	ERR_FAIL_COND_V(err != OK, err);
	Vector<uint8_t> manifest = result->encode_manifest();
	ERR_FAIL_COND_V(CryptoCore::sha256(manifest.ptr(), manifest.size(), result->content_digest) != OK, FAILED);
	r_data = result;
	return OK;
}

String MicroGeometryData::get_content_id() const {
	return String::hex_encode_buffer(content_digest, 32);
}

Error MicroGeometryData::read_encoded_page(uint32_t p_page, Vector<uint8_t> &r_data) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_page, uint32_t(metadata.pages.size()), ERR_INVALID_PARAMETER);
	const Page &page = metadata.pages[p_page];
	Vector<uint8_t> data;
	if (!metadata.encoded_pages.is_empty()) {
		data = metadata.encoded_pages[p_page];
	} else {
		MutexLock lock(source_mutex);
		ERR_FAIL_COND_V(source.is_null(), ERR_FILE_CANT_OPEN);
		ERR_FAIL_COND_V(data.resize(page.encoded_size) != OK, ERR_OUT_OF_MEMORY);
		source->seek(12);
		uint32_t manifest_size = source->get_32();
		source->seek(HEADER_SIZE + uint64_t(manifest_size) + page.offset);
		ERR_FAIL_COND_V(source->get_buffer(data.ptrw(), data.size()) != uint64_t(data.size()), ERR_FILE_CORRUPT);
	}
	uint8_t digest[32];
	ERR_FAIL_COND_V(data.size() != page.encoded_size || CryptoCore::sha256(data.ptr(), data.size(), digest) != OK || memcmp(digest, page.digest, 32), ERR_FILE_CORRUPT);
	r_data = data;
	return OK;
}

Error MicroGeometryData::read_page(uint32_t p_page, Vector<uint8_t> &r_data) const {
	Vector<uint8_t> encoded;
	Error err = read_encoded_page(p_page, encoded);
	ERR_FAIL_COND_V(err != OK, err);
	Vector<uint8_t> decoded;
	ERR_FAIL_COND_V(decoded.resize(metadata.pages[p_page].decoded_size) != OK, ERR_OUT_OF_MEMORY);
	ERR_FAIL_COND_V(Compression::decompress(decoded.ptrw(), decoded.size(), encoded.ptr(), encoded.size(), Compression::MODE_ZSTD) != decoded.size(), ERR_FILE_CORRUPT);
	const Page &page = metadata.pages[p_page];
	for (uint32_t i = 0; i < page.cluster_count; i++) {
		const Cluster &cluster = metadata.clusters[page.first_cluster + i];
		const Surface &surface = metadata.surfaces[cluster.surface];
		uint32_t header_size = cluster_header_size(surface);
		ERR_FAIL_COND_V(uint64_t(cluster.disk_offset) + header_size > uint64_t(decoded.size()), ERR_FILE_CORRUPT);
		const uint8_t *header = decoded.ptr() + cluster.disk_offset;
		uint8_t position_bits[3] = { header[12], header[13], header[14] };
		uint32_t vertex_id_bits = header[15];
		ERR_FAIL_COND_V(header[16] != cluster.vertex_count || header[17] != cluster.triangle_count, ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V(position_bits[0] > 32 || position_bits[1] > 32 || position_bits[2] > 32 || vertex_id_bits > 32, ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V(decode_uint32(header + 20) != cluster.first_primitive, ERR_FILE_CORRUPT);
		uint32_t topology_bytes = decode_uint16(header + 18);
		uint8_t uv_bits[4] = {};
		uint32_t uv_offset = CLUSTER_HEADER_SIZE;
		for (uint32_t channel = 0; channel < 2; channel++) {
			if (surface.uv_mode[channel] == UV_MODE_NONE) {
				continue;
			}
			for (uint32_t component = 0; component < 2; component++) {
				const uint8_t *range = header + uv_offset;
				uint32_t low_min = decode_uint16(range);
				uint32_t low_max = decode_uint16(range + 2);
				uint32_t high_min = decode_uint16(range + 4);
				uint32_t high_max = decode_uint16(range + 6);
				uint32_t bits = range[8] & ~UV_SPLIT_FLAG;
				uint64_t span = uint64_t(low_max - low_min) + 1;
				if (range[8] & UV_SPLIT_FLAG) {
					ERR_FAIL_COND_V(low_max >= high_min, ERR_FILE_CORRUPT);
					span += uint64_t(high_max - high_min) + 1;
				} else {
					ERR_FAIL_COND_V(low_min != high_min || low_max != high_max, ERR_FILE_CORRUPT);
				}
				ERR_FAIL_COND_V(low_min > low_max || low_max > high_min || high_min > high_max || bits > 17 || span > (uint64_t(1) << bits), ERR_FILE_CORRUPT);
				uv_bits[channel * 2 + component] = bits;
				uv_offset += CLUSTER_UV_HEADER_SIZE;
			}
		}
		uint64_t size = cluster_disk_size(surface, cluster.vertex_count, position_bits, uv_bits, vertex_id_bits, topology_bytes);
		ERR_FAIL_COND_V(uint64_t(cluster.disk_offset) + size > uint64_t(decoded.size()), ERR_FILE_CORRUPT);
	}
	r_data = decoded;
	return OK;
}

Error MicroGeometryData::save(const String &p_path) const {
	if (FileAccess::exists(p_path)) {
		Ref<MicroGeometryData> existing;
		if (load(p_path, existing) == OK && existing->get_content_id() == get_content_id()) {
			bool valid = true;
			for (int i = 0; i < metadata.pages.size(); i++) {
				Vector<uint8_t> encoded;
				if (existing->read_encoded_page(i, encoded) != OK) {
					valid = false;
					break;
				}
			}
			if (valid) {
				return OK;
			}
		}
	}
	Vector<uint8_t> manifest = encode_manifest();
	String temporary = p_path + "." + itos(get_instance_id()) + ".tmp";
	Error err;
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V(file.is_null(), err);
	uint8_t header[HEADER_SIZE] = {};
	encode_uint32(MAGIC, header);
	encode_uint32(FORMAT_VERSION, header + 4);
	encode_uint32(BUILD_VERSION, header + 8);
	encode_uint32(manifest.size(), header + 12);
	memcpy(header + 16, content_digest, 32);
	memcpy(header + 48, BUILDER_COMMIT, 40);
	bool success = file->store_buffer(header, HEADER_SIZE) && file->store_buffer(manifest.ptr(), manifest.size());
	for (int i = 0; success && i < metadata.pages.size(); i++) {
		Vector<uint8_t> encoded;
		err = read_encoded_page(i, encoded);
		success = err == OK && file->store_buffer(encoded.ptr(), encoded.size());
	}
	file->flush();
	success = success && file->get_error() == OK;
	file.unref();
	if (!success) {
		DirAccess::remove_absolute(temporary);
		return err == OK ? ERR_FILE_CANT_WRITE : err;
	}
	err = DirAccess::rename_absolute(temporary, p_path);
	if (err != OK) {
		DirAccess::remove_absolute(temporary);
	}
	return err;
}

Error MicroGeometryData::load(const String &p_path, Ref<MicroGeometryData> &r_data) {
	Error err;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V(file.is_null(), err);
	uint8_t header[HEADER_SIZE];
	ERR_FAIL_COND_V(file->get_buffer(header, HEADER_SIZE) != HEADER_SIZE || decode_uint32(header) != MAGIC || decode_uint32(header + 4) != FORMAT_VERSION || decode_uint32(header + 8) != BUILD_VERSION || memcmp(header + 48, BUILDER_COMMIT, 40), ERR_FILE_UNRECOGNIZED);
	uint32_t manifest_size = decode_uint32(header + 12);
	ERR_FAIL_COND_V(manifest_size < MANIFEST_PROLOGUE_SIZE || manifest_size > INT32_MAX || HEADER_SIZE + uint64_t(manifest_size) > file->get_length(), ERR_FILE_CORRUPT);
	Vector<uint8_t> manifest;
	ERR_FAIL_COND_V(manifest.resize(manifest_size) != OK, ERR_OUT_OF_MEMORY);
	ERR_FAIL_COND_V(file->get_buffer(manifest.ptrw(), manifest_size) != manifest_size, ERR_FILE_CORRUPT);
	uint8_t digest[32];
	ERR_FAIL_COND_V(CryptoCore::sha256(manifest.ptr(), manifest.size(), digest) != OK || memcmp(header + 16, digest, 32), ERR_FILE_CORRUPT);
	Ref<MicroGeometryData> result;
	result.instantiate();
	err = result->decode_manifest(manifest);
	ERR_FAIL_COND_V(err != OK, err);
	const Page &last = result->metadata.pages[result->metadata.pages.size() - 1];
	ERR_FAIL_COND_V(HEADER_SIZE + uint64_t(manifest_size) + last.offset + last.encoded_size != file->get_length(), ERR_FILE_CORRUPT);
	result->source = file;
	result->source_path = p_path;
	memcpy(result->content_digest, digest, 32);
	r_data = result;
	return OK;
}
