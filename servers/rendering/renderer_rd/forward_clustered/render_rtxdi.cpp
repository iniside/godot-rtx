/**************************************************************************/
/*  render_rtxdi.cpp                                                      */
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

#include "render_rtxdi.h"

#include "render_raytracing.h"

#ifdef DEBUG_ENABLED
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#endif

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_globals.h"

namespace RendererSceneRenderImplementation {

namespace {

struct alignas(16) RtxdiParametersBlock {
	RTXDI_Parameters restir = {};
	RTXDI_RuntimeParameters runtime = {};
	RTXDI_LightBufferParameters light_buffer = {};
	uint32_t extent_history[4] = {};
};

static_assert(sizeof(RtxdiParametersBlock) % 16 == 0);

void append_uniform(Vector<RD::Uniform> &r_uniforms, RD::UniformType p_type, uint32_t p_binding, RID p_rid) {
	RD::Uniform uniform;
	uniform.uniform_type = p_type;
	uniform.binding = p_binding;
	uniform.append_id(p_rid);
	r_uniforms.push_back(uniform);
}

} // namespace

void RenderRTXDI::initialize(RenderRaytracing *p_raytracing, bool p_radiance_uses_array, uint32_t p_roughness_layers) {
	raytracing = p_raytracing;
	radiance_uses_array = p_radiance_uses_array;

#ifdef DEBUG_ENABLED
	diagnostic_prefix = OS::get_singleton()->get_environment("GODOT_RTXDI_CAPTURE_PREFIX");
	if (!diagnostic_prefix.is_empty()) {
		String frame = OS::get_singleton()->get_environment("GODOT_RTXDI_CAPTURE_FRAME");
		if (!frame.is_empty()) {
			if (frame.is_valid_int() && frame.to_int() > 0 && frame.to_int() <= UINT32_MAX) {
				diagnostic_frame = uint32_t(frame.to_int());
			} else {
				ERR_PRINT("GODOT_RTXDI_CAPTURE_FRAME requires a positive 32-bit render-call number; capture disabled.");
				diagnostic_prefix = String();
			}
		}
	}
#endif
	Vector<ShaderRD::VariantDefine> modes;
	modes.push_back(ShaderRD::VariantDefine(0, "\n#define MODE_INITIAL 1\n", true));
	modes.push_back(ShaderRD::VariantDefine(0, "\n#define MODE_TEMPORAL 1\n", true));
	modes.push_back(ShaderRD::VariantDefine(0, "\n#define MODE_SPATIAL 1\n", true));
	modes.push_back(ShaderRD::VariantDefine(0, "\n#define MODE_SHADE 1\n", true));
	String defines = "\n#define MAX_ROUGHNESS_LOD " + itos(p_roughness_layers - 1) + ".0\n";
#ifdef REAL_T_IS_DOUBLE
	defines += "\n#define USE_DOUBLE_PRECISION\n";
#endif
	if (radiance_uses_array) {
		defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY\n";
	}
	shader.shader.initialize(modes, defines, Vector<RD::PipelineImmutableSampler>(), Vector<uint64_t>(), false, false);
	shader.version = shader.shader.version_create();
	for (uint32_t i = 0; i < PASS_MAX; i++) {
		shader.shader_rid[i] = shader.shader.version_get_shader(shader.version, i);
		shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(shader.shader_rid[i]);
	}
#ifdef DEBUG_ENABLED
	if (!diagnostic_prefix.is_empty()) {
		diagnostic_shader.initialize(modes, defines + "\n#define RTXDI_DIAGNOSTICS 1\n", Vector<RD::PipelineImmutableSampler>(), Vector<uint64_t>(), false, false);
		diagnostic_version = diagnostic_shader.version_create();
		bool diagnostic_valid = diagnostic_shader.version_is_valid(diagnostic_version);
		for (uint32_t i = 0; i < PASS_MAX && diagnostic_valid; i++) {
			shader.shader_rid[PASS_MAX + i] = diagnostic_shader.version_get_shader(diagnostic_version, i);
			shader.pipeline[PASS_MAX + i] = RD::get_singleton()->compute_pipeline_create(shader.shader_rid[PASS_MAX + i]);
			diagnostic_valid = shader.pipeline[PASS_MAX + i].is_valid();
		}
		if (!diagnostic_valid) {
			diagnostic_shader.version_free(diagnostic_version);
			diagnostic_version = RID();
			diagnostic_prefix = String();
			for (uint32_t i = 0; i < PASS_MAX; i++) {
				shader.shader_rid[PASS_MAX + i] = RID();
				shader.pipeline[PASS_MAX + i] = RID();
			}
			ERR_PRINT("Failed to compile RTXDI diagnostic shaders or create their pipelines; rendering without capture.");
		}
	}
#endif

	Vector<uint8_t> packed_offsets;
	packed_offsets.resize(config.neighbor_offset_count * 2);
	rtxdi::FillNeighborOffsetBuffer(packed_offsets.ptrw(), config.neighbor_offset_count);
	Vector<uint8_t> float_offsets;
	float_offsets.resize(config.neighbor_offset_count * sizeof(float) * 2);
	float *offsets = reinterpret_cast<float *>(float_offsets.ptrw());
	for (uint32_t i = 0; i < config.neighbor_offset_count * 2; i++) {
		offsets[i] = float(int8_t(packed_offsets[i])) / 127.0f;
	}
	neighbor_offsets_buffer = RD::get_singleton()->storage_buffer_create(float_offsets.size(), float_offsets);
	RD::get_singleton()->set_resource_name(neighbor_offsets_buffer, "RTXDI Neighbor Offsets");

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	surface_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	environment_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
}

bool RenderRTXDI::_ensure_viewport_resources(RTViewportState *p_state, const Size2i &p_size, uint32_t p_view_count) {
	ERR_FAIL_NULL_V(p_state, false);
	if (p_state->rtxdi_di && p_state->rtxdi_di->size == p_size && p_state->rtxdi_di->views.size() == p_view_count) {
		return true;
	}
	free_viewport_resources(p_state);

	RenderRTXDIViewportResources *resources = memnew(RenderRTXDIViewportResources);
	resources->size = p_size;
	resources->views.resize(p_view_count);
	p_state->rtxdi_di = resources;

	RD::TextureFormat texture_format;
	texture_format.texture_type = RD::TEXTURE_TYPE_2D;
	texture_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	texture_format.width = p_size.x;
	texture_format.height = p_size.y;
	texture_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;

	for (uint32_t view = 0; view < p_view_count; view++) {
		RenderRTXDIViewResources &view_resources = resources->views[view];
		rtxdi::ReSTIRDIStaticParameters static_parameters;
		static_parameters.NeighborOffsetCount = config.neighbor_offset_count;
		static_parameters.RenderWidth = p_size.x;
		static_parameters.RenderHeight = p_size.y;
		static_parameters.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off;
		view_resources.context = memnew(rtxdi::ReSTIRDIContext(static_parameters));
		view_resources.context->SetResamplingMode(rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);

		RTXDI_DIInitialSamplingParameters initial = view_resources.context->GetInitialSamplingParameters();
		initial.numLocalLightSamples = p_state->settings.rtxdi_local_light_samples;
		initial.numInfiniteLightSamples = config.infinite_light_samples;
		initial.numEnvironmentSamples = config.environment_samples;
		initial.numBrdfSamples = config.brdf_samples;
		initial.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
		initial.enableInitialVisibility = 1;
		view_resources.context->SetInitialSamplingParameters(initial);

		RTXDI_DITemporalResamplingParameters temporal = view_resources.context->GetTemporalResamplingParameters();
		temporal.maxHistoryLength = config.max_history_length;
		temporal.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
		temporal.enableVisibilityShortcut = 0;
		view_resources.context->SetTemporalResamplingParameters(temporal);

		RTXDI_DISpatialResamplingParameters spatial = view_resources.context->GetSpatialResamplingParameters();
		spatial.numSamples = config.spatial_samples;
		spatial.numDisocclusionBoostSamples = config.spatial_disocclusion_samples;
		spatial.samplingRadius = config.spatial_radius;
		spatial.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
		spatial.enableMaterialSimilarityTest = 1;
		spatial.discountNaiveSamples = 1;
		view_resources.context->SetSpatialResamplingParameters(spatial);

		RTXDI_ShadingParameters shading = view_resources.context->GetShadingParameters();
		shading.enableFinalVisibility = 1;
		shading.reuseFinalVisibility = 0;
		view_resources.context->SetShadingParameters(shading);

		const RTXDI_ReservoirBufferParameters reservoir_parameters = view_resources.context->GetReservoirBufferParameters();
		const uint64_t reservoir_size = uint64_t(reservoir_parameters.reservoirArrayPitch) * rtxdi::c_NumReSTIRDIReservoirBuffers * sizeof(RTXDI_PackedDIReservoir);
		if (reservoir_size > UINT32_MAX) {
			free_viewport_resources(p_state);
			ERR_FAIL_V_MSG(false, "RTXDI DI reservoir storage exceeds RenderingDevice buffer limits.");
		}
		view_resources.reservoir_buffer = RD::get_singleton()->storage_buffer_create(uint32_t(reservoir_size));
		view_resources.parameters_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(RtxdiParametersBlock));
		view_resources.diffuse_radiance_distance = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
		view_resources.specular_radiance_distance = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
		if (!view_resources.reservoir_buffer.is_valid() || !view_resources.parameters_buffer.is_valid() || !view_resources.diffuse_radiance_distance.is_valid() || !view_resources.specular_radiance_distance.is_valid()) {
			free_viewport_resources(p_state);
			ERR_FAIL_V_MSG(false, "Failed to allocate RTXDI DI viewport resources.");
		}
		RD::get_singleton()->set_resource_name(view_resources.reservoir_buffer, "RTXDI DI Reservoirs View " + itos(view));
		RD::get_singleton()->set_resource_name(view_resources.diffuse_radiance_distance, "RTXDI DI Diffuse Radiance Distance View " + itos(view));
		RD::get_singleton()->set_resource_name(view_resources.specular_radiance_distance, "RTXDI DI Specular Radiance Distance View " + itos(view));
	}
	return true;
}

