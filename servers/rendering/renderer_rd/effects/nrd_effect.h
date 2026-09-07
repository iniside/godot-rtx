/**************************************************************************/
/*  nrd_effect.h                                                          */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtxdi_frame.glsl.gen.h"

namespace nrd {
struct Instance;
}

namespace RendererRD {

class NRDEffect {
public:
	struct Context {
		nrd::Instance *instance = nullptr;
		Size2i size;
		LocalVector<RID> shaders;
		LocalVector<RID> pipelines;
		LocalVector<RID> permanent_pool;
		LocalVector<RID> transient_pool;
		LocalVector<RID> constants;
		RID normal_roughness;
		RID view_depth;
		RID diffuse;
		RID specular;
		uint64_t last_frame = UINT64_MAX;
		~Context();
	};

	struct Frame {
		RID surface[6];
		RID depth;
		RID noisy_diffuse;
		RID noisy_specular;
		RID scene_data;
		RID color;
		RID separate_specular;
		RID velocity;
		RID normal_roughness;
		RID fog;
		RID radiance;
		RID directional_lights;
		Projection projection;
		Projection previous_projection;
		Transform3D camera;
		Transform3D previous_camera;
		Vector2 jitter;
		Vector2 previous_jitter;
		uint64_t frame_index = 0;
		bool history_valid = false;
		bool orthogonal = false;
		bool fog_enabled = false;
		bool fog_legacy_blending = false;
		float fog_inverse_length = 1.0f;
		float fog_spread = 1.0f;
		float time_step = 0.0f;
		float environment_energy = 0.0f;
	};

private:
	RtxdiFrameShaderRD frame_shader;
	RID shader_version;
	RID frame_pipelines[2];
	RID samplers[2];

	bool _process_frame(Context *p_context, const Frame &p_frame, bool p_compose);
	static RID _create_texture(const Size2i &p_size, RD::DataFormat p_format);

public:
	Context *create_context(const Size2i &p_size);
	bool prepare(Context *p_context, const Frame &p_frame);
	bool process(Context *p_context, const Frame &p_frame);
	NRDEffect(bool p_radiance_array, uint32_t p_roughness_layers);
	~NRDEffect();
};

} // namespace RendererRD
