/**************************************************************************/
/*  render_pathtracing.h                                                   */
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

#include "servers/rendering/renderer_rd/shaders/raytracing/pathtracing.slang.gen.h"

namespace RendererSceneRenderImplementation {

class RenderRaytracing;
struct RTViewportState;

class RenderPathtracing {
public:
	struct Context {
		Size2i size;
		RID images[11];
		RID frame_buffer;
		RID pipeline;
		RID sbt;
		RID material_source_pipeline;
		uint64_t history_epoch = UINT64_MAX;
		uint32_t accumulated_samples = 0;
		uint32_t frame = 0;
		bool history_valid = false;
		bool micro_geometry_debug = false;
		~Context();
		RID get_radiance() const { return images[1]; }
		RID get_diffuse() const { return images[2]; }
		RID get_specular() const { return images[3]; }
		RID get_surface(uint32_t p_index) const { return images[4 + p_index]; }
		RID get_depth() const { return images[10]; }
	};

	RenderPathtracing();
	~RenderPathtracing();
	bool render(RenderRaytracing &p_raytracing, RTViewportState &p_state, RID p_scene_data, RID p_sky, const Size2i &p_size, bool p_sky_array, bool p_draw_sky, const Color &p_background, bool p_micro_geometry_debug = false);

private:
	PathtracingShaderRD shader;
	RID version;
	Context *_create_context(const Size2i &p_size);
};

} // namespace RendererSceneRenderImplementation