RID RenderRTXDI::_create_uniform_set(const RenderRTXDISurfaceResources &p_surface, RTViewportState *p_state, RID p_scene_data_buffer, uint32_t p_view, uint32_t p_variant, RID p_diagnostic_buffer) {
	ERR_FAIL_NULL_V(p_state, RID());
	ERR_FAIL_NULL_V(p_state->rtxdi_di, RID());
	ERR_FAIL_UNSIGNED_INDEX_V(p_view, p_state->rtxdi_di->views.size(), RID());
	RenderRTXDIViewResources &view_resources = p_state->rtxdi_di->views[p_view];
	const RTLightSnapshot &current_lights = p_state->light_snapshots[p_state->current_light_snapshot];
	const RTLightSnapshot &previous_lights = p_state->light_snapshots[p_state->current_light_snapshot ^ 1u];
	const RID previous_light_buffer = previous_lights.light_buffer.is_valid() ? previous_lights.light_buffer : current_lights.light_buffer;
	const RID environment_texture = p_state->environment_texture.is_valid() ? p_state->environment_texture : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(radiance_uses_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RID bindless_linear_clamp = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID bindless_linear_mip_repeat = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);

	Vector<RD::Uniform> uniforms;
	append_uniform(uniforms, RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_scene_data_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, view_resources.parameters_buffer);
	for (uint32_t i = 0; i < 6; i++) {
		append_uniform(uniforms, RD::UNIFORM_TYPE_TEXTURE, 2 + i, p_surface.current[i]);
	}
	append_uniform(uniforms, RD::UNIFORM_TYPE_TEXTURE, 8, p_surface.current_depth);
	for (uint32_t i = 0; i < 6; i++) {
		append_uniform(uniforms, RD::UNIFORM_TYPE_TEXTURE, 9 + i, p_surface.previous[i]);
	}
	append_uniform(uniforms, RD::UNIFORM_TYPE_TEXTURE, 15, p_surface.previous_depth);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, current_lights.light_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, previous_light_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_UNIFORM_BUFFER, 18, current_lights.parameters_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, current_lights.current_to_previous_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, current_lights.previous_to_current_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, p_state->geometry_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, p_state->material_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, view_resources.reservoir_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, neighbor_offsets_buffer);
	append_uniform(uniforms, RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 25, p_state->tlas);
	append_uniform(uniforms, RD::UNIFORM_TYPE_TEXTURE, 26, environment_texture);
	append_uniform(uniforms, RD::UNIFORM_TYPE_SAMPLER, 27, surface_sampler);
	append_uniform(uniforms, RD::UNIFORM_TYPE_SAMPLER, 28, environment_sampler);
	append_uniform(uniforms, RD::UNIFORM_TYPE_SAMPLER, 29, bindless_linear_clamp);
	append_uniform(uniforms, RD::UNIFORM_TYPE_SAMPLER, 30, bindless_linear_mip_repeat);
	append_uniform(uniforms, RD::UNIFORM_TYPE_IMAGE, 31, view_resources.diffuse_radiance_distance);
	append_uniform(uniforms, RD::UNIFORM_TYPE_IMAGE, 32, view_resources.specular_radiance_distance);
	p_surface.samplers.append_uniforms(uniforms, 33);
	append_uniform(uniforms, RD::UNIFORM_TYPE_UNIFORM_BUFFER, 45, p_state->frame_constants_buffer);
	if (p_diagnostic_buffer.is_valid()) {
		append_uniform(uniforms, RD::UNIFORM_TYPE_STORAGE_BUFFER, 46, p_diagnostic_buffer);
	}
	return RD::get_singleton()->uniform_set_create(uniforms, shader.shader_rid[p_variant], 0, true);
}

