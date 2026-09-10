/**************************************************************************/
/*  ddgi_effect.cpp                                                        */
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

#include "ddgi_effect.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/quaternion.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

namespace RendererRD {

DDGIEffect::Context::~Context() {
	RD *rd = RD::get_singleton();
	if (rd->raytracing_pipeline_is_valid(trace_pipeline)) {
		rd->free_rid(trace_sbt);
		rd->free_rid(trace_pipeline);
	}
	for (RID resource : { frame_buffer, indirect_radiance }) {
		if (resource.is_valid()) {
			rd->free_rid(resource);
		}
	}
	for (Cascade &cascade : cascades) {
		for (RID resource : { cascade.ray_data, cascade.irradiance, cascade.distance, cascade.probe_data, cascade.traced_data, cascade.validity, cascade.update_frame, cascade.variability, cascade.reset_mask }) {
			if (resource.is_valid()) {
				rd->free_rid(resource);
			}
		}
	}
	if (volume_buffer.is_valid()) {
		rd->free_rid(volume_buffer);
	}
}

DDGIEffect::DDGIEffect() {
	RD::SamplerState sampler_state;
	sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler = RD::get_singleton()->sampler_create(sampler_state);
	String scene_defines;
	scene_defines += "#define USE_DOUBLE_PRECISION\n";
	trace_shader.initialize(Vector<String>{ "\n", "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" }, scene_defines);
	trace_version = trace_shader.version_create();
	camera_shader.initialize(Vector<String>{ "\n" }, scene_defines);
	camera_version = camera_shader.version_create();
	RID camera = camera_shader.version_get_shader(camera_version, 0);
	camera_pipeline = camera.is_valid() ? RD::get_singleton()->compute_pipeline_create(camera) : RID();
	Vector<String> blend_modes;
	for (int output = 0; output < 2; output++) {
		for (int rays : { 64, 128, 256 }) {
			blend_modes.push_back(vformat("\n#define RTXGI_DDGI_BLEND_RAYS_PER_PROBE %d\n", rays) + (output == 0 ? "#define MODE_IRRADIANCE\n" : ""));
		}
	}
	blend_shader.initialize(blend_modes);
	blend_version = blend_shader.version_create();
	available = camera_pipeline.is_valid() && sampler.is_valid();
	for (int i = 0; i < 6; i++) {
		RID shader = blend_shader.version_get_shader(blend_version, i);
		blend_pipelines[i] = shader.is_valid() ? RD::get_singleton()->compute_pipeline_create(shader) : RID();
		available &= blend_pipelines[i].is_valid();
	}
	classify_shader.initialize(Vector<String>{ "\n" });
	classify_version = classify_shader.version_create();
	RID shader = classify_shader.version_get_shader(classify_version, 0);
	classify_pipeline = shader.is_valid() ? RD::get_singleton()->compute_pipeline_create(shader) : RID();
	relocate_shader.initialize(Vector<String>{ "\n" });
	relocate_version = relocate_shader.version_create();
	shader = relocate_shader.version_get_shader(relocate_version, 0);
	relocate_pipeline = shader.is_valid() ? RD::get_singleton()->compute_pipeline_create(shader) : RID();
	available &= classify_pipeline.is_valid() && relocate_pipeline.is_valid();
	state_shader.initialize(Vector<String>{ "\n#define MODE_RESET\n", "\n#define MODE_PREPARE\n", "\n#define MODE_FINISH\n" });
	state_version = state_shader.version_create();
	for (int i = 0; i < STATE_MODE_COUNT; i++) {
		shader = state_shader.version_get_shader(state_version, i);
		state_pipelines[i] = shader.is_valid() ? RD::get_singleton()->compute_pipeline_create(shader) : RID();
		available &= state_pipelines[i].is_valid();
	}
}

DDGIEffect::~DDGIEffect() {
	debug_pipeline.clear();
	if (debug_version.is_valid()) {
		debug_shader.version_free(debug_version);
	}
	trace_shader.version_free(trace_version);
	camera_shader.version_free(camera_version);
	if (sampler.is_valid()) {
		RD::get_singleton()->free_rid(sampler);
	}
	blend_shader.version_free(blend_version);
	classify_shader.version_free(classify_version);
	relocate_shader.version_free(relocate_version);
	state_shader.version_free(state_version);
}

RID DDGIEffect::_texture(uint32_t p_width, uint32_t p_height, RD::DataFormat p_format) {
	RD::TextureFormat format;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.width = p_width;
	format.height = p_height;
	format.array_layers = PROBE_AXIS;
	format.format = p_format;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	ERR_FAIL_COND_V_MSG(!RD::get_singleton()->texture_is_format_supported_for_usage(p_format, format.usage_bits), RID(), "The DDGI texture format is unsupported on this device.");
	return RD::get_singleton()->texture_create(format, RD::TextureView());
}

DDGIEffect::Context *DDGIEffect::create_context(const RendererEnvironmentStorage::RaytracingSettings &p_settings) {
	ERR_FAIL_COND_V(!available, nullptr);
	ERR_FAIL_COND_V(p_settings.ddgi_cascade_count < 1 || p_settings.ddgi_cascade_count > int(MAX_CASCADES), nullptr);
	ERR_FAIL_COND_V(p_settings.ddgi_rays_per_probe != 64 && p_settings.ddgi_rays_per_probe != 128 && p_settings.ddgi_rays_per_probe != 256, nullptr);
	ERR_FAIL_COND_V(!Math::is_finite(p_settings.ddgi_probe_spacing) || p_settings.ddgi_probe_spacing <= 0.0f, nullptr);
	Context *context = memnew(Context);
#ifdef DEBUG_ENABLED
	context->diagnostic_prefix = OS::get_singleton()->get_environment("GODOT_DDGI_CAPTURE_PREFIX");
	if (!context->diagnostic_prefix.is_empty()) {
		String frames = OS::get_singleton()->get_environment("GODOT_DDGI_CAPTURE_FRAMES");
		if (frames.is_empty()) {
			frames = "120";
		}
		for (const String &value : frames.split(",")) {
			int64_t frame = value.to_int();
			if (value.is_valid_int() && frame > 0 && frame <= UINT32_MAX &&
					(context->diagnostic_frames.is_empty() || frame > context->diagnostic_frames[context->diagnostic_frames.size() - 1])) {
				context->diagnostic_frames.push_back(uint32_t(frame));
			} else {
				WARN_PRINT("GODOT_DDGI_CAPTURE_FRAMES requires increasing positive 32-bit frame numbers; skipping invalid entry.");
			}
		}
	}
#endif
	context->cascade_count = p_settings.ddgi_cascade_count;
	context->rays_per_probe = p_settings.ddgi_rays_per_probe;
	context->base_spacing = p_settings.ddgi_probe_spacing;
	context->volume_buffer = RD::get_singleton()->storage_buffer_create(sizeof(context->descriptors));
	context->frame_buffer = RD::get_singleton()->uniform_buffer_create(16);
	bool valid = context->volume_buffer.is_valid() && context->frame_buffer.is_valid();
	for (uint32_t i = 0; i < context->cascade_count && valid; i++) {
		Cascade &cascade = context->cascades[i];
		cascade.ray_data = _texture(context->rays_per_probe, PROBE_AXIS * PROBE_AXIS, RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
		cascade.irradiance = _texture(PROBE_AXIS * 8, PROBE_AXIS * 8, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
		cascade.distance = _texture(PROBE_AXIS * 16, PROBE_AXIS * 16, RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
		cascade.probe_data = _texture(PROBE_AXIS, PROBE_AXIS, RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
		cascade.traced_data = _texture(PROBE_AXIS, PROBE_AXIS, RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
		cascade.validity = _texture(PROBE_AXIS, PROBE_AXIS, RD::DATA_FORMAT_R32_UINT);
		cascade.update_frame = _texture(PROBE_AXIS, PROBE_AXIS, RD::DATA_FORMAT_R32_UINT);
		cascade.variability = _texture(PROBE_AXIS * 6, PROBE_AXIS * 6, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
		cascade.reset_mask = RD::get_singleton()->storage_buffer_create(PROBE_COUNT * sizeof(uint32_t));
		for (RID resource : { cascade.ray_data, cascade.irradiance, cascade.distance, cascade.probe_data, cascade.traced_data, cascade.validity, cascade.update_frame, cascade.variability, cascade.reset_mask }) {
			valid &= resource.is_valid();
		}
	}
	if (!valid) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "Failed to allocate camera-following DDGI resources.");
	}
	return context;
}

bool DDGIEffect::_dispatch(RID p_shader, RID p_pipeline, const LocalVector<RD::Uniform> &p_uniforms, const void *p_constants, uint32_t p_constant_size, uint32_t p_x, uint32_t p_y, uint32_t p_z) {
	ERR_FAIL_COND_V(p_shader.is_null() || p_pipeline.is_null(), false);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(p_shader, 0, p_uniforms);
	ERR_FAIL_COND_V(uniform_set.is_null(), false);
	RD *rd = RD::get_singleton();
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, p_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, p_constants, p_constant_size);
	rd->compute_list_dispatch(list, p_x, p_y, p_z);
	rd->compute_list_end();
	return true;
}

bool DDGIEffect::_state(Context &p_context, uint32_t p_cascade, StateMode p_mode) {
	Cascade &cascade = p_context.cascades[p_cascade];
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_context.volume_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, { cascade.probe_data }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, { cascade.traced_data }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, { cascade.validity }));
	if (p_mode != STATE_FINISH) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, { cascade.irradiance }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, { cascade.distance }));
	}
	if (p_mode == STATE_RESET) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, { cascade.ray_data }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { cascade.reset_mask }));
	}
	if (p_mode != STATE_PREPARE) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, { cascade.update_frame }));
	}
	uint32_t constants[4] = { p_cascade, uint32_t(p_context.frame), 0, 0 };
	RENDER_TIMESTAMP(vformat("DDGI Cascade %d State %s", p_cascade, p_mode == STATE_RESET ? "Reset" : (p_mode == STATE_PREPARE ? "Prepare" : "Finish")));
	bool dispatched = _dispatch(state_shader.version_get_shader(state_version, p_mode), state_pipelines[p_mode], uniforms, constants, sizeof(constants), PROBE_COUNT / 32);
	RENDER_TIMESTAMP("DDGI State Complete");
	return dispatched;
}

