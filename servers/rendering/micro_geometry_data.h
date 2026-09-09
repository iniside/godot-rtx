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
	static constexpr uint32_t FORMAT_VERSION = 2;
	static constexpr uint32_t BUILD_VERSION = 2;
	static constexpr uint32_t INVALID_ID = UINT32_MAX;
	static constexpr uint32_t MAX_PAGE_SIZE = 65536;
	static constexpr const char *BUILDER_COMMIT = "0870c3881655df9b7d22faa35c825393534416bc";

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
		uint32_t source_vertex_count = 0;
		uint32_t source_triangle_count = 0;
	};
	struct Cluster {
		uint32_t surface = 0;
		uint32_t group = 0;
		uint32_t refined_group = INVALID_ID;
		uint32_t page = 0;
		uint32_t payload_offset = 0;
		uint32_t vertex_count = 0;
		uint32_t triangle_count = 0;
		Bounds bounds;
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
	const Build &get_metadata() const { return metadata; }
	String get_source_path() const { return source_path; }
	String get_content_id() const;
	Error read_encoded_page(uint32_t p_page, Vector<uint8_t> &r_data) const;
	Error read_page(uint32_t p_page, Vector<uint8_t> &r_data) const;
	Error save(const String &p_path) const;
	static Error create(const Build &p_build, Ref<MicroGeometryData> &r_data);
	static Error load(const String &p_path, Ref<MicroGeometryData> &r_data);
	static Error append_page(Build &r_build, const Vector<uint8_t> &p_data);
};