void RenderRTXDI::render(const RenderRTXDISurfaceResources &p_surface, RTViewportState *p_state, RID p_scene_data_buffer, uint32_t p_view, uint32_t p_view_count) {
	ERR_FAIL_NULL(p_state);
	ERR_FAIL_COND(p_surface.size.x <= 0 || p_surface.size.y <= 0);
	ERR_FAIL_COND(!p_state->tlas.is_valid());
	ERR_FAIL_COND(!p_state->frame_constants_buffer.is_valid());
	ERR_FAIL_COND(p_view_count != 1 || p_view != 0);
	ERR_FAIL_COND(!_ensure_viewport_resources(p_state, p_surface.size, p_view_count));
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_state->rtxdi_di->views.size());

	RenderRTXDIViewResources &view_resources = p_state->rtxdi_di->views[p_view];
	RTXDI_DIInitialSamplingParameters initial = view_resources.context->GetInitialSamplingParameters();
	initial.numLocalLightSamples = p_state->settings.rtxdi_local_light_samples;
	view_resources.context->SetInitialSamplingParameters(initial);
	view_resources.context->SetFrameIndex(uint32_t(p_surface.frame_index));
	const RTLightSnapshot &light_snapshot = p_state->light_snapshots[p_state->current_light_snapshot];
	RtxdiParametersBlock parameters;
	parameters.restir.reservoirBufferParams = view_resources.context->GetReservoirBufferParameters();
	parameters.restir.bufferIndices = view_resources.context->GetBufferIndices();
	parameters.restir.initialSamplingParams = view_resources.context->GetInitialSamplingParameters();
	parameters.restir.temporalResamplingParams = view_resources.context->GetTemporalResamplingParameters();
	parameters.restir.boilingFilterParams = view_resources.context->GetBoilingFilterParameters();
	parameters.restir.spatialResamplingParams = view_resources.context->GetSpatialResamplingParameters();
	parameters.restir.spatioTemporalResamplingParams = view_resources.context->GetSpatioTemporalResamplingParameters();
	parameters.restir.shadingParams = view_resources.context->GetShadingParameters();
	parameters.runtime = view_resources.context->GetRuntimeParams();
	parameters.light_buffer.localLightBufferRegion.firstLightIndex = light_snapshot.parameters.local_first;
	parameters.light_buffer.localLightBufferRegion.numLights = light_snapshot.parameters.local_count;
	parameters.light_buffer.infiniteLightBufferRegion.firstLightIndex = light_snapshot.parameters.infinite_first;
	parameters.light_buffer.infiniteLightBufferRegion.numLights = light_snapshot.parameters.infinite_count;
	parameters.light_buffer.environmentLightParams.lightIndex = light_snapshot.parameters.environment_index;
	parameters.light_buffer.environmentLightParams.lightPresent = light_snapshot.parameters.environment_present;
	parameters.extent_history[0] = p_surface.size.x;
	parameters.extent_history[1] = p_surface.size.y;
	parameters.extent_history[2] = p_surface.history_valid && view_resources.camera_history_epoch == p_state->camera_history_epoch && view_resources.last_frame_index != UINT64_MAX && view_resources.last_frame_index + 1 == p_surface.frame_index ? 1u : 0u;
	parameters.extent_history[3] = p_surface.orthogonal ? 1u : 0u;
	RD::get_singleton()->buffer_update(view_resources.parameters_buffer, 0, sizeof(parameters), &parameters);

	RID bindless_uniform_set = raytracing->get_bindless_uniform_set(shader.shader_rid[PASS_INITIAL]);
	ERR_FAIL_COND(!bindless_uniform_set.is_valid());
	RID diagnostic_buffer;
	uint32_t variant_offset = 0;
