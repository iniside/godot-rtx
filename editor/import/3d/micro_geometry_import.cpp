/**************************************************************************/
/*  micro_geometry_import.cpp                                             */
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

#include "micro_geometry_import.h"

#include "core/io/resource_saver.h"
#include "core/templates/hash_set.h"
#include "scene/resources/3d/importer_mesh.h"

namespace {
void collect_meshes(const Variant &p_value, HashSet<ObjectID> &r_visited, Vector<Ref<ArrayMesh>> &r_meshes);

void collect_object(Object *p_object, HashSet<ObjectID> &r_visited, Vector<Ref<ArrayMesh>> &r_meshes) {
	if (!p_object || r_visited.has(p_object->get_instance_id())) {
		return;
	}
	r_visited.insert(p_object->get_instance_id());
	ArrayMesh *mesh = Object::cast_to<ArrayMesh>(p_object);
	if (mesh) {
		r_meshes.push_back(Ref<ArrayMesh>(mesh));
		return;
	}
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.usage & PROPERTY_USAGE_STORAGE) {
			collect_meshes(p_object->get(property.name), r_visited, r_meshes);
		}
	}
	Node *node = Object::cast_to<Node>(p_object);
	if (node) {
		for (int i = 0; i < node->get_child_count(); i++) {
			collect_object(node->get_child(i), r_visited, r_meshes);
		}
	}
}

void collect_meshes(const Variant &p_value, HashSet<ObjectID> &r_visited, Vector<Ref<ArrayMesh>> &r_meshes) {
	if (p_value.get_type() == Variant::OBJECT) {
		Ref<Resource> resource = p_value;
		if (resource.is_valid()) {
			collect_object(resource.ptr(), r_visited, r_meshes);
		}
	} else if (p_value.get_type() == Variant::ARRAY) {
		Array array = p_value;
		for (int i = 0; i < array.size(); i++) {
			collect_meshes(array[i], r_visited, r_meshes);
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dictionary = p_value;
		for (const Variant &key : dictionary.get_key_list()) {
			collect_meshes(key, r_visited, r_meshes);
			collect_meshes(dictionary[key], r_visited, r_meshes);
		}
	}
}

bool remap_meshes(Variant &r_value, HashSet<ObjectID> &r_visited, const HashMap<Ref<ArrayMesh>, Ref<ArrayMesh>> &p_replacements);

void remap_object(Object *p_object, HashSet<ObjectID> &r_visited, const HashMap<Ref<ArrayMesh>, Ref<ArrayMesh>> &p_replacements) {
	if (!p_object || r_visited.has(p_object->get_instance_id())) {
		return;
	}
	r_visited.insert(p_object->get_instance_id());
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.usage & PROPERTY_USAGE_STORAGE) {
			Variant value = p_object->get(property.name);
			if (remap_meshes(value, r_visited, p_replacements)) {
				p_object->set(property.name, value);
			}
		}
	}
	Node *node = Object::cast_to<Node>(p_object);
	if (node) {
		for (int i = 0; i < node->get_child_count(); i++) {
			remap_object(node->get_child(i), r_visited, p_replacements);
		}
	}
}

bool remap_meshes(Variant &r_value, HashSet<ObjectID> &r_visited, const HashMap<Ref<ArrayMesh>, Ref<ArrayMesh>> &p_replacements) {
	bool changed = false;
	if (r_value.get_type() == Variant::OBJECT) {
		Ref<ArrayMesh> mesh = r_value;
		if (mesh.is_valid()) {
			const Ref<ArrayMesh> *replacement = p_replacements.getptr(mesh);
			if (replacement) {
				r_value = *replacement;
				return true;
			}
		} else {
			Ref<Resource> resource = r_value;
			if (resource.is_valid()) {
				remap_object(resource.ptr(), r_visited, p_replacements);
			}
		}
	} else if (r_value.get_type() == Variant::ARRAY) {
		Array array = r_value;
		for (int i = 0; i < array.size(); i++) {
			Variant value = array[i];
			if (remap_meshes(value, r_visited, p_replacements)) {
				array[i] = value;
				changed = true;
			}
		}
	} else if (r_value.get_type() == Variant::DICTIONARY) {
		Dictionary dictionary = r_value;
		for (const Variant &key : dictionary.get_key_list()) {
			Variant next_key = key;
			Variant value = dictionary[key];
			bool key_changed = remap_meshes(next_key, r_visited, p_replacements);
			bool value_changed = remap_meshes(value, r_visited, p_replacements);
			if (key_changed) {
				dictionary.erase(key);
			}
			if (key_changed || value_changed) {
				dictionary[next_key] = value;
				changed = true;
			}
		}
	}
	return changed;
}

