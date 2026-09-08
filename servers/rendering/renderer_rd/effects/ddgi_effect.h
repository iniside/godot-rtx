/**************************************************************************/
/*  ddgi_effect.h                                                          */
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

#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/ddgi_blend.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ddgi_camera.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ddgi_classify.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ddgi_relocate.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ddgi_state.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/ddgi_trace.slang.gen.h"
#include "servers/rendering/storage/environment_storage.h"

namespace RendererRD {

class DDGIEffect {
public:
	static constexpr uint32_t PROBE_AXIS = 16;
	static constexpr uint32_t PROBE_COUNT = PROBE_AXIS * PROBE_AXIS * PROBE_AXIS;
	static constexpr uint32_t MAX_CASCADES = 6;

	struct alignas(16) VolumeDescriptor {
		float origin[3] = {};
		float hysteresis = 0.97f;
		float rotation[4] = { 0, 0, 0, 1 };
		float ray_rotation[4] = { 0, 0, 0, 1 };
		float max_ray_distance = 0;
		float normal_bias = 0.1f;
		float view_bias = 0.1f;
		float distance_exponent = 50.0f;
		float irradiance_gamma = 5.0f;
		float irradiance_threshold = 0.25f;
		float brightness_threshold = 0.1f;
		float min_frontface_distance = 0.2f;
		float spacing[3] = {};
		uint32_t packed[5] = {};
		uint32_t reserved[4] = {};
	};
	static_assert(sizeof(VolumeDescriptor) == 128);

	struct Cascade {
		int64_t minimum_cell[3] = {};
		bool initialized = false;
		uint32_t dirty_count = PROBE_COUNT;
		uint64_t last_update_frame = 0;
		bool update_started = false;
		RID ray_data;
		RID irradiance;
		RID distance;
		RID probe_data;
		RID traced_data;
		RID validity;
		RID update_frame;
		RID variability;
		RID reset_mask;
	};

	struct Context {
		Cascade cascades[MAX_CASCADES];
		VolumeDescriptor descriptors[MAX_CASCADES];
		RID volume_buffer;
		RID frame_buffer;
		RID trace_pipeline;
		RID trace_sbt;
		RID material_source_pipeline;
		RID indirect_radiance;
		Size2i camera_size;
		uint32_t cascade_count = 0;
		uint32_t rays_per_probe = 0;
		float base_spacing = 0;
		uint64_t history_epoch = 0;
		uint64_t frame = 0;
		uint32_t fair_cursor = 0;
		uint64_t schedule_slot = 0;
#ifdef DEBUG_ENABLED
		String diagnostic_prefix;
		LocalVector<uint32_t> diagnostic_frames;
		uint32_t diagnostic_capture_index = 0;
#endif
		LocalVector<uint32_t> updates;
		~Context();
	};

	DDGIEffect();
	~DDGIEffect();
	Context *create_context(const RendererEnvironmentStorage::RaytracingSettings &p_settings);
	bool prepare(Context &p_context, const Vector3 &p_camera, const Vector3 &p_rt_origin, uint64_t p_history_epoch, int p_update_budget);
	bool begin_update(Context &p_context, uint32_t p_cascade);
	bool finish_update(Context &p_context, uint32_t p_cascade);
	RID get_trace_shader(bool p_radiance_array);
	LocalVector<RD::Uniform> get_grid_uniforms(const Context &p_context) const;
	void update_frame(Context &p_context, uint32_t p_cascade, uint32_t p_layers);
	bool render_camera(Context &p_context, RID p_scene_data, RID p_rt_frame, const RID p_surface[6], RID p_depth, const Size2i &p_size, bool p_orthogonal);

private:
	enum StateMode { STATE_RESET,
		STATE_PREPARE,
		STATE_FINISH,
		STATE_MODE_COUNT };
	DdgiBlendShaderRD blend_shader;
	DdgiClassifyShaderRD classify_shader;
	DdgiRelocateShaderRD relocate_shader;
	DdgiStateShaderRD state_shader;
	DdgiTraceShaderRD trace_shader;
	DdgiCameraShaderRD camera_shader;
	RID blend_version;
	RID classify_version;
	RID relocate_version;
	RID state_version;
	RID trace_version;
	RID camera_version;
	RID camera_pipeline;
	RID sampler;
	RID blend_pipelines[6];
	RID classify_pipeline;
	RID relocate_pipeline;
	RID state_pipelines[STATE_MODE_COUNT];
	bool available = false;

	RID _texture(uint32_t p_width, uint32_t p_height, RD::DataFormat p_format);
	bool _dispatch(RID p_shader, RID p_pipeline, const LocalVector<RD::Uniform> &p_uniforms, const void *p_constants, uint32_t p_constant_size, uint32_t p_x, uint32_t p_y = 1, uint32_t p_z = 1);
	bool _state(Context &p_context, uint32_t p_cascade, StateMode p_mode);
	void _capture_diagnostics(Context &p_context);
};

} // namespace RendererRD