#ifdef DEBUG_ENABLED
	if (!diagnostic_prefix.is_empty() && !diagnostic_complete && ++diagnostic_render_count == diagnostic_frame) {
		diagnostic_complete = true;
		Vector<uint8_t> zeros;
		zeros.resize(PASS_MAX * 1024 * 48 * sizeof(uint32_t));
		zeros.fill(0);
		diagnostic_buffer = RD::get_singleton()->storage_buffer_create(zeros.size(), zeros);
		if (diagnostic_buffer.is_valid()) {
			variant_offset = PASS_MAX;
		} else {
			ERR_PRINT("Failed to allocate RTXDI diagnostic capture buffer.");
		}
	}
#endif
	RID uniform_sets[PASS_MAX];
	for (uint32_t pass = 0; pass < PASS_MAX;) {
		if (shader.pipeline[pass + variant_offset].is_valid()) {
			uniform_sets[pass] = _create_uniform_set(p_surface, p_state, p_scene_data_buffer, p_view, pass + variant_offset, diagnostic_buffer);
		}
		if (!uniform_sets[pass].is_valid()) {
			for (uint32_t previous_pass = 0; previous_pass < pass; previous_pass++) {
				RD::get_singleton()->free_rid(uniform_sets[previous_pass]);
			}
			if (diagnostic_buffer.is_valid()) {
				RD::get_singleton()->free_rid(diagnostic_buffer);
				diagnostic_buffer = RID();
				variant_offset = 0;
				pass = 0;
				for (RID &uniform_set : uniform_sets) {
					uniform_set = RID();
				}
				ERR_PRINT("Failed to prepare RTXDI diagnostic dispatch resources; rendering without capture.");
				continue;
			}
			ERR_FAIL_MSG("Failed to prepare RTXDI DI dispatch resources.");
		}
		pass++;
	}

	RD *rd = RD::get_singleton();
	static const char *const pass_timestamps[PASS_MAX] = { "RTXDI Initial Sampling", "RTXDI Temporal Resampling", "RTXDI Spatial Resampling", "RTXDI Final Shading" };
	for (uint32_t pass = 0; pass < PASS_MAX; pass++) {
		RENDER_TIMESTAMP(pass_timestamps[pass]);
		RD::ComputeListID compute_list = rd->compute_list_begin();
		raytracing->register_compute_buffer_dependencies(compute_list);
		rd->compute_list_bind_compute_pipeline(compute_list, shader.pipeline[pass + variant_offset]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_sets[pass], 0);
		rd->compute_list_bind_uniform_set(compute_list, bindless_uniform_set, 1);
		rd->compute_list_dispatch_threads(compute_list, p_surface.size.x, p_surface.size.y, 1);
		rd->compute_list_end();
		rd->free_rid(uniform_sets[pass]);
	}
	RENDER_TIMESTAMP("RTXDI Dispatches Complete");
	view_resources.last_frame_index = p_surface.frame_index;
	view_resources.camera_history_epoch = p_state->camera_history_epoch;