template <typename T>
Vector<T> permute_array(const Vector<T> &p_array, const Vector<uint32_t> &p_permutation) {
	uint32_t count = p_permutation.size();
	ERR_FAIL_COND_V(!count || p_array.size() % count != 0, p_array);
	uint32_t elements = p_array.size() / count;
	Vector<T> result;
	ERR_FAIL_COND_V(result.resize(p_array.size()) != OK, p_array);
	const T *source = p_array.ptr();
	T *target = result.ptrw();
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t j = 0; j < elements; j++) {
			target[uint64_t(i) * elements + j] = source[uint64_t(p_permutation[i]) * elements + j];
		}
	}
	return result;
}

Error permute_arrays(Array &r_arrays, const Vector<uint32_t> &p_permutation) {
	for (int i = 0; i < r_arrays.size(); i++) {
		if (i == Mesh::ARRAY_INDEX) {
			continue;
		}
		switch (r_arrays[i].get_type()) {
			case Variant::NIL:
				break;
			case Variant::PACKED_VECTOR3_ARRAY:
				r_arrays[i] = permute_array<Vector3>(r_arrays[i], p_permutation);
				break;
			case Variant::PACKED_VECTOR2_ARRAY:
				r_arrays[i] = permute_array<Vector2>(r_arrays[i], p_permutation);
				break;
			case Variant::PACKED_FLOAT32_ARRAY:
				r_arrays[i] = permute_array<float>(r_arrays[i], p_permutation);
				break;
			case Variant::PACKED_INT32_ARRAY:
				r_arrays[i] = permute_array<int32_t>(r_arrays[i], p_permutation);
				break;
			case Variant::PACKED_BYTE_ARRAY:
				r_arrays[i] = permute_array<uint8_t>(r_arrays[i], p_permutation);
				break;
			case Variant::PACKED_COLOR_ARRAY:
				r_arrays[i] = permute_array<Color>(r_arrays[i], p_permutation);
				break;
			default:
				ERR_FAIL_V_MSG(ERR_INVALID_DATA, "Unhandled array type.");
		}
	}
	return OK;
}

