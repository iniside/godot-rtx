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

struct Triangle3 {
	int32_t a = 0, b = 0, c = 0;
	bool operator==(const Triangle3 &p_other) const {
		return a == p_other.a && b == p_other.b && c == p_other.c;
	}
	bool operator<(const Triangle3 &p_other) const {
		if (a != p_other.a) {
			return a < p_other.a;
		}
		if (b != p_other.b) {
			return b < p_other.b;
		}
		return c < p_other.c;
	}
};

// Rotates a triangle's vertex order so its smallest index comes first, without reversing it.
// Two triangles referring to the same face with the same winding canonicalize to the same value;
// a reflected (flipped-winding) copy of the same face does not.
Triangle3 canonical_winding(int32_t p_a, int32_t p_b, int32_t p_c) {
	if (p_a <= p_b && p_a <= p_c) {
		return { p_a, p_b, p_c };
	}
	if (p_b <= p_a && p_b <= p_c) {
		return { p_b, p_c, p_a };
	}
	return { p_c, p_a, p_b };
}

Vector<Triangle3> triangle_multiset(const PackedInt32Array &p_indices) {
	Vector<Triangle3> tris;
	tris.resize(p_indices.size() / 3);
	for (int t = 0; t < tris.size(); t++) {
		tris.set(t, canonical_winding(p_indices[t * 3 + 0], p_indices[t * 3 + 1], p_indices[t * 3 + 2]));
	}
	tris.sort();
	return tris;
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
	// decode_header() reads through blob.ptr() with no size check of its own; on an empty blob that
	// is nullptr, and every decode_uint32 call segfaults instead of failing with a diagnostic. This
	// must run before decode_header(), not after.
	REQUIRE((uint32_t)blob.size() >= CLUSTER_HEADER_SIZE);
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

	SUBCASE("Rewritten index buffer's triangles are exactly the original mesh's triangles, winding included") {
		// The "matches at base_triangle" subcase above decodes both sides from the same vert_src/tri_src
		// pointers ten lines apart in generate_clusters()'s own loop; an error inside those pointers (or
		// a vertex_offset/triangle_offset swap) corrupts both sides identically and stays green. This
		// compares against make_grid_arrays()'s original index buffer instead, which the rewrite must
		// preserve as a set of triangles independent of any bug in generate_clusters()'s internals.
		Array original_arrays = make_grid_arrays();
		PackedInt32Array original_indices = original_arrays[RSE::ARRAY_INDEX];
		Vector<Triangle3> original_tris = triangle_multiset(original_indices);
		Vector<Triangle3> rewritten_tris = triangle_multiset(rewritten_indices);

		REQUIRE(original_tris.size() == rewritten_tris.size());
		bool all_triangles_match = true;
		for (int t = 0; t < original_tris.size(); t++) {
			if (!(original_tris[t] == rewritten_tris[t])) {
				all_triangles_match = false;
				break;
			}
		}
		CHECK(all_triangles_match);
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

	SUBCASE("Section offsets and per-cluster running offsets match structurally, independent of the header's own claims") {
		// The subcase above validates header-declared sizes against header-declared offsets, which is
		// self-referential: a header that is internally consistent but wrong (e.g. built from swapped
		// section sizes) would still pass it. These identities are computed independently of the header
		// fields they check, from cluster_count/total_triangles and each record's own vertex/triangle
		// counts, and are the same identities mesh_storage's loader enforces (88bf193745).
		CHECK(header.index_section_offset == CLUSTER_HEADER_SIZE + header.cluster_count * CLUSTER_RECORD_SIZE);
		CHECK(header.position_section_offset == header.index_section_offset + header.total_triangles * 3);

		uint32_t expected_index_offset = 0;
		uint32_t expected_position_offset = 0;
		for (uint32_t j = 0; j < header.cluster_count; j++) {
			ClusterRecord rec = decode_record(blob, j);
			CHECK(rec.index_offset == expected_index_offset);
			CHECK(rec.position_offset == expected_position_offset);
			expected_index_offset += (uint32_t)rec.triangle_count * 3;
			expected_position_offset += (uint32_t)rec.vertex_count * 12;
		}
	}
}

TEST_CASE("[ImporterMesh] generate_clusters _get_data/_set_data round-trips the cluster blob byte for byte") {
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, make_grid_arrays());
	mesh->generate_clusters();

	Vector<uint8_t> original_blob = get_surface_cluster_blob(mesh, 0);
	REQUIRE((uint32_t)original_blob.size() >= CLUSTER_HEADER_SIZE);
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
	REQUIRE((uint32_t)blob.size() >= CLUSTER_HEADER_SIZE);
	ClusterHeader header = decode_header(blob);
	REQUIRE(header.cluster_count > 0);
	CHECK(header.total_triangles == 3);

	PackedInt32Array synthesized_indices = mesh->get_surface_arrays(0)[RSE::ARRAY_INDEX];
	CHECK(synthesized_indices.size() == vertices.size());
}