#ifdef DEBUG_ENABLED
	if (diagnostic_buffer.is_valid()) {
		_capture_diagnostics(diagnostic_buffer, p_surface, p_state, parameters.extent_history[2] != 0);
		rd->free_rid(diagnostic_buffer);
	}
#endif
}

#ifdef DEBUG_ENABLED
void RenderRTXDI::_capture_diagnostics(RID p_buffer, const RenderRTXDISurfaceResources &p_surface, const RTViewportState *p_state, bool p_history_valid) {
	static const char *const counter_names[] = {
		"sampled_pixels",
		"valid_surfaces",
		"light_sample_calls_omni",
		"light_sample_calls_directional",
		"light_sample_calls_spot",
		"light_sample_calls_area",
		"light_sample_calls_emissive_triangle",
		"light_sample_calls_environment",
		"initial_candidate_sample_calls_omni",
		"initial_candidate_sample_calls_directional",
		"initial_candidate_sample_calls_spot",
		"initial_candidate_sample_calls_area",
		"initial_candidate_sample_calls_emissive_triangle",
		"initial_candidate_sample_calls_environment",
		"initial_selected_sample_reconstructions",
		"target_pdf_calls",
		"positive_target_pdf_results",
		"target_pdf_brdf_evaluations",
		"brdf_pdf_evaluations",
		"brdf_sample_calls",
		"local_source_pdf_calls",
		"environment_source_pdf_calls",
		"conservative_visibility_calls",
		"temporal_visibility_calls",
		"shadow_rays",
		"no_shadow_flag_bypasses",
		"shadow_triangle_candidates",
		"shadow_geometry_load_successes",
		"shadow_coverage_calls",
		"shadow_material_tests",
		"shadow_committed_hits",
		"shadow_visible_results",
		"emitter_coverage_calls",
		"emitter_material_tests",
		"local_ray_calls",
		"local_ray_triangle_candidates",
		"local_ray_geometry_load_successes",
		"local_ray_coverage_calls",
		"local_ray_material_tests",
		"local_ray_geometry_hits",
		"local_ray_geometry_misses",
		"local_light_scan_iterations",
		"local_light_scan_matches",
		"local_light_scan_no_match",
		"valid_output_reservoirs",
		"final_shading_brdf_evaluations",
		"gbuffer_load_calls",
		"light_sample_calls_unknown_type",
	};
	static_assert(sizeof(counter_names) / sizeof(counter_names[0]) == 48);
	Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(p_buffer);
	ERR_FAIL_COND_MSG(data.size() != PASS_MAX * 1024 * 48 * int(sizeof(uint32_t)), "RTXDI diagnostic readback has an unexpected size.");
	const uint32_t *records = reinterpret_cast<const uint32_t *>(data.ptr());
	const RTLightSnapshot &lights = p_state->light_snapshots[p_state->current_light_snapshot];
	Dictionary metadata;
	metadata["schema_version"] = 1;
	metadata["render_call"] = diagnostic_render_count;
	metadata["surface_frame_index"] = p_surface.frame_index;
	metadata["shader_frame_index"] = uint32_t(p_surface.frame_index);
	metadata["width"] = p_surface.size.x;
	metadata["height"] = p_surface.size.y;
	metadata["viewport_reservoir_id"] = p_state->rtxdi_di->views[0].reservoir_buffer.get_id();
	metadata["history_valid"] = p_history_valid;
	const uint64_t pixel_count = uint64_t(p_surface.size.x) * uint64_t(p_surface.size.y);
	const uint64_t stride = (pixel_count + 1023) / 1024;
	metadata["sample_limit_per_pass"] = 1024;
	metadata["pixel_count"] = pixel_count;
	metadata["sample_linear_stride"] = stride;
	metadata["sample_linear_offset"] = uint32_t(p_surface.frame_index) % stride;
	metadata["sampling_rule"] = "linear_pixel_index % stride == offset; identical pixels in all four passes; raw counts only, no full-frame extrapolation";
	metadata["measurement"] = "Executed shader events for sampled invocations, not GPU time. Diagnostic variants and the synchronous readback perturb the capture frame; use separate normal-frame native GPU timings.";
	metadata["initial_candidate_sample_calls"] = "RAB_SamplePolymorphicLight calls during SDK initial proposals and the explicit environment proposal loop; excludes selected-reservoir reconstruction";
	metadata["coverage_calls"] = "Entry to geometry_alpha_covered; material_tests count only calls reaching geometry_material_covered after triangle decode";
	metadata["local_light_scan_iterations"] = "One current_lights load per iteration after a BRDF ray geometry hit; stops on a matching emissive triangle";
	metadata["local_light_count"] = lights.parameters.local_count;
	metadata["infinite_light_count"] = lights.parameters.infinite_count;
	metadata["environment_light_present"] = lights.parameters.environment_present;
	metadata["configured_local_samples"] = p_state->rtxdi_di->views[0].context->GetInitialSamplingParameters().numLocalLightSamples;
	metadata["configured_infinite_samples"] = config.infinite_light_samples;
	metadata["configured_environment_samples"] = config.environment_samples;
	metadata["configured_brdf_samples"] = config.brdf_samples;
	static const char *const pass_names[PASS_MAX] = { "initial", "temporal", "spatial", "shade" };
	Dictionary passes;
	for (uint32_t pass = 0; pass < PASS_MAX; pass++) {
		Dictionary counters;
		for (uint32_t counter = 0; counter < 48; counter++) {
			uint64_t total = 0;
			for (uint32_t sample = 0; sample < 1024; sample++) {
				total += records[(pass * 1024 + sample) * 48 + counter];
			}
			counters[counter_names[counter]] = total;
		}
		passes[pass_names[pass]] = counters;
	}
	metadata["passes"] = passes;
	String path = diagnostic_prefix + "-frame-" + uitos(p_surface.frame_index) + ".json";
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	ERR_FAIL_COND_MSG(file.is_null(), "Failed to open RTXDI diagnostic capture: " + path);
	file->store_string(JSON::stringify(metadata, "\t"));
	file->flush();
	ERR_FAIL_COND_MSG(file->get_error() != OK, "Failed to write RTXDI diagnostic capture: " + path);
	print_line("RTXDI_CAPTURE " + path);
}
#endif