Error renumber_surfaces(const Ref<ArrayMesh> &p_mesh, const Ref<ImporterMesh> &p_source, const Vector<MicroGeometryData::Permutation> &p_permutations) {
	ERR_FAIL_COND_V(p_mesh->get_blend_shape_count() != 0, ERR_INVALID_DATA);
	bool shadow = p_mesh->get_shadow_mesh().is_valid();
	int surface_count = p_source->get_surface_count();
	p_mesh->clear_surfaces();
	for (int i = 0; i < surface_count; i++) {
		Array arrays = p_source->get_surface_arrays(i);
		const MicroGeometryData::Permutation *permutation = nullptr;
		for (const MicroGeometryData::Permutation &candidate : p_permutations) {
			if (candidate.surface == uint32_t(i)) {
				permutation = &candidate;
				break;
			}
		}
		Vector<uint32_t> inverse;
		if (permutation) {
			PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
			PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
			uint32_t vertex_count = positions.size();
			uint32_t triangle_count = indices.is_empty() ? vertex_count / 3 : uint32_t(indices.size()) / 3;
			ERR_FAIL_COND_V(uint32_t(permutation->vertices.size()) != vertex_count || uint32_t(permutation->triangles.size()) != triangle_count, ERR_INVALID_DATA);
			ERR_FAIL_COND_V(inverse.resize(vertex_count) != OK, ERR_OUT_OF_MEMORY);
			for (uint32_t j = 0; j < vertex_count; j++) {
				uint32_t source_vertex = permutation->vertices[j];
				ERR_FAIL_COND_V(source_vertex >= vertex_count, ERR_INVALID_DATA);
				inverse.write[source_vertex] = j;
			}
			PackedInt32Array renumbered;
			ERR_FAIL_COND_V(renumbered.resize(uint64_t(triangle_count) * 3) != OK, ERR_OUT_OF_MEMORY);
			for (uint32_t j = 0; j < triangle_count; j++) {
				uint32_t triangle = permutation->triangles[j];
				ERR_FAIL_COND_V(triangle >= triangle_count, ERR_INVALID_DATA);
				for (uint32_t k = 0; k < 3; k++) {
					uint32_t index = indices.is_empty() ? triangle * 3 + k : uint32_t(indices[triangle * 3 + k]);
					ERR_FAIL_COND_V(index >= vertex_count, ERR_INVALID_DATA);
					renumbered.write[uint64_t(j) * 3 + k] = int32_t(inverse[index]);
				}
			}
			Error err = permute_arrays(arrays, permutation->vertices);
			ERR_FAIL_COND_V(err != OK, err);
			arrays[Mesh::ARRAY_INDEX] = renumbered;
		}
		Dictionary lods;
		for (int j = 0; j < p_source->get_surface_lod_count(i); j++) {
			Vector<int> lod_indices = p_source->get_surface_lod_indices(i, j);
			if (permutation) {
				int32_t *values = lod_indices.ptrw();
				for (int k = 0; k < lod_indices.size(); k++) {
					ERR_FAIL_INDEX_V(values[k], inverse.size(), ERR_INVALID_DATA);
					values[k] = int32_t(inverse[values[k]]);
				}
			}
			lods[p_source->get_surface_lod_size(i, j)] = lod_indices;
		}
		p_mesh->add_surface_from_arrays(p_source->get_surface_primitive_type(i), arrays, TypedArray<Array>(), lods, p_source->get_surface_format(i));
		ERR_FAIL_COND_V(p_mesh->get_surface_count() != i + 1, ERR_INVALID_DATA);
		Ref<Material> material = p_source->get_surface_material(i);
		if (material.is_valid()) {
			p_mesh->surface_set_material(i, material);
		}
		String name = p_source->get_surface_name(i);
		if (!name.is_empty()) {
			p_mesh->surface_set_name(i, name);
		}
	}
	if (shadow) {
		Ref<ImporterMesh> renumbered = ImporterMesh::from_mesh(p_mesh);
		renumbered->create_shadow_mesh();
		Ref<ImporterMesh> shadow_mesh = renumbered->get_shadow_mesh();
		p_mesh->set_shadow_mesh(shadow_mesh.is_valid() ? shadow_mesh->get_mesh() : Ref<ArrayMesh>());
	}
	return OK;
}

Error prepare_geometry(const Ref<ArrayMesh> &p_mesh, const String &p_save_path, float p_position_step, List<String> *r_gen_files, Ref<MicroGeometry> &r_geometry) {
	Ref<ImporterMesh> importer = ImporterMesh::from_mesh(p_mesh);
	String context;
	Vector<MicroGeometryData::Permutation> permutations;
	Error err = importer->generate_micro_geometry(p_position_step, permutations, context);
	ERR_FAIL_COND_V_MSG(err != OK, err, context);
	Ref<MicroGeometry> geometry = importer->get_micro_geometry();
	if (geometry.is_null()) {
		return OK;
	}
	err = renumber_surfaces(p_mesh, importer, permutations);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot renumber surfaces of mesh '" + p_mesh->get_name() + "' for microgeometry.");
	String path = p_save_path + "-" + geometry->get_content_id() + ".mgdata";
	err = ResourceSaver::save(geometry, path);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot save derived geometry for mesh '" + p_mesh->get_name() + "' to '" + path + "'.");
	Ref<MicroGeometryData> data;
	err = MicroGeometryData::load(path, data);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot reopen derived geometry '" + path + "'.");
	geometry->set_data(data);
	if (r_gen_files && !r_gen_files->find(path)) {
		r_gen_files->push_back(path);
	}
	r_geometry = geometry;
	return OK;
}

