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

Error prepare_geometry(const Ref<ArrayMesh> &p_mesh, const String &p_save_path, List<String> *r_gen_files, Ref<MicroGeometry> &r_geometry) {
	Ref<ImporterMesh> importer = ImporterMesh::from_mesh(p_mesh);
	String context;
	Error err = importer->generate_micro_geometry(context);
	ERR_FAIL_COND_V_MSG(err != OK, err, context);
	Ref<MicroGeometry> geometry = importer->get_micro_geometry();
	if (geometry.is_null()) {
		return OK;
	}
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

Error import_micro_geometry(const Ref<ArrayMesh> &p_mesh, const String &p_save_path, List<String> *r_gen_files) {
	ERR_FAIL_COND_V(p_mesh.is_null(), ERR_INVALID_PARAMETER);
	Ref<MicroGeometry> geometry;
	Error err = prepare_geometry(p_mesh, p_save_path, r_gen_files, geometry);
	ERR_FAIL_COND_V(err != OK, err);
	err = publish_geometry(geometry);
	ERR_FAIL_COND_V(err != OK, err);
	p_mesh->set_micro_geometry(geometry);
	return OK;
}

Error import_scene_micro_geometry(Node *p_scene, const String &p_save_path, List<String> *r_gen_files, const HashMap<Ref<ArrayMesh>, String> &p_external_mesh_paths) {
	HashSet<ObjectID> visited;
	Vector<Ref<ArrayMesh>> meshes;
	collect_object(p_scene, visited, meshes);
	Vector<Ref<MicroGeometry>> geometry;
	geometry.resize(meshes.size());
	for (int i = 0; i < meshes.size(); i++) {
		Error err = prepare_geometry(meshes[i], p_save_path, r_gen_files, geometry.write[i]);
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