RID RenderRTXDI::get_diffuse_radiance_distance(const RTViewportState *p_state, uint32_t p_view) const {
	ERR_FAIL_NULL_V(p_state, RID());
	ERR_FAIL_NULL_V(p_state->rtxdi_di, RID());
	ERR_FAIL_UNSIGNED_INDEX_V(p_view, p_state->rtxdi_di->views.size(), RID());
	return p_state->rtxdi_di->views[p_view].diffuse_radiance_distance;
}

RID RenderRTXDI::get_specular_radiance_distance(const RTViewportState *p_state, uint32_t p_view) const {
	ERR_FAIL_NULL_V(p_state, RID());
	ERR_FAIL_NULL_V(p_state->rtxdi_di, RID());
	ERR_FAIL_UNSIGNED_INDEX_V(p_view, p_state->rtxdi_di->views.size(), RID());
	return p_state->rtxdi_di->views[p_view].specular_radiance_distance;
}

void RenderRTXDI::free_viewport_resources(RTViewportState *p_state) {
	if (!p_state || !p_state->rtxdi_di) {
		return;
	}
	RD *rd = RD::get_singleton();
	for (RenderRTXDIViewResources &view_resources : p_state->rtxdi_di->views) {
		if (view_resources.reservoir_buffer.is_valid()) {
			rd->free_rid(view_resources.reservoir_buffer);
		}
		if (view_resources.parameters_buffer.is_valid()) {
			rd->free_rid(view_resources.parameters_buffer);
		}
		if (view_resources.diffuse_radiance_distance.is_valid()) {
			rd->free_rid(view_resources.diffuse_radiance_distance);
		}
		if (view_resources.specular_radiance_distance.is_valid()) {
			rd->free_rid(view_resources.specular_radiance_distance);
		}
		if (view_resources.context) {
			memdelete(view_resources.context);
		}
	}
	memdelete(p_state->rtxdi_di);
	p_state->rtxdi_di = nullptr;
}

RenderRTXDI::~RenderRTXDI() {
#ifdef DEBUG_ENABLED
	if (diagnostic_version.is_valid()) {
		diagnostic_shader.version_free(diagnostic_version);
	}
#endif
	if (neighbor_offsets_buffer.is_valid()) {
		RD::get_singleton()->free_rid(neighbor_offsets_buffer);
	}
	if (shader.version.is_valid()) {
		shader.shader.version_free(shader.version);
	}
}

} // namespace RendererSceneRenderImplementation
