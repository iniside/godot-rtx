/**************************************************************************/
/*  micro_geometry_data.h                                                 */
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

#pragma once

#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/vector.h"

class MicroGeometryData : public RefCounted {
public:
	static constexpr uint32_t FORMAT_VERSION = 3;
	static constexpr uint32_t BUILD_VERSION = 3;
	static constexpr uint32_t INVALID_ID = UINT32_MAX;
	static constexpr uint32_t MAX_PAGE_SIZE = 65536;
	static constexpr uint32_t MAX_CLUSTER_VERTICES = 128;
	static constexpr uint32_t MAX_CLUSTER_TRIANGLES = 128;
	// Disk cluster header: grid minimum 3xi32, position bits 3xu8, vertex id bits, vertex/triangle counts, topology bytes u16, first primitive u32, then CLUSTER_UV_HEADER_SIZE per present UV component.
	static constexpr uint32_t CLUSTER_HEADER_SIZE = 24;
	static constexpr uint32_t CLUSTER_UV_HEADER_SIZE = 10;
	static constexpr uint32_t VERTEX_REFERENCE_ESCAPE = 31;
	static constexpr uint32_t UV_SPLIT_FLAG = 0x80;
	static constexpr const char *BUILDER_COMMIT = "3608e8b74fe387325ae48c821ba4091a42c26d5c";

	enum AttributeFormat : uint32_t {
		ATTRIBUTE_NONE = 0,
		ATTRIBUTE_POSITION_SNORM16 = 1,
		ATTRIBUTE_NORMAL_OCTAHEDRAL8 = 2,
		ATTRIBUTE_COLOR_UNORM8 = 3,
		ATTRIBUTE_UV_UNORM16 = 4,
		ATTRIBUTE_UV_HALF = 5,
		ATTRIBUTE_CUSTOM = 6,
	};
	enum UVMode : uint32_t {
		UV_MODE_NONE = 0,
		UV_MODE_UNORM16 = 1,
		UV_MODE_HALF = 2,
	};

	struct Bounds {
		float center[3] = {};
		float radius = 0;
		float error = 0;
	};
	struct Surface {
		uint32_t source_surface = 0;
		uint64_t format = 0;
		uint32_t vertex_stride = 0;
		uint32_t attribute_offsets[10] = {};
		uint32_t attribute_formats[10] = {};
		uint32_t source_vertex_count = 0;
		uint32_t source_triangle_count = 0;
		float frame_center[3] = {};
		float frame_scale = 0;
		uint32_t uv_mode[2] = {};
		float uv_min[4] = {};
		float uv_scale[4] = {};
	};
	struct Cluster {
		uint32_t surface = 0;
		uint32_t group = 0;
		uint32_t refined_group = INVALID_ID;
		uint32_t page = 0;
		uint32_t payload_offset = 0;
		uint32_t disk_offset = 0;
		uint32_t vertex_count = 0;
		uint32_t triangle_count = 0;
		uint32_t first_primitive = INVALID_ID;
		Bounds bounds;
	};
	struct ClusterLayout {
		uint32_t vertices = 0;
		uint32_t indices = 0;
		uint32_t vertex_ids = 0;
		uint32_t end = 0;
	};
	struct Permutation {
		uint32_t surface = 0;
		Vector<uint32_t> triangles;
		Vector<uint32_t> vertices;
	};
	struct Group {
		uint32_t first_cluster = 0;
		uint32_t cluster_count = 0;
		uint32_t depth = 0;
		uint32_t first_parent = 0;
		uint32_t parent_count = 0;
		Bounds bounds;
	};
	struct Node {
		uint32_t group = INVALID_ID;
		uint32_t first_child = 0;
		uint32_t child_count = 0;
		Bounds bounds;
	};
	struct Page {
		uint64_t offset = 0;
		uint32_t encoded_size = 0;
		uint32_t decoded_size = 0;
		uint32_t first_cluster = 0;
		uint32_t cluster_count = 0;
		uint8_t digest[32] = {};
	};
	struct Build {
		uint32_t coarse_cluster_count = 0;
		float frame_center[3] = {};
		float frame_scale = 1;
		float position_step = 0;
		Vector<Surface> surfaces;
		Vector<Cluster> clusters;
		Vector<Group> groups;
		Vector<uint32_t> parent_groups;
		Vector<uint32_t> terminals;
		Vector<Node> nodes;
		Vector<uint32_t> roots;
		Vector<Page> pages;
		Vector<Vector<uint8_t>> encoded_pages;
	};

private:
	Build metadata;
	String source_path;
	Ref<FileAccess> source;
	mutable Mutex source_mutex;
	uint8_t content_digest[32] = {};
	Vector<uint8_t> encode_manifest() const;
	Error decode_manifest(const Vector<uint8_t> &p_manifest);
	Error validate() const;

public:
	static uint32_t custom_attribute_size(uint64_t p_format, uint32_t p_index);
	static ClusterLayout compute_cluster_layout(uint32_t p_payload_offset, uint32_t p_vertex_count, uint32_t p_triangle_count, uint32_t p_vertex_stride);
	static uint32_t cluster_header_size(const Surface &p_surface);
	static uint32_t cluster_disk_size(const Surface &p_surface, uint32_t p_vertex_count, const uint8_t p_position_bits[3], const uint8_t p_uv_bits[4], uint32_t p_vertex_id_bits, uint32_t p_topology_bytes);

	const Build &get_metadata() const { return metadata; }
	ClusterLayout get_cluster_layout(uint32_t p_cluster) const;
	String get_source_path() const { return source_path; }
	String get_content_id() const;
	Error read_encoded_page(uint32_t p_page, Vector<uint8_t> &r_data) const;
	Error read_page(uint32_t p_page, Vector<uint8_t> &r_data) const;
	Error save(const String &p_path) const;
	static Error create(const Build &p_build, Ref<MicroGeometryData> &r_data);
	static Error load(const String &p_path, Ref<MicroGeometryData> &r_data);
	static Error append_page(Build &r_build, const Vector<uint8_t> &p_data);
};