Error publish_geometry(Ref<MicroGeometry> &r_geometry) {
	if (r_geometry.is_null()) {
		return OK;
	}
	String path = r_geometry->get_data()->get_source_path();
	Ref<Resource> cached = ResourceCache::get_ref(path);
	if (cached.is_valid()) {
		Ref<MicroGeometry> existing = cached;
		ERR_FAIL_COND_V(existing.is_null() || existing->get_content_id() != r_geometry->get_content_id(), ERR_ALREADY_IN_USE);
		existing->set_data(r_geometry->get_data());
		r_geometry = existing;
	} else {
		r_geometry->set_path(path);
		ERR_FAIL_COND_V(r_geometry->get_path() != path, ERR_ALREADY_IN_USE);
	}
	return OK;
}
}

Error import_micro_geometry(const Ref<ArrayMesh> &p_mesh, const String &p_save_path, float p_position_step, List<String> *r_gen_files) {
	ERR_FAIL_COND_V(p_mesh.is_null(), ERR_INVALID_PARAMETER);
	Ref<MicroGeometry> geometry;
	Error err = prepare_geometry(p_mesh, p_save_path, p_position_step, r_gen_files, geometry);
	ERR_FAIL_COND_V(err != OK, err);
	err = publish_geometry(geometry);
	ERR_FAIL_COND_V(err != OK, err);
	p_mesh->set_micro_geometry(geometry);
	return OK;
}

Error import_scene_micro_geometry(Node *p_scene, const String &p_save_path, float p_position_step, List<String> *r_gen_files, const HashMap<Ref<ArrayMesh>, String> &p_external_mesh_paths) {
	HashSet<ObjectID> visited;
	Vector<Ref<ArrayMesh>> meshes;
	collect_object(p_scene, visited, meshes);
	Vector<Ref<MicroGeometry>> geometry;
	geometry.resize(meshes.size());
	for (int i = 0; i < meshes.size(); i++) {
		Error err = prepare_geometry(meshes[i], p_save_path, p_position_step, r_gen_files, geometry.write[i]);
		ERR_FAIL_COND_V(err != OK, err);
	}
	for (int i = 0; i < meshes.size(); i++) {
		Error err = publish_geometry(geometry.write[i]);
		ERR_FAIL_COND_V(err != OK, err);
		meshes[i]->set_micro_geometry(geometry[i]);
	}
	HashMap<Ref<ArrayMesh>, Ref<ArrayMesh>> replacements;
	for (const Ref<ArrayMesh> &mesh : meshes) {
		const String *external = p_external_mesh_paths.getptr(mesh);
		String path = external ? ResourceUID::ensure_path(*external) : mesh->get_path();
		if (path.is_empty() || path.contains("::") || FileAccess::exists(path + ".import")) {
			continue;
		}
		Error err = ResourceSaver::save(mesh, path);
		ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot save external mesh with derived geometry to '" + path + "'.");
		if (external && external->begins_with("uid://")) {
			err = ResourceSaver::set_uid(path, ResourceUID::get_singleton()->text_to_id(*external));
			ERR_FAIL_COND_V(err != OK, err);
		}
		Ref<ArrayMesh> existing = ResourceCache::get_ref(path);
		if (existing.is_valid() && existing != mesh) {
			err = existing->copy_from(mesh);
			ERR_FAIL_COND_V(err != OK, err);
			replacements.insert(mesh, existing);
		} else {
			mesh->set_path(path);
			ERR_FAIL_COND_V(mesh->get_path() != path, ERR_ALREADY_IN_USE);
		}
	}
	visited.clear();
	remap_object(p_scene, visited, replacements);
	return OK;
}
