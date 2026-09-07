/**************************************************************************/
/*  render_rtxdi.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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
/* included in all copies or substantial portions of the Software.       */
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

#include "core/math/projection.h"
#include "core/math/vector2.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/rtxdi_di.slang.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"

#include <Rtxdi/DI/ReSTIRDI.h>

namespace RendererSceneRenderImplementation {

class RenderRaytracing;
struct RTViewportState;

struct RenderRTXDISurfaceResources {
	RendererRD::MaterialStorage::Samplers samplers;
	RID current[6];
	RID previous[6];
	RID current_depth;
	RID previous_depth;
	Size2i size;
	bool history_valid = false;
	bool orthogonal = false;
	uint64_t frame_index = 0;
};

struct RenderRTXDIViewResources {
	rtxdi::ReSTIRDIContext *context = nullptr;
	RID reservoir_buffer;
	RID parameters_buffer;
	RID diffuse_radiance_distance;
	RID specular_radiance_distance;
	uint64_t last_frame_index = UINT64_MAX;
};

struct RenderRTXDIViewportResources {
	Size2i size;
	LocalVector<RenderRTXDIViewResources> views;
};

class RenderRTXDI {
	enum Pass {
		PASS_INITIAL,
		PASS_TEMPORAL,
		PASS_SPATIAL,
		PASS_SHADE,
		PASS_MAX,
	};

	struct Config {
		uint32_t neighbor_offset_count = 8192;
		uint32_t local_light_samples = 8;
		uint32_t infinite_light_samples = 1;
		uint32_t environment_samples = 1;
		uint32_t brdf_samples = 0;
		uint32_t max_history_length = 20;
		uint32_t spatial_samples = 1;
		uint32_t spatial_disocclusion_samples = 8;
		float spatial_radius = 32.0f;
	};

	struct ShaderData {
		RtxdiDiShaderRD shader;
		RID version;
		RID shader_rid[PASS_MAX];
		RID pipeline[PASS_MAX];
	} shader;

	RenderRaytracing *raytracing = nullptr;
	Config config;
	bool radiance_uses_array = false;
	RID neighbor_offsets_buffer;
	RID surface_sampler;
	RID environment_sampler;

	bool _ensure_viewport_resources(RTViewportState *p_state, const Size2i &p_size, uint32_t p_view_count);
	RID _create_uniform_set(const RenderRTXDISurfaceResources &p_surface, RTViewportState *p_state, RID p_scene_data_buffer, uint32_t p_view, Pass p_pass);

public:
	void initialize(RenderRaytracing *p_raytracing, bool p_radiance_uses_array, uint32_t p_roughness_layers);
	void render(const RenderRTXDISurfaceResources &p_surface, RTViewportState *p_state, RID p_scene_data_buffer, uint32_t p_view, uint32_t p_view_count);

	RID get_diffuse_radiance_distance(const RTViewportState *p_state, uint32_t p_view) const;
	RID get_specular_radiance_distance(const RTViewportState *p_state, uint32_t p_view) const;

	static void free_viewport_resources(RTViewportState *p_state);

	~RenderRTXDI();
};

} // namespace RendererSceneRenderImplementation