bool DDGIEffect::prepare(Context &p_context, const double *p_camera, const double *p_rt_origin, uint64_t p_history_epoch, int p_update_budget) {
	for (int axis = 0; axis < 3; axis++) {
		ERR_FAIL_COND_V(!Math::is_finite(p_camera[axis]) || !Math::is_finite(p_rt_origin[axis]), false);
	}
	p_context.frame++;
	p_context.camera_rendered = false;
	p_context.camera_scene_data = RID();
	p_context.updates.clear();
	bool reset_cascade[MAX_CASCADES] = {};
	uint32_t reset_mask[PROBE_COUNT];
	const float outer_spacing = p_context.base_spacing * float(1u << (p_context.cascade_count - 1));
	const float max_distance = Math::sqrt(3.0f) * float(PROBE_AXIS - 1) * outer_spacing;
	for (uint32_t i = 0; i < p_context.cascade_count; i++) {
		Cascade &cascade = p_context.cascades[i];
		ERR_FAIL_COND_V(cascade.update_started, false);
		VolumeDescriptor &descriptor = p_context.descriptors[i];
		const double spacing = double(p_context.base_spacing) * double(1u << i);
		int64_t minimum[3];
		uint32_t offset[3];
		for (uint32_t axis = 0; axis < 3; axis++) {
			double cell = Math::floor(double(p_camera[axis]) / spacing);
			ERR_FAIL_COND_V(cell <= -double(INT64_MAX) + 4096.0 || cell >= double(INT64_MAX) - 4096.0, false);
			minimum[axis] = int64_t(cell) - int64_t(PROBE_AXIS / 2);
			offset[axis] = uint32_t((minimum[axis] % int64_t(PROBE_AXIS) + PROBE_AXIS) % PROBE_AXIS);
			descriptor.origin[axis] = float((double(minimum[axis]) * spacing - double(p_rt_origin[axis])) + (double(PROBE_AXIS - 1) * 0.5 - offset[axis]) * spacing);
			descriptor.spacing[axis] = float(spacing);
		}
		const bool full_reset = !cascade.initialized || p_context.history_epoch != p_history_epoch;
		uint32_t entering_count = 0;
		for (uint32_t index = 0; index < PROBE_COUNT; index++) {
			uint32_t physical[3] = { index % PROBE_AXIS, index / (PROBE_AXIS * PROBE_AXIS), (index / PROBE_AXIS) % PROBE_AXIS };
			bool entering = full_reset;
			for (uint32_t axis = 0; axis < 3; axis++) {
				int64_t absolute = minimum[axis] + ((physical[axis] + PROBE_AXIS - offset[axis]) % PROBE_AXIS);
				entering |= absolute < cascade.minimum_cell[axis] || absolute >= cascade.minimum_cell[axis] + PROBE_AXIS;
			}
			reset_mask[index] = entering ? 1u : 0u;
			entering_count += reset_mask[index];
		}
		if (entering_count > 0) {
			RD::get_singleton()->buffer_update(cascade.reset_mask, 0, sizeof(reset_mask), reset_mask);
			reset_cascade[i] = true;
			cascade.dirty_count = MAX(cascade.dirty_count, entering_count);
		}
		for (uint32_t axis = 0; axis < 3; axis++) {
			cascade.minimum_cell[axis] = minimum[axis];
		}
		Quaternion ray_rotation(Vector3(1, 2, 3).normalized(), real_t(Math::fmod(double(p_context.frame + i * 17) * 0.6180339887498948, 1.0) * Math::TAU));
		for (uint32_t component = 0; component < 4; component++) {
			descriptor.ray_rotation[component] = float(ray_rotation[component]);
		}
		descriptor.max_ray_distance = max_distance;
		descriptor.normal_bias = float(spacing * 0.05);
		descriptor.view_bias = float(spacing * 0.05);
		descriptor.min_frontface_distance = float(spacing * 0.1);
		descriptor.packed[0] = PROBE_AXIS | (PROBE_AXIS << 10) | (PROBE_AXIS << 20);
		descriptor.packed[1] = 6553u | (16383u << 16);
		descriptor.packed[2] = p_context.rays_per_probe | (6u << 16) | (14u << 24);
		descriptor.packed[3] = offset[0] | (offset[1] << 16);
		descriptor.packed[4] = offset[2] | (1u << 16) | (6u << 17) | (3u << 20) | (1u << 23) | (1u << 24);
	}
	RD::get_singleton()->buffer_update(p_context.volume_buffer, 0, sizeof(p_context.descriptors), p_context.descriptors);
	for (uint32_t i = 0; i < p_context.cascade_count; i++) {
		if (reset_cascade[i] && !_state(p_context, i, STATE_RESET)) {
			for (Cascade &cascade : p_context.cascades) {
				cascade.initialized = false;
			}
			return false;
		}
		if (reset_cascade[i]) {
			p_context.cascades[i].reset_frame = p_context.frame;
		}
		p_context.cascades[i].initialized = true;
	}
	p_context.history_epoch = p_history_epoch;
	bool selected[MAX_CASCADES] = {};
	uint32_t budget = uint32_t(CLAMP(p_update_budget, 1, int(p_context.cascade_count)));
	for (uint32_t work = 0; work < budget; work++) {
		uint32_t candidate = p_context.fair_cursor;
		if ((p_context.schedule_slot++ & 1u) == 0u) {
			while (selected[candidate]) {
				candidate = (candidate + 1) % p_context.cascade_count;
			}
			p_context.fair_cursor = (candidate + 1) % p_context.cascade_count;
		} else {
			candidate = MAX_CASCADES;
			for (uint32_t i = 0; i < p_context.cascade_count; i++) {
				if (selected[i]) {
					continue;
				}
				if (candidate == MAX_CASCADES || p_context.cascades[i].dirty_count > p_context.cascades[candidate].dirty_count ||
						(p_context.cascades[i].dirty_count == p_context.cascades[candidate].dirty_count && p_context.cascades[i].last_update_frame < p_context.cascades[candidate].last_update_frame)) {
					candidate = i;
				}
			}
		}
		selected[candidate] = true;
		p_context.updates.push_back(candidate);
	}
	return true;
}

