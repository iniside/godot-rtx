/**************************************************************************/
/*  micro_geometry.cpp                                                    */
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

#include "micro_geometry.h"

#include "core/object/class_db.h"

void MicroGeometry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_content_id"), &MicroGeometry::get_content_id);
	ClassDB::bind_method(D_METHOD("get_statistics"), &MicroGeometry::get_statistics);
}

Error MicroGeometry::copy_from(const Ref<Resource> &p_resource) {
	Ref<MicroGeometry> geometry = p_resource;
	ERR_FAIL_COND_V(geometry.is_null(), ERR_INVALID_PARAMETER);
	if (geometry.ptr() == this) {
		return OK;
	}
	Ref<MicroGeometryData> next_data = geometry->get_data();
	Error err = Resource::copy_from(p_resource);
	ERR_FAIL_COND_V(err != OK, err);
	set_data(next_data);
	return OK;
}

void MicroGeometry::reset_state() {
	set_data(Ref<MicroGeometryData>());
}

void MicroGeometry::set_data(const Ref<MicroGeometryData> &p_data) {
	if (data == p_data) {
		return;
	}
	data = p_data;
	emit_changed();
}

String MicroGeometry::get_content_id() const {
	return data.is_valid() ? data->get_content_id() : String();
}

Dictionary MicroGeometry::get_statistics() const {
	Dictionary result;
	if (data.is_null()) {
		return result;
	}
	const MicroGeometryData::Build &manifest = data->get_metadata();
	uint64_t leaf_clusters = 0;
	uint64_t triangles = 0;
	uint32_t levels = 0;
	uint64_t encoded_bytes = 0;
	for (const MicroGeometryData::Cluster &cluster : manifest.clusters) {
		if (cluster.refined_group == MicroGeometryData::INVALID_ID) {
			leaf_clusters++;
		}
		triangles += cluster.triangle_count;
	}
	for (const MicroGeometryData::Group &group : manifest.groups) {
		levels = MAX(levels, group.depth + 1);
	}
	for (const MicroGeometryData::Page &page : manifest.pages) {
		encoded_bytes += page.encoded_size;
	}
	result["surfaces"] = manifest.surfaces.size();
	result["clusters"] = manifest.clusters.size();
	result["leaf_clusters"] = leaf_clusters;
	result["triangles"] = triangles;
	result["groups"] = manifest.groups.size();
	result["levels"] = levels;
	result["terminal_groups"] = manifest.terminals.size();
	result["pages"] = manifest.pages.size();
	result["encoded_bytes"] = encoded_bytes;
	return result;
}

Ref<Resource> ResourceFormatLoaderMicroGeometry::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<MicroGeometryData> data;
	Error err = MicroGeometryData::load(p_path, data);
	if (r_error) {
		*r_error = err;
	}
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Resource>(), "Cannot load microgeometry '" + p_path + "'.");
	Ref<MicroGeometry> resource;
	if (p_cache_mode == CACHE_MODE_REPLACE || p_cache_mode == CACHE_MODE_REPLACE_DEEP) {
		resource = ResourceCache::get_ref(p_original_path.is_empty() ? p_path : p_original_path);
	}
	if (resource.is_null()) {
		resource.instantiate();
	}
	resource->set_data(data);
	if (r_progress) {
		*r_progress = 1.0f;
	}
	return resource;
}

void ResourceFormatLoaderMicroGeometry::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("mgdata");
}
bool ResourceFormatLoaderMicroGeometry::handles_type(const String &p_type) const {
	return p_type == "MicroGeometry";
}
String ResourceFormatLoaderMicroGeometry::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "mgdata" ? "MicroGeometry" : String();
}
void ResourceFormatLoaderMicroGeometry::get_classes_used(const String &p_path, HashSet<StringName> *r_classes) {
	r_classes->insert("MicroGeometry");
}

Error ResourceFormatSaverMicroGeometry::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<MicroGeometry> geometry = p_resource;
	ERR_FAIL_COND_V(geometry.is_null() || geometry->get_data().is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_path.get_extension().to_lower() != "mgdata", ERR_FILE_UNRECOGNIZED);
	return geometry->get_data()->save(p_path);
}

bool ResourceFormatSaverMicroGeometry::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<MicroGeometry>(p_resource.ptr()) != nullptr;
}
void ResourceFormatSaverMicroGeometry::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (recognize(p_resource)) {
		p_extensions->push_back("mgdata");
	}
}
