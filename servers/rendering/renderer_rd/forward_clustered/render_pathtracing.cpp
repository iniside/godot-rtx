/**************************************************************************/
/*  render_pathtracing.cpp                                                 */
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

#include "render_pathtracing.h"

#include "render_raytracing.h"

#ifdef DEBUG_ENABLED
#include "core/os/os.h"
#include "core/string/print_string.h"
#endif

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererSceneRenderImplementation {

RenderPathtracing::Context::~Context() {
	RD *rd = RD::get_singleton();
	if (rd->raytracing_pipeline_is_valid(pipeline)) {
		rd->free_rid(sbt);
		rd->free_rid(pipeline);
	}
	for (RID image : images) {
		if (image.is_valid()) {
			rd->free_rid(image);
		}
	}
	if (frame_buffer.is_valid()) {
		rd->free_rid(frame_buffer);
	}
}

RenderPathtracing::RenderPathtracing() {
	String defines;
#ifdef REAL_T_IS_DOUBLE
	defines += "#define USE_DOUBLE_PRECISION\n";
#endif
	shader.initialize(Vector<String>{ "\n", "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" }, defines);
	version = shader.version_create();
}

RenderPathtracing::~RenderPathtracing() {
	shader.version_free(version);
}

RenderPathtracing::Context *RenderPathtracing::_create_context(const Size2i &p_size) {
	Context *context = memnew(Context);
	context->size = p_size;
	RD *rd = RD::get_singleton();
	const RD::DataFormat formats[11] = {
		RD::DATA_FORMAT_R32G32B32A32_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R32_UINT,
		RD::DATA_FORMAT_R32G32_UINT,
		RD::DATA_FORMAT_R32_SFLOAT,
	};
	bool valid = true;
	for (uint32_t i = 0; i < 11; i++) {
		RD::TextureFormat format;
		format.width = p_size.x;
		format.height = p_size.y;
		format.format = formats[i];
		format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		context->images[i] = rd->texture_create(format, RD::TextureView());
		valid &= context->images[i].is_valid();
	}
	context->frame_buffer = rd->uniform_buffer_create(64);
	valid &= context->frame_buffer.is_valid();
	if (!valid) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "Failed to allocate path-tracing camera resources.");
	}
	return context;
}

bool RenderPathtracing::render(RenderRaytracing &p_raytracing, RTViewportState &p_state, RID p_scene_data, RID p_sky, const Size2i &p_size, bool p_sky_array, bool p_draw_sky, const Color &p_background, bool p_micro_geometry_debug) {
	RD *rd = RD::get_singleton();
	if (p_state.pathtracing && p_state.pathtracing->size != p_size) {
		memdelete(p_state.pathtracing);
		p_state.pathtracing = nullptr;
	}
	if (!p_state.pathtracing) {
		p_state.pathtracing = _create_context(p_size);
	}
	ERR_FAIL_NULL_V(p_state.pathtracing, false);
	Context &context = *p_state.pathtracing;
	RID shader_rid = shader.version_get_shader(version, p_sky_array ? 1 : 0);
	ERR_FAIL_COND_V(shader_rid.is_null(), false);
	if (context.material_source_pipeline != p_state.material_pipeline || !rd->raytracing_pipeline_is_valid(context.pipeline)) {
		if (rd->raytracing_pipeline_is_valid(context.pipeline)) {
			rd->free_rid(context.sbt);
			rd->free_rid(context.pipeline);
		}
		context.pipeline = RID();
		context.sbt = RID();
		RD::PipelineShader entry;
		entry.shader = shader_rid;
		ERR_FAIL_COND_V(!p_raytracing.create_material_pipeline(&p_state, { &entry, 1 }, { &entry, 1 }, 1, context.pipeline, context.sbt), false);
		context.material_source_pipeline = p_state.material_pipeline;
		context.history_epoch = UINT64_MAX;
		context.accumulated_samples = 0;
	}
	const bool raw = !p_micro_geometry_debug && p_state.settings.raytracing_denoiser == RSE::RAYTRACING_DENOISER_NONE;
	const bool accumulate = raw && p_state.settings.pathtracing_accumulate;
	uint32_t samples = raw ? uint32_t(p_state.settings.pathtracing_samples_per_pixel) : 1;
	const uint64_t history_epoch = raw ? p_state.pathtracing_history_epoch : p_state.camera_history_epoch;
	context.history_valid = context.history_epoch == history_epoch && context.micro_geometry_debug == p_micro_geometry_debug;
	context.micro_geometry_debug = p_micro_geometry_debug;
	if (!accumulate || !context.history_valid || context.accumulated_samples > UINT32_MAX - samples) {
		context.accumulated_samples = 0;
	}
	struct Frame {
		uint32_t extent_frame[4];
		uint32_t settings[4];
		float options[4];
		float background[4];
	} frame = {
		{ uint32_t(p_size.x), uint32_t(p_size.y), context.frame, context.accumulated_samples },
		{ samples, p_micro_geometry_debug ? 0u : uint32_t(p_state.settings.pathtracing_max_bounces), uint32_t(accumulate), p_state.settings_visible_layers },
		{ float(raw), 3.402823466e+38f, float(p_state.camera_orthogonal), float(p_draw_sky) },
		{ p_background.r, p_background.g, p_background.b, p_background.a },
	};
	static_assert(sizeof(Frame) == 64);
	rd->buffer_update(context.frame_buffer, 0, sizeof(frame), &frame);
	RID material_set = p_raytracing.create_material_uniform_set(&p_state, p_scene_data, shader_rid);
	ERR_FAIL_COND_V(material_set.is_null(), false);
	RID bindless_set = p_raytracing.get_bindless_uniform_set(shader_rid);
	const RTLightSnapshot &lights = p_state.light_snapshots[p_state.current_light_snapshot];
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, { context.frame_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { lights.light_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, { lights.parameters_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, { p_sky }));
	for (uint32_t i = 0; i < 11; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4 + i, { context.images[i] }));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, { p_state.motion_index_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, { p_state.motion_transform_buffer }));
	RID output_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 2, uniforms);
	if (bindless_set.is_null() || output_set.is_null()) {
		rd->free_rid(material_set);
		return false;
	}
	RD::RaytracingListID list = rd->raytracing_list_begin();
	rd->raytracing_list_bind_raytracing_pipeline(list, context.pipeline);
	rd->raytracing_list_bind_uniform_set(list, material_set, 0);
	rd->raytracing_list_bind_uniform_set(list, bindless_set, 1);
	rd->raytracing_list_bind_uniform_set(list, output_set, 2);
	p_raytracing.register_raytracing_buffer_dependencies(list);
	rd->raytracing_list_trace_rays(list, 0, context.sbt, p_size.x, p_size.y, 1);
	rd->raytracing_list_end();
	rd->free_rid(material_set);
	context.accumulated_samples += samples;
#ifdef DEBUG_ENABLED
	if ((context.frame == 0 || context.frame == 499) && OS::get_singleton()->get_environment("GODOT_PT_TRACE_STATS") == "1") {
		print_line(vformat("[PT] frame=%d samples=%d history_epoch=%d scene_generation=%d scene_signature=%d history_valid=%s", context.frame + 1, context.accumulated_samples, p_state.pathtracing_history_epoch, p_state.scene_generation, p_state.scene_signature, context.history_valid ? "true" : "false"));
	}
#endif
	context.history_epoch = history_epoch;
	context.frame++;
	return true;
}

} // namespace RendererSceneRenderImplementation