bool DDGIEffect::begin_update(Context &p_context, uint32_t p_cascade) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_cascade, p_context.cascade_count, false);
	bool scheduled = false;
	for (uint32_t cascade : p_context.updates) {
		scheduled |= cascade == p_cascade;
	}
	ERR_FAIL_COND_V(!scheduled || p_context.cascades[p_cascade].update_started, false);
	if (!_state(p_context, p_cascade, STATE_PREPARE)) {
		return false;
	}
	p_context.cascades[p_cascade].update_started = true;
	return true;
}

bool DDGIEffect::finish_update(Context &p_context, uint32_t p_cascade) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_cascade, p_context.cascade_count, false);
	Cascade &cascade = p_context.cascades[p_cascade];
	ERR_FAIL_COND_V(!cascade.update_started, false);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_context.volume_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, { cascade.ray_data }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, { cascade.probe_data }));
	uint32_t constants[6] = { p_cascade, 0, 0, 0, 0, 0 };
	RENDER_TIMESTAMP(vformat("DDGI Cascade %d Classify", p_cascade));
	if (!_dispatch(classify_shader.version_get_shader(classify_version, 0), classify_pipeline, uniforms, constants, sizeof(constants), PROBE_COUNT / 32)) {
		return false;
	}
	const int ray_variant = p_context.rays_per_probe == 64 ? 0 : (p_context.rays_per_probe == 128 ? 1 : 2);
	for (int output = 0; output < 2; output++) {
		LocalVector<RD::Uniform> blend_uniforms(uniforms);
		blend_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, output == 0 ? 2 : 3, { output == 0 ? cascade.irradiance : cascade.distance }));
		if (output == 0) {
			blend_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, { cascade.variability }));
		}
		int variant = ray_variant + output * 3;
		RENDER_TIMESTAMP(vformat("DDGI Cascade %d Blend %s", p_cascade, output == 0 ? "Irradiance" : "Distance"));
		if (!_dispatch(blend_shader.version_get_shader(blend_version, variant), blend_pipelines[variant], blend_uniforms, constants, sizeof(constants), PROBE_AXIS, PROBE_AXIS, PROBE_AXIS)) {
			return false;
		}
	}
	RENDER_TIMESTAMP(vformat("DDGI Cascade %d Relocate", p_cascade));
	if (!_dispatch(relocate_shader.version_get_shader(relocate_version, 0), relocate_pipeline, uniforms, constants, sizeof(constants), PROBE_COUNT / 32) || !_state(p_context, p_cascade, STATE_FINISH)) {
		return false;
	}
	cascade.update_started = false;
	cascade.last_update_frame = p_context.frame;
	cascade.dirty_count = 0;
	return true;
}

