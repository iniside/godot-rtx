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

#include <cfloat>

namespace {
constexpr uint32_t MAGIC = 0x4144474d;
constexpr uint32_t HEADER_SIZE = 96;

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
	for (const Surface &s : metadata.surfaces) {
		w.u32(s.source_surface);
		w.u64(s.format);
		w.u32(s.vertex_stride);
		for (uint32_t offset : s.attribute_offsets) {
			w.u32(offset);
		}
		w.u32(s.source_vertex_count);
		w.u32(s.source_triangle_count);
	}
	for (const Cluster &c : metadata.clusters) {
		w.u32(c.surface);
		w.u32(c.group);
		w.u32(c.refined_group);
		w.u32(c.page);
		w.u32(c.payload_offset);
		w.u32(c.vertex_count);
		w.u32(c.triangle_count);
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
	ERR_FAIL_COND_V(p_manifest.size() < 36, ERR_FILE_CORRUPT);
	ManifestReader r{ p_manifest.ptr() };
	uint32_t sizes[8];
	for (uint32_t &size : sizes) {
		size = r.u32();
	}
	metadata.coarse_cluster_count = r.u32();
	uint64_t expected = 36 + uint64_t(sizes[0]) * 64 + uint64_t(sizes[1]) * 48 + uint64_t(sizes[2]) * 40 + uint64_t(sizes[3]) * 4 + uint64_t(sizes[4]) * 56 + uint64_t(sizes[5]) * 32 + uint64_t(sizes[6]) * 4 + uint64_t(sizes[7]) * 4;
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
		s.source_vertex_count = r.u32();
		s.source_triangle_count = r.u32();
	}
	for (Cluster &c : metadata.clusters) {
		c.surface = r.u32();
		c.group = r.u32();
		c.refined_group = r.u32();
		c.page = r.u32();
		c.payload_offset = r.u32();
		c.vertex_count = r.u32();
		c.triangle_count = r.u32();
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
	for (const Surface &s : metadata.surfaces) {
		ERR_FAIL_COND_V(s.vertex_stride < 12 || s.vertex_stride > 256 || s.source_vertex_count == 0 || s.source_triangle_count == 0, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(s.attribute_offsets[0] != 0, ERR_INVALID_DATA);
		for (uint32_t offset : s.attribute_offsets) {
			ERR_FAIL_COND_V(offset != INVALID_ID && offset >= s.vertex_stride, ERR_INVALID_DATA);
		}
	}
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
			ERR_FAIL_COND_V(!c.vertex_count || c.vertex_count > 128 || !c.triangle_count || c.triangle_count > 128, ERR_INVALID_DATA);
			uint64_t end = uint64_t(c.payload_offset) + uint64_t(c.vertex_count) * (metadata.surfaces[c.surface].vertex_stride + 4) + uint64_t(c.triangle_count) * 7;
			ERR_FAIL_COND_V(end > metadata.pages[c.page].decoded_size, ERR_INVALID_DATA);
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
		const uint8_t *indices = decoded.ptr() + cluster.payload_offset + uint64_t(cluster.vertex_count) * surface.vertex_stride;
		for (uint32_t j = 0; j < cluster.triangle_count * 3; j++) {
			ERR_FAIL_COND_V(indices[j] >= cluster.vertex_count, ERR_FILE_CORRUPT);
		}
		const uint8_t *identities = indices + cluster.triangle_count * 3;
		for (uint32_t j = 0; j < cluster.triangle_count; j++) {
			uint32_t primitive = decode_uint32(identities + j * 4);
			ERR_FAIL_COND_V(cluster.refined_group == INVALID_ID ? primitive >= surface.source_triangle_count : primitive != INVALID_ID, ERR_FILE_CORRUPT);
		}
		for (uint32_t j = 0; j < cluster.vertex_count; j++) {
			ERR_FAIL_COND_V(decode_uint32(identities + cluster.triangle_count * 4 + j * 4) >= surface.source_vertex_count, ERR_FILE_CORRUPT);
		}
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
	ERR_FAIL_COND_V(manifest_size < 36 || manifest_size > INT32_MAX || HEADER_SIZE + uint64_t(manifest_size) > file->get_length(), ERR_FILE_CORRUPT);
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
