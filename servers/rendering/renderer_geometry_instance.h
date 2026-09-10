/**************************************************************************/
/*  renderer_geometry_instance.h                                          */
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

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "servers/rendering/storage/utilities.h"
#include "servers/rendering/renderer_scene_data.h"

// API definition for our RenderGeometryInstance class so we can expose this through GDExtension in the near future
class RenderGeometryInstance {
public:
	virtual ~RenderGeometryInstance() {}
	bool micro_geometry_raster_only = false;
	bool micro_geometry_cpu_culling = false;
	void *micro_geometry_routing_data = nullptr;
	void (*micro_geometry_routing_changed)(void *, bool) = nullptr;
	void set_micro_geometry_raster_only(bool p_enabled) {
		if (micro_geometry_raster_only == p_enabled) {
			return;
		}
		micro_geometry_raster_only = p_enabled;
		if (micro_geometry_routing_changed) {
			micro_geometry_routing_changed(micro_geometry_routing_data, p_enabled);
		}
	}


	virtual void _mark_dirty() = 0;
	virtual void _mark_instance_data_dirty() {}
	virtual void scene_data_changed() = 0;
	virtual void scene_membership_changed() {}

	virtual RID get_instance_rid() const { return RID(); }
	virtual void set_rt_visibility(bool p_visible_receiver, bool p_casts_shadows, bool p_shadows_only) {}
	virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) = 0;
	virtual void set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) = 0;
	virtual void set_parent_fade_alpha(float p_alpha) = 0;
	virtual void set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) = 0;
	virtual void set_lightmap_capture(const Color *p_sh9) = 0;
	virtual void set_instance_shader_uniforms_offset(int32_t p_offset) = 0;

	virtual void reset_motion_vectors() = 0;

	virtual Transform3D get_transform() = 0;
	virtual Transform3D get_camera_relative_transform(const double *p_origin) const = 0;
	virtual AABB get_aabb() = 0;

	virtual void clear_light_instances() = 0;
	virtual void pair_light_instance(const RID p_light_instance, RSE::LightType light_type, uint32_t placement_idx) = 0;
	virtual void pair_reflection_probe_instances(const RID *p_reflection_probe_instances, uint32_t p_reflection_probe_instance_count) = 0;
	virtual void pair_decal_instances(const RID *p_decal_instances, uint32_t p_decal_instance_count) = 0;
	virtual void pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) = 0;

	virtual void set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) = 0;

	virtual void set_rt_procedural(bool p_procedural, const AABB &p_aabb) {}
	virtual void set_rt_procedural_bounds(const Vector<float> &p_aabb_data, bool p_expose_bounds) {}
};

// Base implementation of RenderGeometryInstance shared by internal renderers.
class RenderGeometryInstanceBase : public RenderGeometryInstance {
public:
	// setup
	uint32_t base_flags = 0;
	uint32_t flags_cache = 0;

	// used during rendering
	float depth = 0;

	bool rt_visible_receiver = false;
	bool rt_casts_shadows = false;
	bool rt_shadows_only = false;

	Transform3D transform;
	double origin[3] = {};
	bool non_uniform_scale = false;
	float lod_model_scale = 1.0;


	bool fade_near = false;
	float fade_near_begin = 0;
	float fade_near_end = 0;
	bool fade_far = false;
	float fade_far_begin = 0;
	float fade_far_end = 0;

	float parent_fade_alpha = 1.0;
	float force_alpha = 1.0;

	int32_t shader_uniforms_offset = -1;

	struct Data {
		//data used less often goes into regular heap


		bool cast_double_sided_shadows = false;
		bool dirty_dependencies = false;

		DependencyTracker dependency_tracker;
	};

	Data *data = nullptr;
	RenderSceneInstanceData *scene_data = nullptr;
	void scene_data_changed() override;

	virtual void scene_membership_changed() override;
	virtual RID get_instance_rid() const override { return scene_data->render_handle; }
	virtual void set_rt_visibility(bool p_visible_receiver, bool p_casts_shadows, bool p_shadows_only) override {
		rt_visible_receiver = p_visible_receiver;
		rt_casts_shadows = p_casts_shadows;
		rt_shadows_only = p_shadows_only;
	}
	virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) override;
	virtual void set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) override;
	virtual void set_parent_fade_alpha(float p_alpha) override;
	virtual void set_instance_shader_uniforms_offset(int32_t p_offset) override;

	virtual void reset_motion_vectors() override;

	virtual Transform3D get_transform() override;
	Transform3D get_camera_relative_transform(const double *p_origin) const override;
	virtual AABB get_aabb() override;
};
