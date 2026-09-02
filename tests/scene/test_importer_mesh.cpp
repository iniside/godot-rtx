/**************************************************************************/
/*  test_importer_mesh.cpp                                               */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_importer_mesh)

#ifndef _3D_DISABLED

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"

#include <cstring>

namespace TestImporterMesh {

// Mirrors the record/limit constants in ImporterMesh::generate_clusters()'s anonymous namespace
// (scene/resources/3d/importer_mesh.cpp); duplicated here since they are not exposed to the header.
constexpr uint32_t CLUSTER_BLOB_MAGIC = 0x53554c43;
constexpr uint32_t CLUSTER_BLOB_VERSION = 1;
constexpr uint32_t CLUSTER_HEADER_SIZE = 32;
constexpr uint32_t CLUSTER_RECORD_SIZE = 16;
constexpr uint32_t CLUSTER_MAX_VERTICES = 128;
constexpr uint32_t CLUSTER_MAX_TRIANGLES = 128;

constexpr int GRID_SIZE = 16; // 16x16 quads = 512 triangles: enough that max_triangles=128 forces multiple clusters.
constexpr int GRID_VERTS_PER_ROW = GRID_SIZE + 1;

// A grid in the XY plane with integer-valued vertex positions (exactly representable in float32),
// so a cluster's baked position can be inverted back to the original vertex index by rounding.
Array make_grid_arrays() {
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);

	PackedVector3Array vertices;
	vertices.resize(GRID_VERTS_PER_ROW * GRID_VERTS_PER_ROW);
	for (int y = 0; y < GRID_VERTS_PER_ROW; y++) {
		for (int x = 0; x < GRID_VERTS_PER_ROW; x++) {
			vertices.set(y * GRID_VERTS_PER_ROW + x, Vector3((float)x, (float)y, 0.0f));
		}
	}

	PackedInt32Array indices;
	indices.resize(GRID_SIZE * GRID_SIZE * 6);
	int idx = 0;
	for (int y = 0; y < GRID_SIZE; y++) {
		for (int x = 0; x < GRID_SIZE; x++) {
			int v00 = y * GRID_VERTS_PER_ROW + x;
			int v10 = v00 + 1;
			int v01 = v00 + GRID_VERTS_PER_ROW;
			int v11 = v01 + 1;
			indices.set(idx++, v00);
			indices.set(idx++, v10);
			indices.set(idx++, v11);
			indices.set(idx++, v00);
			indices.set(idx++, v11);
			indices.set(idx++, v01);
		}
	}

	arr[RSE::ARRAY_VERTEX] = vertices;
	arr[RSE::ARRAY_INDEX] = indices;
	return arr;
}

struct ClusterHeader {
	uint32_t cluster_count;
	uint32_t total_triangles;
	uint32_t index_section_offset;
	uint32_t position_section_offset;
	uint32_t position_vertex_total;
};

ClusterHeader decode_header(const Vector<uint8_t> &p_blob) {
	const uint8_t *p = p_blob.ptr();
	ClusterHeader h;
	CHECK(decode_uint32(p + 0) == CLUSTER_BLOB_MAGIC);
	CHECK(decode_uint32(p + 4) == CLUSTER_BLOB_VERSION);
	h.cluster_count = decode_uint32(p + 8);
	h.total_triangles = decode_uint32(p + 12);
	h.index_section_offset = decode_uint32(p + 16);
	h.position_section_offset = decode_uint32(p + 20);
	h.position_vertex_total = decode_uint32(p + 24);
	return h;
}

struct ClusterRecord {
	uint32_t position_offset;
	uint32_t index_offset;
	uint8_t vertex_count;
	uint8_t triangle_count;
	uint32_t base_triangle;
};

ClusterRecord decode_record(const Vector<uint8_t> &p_blob, uint32_t p_record_index) {
	const uint8_t *r = p_blob.ptr() + CLUSTER_HEADER_SIZE + p_record_index * CLUSTER_RECORD_SIZE;
	ClusterRecord rec;
	rec.position_offset = decode_uint32(r + 0);
	rec.index_offset = decode_uint32(r + 4);
	rec.vertex_count = r[8];
	rec.triangle_count = r[9];
	rec.base_triangle = decode_uint32(r + 12);
	return rec;
}

Vector<uint8_t> get_surface_cluster_blob(const Ref<ImporterMesh> &p_mesh, int p_surface) {
	Dictionary data = p_mesh->get("_data");
	Array surface_arr = data["surfaces"];
	Dictionary s = surface_arr[p_surface];
	if (!s.has("clusters")) {
		return Vector<uint8_t>();
	}
	return s["clusters"];
}