RID DDGIEffect::get_trace_shader(bool p_radiance_array) {
	return trace_shader.version_get_shader(trace_version, p_radiance_array ? 1 : 0);
}

LocalVector<RD::Uniform> DDGIEffect::get_grid_uniforms(const Context &p_context) const {
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_context.volume_buffer }));
	for (uint32_t binding = 1; binding <= 4; binding++) {
		RD::Uniform uniform;
		uniform.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		uniform.binding = binding;
		for (uint32_t i = 0; i < MAX_CASCADES; i++) {
			const Cascade &cascade = p_context.cascades[MIN(i, p_context.cascade_count - 1)];
			const RID resources[] = { cascade.irradiance, cascade.distance, cascade.probe_data, cascade.validity };
			uniform.append_id(resources[binding - 1]);
		}
		uniforms.push_back(uniform);
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 5, { sampler }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, { p_context.frame_buffer }));
	return uniforms;
}

void DDGIEffect::update_frame(Context &p_context, uint32_t p_cascade, uint32_t p_layers) {
	uint32_t data[4] = { p_cascade, p_context.cascade_count, uint32_t(p_context.frame), p_layers };
	RD::get_singleton()->buffer_update(p_context.frame_buffer, 0, sizeof(data), data);
}

bool DDGIEffect::render_camera(Context &p_context, RID p_scene_data, RID p_rt_frame, const RID p_surface[6], RID p_depth, const Size2i &p_size, const Size2i &p_interpolation_size, bool p_orthogonal) {
	RD *rd = RD::get_singleton();
	p_context.camera_rendered = false;
	if (p_context.interpolation_size != p_interpolation_size) {
		if (p_context.indirect_radiance.is_valid()) {
			rd->free_rid(p_context.indirect_radiance);
		}
		p_context.indirect_radiance = RID();
		p_context.interpolation_size = Size2i();
	}
	if (p_context.indirect_radiance.is_null()) {
		RD::TextureFormat format;
		format.width = p_interpolation_size.x;
		format.height = p_interpolation_size.y;
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		p_context.indirect_radiance = rd->texture_create(format, RD::TextureView());
		ERR_FAIL_COND_V(p_context.indirect_radiance.is_null(), false);
		p_context.interpolation_size = p_interpolation_size;
		print_verbose(vformat("DDGI interpolation resolution: %dx%d; full-resolution camera: %dx%d.", p_interpolation_size.x, p_interpolation_size.y, p_size.x, p_size.y));
	}
	p_context.camera_size = p_size;
	RID shader = camera_shader.version_get_shader(camera_version, 0);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, { p_scene_data }));
	for (uint32_t i = 0; i < 6; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, i + 1, { p_surface[i] }));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, { p_depth }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, { p_rt_frame }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, { p_context.indirect_radiance }));
	RID camera_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms);
	RID grid_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 2, get_grid_uniforms(p_context));
	ERR_FAIL_COND_V(camera_set.is_null() || grid_set.is_null(), false);
	uint32_t constants[6] = { uint32_t(p_size.x), uint32_t(p_size.y), uint32_t(p_orthogonal), 0, uint32_t(p_interpolation_size.x), uint32_t(p_interpolation_size.y) };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, camera_pipeline);
	rd->compute_list_bind_uniform_set(list, camera_set, 0);
	rd->compute_list_bind_uniform_set(list, grid_set, 2);
	rd->compute_list_set_push_constant(list, constants, sizeof(constants));
	rd->compute_list_dispatch_threads(list, p_interpolation_size.x, p_interpolation_size.y, 1);
	rd->compute_list_end();
	p_context.camera_scene_data = p_scene_data;
	p_context.camera_rendered = true;
	_capture_diagnostics(p_context);
	return true;
}

