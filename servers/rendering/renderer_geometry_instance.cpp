/**************************************************************************/
/*  renderer_geometry_instance.cpp                                        */
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

#include "servers/rendering/renderer_geometry_instance.h"

void RenderGeometryInstanceBase::scene_data_changed() {
	force_alpha = CLAMP(1.0f - scene_data->transparency, 0.0f, 1.0f);
	data->cast_double_sided_shadows = scene_data->cast_shadows == RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED;
	data->dirty_dependencies = true;
	_mark_dirty();
	_mark_instance_data_dirty();
}

void RenderGeometryInstanceBase::set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) {
	transform = p_transform;
	for (int axis = 0; axis < 3; axis++) {
		origin[axis] = scene_data->origin[axis];
	}
	scene_data->mirror = p_transform.basis.determinant() < 0;
	scene_data->aabb = p_aabb;
	scene_data->transformed_aabb = p_transformed_aabb;

	Vector3 model_scale_vec = p_transform.basis.get_scale_abs();
	// handle non uniform scale here

	float max_scale = MAX(model_scale_vec.x, MAX(model_scale_vec.y, model_scale_vec.z));
	float min_scale = MIN(model_scale_vec.x, MIN(model_scale_vec.y, model_scale_vec.z));
	non_uniform_scale = max_scale >= 0.0 && (min_scale / max_scale) < 0.999;

	lod_model_scale = max_scale;
	_mark_instance_data_dirty();
}

void RenderGeometryInstanceBase::set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) {
	fade_near = p_enable_near;
	fade_near_begin = p_near_begin;
	fade_near_end = p_near_end;
	fade_far = p_enable_far;
	fade_far_begin = p_far_begin;
	fade_far_end = p_far_end;
	_mark_instance_data_dirty();
}

void RenderGeometryInstanceBase::set_parent_fade_alpha(float p_alpha) {
	parent_fade_alpha = p_alpha;
	_mark_instance_data_dirty();
}

void RenderGeometryInstanceBase::set_instance_shader_uniforms_offset(int32_t p_offset) {
	shader_uniforms_offset = p_offset;

	_mark_dirty();
}

void RenderGeometryInstanceBase::reset_motion_vectors() {
}

Transform3D RenderGeometryInstanceBase::get_transform() {
	return transform;
}

Transform3D RenderGeometryInstanceBase::get_camera_relative_transform(const double *p_origin) const {
	Transform3D relative = transform;
	for (int axis = 0; axis < 3; axis++) {
		relative.origin[axis] = origin[axis] - p_origin[axis];
	}
	return relative;
}

AABB RenderGeometryInstanceBase::get_aabb() {
	return scene_data->aabb;
}

void RenderGeometryInstanceBase::scene_membership_changed() {
	_mark_instance_data_dirty();
}