TEST_CASE("[ImporterMesh] generate_clusters bakes a valid, spatially-correct cluster blob") {
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, make_grid_arrays());
	mesh->generate_clusters();

	Vector<uint8_t> blob = get_surface_cluster_blob(mesh, 0);
	ClusterHeader header = decode_header(blob);

	// Assertion 0: guards the whole test file against a null meshoptimizer function pointer table
	// (generate_clusters() early-returns and every following assertion would pass vacuously on an
	// empty blob) — see initialize_meshoptimizer_module() in modules/meshoptimizer/register_types.cpp.
	REQUIRE(header.cluster_count > 0);

	PackedInt32Array rewritten_indices = mesh->get_surface_arrays(0)[RSE::ARRAY_INDEX];
	const uint32_t original_triangle_count = GRID_SIZE * GRID_SIZE * 2;

	SUBCASE("Sum of per-cluster triangle counts equals the surface's triangle count") {
		uint32_t triangle_sum = 0;
		for (uint32_t j = 0; j < header.cluster_count; j++) {
			triangle_sum += decode_record(blob, j).triangle_count;
		}
		CHECK(header.total_triangles == original_triangle_count);
		CHECK(triangle_sum == original_triangle_count);
		CHECK((uint32_t)rewritten_indices.size() == original_triangle_count * 3);
	}

	SUBCASE("Every cluster's local triangle matches the rewritten index buffer at base_triangle") {
		const uint8_t *blob_ptr = blob.ptr();
		bool any_triangle_checked = false;
		for (uint32_t j = 0; j < header.cluster_count; j++) {
			ClusterRecord rec = decode_record(blob, j);
			const uint8_t *tri_src = blob_ptr + header.index_section_offset + rec.index_offset;
			const uint8_t *pos_src = blob_ptr + header.position_section_offset + rec.position_offset;

			for (uint32_t t = 0; t < rec.triangle_count; t++) {
				for (uint32_t k = 0; k < 3; k++) {
					uint8_t local_vertex = tri_src[t * 3 + k];
					REQUIRE(local_vertex < rec.vertex_count);

					const uint8_t *p = pos_src + (size_t)local_vertex * 12;
					float px = decode_float(p + 0);
					float py = decode_float(p + 4);
					// This is the load-bearing check: it is a correspondence with the buffer's actual
					// content, not a coverage check over base_triangle's numeric range. A prefix-sum
					// base_triangle with no reordering of the index buffer (the a643a15e48 bug) passes
					// a "no gaps/overlaps" check but fails this one, because meshopt_buildMeshletsSpatial
					// consumes faces in BVH-sorted order, not original index-buffer order.
					int32_t reconstructed_vertex = Math::round(py) * GRID_VERTS_PER_ROW + Math::round(px);
					int32_t rewritten_vertex = rewritten_indices[(rec.base_triangle + t) * 3 + k];
					CHECK(reconstructed_vertex == rewritten_vertex);
					any_triangle_checked = true;
				}
			}
		}
		CHECK(any_triangle_checked);
	}

	SUBCASE("Per-cluster vertex/triangle counts stay within the 8-bit blob record's limits") {
		for (uint32_t j = 0; j < header.cluster_count; j++) {
			ClusterRecord rec = decode_record(blob, j);
			CHECK(rec.vertex_count > 0);
			CHECK(rec.vertex_count <= CLUSTER_MAX_VERTICES);
			CHECK(rec.triangle_count > 0);
			CHECK(rec.triangle_count <= CLUSTER_MAX_TRIANGLES);
		}
	}

	SUBCASE("Position section size and every offset/local-index stay within their own section") {
		const uint32_t position_section_size = header.position_vertex_total * 12;
		CHECK((uint32_t)blob.size() == header.position_section_offset + position_section_size);

		const uint32_t index_section_size = header.total_triangles * 3;
		for (uint32_t j = 0; j < header.cluster_count; j++) {
			ClusterRecord rec = decode_record(blob, j);
			CHECK(rec.position_offset + (uint32_t)rec.vertex_count * 12 <= position_section_size);
			CHECK(rec.index_offset + (uint32_t)rec.triangle_count * 3 <= index_section_size);
		}
	}
}

TEST_CASE("[ImporterMesh] generate_clusters _get_data/_set_data round-trips the cluster blob byte for byte") {
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, make_grid_arrays());
	mesh->generate_clusters();

	Vector<uint8_t> original_blob = get_surface_cluster_blob(mesh, 0);
	REQUIRE(decode_header(original_blob).cluster_count > 0);

	Dictionary data = mesh->get("_data");
	Ref<ImporterMesh> reloaded;
	reloaded.instantiate();
	reloaded->set("_data", data);

	Vector<uint8_t> reloaded_blob = get_surface_cluster_blob(reloaded, 0);
	REQUIRE(reloaded_blob.size() == original_blob.size());
	CHECK(memcmp(original_blob.ptr(), reloaded_blob.ptr(), original_blob.size()) == 0);
}

TEST_CASE("[ImporterMesh] generate_clusters leaves non-triangle surfaces without cluster data") {
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_LINES, make_grid_arrays());
	mesh->generate_clusters();

	Vector<uint8_t> blob = get_surface_cluster_blob(mesh, 0);
	CHECK(blob.is_empty());
}

TEST_CASE("[ImporterMesh] generate_clusters synthesizes an index buffer for non-indexed triangle surfaces") {
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices;
	// Three unindexed triangles: every corner is its own vertex, no shared ARRAY_INDEX.
	for (int t = 0; t < 3; t++) {
		vertices.push_back(Vector3((float)(t * 3 + 0), 0.0f, 0.0f));
		vertices.push_back(Vector3((float)(t * 3 + 1), 0.0f, 0.0f));
		vertices.push_back(Vector3((float)(t * 3 + 2), 0.0f, 0.0f));
	}
	arr[RSE::ARRAY_VERTEX] = vertices;

	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, arr);
	REQUIRE(((PackedInt32Array)mesh->get_surface_arrays(0)[RSE::ARRAY_INDEX]).is_empty());

	mesh->generate_clusters();

	Vector<uint8_t> blob = get_surface_cluster_blob(mesh, 0);
	ClusterHeader header = decode_header(blob);
	REQUIRE(header.cluster_count > 0);
	CHECK(header.total_triangles == 3);

	PackedInt32Array synthesized_indices = mesh->get_surface_arrays(0)[RSE::ARRAY_INDEX];
	CHECK(synthesized_indices.size() == vertices.size());
}

} // namespace TestImporterMesh

#endif // _3D_DISABLED