bool DDGIEffect::render_debug(Context &p_context, RID p_framebuffer, RID p_rt_frame, const RID p_surface[6], RID p_depth, const Size2i &p_output_size, bool p_orthogonal, uint32_t p_mode) {
	ERR_FAIL_COND_V(p_mode > 3 || !p_context.camera_rendered || p_context.camera_scene_data.is_null(), false);
	RD *rd = RD::get_singleton();
	if (debug_version.is_null()) {
		String defines;
		defines += "#define USE_DOUBLE_PRECISION\n";
		debug_shader.initialize(Vector<String>{ "\n" }, defines);
		debug_version = debug_shader.version_create();
		RID shader = debug_shader.version_get_shader(debug_version, 0);
		ERR_FAIL_COND_V(shader.is_null(), false);
		debug_pipeline.setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled());
	}
	RID shader = debug_shader.version_get_shader(debug_version, 0);
	ERR_FAIL_COND_V(shader.is_null(), false);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, { p_context.camera_scene_data }));
	for (uint32_t i = 0; i < 6; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, i + 1, { p_surface[i] }));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, { p_depth }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, { p_rt_frame }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 9, { p_context.indirect_radiance }));
	RID camera_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms);
	RID debug_sets[MAX_CASCADES];
	uint32_t reset_cascades = 0;
	for (uint32_t i = 0; i < p_context.cascade_count; i++) {
		const Cascade &cascade = p_context.cascades[i];
		LocalVector<RD::Uniform> debug_uniforms;
		debug_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, { cascade.update_frame }));
		debug_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { cascade.reset_mask }));
		debug_sets[i] = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 1, debug_uniforms);
		ERR_FAIL_COND_V(debug_sets[i].is_null(), false);
		if (cascade.reset_frame == p_context.frame) {
			reset_cascades |= 1u << i;
		}
	}
	RID grid_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 2, get_grid_uniforms(p_context));
	ERR_FAIL_COND_V(camera_set.is_null() || grid_set.is_null(), false);
	RID pipeline = debug_pipeline.get_render_pipeline(RD::INVALID_FORMAT_ID, rd->framebuffer_get_format(p_framebuffer));
	ERR_FAIL_COND_V(pipeline.is_null(), false);
	uint32_t constants[10] = { uint32_t(p_output_size.x), uint32_t(p_output_size.y), uint32_t(p_context.camera_size.x), uint32_t(p_context.camera_size.y), p_mode, 0, reset_cascades, uint32_t(p_orthogonal), uint32_t(p_context.interpolation_size.x), uint32_t(p_context.interpolation_size.y) };
	RD::DrawListID list = rd->draw_list_begin(p_framebuffer);
	rd->draw_list_bind_render_pipeline(list, pipeline);
	rd->draw_list_bind_uniform_set(list, camera_set, 0);
	rd->draw_list_bind_uniform_set(list, debug_sets[0], 1);
	rd->draw_list_bind_uniform_set(list, grid_set, 2);
	if (p_mode <= 1) {
		for (int cascade = int(p_context.cascade_count) - 1; cascade >= 0; cascade--) {
			rd->draw_list_bind_uniform_set(list, debug_sets[cascade], 1);
			constants[5] = uint32_t(cascade);
			constants[4] = 4;
			rd->draw_list_set_push_constant(list, constants, sizeof(constants));
			rd->draw_list_draw(list, false, 12, 6);
			constants[4] = p_mode;
			rd->draw_list_set_push_constant(list, constants, sizeof(constants));
			rd->draw_list_draw(list, false, PROBE_COUNT, 6);
		}
	} else {
		rd->draw_list_set_push_constant(list, constants, sizeof(constants));
		rd->draw_list_draw(list, false, 1, 6);
	}
	rd->draw_list_end();
	return true;
}