TEST_CASE("[ImporterMesh] _set_data does not write to surfaces[-1] when add_surface rejects a surface carrying a \"clusters\" key") {
	Array empty_arrays;
	empty_arrays.resize(Mesh::ARRAY_MAX);
	empty_arrays[RSE::ARRAY_VERTEX] = PackedVector3Array(); // add_surface's ERR_FAIL_COND(vertex_count == 0) rejects this.

	Dictionary surface_dict;
	surface_dict["primitive"] = Mesh::PRIMITIVE_TRIANGLES;
	surface_dict["arrays"] = empty_arrays;
	surface_dict["clusters"] = PackedByteArray();

	Array surface_arr;
	surface_arr.push_back(surface_dict);
	Dictionary data;
	data["surfaces"] = surface_arr;

	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	ERR_PRINT_OFF;
	mesh->set("_data", data);
	ERR_PRINT_ON;

	CHECK(mesh->get_surface_count() == 0);
}

TEST_CASE("[ImporterMesh] _set_data discards a cluster blob that fails magic/version or size validation") {
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, make_grid_arrays());

	Dictionary data = mesh->get("_data");
	Array surface_arr = data["surfaces"];
	Dictionary s = surface_arr[0];

	SUBCASE("Blob smaller than the header size") {
		PackedByteArray too_small;
		too_small.resize(10);
		s["clusters"] = too_small;
	}

	SUBCASE("Blob at the header size but with the wrong magic") {
		PackedByteArray wrong_magic;
		wrong_magic.resize(CLUSTER_HEADER_SIZE);
		wrong_magic.fill(0);
		s["clusters"] = wrong_magic;
	}

	surface_arr[0] = s;
	data["surfaces"] = surface_arr;

	Ref<ImporterMesh> reloaded;
	reloaded.instantiate();
	reloaded->set("_data", data);

	CHECK(get_surface_cluster_blob(reloaded, 0).is_empty());
}

TEST_CASE("[ImporterMesh] optimize_indices clears cluster data instead of leaving it pointing at rewritten geometry") {
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, make_grid_arrays());
	mesh->generate_clusters();
	REQUIRE(!get_surface_cluster_blob(mesh, 0).is_empty());

	mesh->optimize_indices();

	CHECK(get_surface_cluster_blob(mesh, 0).is_empty());
}

// Known unreachable guards, all in ImporterMesh::generate_clusters() (scene/resources/3d/importer_mesh.cpp):
// - The ERR_CONTINUE_MSG for total_triangles*3 != index_count (introduced alongside e887eafb11's
//   validation) only fires if meshopt_buildMeshletsSpatial's own accounting of its output disagrees
//   with its return value, which requires mocking the library, not reachable from real input.
// - The cluster_overflow / 255-truncation ERR_CONTINUE_MSG and the blob.resize()/indices.resize()
//   Error guards are unreachable by construction from the current constants: max_vertices and
//   max_triangles are hardcoded to 128, so neither count can exceed 255. cluster_overflow becomes
//   reachable the moment Krok 3's device-queried limits replace those constants.

} // namespace TestImporterMesh

#endif // _3D_DISABLED