void DDGIEffect::_capture_diagnostics(Context &p_context) {
#ifdef DEBUG_ENABLED
	// Explicit, bounded GPU readback for manual rendering investigations only.
	// A unique render-buffer suffix keeps simultaneous viewports independent.
	if (p_context.diagnostic_capture_index >= p_context.diagnostic_frames.size() ||
			p_context.frame < p_context.diagnostic_frames[p_context.diagnostic_capture_index]) {
		return;
	}
	p_context.diagnostic_capture_index++;
	String prefix = p_context.diagnostic_prefix + "-" + uitos(p_context.volume_buffer.get_id()) + "-frame-" + uitos(p_context.frame);
	Dictionary metadata;
	metadata["frame"] = p_context.frame;
	metadata["cascade_count"] = p_context.cascade_count;
	metadata["rays_per_probe"] = p_context.rays_per_probe;
	metadata["base_spacing"] = p_context.base_spacing;
	metadata["camera_width"] = p_context.interpolation_size.x;
	metadata["camera_height"] = p_context.interpolation_size.y;
	metadata["internal_width"] = p_context.camera_size.x;
	metadata["internal_height"] = p_context.camera_size.y;
	metadata["camera_signal"] = "irradiance / pi, before material and ambient occlusion";
	metadata["probe_axis"] = PROBE_AXIS;
	metadata["texture_layer_order"] = "Y layers, Z rows, X columns; little-endian components";
	Array cascades;
	auto save = [&](const String &p_name, RID p_texture, uint32_t p_layers) {
		Ref<FileAccess> file = FileAccess::open(prefix + "-" + p_name + ".bin", FileAccess::WRITE);
		ERR_FAIL_COND(file.is_null());
		for (uint32_t layer = 0; layer < p_layers; layer++) {
			file->store_buffer(RD::get_singleton()->texture_get_data(p_texture, layer));
		}
	};
	for (uint32_t i = 0; i < p_context.cascade_count; i++) {
		const Cascade &cascade = p_context.cascades[i];
		Dictionary data;
		Array minimum;
		for (uint32_t axis = 0; axis < 3; axis++) {
			minimum.push_back(cascade.minimum_cell[axis]);
		}
		data["minimum_cell"] = minimum;
		data["last_update_frame"] = cascade.last_update_frame;
		cascades.push_back(data);
		String name = "cascade-" + itos(i) + "-";
		save(name + "rays-rgba32f", cascade.ray_data, PROBE_AXIS);
		save(name + "irradiance-rgba16f", cascade.irradiance, PROBE_AXIS);
		save(name + "distance-rgba32f", cascade.distance, PROBE_AXIS);
		save(name + "position-state-rgba32f", cascade.probe_data, PROBE_AXIS);
		save(name + "traced-position-state-rgba32f", cascade.traced_data, PROBE_AXIS);
		save(name + "validity-r32ui", cascade.validity, PROBE_AXIS);
		save(name + "update-frame-r32ui", cascade.update_frame, PROBE_AXIS);
	}
	metadata["cascades"] = cascades;
	save("camera-rgba16f", p_context.indirect_radiance, 1);
	Ref<FileAccess> descriptors = FileAccess::open(prefix + "-volume-descriptors.bin", FileAccess::WRITE);
	ERR_FAIL_COND(descriptors.is_null());
	descriptors->store_buffer(RD::get_singleton()->buffer_get_data(p_context.volume_buffer));
	Ref<FileAccess> file = FileAccess::open(prefix + ".json", FileAccess::WRITE);
	ERR_FAIL_COND(file.is_null());
	file->store_string(JSON::stringify(metadata, "\t"));
	print_line("DDGI_CAPTURE " + prefix);
#endif
}

} // namespace RendererRD
