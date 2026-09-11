/**************************************************************************/
/*  render_forward_clustered.cpp                                          */
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

#include "render_forward_clustered.h"

#include "render_rtxdi.h"

#include "core/config/project_settings.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/storage/ltc_lut.gen.h"

using namespace RendererSceneRenderImplementation;

#define PRELOAD_PIPELINES_ON_SURFACE_CACHE_CONSTRUCTION 1

#define FADE_ALPHA_PASS_THRESHOLD 0.999

static const RD::DataFormat rtxdi_surface_formats[RenderForwardClustered::RenderBufferDataForwardClustered::RTXDI_SURFACE_ATTACHMENT_COUNT] = {
	RD::DATA_FORMAT_R8G8B8A8_UNORM,
	RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
	RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
	RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
	RD::DATA_FORMAT_R32_UINT,
	RD::DATA_FORMAT_R32G32_UINT,
};

StringName RenderForwardClustered::RenderBufferDataForwardClustered::_get_rtxdi_surface_texture_name(uint32_t p_set, uint32_t p_attachment) const {
	static const StringName names[2][6] = {
		{ RB_TEX_RTXDI_BASE_0, RB_TEX_RTXDI_SHADING_0, RB_TEX_RTXDI_EMISSION_0, RB_TEX_RTXDI_MOTION_0, RB_TEX_RTXDI_GEOMETRY_0, RB_TEX_RTXDI_CLASSIFICATION_0 },
		{ RB_TEX_RTXDI_BASE_1, RB_TEX_RTXDI_SHADING_1, RB_TEX_RTXDI_EMISSION_1, RB_TEX_RTXDI_MOTION_1, RB_TEX_RTXDI_GEOMETRY_1, RB_TEX_RTXDI_CLASSIFICATION_1 },
	};
	ERR_FAIL_UNSIGNED_INDEX_V(p_set, 2, StringName());
	ERR_FAIL_UNSIGNED_INDEX_V(p_attachment, 6, StringName());
	return names[p_set][p_attachment];
}

bool RenderForwardClustered::RenderBufferDataForwardClustered::_ensure_rtxdi_surface() {
	ERR_FAIL_NULL_V(render_buffers, false);
	if (render_buffers->has_texture(RB_SCOPE_RTXDI_SURFACE, RB_TEX_RTXDI_BASE_0)) {
		return true;
	}

	const uint32_t usage = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	for (RD::DataFormat format : rtxdi_surface_formats) {
		ERR_FAIL_COND_V_MSG(!RD::get_singleton()->texture_is_format_supported_for_usage(format, usage), false, "The RTXDI renderer requires surface formats supporting color attachment, sampling and storage usage.");
	}
	RD::TextureFormat depth_format = render_buffers->get_texture_format(RB_SCOPE_BUFFERS, RB_TEX_DEPTH);
	depth_format.usage_bits |= RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	ERR_FAIL_COND_V_MSG(!RD::get_singleton()->texture_is_format_supported_for_usage(depth_format.format, depth_format.usage_bits), false, "The RTXDI renderer requires sampleable depth history with transfer-destination support.");
	bool valid = true;
	for (uint32_t set = 0; set < 2; set++) {
		for (uint32_t attachment = 0; attachment < 6; attachment++) {
			valid &= render_buffers->create_texture(RB_SCOPE_RTXDI_SURFACE, _get_rtxdi_surface_texture_name(set, attachment), rtxdi_surface_formats[attachment], usage).is_valid();
		}
		valid &= render_buffers->create_texture_from_format(RB_SCOPE_RTXDI_SURFACE, set == 0 ? RB_TEX_RTXDI_DEPTH_0 : RB_TEX_RTXDI_DEPTH_1, depth_format).is_valid();
	}
	if (!valid) {
		render_buffers->clear_context(RB_SCOPE_RTXDI_SURFACE);
		ERR_FAIL_V_MSG(false, "Failed to allocate canonical primary surface resources.");
	}
	return true;
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_primary_surface_trace_depth() {
	ERR_FAIL_NULL_V(render_buffers, RID());
	if (!render_buffers->has_texture(RB_SCOPE_RTXDI_SURFACE, RB_TEX_RTXDI_TRACE_DEPTH)) {
		const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		ERR_FAIL_COND_V_MSG(!RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R32_SFLOAT, usage), RID(), "Primary ray depth requires a sampleable R32F storage image.");
		return render_buffers->create_texture(RB_SCOPE_RTXDI_SURFACE, RB_TEX_RTXDI_TRACE_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage);
	}
	return render_buffers->get_texture(RB_SCOPE_RTXDI_SURFACE, RB_TEX_RTXDI_TRACE_DEPTH);
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::prepare_rtxdi_surface(const RenderSceneDataRD *p_scene_data, bool p_invalid_deformation, bool p_invalid_micro_geometry_history) {
	micro_geometry_history_valid = false;
	ERR_FAIL_NULL_V(render_buffers, RID());
	ERR_FAIL_NULL_V(p_scene_data, RID());
	ERR_FAIL_COND_V(!_ensure_rtxdi_surface(), RID());

	const uint64_t engine_frame = RSG::rasterizer->get_frame_number();
	const Size2i surface_size = render_buffers->get_internal_size();
	bool history_continuous = rtxdi_surface_initialized && rtxdi_surface_depth_valid[rtxdi_surface_set] && rtxdi_surface_size == surface_size && engine_frame == rtxdi_surface_last_engine_frame + 1;
	if (history_continuous) {
		history_continuous = p_scene_data->camera.is_valid() && p_scene_data->camera == p_scene_data->prev_camera && rtxdi_surface_camera == p_scene_data->prev_camera && rtxdi_surface_camera_transform.is_equal_approx(p_scene_data->prev_cam_transform) && rtxdi_surface_camera_projection.is_same(p_scene_data->prev_cam_projection) && rtxdi_surface_camera_jitter.is_equal_approx(p_scene_data->prev_taa_jitter) && rtxdi_surface_camera_orthogonal == p_scene_data->prev_cam_orthogonal && p_scene_data->cam_orthogonal == p_scene_data->prev_cam_orthogonal && p_scene_data->cam_projection.is_same(p_scene_data->prev_cam_projection);
	}
	rtxdi_surface_history_valid = history_continuous && !p_invalid_deformation;
	micro_geometry_history_valid = history_continuous && !p_invalid_micro_geometry_history;
	if (RSG::utilities->capturing_timestamps && (rtxdi_surface_frame_index < 4 || engine_frame % 120 == 0)) {
		String rejected;
		auto reject = [&](bool p_rejected, const char *p_reason) {
			if (p_rejected) {
				if (!rejected.is_empty()) {
					rejected += ",";
				}
				rejected += p_reason;
			}
		};
		reject(!rtxdi_surface_initialized, "uninitialized");
		reject(!rtxdi_surface_depth_valid[rtxdi_surface_set], "depth_invalid");
		reject(rtxdi_surface_size != surface_size, "size_changed");
		reject(engine_frame != rtxdi_surface_last_engine_frame + 1, "frame_gap");
		reject(p_invalid_deformation, "deformation");
		reject(!p_scene_data->camera.is_valid(), "camera_missing");
		reject(p_scene_data->camera != p_scene_data->prev_camera, "camera_changed");
		reject(rtxdi_surface_camera != p_scene_data->prev_camera, "previous_camera_mismatch");
		reject(!rtxdi_surface_camera_transform.is_equal_approx(p_scene_data->prev_cam_transform), "previous_transform_mismatch");
		reject(!rtxdi_surface_camera_projection.is_same(p_scene_data->prev_cam_projection), "previous_projection_mismatch");
		reject(!rtxdi_surface_camera_jitter.is_equal_approx(p_scene_data->prev_taa_jitter), "previous_jitter_mismatch");
		reject(rtxdi_surface_camera_orthogonal != p_scene_data->prev_cam_orthogonal, "previous_orthogonal_mismatch");
		reject(p_scene_data->cam_orthogonal != p_scene_data->prev_cam_orthogonal, "orthogonal_changed");
		reject(!p_scene_data->cam_projection.is_same(p_scene_data->prev_cam_projection), "projection_changed");
		print_line(vformat("Microgeometry history: frame=%d last_rendered_frame=%d surface_frame=%d camera=%d previous_camera=%d stored_camera=%d size=%s previous_size=%s valid=%s hzb_valid=%s hzb_geometry_invalid=%s rejected=%s", engine_frame, rtxdi_surface_last_engine_frame, rtxdi_surface_frame_index, p_scene_data->camera.get_id(), p_scene_data->prev_camera.get_id(), rtxdi_surface_camera.get_id(), surface_size, rtxdi_surface_size, rtxdi_surface_history_valid, micro_geometry_history_valid, p_invalid_micro_geometry_history, rejected.is_empty() ? String("none") : rejected));
	}
	rtxdi_surface_set ^= 1;
	rtxdi_surface_depth_valid[rtxdi_surface_set] = false;
	rtxdi_surface_frame_index++;
	rtxdi_surface_last_engine_frame = engine_frame;
	rtxdi_surface_size = surface_size;
	rtxdi_surface_previous_camera_transform = p_scene_data->prev_cam_transform;
	rtxdi_surface_previous_camera_projection = p_scene_data->prev_cam_projection;
	rtxdi_surface_previous_camera_jitter = p_scene_data->prev_taa_jitter;
	rtxdi_surface_previous_camera_orthogonal = p_scene_data->prev_cam_orthogonal;
	rtxdi_surface_camera = p_scene_data->camera;
	rtxdi_surface_camera_transform = p_scene_data->cam_transform;
	rtxdi_surface_camera_projection = p_scene_data->cam_projection;
	rtxdi_surface_camera_jitter = p_scene_data->taa_jitter;
	rtxdi_surface_camera_orthogonal = p_scene_data->cam_orthogonal;
	rtxdi_surface_initialized = true;

	return FramebufferCacheRD::get_singleton()->get_cache(
			get_rtxdi_surface_texture(0), get_rtxdi_surface_texture(1), get_rtxdi_surface_texture(2),
			get_rtxdi_surface_texture(3), get_rtxdi_surface_texture(4), get_rtxdi_surface_texture(5),
			render_buffers->get_depth_texture());
}

void RenderForwardClustered::RenderBufferDataForwardClustered::commit_rtxdi_surface() {
	ERR_FAIL_NULL(render_buffers);
	const Size2i size = render_buffers->get_internal_size();
	for (uint32_t view = 0; view < render_buffers->get_view_count(); view++) {
		RD::get_singleton()->texture_copy(render_buffers->get_depth_texture(), get_rtxdi_surface_depth(), Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(size.x, size.y, 1), 0, 0, view, view);
	}
	rtxdi_surface_depth_valid[rtxdi_surface_set] = true;
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_rtxdi_surface_texture(uint32_t p_attachment, bool p_previous) const {
	ERR_FAIL_NULL_V(render_buffers, RID());
	const uint32_t set = p_previous ? (rtxdi_surface_set ^ 1) : rtxdi_surface_set;
	return render_buffers->get_texture(RB_SCOPE_RTXDI_SURFACE, _get_rtxdi_surface_texture_name(set, p_attachment));
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_rtxdi_surface_depth(bool p_previous) const {
	ERR_FAIL_NULL_V(render_buffers, RID());
	const uint32_t set = p_previous ? (rtxdi_surface_set ^ 1) : rtxdi_surface_set;
	return render_buffers->get_texture(RB_SCOPE_RTXDI_SURFACE, set == 0 ? RB_TEX_RTXDI_DEPTH_0 : RB_TEX_RTXDI_DEPTH_1);
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_specular() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR, get_specular_format(), get_specular_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR_MSAA, get_specular_format(), get_specular_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_normal_roughness_texture() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS, get_normal_roughness_format(), get_normal_roughness_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS_MSAA, get_normal_roughness_format(), get_normal_roughness_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_voxelgi() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI, get_voxelgi_format(), get_voxelgi_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI_MSAA, get_voxelgi_format(), get_voxelgi_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_fsr2(RendererRD::FSR2Effect *effect) {
	if (fsr2_context == nullptr) {
		fsr2_context = effect->create_context(render_buffers->get_internal_size(), render_buffers->get_target_size());
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_dlss(RendererRD::DLSSEffect *effect, bool p_ray_reconstruction) {
	if (dlss_context == nullptr) {
		dlss_context = effect->create_context(render_buffers->get_internal_size(), render_buffers->get_target_size(), p_ray_reconstruction);
	}
}

#ifdef METAL_MFXTEMPORAL_ENABLED
bool RenderForwardClustered::RenderBufferDataForwardClustered::ensure_mfx_temporal(RendererRD::MFXTemporalEffect *p_effect) {
	if (mfx_temporal_context == nullptr) {
		RendererRD::MFXTemporalEffect::CreateParams params;
		params.input_size = render_buffers->get_internal_size();
		params.output_size = render_buffers->get_target_size();
		params.input_format = render_buffers->get_base_data_format();
		params.depth_format = render_buffers->get_depth_format(false, false, render_buffers->get_can_be_storage());
		params.motion_format = render_buffers->get_velocity_format();
		params.reactive_format = render_buffers->get_base_data_format(); // Reactive is derived from input.
		params.output_format = render_buffers->get_base_data_format();
		params.motion_vector_scale = render_buffers->get_internal_size();
		mfx_temporal_context = p_effect->create_context(params);
		return true;
	}
	return false;
}
#endif

void RenderForwardClustered::RenderBufferDataForwardClustered::micro_geometry_stats_received(const Vector<uint8_t> &p_bytes, Ref<RenderBufferDataForwardClustered> p_data, uint64_t p_epoch, bool p_profiled) {
	if (p_data->micro_geometry_stats_epoch != p_epoch) {
		return;
	}
	p_data->micro_geometry_stats_pending = false;
	if (p_bytes.size() >= 8) {
		p_data->micro_geometry_stats_frame = p_data->micro_geometry_stats_submitted_frame;
		p_data->micro_geometry_clusters = decode_uint32(p_bytes.ptr());
		p_data->micro_geometry_triangles = decode_uint32(p_bytes.ptr() + 4);
	}
	if (p_profiled && p_bytes.size() >= MicroGeometrySelection::STATISTICS_BYTES && decode_uint32(p_bytes.ptr() + 8) != 0) {
		auto statistic = [&](uint32_t p_index) { return decode_uint32(p_bytes.ptr() + p_index * 4); };
		print_line(vformat("Microgeometry camera vertex work: frame=%d unique_cluster_vertices=%d triangle_corners=%d", p_data->micro_geometry_stats_submitted_frame, statistic(84), uint64_t(statistic(1)) * 3));
		print_line(vformat("Microgeometry camera mesh work: frame=%d launched_groups=%d valid_groups=%d invalid_clusters=%d evaluated_unique_vertices=%d vertex_program_evaluations=%d input_triangles=%d emitted_triangles=%d facing_rejected=%d footprint_rejected=%d uncertain_triangles=%d", p_data->micro_geometry_stats_submitted_frame, statistic(85), statistic(86), statistic(87), statistic(88), statistic(89), statistic(90), statistic(91), statistic(92), statistic(93), statistic(94)));
		print_line(vformat("Microgeometry camera cut: frame=%d flags=%d candidate_clusters=%d committed_clusters=%d retained_units=%d valid_units=%d", p_data->micro_geometry_stats_submitted_frame, statistic(3), statistic(28), statistic(29), statistic(30), statistic(31)));
		print_line(vformat("Microgeometry camera refinement: frame=%d wanted_not_ready_groups=%d wanted_ready_parents_inactive_groups=%d accepted_refinement_groups=%d threshold_stopped_groups=%d force_finest_units=%d emitted_coarse_inactive_child_clusters=%d", p_data->micro_geometry_stats_submitted_frame, statistic(32), statistic(33), statistic(34), statistic(39), statistic(35), statistic(36)));
		print_line(vformat("Microgeometry camera projected bounds: frame=%d instance_near_plane_units=%d instance_epsilon_clamped_units=%d refinement_near_plane_groups=%d refinement_epsilon_clamped_groups=%d", p_data->micro_geometry_stats_submitted_frame, statistic(64), statistic(65), statistic(66), statistic(67)));
		const char *diameter_buckets[] = { "0-16", "16-32", "32-64", "64-128", "128-256", "256+" };
		for (uint32_t bucket = 0; bucket < 6; bucket++) {
			const uint32_t offset = 40 + bucket * 4;
			print_line(vformat("Microgeometry camera size bucket: frame=%d conservative_diameter_px=%s valid_units=%d emitted_clusters=%d emitted_triangles=%d emitted_leaf_clusters=%d", p_data->micro_geometry_stats_submitted_frame, diameter_buckets[bucket], statistic(offset), statistic(offset + 1), statistic(offset + 2), statistic(offset + 3)));
		}
		for (uint32_t phase = 0; phase < 2; phase++) {
			const uint32_t offset = 4 + phase * 12;
			print_line(vformat("Microgeometry camera occlusion: frame=%d phase=%s input_clusters=%d frustum_rejected=%d hzb_disabled=%d task_ineligible=%d projection_unsafe=%d outside_viewport=%d hzb_sampled=%d zero_depth=%d hzb_rejected=%d residency_rejected=%d emitted_clusters=%d emitted_triangles=%d", p_data->micro_geometry_stats_submitted_frame, phase == 0 ? "initial" : "recovery", statistic(offset), statistic(offset + 1), statistic(offset + 2), statistic(offset + 3), statistic(offset + 4), statistic(offset + 5), statistic(offset + 6), statistic(offset + 7), statistic(offset + 8), statistic(offset + 9), statistic(offset + 10), statistic(offset + 11)));
			print_line(vformat("Microgeometry camera depth footprint: frame=%d phase=%s zero_depth_padding_overlap=%d zero_depth_interior=%d", p_data->micro_geometry_stats_submitted_frame, phase == 0 ? "initial" : "recovery", statistic(37 + phase), statistic(offset + 7) - statistic(37 + phase)));
			const uint32_t query_offset = 68 + phase * 8;
			print_line(vformat("Microgeometry camera HZB query: frame=%d phase=%s refined_tests=%d refined_rejected=%d inside_zero_failures=%d inside_nonzero_failures=%d budget_exhaustions=%d texture_fetches=%d peak_fetches=%d boundary_descents=%d", p_data->micro_geometry_stats_submitted_frame, phase == 0 ? "initial" : "recovery", statistic(query_offset), statistic(query_offset + 1), statistic(query_offset + 2), statistic(query_offset + 3), statistic(query_offset + 4), statistic(query_offset + 5), statistic(query_offset + 6), statistic(query_offset + 7)));
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::free_data() {
	if (camera_micro_geometry) {
		memdelete(camera_micro_geometry);
		camera_micro_geometry = nullptr;
	}
	micro_geometry_stats_epoch++;
	micro_geometry_stats_pending = false;
	micro_geometry_clusters = 0;
	micro_geometry_triangles = 0;
	micro_geometry_stats_frame = 0;
	micro_geometry_stats_submitted_frame = 0;
	micro_geometry_stats_profile_frame = 0;
	if (micro_geometry_depth.texture.is_valid()) {
		RD::get_singleton()->free_rid(micro_geometry_depth.texture);
		micro_geometry_depth.texture = RID();
		micro_geometry_depth.levels.clear();
		micro_geometry_depth.size = Size2i();
	}
	if (dlss_context) {
		RD::get_singleton()->flush_and_stall();
	}
	if (nrd_context) {
		memdelete(nrd_context);
		nrd_context = nullptr;
	}
	// JIC, should already have been cleared
	if (render_buffers) {
		render_buffers->clear_context(RB_SCOPE_FORWARD_CLUSTERED);
		render_buffers->clear_context(RB_SCOPE_RTXDI_SURFACE);
		render_buffers->clear_context(RB_SCOPE_SSDS);
		render_buffers->clear_context(RB_SCOPE_SSIL);
		render_buffers->clear_context(RB_SCOPE_SSAO);
		render_buffers->clear_context(RB_SCOPE_SSR);

		if (RenderForwardClustered *rfc = RenderForwardClustered::get_singleton()) {
			rfc->_free_rt_viewport_state(render_buffers);
		}
	}
	rtxdi_surface_set = 0;
	rtxdi_surface_frame_index = 0;
	rtxdi_surface_last_engine_frame = 0;
	rtxdi_surface_initialized = false;
	rtxdi_surface_history_valid = false;
	micro_geometry_history_valid = false;
	rtxdi_surface_depth_valid[0] = false;
	rtxdi_surface_depth_valid[1] = false;
	rtxdi_surface_size = Size2i();
	rtxdi_surface_camera = RID();
	rtxdi_surface_camera_orthogonal = false;
	rtxdi_surface_previous_camera_orthogonal = false;

	if (cluster_builder) {
		memdelete(cluster_builder);
		cluster_builder = nullptr;
	}

	if (fsr2_context) {
		memdelete(fsr2_context);
		fsr2_context = nullptr;
	}

#ifdef METAL_MFXTEMPORAL_ENABLED
	if (mfx_temporal_context) {
		memdelete(mfx_temporal_context);
		mfx_temporal_context = nullptr;
	}
#endif

	if (dlss_context) {
		memdelete(dlss_context);
		dlss_context = nullptr;
	}

	if (!render_sdfgi_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_sdfgi_uniform_set)) {
		RD::get_singleton()->free_rid(render_sdfgi_uniform_set);
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (render_buffers) {
		// JIC
		free_data();
	}

	render_buffers = p_render_buffers;
	ERR_FAIL_NULL(render_buffers);

	if (cluster_builder == nullptr) {
		cluster_builder = memnew(ClusterBuilderRD);
	}
	cluster_builder->set_shared(RenderForwardClustered::get_singleton()->get_cluster_builder_shared());

	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	cluster_builder->setup(p_render_buffers->get_internal_size(), p_render_buffers->get_max_cluster_elements(), p_render_buffers->get_depth_texture(), sampler, p_render_buffers->get_internal_texture());
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_color_only_fb() {
	ERR_FAIL_NULL_V(render_buffers, RID());

	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID color = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_MSAA) : render_buffers->get_internal_texture();
	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	if (render_buffers->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
		RID vrs_texture = render_buffers->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), color, depth, vrs_texture);
	} else {
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), color, depth);
	}
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_depth_fb(DepthFrameBufferType p_type) {
	ERR_FAIL_NULL_V(render_buffers, RID());
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	switch (p_type) {
		case DEPTH_FB: {
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth);
		} break;
		case DEPTH_FB_ROUGHNESS: {
			ensure_normal_roughness_texture();

			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);

			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer);
		} break;
		case DEPTH_FB_ROUGHNESS_VOXELGI: {
			ensure_normal_roughness_texture();
			ensure_voxelgi();

			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);
			RID voxelgi_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_VOXEL_GI_MSAA : RB_TEX_VOXEL_GI);

			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer, voxelgi_buffer);
		} break;
		default: {
			ERR_FAIL_V(RID());
		} break;
	}
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_only_fb() {
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID specular = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_SPECULAR_MSAA : RB_TEX_SPECULAR);

	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), specular);
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_velocity_only_fb() {
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID velocity = render_buffers->get_texture(RB_SCOPE_BUFFERS, use_msaa ? RB_TEX_VELOCITY_MSAA : RB_TEX_VELOCITY);

	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), velocity);
}

RD::DataFormat RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_format() {
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

uint32_t RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

RD::DataFormat RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_format() {
	return RD::DATA_FORMAT_R8G8B8A8_UNORM;
}

uint32_t RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

RD::DataFormat RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_format() {
	return RD::DATA_FORMAT_R8G8_UINT;
}

uint32_t RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

void RenderForwardClustered::setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataForwardClustered> data;
	data.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_FORWARD_CLUSTERED, data);
}

bool RenderForwardClustered::free(RID p_rid) {
	if (RendererSceneRenderRD::free(p_rid)) {
		return true;
	}
	return false;
}

void RenderForwardClustered::update() {
	RendererSceneRenderRD::update();
	_update_global_pipeline_data_requirements_from_project();
	_update_global_pipeline_data_requirements_from_light_storage();
}

/// RENDERING ///

bool RenderForwardClustered::_micro_geometry_eligible(const GeometryInstanceSurfaceDataCache *p_surface, PassMode p_pass) const {
	return p_surface->micro_geometry_element.in_list() && ((p_pass != PASS_MODE_SHADOW && p_pass != PASS_MODE_SHADOW_DP) || (p_surface->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW));
}

void RenderForwardClustered::_update_micro_geometry_instances(const LocalVector<RenderGeometryInstance *> &p_instances) {
	auto *mesh_storage = RendererRD::MeshStorage::get_singleton();
	auto *storage = mesh_storage->get_micro_geometry_storage();
	HashMap<RID, Ref<MicroGeometryData>> sources;
	HashMap<RID, RID> command_buffers;
	for (auto *geometry : p_instances) {
		auto *instance = static_cast<GeometryInstanceForwardClustered *>(geometry);
		if (!instance->persistent_instance) {
			continue;
		}
		const auto &record = raytracing->persistent_instances[uint32_t(instance->persistent_instance) - 1].data;
		RID asset = RID::from_uint64(record.asset);
		Ref<MicroGeometryData> source;
		if (asset.is_valid()) {
			if (!sources.has(asset)) {
				sources.insert(asset, storage->get_source(asset));
			}
			source = sources[asset];
		}
		const bool raster_ready = source.is_valid() && storage->is_ready(asset);
		const bool rt_ready = source.is_valid() && storage->is_ready(asset, true);
		bool raster_only = instance->surface_caches != nullptr;
		for (auto *surface = instance->surface_caches; surface; surface = surface->next) {
			MicroGeometrySelection::Task task;
			MicroGeometryRasterPass::Bin bins[2];
			RID commands;
			uint32_t source_surface = UINT32_MAX;
			uint32_t levels = 0;
			const auto *shader = surface->shader;
			bool eligible = source.is_valid() && shader && surface->persistent_surface && !instance->scene_data->mesh_instance.is_valid() && !instance->rt_procedural && instance->instance_count != 0 && surface->primitive == RSE::PRIMITIVE_TRIANGLES;
			eligible = eligible && (instance->scene_data->base_type == RSE::INSTANCE_MESH || instance->scene_data->base_type == RSE::INSTANCE_MULTIMESH);
			eligible = eligible && !shader->uses_alpha_pass() && !shader->uses_vertex && !shader->uses_position && !shader->uses_vertex_time && !shader->writes_modelview_or_projection && !shader->uses_particle_trails && !shader->uses_point_size && !shader->uses_z_clip_scale;
			if (eligible) {
				const auto &metadata = source->get_metadata();
				for (uint32_t index = 0; index < uint32_t(metadata.surfaces.size()); index++) {
					if (metadata.surfaces[index].source_surface == surface->surface_index) {
						source_surface = index;
						break;
					}
				}
				eligible = source_surface != UINT32_MAX;
				if (eligible) {
					task.instance = record.handle;
					task.surface = surface->persistent_surface;
					task.asset = record.asset;
					task.group_count = metadata.groups.size();
					task.cluster_count = metadata.clusters.size();
					task.coarse_count = metadata.coarse_cluster_count;
					task.multimesh_count = record.multimesh_address ? record.multimesh_count : 1;
					task.flags = instance->store_transform_cache ? 0 : 1;
					levels = metadata.roots.size();
					if (instance->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT) {
						if (!command_buffers.has(instance->scene_data->base)) {
							command_buffers.insert(instance->scene_data->base, mesh_storage->_multimesh_get_command_buffer_rd_rid(instance->scene_data->base));
						}
						commands = command_buffers[instance->scene_data->base];
						task.indirect_command = RD::get_singleton()->buffer_get_device_address(commands) + uint64_t(surface->surface_index) * sizeof(uint32_t) * RendererRD::MeshStorage::INDIRECT_MULTIMESH_COMMAND_STRIDE;
					}
					for (uint32_t index = 0; index < 2; index++) {
						bins[index].shader = index ? surface->shader_shadow : surface->shader;
						bins[index].material = index ? surface->material_uniform_set_shadow : surface->material_uniform_set;
						bins[index].flags = instance->base_flags;
						bins[index].mirror = instance->scene_data->mirror;
						bins[index].double_sided = (surface->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) != 0;
					}
				}
			}
			const bool raster_eligible = eligible && !instance->micro_geometry_cpu_culling && !shader->uses_time && raster_ready && task.multimesh_count && !instance->fade_near && !instance->fade_far && instance->force_alpha >= 1 && instance->parent_fade_alpha >= 1;
			raster_only &= raster_eligible;
			if (memcmp(&surface->micro_geometry_task, &task, sizeof(task)) != 0 || !(surface->micro_geometry_bins[0] == bins[0]) || !(surface->micro_geometry_bins[1] == bins[1]) || surface->micro_geometry_element.in_list() != raster_eligible) {
				micro_geometry_generation++;
			}
			const bool rt_eligible = eligible && rt_ready && task.multimesh_count && !(surface->rt_pass_flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA);
			const uint64_t material_generation = surface->persistent_surface ? raytracing->persistent_surfaces[uint32_t(surface->persistent_surface) - 1].data.material_generation : 0;
			if (surface->micro_geometry_rt_element.in_list() != rt_eligible || (rt_eligible && (memcmp(&surface->micro_geometry_task, &task, sizeof(task)) != 0 || surface->micro_geometry_rt_material_generation != material_generation))) {
				micro_geometry_rt_generation++;
			}
			surface->micro_geometry_rt_material_generation = material_generation;
			if (rt_eligible && !surface->micro_geometry_rt_element.in_list()) {
				micro_geometry_rt_surface_list.add(&surface->micro_geometry_rt_element);
			} else if (!rt_eligible) {
				surface->micro_geometry_rt_element.remove_from_list();
			}
			surface->micro_geometry_task = task;
			surface->micro_geometry_bins[0] = bins[0];
			surface->micro_geometry_bins[1] = bins[1];
			surface->micro_geometry_source = eligible ? source : Ref<MicroGeometryData>();
			surface->micro_geometry_commands = commands;
			surface->micro_geometry_levels = levels;
			surface->micro_geometry_surface_index = source_surface;
			surface->micro_geometry_rt_ready = eligible && rt_ready;
			if (raster_eligible && !surface->micro_geometry_element.in_list()) {
				micro_geometry_surface_list.add(&surface->micro_geometry_element);
			} else if (!raster_eligible) {
				surface->micro_geometry_element.remove_from_list();
			}
		}
		instance->set_micro_geometry_raster_only(raster_only);
	}
}

RenderForwardClustered::MicroGeometryRasterPass *RenderForwardClustered::_prepare_micro_geometry(const RenderDataRD *p_render_data, PassMode p_pass) {
	if (!raytracing || !scene_shader.micro_geometry_mesh_supported || p_pass == PASS_MODE_SDF || p_pass == PASS_MODE_DEPTH_MATERIAL) {
		return nullptr;
	}
	const bool camera_pass = p_pass == PASS_MODE_RTXDI_SURFACE && p_render_data->render_buffers.is_valid();
	const bool shadow = p_pass == PASS_MODE_SHADOW || p_pass == PASS_MODE_SHADOW_DP;
	auto &batch = micro_geometry_batches[p_pass];
	Vector<RID> voxelgis;
	for (uint32_t index = 0; index < scene_state.voxelgis_used; index++) {
		voxelgis.push_back(scene_state.voxelgi_ids[index]);
	}
	if (batch.generation != micro_geometry_generation || batch.debug_mode != get_debug_draw_mode() || batch.voxelgis != voxelgis) {
		batch.tasks.clear();
		batch.bins.clear();
		batch.materials.clear();
		batch.sources.clear();
		batch.dependencies.clear();
		batch.levels = 0;
		HashSet<RID> commands;
		HashSet<SceneShaderForwardClustered::MaterialData *> materials;
		for (auto *entry = micro_geometry_surface_list.first(); entry; entry = entry->next()) {
			auto *surface = entry->self();
			if (!_micro_geometry_eligible(surface, p_pass)) {
				continue;
			}
			MicroGeometryRasterPass::Bin bin = surface->micro_geometry_bins[(shadow || p_pass == PASS_MODE_DEPTH) ? 1 : 0];
#ifdef DEBUG_ENABLED
			if (!shadow) {
				if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LIGHTING) {
					bin.shader = scene_shader.default_material_shader_ptr;
					bin.material = scene_shader.default_material_uniform_set;
				} else if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
					bin.shader = scene_shader.overdraw_material_shader_ptr;
					bin.material = scene_shader.overdraw_material_uniform_set;
				} else if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS) {
					bin.shader = scene_shader.debug_shadow_splits_material_shader_ptr;
					bin.material = scene_shader.debug_shadow_splits_material_uniform_set;
				}
			}
#endif
			uint32_t bin_index = batch.bins.find(bin);
			if (bin_index == uint32_t(-1)) {
				bin_index = batch.bins.size();
				batch.bins.push_back(bin);
			}
			auto task = surface->micro_geometry_task;
			task.bin = bin_index;
			if (bin.shader->writes_depth) {
				task.flags |= 16;
			}
			if (p_pass == PASS_MODE_SHADOW_DP) {
				task.flags |= 2;
			}
			task.gi_offset = UINT32_MAX;
			if (surface->owner->voxel_gi_instances[0].is_valid()) {
				uint32_t probes[2] = { 0xffff, 0xffff };
				for (uint32_t probe = 0; probe < scene_state.voxelgis_used; probe++) {
					for (uint32_t slot = 0; slot < 2; slot++) {
						if (scene_state.voxelgi_ids[probe] == surface->owner->voxel_gi_instances[slot]) {
							probes[slot] = probe;
						}
					}
				}
				if (probes[0] == 0xffff) {
					SWAP(probes[0], probes[1]);
				}
				task.gi_offset = probes[0] | (probes[1] << 16);
				task.flags |= INSTANCE_DATA_FLAG_USE_VOXEL_GI;
			}

			batch.tasks.push_back(task);
			batch.levels = MAX(batch.levels, surface->micro_geometry_levels);
			batch.sources.insert(RID::from_uint64(task.asset), surface->micro_geometry_source);
			if (surface->micro_geometry_commands.is_valid()) {
				commands.insert(surface->micro_geometry_commands);
			}
			if (surface->material) {
				materials.insert(surface->material);
			}
		}
		for (RID command : commands) {
			batch.dependencies.push_back(command);
		}
		for (auto *material : materials) {
			batch.materials.push_back(material);
		}
		batch.generation = micro_geometry_generation;
		batch.debug_mode = get_debug_draw_mode();
		batch.voxelgis = voxelgis;
		batch.version++;
	}
	for (auto *material : batch.materials) {
		material->set_as_used();
	}
	RenderBufferDataForwardClustered *render_buffers = nullptr;
	if (camera_pass) {
		Ref<RenderBufferDataForwardClustered> data = p_render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		render_buffers = data.ptr();
	}
	MicroGeometrySelection::Pass **retained;
	if (camera_pass) {
		retained = &render_buffers->camera_micro_geometry;
	} else {
		const uint64_t frame = RSG::rasterizer->get_frame_number();
		if (micro_geometry_pass_frame != frame) {
			micro_geometry_pass_frame = frame;
			micro_geometry_pass_cursor = 0;
		}
		if (micro_geometry_pass_cursor == micro_geometry_passes.size()) {
			micro_geometry_passes.push_back(nullptr);
		}
		retained = &micro_geometry_passes[micro_geometry_pass_cursor++];
	}
	const bool freeze = camera_pass && p_render_data->render_buffers->is_micro_geometry_debug_freeze();
	if (*retained && ((*retained)->input_version != batch.version || (*retained)->input_kind != uint32_t(p_pass) || (*retained)->freeze_requested != freeze || batch.tasks.is_empty())) {
		memdelete(*retained);
		*retained = nullptr;
	}
	if (batch.tasks.is_empty()) {
		if (render_buffers) {
			render_buffers->micro_geometry_clusters = 0;
			render_buffers->micro_geometry_triangles = 0;
			render_buffers->micro_geometry_stats_epoch++;
			render_buffers->micro_geometry_stats_pending = false;
		}
		return nullptr;
	}
	MicroGeometrySelection::Parameters parameters;
	parameters.flags = 1 | 2 | (shadow ? 64 : 0);
	parameters.scenario = p_render_data->scenario.get_id();
	const uint32_t output_height = camera_pass ? p_render_data->render_buffers->get_target_size().y : MAX(1, micro_geometry_pass_size.y);
	const Size2i hzb_size = camera_pass ? p_render_data->render_buffers->get_internal_size() : micro_geometry_pass_size.max(Size2i(1, 1));
	parameters.layer_mask = p_render_data->scene_data->camera_visible_layers;
	parameters.near_plane = p_render_data->scene_data->cam_projection.get_z_near();
	parameters.output_height = output_height;
	parameters.hzb_width = hzb_size.x;
	parameters.hzb_height = hzb_size.y;
	const RenderSceneDataRD *scene = p_render_data->scene_data;
	Vector<Plane> planes = shadow ? micro_geometry_shadow_planes : scene->cam_projection.get_projection_planes(Transform3D());
	ERR_FAIL_COND_V(planes.size() > 24, nullptr);
	parameters.cull_plane_count = planes.size();
	Transform3D inverse_view(scene->cam_transform.basis.inverse(), Vector3());
	if (shadow) {
		Vector3 offset;
		for (int axis = 0; axis < 3; axis++) {
			offset[axis] = micro_geometry_shadow_origin[axis] - scene->cam_origin[axis];
		}
		inverse_view.origin = inverse_view.basis.xform(offset);
	}
	for (uint32_t index = 0; index < parameters.cull_plane_count; index++) {
		const Plane plane = shadow ? inverse_view.xform(planes[index]) : planes[index];
		for (uint32_t axis = 0; axis < 3; axis++) {
			parameters.cull_planes[index * 4 + axis] = plane.normal[axis];
		}
		parameters.cull_planes[index * 4 + 3] = plane.d;
	}

	Projection correction;
	correction.set_depth_correction(scene->flip_y);
	correction.add_jitter_offset(scene->taa_jitter);
	RendererRD::MaterialStorage::store_camera(correction * scene->cam_projection, parameters.projection);
	correction.set_depth_correction(scene->flip_y);
	correction.add_jitter_offset(scene->prev_taa_jitter);
	RendererRD::MaterialStorage::store_camera(correction * scene->prev_cam_projection, parameters.previous_projection);
	RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(scene->cam_transform.basis.inverse(), Vector3()), parameters.view_rotation);
	RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(scene->prev_cam_transform.basis.inverse(), Vector3()), parameters.previous_view_rotation);
	for (int axis = 0; axis < 3; axis++) {
		RendererRD::MaterialStorage::split_double(scene->cam_origin[axis], &parameters.camera[axis], &parameters.camera_low[axis]);
		RendererRD::MaterialStorage::split_double(scene->prev_cam_origin[axis], &parameters.previous_camera[axis], &parameters.previous_camera_low[axis]);
	}
	for (uint32_t index = 0; index < scene_state.lightmaps_used; index++) {
		parameters.lightmaps[index] = scene_state.lightmap_ids[index].get_id();
		parameters.lightmap_sh |= uint32_t(scene_state.lightmap_has_sh[index]) << index;
	}
	if (!micro_geometry) {
		micro_geometry = memnew(MicroGeometrySelection);
	}
	if (!*retained) {
		Vector<RID> dependencies;
		raytracing->get_persistent_buffer_dependencies(dependencies);
		*retained = micro_geometry->create(batch.tasks, batch.bins.size(), parameters, batch.levels, sizeof(SceneState::InstanceData), raytracing->get_persistent_instance_buffer(), raytracing->get_persistent_surface_buffer(), dependencies);
		if (!*retained) {
			return nullptr;
		}
		auto *gpu = *retained;
		gpu->input_version = batch.version;
		gpu->input_kind = p_pass;
		gpu->freeze_requested = freeze;
		auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
		for (const auto &source : batch.sources) {
			storage->acquire(source.value);
			gpu->assets.push_back(source.key);
			for (uint32_t group = 0; freeze && group < uint32_t(source.value->get_metadata().groups.size()); group++) {
				if (storage->is_group_ready(source.key, group)) {
					storage->pin_group(source.key, group);
					gpu->pins.push_back({ source.key, group });
				}
			}
		}
	} else {
		auto *gpu = *retained;
		parameters.task_count = gpu->data.task_count;
		parameters.bin_count = gpu->data.bin_count;
		parameters.queue_work = gpu->data.queue_work;
		parameters.record_work = gpu->data.record_work;
		parameters.unit_count = gpu->data.unit_count;
		gpu->data = parameters;
	}
	auto *pass = memnew(MicroGeometryRasterPass);
	pass->gpu = *retained;
	pass->owns_gpu = false;
	pass->bins = batch.bins;
	pass->task_dependencies = batch.dependencies;
	pass->render_buffers = render_buffers;
	return pass;
}

void RenderForwardClustered::_select_micro_geometry(MicroGeometryRasterPass *p_pass) {
	if (!p_pass || p_pass->dispatched) {
		return;
	}
	RENDER_TIMESTAMP(p_pass->render_buffers ? "Microgeometry Camera Selection Prepare" : "Microgeometry Shadow Selection Prepare");
	p_pass->gpu->persistent_instances = raytracing->get_persistent_instance_buffer();
	p_pass->gpu->persistent_surfaces = raytracing->get_persistent_surface_buffer();
	p_pass->gpu->dependencies.clear();
	raytracing->get_persistent_buffer_dependencies(p_pass->gpu->dependencies);
	p_pass->gpu->dependencies.append_array(p_pass->task_dependencies);
	MicroGeometryRasterParameters raster = p_pass->gpu->raster_data;
	raster.page_pool = RD::get_singleton()->buffer_get_device_address(RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->get_pool());
	raster.selected_cluster_color = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RASTER;
	if (memcmp(&p_pass->gpu->raster_data, &raster, sizeof(raster)) != 0) {
		RD::get_singleton()->buffer_update(p_pass->gpu->raster_parameters, 0, sizeof(raster), &raster);
		p_pass->gpu->raster_data = raster;
	}
	if (p_pass->gpu->frozen) {
		micro_geometry->update_frozen(p_pass->gpu);
		p_pass->dispatched = true;
		return;
	}
	p_pass->gpu->data.flags &= ~128u;
	if (p_pass->render_buffers && RSG::utilities->capturing_timestamps && !p_pass->render_buffers->micro_geometry_stats_pending && !p_pass->gpu->freeze_requested && RSG::rasterizer->get_frame_number() >= p_pass->render_buffers->micro_geometry_stats_profile_frame + 120) {
		p_pass->gpu->data.flags |= 128;
		print_line(vformat("Microgeometry camera selection: frame=%d owner=%d error=%f output_height=%f near_plane=%f hzb_size=%dx%d tasks=%d units=%d", RSG::rasterizer->get_frame_number(), p_pass->gpu->capacity_feedback->owner, p_pass->gpu->data.error, p_pass->gpu->data.output_height, p_pass->gpu->data.near_plane, p_pass->gpu->data.hzb_width, p_pass->gpu->data.hzb_height, p_pass->gpu->data.task_count, p_pass->gpu->data.unit_count));
	}
	RID depth;
	if ((p_pass->gpu->data.flags & 2) != 0 && p_pass->render_buffers && p_pass->render_buffers->is_micro_geometry_history_valid() && p_pass->render_buffers->micro_geometry_depth.texture.is_valid()) {
		p_pass->gpu->data.flags |= 4;
		p_pass->gpu->data.hzb_mips = p_pass->render_buffers->micro_geometry_depth.levels.size();
		depth = p_pass->render_buffers->micro_geometry_depth.texture;
	}
	if (RSG::utilities->capturing_timestamps && RSG::rasterizer->get_frame_number() % 120 == 0) {
		print_line(vformat("Microgeometry HZB: frame=%d pass=%s owner=%d camera_eligible=%s history_valid=%s pyramid_valid=%s enabled=%s frozen=%s", RSG::rasterizer->get_frame_number(), p_pass->render_buffers ? "camera" : "shadow", p_pass->gpu->capacity_feedback->owner, p_pass->render_buffers && (p_pass->gpu->data.flags & 2) != 0, p_pass->render_buffers && p_pass->render_buffers->is_micro_geometry_history_valid(), p_pass->render_buffers && p_pass->render_buffers->micro_geometry_depth.texture.is_valid(), (p_pass->gpu->data.flags & 4) != 0, p_pass->gpu->frozen));
	}
	micro_geometry->select(p_pass->gpu, depth);
	p_pass->dispatched = true;
}

void RenderForwardClustered::_render_micro_geometry(RD::DrawListID p_list, RD::FramebufferFormatID p_framebuffer_format, RenderListParameters *p_parameters) {
	MicroGeometryRasterPass *pass = p_parameters->micro_geometry;
	if (!pass || !pass->dispatched) {
		return;
	}
	static const SceneShaderForwardClustered::PipelineVersion versions[PASS_MODE_MAX] = {
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS,
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP,
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS,
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS,
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI,
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL,
		SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF,
		SceneShaderForwardClustered::PIPELINE_VERSION_RTXDI_SURFACE
	};
	micro_geometry->add_draw_dependencies(pass->gpu, p_list);
	uint32_t mesh_dispatches = 0;
	for (uint32_t index = 0; index < pass->bins.size(); index++) {
		const MicroGeometryRasterPass::Bin &bin = pass->bins[index];
		auto *shader = bin.shader;
		if (!shader) {
			continue;
		}
		SceneShaderForwardClustered::ShaderData::PipelineKey key;
		key.micro_geometry = true;
		key.vertex_format_id = RD::INVALID_ID;
		key.framebuffer_format_id = p_framebuffer_format;
		key.primitive_type = RSE::PRIMITIVE_TRIANGLES;
		key.version = versions[p_parameters->pass_mode];
		key.wireframe = p_parameters->force_wireframe;
		auto cull = (bin.mirror != p_parameters->reverse_cull) ? SceneShaderForwardClustered::ShaderData::CULL_VARIANT_REVERSED : SceneShaderForwardClustered::ShaderData::CULL_VARIANT_NORMAL;
		if (p_parameters->pass_mode == PASS_MODE_SDF || p_parameters->pass_mode == PASS_MODE_DEPTH_MATERIAL || ((p_parameters->pass_mode == PASS_MODE_SHADOW || p_parameters->pass_mode == PASS_MODE_SHADOW_DP) && bin.double_sided)) {
			cull = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
		}
		const RD::PolygonCullMode cull_mode = shader->get_cull_mode_from_cull_variant(cull);
		auto specialization = p_parameters->base_specialization;
		specialization.multimesh = (bin.flags & INSTANCE_DATA_FLAG_MULTIMESH) != 0;
		specialization.multimesh_format_2d = (bin.flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D) != 0;
		specialization.multimesh_has_color = (bin.flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR) != 0;
		specialization.multimesh_has_custom_data = (bin.flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA) != 0;
		RID pipeline;
		for (key.ubershader = 0; key.ubershader < 2; key.ubershader++) {
			key.cull_mode = key.ubershader ? RD::POLYGON_CULL_DISABLED : cull_mode;
			key.shader_specialization = key.ubershader ? SceneShaderForwardClustered::ShaderSpecialization{} : specialization;
			pipeline = shader->pipeline_hash_map.get_pipeline(key, key.hash(), key.ubershader, key.ubershader ? RSE::PIPELINE_SOURCE_DRAW : RSE::PIPELINE_SOURCE_SPECIALIZATION);
			if (pipeline.is_valid()) {
				break;
			}
		}
		if (pipeline.is_null()) {
			continue;
		}
		RD::get_singleton()->draw_list_bind_render_pipeline(p_list, pipeline);
		if (bin.material.is_valid()) {
			RD::get_singleton()->draw_list_bind_uniform_set(p_list, bin.material, MATERIAL_UNIFORM_SET);
		}
		RD::get_singleton()->draw_list_bind_uniform_set(p_list, micro_geometry->get_raster_uniform_set(pass->gpu, shader->get_shader_variant(key.version, key.ubershader, true)), 4);
		const auto &range = pass->gpu->bin_data[index];
		SceneState::PushConstant push = {};
		push.base_index = range.offset;
		push.micro_geometry_bin = index;
		push.micro_geometry_flags = uint32_t(pass->gpu->recovered) | uint32_t(!pass->gpu->frozen && (pass->gpu->data.flags & 128) != 0) << 1;
		push.uv_offset = uint32_t(Math::make_half_float(p_parameters->uv_offset.y)) << 16 | Math::make_half_float(p_parameters->uv_offset.x);
		push.ubershader.specialization = specialization;
		push.ubershader.constants.cull_mode = cull_mode;
		RD::get_singleton()->draw_list_set_push_constant(p_list, &push, key.ubershader ? sizeof(push) : sizeof(push) - sizeof(push.ubershader));
		RD::get_singleton()->draw_list_draw_mesh_tasks_indirect(p_list, pass->gpu->dispatch_arguments, index * 12);
		mesh_dispatches++;
		if (shader->uses_time) {
			RenderingServerDefault::redraw_request();
		}
	}
	if (RSG::utilities->capturing_timestamps && RSG::rasterizer->get_frame_number() % 120 == 0) {
		print_line(vformat("Microgeometry mesh stage dispatch: frame=%d mode=%d bins=%d dispatches=%d width=%d recovery=%s frozen=%s", RSG::rasterizer->get_frame_number(), p_parameters->pass_mode, pass->bins.size(), mesh_dispatches, pass->gpu->raster_data.dispatch_width, pass->gpu->recovered, pass->gpu->frozen));
	}
}

template <RenderForwardClustered::PassMode p_pass_mode>
void RenderForwardClustered::_render_list_template(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	RD::DrawListID draw_list = p_draw_list;
	RD::FramebufferFormatID framebuffer_format = p_framebuffer_Format;

	//global scope bindings
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, render_base_uniform_set, SCENE_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, p_params->render_pass_uniform_set, RENDER_PASS_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);

	RID prev_material_uniform_set;

	RID prev_vertex_array_rd;
	RID prev_index_array_rd;
	RID prev_xforms_uniform_set;

	SceneShaderForwardClustered::ShaderData *shader = nullptr;
	SceneShaderForwardClustered::ShaderData *prev_shader = nullptr;
	SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key;
	uint32_t pipeline_hash = 0;
	uint32_t prev_pipeline_hash = 0;

	bool shadow_pass = (p_pass_mode == PASS_MODE_SHADOW) || (p_pass_mode == PASS_MODE_SHADOW_DP);

	SceneState::PushConstant push_constant;

	if constexpr (p_pass_mode == PASS_MODE_DEPTH_MATERIAL) {
		push_constant.uv_offset = Math::make_half_float(p_params->uv_offset.y) << 16;
		push_constant.uv_offset |= Math::make_half_float(p_params->uv_offset.x);
	} else {
		push_constant.uv_offset = 0;
	}

	bool should_request_redraw = false;

	for (uint32_t i = p_from_element; i < p_to_element; i++) {
		const GeometryInstanceSurfaceDataCache *surf = p_params->elements[i].surface;
		const RenderElementInfo &element_info = p_params->element_info[i];

		if (surf->owner->instance_count == 0) {
			continue;
		}

		push_constant.base_index = i + p_params->element_offset;

		RID material_uniform_set;
		void *mesh_surface;

		if (shadow_pass || p_pass_mode == PASS_MODE_DEPTH) { //regular depth pass can use these too
			material_uniform_set = surf->material_uniform_set_shadow;
			shader = surf->shader_shadow;
			mesh_surface = surf->surface_shadow;

		} else {
#ifdef DEBUG_ENABLED
			if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LIGHTING)) {
				material_uniform_set = scene_shader.default_material_uniform_set;
				shader = scene_shader.default_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW)) {
				material_uniform_set = scene_shader.overdraw_material_uniform_set;
				shader = scene_shader.overdraw_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS)) {
				material_uniform_set = scene_shader.debug_shadow_splits_material_uniform_set;
				shader = scene_shader.debug_shadow_splits_material_shader_ptr;
			} else {
#endif
				material_uniform_set = surf->material_uniform_set;
				shader = surf->shader;
				surf->material->set_as_used();
#ifdef DEBUG_ENABLED
			}
#endif
			mesh_surface = surf->surface;
		}

		if (!mesh_surface) {
			continue;
		}

		//request a redraw if one of the shaders uses TIME
		if (shader->uses_time) {
			should_request_redraw = true;
		}

		// Determine the cull variant.
		SceneShaderForwardClustered::ShaderData::CullVariant cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_MAX;
		if constexpr (p_pass_mode == PASS_MODE_DEPTH_MATERIAL || p_pass_mode == PASS_MODE_SDF) {
			cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
		} else {
			if constexpr (p_pass_mode == PASS_MODE_SHADOW || p_pass_mode == PASS_MODE_SHADOW_DP) {
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) {
					cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
				}
			}

			if (cull_variant == SceneShaderForwardClustered::ShaderData::CULL_VARIANT_MAX) {
				bool mirror = surf->owner->scene_data->mirror;
				if (p_params->reverse_cull) {
					mirror = !mirror;
				}

				cull_variant = mirror ? SceneShaderForwardClustered::ShaderData::CULL_VARIANT_REVERSED : SceneShaderForwardClustered::ShaderData::CULL_VARIANT_NORMAL;
			}
		}

		pipeline_key.primitive_type = surf->primitive;

		RID xforms_uniform_set = surf->owner->transforms_uniform_set;

		SceneShaderForwardClustered::ShaderSpecialization pipeline_specialization = p_params->base_specialization;
		pipeline_specialization.multimesh = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH);
		pipeline_specialization.multimesh_format_2d = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D);
		pipeline_specialization.multimesh_has_color = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR);
		pipeline_specialization.multimesh_has_custom_data = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA);

		switch (p_pass_mode) {
			case PASS_MODE_SHADOW:
			case PASS_MODE_DEPTH: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
			} break;
			case PASS_MODE_SHADOW_DP: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for shadow DP pass");
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP;
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
			} break;
			case PASS_MODE_DEPTH_MATERIAL: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for material pass");
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL;
			} break;
			case PASS_MODE_SDF: {
				// Note, SDF is prepared in world space, this shouldn't be a multiview buffer even when stereoscopic rendering is used.
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for SDF pass");
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF;
			} break;
			case PASS_MODE_RTXDI_SURFACE: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for RTXDI surface pass");
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_RTXDI_SURFACE;
			} break;
		}

		pipeline_key.framebuffer_format_id = framebuffer_format;
		pipeline_key.wireframe = p_params->force_wireframe;
		pipeline_key.ubershader = 0;

		bool emulate_point_size = shader->uses_point_size && scene_shader.emulate_point_size;

		const RD::PolygonCullMode cull_mode = shader->get_cull_mode_from_cull_variant(cull_variant);
		RID vertex_array_rd;
		RID index_array_rd;
		RID pipeline_rd;
		const uint32_t ubershader_iterations = 2;
		bool pipeline_valid = false;
		while (pipeline_key.ubershader < ubershader_iterations) {
			// Skeleton and blend shape.
			RD::VertexFormatID vertex_format = -1;
			bool pipeline_motion_vectors = p_pass_mode == PASS_MODE_RTXDI_SURFACE;
			uint64_t input_mask = shader->get_vertex_input_mask(pipeline_key.version, pipeline_key.ubershader);
			if (surf->owner->scene_data->mesh_instance.is_valid()) {
				mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(surf->owner->scene_data->mesh_instance, surf->surface_index, input_mask, pipeline_motion_vectors, emulate_point_size, vertex_array_rd, vertex_format);
			} else {
				mesh_storage->mesh_surface_get_vertex_arrays_and_format(mesh_surface, input_mask, pipeline_motion_vectors, emulate_point_size, vertex_array_rd, vertex_format);
			}

			pipeline_key.vertex_format_id = vertex_format;

			if (pipeline_key.ubershader) {
				pipeline_key.shader_specialization = {};
				pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
			} else {
				pipeline_key.shader_specialization = pipeline_specialization;
				pipeline_key.cull_mode = cull_mode;
			}

			pipeline_hash = pipeline_key.hash();

			if (shader != prev_shader || pipeline_hash != prev_pipeline_hash) {
				RSE::PipelineSource pipeline_source = pipeline_key.ubershader ? RSE::PIPELINE_SOURCE_DRAW : RSE::PIPELINE_SOURCE_SPECIALIZATION;
				pipeline_rd = shader->pipeline_hash_map.get_pipeline(pipeline_key, pipeline_hash, pipeline_key.ubershader, pipeline_source);

				if (pipeline_rd.is_valid()) {
					pipeline_valid = true;
					prev_shader = shader;
					prev_pipeline_hash = pipeline_hash;
					break;
				} else {
					pipeline_key.ubershader++;
				}
			} else {
				// The same pipeline is bound already.
				pipeline_valid = true;
				break;
			}
		}

		if (pipeline_valid) {
			if (!emulate_point_size) {
				index_array_rd = mesh_storage->mesh_surface_get_index_array(mesh_surface, element_info.lod_index);
			} else {
				index_array_rd = RID();
			}

			if (prev_vertex_array_rd != vertex_array_rd) {
				RD::get_singleton()->draw_list_bind_vertex_array(draw_list, vertex_array_rd);
				prev_vertex_array_rd = vertex_array_rd;
			}

			if (prev_index_array_rd != index_array_rd) {
				if (index_array_rd.is_valid()) {
					RD::get_singleton()->draw_list_bind_index_array(draw_list, index_array_rd);
				}
				prev_index_array_rd = index_array_rd;
			}

			if (!pipeline_rd.is_null()) {
				RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline_rd);
			}

			if (xforms_uniform_set.is_valid() && prev_xforms_uniform_set != xforms_uniform_set) {
				RD::get_singleton()->draw_list_bind_uniform_set(draw_list, xforms_uniform_set, TRANSFORMS_UNIFORM_SET);
				prev_xforms_uniform_set = xforms_uniform_set;
			}

			if (material_uniform_set != prev_material_uniform_set) {
				// Update uniform set.
				if (material_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(material_uniform_set)) { // Material may not have a uniform set.
					RD::get_singleton()->draw_list_bind_uniform_set(draw_list, material_uniform_set, MATERIAL_UNIFORM_SET);
				}

				prev_material_uniform_set = material_uniform_set;
			}

			if (surf->owner->base_flags & INSTANCE_DATA_FLAG_PARTICLES) {
				particles_storage->particles_get_instance_buffer_motion_vectors_offsets(surf->owner->scene_data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
			} else if (surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) {
				mesh_storage->_multimesh_get_motion_vectors_offsets(surf->owner->scene_data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
			} else {
				push_constant.multimesh_motion_vectors_current_offset = 0;
				push_constant.multimesh_motion_vectors_previous_offset = 0;
			}

			size_t push_constant_size = 0;
			if (pipeline_key.ubershader) {
				push_constant_size = sizeof(SceneState::PushConstant);
				push_constant.ubershader.specialization = pipeline_specialization;
				push_constant.ubershader.constants = {};
				push_constant.ubershader.constants.cull_mode = cull_mode;
			} else {
				push_constant_size = sizeof(SceneState::PushConstant) - sizeof(SceneState::PushConstantUbershader);
			}

			RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, push_constant_size);

			uint32_t instance_count = surf->owner->instance_count > 1 ? surf->owner->instance_count : element_info.repeat;
			if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS) {
				instance_count /= surf->owner->trail_steps;
			}

			bool indirect = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT);

			if (emulate_point_size) {
				if (indirect) {
					WARN_PRINT("Indirect draws are not supported when emulating point size.");
				}
				RD::get_singleton()->draw_list_draw(draw_list, false, mesh_storage->mesh_surface_get_vertex_count(mesh_surface), instance_count * 6);
			} else if (indirect) {
				RD::get_singleton()->draw_list_draw_indirect(draw_list, index_array_rd.is_valid(), mesh_storage->_multimesh_get_command_buffer_rd_rid(surf->owner->scene_data->base), surf->surface_index * sizeof(uint32_t) * mesh_storage->INDIRECT_MULTIMESH_COMMAND_STRIDE, 1, 0);
			} else {
				RD::get_singleton()->draw_list_draw(draw_list, index_array_rd.is_valid(), instance_count);
			}
		}

		i += element_info.repeat - 1; //skip equal elements
	}

	// Make the actual redraw request
	if (should_request_redraw) {
		RenderingServerDefault::redraw_request();
	}
	_render_micro_geometry(draw_list, framebuffer_format, p_params);
}

void RenderForwardClustered::_render_list(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	//use template for faster performance (pass mode comparisons are inlined)

	switch (p_params->pass_mode) {
		case PASS_MODE_SHADOW: {
			_render_list_template<PASS_MODE_SHADOW>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SHADOW_DP: {
			_render_list_template<PASS_MODE_SHADOW_DP>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH: {
			_render_list_template<PASS_MODE_DEPTH>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_MATERIAL: {
			_render_list_template<PASS_MODE_DEPTH_MATERIAL>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SDF: {
			_render_list_template<PASS_MODE_SDF>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_RTXDI_SURFACE: {
			_render_list_template<PASS_MODE_RTXDI_SURFACE>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		default: {
			// Unknown pass mode.
		} break;
	}
}

void RenderForwardClustered::_render_list_with_draw_list(RenderListParameters *p_params, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_color_values, float p_clear_depth_value, uint32_t p_clear_stencil_value, const Rect2 &p_region) {
	_select_micro_geometry(p_params->micro_geometry);
	RD::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_framebuffer);
	p_params->framebuffer_format = fb_format;
	const bool shadow_pass = p_params->pass_mode == PASS_MODE_SHADOW || p_params->pass_mode == PASS_MODE_SHADOW_DP;
	RENDER_TIMESTAMP(shadow_pass ? "Shadow Raster Combined Draw" : "Camera Raster Combined Draw");

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, p_draw_flags, p_clear_color_values, p_clear_depth_value, p_clear_stencil_value, p_region);
	_render_list(draw_list, fb_format, p_params, 0, p_params->element_count);
	RD::get_singleton()->draw_list_end();
	RENDER_TIMESTAMP(shadow_pass ? "Shadow Raster Combined Draw Complete" : "Camera Raster Combined Draw Complete");
	MicroGeometryRasterPass *pass = p_params->micro_geometry;
	if (pass && !pass->gpu->frozen && pass->render_buffers && p_params->view_count == 1) {
		auto &pyramid = pass->render_buffers->micro_geometry_depth;
		RID depth = pass->render_buffers->get_primary_surface_depth_attachment();
		RID classification = pass->render_buffers->get_rtxdi_surface_texture(RenderBufferDataForwardClustered::RTXDI_SURFACE_CLASSIFICATION);
		const Size2i size(pass->gpu->data.hzb_width, pass->gpu->data.hzb_height);
		micro_geometry->build_depth_pyramid(pyramid, depth, classification, size);
		if ((pass->gpu->data.flags & 4) != 0) {
			pass->gpu->data.hzb_mips = pyramid.levels.size();
			micro_geometry->recover(pass->gpu, pyramid.texture);
			RENDER_TIMESTAMP("Microgeometry Camera Recovery Draw");
			RD::DrawListID recovery_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0, 0, p_region);
			RD::get_singleton()->draw_list_bind_uniform_set(recovery_list, render_base_uniform_set, SCENE_UNIFORM_SET);
			RD::get_singleton()->draw_list_bind_uniform_set(recovery_list, p_params->render_pass_uniform_set, RENDER_PASS_UNIFORM_SET);
			RD::get_singleton()->draw_list_bind_uniform_set(recovery_list, scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);
			_render_micro_geometry(recovery_list, fb_format, p_params);
			RD::get_singleton()->draw_list_end();
			RENDER_TIMESTAMP("Microgeometry Camera Recovery Draw Complete");
			micro_geometry->build_depth_pyramid(pyramid, depth, classification, size);
		}
	}
	if (pass) {
		micro_geometry->submit_feedback(pass->gpu);
		if (pass->gpu->freeze_requested && !pass->gpu->frozen) {
			if (!micro_geometry->freeze(pass->gpu)) {
				pass->render_buffers->camera_micro_geometry = nullptr;
				pass->owns_gpu = true;
			}
		}
		if (pass->render_buffers && !pass->render_buffers->micro_geometry_stats_pending) {
			RENDER_TIMESTAMP("Microgeometry Raster Statistics Readback");
			Ref<RenderBufferDataForwardClustered> data(pass->render_buffers);
			data->micro_geometry_stats_submitted_frame = RSG::rasterizer->get_frame_number();
			data->micro_geometry_stats_pending = true;
			const bool profiled = !pass->gpu->frozen && (pass->gpu->data.flags & 128) != 0;
			if (RD::get_singleton()->buffer_get_data_async(pass->gpu->statistics, callable_mp_static(&RenderBufferDataForwardClustered::micro_geometry_stats_received).bind(data, data->micro_geometry_stats_epoch, profiled)) != OK) {
				data->micro_geometry_stats_pending = false;
			} else if (profiled) {
				data->micro_geometry_stats_profile_frame = data->micro_geometry_stats_submitted_frame;
			}
			RENDER_TIMESTAMP("Microgeometry Raster Statistics Readback Complete");
		}
	}
	if (RSG::utilities->capturing_timestamps && RSG::rasterizer->get_frame_number() % 120 == 0) {
		const RenderBufferDataForwardClustered *data = pass ? pass->render_buffers : nullptr;
		print_line(vformat("Microgeometry raster draw: frame=%d pass=%s mode=%d owner=%d conventional_elements=%d micro_bins=%d hzb_enabled=%s recovery=%s raster_stats_available=%s raster_stats_frame=%d raster_clusters=%d raster_triangles=%d", RSG::rasterizer->get_frame_number(), shadow_pass ? "shadow" : "camera", p_params->pass_mode, pass ? pass->gpu->capacity_feedback->owner : 0, p_params->element_count, pass ? pass->bins.size() : 0, pass && (pass->gpu->data.flags & 4) != 0, pass && pass->gpu->recovered, data && data->micro_geometry_stats_frame != 0, data ? data->micro_geometry_stats_frame : 0, data ? data->micro_geometry_clusters : 0, data ? data->micro_geometry_triangles : 0));
	}
}

uint32_t RenderForwardClustered::_setup_environment(const RenderDataRD *p_render_data, bool p_no_fog, const Size2i &p_screen_size, const Size2 &p_viewport_size, const Color &p_default_bg_color, bool p_opaque_render_buffers, bool p_apply_alpha_multiplier, bool p_pancake_shadows) {
	micro_geometry_pass_size = p_viewport_size;
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rd = p_render_data->render_buffers;
	RID env = is_environment(p_render_data->environment) ? p_render_data->environment : RID();
	RID reflection_probe_instance = p_render_data->reflection_probe.is_valid() ? light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe) : RID();

	// May do this earlier in RenderSceneRenderRD::render_scene
	uint32_t uniform_buffer_index = scene_state.used_uniform_buffer_count;
	++scene_state.used_uniform_buffer_count;

	if (uniform_buffer_index >= scene_state.uniform_buffers.size()) {
		uint32_t from = scene_state.uniform_buffers.size();
		scene_state.uniform_buffers.resize(uniform_buffer_index + 1);
		for (uint32_t i = from; i < scene_state.uniform_buffers.size(); i++) {
			scene_state.uniform_buffers[i] = p_render_data->scene_data->create_uniform_buffer();
		}
	}

	float luminance_multiplier = rd.is_valid() ? rd->get_luminance_multiplier() : 1.0;

	p_render_data->scene_data->update_ubo(scene_state.uniform_buffers[uniform_buffer_index], get_debug_draw_mode(), env, reflection_probe_instance, p_render_data->camera_attributes, p_pancake_shadows, p_screen_size, p_viewport_size, p_default_bg_color, luminance_multiplier, p_opaque_render_buffers, p_apply_alpha_multiplier);

	// now do implementation UBO

	scene_state.ubo.cluster_shift = Math::get_shift_from_power_of_2(p_render_data->cluster_size);
	scene_state.ubo.max_cluster_element_count_div_32 = p_render_data->cluster_max_elements / 32;
	{
		uint32_t cluster_screen_width = Math::division_round_up((uint32_t)p_screen_size.width, p_render_data->cluster_size);
		uint32_t cluster_screen_height = Math::division_round_up((uint32_t)p_screen_size.height, p_render_data->cluster_size);
		scene_state.ubo.cluster_type_size = cluster_screen_width * cluster_screen_height * (scene_state.ubo.max_cluster_element_count_div_32 + 32);
		scene_state.ubo.cluster_width = cluster_screen_width;
	}

	scene_state.ubo.gi_upscale_for_msaa = false;
	scene_state.ubo.volumetric_fog_enabled = false;

	if (rd.is_valid()) {
		if (rd->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED) {
			scene_state.ubo.gi_upscale_for_msaa = true;
		}

		if (rd->has_custom_data(RB_SCOPE_FOG)) {
			Ref<RendererRD::Fog::VolumetricFog> fog = rd->get_custom_data(RB_SCOPE_FOG);

			scene_state.ubo.volumetric_fog_enabled = true;
			float fog_end = fog->length;
			if (fog_end > 0.0) {
				scene_state.ubo.volumetric_fog_inv_length = 1.0 / fog_end;
			} else {
				scene_state.ubo.volumetric_fog_inv_length = 1.0;
			}

			float fog_detail_spread = fog->spread; //reverse lookup
			if (fog_detail_spread > 0.0) {
				scene_state.ubo.volumetric_fog_detail_spread = 1.0 / fog_detail_spread;
			} else {
				scene_state.ubo.volumetric_fog_detail_spread = 1.0;
			}
		}
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_UNSHADED) {
		scene_state.ubo.ss_effects_flags = 0;
	} else if (p_render_data->reflection_probe.is_null() && is_environment(p_render_data->environment)) {
		scene_state.ubo.ssao_ao_affect = environment_get_ssao_ao_channel_affect(p_render_data->environment);
		scene_state.ubo.ssao_light_affect = environment_get_ssao_direct_light_affect(p_render_data->environment);
		uint32_t ss_flags = 0;
		if (p_opaque_render_buffers) {
			ss_flags |= environment_get_ssao_enabled(p_render_data->environment) ? (1 << 0) : 0;
			ss_flags |= environment_get_ssil_enabled(p_render_data->environment) ? (1 << 1) : 0;
			ss_flags |= environment_get_ssr_enabled(p_render_data->environment) ? (1 << 2) : 0;

			if (rd.is_valid()) {
				Ref<RenderBufferDataForwardClustered> rb_data;
				if (rd->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
					rb_data = rd->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
					ss_flags |= (rb_data.is_valid() && !rb_data->ss_effects_data.ssr.half_size) ? (1 << 3) : 0;
				}
			}
		}
		scene_state.ubo.ss_effects_flags = ss_flags;
	} else {
		scene_state.ubo.ss_effects_flags = 0;
	}

	if (uniform_buffer_index >= scene_state.implementation_uniform_buffers.size()) {
		uint32_t from = scene_state.implementation_uniform_buffers.size();
		scene_state.implementation_uniform_buffers.resize(uniform_buffer_index + 1);
		for (uint32_t i = from; i < scene_state.implementation_uniform_buffers.size(); i++) {
			scene_state.implementation_uniform_buffers[i] = RD::get_singleton()->uniform_buffer_create(sizeof(SceneState::UBO));
		}
	}

	RD::get_singleton()->buffer_update(scene_state.implementation_uniform_buffers[uniform_buffer_index], 0, sizeof(SceneState::UBO), &scene_state.ubo);

	return uniform_buffer_index;
}

void RenderForwardClustered::SceneState::grow_instance_buffer(RenderListType p_render_list, uint32_t p_req_element_count, bool p_append) {
	if (p_req_element_count > 0) {
		if (instance_buffer[p_render_list].get_size(0u) < p_req_element_count * sizeof(SceneState::InstanceData)) {
			instance_buffer[p_render_list].uninit();
			uint32_t new_size = Math::nearest_power_of_2_templated(MAX(uint64_t(INSTANCE_DATA_BUFFER_MIN_SIZE), p_req_element_count));
			instance_buffer[p_render_list].set_storage_size(0u, new_size * sizeof(SceneState::InstanceData));
		}

		instance_buffer[p_render_list].prepare_for_map(p_append);
	}
}

void RenderForwardClustered::RenderList::_sort_elements(uint32_t p_from, uint32_t p_count, uint32_t p_mode) {
	if (p_count < 2) {
		return;
	}
	auto prepare = [&](uint32_t) {
		if (p_mode == 0) {
			SortArray<RenderElement, SortByKey> sorter;
			sorter.sort(elements.ptr() + p_from, p_count);
		} else if (p_mode == 1) {
			SortArray<RenderElement, SortByDepth> sorter;
			sorter.sort(elements.ptr() + p_from, p_count);
		} else {
			SortArray<RenderElement, SortByReverseDepthAndPriority> sorter;
			sorter.sort(elements.ptr() + p_from, p_count);
		}
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	WorkerThreadPool::GroupID job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("RasterPassSort");
		(*static_cast<decltype(prepare) *>(p_data))(p_index);
	},
			&prepare, 1, 1, true, SNAME("RasterPassSort"));
	pool->wait_for_group_task_completion(job);
}

void RenderForwardClustered::_fill_instance_payload(RenderList *rl, uint32_t p_from, uint32_t p_count) {
	for (uint32_t i = p_from; i < p_from + p_count; i++) {
		const RenderElement &element = rl->elements[i];
		GeometryInstanceSurfaceDataCache *surface = element.surface;
		GeometryInstanceForwardClustered *inst = surface->owner;

		SceneState::InstanceData instance_data = {};

		if (likely(inst->store_transform_cache)) {
			RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->transform, instance_data.transform);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->prev_transform, instance_data.prev_transform);

			// Split the origin into two components, the float approximation and the missing precision.
			// In the shader we will combine these back together to restore the lost precision.
			RendererRD::MaterialStorage::split_double(inst->origin[0], &instance_data.transform[3], &instance_data.model_precision[0]);
			RendererRD::MaterialStorage::split_double(inst->origin[1], &instance_data.transform[7], &instance_data.model_precision[1]);
			RendererRD::MaterialStorage::split_double(inst->origin[2], &instance_data.transform[11], &instance_data.model_precision[2]);
			RendererRD::MaterialStorage::split_double(inst->prev_origin[0], &instance_data.prev_transform[3], &instance_data.prev_model_precision[0]);
			RendererRD::MaterialStorage::split_double(inst->prev_origin[1], &instance_data.prev_transform[7], &instance_data.prev_model_precision[1]);
			RendererRD::MaterialStorage::split_double(inst->prev_origin[2], &instance_data.prev_transform[11], &instance_data.prev_model_precision[2]);
		} else {
			RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), instance_data.transform);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), instance_data.prev_transform);
			memset(instance_data.model_precision, 0, sizeof(instance_data.model_precision));
			memset(instance_data.prev_model_precision, 0, sizeof(instance_data.prev_model_precision));
			if (inst->scene_data->base_type == RSE::INSTANCE_PARTICLES) {
				const double *simulation_origin = RendererRD::ParticlesStorage::get_singleton()->particles_get_simulation_origin(inst->scene_data->base);
				if (simulation_origin) {
					for (int axis = 0; axis < 3; axis++) {
						RendererRD::MaterialStorage::split_double(simulation_origin[axis], &instance_data.transform[axis * 4 + 3], &instance_data.model_precision[axis]);
						instance_data.prev_transform[axis * 4 + 3] = instance_data.transform[axis * 4 + 3];
						instance_data.prev_model_precision[axis] = instance_data.model_precision[axis];
					}
				}
			}
		}

		instance_data.flags = element.flags;
		instance_data.gi_offset = element.gi_offset;
		instance_data.layer_mask = inst->scene_data->layer_mask;
		instance_data.rtxdi_material_flags = surface->rtxdi_material_flags;
		if (inst->rt_procedural != nullptr) {
			instance_data.rtxdi_material_flags |= GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_UNSUPPORTED | GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_DEFORMED;
		}
		if (((instance_data.flags & INSTANCE_DATA_FLAGS_FADE_MASK) >> INSTANCE_DATA_FLAGS_FADE_SHIFT) != 255) {
			instance_data.rtxdi_material_flags |= GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_UNSUPPORTED;
		}
		memset(instance_data.rtxdi_padding, 0, sizeof(instance_data.rtxdi_padding));
		instance_data.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
		instance_data.set_lightmap_uv_scale(inst->lightmap_uv_scale);

		AABB surface_aabb = AABB(Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0));
		uint64_t format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(surface->surface);
		Vector4 uv_scale = Vector4(0.0, 0.0, 0.0, 0.0);

		if (format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
			surface_aabb = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_aabb(surface->surface);
			uv_scale = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_uv_scale(surface->surface);
		}

		instance_data.set_compressed_aabb(surface_aabb);
		instance_data.set_uv_scale(uv_scale);

		rl->instance_data[i] = instance_data;
	}
}

void RenderForwardClustered::_fill_instance_runs(RenderList *rl, uint32_t p_from, uint32_t p_count, int *p_render_info) {
	if (p_render_info) {
		p_render_info[RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += p_count;
	}
	uint32_t repeats = 0;
	const RenderElement *prev_element = nullptr;
	for (uint32_t i = 0; i < p_count; i++) {
		const RenderElement &element = rl->elements[i + p_from];
		const auto *inst = element.surface->owner;
		const SceneState::InstanceData &instance_data = rl->instance_data[i + p_from];

		const bool cant_repeat = instance_data.flags & INSTANCE_DATA_FLAG_MULTIMESH || inst->scene_data->mesh_instance.is_valid();

		if (prev_element != nullptr && !cant_repeat && prev_element->sort.sort_key1 == element.sort.sort_key1 && prev_element->sort.sort_key2 == element.sort.sort_key2 && inst->scene_data->mirror == prev_element->surface->owner->scene_data->mirror && repeats < RenderElementInfo::MAX_REPEATS) {
			//this element is the same as the previous one, count repeats to draw it using instancing
			repeats++;
		} else {
			if (repeats > 0) {
				for (uint32_t j = 1; j <= repeats; j++) {
					rl->element_info[p_from + i - j].repeat = j;
				}
			}
			repeats = 1;
			if (p_render_info) {
				p_render_info[RSE::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME]++;
			}
		}

		RenderElementInfo &element_info = rl->element_info[p_from + i];

		element_info.value = uint32_t(element.sort.sort_key1 & 0xFFF);

		if (cant_repeat) {
			prev_element = nullptr;
		} else {
			prev_element = &element;
		}
	}

	if (repeats > 0) {
		for (uint32_t j = 1; j <= repeats; j++) {
			rl->element_info[p_from + p_count - j].repeat = j;
		}
	}
}

void RenderForwardClustered::_fill_instance_data(RenderListType p_render_list, int *p_render_info, uint32_t p_offset, int32_t p_max_elements, bool p_update_buffer) {
	RenderList *rl = &render_list[p_render_list];
	uint32_t element_total = p_max_elements >= 0 ? uint32_t(p_max_elements) : rl->elements.size();

	rl->element_info.resize(p_offset + element_total);

	rl->instance_data.resize(p_offset + element_total);
	auto prepare = [&](uint32_t p_batch) {
		const uint32_t from = p_batch * 256;
		_fill_instance_payload(rl, p_offset + from, MIN(256u, element_total - from));
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	WorkerThreadPool::GroupID payload_job = pool->add_native_group_task([](void *p_data, uint32_t p_batch) {
		GodotProfileZone("RasterInstancePayload");
		(*static_cast<decltype(prepare) *>(p_data))(p_batch);
	},
			&prepare, (element_total + 255) / 256, -1, true, SNAME("RasterInstancePayload"));
	pool->wait_for_group_task_completion(payload_job);
	auto finalize = [&](uint32_t) {
		_fill_instance_runs(rl, p_offset, element_total, p_render_info);
	};
	WorkerThreadPool::GroupID finalize_job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("RasterInstanceRuns");
		(*static_cast<decltype(finalize) *>(p_data))(p_index);
	},
			&finalize, 1, 1, true, SNAME("RasterInstanceRuns"));
	pool->wait_for_group_task_completion(finalize_job);
	if (p_update_buffer && !rl->instance_data.is_empty()) {
		scene_state.grow_instance_buffer(p_render_list, rl->instance_data.size(), false);
		void *destination = scene_state.instance_buffer[p_render_list].map_raw_for_upload(0u);
		memcpy(destination, rl->instance_data.ptr(), rl->instance_data.size() * sizeof(SceneState::InstanceData));
		RenderingDevice::get_singleton()->buffer_flush(scene_state.instance_buffer[p_render_list]._get(0u));
	}
}

_FORCE_INLINE_ static uint32_t _indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}
void RenderForwardClustered::_prepare_render_list_chunk(uint32_t p_batch, RenderListPreparation *p_preparation) {
	GodotProfileZone("RasterPassLists");
	RenderList *rl = p_preparation->list;
	auto *mesh_storage = RendererRD::MeshStorage::get_singleton();
	Plane near_plane(-p_preparation->camera_transform.basis.get_column(Vector3::AXIS_Z), Vector3());
	near_plane.d += p_preparation->projection.get_z_near();
	float z_max = p_preparation->projection.get_z_far() - p_preparation->projection.get_z_near();

	auto &batch = p_preparation->batches[p_batch];
	if (p_preparation->profile) {
		batch.worker = Thread::get_caller_id();
		batch.begin_usec = OS::get_singleton()->get_ticks_usec();
	}
	uint32_t lightmap_captures_used = 0;
	const uint32_t from = p_batch * 256;
	const uint32_t to = MIN(from + 256, p_preparation->instances->size());
	for (uint32_t i = from; i < to; i++) {
		float depth = 0.0f;
		uint32_t gi_offset = UINT32_MAX;
		GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>((*p_preparation->instances)[i]);
		if (rl->last_micro_pass && inst->micro_geometry_raster_only) {
			continue;
		}

		const Transform3D relative = inst->get_camera_relative_transform(p_preparation->camera_origin);
		const AABB relative_aabb = relative.xform(inst->scene_data->aabb);
		Vector3 center = relative.origin;
		if (p_preparation->orthogonal) {
			if (inst->scene_data->use_aabb_center) {
				center = relative_aabb.get_support(-near_plane.normal);
			}
			depth = near_plane.distance_to(center) - inst->scene_data->sorting_offset;
		} else {
			if (inst->scene_data->use_aabb_center) {
				center = relative_aabb.get_center();
			}
			depth = center.length() - inst->scene_data->sorting_offset;
		}
		uint32_t depth_layer = CLAMP(int(depth * 16 / z_max), 0, 15);

		uint32_t flags = inst->base_flags; //fill flags if appropriate

		if (inst->non_uniform_scale) {
			flags |= INSTANCE_DATA_FLAGS_NON_UNIFORM_SCALE;
		}
		float fade_alpha = 1.0;

		if (inst->fade_near || inst->fade_far) {
			float fade_dist = relative_aabb.get_center().length();
			// Use `smoothstep()` to make opacity changes more gradual and less noticeable to the player.
			if (inst->fade_far && fade_dist > inst->fade_far_begin) {
				fade_alpha = Math::smoothstep(0.0f, 1.0f, 1.0f - (fade_dist - inst->fade_far_begin) / (inst->fade_far_end - inst->fade_far_begin));
			} else if (inst->fade_near && fade_dist < inst->fade_near_end) {
				fade_alpha = Math::smoothstep(0.0f, 1.0f, (fade_dist - inst->fade_near_begin) / (inst->fade_near_end - inst->fade_near_begin));
			}
		}

		fade_alpha *= inst->force_alpha * inst->parent_fade_alpha;

		flags = (flags & ~INSTANCE_DATA_FLAGS_FADE_MASK) | (uint32_t(fade_alpha * 255.0) << INSTANCE_DATA_FLAGS_FADE_SHIFT);

		if (p_preparation->list_type == RENDER_LIST_OPAQUE) {
			// Detect if object moved since last frame.
			if (p_preparation->pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || p_preparation->pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI || p_preparation->pass_mode == PASS_MODE_RTXDI_SURFACE) {
				bool transform_changed = inst->transform_status == GeometryInstanceForwardClustered::TransformStatus::MOVED;
				bool has_mesh_instance = inst->scene_data->mesh_instance.is_valid();
				bool uses_particles = inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES;
				bool is_multimesh_with_motion = !uses_particles && (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) && mesh_storage->_multimesh_uses_motion_vectors_offsets(inst->scene_data->base);
				bool is_dynamic = transform_changed || has_mesh_instance || uses_particles || is_multimesh_with_motion;
				if (is_dynamic) {
					flags |= INSTANCE_DATA_FLAGS_DYNAMIC;
				}
			}

			// Alpha-only (RT path): skip instances with no transparent/fading
			// surfaces; opaque geometry is in the TLAS. Uses rt_pass_flags so
			// `#if defined(RT)` overrides are honored.
			if (p_preparation->alpha_only && fade_alpha >= FADE_ALPHA_PASS_THRESHOLD) {
				bool has_alpha_surface = false;
				const GeometryInstanceSurfaceDataCache *s = inst->surface_caches;
				while (s) {
					if (s->rt_pass_flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) {
						has_alpha_surface = true;
						break;
					}
					s = s->next;
				}
				if (!has_alpha_surface) {
					continue;
				}
			}

			// Setup GI.
			if (inst->lightmap_instance.is_valid()) {
				// find index of the lightmap_instance of the instance being rendered
				int32_t lightmap_cull_index = -1;
				for (uint32_t j = 0; j < p_preparation->lightmaps_used; j++) {
					if (p_preparation->lightmap_ids[j] == inst->lightmap_instance) {
						lightmap_cull_index = j;
						break;
					}
				}
				if (lightmap_cull_index >= 0) {
					gi_offset = inst->lightmap_slice_index << 16;
					gi_offset |= lightmap_cull_index;
					flags |= INSTANCE_DATA_FLAG_USE_LIGHTMAP;
					if (p_preparation->lightmap_has_sh[lightmap_cull_index]) {
						flags |= INSTANCE_DATA_FLAG_USE_SH_LIGHTMAP;
					}
				} else {
					gi_offset = 0xFFFFFFFF;
				}

			} else if (inst->lightmap_sh) {
				if (lightmap_captures_used < p_preparation->max_lightmap_captures) {
					const Color *src_capture = inst->lightmap_sh->sh;
					LightmapCaptureData lcd;
					for (int j = 0; j < 9; j++) {
						lcd.sh[j * 4 + 0] = src_capture[j].r;
						lcd.sh[j * 4 + 1] = src_capture[j].g;
						lcd.sh[j * 4 + 2] = src_capture[j].b;
						lcd.sh[j * 4 + 3] = src_capture[j].a;
					}
					batch.captures.push_back(lcd);
					flags |= INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE;
					gi_offset = lightmap_captures_used;
					lightmap_captures_used++;
				}

			} else {
				if (p_preparation->using_opaque_gi) {
					flags |= INSTANCE_DATA_FLAG_USE_GI_BUFFERS;
				}

				if (inst->voxel_gi_instances[0].is_valid()) {
					uint32_t probe0_index = 0xFFFF;
					uint32_t probe1_index = 0xFFFF;

					for (uint32_t j = 0; j < p_preparation->voxelgis_used; j++) {
						if (p_preparation->voxelgi_ids[j] == inst->voxel_gi_instances[0]) {
							probe0_index = j;
						} else if (p_preparation->voxelgi_ids[j] == inst->voxel_gi_instances[1]) {
							probe1_index = j;
						}
					}

					if (probe0_index == 0xFFFF && probe1_index != 0xFFFF) {
						//0 must always exist if a probe exists
						SWAP(probe0_index, probe1_index);
					}

					gi_offset = probe0_index | (probe1_index << 16);
					flags |= INSTANCE_DATA_FLAG_USE_VOXEL_GI;
				} else {
					if (p_preparation->using_sdfgi && inst->can_sdfgi) {
						flags |= INSTANCE_DATA_FLAG_USE_SDFGI;
					}
					gi_offset = 0xFFFFFFFF;
				}
			}
		}

		GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;

		float lod_distance = 0.0;

		if (p_preparation->orthogonal) {
			lod_distance = 1.0;
		} else {
			const AABB lod_aabb = inst->get_camera_relative_transform(p_preparation->main_camera_origin).xform(inst->scene_data->aabb);
			Vector3 surface_distance = Vector3().max(lod_aabb.position).max(-lod_aabb.get_end());

			lod_distance = surface_distance.length();
		}

		uint32_t surface_ordinal = 0;
		while (surf) {
			RenderElement element;
			element.surface = surf;
			element.sort = surf->sort;
			element.ordinal = (uint64_t(i) << 32) | surface_ordinal++;
			element.depth = depth;
			element.flags = flags;
			element.gi_offset = gi_offset;
			element.sort.depth_layer = depth_layer;
			if (rl->last_micro_pass && _micro_geometry_eligible(surf, p_preparation->pass_mode)) {
				surf = surf->next;
				continue;
			}
			element.sort.uses_forward_gi = 0;
			element.sort.uses_lightmap = 0;

			// LOD
			if (p_preparation->pass_mode != PASS_MODE_RTXDI_SURFACE && p_preparation->screen_mesh_lod_threshold > 0.0 && mesh_storage->mesh_surface_has_lod(surf->surface)) {
				uint32_t indices = 0;
				element.sort.lod_index = mesh_storage->mesh_surface_get_lod(surf->surface, inst->lod_model_scale * inst->scene_data->lod_bias, lod_distance * p_preparation->lod_distance_multiplier, p_preparation->screen_mesh_lod_threshold, indices);
				if (p_preparation->render_info && !p_preparation->alpha_only) {
					indices = _indices_to_primitives(surf->primitive, indices);
					if (p_preparation->list_type == RENDER_LIST_OPAQUE) { //opaque
						batch.primitives += indices;
					} else if (p_preparation->list_type == RENDER_LIST_SECONDARY) { //shadow
						batch.primitives += indices;
					}
				}
			} else {
				element.sort.lod_index = 0;
				if (p_preparation->render_info && !p_preparation->alpha_only) {
					// This does not include primitives rendered via indirect draw calls.
					uint32_t to_draw = mesh_storage->mesh_surface_get_vertices_drawn_count(surf->surface);
					to_draw = _indices_to_primitives(surf->primitive, to_draw);
					to_draw *= inst->instance_count;
					if (p_preparation->list_type == RENDER_LIST_OPAQUE) { //opaque
						batch.primitives += to_draw;
					} else if (p_preparation->list_type == RENDER_LIST_SECONDARY) { //shadow
						batch.primitives += to_draw;
					}
				}
			}

			// ADD Element
			if (p_preparation->pass_mode == PASS_MODE_RTXDI_SURFACE) {
				batch.elements.push_back(element);
			} else if (p_preparation->pass_mode == PASS_MODE_SHADOW || p_preparation->pass_mode == PASS_MODE_SHADOW_DP) {
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW) {
					batch.elements.push_back(element);
				}
			} else if (p_preparation->pass_mode == PASS_MODE_DEPTH_MATERIAL) {
				if (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE | GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
					batch.elements.push_back(element);
				}
			} else {
				if (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE)) {
					batch.elements.push_back(element);
				}
			}

			surf = surf->next;
		}
	}

	if (p_preparation->profile) {
		batch.end_usec = OS::get_singleton()->get_ticks_usec();
	}
}

RenderForwardClustered::RenderListPreparation *RenderForwardClustered::_begin_render_list(RenderListType p_render_list, const RenderDataRD *p_render_data, PassMode p_pass_mode, bool p_using_sdfgi, bool p_using_opaque_gi, bool p_append, bool p_alpha_only, RenderList *p_target) {
	if (p_render_list == RENDER_LIST_OPAQUE) {
		scene_state.used_sss = false;
		scene_state.used_screen_texture = false;
		scene_state.used_normal_texture = false;
		scene_state.used_depth_texture = false;
		scene_state.used_lightmap = false;
		scene_state.used_opaque_stencil = false;
	}
	RenderList *rl = p_target ? p_target : &render_list[p_render_list];
	if (!p_target) {
		_update_dirty_geometry_instances();
	}

	if (!p_append) {
		rl->clear();
		if (p_render_list == RENDER_LIST_OPAQUE) {
			// Opaque fills motion and alpha lists.
			render_list[RENDER_LIST_MOTION].clear();
			render_list[RENDER_LIST_ALPHA].clear();
		}
	}

	//fill list
	RENDER_TIMESTAMP("Microgeometry Raster Prepare");
	rl->last_micro_pass = _prepare_micro_geometry(p_render_data, p_pass_mode);
	RENDER_TIMESTAMP("Raster Render List Fill");
	if (rl->last_micro_pass) {
		rl->micro_passes.push_back(rl->last_micro_pass);
	}

	RenderListPreparation *preparation = memnew(RenderListPreparation);
	preparation->list = rl;
	preparation->instances = p_render_data->instances;
	preparation->render_info = p_render_data->render_info;
	preparation->camera_transform = p_render_data->scene_data->cam_transform;
	for (int axis = 0; axis < 3; axis++) {
		preparation->camera_origin[axis] = p_render_data->scene_data->cam_origin[axis];
		preparation->main_camera_origin[axis] = p_render_data->scene_data->main_cam_origin[axis];
	}
	preparation->projection = p_render_data->scene_data->cam_projection;
	preparation->orthogonal = p_render_data->scene_data->cam_orthogonal;
	preparation->lod_distance_multiplier = p_render_data->scene_data->lod_distance_multiplier;
	preparation->screen_mesh_lod_threshold = p_render_data->scene_data->screen_mesh_lod_threshold;
	preparation->list_type = p_render_list;
	preparation->pass_mode = p_pass_mode;
	preparation->using_sdfgi = p_using_sdfgi;
	preparation->using_opaque_gi = p_using_opaque_gi;
	preparation->alpha_only = p_alpha_only;
	preparation->lightmaps_used = scene_state.lightmaps_used;
	preparation->voxelgis_used = scene_state.voxelgis_used;
	preparation->max_lightmap_captures = scene_state.max_lightmap_captures;
	for (uint32_t index = 0; index < preparation->lightmaps_used; index++) {
		preparation->lightmap_ids[index] = scene_state.lightmap_ids[index];
		preparation->lightmap_has_sh[index] = scene_state.lightmap_has_sh[index];
	}
	for (uint32_t index = 0; index < preparation->voxelgis_used; index++) {
		preparation->voxelgi_ids[index] = scene_state.voxelgi_ids[index];
	}
	preparation->frame = RSG::rasterizer->get_frame_number();
	preparation->profile = RSG::utilities->capturing_timestamps && preparation->frame % 120 == 0;
	preparation->pass_index = shadow_preparations.size();
	preparation->batches.resize((preparation->instances->size() + 255) / 256);
	if (preparation->profile) {
		preparation->coordinator = Thread::get_caller_id();
		preparation->queued_usec = OS::get_singleton()->get_ticks_usec();
	}
	preparation->job = WorkerThreadPool::get_singleton()->add_template_group_task(this, &RenderForwardClustered::_prepare_render_list_chunk, preparation, preparation->batches.size(), -1, true, SNAME("RasterPassLists"));
	return preparation;
}

void RenderForwardClustered::_finish_render_list(RenderListPreparation *p_preparation) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	{
		GodotProfileZone("RasterPassJoin");
		pool->wait_for_group_task_completion(p_preparation->job);
	}
	if (p_preparation->profile) {
		const uint64_t joined = OS::get_singleton()->get_ticks_usec();
		String rows;
		for (uint32_t index = 0; index < p_preparation->batches.size(); index++) {
			const auto &batch = p_preparation->batches[index];
			rows += vformat("RenderPrep stage=RasterPassLists frame=%d pass=%d mode=%d chunk=%d coordinator=%d queued_usec=%d joined_usec=%d worker=%d begin_usec=%d end_usec=%d work=%d", p_preparation->frame, p_preparation->pass_index, p_preparation->pass_mode, index, p_preparation->coordinator, p_preparation->queued_usec, joined, batch.worker, batch.begin_usec, batch.end_usec, MIN(256u, p_preparation->instances->size() - index * 256)) + "\n";
		}
		print_line(rows);
	}
	LocalVector<LightmapCaptureData> captures;
	int primitives = 0;
	auto merge = [&](uint32_t) {
		for (auto &batch : p_preparation->batches) {
			const uint32_t capture_offset = captures.size();
			for (const LightmapCaptureData &capture : batch.captures) {
				if (captures.size() < p_preparation->max_lightmap_captures) {
					captures.push_back(capture);
				}
			}
			for (RenderElement &element : batch.elements) {
				if (element.flags & INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE) {
					element.gi_offset += capture_offset;
					if (element.gi_offset >= p_preparation->max_lightmap_captures) {
						element.flags &= ~INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE;
						element.gi_offset = UINT32_MAX;
					}
				}
				p_preparation->list->add_element(element);
			}
			primitives += batch.primitives;
		}
	};
	auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("RasterPassMerge");
		(*static_cast<decltype(merge) *>(p_data))(p_index);
	},
			&merge, 1, 1, true, SNAME("RasterPassMerge"));
	pool->wait_for_group_task_completion(job);
	if (p_preparation->render_info && (p_preparation->list_type == RENDER_LIST_OPAQUE || p_preparation->list_type == RENDER_LIST_SECONDARY)) {
		const int kind = p_preparation->list_type == RENDER_LIST_OPAQUE ? RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE : RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW;
		p_preparation->render_info->info[kind][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += primitives;
	}
	if (p_preparation->list_type == RENDER_LIST_OPAQUE && !captures.is_empty()) {
		RD::get_singleton()->buffer_update(scene_state.lightmap_capture_buffer, 0, sizeof(LightmapCaptureData) * captures.size(), captures.ptr());
	}
	memdelete(p_preparation);
}

void RenderForwardClustered::_fill_render_list(RenderListType p_render_list, const RenderDataRD *p_render_data, PassMode p_pass_mode, bool p_using_sdfgi, bool p_using_opaque_gi, bool p_append, bool p_alpha_only) {
	_finish_render_list(_begin_render_list(p_render_list, p_render_data, p_pass_mode, p_using_sdfgi, p_using_opaque_gi, p_append, p_alpha_only));
}

void RenderForwardClustered::_setup_voxelgis(const PagedArray<RID> &p_voxelgis) {
	scene_state.voxelgis_used = MIN(p_voxelgis.size(), uint32_t(MAX_VOXEL_GI_INSTANCESS));
	for (uint32_t i = 0; i < scene_state.voxelgis_used; i++) {
		scene_state.voxelgi_ids[i] = p_voxelgis[i];
	}
}

void RenderForwardClustered::_setup_lightmaps(const RenderDataRD *p_render_data, const PagedArray<RID> &p_lightmaps, const Transform3D &p_cam_transform) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	scene_state.lightmaps_used = 0;
	for (int i = 0; i < (int)p_lightmaps.size(); i++) {
		if (i >= (int)scene_state.max_lightmaps) {
			break;
		}

		RID lightmap = light_storage->lightmap_instance_get_lightmap(p_lightmaps[i]);

		// Transform (for directional lightmaps).
		Basis to_lm = light_storage->lightmap_instance_get_transform(p_lightmaps[i]).basis.inverse() * p_cam_transform.basis;
		to_lm = to_lm.inverse().transposed(); //will transform normals
		RendererRD::MaterialStorage::store_transform_3x3(to_lm, scene_state.lightmaps[i].normal_xform);

		// Light texture size.
		Vector2i lightmap_size = light_storage->lightmap_get_light_texture_size(lightmap);
		scene_state.lightmaps[i].texture_size[0] = lightmap_size[0];
		scene_state.lightmaps[i].texture_size[1] = lightmap_size[1];

		// Exposure.
		scene_state.lightmaps[i].exposure_normalization = 1.0;
		scene_state.lightmaps[i].flags = light_storage->lightmap_get_shadowmask_mode(lightmap);
		if (p_render_data->camera_attributes.is_valid()) {
			float baked_exposure = light_storage->lightmap_get_baked_exposure_normalization(lightmap);
			float enf = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
			scene_state.lightmaps[i].exposure_normalization = enf / baked_exposure;
		}

		scene_state.lightmap_ids[i] = p_lightmaps[i];
		scene_state.lightmap_has_sh[i] = light_storage->lightmap_uses_spherical_harmonics(lightmap);

		scene_state.lightmaps_used++;
	}
	if (scene_state.lightmaps_used > 0) {
		RD::get_singleton()->buffer_update(scene_state.lightmap_buffer, 0, sizeof(LightmapData) * scene_state.lightmaps_used, scene_state.lightmaps);
	}
}

/* SDFGI */

void RenderForwardClustered::_debug_draw_cluster(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers.is_valid() && current_cluster_builder != nullptr) {
		RSE::ViewportDebugDraw dd = get_debug_draw_mode();

		if (dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES) {
			ClusterBuilderRD::ElementType elem_type = ClusterBuilderRD::ELEMENT_TYPE_MAX;
			switch (dd) {
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_OMNI_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_SPOT_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_DECAL;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_REFLECTION_PROBE;
					break;
				default: {
				}
			}
			current_cluster_builder->debug(elem_type);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// FOG SHADER

void RenderForwardClustered::_update_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const Projection &p_cam_projection, const Transform3D &p_cam_transform, const Transform3D &p_prev_cam_inv_transform, RID p_shadow_atlas, int p_directional_light_count, bool p_use_directional_shadows, int p_positional_light_count, int p_voxel_gi_count, float p_camera_exposure, const PagedArray<RID> &p_fog_volumes, const double *p_cam_origin, const double *p_prev_cam_origin) {
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	Ref<RendererRD::GI::SDFGI> sdfgi;
	if (p_render_buffers->has_custom_data(RB_SCOPE_SDFGI)) {
		sdfgi = p_render_buffers->get_custom_data(RB_SCOPE_SDFGI);
	}

	Size2i size = p_render_buffers->get_internal_size();
	float ratio = float(size.x) / float((size.x + size.y) / 2);
	uint32_t target_width = uint32_t(float(get_volumetric_fog_size()) * ratio);
	uint32_t target_height = uint32_t(float(get_volumetric_fog_size()) / ratio);

	if (p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_buffers->get_custom_data(RB_SCOPE_FOG);
		//validate
		if (p_environment.is_null() || !environment_get_volumetric_fog_enabled(p_environment) || fog->width != target_width || fog->height != target_height || fog->depth != get_volumetric_fog_depth()) {
			p_render_buffers->set_custom_data(RB_SCOPE_FOG, Ref<RenderBufferCustomDataRD>());
		}
	}

	if (p_environment.is_null() || !environment_get_volumetric_fog_enabled(p_environment)) {
		//no reason to enable or update, bye
		return;
	}

	if (p_environment.is_valid() && environment_get_volumetric_fog_enabled(p_environment) && !p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		//required volumetric fog but not existing, create
		Ref<RendererRD::Fog::VolumetricFog> fog;

		fog.instantiate();
		fog->init(Vector3i(target_width, target_height, get_volumetric_fog_depth()), sky.sky_shader.default_shader_rd);

		p_render_buffers->set_custom_data(RB_SCOPE_FOG, fog);
	}

	if (p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_buffers->get_custom_data(RB_SCOPE_FOG);

		RendererRD::Fog::VolumetricFogSettings settings;
		settings.sky_energy = environment_get_bg_energy_multiplier(p_environment) * environment_get_bg_intensity(p_environment) * p_camera_exposure;
		settings.rb_size = size;
		settings.time = time;
		settings.is_using_radiance_octmap_array = is_using_radiance_octmap_array();
		settings.max_cluster_elements = RendererRD::LightStorage::get_singleton()->get_max_cluster_elements();
		settings.volumetric_fog_filter_active = get_volumetric_fog_filter_active();

		settings.shadow_sampler = shadow_sampler;
		settings.shadow_atlas_depth = RendererRD::LightStorage::get_singleton()->owns_shadow_atlas(p_shadow_atlas) ? RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_shadow_atlas) : RID();
		settings.voxel_gi_buffer = render_buffers_get_default_voxel_gi_buffer();
		settings.omni_light_buffer = RendererRD::LightStorage::get_singleton()->get_omni_light_buffer();
		settings.spot_light_buffer = RendererRD::LightStorage::get_singleton()->get_spot_light_buffer();
		settings.area_light_buffer = RendererRD::LightStorage::get_singleton()->get_area_light_buffer();
		settings.area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
		settings.directional_shadow_depth = RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture();
		settings.directional_light_buffer = RendererRD::LightStorage::get_singleton()->get_directional_light_buffer();

		settings.vfog = fog;
		settings.cluster_builder = rb_data->cluster_builder;
		settings.sdfgi = sdfgi;
		settings.env = p_environment;
		settings.sky = &sky;
		settings.gi = &gi;

		RendererRD::Fog::get_singleton()->volumetric_fog_update(settings, p_cam_projection, p_cam_transform, p_prev_cam_inv_transform, p_shadow_atlas, p_directional_light_count, p_use_directional_shadows, p_positional_light_count, p_voxel_gi_count, p_fog_volumes, p_cam_origin, p_prev_cam_origin);
	}
}

/* Lighting */

void RenderForwardClustered::setup_added_reflection_probe(const Transform3D &p_transform, const Vector3 &p_half_size, const double *p_origin) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_REFLECTION_PROBE, p_transform, p_half_size, p_origin);
	}
}

void RenderForwardClustered::setup_added_light(const RSE::LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size, const double *p_origin) {
	if (current_cluster_builder != nullptr) {
		ClusterBuilderRD::LightType type;
		if (p_type == RSE::LIGHT_SPOT) {
			type = ClusterBuilderRD::LIGHT_TYPE_SPOT;
		} else if (p_type == RSE::LIGHT_OMNI) {
			type = ClusterBuilderRD::LIGHT_TYPE_OMNI;
		} else {
			type = ClusterBuilderRD::LIGHT_TYPE_AREA;
		}

		current_cluster_builder->add_light(type, p_transform, p_radius, p_spot_aperture, p_area_size, p_origin);
	}
}

void RenderForwardClustered::setup_added_decal(const Transform3D &p_transform, const Vector3 &p_half_size, const double *p_origin) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_DECAL, p_transform, p_half_size, p_origin);
	}
}

/* Render scene */

void RenderForwardClustered::_process_ssao(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_environment.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSAO");

	RendererRD::SSEffects::SSAOSettings settings;
	settings.radius = environment_get_ssao_radius(p_environment);
	settings.intensity = environment_get_ssao_intensity(p_environment);
	settings.power = environment_get_ssao_power(p_environment);
	settings.detail = environment_get_ssao_detail(p_environment);
	settings.horizon = environment_get_ssao_horizon(p_environment);
	settings.sharpness = environment_get_ssao_sharpness(p_environment);
	settings.full_screen_size = p_render_buffers->get_internal_size();

	ss_effects->ssao_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssao, settings);

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		ss_effects->generate_ssao(p_render_buffers, rb_data->ss_effects_data.ssao, v, p_normal_buffers[v], p_projections[v], settings);
	}
}

void RenderForwardClustered::_process_ssil(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections, const Transform3D &p_transform) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_environment.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSIL");

	RendererRD::SSEffects::SSILSettings settings;
	settings.radius = environment_get_ssil_radius(p_environment);
	settings.intensity = environment_get_ssil_intensity(p_environment);
	settings.sharpness = environment_get_ssil_sharpness(p_environment);
	settings.normal_rejection = environment_get_ssil_normal_rejection(p_environment);
	settings.full_screen_size = p_render_buffers->get_internal_size();

	ss_effects->ssil_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssil, settings);

	Transform3D transform = p_transform;
	transform.set_origin(Vector3(0.0, 0.0, 0.0));

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		Projection correction;
		correction.set_depth_correction(true);
		Projection projection = correction * p_projections[v];
		Projection last_frame_projection = rb_data->ss_effects_data.ssil_last_frame_projections[v] * Projection(rb_data->ss_effects_data.ssil_last_frame_transform.affine_inverse()) * Projection(transform) * projection.inverse();

		ss_effects->screen_space_indirect_lighting(p_render_buffers, rb_data->ss_effects_data.ssil, v, p_normal_buffers[v], p_projections[v], last_frame_projection, settings);

		rb_data->ss_effects_data.ssil_last_frame_projections[v] = projection;
	}
	rb_data->ss_effects_data.ssil_last_frame_transform = transform;
}

void RenderForwardClustered::_process_ssr(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_slices, const Projection *p_projections, const Vector3 *p_eye_offsets, const Transform3D &p_transform) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSR");

	ss_effects->ssr_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssr, p_render_buffers->get_base_data_format());

	Projection reprojections[RendererSceneRender::MAX_RENDER_VIEWS];

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		Projection correction;
		correction.set_depth_correction(true);

		Projection projection = correction * p_projections[v];
		reprojections[v] = rb_data->ss_effects_data.ssr_last_frame_projections[v] * Projection(rb_data->ss_effects_data.ssr_last_frame_transform.affine_inverse()) * Projection(p_transform) * projection.inverse();

		rb_data->ss_effects_data.ssr_last_frame_projections[v] = projection;
	}
	rb_data->ss_effects_data.ssr_last_frame_transform = p_transform;

	ss_effects->screen_space_reflection(p_render_buffers, rb_data->ss_effects_data.ssr, p_normal_slices, environment_get_ssr_max_steps(p_environment), environment_get_ssr_fade_in(p_environment), environment_get_ssr_fade_out(p_environment), environment_get_ssr_depth_tolerance(p_environment), p_projections, reprojections, p_eye_offsets, *copy_effects);
}

void RenderForwardClustered::_copy_framebuffer_to_ss_effects(Ref<RenderSceneBuffersRD> p_render_buffers, bool p_use_ssil, bool p_use_ssr) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	ss_effects->copy_internal_texture_to_last_frame(p_render_buffers, *copy_effects);
}

uint32_t RenderForwardClustered::_count_directional_lights(const RenderDataRD *p_render_data) {
	// TODO: Cache this? Seems wasteful to recalculate every frame, it changes rarely.
	if (!p_render_data || !p_render_data->lights) {
		return 0;
	}
	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	uint32_t count = 0;
	for (uint32_t li = 0; li < (uint32_t)p_render_data->lights->size(); li++) {
		RID base = ls->light_instance_get_base_light((*p_render_data->lights)[li]);
		if (ls->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
			count++;
		}
	}
	return count;
}

struct RenderForwardClustered::LightClusterPreparation {
	RenderDataRD *render_data;
	RendererRD::LightStorage::LightBufferPreparation lights;
	RendererRD::TextureStorage::DecalBufferPreparation decals;
	WorkerThreadPool::GroupID light_task = -1;
	WorkerThreadPool::GroupID decal_task = -1;

	static void prepare_lights(void *p_data, uint32_t) {
		GodotProfileZone("LightBufferPreparation");
		LightClusterPreparation &preparation = *static_cast<LightClusterPreparation *>(p_data);
		RenderDataRD *data = preparation.render_data;
		RendererRD::LightStorage::get_singleton()->prepare_light_buffers(data, *data->lights, data->scene_data->cam_transform, data->shadow_atlas, true, preparation.lights);
	}
	static void prepare_decals(void *p_data, uint32_t) {
		GodotProfileZone("DecalBufferPreparation");
		LightClusterPreparation &preparation = *static_cast<LightClusterPreparation *>(p_data);
		RendererRD::TextureStorage::get_singleton()->prepare_decal_buffer(*preparation.render_data->decals, preparation.render_data->scene_data->cam_transform, preparation.decals, preparation.render_data->scene_data->cam_origin);
	}
	LightClusterPreparation(RenderDataRD *p_render_data) : render_data(p_render_data) {
		if (render_data->decals->size()) {
			decal_task = WorkerThreadPool::get_singleton()->try_add_native_group_task(prepare_decals, this, 1, 1, true, SNAME("DecalBufferPreparation"));
		}
		if (decal_task < 0) {
			prepare_decals(this, 0);
		}
	}
	void begin_lights() {
		if (render_data->lights->size()) {
			light_task = WorkerThreadPool::get_singleton()->try_add_native_group_task(prepare_lights, this, 1, 1, true, SNAME("LightBufferPreparation"));
		}
		if (light_task < 0) {
			prepare_lights(this, 0);
		}
	}
	void join_lights() {
		if (light_task >= 0) {
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(light_task);
			light_task = -1;
		}
	}
	void join_decals() {
		if (decal_task >= 0) {
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(decal_task);
			decal_task = -1;
		}
	}
	~LightClusterPreparation() {
		join_lights();
		join_decals();
	}
};

void RenderForwardClustered::_render_shadows(RenderDataRD *p_render_data) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;

	RENDER_TIMESTAMP("Setup Shadows");

	Size2i viewport_size = Size2i(1, 1);
	if (rb.is_valid()) {
		viewport_size = rb->get_internal_size();
	}

	p_render_data->cube_shadows.clear();
	p_render_data->shadows.clear();
	p_render_data->directional_shadows.clear();

	float lod_distance_multiplier = p_render_data->scene_data->cam_projection.get_lod_multiplier();
	{
		for (int i = 0; i < p_render_data->render_shadow_count; i++) {
			RID li = p_render_data->render_shadows[i].light;
			RID base = light_storage->light_instance_get_base_light(li);

			if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
				p_render_data->directional_shadows.push_back(i);
			} else if (light_storage->light_get_type(base) == RSE::LIGHT_OMNI && light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
				p_render_data->cube_shadows.push_back(i);
			} else {
				p_render_data->shadows.push_back(i);
			}
		}

		if (p_render_data->cube_shadows.size()) {
			RENDER_TIMESTAMP("Render OmniLight Shadows");
			// Cube shadows are rendered in their own way.
			for (const int &index : p_render_data->cube_shadows) {
				_render_shadow_pass(p_render_data->render_shadows[index].light, p_render_data->shadow_atlas, p_render_data->render_shadows[index].pass, p_render_data->render_shadows[index].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, true, true, true, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform, p_render_data->render_shadows[index].cull_planes, p_render_data->scene_data->cam_origin, p_render_data->render_shadows[index].cull_origin);
			}
		}

		_discard_shadow_preparations();

		if (p_render_data->directional_shadows.size()) {
			//open the pass for directional shadows
			light_storage->update_directional_shadow_atlas();
			RD::get_singleton()->draw_list_begin(light_storage->direction_shadow_get_fb(), RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
			RD::get_singleton()->draw_list_end();
		}
	}

	bool render_shadows = p_render_data->directional_shadows.size() || p_render_data->shadows.size();
	if (render_shadows) {
		RENDER_TIMESTAMP("Render Directional/SpotLight Shadows");
	}

	//prepare shadow rendering
	if (render_shadows) {
		_render_shadow_begin();

		//render directional shadows
		for (uint32_t i = 0; i < p_render_data->directional_shadows.size(); i++) {
			_render_shadow_pass(p_render_data->render_shadows[p_render_data->directional_shadows[i]].light, p_render_data->shadow_atlas, p_render_data->render_shadows[p_render_data->directional_shadows[i]].pass, p_render_data->render_shadows[p_render_data->directional_shadows[i]].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, false, i == p_render_data->directional_shadows.size() - 1, false, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform, p_render_data->render_shadows[p_render_data->directional_shadows[i]].cull_planes, p_render_data->scene_data->cam_origin, p_render_data->render_shadows[p_render_data->directional_shadows[i]].cull_origin);
		}
		//render positional shadows
		for (uint32_t i = 0; i < p_render_data->shadows.size(); i++) {
			_render_shadow_pass(p_render_data->render_shadows[p_render_data->shadows[i]].light, p_render_data->shadow_atlas, p_render_data->render_shadows[p_render_data->shadows[i]].pass, p_render_data->render_shadows[p_render_data->shadows[i]].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, i == 0, i == p_render_data->shadows.size() - 1, true, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform, p_render_data->render_shadows[p_render_data->shadows[i]].cull_planes, p_render_data->scene_data->cam_origin, p_render_data->render_shadows[p_render_data->shadows[i]].cull_origin);
		}

		_render_shadow_process();
	}

	if (render_shadows) {
		_render_shadow_end();
	}
}

void RenderForwardClustered::_pre_opaque_render(RenderDataRD *p_render_data, LightClusterPreparation &p_preparation) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}

	RENDER_TIMESTAMP("Pre Opaque Render");

	uint32_t directional_light_count = 0;
	uint32_t positional_light_count = 0;
	_setup_lights_cluster_decals(p_render_data, p_preparation, directional_light_count, positional_light_count);

	if (rb_data.is_valid()) {
		RENDER_TIMESTAMP("Update Volumetric Fog");
		bool directional_shadows = RendererRD::LightStorage::get_singleton()->has_directional_shadows(directional_light_count);
		_update_volumetric_fog(rb, p_render_data->environment, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform, p_render_data->scene_data->prev_cam_transform.affine_inverse(), p_render_data->shadow_atlas, directional_light_count, directional_shadows, positional_light_count, p_render_data->voxel_gi_count, p_render_data->camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes) : 1.0f, *p_render_data->fog_volumes, p_render_data->scene_data->cam_origin, p_render_data->scene_data->prev_cam_origin);
	}
}

void RenderForwardClustered::_setup_lights_cluster_decals(RenderDataRD *p_render_data, LightClusterPreparation &p_preparation, uint32_t &r_directional_light_count, uint32_t &r_positional_light_count) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	if (current_cluster_builder) {
		// Note: when rendering stereoscopic (multiview) we are using our combined frustum projection to create
		// our cluster data. We use reprojection in the shader to adjust for our left/right eye.
		// This only works as we don't filter our cluster by depth buffer.
		// If we ever make this optimization we should make it optional and only use it in mono.
		// What we win by filtering out a few lights, we loose by having to do the work double for stereo.
		current_cluster_builder->begin(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, !p_render_data->reflection_probe.is_valid(), p_render_data->scene_data->cam_origin);
	}

	p_preparation.join_lights();
	light_storage->publish_light_buffers(p_preparation.lights);
	r_directional_light_count = p_preparation.lights.directional_light_count;
	r_positional_light_count = p_preparation.lights.positional_light_count;
	p_render_data->directional_light_soft_shadows = p_preparation.lights.directional_light_soft_shadows;
	p_preparation.join_decals();
	texture_storage->publish_decal_buffer(p_preparation.decals);

	p_render_data->directional_light_count = r_directional_light_count;

	if (current_cluster_builder) {
		current_cluster_builder->bake_cluster();
	}
}

void RenderForwardClustered::_process_sss(Ref<RenderSceneBuffersRD> p_render_buffers, const Projection &p_camera) {
	ERR_FAIL_COND(p_render_buffers.is_null());

	Size2i internal_size = p_render_buffers->get_internal_size();
	bool can_use_effects = internal_size.x >= 8 && internal_size.y >= 8;

	if (!can_use_effects) {
		//just copy
		return;
	}

	p_render_buffers->allocate_blur_textures();

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		RID internal_texture = p_render_buffers->get_internal_texture(v);
		RID depth_texture = p_render_buffers->get_depth_texture(v);
		ss_effects->sub_surface_scattering(p_render_buffers, internal_texture, depth_texture, p_camera, internal_size);
	}
}

void RenderForwardClustered::_free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	if (raytracing != nullptr) {
		raytracing->free_viewport_state(p_render_buffers);
	}
}

RenderForwardClustered::Scale3DMode RenderForwardClustered::_resolve_scale_3d_mode(Ref<RenderSceneBuffersRD> p_render_buffers) const {
	switch (p_render_buffers->get_scaling_3d_mode()) {
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR2:
			return SCALE_3D_FSR2;
		case RSE::VIEWPORT_SCALING_3D_MODE_DLSS:
			return SCALE_3D_DLSS;
		case RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL:
#ifdef METAL_MFXTEMPORAL_ENABLED
			return SCALE_3D_MFX;
#else
			return SCALE_3D_NONE;
#endif
		default:
			return SCALE_3D_NONE;
	}
}

void RenderForwardClustered::_render_3d_upscaling(const RenderDataRD *p_render_data, Scale3DMode p_scale_type, bool p_using_taa, double p_time_step) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	if (p_scale_type == SCALE_3D_FSR2) {
		rb_data->ensure_fsr2(fsr2_effect);

		RID exposure;
		if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
			exposure = luminance->get_current_luminance_buffer(rb);
		}

		RD::get_singleton()->draw_command_begin_label("FSR2");
		RENDER_TIMESTAMP("FSR2");

		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			real_t fov = p_render_data->scene_data->cam_projection.get_fov();
			real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
			real_t fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);
			Vector2 jitter = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f;
			RendererRD::FSR2Effect::Parameters params;
			params.context = rb_data->get_fsr2_context();
			params.internal_size = rb->get_internal_size();
			params.sharpness = CLAMP(1.0f - (rb->get_fsr_sharpness() / 2.0f), 0.0f, 1.0f);
			params.color = rb->get_internal_texture(v);
			params.depth = rb->get_depth_texture(v);
			params.velocity = rb->get_velocity_buffer(false, v);
			params.reactive = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
			params.exposure = exposure;
			params.output = rb->get_upscaled_texture(v);
			params.z_near = p_render_data->scene_data->z_near;
			params.z_far = p_render_data->scene_data->z_far;
			params.fovy = fovy;
			params.jitter = jitter;
			params.delta_time = float(p_time_step);
			params.reset_accumulation = false; // FIXME: The engine does not provide a way to reset the accumulation.

			Projection correction;
			correction.set_depth_correction(true, true, false);

			const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;
			const Projection &cur_proj = p_render_data->scene_data->cam_projection;
			const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;
			const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;
			params.reprojection = (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform * (correction * cur_proj).inverse();

			rb->set_upscaler_ready(true);
			fsr2_effect->upscale(params);
		}

		RD::get_singleton()->draw_command_end_label();
	} else if (p_scale_type == SCALE_3D_DLSS) {
		RENDER_TIMESTAMP("DLSS");
		RTViewportState *rt_state = raytracing->_get_viewport_state(p_render_data);
		const bool ray_reconstruction = rt_state->settings.raytracing_denoiser == RSE::RAYTRACING_DENOISER_DLSS_RR;
		rb->set_upscaler_ready(false);
		ERR_FAIL_COND_MSG(!dlss_effect->is_available(ray_reconstruction), ray_reconstruction ? "DLSS Ray Reconstruction is unavailable on this device or its runtime is missing. SR is not substituted." : "DLSS Super Resolution is unavailable on this device or its runtime is missing.");
		ERR_FAIL_COND_MSG(ray_reconstruction && !rb_data->nrd_context, "DLSS Ray Reconstruction requires prepared camera guides.");
		rb_data->ensure_dlss(dlss_effect, ray_reconstruction);

		RID exposure;
		if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
			exposure = luminance->get_current_luminance_buffer(rb);
		}

		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			real_t fov = p_render_data->scene_data->cam_projection.get_fov();
			real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
			real_t fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);
			Vector2 jitter = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f;
			RendererRD::DLSSContext::Parameters params;
			params.context = rb_data->get_dlss_context();
			params.internal_size = rb->get_internal_size();
			params.sharpness = CLAMP((rb->get_fsr_sharpness() / 2.0f), 0.0f, 1.0f);
			params.color = rb->get_internal_texture(v);
			params.depth = rb->get_depth_texture(v);
			params.velocity = rb->get_velocity_buffer(false, v);
			params.reactive = rb->get_internal_texture_reactive(v);
			params.exposure = exposure;
			params.output = rb->get_upscaled_texture(v);
			params.dlss_g = rb->get_frame_generation();
			params.dlss_rr = ray_reconstruction;
			params.orthogonal = p_render_data->scene_data->cam_orthogonal;
			if (ray_reconstruction) {
				params.dlss_rr_diffuse_albedo = rb_data->nrd_context->get_rr_diffuse_albedo();
				params.dlss_rr_specular_albedo = rb_data->nrd_context->get_rr_specular_albedo();
				params.dlss_rr_normal_roughness = rb_data->nrd_context->get_rr_normal_roughness();
				params.dlss_rr_specular_hit_dist = rb_data->nrd_context->get_rr_specular_hit_distance();
			}
			params.preset = '?'; // FIXME: Unique preset per viewport? Does anyone need this?
			params.z_near = p_render_data->scene_data->z_near;
			params.z_far = p_render_data->scene_data->z_far;
			params.fovy = fovy;
			params.jitter = jitter;
			params.delta_time = float(p_time_step);
			params.reset_accumulation = !rb_data->nrd_context || rb_data->nrd_context->get_rr_reset_history();

			Projection correction;
			correction.set_depth_correction(true, true, true);

			const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;
			const Projection &cur_proj = p_render_data->scene_data->cam_projection;
			const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;
			const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;
			Projection prev_projection = correction * prev_proj;
			Projection cur_projection = correction * cur_proj;
			Transform3D current_from_previous = cur_transform;
			for (int axis = 0; axis < 3; axis++) {
				current_from_previous.origin[axis] = p_render_data->scene_data->cam_origin[axis] - p_render_data->scene_data->prev_cam_origin[axis];
			}
			params.reprojection = prev_projection * Transform3D(prev_transform.basis.inverse()) * current_from_previous * cur_projection.inverse();
			params.cam_projection = cur_projection;
			params.cam_transform = cur_transform;
			for (int axis = 0; axis < 3; axis++) {
				params.cam_transform.origin[axis] = p_render_data->scene_data->cam_origin[axis] - rt_state->rt_origin[axis];
			}

			rb->set_upscaler_ready(dlss_effect->is_ready(rb_data->get_dlss_context(), ray_reconstruction));
			dlss_effect->upscale(params);
		}
	} else if (p_scale_type == SCALE_3D_MFX) {
#ifdef METAL_MFXTEMPORAL_ENABLED
		bool reset = rb_data->ensure_mfx_temporal(mfx_temporal_effect);

		RID exposure;
		if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
			exposure = luminance->get_current_luminance_buffer(rb);
		}

		RD::get_singleton()->draw_command_begin_label("MetalFX Temporal");
		// Scale to Ãƒâ€šÃ‚Â±0.5.
		Vector2 jitter = p_render_data->scene_data->taa_jitter * 0.5f;
		jitter *= Vector2(1.0, -1.0); // Flip y-axis as bottom left is origin.

		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			RendererRD::MFXTemporalEffect::Params params;
			params.src = rb->get_internal_texture(v);
			params.depth = rb->get_depth_texture(v);
			params.motion = rb->get_velocity_buffer(false, v);
			params.exposure = exposure;
			params.dst = rb->get_upscaled_texture(v);
			params.jitter_offset = jitter;
			params.reset = reset;

			rb->set_upscaler_ready(true);
			mfx_temporal_effect->process(rb_data->get_mfx_temporal_context(), params);
		}

		RD::get_singleton()->draw_command_end_label();
#endif
	} else if (p_using_taa) {
		RD::get_singleton()->draw_command_begin_label("TAA");
		RENDER_TIMESTAMP("TAA");
		taa->process(rb, rb->get_base_data_format(), p_render_data->scene_data->z_near, p_render_data->scene_data->z_far);
		RD::get_singleton()->draw_command_end_label();
	}
}

void RenderForwardClustered::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
#ifdef DEBUG_ENABLED
	if (primary_visibility_mode != PRIMARY_VISIBILITY_RASTER) {
		ERR_PRINT_ONCE("GODOT_PRIMARY_VISIBILITY currently supports only R; T, H-R and H-T are not implemented.");
		return;
	}
#endif
	micro_geometry_scenario = p_render_data->scenario;
	micro_geometry_visible_layers = p_render_data->scene_data->camera_visible_layers;
	while (micro_geometry_passes.size() > micro_geometry_pass_cursor) {
		if (micro_geometry_passes[micro_geometry_passes.size() - 1]) {
			memdelete(micro_geometry_passes[micro_geometry_passes.size() - 1]);
		}
		micro_geometry_passes.resize(micro_geometry_passes.size() - 1);
	}
	micro_geometry_pass_cursor = 0;
	micro_geometry_pass_frame = RSG::rasterizer->get_frame_number();
	for (auto &batch : micro_geometry_batches) {
		if (batch.generation != micro_geometry_generation) {
			batch.sources.clear();
		}
	}
	scene_state.used_uniform_buffer_count = 0;
	ERR_FAIL_NULL(p_render_data);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	ERR_FAIL_COND_MSG(p_render_data->reflection_probe.is_valid(), "Reflection captures have no consumer in the RTXDI renderer.");
	ERR_FAIL_COND_MSG(rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED, "RTXDI surface rendering does not support MSAA.");
	ERR_FAIL_COND_MSG(p_render_data->scene_data->view_count != 1, "RTXDI surface rendering supports one view per viewport.");
	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());
	if (raytracing->update_viewport_settings(p_render_data)) {
		rb_data->invalidate_raytracing_history();
	}
	const RSE::ViewportDebugDraw micro_debug_mode = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RASTER || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RT ? get_debug_draw_mode() : RSE::VIEWPORT_DEBUG_DRAW_DISABLED;
	if (rb_data->micro_geometry_debug_mode != micro_debug_mode) {
		rb_data->micro_geometry_debug_mode = micro_debug_mode;
		rb_data->invalidate_raytracing_history();
		raytracing->_get_viewport_state(p_render_data)->camera_history_epoch++;
		raytracing->_get_viewport_state(p_render_data)->pathtracing_history_epoch++;
	}
	const RendererEnvironmentStorage::RaytracingSettings &rt_settings = raytracing->_get_viewport_state(p_render_data)->settings;
	const bool path_traced = rt_settings.raytracing_rendering_mode == RSE::RAYTRACING_RENDERING_MODE_PATH_TRACED;
	const bool raw_path_traced = path_traced && rt_settings.raytracing_denoiser == RSE::RAYTRACING_DENOISER_NONE;
	ERR_FAIL_COND_MSG(raw_path_traced && (rb->get_internal_size() != rb->get_target_size() || RSE::scaling_3d_mode_type(rb->get_scaling_3d_mode()) == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL || rb->get_use_taa() || rb->get_frame_generation()), "Raw path-traced reference requires native resolution, no temporal upscaler, TAA or frame generation.");
	if (rt_settings.raytracing_denoiser == RSE::RAYTRACING_DENOISER_DLSS_RR) {
		ERR_FAIL_COND_MSG(rb->get_scaling_3d_mode() != RSE::VIEWPORT_SCALING_3D_MODE_DLSS, "DLSS Ray Reconstruction requires Viewport.scaling_3d_mode = NVIDIA DLSS, or Project Settings > Rendering > Scaling 3D > Mode > NVIDIA DLSS.");
	}
	const bool separate_specular = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_SEPARATE_SPECULAR);
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	current_cluster_builder = rb_data->cluster_builder;
	ERR_FAIL_NULL(current_cluster_builder);
	p_render_data->cluster_buffer = current_cluster_builder->get_cluster_buffer();
	p_render_data->cluster_size = current_cluster_builder->get_cluster_size();
	p_render_data->cluster_max_elements = current_cluster_builder->get_max_cluster_elements();
	p_render_data->voxel_gi_count = 0;
	scene_state.lightmaps_used = 0;
	_update_vrs(rb);

	RENDER_TIMESTAMP("Prepare 3D Scene");
	const Size2i screen_size = rb->get_internal_size();
	Size2i lighting_size = screen_size;
	if (!path_traced && rt_settings.rtxdi_resolution != RSE::RTXDI_RESOLUTION_FULL) {
		const double scale = rt_settings.rtxdi_resolution == RSE::RTXDI_RESOLUTION_HALF_PIXELS ? Math::sqrt(0.5) : 0.5;
		lighting_size = Size2i(MAX(1, int(Math::ceil(screen_size.x * scale))), MAX(1, int(Math::ceil(screen_size.y * scale))));
	}
	Size2i indirect_size = screen_size;
	if (!path_traced && rt_settings.ddgi_resolution != RSE::DDGI_RESOLUTION_FULL) {
		const double scale = rt_settings.ddgi_resolution == RSE::DDGI_RESOLUTION_HALF_PIXELS ? Math::sqrt(0.5) : 0.5;
		indirect_size = Size2i(MAX(1, int(Math::ceil(screen_size.x * scale))), MAX(1, int(Math::ceil(screen_size.y * scale))));
	}
	const bool using_taa = rb->get_use_taa();
	const Scale3DMode scale_type = _resolve_scale_3d_mode(rb);
	const bool using_upscaling = scale_type != SCALE_3D_NONE;
	const bool reverse_cull = p_render_data->scene_data->cam_transform.basis.determinant() < 0;
	const RID color_only_framebuffer = rb_data->get_color_only_fb();
	const RendererRD::MaterialStorage::Samplers samplers = rb->get_samplers();
	RID color_framebuffer;
	rb->ensure_velocity();
	rb_data->ensure_normal_roughness_texture();
	if (rb_data->nrd_context && (rb_data->nrd_context->size != lighting_size || rb_data->nrd_context->frame_size != screen_size)) {
		memdelete(rb_data->nrd_context);
		rb_data->nrd_context = nullptr;
	}
	if (!rb_data->nrd_context) {
		rb_data->nrd_context = nrd_effect->create_context(lighting_size, screen_size);
	}
	ERR_FAIL_NULL(rb_data->nrd_context);
	p_render_data->scene_data->calculate_motion_vectors = true;
	p_render_data->scene_data->directional_light_count = 0;
	p_render_data->scene_data->opaque_prepass_threshold = 0.0f;
	p_render_data->scene_data->emissive_exposure_normalization = -1.0f;
	LightClusterPreparation light_preparation(p_render_data);
	const uint64_t engine_frame = RSG::rasterizer->get_frame_number();
	auto prepare_camera_motion = [&](uint32_t) {
		for (const PagedArray<RenderGeometryInstance *> *instances : { p_render_data->instances, p_render_data->rt_instances }) {
			if (instances) {
				for (uint32_t i = 0; i < instances->size(); i++) {
					static_cast<GeometryInstanceForwardClustered *>((*instances)[i])->age_out_motion(engine_frame);
				}
			}
		}
	};
	WorkerThreadPool::GroupID prepare_camera_motion_job = -1;
	if ((p_render_data->instances && p_render_data->instances->size()) || (p_render_data->rt_instances && p_render_data->rt_instances->size())) {
		prepare_camera_motion_job = WorkerThreadPool::get_singleton()->try_add_native_group_task([](void *p_data, uint32_t p_index) {
			GodotProfileZone("prepare_camera_motion");
			(*static_cast<decltype(prepare_camera_motion) *>(p_data))(p_index);
		},
				&prepare_camera_motion, 1, 1, true, SNAME("prepare_camera_motion"));
	}
	if (prepare_camera_motion_job < 0) {
		prepare_camera_motion(0);
	}
	_setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);
	_update_render_base_uniform_set();
	if (prepare_camera_motion_job >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(prepare_camera_motion_job);
	}
	_update_dirty_geometry_instances();
	bool invalid_deformation = false;
	bool invalid_micro_geometry_history = false;
	const bool profile_deformation = RSG::utilities->capturing_timestamps && engine_frame % 120 == 0;
	const char *deformation_reason = "none";
	uint32_t deformation_instance = UINT32_MAX;
	int32_t deformation_surface = -1;
	uint32_t deformation_material_flags = 0;
	const SceneShaderForwardClustered::ShaderData *deformation_shader = nullptr;
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	auto prepare_deformation_validity = [&](uint32_t) {
		for (uint32_t i = 0; i < p_render_data->instances->size() && !invalid_micro_geometry_history; i++) {
			GeometryInstanceForwardClustered *instance = static_cast<GeometryInstanceForwardClustered *>((*p_render_data->instances)[i]);
			const bool had_invalid_deformation = invalid_deformation;
			invalid_micro_geometry_history = instance->transform_status == GeometryInstanceForwardClustered::TransformStatus::TELEPORTED || instance->rt_procedural != nullptr;
			if (profile_deformation && !invalid_deformation && invalid_micro_geometry_history) {
				deformation_reason = instance->transform_status == GeometryInstanceForwardClustered::TransformStatus::TELEPORTED ? "teleported" : "procedural";
			}
			invalid_deformation |= invalid_micro_geometry_history;
			for (GeometryInstanceSurfaceDataCache *surface = instance->surface_caches; surface && !invalid_micro_geometry_history; surface = surface->next) {
				const bool material_deformed = bool(surface->rtxdi_material_flags & GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_DEFORMED);
				if (profile_deformation && !invalid_deformation && material_deformed) {
					deformation_reason = "material_deformed";
					deformation_surface = surface->surface_index;
					deformation_material_flags = surface->rtxdi_material_flags;
					deformation_shader = surface->shader;
				}
				invalid_deformation |= material_deformed;
				if (instance->scene_data->mesh_instance.is_valid() && mesh_storage->mesh_instance_get_last_change(instance->scene_data->mesh_instance, surface->surface_index) == engine_frame) {
					invalid_micro_geometry_history = mesh_storage->mesh_instance_get_prev_vertex_buffer(instance->scene_data->mesh_instance, surface->surface_index) == mesh_storage->mesh_instance_get_vertex_buffer(instance->scene_data->mesh_instance, surface->surface_index);
					if (profile_deformation && !invalid_deformation && invalid_micro_geometry_history) {
						deformation_reason = "previous_vertex_buffer_alias";
						deformation_surface = surface->surface_index;
					}
					invalid_deformation |= invalid_micro_geometry_history;
				}
			}
			if (profile_deformation && !had_invalid_deformation && invalid_deformation) {
				deformation_instance = instance->persistent_instance;
			}
		}
	};
	WorkerThreadPool::GroupID prepare_deformation_validity_job = -1;
	if (p_render_data->instances->size()) {
		prepare_deformation_validity_job = WorkerThreadPool::get_singleton()->try_add_native_group_task([](void *p_data, uint32_t p_index) {
			GodotProfileZone("prepare_deformation_validity");
			(*static_cast<decltype(prepare_deformation_validity) *>(p_data))(p_index);
		},
				&prepare_deformation_validity, 1, 1, true, SNAME("prepare_deformation_validity"));
	}
	if (prepare_deformation_validity_job < 0) {
		prepare_deformation_validity(0);
	}

	_update_dirty_geometry_pipelines();
	RID radiance_texture;
	bool draw_sky = false;
	bool draw_sky_fog_only = false;
	// We invert luminance_multiplier for sky so that we can combine it with exposure value.
	float sky_luminance_multiplier = 1.0 / rb->get_luminance_multiplier();
	float sky_brightness_multiplier = 1.0;

	Color clear_color;
	bool load_color = false;

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		clear_color = Color(0, 0, 0, 1); //in overdraw mode, BG should always be black
	} else if (is_environment(p_render_data->environment)) {
		RSE::EnvironmentBG bg_mode = environment_get_background(p_render_data->environment);
		float bg_energy_multiplier = environment_get_bg_energy_multiplier(p_render_data->environment);
		bg_energy_multiplier *= environment_get_bg_intensity(p_render_data->environment);

		if (p_render_data->camera_attributes.is_valid()) {
			bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}

		switch (bg_mode) {
			case RSE::ENV_BG_CLEAR_COLOR:
			case RSE::ENV_BG_COLOR: {
				clear_color = bg_mode == RSE::ENV_BG_CLEAR_COLOR ? p_default_bg_color : environment_get_bg_color(p_render_data->environment);

				if (!p_render_data->transparent_bg && (rb->has_custom_data(RB_SCOPE_FOG) || environment_get_fog_enabled(p_render_data->environment))) {
					draw_sky_fog_only = true;
					RendererRD::MaterialStorage::get_singleton()->material_set_param(sky.sky_scene_state.fog_material, "clear_color", Variant(clear_color));
				}

				clear_color = clear_color.srgb_to_linear();
				clear_color.r *= bg_energy_multiplier;
				clear_color.g *= bg_energy_multiplier;
				clear_color.b *= bg_energy_multiplier;
			} break;
			case RSE::ENV_BG_SKY: {
				draw_sky = !p_render_data->transparent_bg;
			} break;
			case RSE::ENV_BG_CANVAS: {
				{
					RID texture = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_texture(rb->get_render_target());
					bool convert_to_linear = !RendererRD::TextureStorage::get_singleton()->render_target_is_using_hdr(rb->get_render_target());
					copy_effects->copy_to_fb_rect(texture, color_only_framebuffer, Rect2i(), false, false, false, false, RID(), false, false, convert_to_linear);
				}
				load_color = true;
			} break;
			case RSE::ENV_BG_KEEP: {
				load_color = true;
			} break;
			case RSE::ENV_BG_CAMERA_FEED: {
			} break;
			default: {
			}
		}

		if (draw_sky || draw_sky_fog_only || environment_get_sky(p_render_data->environment).is_valid()) {
			RENDER_TIMESTAMP("Setup Sky");
			RD::get_singleton()->draw_command_begin_label("Setup Sky");

			// Setup our sky render information for this frame/viewport
			sky.setup_sky(p_render_data, screen_size);

			sky_brightness_multiplier *= bg_energy_multiplier;

			RID sky_rid = environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				sky.update_radiance_buffers(rb, p_render_data->environment, p_render_data->scene_data->cam_transform.origin, time, 1.0f, 1.0f);
				radiance_texture = sky.sky_get_radiance_texture_rd(sky_rid);
			} else {
				// do not try to draw sky if invalid
				draw_sky = false;
			}

			if (draw_sky || draw_sky_fog_only) {
				// update sky half/quarter res buffers (if required)
				sky.update_res_buffers(rb, p_render_data->environment, time, sky_luminance_multiplier, sky_brightness_multiplier);
			}

			RD::get_singleton()->draw_command_end_label();
		}

		if (bg_mode != RSE::ENV_BG_CLEAR_COLOR && bg_mode != RSE::ENV_BG_COLOR) {
			clear_color = clear_color.srgb_to_linear();
		}
	} else {
		clear_color = p_default_bg_color.srgb_to_linear();
	}

	if (prepare_deformation_validity_job >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(prepare_deformation_validity_job);
	}
	if (profile_deformation) {
		String shader_details;
		if (deformation_shader) {
			const SceneShaderForwardClustered::ShaderData *draw_shader = deformation_shader;
#ifdef DEBUG_ENABLED
			if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LIGHTING)) {
				draw_shader = scene_shader.default_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW)) {
				draw_shader = scene_shader.overdraw_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS)) {
				draw_shader = scene_shader.debug_shadow_splits_material_shader_ptr;
			}
#endif
			shader_details = vformat(" material_flags=%d uses_vertex=%s uses_position=%s writes_modelview_or_projection=%s uses_world_coordinates=%s uses_time=%s generated_standard=%s depth_draw=%d depth_test=%d shader_path=%s shader_prefix=%s", deformation_material_flags, deformation_shader->uses_vertex, deformation_shader->uses_position, deformation_shader->writes_modelview_or_projection, deformation_shader->uses_world_coordinates, deformation_shader->uses_time, deformation_shader->generated_standard_material, int(draw_shader->depth_draw), int(draw_shader->depth_test), draw_shader->path, draw_shader->code.left(768).replace("\r", " ").replace("\n", " "));
		}
		print_line(vformat("Microgeometry deformation: frame=%d camera=%d invalid=%s subtype=%s persistent_instance=%d surface=%d%s", engine_frame, p_render_data->scene_data->camera.get_id(), invalid_deformation, deformation_reason, deformation_instance, deformation_surface, shader_details));
	}
	color_framebuffer = rb_data->prepare_rtxdi_surface(p_render_data->scene_data, invalid_deformation, invalid_micro_geometry_history);
	ERR_FAIL_COND(color_framebuffer.is_null());
	_render_shadows(p_render_data);
	light_preparation.begin_lights();

	const uint64_t camera_history_epoch = raytracing->_get_viewport_state(p_render_data)->camera_history_epoch;
	RenderListPreparation *camera_preparation = _begin_render_list(RENDER_LIST_OPAQUE, p_render_data, PASS_MODE_RTXDI_SURFACE);
	RENDER_TIMESTAMP("Primary Visibility Acceleration Structures");
	RTViewportState *rt_state = raytracing->build_tlas(p_render_data);
	RENDER_TIMESTAMP("Primary Visibility Acceleration Structures Complete");
	_finish_render_list(camera_preparation);
	render_list[RENDER_LIST_OPAQUE].sort_by_key();
	int *render_info = p_render_data->render_info ? p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE] : nullptr;
	_fill_instance_data(RENDER_LIST_OPAQUE, render_info);
	ERR_FAIL_NULL(rt_state);
	const bool geometry_history_changed = camera_history_epoch != rt_state->camera_history_epoch;
	ERR_FAIL_COND_MSG(!raytracing->_prepare_ddgi(rt_state, rb->is_ddgi_debug_freeze_anchor()), "Camera-following DDGI state preparation failed.");
	_pre_opaque_render(p_render_data, light_preparation);
	SceneShaderForwardClustered::ShaderSpecialization base_specialization = scene_shader.default_specialization;
	base_specialization.cluster_has_area_light = current_cluster_builder->get_cluster_count_by_type(ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT) != 0;
	p_render_data->scene_data->directional_light_count = p_render_data->directional_light_count;
	_update_render_base_uniform_set();
	uint32_t opaque_pass_uniform_buffer_index = _setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, true);
	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, p_render_data, radiance_texture, samplers, opaque_pass_uniform_buffer_index, true);
	if (!load_color) {
		clear_color.a = p_render_data->transparent_bg ? 0.0f : 1.0f;
		RD::get_singleton()->draw_list_begin(color_only_framebuffer, RD::DRAW_CLEAR_COLOR_0, Vector<Color>({ clear_color }));
		RD::get_singleton()->draw_list_end();
	}
	if (!path_traced) {
		RENDER_TIMESTAMP("RTXDI Surface");
		RD::get_singleton()->draw_command_begin_label("RTXDI Surface");
		Vector<Color> surface_clear;
		for (uint32_t i = 0; i < 6; i++) {
			surface_clear.push_back(Color(0, 0, 0, 0));
		}
		RenderListParameters render_list_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), reverse_cull, PASS_MODE_RTXDI_SURFACE, true, p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, 1, 0, base_specialization);
		render_list_params.micro_geometry = render_list[RENDER_LIST_OPAQUE].last_micro_pass;
		_render_list_with_draw_list(&render_list_params, color_framebuffer, RD::DRAW_CLEAR_ALL, surface_clear, 0.0f, 0u, p_render_data->render_region);
		RD::get_singleton()->draw_command_end_label();
	}
	RenderRTXDISurfaceResources surface;
	surface.samplers = samplers;
	for (uint32_t attachment = 0; attachment < 6; attachment++) {
		surface.current[attachment] = rb_data->get_rtxdi_surface_texture(attachment);
		surface.previous[attachment] = rb_data->get_rtxdi_surface_texture(attachment, true);
	}
	surface.current_depth = rb_data->get_rtxdi_surface_depth();
	surface.previous_depth = rb_data->get_rtxdi_surface_depth(true);
	surface.size = lighting_size;
	surface.history_valid = rb_data->is_rtxdi_surface_history_valid() && !geometry_history_changed;
	surface.orthogonal = rb_data->is_rtxdi_surface_camera_orthogonal();
	surface.frame_index = rb_data->get_rtxdi_surface_frame_index();
	RendererRD::NRDEffect::Frame frame;
	frame.stochastic_direct_samples = rt_settings.raytracing_denoiser == RSE::RAYTRACING_DENOISER_DLSS_RR;
	for (uint32_t attachment = 0; attachment < 6; attachment++) {
		frame.surface[attachment] = surface.current[attachment];
	}
	frame.depth = surface.current_depth;
	frame.scene_data = scene_state.uniform_buffers[opaque_pass_uniform_buffer_index];
	frame.color = rb->get_internal_texture(0);
	if (separate_specular) {
		rb_data->ensure_specular();
		frame.separate_specular = rb_data->get_specular(0);
	}
	frame.velocity = rb->get_velocity_buffer(false, 0);
	frame.normal_roughness = rb_data->get_normal_roughness(0);
	frame.fog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	if (rb->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = rb->get_custom_data(RB_SCOPE_FOG);
		if (fog->fog_map.is_valid()) {
			frame.fog = fog->fog_map;
			frame.fog_enabled = true;
			frame.fog_inverse_length = fog->length > 0.0f ? 1.0f / fog->length : 1.0f;
			frame.fog_spread = fog->spread > 0.0f ? 1.0f / fog->spread : 1.0f;
		}
	}
	frame.fog_legacy_blending = fog_use_legacy_blending_get();
	frame.radiance = radiance_texture.is_valid() ? radiance_texture : texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	frame.environment_energy = p_render_data->environment.is_valid() ? environment_get_bg_energy_multiplier(p_render_data->environment) * environment_get_bg_intensity(p_render_data->environment) : 0.0f;
	frame.directional_lights = RendererRD::LightStorage::get_singleton()->get_directional_light_buffer();
	frame.projection = rb_data->get_rtxdi_surface_camera_projection();
	frame.previous_projection = rb_data->get_rtxdi_surface_previous_camera_projection();
	frame.camera = rb_data->get_rtxdi_surface_camera_transform();
	frame.previous_camera = rb_data->get_rtxdi_surface_previous_camera_transform();
	frame.jitter = rb_data->get_rtxdi_surface_camera_jitter();
	frame.previous_jitter = rb_data->get_rtxdi_surface_previous_camera_jitter();
	frame.frame_index = surface.frame_index;
	frame.history_valid = surface.history_valid;
	frame.orthogonal = surface.orthogonal;
	frame.time_step = time_step;
	if (path_traced) {
		if (!raytracing->pathtracing) {
			raytracing->pathtracing = memnew(RenderPathtracing);
		}
		Color background = clear_color;
		if (p_render_data->camera_attributes.is_valid()) {
			float exposure = MAX(RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes), 1e-20f);
			background.r /= exposure;
			background.g /= exposure;
			background.b /= exposure;
		}
		RENDER_TIMESTAMP("Path Tracing");
		ERR_FAIL_COND_MSG(!raytracing->pathtracing->render(*raytracing, *rt_state, frame.scene_data, frame.radiance, { surface.current, 6 }, rb_data->get_primary_surface_trace_depth(), screen_size, is_using_radiance_octmap_array(), draw_sky, background), "Native camera path tracing failed.");
		frame.noisy_diffuse = rt_state->pathtracing->get_diffuse();
		frame.noisy_specular = rt_state->pathtracing->get_specular();
		frame.camera_radiance = rt_state->pathtracing->get_radiance();
		frame.history_valid &= rt_state->pathtracing->history_valid;
		RENDER_TIMESTAMP("Primary Surface Depth Resolve");
		copy_effects->copy_r32f_to_depth_fb(rb_data->get_primary_surface_trace_depth(), rb_data->get_depth_fb(), Rect2i(Point2i(), screen_size));
	}
	RENDER_TIMESTAMP("Primary Surface History Commit");
	rb_data->commit_rtxdi_surface();
	RENDER_TIMESTAMP("Primary Surface History Commit Complete");
	RENDER_TIMESTAMP("Camera Guides");
	ERR_FAIL_COND_MSG(!nrd_effect->prepare(rb_data->nrd_context, frame), "Camera guide preparation failed.");
	RENDER_TIMESTAMP("Process Pre Opaque Compositor Effects");
	_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE, p_render_data);
	if (!path_traced) {
		RENDER_TIMESTAMP("RTXDI Direct Lighting");
		rtxdi->render(surface, rt_state, scene_state.uniform_buffers[opaque_pass_uniform_buffer_index], 0, 1);
		ERR_FAIL_COND_MSG(!rt_state->rtxdi_di || rt_state->rtxdi_di->views.is_empty() || rt_state->rtxdi_di->views[0].last_frame_index != surface.frame_index, "RTXDI did not produce lighting for this frame.");
		frame.noisy_diffuse = rtxdi->get_diffuse_radiance_distance(rt_state, 0);
		frame.noisy_specular = rtxdi->get_specular_radiance_distance(rt_state, 0);
		if (rt_state->ddgi) {
			RENDER_TIMESTAMP("DDGI Probe Lighting");
			ERR_FAIL_COND_MSG(!raytracing->_render_ddgi(rt_state, frame.scene_data, frame.radiance), "DDGI probe lighting failed.");
			RENDER_TIMESTAMP("DDGI Camera Irradiance");
			ERR_FAIL_COND_MSG(!raytracing->ddgi_effect->render_camera(*rt_state->ddgi, frame.scene_data, rt_state->frame_constants_buffer, frame.surface, frame.depth, screen_size, indirect_size, frame.orthogonal), "DDGI camera interpolation failed.");
			frame.indirect_diffuse = rt_state->ddgi->indirect_radiance;
			frame.indirect_size = rt_state->ddgi->interpolation_size;
		}
	}
	RENDER_TIMESTAMP("Camera HDR Composition");
	ERR_FAIL_COND_MSG(!nrd_effect->process(rb_data->nrd_context, frame, rt_settings.raytracing_denoiser == RSE::RAYTRACING_DENOISER_NRD), "Camera frame processing failed.");
	RENDER_TIMESTAMP("Process Post Opaque Compositor Effects");
	_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, p_render_data);
	if (!path_traced && (draw_sky || draw_sky_fog_only)) {
		RENDER_TIMESTAMP("Render Sky");

		RD::get_singleton()->draw_command_begin_label("Draw Sky");
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_only_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);

		sky.draw_sky(draw_list, rb, p_render_data->environment, color_only_framebuffer, time, sky_luminance_multiplier, sky_brightness_multiplier);

		RD::get_singleton()->draw_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	RENDER_TIMESTAMP("Process Post Sky Compositor Effects");
	_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY, p_render_data);
	if (separate_specular) {
		RENDER_TIMESTAMP("Merge Specular");
		copy_effects->merge_specular(color_only_framebuffer, frame.separate_specular, RID(), RID(), 1);
	}
	if (using_upscaling) {
		rb->ensure_upscaled();
	}
	if (scene_state.used_screen_texture || global_surface_data.screen_texture_used) {
		RENDER_TIMESTAMP("Copy Screen Texture");

		_render_buffers_ensure_screen_texture(p_render_data);

		if (scene_state.used_screen_texture) {
			// Copy screen texture to backbuffer so we can read from it
			_render_buffers_copy_screen_texture(p_render_data);
		}
	}

	if (scene_state.used_depth_texture || global_surface_data.depth_texture_used) {
		RENDER_TIMESTAMP("Copy Depth Texture");

		_render_buffers_ensure_depth_texture(p_render_data);

		if (scene_state.used_depth_texture) {
			// Copy depth texture to backbuffer so we can read from it
			_render_buffers_copy_depth_texture(p_render_data);
		}
	}

	RENDER_TIMESTAMP("Process Pre Transparent Compositor Effects");
	_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT, p_render_data);
	RENDER_TIMESTAMP("Process Post Transparent Compositor Effects");
	_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, p_render_data);
	if (using_upscaling || using_taa) {
		_render_3d_upscaling(p_render_data, scale_type, using_taa, time_step);
	}
	_debug_draw_cluster(rb);
	RENDER_TIMESTAMP("Tonemap");
	_render_buffers_post_process_and_tonemap(p_render_data);
	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RT) {
		if (!raytracing->pathtracing) {
			raytracing->pathtracing = memnew(RenderPathtracing);
		}
		RENDER_TIMESTAMP("Microgeometry RT Debug");
		ERR_FAIL_COND_MSG(!raytracing->pathtracing->render(*raytracing, *rt_state, frame.scene_data, frame.radiance, { surface.current, 6 }, rb_data->get_primary_surface_trace_depth(), screen_size, is_using_radiance_octmap_array(), false, Color(), true), "Microgeometry RT debug rays failed.");
	}
	if (p_render_data->render_info) {
		auto statistics = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->get_statistics();
		uint64_t geometry_bytes = statistics.pool_bytes + statistics.metadata_bytes + statistics.retired_metadata_bytes + statistics.io_bytes + statistics.feedback_bytes + statistics.acceleration_structure_bytes + raytracing->get_persistent_memory_bytes() + statistics.raster_selection_bytes + raytracing->get_micro_geometry_memory_bytes();
		uint64_t as_bytes = statistics.acceleration_structure_bytes + raytracing->get_micro_geometry_as_memory_bytes();
		uint64_t values[] = {
			rb_data->micro_geometry_clusters, rb_data->micro_geometry_triangles,
			rt_state->micro_geometry ? rt_state->micro_geometry->selected_clusters : 0,
			rt_state->micro_geometry ? rt_state->micro_geometry->selected_triangles : 0,
			statistics.resident_pages, statistics.pending_pages, statistics.pool_bytes / 1024,
			geometry_bytes / 1024, as_bytes / 1024, statistics.clas_builds,
			rt_state->micro_geometry ? rt_state->micro_geometry->completed_builds : 0,
			statistics.pressure
		};
		for (uint32_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
			p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_MICRO_GEOMETRY_RASTER_CLUSTERS + index] = MIN(values[index], uint64_t(INT32_MAX));
		}
	}
	_render_buffers_debug_draw(p_render_data);
}

void RenderForwardClustered::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RendererSceneRenderRD::_render_buffers_debug_draw(p_render_data);

	RID render_target = rb->get_render_target();

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RASTER || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RT) {
		RTViewportState *state = raytracing ? raytracing->_get_viewport_state(p_render_data) : nullptr;
		RID source;
		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RASTER && state && state->settings.raytracing_rendering_mode == RSE::RAYTRACING_RENDERING_MODE_HYBRID) {
			source = rb_data->get_rtxdi_surface_texture(RenderBufferDataForwardClustered::RTXDI_SURFACE_BASE);
		} else if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MICRO_GEOMETRY_RT && state && state->pathtracing && state->pathtracing->micro_geometry_debug) {
			source = state->pathtracing->get_radiance();
		}
		RID framebuffer = texture_storage->render_target_get_rd_framebuffer(render_target);
		if (source.is_valid()) {
			copy_effects->copy_to_fb_rect(source, framebuffer, Rect2(Vector2(), texture_storage->render_target_get_size(render_target)), false, false);
		} else {
			RD::get_singleton()->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_ALL, Vector<Color>{ Color(0, 0, 0, 1) });
			RD::get_singleton()->draw_list_end();
		}
		return;
	}

	if (get_debug_draw_mode() >= RSE::VIEWPORT_DEBUG_DRAW_DDGI_PROBES && get_debug_draw_mode() <= RSE::VIEWPORT_DEBUG_DRAW_DDGI_INDIRECT) {
		RTViewportState *state = raytracing ? raytracing->_get_viewport_state(p_render_data) : nullptr;
		RID framebuffer = texture_storage->render_target_get_rd_framebuffer(render_target);
		Size2i size = texture_storage->render_target_get_size(render_target);
		if (!state || !state->ddgi || !state->ddgi->camera_rendered || rb->get_view_count() != 1 || state->settings.raytracing_rendering_mode != RSE::RAYTRACING_RENDERING_MODE_HYBRID) {
			RD::get_singleton()->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_ALL, Vector<Color>{ Color(0, 0, 0, 1) });
			RD::get_singleton()->draw_list_end();
			return;
		}
		RID surface[6];
		for (uint32_t i = 0; i < 6; i++) {
			surface[i] = rb_data->get_rtxdi_surface_texture(i);
		}
		ERR_FAIL_COND_MSG(!raytracing->ddgi_effect->render_debug(*state->ddgi, framebuffer, state->frame_constants_buffer, surface, rb_data->get_rtxdi_surface_depth(), size, state->camera_orthogonal, uint32_t(get_debug_draw_mode() - RSE::VIEWPORT_DEBUG_DRAW_DDGI_PROBES)), "DDGI debug drawing failed.");
		return;
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SSAO && rb->has_texture(RB_SCOPE_SSAO, RB_FINAL)) {
		RID final = rb->get_texture_slice(RB_SCOPE_SSAO, RB_FINAL, 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(final, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, true);
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SSIL && rb->has_texture(RB_SCOPE_SSIL, RB_FINAL)) {
		RID final = rb->get_texture_slice(RB_SCOPE_SSIL, RB_FINAL, 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(final, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false);
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_GI_BUFFER && rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID ambient_texture = rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
		RID reflection_texture = rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION);
		copy_effects->copy_to_fb_rect(ambient_texture, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false, false, true, reflection_texture, rb->get_view_count() > 1);
	}
}

void RenderForwardClustered::_render_shadow_pass(RID p_light, RID p_shadow_atlas, int p_pass, const PagedArray<RenderGeometryInstance *> &p_instances, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, bool p_open_pass, bool p_close_pass, bool p_clear_region, RenderingServerTypes::RenderInfo *p_render_info, const Size2i &p_viewport_size, const Transform3D &p_main_cam_transform, const Vector<Plane> &p_cull_planes, const double *p_main_origin, const double *p_cull_origin) {
	micro_geometry_shadow_planes = p_cull_planes;
	for (int axis = 0; axis < 3; axis++) {
		micro_geometry_shadow_origin[axis] = p_cull_origin[axis];
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	ERR_FAIL_COND(!light_storage->owns_light_instance(p_light));

	RID base = light_storage->light_instance_get_base_light(p_light);
	micro_geometry_shadow_layers = micro_geometry_visible_layers & light_storage->light_get_shadow_caster_mask(base);

	Rect2i atlas_rect;
	uint32_t atlas_size = 1;
	RID atlas_fb;

	bool reverse_cull_face = light_storage->light_get_reverse_cull_face_mode(base);
	bool using_dual_paraboloid = false;
	bool using_dual_paraboloid_flip = false;
	Vector2i dual_paraboloid_offset;
	RID render_fb;
	RID render_texture;
	float zfar;

	bool use_pancake = false;
	bool render_cubemap = false;
	bool finalize_cubemap = false;

	bool flip_y = false;

	Projection light_projection;
	Transform3D light_transform;
	const double *shadow_origin = nullptr;

	if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
		//set pssm stuff
		uint64_t last_scene_shadow_pass = light_storage->light_instance_get_shadow_pass(p_light);
		if (last_scene_shadow_pass != get_scene_pass()) {
			light_storage->light_instance_set_directional_rect(p_light, light_storage->get_directional_shadow_rect());
			light_storage->directional_shadow_increase_current_light();
			light_storage->light_instance_set_shadow_pass(p_light, get_scene_pass());
		}

		use_pancake = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE) > 0;
		light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
		light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);
		shadow_origin = light_storage->light_instance_get_shadow_origin(p_light, p_pass);

		atlas_rect = light_storage->light_instance_get_directional_rect(p_light);

		if (light_storage->light_directional_get_shadow_mode(base) == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS) {
			atlas_rect.size.width /= 2;
			atlas_rect.size.height /= 2;

			if (p_pass == 1) {
				atlas_rect.position.x += atlas_rect.size.width;
			} else if (p_pass == 2) {
				atlas_rect.position.y += atlas_rect.size.height;
			} else if (p_pass == 3) {
				atlas_rect.position += atlas_rect.size;
			}
		} else if (light_storage->light_directional_get_shadow_mode(base) == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
			atlas_rect.size.height /= 2;

			if (p_pass == 0) {
			} else {
				atlas_rect.position.y += atlas_rect.size.height;
			}
		}

		float directional_shadow_size = light_storage->directional_shadow_get_size();
		Rect2 atlas_rect_norm = atlas_rect;
		atlas_rect_norm.position /= directional_shadow_size;
		atlas_rect_norm.size /= directional_shadow_size;
		light_storage->light_instance_set_directional_shadow_atlas_rect(p_light, p_pass, atlas_rect_norm);

		zfar = RSG::light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

		render_fb = light_storage->direction_shadow_get_fb();
		render_texture = RID();
		flip_y = true;

	} else {
		//set from shadow atlas

		ERR_FAIL_COND(!light_storage->owns_shadow_atlas(p_shadow_atlas));
		ERR_FAIL_COND(!light_storage->shadow_atlas_owns_light_instance(p_shadow_atlas, p_light));

		RSG::light_storage->shadow_atlas_update(p_shadow_atlas);

		uint32_t key = light_storage->shadow_atlas_get_light_instance_key(p_shadow_atlas, p_light);

		uint32_t quadrant = (key >> RendererRD::LightStorage::QUADRANT_SHIFT) & 0x3;
		uint32_t shadow = key & RendererRD::LightStorage::SHADOW_INDEX_MASK;
		uint32_t subdivision = light_storage->shadow_atlas_get_quadrant_subdivision(p_shadow_atlas, quadrant);

		ERR_FAIL_INDEX((int)shadow, light_storage->shadow_atlas_get_quadrant_shadow_size(p_shadow_atlas, quadrant));

		uint32_t shadow_atlas_size = light_storage->shadow_atlas_get_size(p_shadow_atlas);
		uint32_t quadrant_size = shadow_atlas_size >> 1;

		atlas_rect.position.x = (quadrant & 1) * quadrant_size;
		atlas_rect.position.y = (quadrant >> 1) * quadrant_size;

		uint32_t shadow_size = (quadrant_size / subdivision);
		atlas_rect.position.x += (shadow % subdivision) * shadow_size;
		atlas_rect.position.y += (shadow / subdivision) * shadow_size;

		atlas_rect.size.width = shadow_size;
		atlas_rect.size.height = shadow_size;

		zfar = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

		if (light_storage->light_get_type(base) == RSE::LIGHT_OMNI) {
			bool wrap = (shadow + 1) % subdivision == 0;
			dual_paraboloid_offset = wrap ? Vector2i(1 - subdivision, 1) : Vector2i(1, 0);

			if (light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
				render_texture = light_storage->get_cubemap(shadow_size / 2);
				render_fb = light_storage->get_cubemap_fb(shadow_size / 2, p_pass);

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);
				shadow_origin = light_storage->light_instance_get_shadow_origin(p_light, p_pass);
				render_cubemap = true;
				finalize_cubemap = p_pass == 5;
				atlas_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

				atlas_size = shadow_atlas_size;

				if (p_pass == 0) {
					_render_shadow_begin();
				}

			} else {
				atlas_rect.position.x += 1;
				atlas_rect.position.y += 1;
				atlas_rect.size.x -= 2;
				atlas_rect.size.y -= 2;

				atlas_rect.position += p_pass * atlas_rect.size * dual_paraboloid_offset;

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);
				shadow_origin = light_storage->light_instance_get_shadow_origin(p_light, 0);

				using_dual_paraboloid = true;
				using_dual_paraboloid_flip = p_pass == 1;
				render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);
				flip_y = true;
			}

		} else if (light_storage->light_get_type(base) == RSE::LIGHT_SPOT) {
			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);
			shadow_origin = light_storage->light_instance_get_shadow_origin(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;
		} else if (light_storage->light_get_type(base) == RSE::LIGHT_AREA) {
			Vector2 area_size = light_storage->light_area_get_size(base);

			zfar = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE) + area_size.length() / 2.0;

			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);
			shadow_origin = light_storage->light_instance_get_shadow_origin(p_light, 0);

			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;

			using_dual_paraboloid = true;
		}
	}

	if (render_cubemap) {
		//rendering to cubemap
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, false, false, use_pancake, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, Rect2(), false, true, true, true, p_render_info, p_viewport_size, p_main_cam_transform, shadow_origin, p_main_origin);
		if (finalize_cubemap) {
			_render_shadow_process();
			_render_shadow_end();
			//reblit
			Rect2 atlas_rect_norm = atlas_rect;
			atlas_rect_norm.position /= float(atlas_size);
			atlas_rect_norm.size /= float(atlas_size);
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), zfar, false);
			atlas_rect_norm.position += Vector2(dual_paraboloid_offset) * atlas_rect_norm.size;
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), zfar, true);

			//restore transform so it can be properly used
			light_storage->light_instance_set_shadow_transform(p_light, Projection(), light_storage->light_instance_get_base_transform(p_light), zfar, 0, 0, 0, 1.0, 0.0, Vector2(), light_storage->light_instance_get_origin(p_light));
		}

	} else {
		//render shadow
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, using_dual_paraboloid, using_dual_paraboloid_flip, use_pancake, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, atlas_rect, flip_y, p_clear_region, p_open_pass, p_close_pass, p_render_info, p_viewport_size, p_main_cam_transform, shadow_origin, p_main_origin);
	}
}

void RenderForwardClustered::_discard_shadow_preparations() {
	if (shadow_preparations.is_empty()) {
		return;
	}
	for (ShadowPreparation &preparation : shadow_preparations) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(preparation.preparation->job);
		memdelete(preparation.preparation);
		memdelete(preparation.list);
	}
	shadow_preparations.clear();
	scene_state.shadow_passes.clear();
	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_shadow_begin() {
	_discard_shadow_preparations();
	scene_state.shadow_passes.clear();
	_update_dirty_geometry_instances();
	RD::get_singleton()->draw_command_begin_label("Shadow Setup");
	_update_render_base_uniform_set();

	render_list[RENDER_LIST_SECONDARY].clear();
}

void RenderForwardClustered::_render_shadow_append(RID p_framebuffer, const PagedArray<RenderGeometryInstance *> &p_instances, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, float p_bias, float p_normal_bias, bool p_reverse_cull_face, bool p_use_dp, bool p_use_dp_flip, bool p_use_pancake, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, const Rect2i &p_rect, bool p_flip_y, bool p_clear_region, bool p_begin, bool p_end, RenderingServerTypes::RenderInfo *p_render_info, const Size2i &p_viewport_size, const Transform3D &p_main_cam_transform, const double *p_origin, const double *p_main_origin) {
	SceneState::ShadowPass shadow_pass;

	RenderSceneDataRD scene_data;
	scene_data.flip_y = !p_flip_y; // Q: Why is this inverted? Do we assume flip in shadow logic?
	scene_data.cam_projection = p_projection;
	scene_data.cam_transform = p_transform;
	for (int axis = 0; axis < 3; axis++) {
		scene_data.cam_origin[axis] = p_origin ? p_origin[axis] : double(p_transform.origin[axis]);
	}
	scene_data.view_projection[0] = p_projection;
	scene_data.z_far = p_zfar;
	scene_data.z_near = 0.0;
	scene_data.lod_distance_multiplier = p_lod_distance_multiplier;
	scene_data.dual_paraboloid_side = p_use_dp_flip ? -1 : 1;
	scene_data.opaque_prepass_threshold = 0.1f;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_main_cam_transform;
	for (int axis = 0; axis < 3; axis++) {
		scene_data.main_cam_origin[axis] = p_main_origin ? p_main_origin[axis] : double(scene_data.main_cam_transform.origin[axis]);
	}
	scene_data.shadow_pass = true;
	scene_data.camera_visible_layers = micro_geometry_shadow_layers;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.scenario = micro_geometry_scenario;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;
	render_data.render_info = p_render_info;

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_rect.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color(), false, false, p_use_pancake);

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DISABLE_LOD) {
		scene_data.screen_mesh_lod_threshold = 0.0;
	} else {
		scene_data.screen_mesh_lod_threshold = p_screen_mesh_lod_threshold;
	}

	PassMode pass_mode = p_use_dp ? PASS_MODE_SHADOW_DP : PASS_MODE_SHADOW;

	ShadowPreparation preparation;
	preparation.list = memnew(RenderList);
	preparation.render_info = p_render_info;
	preparation.preparation = _begin_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode, false, false, false, false, preparation.list);
	shadow_preparations.push_back(preparation);

	{
		//regular forward for now
		bool flip_cull = p_use_dp_flip;
		if (p_flip_y) {
			flip_cull = !flip_cull;
		}

		if (p_reverse_cull_face) {
			flip_cull = !flip_cull;
		}

		shadow_pass.element_from = 0;
		shadow_pass.micro_geometry = preparation.list->last_micro_pass;
		shadow_pass.element_count = 0;
		shadow_pass.flip_cull = flip_cull;
		shadow_pass.pass_mode = pass_mode;

		shadow_pass.rp_uniform_set = RID(); //will be filled later when instance buffer is complete
		shadow_pass.screen_mesh_lod_threshold = scene_data.screen_mesh_lod_threshold;
		shadow_pass.lod_distance_multiplier = scene_data.lod_distance_multiplier;

		shadow_pass.framebuffer = p_framebuffer;
		shadow_pass.clear_depth = p_begin || p_clear_region;
		shadow_pass.rect = p_rect;

		shadow_pass.uniform_buffer_index = uniform_buffer_index;

		scene_state.shadow_passes.push_back(shadow_pass);
	}
}

void RenderForwardClustered::_render_shadow_process() {
	for (ShadowPreparation &preparation : shadow_preparations) {
		_finish_render_list(preparation.preparation);
		preparation.preparation = nullptr;
	}
	const uint64_t profile_frame = RSG::rasterizer->get_frame_number();
	const bool profile = RSG::utilities->capturing_timestamps && profile_frame % 120 == 0;
	const uint64_t coordinator = profile ? Thread::get_caller_id() : 0;
	auto prepare_payload = [&](uint32_t p_index) {
		ShadowPreparation &preparation = shadow_preparations[p_index];
		if (profile) {
			preparation.worker = Thread::get_caller_id();
			preparation.begin_usec = OS::get_singleton()->get_ticks_usec();
		}

		RenderList *list = preparation.list;
		if (list->elements.size() > 1) {
			SortArray<RenderElement, RenderList::SortByKey> sorter;
			sorter.sort(list->elements.ptr(), list->elements.size());
		}
		list->element_info.resize(list->elements.size());
		list->instance_data.resize(list->elements.size());
		_fill_instance_payload(list, 0, list->elements.size());
		_fill_instance_runs(list, 0, list->elements.size(), preparation.render_info ? preparation.statistics.info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW] : nullptr);
		if (profile) {
			preparation.end_usec = OS::get_singleton()->get_ticks_usec();
		}
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const uint64_t queued = profile ? OS::get_singleton()->get_ticks_usec() : 0;
	auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("ShadowPassPayload");
		(*static_cast<decltype(prepare_payload) *>(p_data))(p_index);
	},
			&prepare_payload, shadow_preparations.size(), -1, true, SNAME("ShadowPassPayload"));
	pool->wait_for_group_task_completion(job);
	if (profile) {
		const uint64_t joined = OS::get_singleton()->get_ticks_usec();
		String rows;
		for (uint32_t index = 0; index < shadow_preparations.size(); index++) {
			const auto &preparation = shadow_preparations[index];
			rows += vformat("RenderPrep stage=ShadowPassPayload frame=%d pass=%d coordinator=%d queued_usec=%d joined_usec=%d worker=%d begin_usec=%d end_usec=%d work=%d", profile_frame, index, coordinator, queued, joined, preparation.worker, preparation.begin_usec, preparation.end_usec, preparation.list->elements.size()) + "\n";
		}
		print_line(rows);
	}

	auto merge = [&](uint32_t) {
		RenderList &destination = render_list[RENDER_LIST_SECONDARY];
		for (uint32_t index = 0; index < shadow_preparations.size(); index++) {
			RenderList &source = *shadow_preparations[index].list;
			auto &pass = scene_state.shadow_passes[index];
			pass.element_from = destination.elements.size();
			pass.element_count = source.elements.size();
			for (const auto &element : source.elements) {
				destination.elements.push_back(element);
			}
			for (const auto &info : source.element_info) {
				destination.element_info.push_back(info);
			}
			for (const auto &instance : source.instance_data) {
				destination.instance_data.push_back(instance);
			}
			for (auto *micro_pass : source.micro_passes) {
				destination.micro_passes.push_back(micro_pass);
			}
			destination.last_micro_pass = source.last_micro_pass;
			source.micro_passes.clear();
			source.last_micro_pass = nullptr;
		}
	};
	job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("ShadowPassMerge");
		(*static_cast<decltype(merge) *>(p_data))(p_index);
	},
			&merge, 1, 1, true, SNAME("ShadowPassMerge"));
	pool->wait_for_group_task_completion(job);
	for (ShadowPreparation &preparation : shadow_preparations) {
		if (preparation.render_info) {
			for (auto metric : { RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME, RSE::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME }) {
				preparation.render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW][metric] += preparation.statistics.info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW][metric];
			}
		}
		memdelete(preparation.list);
	}
	shadow_preparations.clear();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	const auto &payload = render_list[RENDER_LIST_SECONDARY].instance_data;
	if (!payload.is_empty()) {
		scene_state.grow_instance_buffer(RENDER_LIST_SECONDARY, payload.size(), false);
		void *destination = scene_state.instance_buffer[RENDER_LIST_SECONDARY].map_raw_for_upload(0u);
		memcpy(destination, payload.ptr(), payload.size() * sizeof(SceneState::InstanceData));
		rd->buffer_flush(scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u));
	}

	//render shadows one after the other, so this can be done un-barriered and the driver can optimize (as well as allow us to run compute at the same time)

	for (uint32_t i = 0; i < scene_state.shadow_passes.size(); i++) {
		//render passes need to be configured after instance buffer is done, since they need the latest version
		SceneState::ShadowPass &shadow_pass = scene_state.shadow_passes[i];
		shadow_pass.rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), shadow_pass.uniform_buffer_index, false);
	}

	RD::get_singleton()->draw_command_end_label();
}
void RenderForwardClustered::_render_shadow_end() {
	RD::get_singleton()->draw_command_begin_label("Shadow Render");

	for (SceneState::ShadowPass &shadow_pass : scene_state.shadow_passes) {
		RenderListParameters render_list_parameters(render_list[RENDER_LIST_SECONDARY].elements.ptr() + shadow_pass.element_from, render_list[RENDER_LIST_SECONDARY].element_info.ptr() + shadow_pass.element_from, shadow_pass.element_count, shadow_pass.flip_cull, shadow_pass.pass_mode, true, false, shadow_pass.rp_uniform_set, false, Vector2(), shadow_pass.lod_distance_multiplier, shadow_pass.screen_mesh_lod_threshold, 1, shadow_pass.element_from);
		render_list_parameters.micro_geometry = shadow_pass.micro_geometry;
		_render_list_with_draw_list(&render_list_parameters, shadow_pass.framebuffer, shadow_pass.clear_depth ? RD::DRAW_CLEAR_DEPTH : RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0, shadow_pass.rect);
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_particle_collider_heightfield(RID p_fb, const Transform3D &p_cam_transform, const Projection &p_cam_projection, const PagedArray<RenderGeometryInstance *> &p_instances, const double *p_origin, RID p_scenario, uint32_t p_layers) {
	RENDER_TIMESTAMP("Setup GPUParticlesCollisionHeightField3D");

	RD::get_singleton()->draw_command_begin_label("Render Collider Heightfield");

	RenderSceneDataRD scene_data;
	scene_data.flip_y = true;
	scene_data.cam_projection = p_cam_projection;
	scene_data.cam_transform = p_cam_transform;
	for (int axis = 0; axis < 3; axis++) {
		scene_data.cam_origin[axis] = p_origin[axis];
	}
	scene_data.view_projection[0] = p_cam_projection;
	scene_data.z_near = 0.0;
	scene_data.z_far = p_cam_projection.get_z_far();
	scene_data.dual_paraboloid_side = 0;
	scene_data.opaque_prepass_threshold = 0.0;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_cam_transform;
	for (int axis = 0; axis < 3; axis++) {
		scene_data.main_cam_origin[axis] = p_origin[axis];
	}
	scene_data.shadow_pass = true;
	scene_data.camera_visible_layers = p_layers;
	micro_geometry_shadow_planes = p_cam_projection.get_projection_planes(Transform3D(p_cam_transform.basis, Vector3()));
	for (int axis = 0; axis < 3; axis++) {
		micro_geometry_shadow_origin[axis] = p_origin[axis];
	}
	micro_geometry_pass_size = RD::get_singleton()->framebuffer_get_size(p_fb);

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.scenario = p_scenario;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_fb);
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, screen_size, Color(), false, false, false);

	PassMode pass_mode = PASS_MODE_SHADOW;

	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render Collider Heightfield");

	{
		//regular forward for now
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), false, pass_mode, true, false, rp_uniform_set);
		render_list_params.micro_geometry = render_list[RENDER_LIST_SECONDARY].last_micro_pass;
		_render_list_with_draw_list(&render_list_params, p_fb, RD::DRAW_CLEAR_ALL);
	}
	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region, float p_exposure_normalization, const double *p_origin) {
	RENDER_TIMESTAMP("Setup Rendering 3D Material");

	RD::get_singleton()->draw_command_begin_label("Render 3D Material");

	RenderSceneDataRD scene_data;
	scene_data.cam_projection = p_cam_projection;
	scene_data.cam_transform = p_cam_transform;
	for (int axis = 0; axis < 3; axis++) {
		scene_data.cam_origin[axis] = p_origin ? p_origin[axis] : double(p_cam_transform.origin[axis]);
	}
	scene_data.view_projection[0] = p_cam_projection;
	scene_data.dual_paraboloid_side = 0;
	scene_data.material_uv2_mode = false;
	scene_data.opaque_prepass_threshold = 0.0f;
	scene_data.emissive_exposure_normalization = p_exposure_normalization;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_cam_transform;
	for (int axis = 0; axis < 3; axis++) {
		scene_data.main_cam_origin[axis] = scene_data.cam_origin[axis];
	}

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	scene_shader.enable_advanced_shader_group();

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_region.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());

	PassMode pass_mode = PASS_MODE_DEPTH_MATERIAL;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render 3D Material");

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, true, false, rp_uniform_set);
		render_list_params.micro_geometry = render_list[RENDER_LIST_SECONDARY].last_micro_pass;
		_select_micro_geometry(render_list_params.micro_geometry);
		//regular forward for now
		Vector<Color> clear = {
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)
		};

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear, 0.0f, 0, p_region);
		_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count);
		RD::get_singleton()->draw_list_end();
		if (render_list_params.micro_geometry) {
			micro_geometry->submit_feedback(render_list_params.micro_geometry->gpu);
		}
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_uv2(const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) {
	RENDER_TIMESTAMP("Setup Rendering UV2");

	RD::get_singleton()->draw_command_begin_label("Render UV2");

	RenderSceneDataRD scene_data;
	scene_data.dual_paraboloid_side = 0;
	scene_data.material_uv2_mode = true;
	scene_data.opaque_prepass_threshold = 0.0;
	scene_data.emissive_exposure_normalization = -1.0;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	scene_shader.enable_advanced_shader_group();

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_region.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());

	PassMode pass_mode = PASS_MODE_DEPTH_MATERIAL;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render 3D Material");

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, true, false, rp_uniform_set, true);
		render_list_params.micro_geometry = render_list[RENDER_LIST_SECONDARY].last_micro_pass;
		_select_micro_geometry(render_list_params.micro_geometry);
		//regular forward for now
		Vector<Color> clear = {
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)
		};
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear, 0.0f, 0, p_region);

		const int uv_offset_count = 9;
		static const Vector2 uv_offsets[uv_offset_count] = {
			Vector2(-1, 1),
			Vector2(1, 1),
			Vector2(1, -1),
			Vector2(-1, -1),
			Vector2(-1, 0),
			Vector2(1, 0),
			Vector2(0, -1),
			Vector2(0, 1),
			Vector2(0, 0),

		};

		for (int i = 0; i < uv_offset_count; i++) {
			Vector2 ofs = uv_offsets[i];
			ofs.x /= p_region.size.width;
			ofs.y /= p_region.size.height;
			render_list_params.uv_offset = ofs;
			_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count); //first wireframe, for pseudo conservative
		}
		render_list_params.uv_offset = Vector2();
		render_list_params.force_wireframe = false;
		_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count); //second regular triangles

		RD::get_singleton()->draw_list_end();
		if (render_list_params.micro_geometry) {
			micro_geometry->submit_feedback(render_list_params.micro_geometry->gpu);
		}
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_sdfgi(Ref<RenderSceneBuffersRD> p_render_buffers, const Vector3i &p_from, const Vector3i &p_size, const AABB &p_bounds, const PagedArray<RenderGeometryInstance *> &p_instances, const RID &p_albedo_texture, const RID &p_emission_texture, const RID &p_emission_aniso_texture, const RID &p_geom_facing_texture, float p_exposure_normalization) {
	RENDER_TIMESTAMP("Render SDFGI");

	RD::get_singleton()->draw_command_begin_label("Render SDFGI Voxel");

	RenderSceneDataRD scene_data;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	_update_render_base_uniform_set();

	// Indicate pipelines for SDFGI are required.
	global_pipeline_data_required.use_sdfgi = true;

	PassMode pass_mode = PASS_MODE_SDF;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	Vector3 half_size = p_bounds.size * 0.5;
	Vector3 center = p_bounds.position + half_size;

	//print_line("re-render " + p_from + " - " + p_size + " bounds " + p_bounds);
	for (int i = 0; i < 3; i++) {
		scene_state.ubo.sdf_offset[i] = p_from[i];
		scene_state.ubo.sdf_size[i] = p_size[i];
	}

	for (int i = 0; i < 3; i++) {
		Vector3 axis;
		axis[i] = 1.0;
		Vector3 up, right;
		int right_axis = (i + 1) % 3;
		int up_axis = (i + 2) % 3;
		up[up_axis] = 1.0;
		right[right_axis] = 1.0;

		Size2i fb_size;
		fb_size.x = p_size[right_axis];
		fb_size.y = p_size[up_axis];

		scene_data.cam_transform.origin = center + axis * half_size;
		scene_data.cam_transform.basis.set_column(0, right);
		scene_data.cam_transform.basis.set_column(1, up);
		scene_data.cam_transform.basis.set_column(2, axis);

		//print_line("pass: " + itos(i) + " xform " + scene_data.cam_transform);

		float h_size = half_size[right_axis];
		float v_size = half_size[up_axis];
		float d_size = half_size[i] * 2.0;
		scene_data.cam_projection.set_orthogonal(-h_size, h_size, -v_size, v_size, 0, d_size);
		//print_line("pass: " + itos(i) + " cam hsize: " + rtos(h_size) + " vsize: " + rtos(v_size) + " dsize " + rtos(d_size));

		Transform3D to_bounds;
		to_bounds.origin = p_bounds.position;
		to_bounds.basis.scale(p_bounds.size);

		RendererRD::MaterialStorage::store_transform(to_bounds.affine_inverse() * scene_data.cam_transform, scene_state.ubo.sdf_to_bounds);

		scene_data.emissive_exposure_normalization = p_exposure_normalization;
		uint32_t uniform_buffer_index = _setup_environment(&render_data, true, fb_size, fb_size, Color());

		RID rp_uniform_set = _setup_sdfgi_render_pass_uniform_set(p_albedo_texture, p_emission_texture, p_emission_aniso_texture, p_geom_facing_texture, RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

		HashMap<Size2i, RID>::Iterator E = sdfgi_framebuffer_size_cache.find(fb_size);
		if (!E) {
			RID fb = RD::get_singleton()->framebuffer_create_empty(fb_size);
			E = sdfgi_framebuffer_size_cache.insert(fb_size, fb);
		}

		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, true, false, rp_uniform_set, false);
		render_list_params.micro_geometry = render_list[RENDER_LIST_SECONDARY].last_micro_pass;
		_render_list_with_draw_list(&render_list_params, E->value);
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::base_uniforms_changed() {
	if (!render_base_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
		RD::get_singleton()->free_rid(render_base_uniform_set);
	}
	render_base_uniform_set = RID();
}

void RenderForwardClustered::_update_render_base_uniform_set() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	if (render_base_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set) || (lightmap_texture_array_version != light_storage->lightmap_array_get_version())) {
		if (render_base_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
			RD::get_singleton()->free_rid(render_base_uniform_set);
		}

		lightmap_texture_array_version = light_storage->lightmap_array_get_version();

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 2;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(scene_shader.shadow_sampler);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 3;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_omni_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 4;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_spot_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 5;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_area_light_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 6;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_reflection_probe_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 7;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_directional_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 8;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_capture_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::TextureStorage::get_singleton()->get_decal_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 13;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.binding = 14;
			u.append_id(sdfgi_get_ubo());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 15;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CanvasItemTextureFilter::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CanvasItemTextureRepeat::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 16;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(best_fit_normal.texture);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 17;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(dfg_lut.texture);
			uniforms.push_back(u);
		}

		{ // Lookup-table for Area Lights - Linearly transformed cosines (LTC)
			if (ltc.lut1_texture.is_null() || ltc.lut2_texture.is_null()) {
				Ref<Image> lut1_image;
				int dimensions = LTC_LUT_DIMENSIONS;
				int lut1_bytes = 4 * dimensions * dimensions;
				size_t lut1_size = lut1_bytes * 4; // float

				Vector<uint8_t> lut1_data;
				lut1_data.resize(lut1_size);

				memcpy(lut1_data.ptrw(), LTC_LUT1, lut1_size);
				lut1_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut1_data);

				ltc.lut1_texture = RS::get_singleton()->texture_2d_create(lut1_image);

				int lut2_bytes = 4 * dimensions * dimensions;
				size_t lut2_size = lut2_bytes * 4;

				Ref<Image> lut2_image;
				Vector<uint8_t> lut2_data;
				lut2_data.resize(lut2_size);

				memcpy(lut2_data.ptrw(), LTC_LUT2, lut2_size);
				lut2_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut2_data);

				ltc.lut2_texture = RS::get_singleton()->texture_2d_create(lut2_image);
			}
		}

		{
			RD::Uniform u;
			u.binding = 18;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
			u.append_id(RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc.lut1_texture));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 19;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
			u.append_id(RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc.lut2_texture));
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 20;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
			u.append_id(area_light_atlas);
			uniforms.push_back(u);
		}

		render_base_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, scene_shader.default_shader_rd, SCENE_UNIFORM_SET);
	}
}

RID RenderForwardClustered::_setup_render_pass_uniform_set(RenderListType p_render_list, const RenderDataRD *p_render_data, RID p_radiance_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index, bool p_use_directional_shadow_atlas) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	bool is_multiview = false;

	Ref<RenderSceneBuffersRD> rb; // handy for not having to fully type out p_render_data->render_buffers all the time...
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		rb = p_render_data->render_buffers;
		is_multiview = rb->get_view_count() > 1;
		if (rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
			// This will not be available when rendering reflection probes.
			rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
	}

	//default render buffer and scene state uniform set

	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[p_render_list].get_size(0u) == 0u) {
			// Any buffer will do since it's not used, so just create one.
			// We can't use scene_shader.default_vec4_xform_buffer because it's not dynamic.
			scene_state.instance_buffer[p_render_list].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[p_render_list].prepare_for_upload();
		}
		RID instance_buffer = scene_state.instance_buffer[p_render_list]._get(0u);
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		RID radiance_texture;
		if (p_radiance_texture.is_valid()) {
			radiance_texture = p_radiance_texture;
		} else {
			radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}
	{
		RID ref_texture = (p_render_data && p_render_data->reflection_atlas.is_valid()) ? light_storage->reflection_atlas_get_texture(p_render_data->reflection_atlas) : RID();
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		if (ref_texture.is_valid()) {
			u.append_id(ref_texture);
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture;
		if (p_render_data && p_render_data->shadow_atlas.is_valid()) {
			texture = RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_render_data->shadow_atlas);
		}
		if (!texture.is_valid()) {
			texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		if (p_use_directional_shadow_atlas && RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture().is_valid()) {
			u.append_id(RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture());
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
		}
		uniforms.push_back(u);
	}
	{
		Vector<RID> textures;
		textures.resize(scene_state.max_lightmaps * 2);

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			uint32_t current_lightmap_index = i < scene_state.max_lightmaps ? i : i - scene_state.max_lightmaps;

			if (p_render_data && current_lightmap_index < p_render_data->lightmaps->size()) {
				RID base = light_storage->lightmap_instance_get_lightmap((*p_render_data->lightmaps)[current_lightmap_index]);
				RID texture;

				if (i < scene_state.max_lightmaps) {
					// Lightmap
					texture = light_storage->lightmap_get_texture(base);
				} else {
					// Shadowmask
					texture = light_storage->shadowmask_get_texture(base);
				}

				if (texture.is_valid()) {
					RID rd_texture = texture_storage->texture_get_rd_texture(texture);
					textures.write[i] = rd_texture;
					continue;
				}
			}

			textures.write[i] = default_tex;
		}
		RD::Uniform u(RD::UNIFORM_TYPE_TEXTURE, 7, textures);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			if (p_render_data && i < (int)p_render_data->voxel_gi_instances->size()) {
				RID tex = gi.voxel_gi_instance_get_texture((*p_render_data->voxel_gi_instances)[i]);
				if (!tex.is_valid()) {
					tex = default_tex;
				}
				u.append_id(tex);
			} else {
				u.append_id(default_tex);
			}
		}

		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID cb = (p_render_data && p_render_data->cluster_buffer.is_valid()) ? p_render_data->cluster_buffer : scene_shader.default_vec4_xform_buffer;
		u.append_id(cb);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	{
		RD::Uniform u;
		u.binding = 24;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture;
		if (rb.is_valid() && rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH)) {
			texture = rb->get_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH);
		} else {
			texture = texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_DEPTH : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 25;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID bbt = rb_data.is_valid() ? rb->get_back_buffer_texture() : RID();
		RID texture = bbt.is_valid() ? bbt : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 26;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb_data->has_normal_roughness() ? rb_data->get_normal_roughness() : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_NORMAL : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 27;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID aot = rb.is_valid() && rb->has_texture(RB_SCOPE_SSAO, RB_FINAL) ? rb->get_texture(RB_SCOPE_SSAO, RB_FINAL) : RID();
		RID texture = aot.is_valid() ? aot : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 29;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 30;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID t;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			t = sdfgi->lightprobe_texture;
		}
		if (t.is_null()) {
			t = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		}
		u.append_id(t);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 31;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID t;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			t = sdfgi->occlusion_texture;
		}
		if (t.is_null()) {
			t = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		}
		u.append_id(t);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		RID voxel_gi;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_GI)) {
			Ref<RendererRD::GI::RenderBuffersGI> rbgi = rb->get_custom_data(RB_SCOPE_GI);
			voxel_gi = rbgi->get_voxel_gi_buffer();
		}
		u.append_id(voxel_gi.is_valid() ? voxel_gi : render_buffers_get_default_voxel_gi_buffer());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 33;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID vfog;
		if (rb_data.is_valid() && rb->has_custom_data(RB_SCOPE_FOG)) {
			Ref<RendererRD::Fog::VolumetricFog> fog = rb->get_custom_data(RB_SCOPE_FOG);
			vfog = fog->fog_map;
			if (vfog.is_null()) {
				vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
			}
		} else {
			vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		}
		u.append_id(vfog);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 34;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID ssil = rb.is_valid() && rb->has_texture(RB_SCOPE_SSIL, RB_FINAL) ? rb->get_texture(RB_SCOPE_SSIL, RB_FINAL) : RID();
		RID texture = ssil.is_valid() ? ssil : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 35;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID ssr;
		if (rb_data.is_valid()) {
			if (rb_data->ss_effects_data.ssr.half_size) {
				if (rb->has_texture(RB_SCOPE_SSR, RB_FINAL)) {
					ssr = rb->get_texture(RB_SCOPE_SSR, RB_FINAL);
				}
			} else {
				if (rb->has_texture(RB_SCOPE_SSR, RB_SSR)) {
					ssr = rb->get_texture(RB_SCOPE_SSR, RB_SSR);
				}
			}
		}

		RID texture = ssr.is_valid() ? ssr : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 36;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID ssr_mip_level = (rb_data.is_valid() && !rb_data->ss_effects_data.ssr.half_size && rb->has_texture(RB_SCOPE_SSR, RB_MIP_LEVEL)) ? rb->get_texture(RB_SCOPE_SSR, RB_MIP_LEVEL) : RID();
		RID texture = ssr_mip_level.is_valid() ? ssr_mip_level : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.default_shader_rd, RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderForwardClustered::_setup_sdfgi_render_pass_uniform_set(RID p_albedo_texture, RID p_emission_texture, RID p_emission_aniso_texture, RID p_geom_facing_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) == 0u) {
			// Any buffer will do since it's not used, so just create one.
			// We can't use scene_shader.default_vec4_xform_buffer because it's not dynamic.
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].prepare_for_upload();
		}
		RID instance_buffer = scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u);
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		// No radiance texture.
		RID radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}

	{
		// No reflection atlas.
		RID ref_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(ref_texture);
		uniforms.push_back(u);
	}

	{
		// No shadow atlas.
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		// No directional shadow atlas.
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		// No Lightmaps
		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			u.append_id(default_tex);
		}

		uniforms.push_back(u);
	}

	{
		// No VoxelGIs
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			u.append_id(default_tex);
		}

		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID cb = scene_shader.default_vec4_xform_buffer;
		u.append_id(cb);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	// actual sdfgi stuff

	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 24;
		u.append_id(p_albedo_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 25;
		u.append_id(p_emission_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 26;
		u.append_id(p_emission_aniso_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 27;
		u.append_id(p_geom_facing_texture);
		uniforms.push_back(u);
	}

	if (scene_shader.default_shader_sdfgi_rd.is_null()) {
		// The variant for SDF from the default material should only be retrieved when SDFGI is required.
		ERR_FAIL_NULL_V(scene_shader.default_material_shader_ptr, RID());
		scene_shader.enable_advanced_shader_group();
		scene_shader.default_shader_sdfgi_rd = scene_shader.default_material_shader_ptr->get_shader_variant(SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF, true);
		ERR_FAIL_COND_V(scene_shader.default_shader_sdfgi_rd.is_null(), RID());
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.default_shader_sdfgi_rd, RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderForwardClustered::_render_buffers_get_normal_texture(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);

	return rb_data->get_normal_roughness();
}

RID RenderForwardClustered::_render_buffers_get_velocity_texture(Ref<RenderSceneBuffersRD> p_render_buffers) {
	return p_render_buffers->get_velocity_buffer(false);
}

void RenderForwardClustered::environment_set_ssao_quality(RSE::EnvironmentSSAOQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::EnvironmentSSAOQuality::ENV_SSAO_QUALITY_VERY_LOW || p_quality > RSE::EnvironmentSSAOQuality::ENV_SSAO_QUALITY_ULTRA);
	ss_effects->ssao_set_quality(p_quality, p_half_size, p_adaptive_target, p_blur_passes, p_fadeout_from, p_fadeout_to);
}

void RenderForwardClustered::environment_set_ssil_quality(RSE::EnvironmentSSILQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::EnvironmentSSILQuality::ENV_SSIL_QUALITY_VERY_LOW || p_quality > RSE::EnvironmentSSILQuality::ENV_SSIL_QUALITY_ULTRA);
	ss_effects->ssil_set_quality(p_quality, p_half_size, p_adaptive_target, p_blur_passes, p_fadeout_from, p_fadeout_to);
}

void RenderForwardClustered::environment_set_ssr_half_size(bool p_half_size) {
	ERR_FAIL_NULL(ss_effects);
	ss_effects->ssr_set_half_size(p_half_size);
}

void RenderForwardClustered::environment_set_ssr_roughness_quality(RSE::EnvironmentSSRRoughnessQuality p_quality) {
	WARN_PRINT_ONCE("environment_set_ssr_roughness_quality has been deprecated and no longer does anything.");
}

void RenderForwardClustered::sub_surface_scattering_set_quality(RSE::SubSurfaceScatteringQuality p_quality) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::SubSurfaceScatteringQuality::SUB_SURFACE_SCATTERING_QUALITY_DISABLED || p_quality > RSE::SubSurfaceScatteringQuality::SUB_SURFACE_SCATTERING_QUALITY_HIGH);
	ss_effects->sss_set_quality(p_quality);
}

void RenderForwardClustered::sub_surface_scattering_set_scale(float p_scale, float p_depth_scale) {
	ERR_FAIL_NULL(ss_effects);
	ss_effects->sss_set_scale(p_scale, p_depth_scale);
}

RenderForwardClustered *RenderForwardClustered::singleton = nullptr;

void RenderForwardClustered::sdfgi_update(const Ref<RenderSceneBuffers> &p_render_buffers, RID p_environment, const Vector3 &p_world_position) {
	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND(rb.is_null());
	if (rb->has_custom_data(RB_SCOPE_SDFGI)) {
		rb->set_custom_data(RB_SCOPE_SDFGI, Ref<RenderBufferCustomDataRD>());
	}
}

int RenderForwardClustered::sdfgi_get_pending_region_count(const Ref<RenderSceneBuffers> &p_render_buffers) const {
	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), 0);

	if (!rb->has_custom_data(RB_SCOPE_SDFGI)) {
		return 0;
	}
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);

	int dirty_count = 0;
	for (const RendererRD::GI::SDFGI::Cascade &c : sdfgi->cascades) {
		if (c.dirty_regions == RendererRD::GI::SDFGI::Cascade::DIRTY_ALL) {
			dirty_count++;
		} else {
			for (int j = 0; j < 3; j++) {
				if (c.dirty_regions[j] != 0) {
					dirty_count++;
				}
			}
		}
	}

	return dirty_count;
}

AABB RenderForwardClustered::sdfgi_get_pending_region_bounds(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const {
	AABB bounds;
	Vector3i from;
	Vector3i size;

	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), AABB());
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	ERR_FAIL_COND_V(sdfgi.is_null(), AABB());

	int c = sdfgi->get_pending_region_data(p_region, from, size, bounds);
	ERR_FAIL_COND_V(c == -1, AABB());
	return bounds;
}

uint32_t RenderForwardClustered::sdfgi_get_pending_region_cascade(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const {
	AABB bounds;
	Vector3i from;
	Vector3i size;

	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), -1);
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	ERR_FAIL_COND_V(sdfgi.is_null(), -1);

	return sdfgi->get_pending_region_data(p_region, from, size, bounds);
}

void RenderForwardClustered::GeometryInstanceForwardClustered::_mark_dirty() {
	set_micro_geometry_raster_only(false);
	persistent_surfaces_dirty = true;
	_mark_instance_data_dirty();
	if (dirty_list_element.in_list()) {
		return;
	}

	//clear surface caches
	GeometryInstanceSurfaceDataCache *surf = surface_caches;

	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		if (surf->micro_geometry_element.in_list()) {
			RenderForwardClustered::get_singleton()->micro_geometry_generation++;
		}
		if (surf->micro_geometry_rt_element.in_list()) {
			RenderForwardClustered::get_singleton()->micro_geometry_rt_generation++;
		}
		RenderForwardClustered::get_singleton()->geometry_instance_surface_alloc.free(surf);
		surf = next;
	}

	surface_caches = nullptr;

	RenderForwardClustered::get_singleton()->geometry_instance_dirty_list.add(&dirty_list_element);
}

void RenderForwardClustered::_update_global_pipeline_data_requirements_from_project() {
	const int msaa_3d_mode = GLOBAL_GET_CACHED(int, "rendering/anti_aliasing/quality/msaa_3d");
	const bool directional_shadow_16_bits = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/directional_shadow/16_bits");
	const bool positional_shadow_16_bits = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/positional_shadow/atlas_16_bits");
	global_pipeline_data_required.use_16_bit_shadows = directional_shadow_16_bits || positional_shadow_16_bits;
	global_pipeline_data_required.use_32_bit_shadows = !directional_shadow_16_bits || !positional_shadow_16_bits;
	global_pipeline_data_required.texture_samples = RenderSceneBuffersRD::msaa_to_samples(RSE::ViewportMSAA(msaa_3d_mode));
}

void RenderForwardClustered::_update_global_pipeline_data_requirements_from_light_storage() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	global_pipeline_data_required.use_shadow_cubemaps = light_storage->get_shadow_cubemaps_used();
	global_pipeline_data_required.use_shadow_dual_paraboloid = light_storage->get_shadow_dual_paraboloid_used();
}

void RenderForwardClustered::_geometry_instance_add_surface_with_material(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, SceneShaderForwardClustered::MaterialData *p_material, RID p_material_rid, uint32_t p_shader_id, RID p_mesh) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint32_t flags = 0;

	if (p_material->shader_data->uses_sss) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SUBSURFACE_SCATTERING;
		global_surface_data.sss_used = true;
	}

	if (p_material->shader_data->uses_screen_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SCREEN_TEXTURE;
		global_surface_data.screen_texture_used = true;
	}

	if (p_material->shader_data->uses_depth_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_DEPTH_TEXTURE;
		global_surface_data.depth_texture_used = true;
	}

	if (p_material->shader_data->uses_normal_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_NORMAL_TEXTURE;
		global_surface_data.normal_texture_used = true;
	}

	if (ginstance->data->cast_double_sided_shadows) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS;
	}

	if (p_material->shader_data->stencil_enabled) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_STENCIL;
	}

	// Raster-side pass classification.
	if (p_material->shader_data->uses_alpha_pass()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA;
		if (p_material->shader_data->uses_depth_in_alpha_pass()) {
			flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
			flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
		}
	} else {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE;
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
	}

	// RT-side pass classification; mirrors `flags` unless the shader has
	// `#if defined(RT)` divergence.
	uint32_t rt_pass_flags = 0;
	if (p_material->shader_data->rt_uses_alpha_pass()) {
		rt_pass_flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA;
		if (p_material->shader_data->rt_uses_depth_in_alpha_pass()) {
			rt_pass_flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
			rt_pass_flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
		}
	} else {
		rt_pass_flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE;
		rt_pass_flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
		rt_pass_flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
	}

	if (p_material->shader_data->uses_particle_trails) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS;
	}

	if (p_material->shader_data->is_animated()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR;
	}

	if (p_material->shader_data->stencil_enabled) {
		if (p_material->shader_data->stencil_flags & SceneShaderForwardClustered::ShaderData::STENCIL_FLAG_READ) {
			// Stencil materials which read from the stencil buffer must be in the alpha pass.
			// This is critical to preserve compatibility once we'll have the compositor.
			if (!(flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
				String shader_path = p_material->shader_data->path.is_empty() ? "" : "(" + p_material->shader_data->path + ")";
				ERR_PRINT_ED(vformat("Attempting to use a shader %s that reads stencil but is not in the alpha queue. Ensure the material uses alpha blending or has depth_draw disabled or depth_test disabled.", shader_path));
			}
		}
	}

	SceneShaderForwardClustered::MaterialData *material_shadow = nullptr;
	void *surface_shadow = nullptr;
	if (p_material->shader_data->uses_shared_shadow_material()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SHARED_SHADOW_MATERIAL;
		material_shadow = static_cast<SceneShaderForwardClustered::MaterialData *>(RendererRD::MaterialStorage::get_singleton()->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));

		RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);
		if (shadow_mesh.is_valid()) {
			surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, p_surface);
		}
	} else {
		material_shadow = p_material;
	}

	GeometryInstanceSurfaceDataCache *sdcache = geometry_instance_surface_alloc.alloc();

	sdcache->flags = flags;
	sdcache->rt_pass_flags = rt_pass_flags;
	sdcache->rtxdi_material_flags = p_material->shader_data->get_surface_material_flags();
	if ((sdcache->rtxdi_material_flags & GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_UNSUPPORTED) && !p_material->rtxdi_diagnostic_reported) {
		WARN_PRINT(vformat("Forward Clustered RTXDI surface: material using shader %s cannot be represented consistently by the raster surface and ray-traced material contracts; affected surfaces render as a magenta diagnostic.", p_material->shader_data->path.is_empty() ? String("<inline>") : p_material->shader_data->path));
		p_material->rtxdi_diagnostic_reported = true;
	}

	sdcache->shader = p_material->shader_data;
	sdcache->material = p_material;
	sdcache->material_rid = p_material_rid;
	sdcache->material_uniform_set = p_material->uniform_set;
	sdcache->surface = mesh_storage->mesh_get_surface(p_mesh, p_surface);
	sdcache->primitive = mesh_storage->mesh_surface_get_primitive(sdcache->surface);
	sdcache->surface_index = p_surface;

	if (ginstance->data->dirty_dependencies) {
		RSG::utilities->base_update_dependency(p_mesh, &ginstance->data->dependency_tracker);
	}

	//shadow
	sdcache->shader_shadow = material_shadow->shader_data;
	sdcache->material_uniform_set_shadow = material_shadow->uniform_set;

	sdcache->surface_shadow = surface_shadow ? surface_shadow : sdcache->surface;

	sdcache->owner = ginstance;

	sdcache->next = ginstance->surface_caches;
	ginstance->surface_caches = sdcache;

	//sortkey

	sdcache->sort.sort_key1 = 0;
	sdcache->sort.sort_key2 = 0;

	sdcache->sort.surface_index = p_surface;
	sdcache->sort.material_id_hi = (p_material_rid.get_local_index() & 0xFF000000) >> 24;
	sdcache->sort.material_id_lo = (p_material_rid.get_local_index() & 0x00FFFFFF);
	sdcache->sort.shader_id = p_shader_id;
	sdcache->sort.geometry_id = p_mesh.get_local_index(); //only meshes can repeat anyway
	sdcache->sort.uses_forward_gi = ginstance->can_sdfgi;
	sdcache->sort.priority = p_material->priority;
	sdcache->sort.uses_projector = ginstance->using_projectors;
	sdcache->sort.uses_softshadow = ginstance->using_softshadows;

	uint64_t format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(sdcache->surface);
	if (p_material->shader_data->uses_tangent && !p_material->shader_data->writes_tangent && !(format & RSE::ARRAY_FORMAT_TANGENT)) {
		String shader_path = p_material->shader_data->path.is_empty() ? "" : "(" + p_material->shader_data->path + ")";
		String mesh_path = mesh_storage->mesh_get_path(p_mesh).is_empty() ? "" : "(" + mesh_storage->mesh_get_path(p_mesh) + ")";
		WARN_PRINT_ED(vformat("Attempting to use a shader %s that requires tangents with a mesh %s that doesn't contain tangents. Ensure that meshes are imported with the 'ensure_tangents' option. If creating your own meshes, add an `ARRAY_TANGENT` array (when using ArrayMesh) or call `generate_tangents()` (when using SurfaceTool).", shader_path, mesh_path));
	}

#if PRELOAD_PIPELINES_ON_SURFACE_CACHE_CONSTRUCTION
	if (!sdcache->compilation_dirty_element.in_list()) {
		geometry_surface_compilation_dirty_list.add(&sdcache->compilation_dirty_element);
	}

	if (!sdcache->compilation_all_element.in_list()) {
		geometry_surface_compilation_all_list.add(&sdcache->compilation_all_element);
	}
#endif
}

void RenderForwardClustered::_geometry_instance_add_surface_with_material_chain(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, SceneShaderForwardClustered::MaterialData *p_material, RID p_mat_src, RID p_mesh) {
	SceneShaderForwardClustered::MaterialData *material = p_material;
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	_geometry_instance_add_surface_with_material(ginstance, p_surface, material, p_mat_src, material_storage->material_get_shader_id(p_mat_src), p_mesh);

	while (material->next_pass.is_valid()) {
		RID next_pass = material->next_pass;
		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(next_pass, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (!material || !material->shader_data->is_valid()) {
			break;
		}
		if (ginstance->data->dirty_dependencies) {
			material_storage->material_update_dependency(next_pass, &ginstance->data->dependency_tracker);
		}
		_geometry_instance_add_surface_with_material(ginstance, p_surface, material, next_pass, material_storage->material_get_shader_id(next_pass), p_mesh);
	}
}

void RenderForwardClustered::_geometry_instance_add_surface(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, RID p_material, RID p_mesh) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RID m_src;

	m_src = ginstance->scene_data->material_override.is_valid() ? ginstance->scene_data->material_override : p_material;

	SceneShaderForwardClustered::MaterialData *material = nullptr;

	if (m_src.is_valid()) {
		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(m_src, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (!material || !material->shader_data->is_valid()) {
			material = nullptr;
		}
	}

	if (material) {
		if (ginstance->data->dirty_dependencies) {
			material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
		}
	} else {
		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		m_src = scene_shader.default_material;
	}

	ERR_FAIL_NULL(material);

	_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material, m_src, p_mesh);

	if (ginstance->scene_data->material_overlay.is_valid()) {
		m_src = ginstance->scene_data->material_overlay;

		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(m_src, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (material && material->shader_data->is_valid()) {
			if (ginstance->data->dirty_dependencies) {
				material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
			}

			_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material, m_src, p_mesh);
		}
	}
}

void RenderForwardClustered::_geometry_instance_update(RenderGeometryInstance *p_geometry_instance) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_geometry_instance);

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_begin();
	}

	//add geometry for drawing
	switch (ginstance->scene_data->base_type) {
		case RSE::INSTANCE_MESH: {
			const RID *materials = nullptr;
			uint32_t surface_count;
			RID mesh = ginstance->scene_data->base;

			materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
			if (materials) {
				//if no materials, no surfaces.
				const RID *inst_materials = ginstance->scene_data->materials.ptr();
				uint32_t surf_mat_count = ginstance->scene_data->materials.size();

				for (uint32_t j = 0; j < surface_count; j++) {
					RID material = (j < surf_mat_count && inst_materials[j].is_valid()) ? inst_materials[j] : materials[j];
					_geometry_instance_add_surface(ginstance, j, material, mesh);
				}
			}

			ginstance->instance_count = 1;

		} break;

		case RSE::INSTANCE_MULTIMESH: {
			RID mesh = mesh_storage->multimesh_get_mesh(ginstance->scene_data->base);
			if (mesh.is_valid()) {
				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t j = 0; j < surface_count; j++) {
						_geometry_instance_add_surface(ginstance, j, materials[j], mesh);
					}
				}

				ginstance->instance_count = mesh_storage->multimesh_get_instances_to_draw(ginstance->scene_data->base);
			}

		} break;
#if 0
		case RSE::INSTANCE_IMMEDIATE: {
			RasterizerStorageGLES3::Immediate *immediate = storage->immediate_owner.get_or_null(inst->base);
			ERR_CONTINUE(!immediate);

			_add_geometry(immediate, inst, nullptr, -1, p_depth_pass, p_shadow_pass);

		} break;
#endif
		case RSE::INSTANCE_PARTICLES: {
			int draw_passes = particles_storage->particles_get_draw_passes(ginstance->scene_data->base);

			for (int j = 0; j < draw_passes; j++) {
				RID mesh = particles_storage->particles_get_draw_pass_mesh(ginstance->scene_data->base, j);
				if (!mesh.is_valid()) {
					continue;
				}

				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t k = 0; k < surface_count; k++) {
						_geometry_instance_add_surface(ginstance, k, materials[k], mesh);
					}
				}
			}

			ginstance->instance_count = particles_storage->particles_get_amount(ginstance->scene_data->base, ginstance->trail_steps);

		} break;

		default: {
		}
	}

	//Fill push constant

	ginstance->base_flags = 0;

	bool store_transform = true;
	if (ginstance->scene_data->base_type == RSE::INSTANCE_MULTIMESH) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		if (mesh_storage->multimesh_get_transform_format(ginstance->scene_data->base) == RSE::MULTIMESH_TRANSFORM_2D) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D;
		}
		if (mesh_storage->multimesh_uses_colors(ginstance->scene_data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		}
		if (mesh_storage->multimesh_uses_custom_data(ginstance->scene_data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;
		}
		if (mesh_storage->multimesh_uses_indirect(ginstance->scene_data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT;
		}

		ginstance->transforms_uniform_set = mesh_storage->multimesh_get_3d_uniform_set(ginstance->scene_data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

	} else if (ginstance->scene_data->base_type == RSE::INSTANCE_PARTICLES) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_PARTICLES;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;

		//for particles, stride is the trail size
		ginstance->base_flags |= (ginstance->trail_steps << INSTANCE_DATA_FLAGS_PARTICLE_TRAIL_SHIFT);

		if (!particles_storage->particles_is_using_local_coords(ginstance->scene_data->base)) {
			store_transform = false;
		}
		ginstance->transforms_uniform_set = particles_storage->particles_get_instance_buffer_uniform_set(ginstance->scene_data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

		if (particles_storage->particles_get_frame_counter(ginstance->scene_data->base) == 0) {
			// Particles haven't been cleared or updated, update once now to ensure they are ready to render.
			particles_storage->update_particles();
		}

		if (ginstance->data->dirty_dependencies) {
			particles_storage->particles_update_dependency(ginstance->scene_data->base, &ginstance->data->dependency_tracker);
		}
	} else if (ginstance->scene_data->base_type == RSE::INSTANCE_MESH) {
		if (mesh_storage->skeleton_is_valid(ginstance->scene_data->skeleton)) {
			ginstance->transforms_uniform_set = mesh_storage->skeleton_get_3d_uniform_set(ginstance->scene_data->skeleton, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);
			if (ginstance->data->dirty_dependencies) {
				mesh_storage->skeleton_update_dependency(ginstance->scene_data->skeleton, &ginstance->data->dependency_tracker);
			}
		} else {
			ginstance->transforms_uniform_set = RID();
		}
	}

	ginstance->store_transform_cache = store_transform;
	ginstance->can_sdfgi = false;

	if (!RendererRD::LightStorage::get_singleton()->lightmap_instance_is_valid(ginstance->lightmap_instance)) {
		if (ginstance->voxel_gi_instances[0].is_null() && (ginstance->scene_data->baked_light || ginstance->scene_data->dynamic_gi)) {
			ginstance->can_sdfgi = true;
		}
	}

	if (ginstance->data->dirty_dependencies) {
		if (ginstance->scene_data->base_type == RSE::INSTANCE_MESH || ginstance->scene_data->base_type == RSE::INSTANCE_MULTIMESH) {
			RID mesh = ginstance->scene_data->base_type == RSE::INSTANCE_MULTIMESH ? mesh_storage->multimesh_get_mesh(ginstance->scene_data->base) : ginstance->scene_data->base;
			mesh_storage->get_micro_geometry_storage()->update_dependency(mesh_storage->mesh_get_micro_geometry_asset(mesh), &ginstance->data->dependency_tracker);
		}
		ginstance->data->dependency_tracker.update_end();
		ginstance->data->dirty_dependencies = false;
	}

	ginstance->dirty_list_element.remove_from_list();
	ginstance->_mark_instance_data_dirty();
}

static RD::FramebufferFormatID _get_rtxdi_surface_framebuffer_format_for_pipeline(bool p_can_be_storage) {
	Vector<RD::AttachmentFormat> attachments;
	RD::AttachmentFormat attachment;
	attachment.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	for (RD::DataFormat format : rtxdi_surface_formats) {
		attachment.format = format;
		attachments.push_back(attachment);
	}
	attachment.format = RenderSceneBuffersRD::get_depth_format(false, false, p_can_be_storage);
	attachment.usage_flags = RenderSceneBuffersRD::get_depth_usage_bits(false, false, p_can_be_storage);
	attachments.push_back(attachment);
	return RD::get_singleton()->framebuffer_format_create(attachments);
}

static RD::FramebufferFormatID _get_depth_framebuffer_format_for_pipeline(bool p_can_be_storage, RD::TextureSamples p_samples, bool p_normal_roughness, bool p_voxelgi) {
	const bool multisampling = p_samples > RD::TEXTURE_SAMPLES_1;
	RD::AttachmentFormat attachment;
	attachment.samples = p_samples;

	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	attachment.format = RenderSceneBuffersRD::get_depth_format(false, multisampling, p_can_be_storage);
	attachment.usage_flags = RenderSceneBuffersRD::get_depth_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	if (p_normal_roughness) {
		attachment.format = RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_format();
		attachment.usage_flags = RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	}

	if (p_voxelgi) {
		attachment.format = RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_format();
		attachment.usage_flags = RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	}

	thread_local Vector<RD::FramebufferPass> passes;
	passes.resize(1);
	passes.ptrw()[0].color_attachments.resize(attachments.size() - 1);

	int *color_attachments = passes.ptrw()[0].color_attachments.ptrw();
	for (int64_t i = 1; i < attachments.size(); i++) {
		color_attachments[i - 1] = (attachments[i].usage_flags == RD::AttachmentFormat::UNUSED_ATTACHMENT) ? RD::ATTACHMENT_UNUSED : i;
	}

	passes.ptrw()[0].depth_attachment = 0;

	return RD::get_singleton()->framebuffer_format_create_multipass(Vector<RD::AttachmentFormat>(attachments), passes);
}

static RD::FramebufferFormatID _get_shadow_cubemap_framebuffer_format_for_pipeline() {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_cubemap_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_cubemap_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

static RD::FramebufferFormatID _get_shadow_atlas_framebuffer_format_for_pipeline(bool p_use_16_bits) {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_shadow_atlas_depth_format(p_use_16_bits);
	attachment.usage_flags = RendererRD::LightStorage::get_shadow_atlas_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

static RD::FramebufferFormatID _get_reflection_probe_depth_framebuffer_format_for_pipeline() {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_reflection_probe_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

void RenderForwardClustered::_mesh_compile_pipeline_for_surface(SceneShaderForwardClustered::ShaderData *p_shader, void *p_mesh_surface, bool p_ubershader, bool p_instanced_surface, RSE::PipelineSource p_source, SceneShaderForwardClustered::ShaderData::PipelineKey &r_pipeline_key, Vector<ShaderPipelinePair> *r_pipeline_pairs) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint64_t input_mask = p_shader->get_vertex_input_mask(r_pipeline_key.version, p_ubershader);
	bool pipeline_motion_vectors = r_pipeline_key.version == SceneShaderForwardClustered::PIPELINE_VERSION_RTXDI_SURFACE;
	bool emulate_point_size = p_shader->uses_point_size && scene_shader.emulate_point_size;
	r_pipeline_key.vertex_format_id = mesh_storage->mesh_surface_get_vertex_format(p_mesh_surface, input_mask, p_instanced_surface, pipeline_motion_vectors, emulate_point_size);
	r_pipeline_key.ubershader = p_ubershader;

	p_shader->pipeline_hash_map.compile_pipeline(r_pipeline_key, r_pipeline_key.hash(), p_source, p_ubershader);

	if (r_pipeline_pairs != nullptr) {
		r_pipeline_pairs->push_back({ p_shader, r_pipeline_key });
	}
}

void RenderForwardClustered::_mesh_compile_pipelines_for_surface(const SurfacePipelineData &p_surface, const GlobalPipelineData &p_global, RSE::PipelineSource p_source, Vector<ShaderPipelinePair> *r_pipeline_pairs) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	const bool buffers_can_be_storage = _render_buffers_can_be_storage();

	// Set the attributes common to all pipelines.
	SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key;
	pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
	pipeline_key.primitive_type = mesh_storage->mesh_surface_get_primitive(p_surface.mesh_surface);
	pipeline_key.wireframe = false;

	pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_RTXDI_SURFACE;
	pipeline_key.framebuffer_format_id = _get_rtxdi_surface_framebuffer_format_for_pipeline(buffers_can_be_storage);
	_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

	if (!p_surface.uses_depth) {
		return;
	}

	if (p_global.use_normal_and_roughness) {
		// A lot of different effects rely on normal and roughness being written to during the depth pass.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), true, false);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_voxelgi) {
		// Depth pass with VoxelGI support.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), true, true);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_sdfgi) {
		// Depth pass with SDFGI support.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

		// Depth pass with SDFGI support for an empty framebuffer.
		pipeline_key.framebuffer_format_id = RD::get_singleton()->framebuffer_format_create_empty();
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// The dedicated depth passes use a different version of the surface and the shader.
	pipeline_key.primitive_type = mesh_storage->mesh_surface_get_primitive(p_surface.mesh_surface_shadow);
	pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
	pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false);
	_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

	if (p_global.use_shadow_dual_paraboloid) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP;
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_shadow_cubemaps) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_shadow_cubemap_framebuffer_format_for_pipeline();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// Atlas shadowmaps (omni lights) can be in both 16-bit and 32-bit versions.
	const uint32_t use_16_bits_start = p_global.use_32_bit_shadows ? 0 : 1;
	const uint32_t use_16_bits_iterations = p_global.use_16_bit_shadows ? 2 : 1;
	for (uint32_t use_16_bits = use_16_bits_start; use_16_bits < use_16_bits_iterations; use_16_bits++) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_shadow_atlas_framebuffer_format_for_pipeline(use_16_bits);
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

		if (p_global.use_shadow_dual_paraboloid) {
			pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP;
			_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
		}
	}

	if (p_global.use_reflection_probes) {
		// Depth pass for reflection probes. Normally this will be redundant as the format is the exact same as the shadow cubemap.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_reflection_probe_depth_framebuffer_format_for_pipeline();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}
}

void RenderForwardClustered::_mesh_generate_all_pipelines_for_surface_cache(GeometryInstanceSurfaceDataCache *p_surface_cache, const GlobalPipelineData &p_global) {
	SurfacePipelineData surface;
	surface.mesh_surface = p_surface_cache->surface;
	surface.mesh_surface_shadow = p_surface_cache->surface_shadow;
	surface.shader = p_surface_cache->shader;
	surface.shader_shadow = p_surface_cache->shader_shadow;
	surface.instanced = p_surface_cache->owner->scene_data->mesh_instance.is_valid();
	surface.uses_depth = (p_surface_cache->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE | GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW)) != 0;
	_mesh_compile_pipelines_for_surface(surface, p_global, RSE::PIPELINE_SOURCE_SURFACE);
}

void RenderForwardClustered::_update_dirty_geometry_instances() {
	RENDER_TIMESTAMP("Geometry Dirty Instances");
	const uint64_t profile_frame = RSG::rasterizer->get_frame_number();
	const bool profile_preparation = RSG::utilities->capturing_timestamps && profile_frame % 120 == 0;
	const uint64_t coordinator = profile_preparation ? Thread::get_caller_id() : 0;
	const uint64_t owner_begin = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
	uint64_t geometry_count = 0;
	uint64_t motion_count = 0;
	uint64_t worker = 0;
	uint64_t job_begin = 0;
	uint64_t job_end = 0;
	uint64_t queued = 0;
	uint64_t joined = 0;
	uint32_t jobs = 0;
	while (geometry_instance_dirty_list.first()) {
		_geometry_instance_update(geometry_instance_dirty_list.first()->self());
		if (profile_preparation) {
			geometry_count++;
		}
	}

	const uint64_t geometry_end = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	LocalVector<RenderGeometryInstance *> dirty_instances;
	auto collect = [&](uint32_t) {
		if (profile_preparation) {
			worker = Thread::get_caller_id();
			job_begin = OS::get_singleton()->get_ticks_usec();
		}
		for (auto *entry = instance_motion_update_frame != frame ? instance_motion_update_list.first() : nullptr; entry;) {
			if (profile_preparation) {
				motion_count++;
			}
			auto *next = entry->next();
			GeometryInstanceForwardClustered *instance = entry->self();
			if (instance->last_aged_frame == frame) {
				entry = next;
				continue;
			}
			instance->age_out_motion(frame);
			instance->_mark_instance_data_dirty();
			const bool mm_moving = instance->scene_data->base_type == RSE::INSTANCE_MULTIMESH && RendererRD::MeshStorage::get_singleton()->multimesh_get_last_change(instance->scene_data->base) + 1 >= frame;
			if (instance->transform_status == GeometryInstanceForwardClustered::NONE && !mm_moving) {
				instance->motion_update_element.remove_from_list();
			}
			entry = next;
		}

		instance_motion_update_frame = frame;

		while (instance_data_dirty_list.first()) {
			GeometryInstanceForwardClustered *instance = instance_data_dirty_list.first()->self();
			instance->instance_data_dirty_element.remove_from_list();
			dirty_instances.push_back(instance);
		}
		if (profile_preparation) {
			job_end = OS::get_singleton()->get_ticks_usec();
		}
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if ((instance_motion_update_frame != frame && instance_motion_update_list.first()) || instance_data_dirty_list.first()) {
		jobs = 1;
		queued = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
		auto job = pool->add_native_group_task([](void *p_data, uint32_t p_index) {
			GodotProfileZone("GeometryMotionPreparation");
			(*static_cast<decltype(collect) *>(p_data))(p_index);
		},
				&collect, 1, 1, true, SNAME("GeometryMotionPreparation"));
		pool->wait_for_group_task_completion(job);
		joined = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
	}
	RENDER_TIMESTAMP("Geometry Persistent Upload");
	const uint64_t persistent_begin = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
	if (raytracing) {
		raytracing->update_persistent_instances(dirty_instances);
		_update_micro_geometry_instances(dirty_instances);
	}
	const uint64_t persistent_end = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
	RENDER_TIMESTAMP("Geometry Pipeline Update");
	uint32_t pipeline_count = 0;
	const bool all_pipelines = global_pipeline_data_required.key != global_pipeline_data_compiled.key;
	if (profile_preparation) {
		auto *entry = all_pipelines ? geometry_surface_compilation_all_list.first() : geometry_surface_compilation_dirty_list.first();
		while (entry) {
			pipeline_count++;
			entry = entry->next();
		}
	}
	const uint64_t pipeline_begin = profile_preparation ? OS::get_singleton()->get_ticks_usec() : 0;
	_update_dirty_geometry_pipelines();
	RENDER_TIMESTAMP("Geometry Dirty Update Complete");
	if (profile_preparation) {
		const uint64_t owner_end = OS::get_singleton()->get_ticks_usec();
		String rows = vformat("RenderPrep stage=GeometryMotionPreparation frame=%d coordinator=%d jobs=%d motion=%d dirty=%d queued_usec=%d joined_usec=%d worker=%d begin_usec=%d end_usec=%d timing=elapsed", profile_frame, coordinator, jobs, motion_count, dirty_instances.size(), queued, joined, worker, job_begin, job_end) + "\n";
		rows += vformat("RenderPrep stage=GeometryDirtyOwner frame=%d coordinator=%d geometry=%d dirty=%d pipelines=%d all_pipelines=%d begin_usec=%d end_usec=%d geometry_usec=%d persistent_usec=%d pipeline_usec=%d timing=elapsed", profile_frame, coordinator, geometry_count, dirty_instances.size(), pipeline_count, int(all_pipelines), owner_begin, owner_end, geometry_end - owner_begin, persistent_end - persistent_begin, owner_end - pipeline_begin);
		print_line(rows);
	}
}

void RenderForwardClustered::_update_dirty_geometry_pipelines() {
	if (global_pipeline_data_required.key != global_pipeline_data_compiled.key) {
		// Go through the entire list of surfaces and compile pipelines for everything again.
		SelfList<GeometryInstanceSurfaceDataCache> *list = geometry_surface_compilation_all_list.first();
		while (list != nullptr) {
			GeometryInstanceSurfaceDataCache *surface_cache = list->self();
			_mesh_generate_all_pipelines_for_surface_cache(surface_cache, global_pipeline_data_required);

			if (surface_cache->compilation_dirty_element.in_list()) {
				// Remove any elements from the dirty list as they don't need to be processed again.
				geometry_surface_compilation_dirty_list.remove(&surface_cache->compilation_dirty_element);
			}

			list = list->next();
		}

		global_pipeline_data_compiled.key = global_pipeline_data_required.key;
	} else {
		// Compile pipelines only for the dirty list.
		if (!geometry_surface_compilation_dirty_list.first()) {
			return;
		}

		while (geometry_surface_compilation_dirty_list.first() != nullptr) {
			GeometryInstanceSurfaceDataCache *surface_cache = geometry_surface_compilation_dirty_list.first()->self();
			_mesh_generate_all_pipelines_for_surface_cache(surface_cache, global_pipeline_data_compiled);
			surface_cache->compilation_dirty_element.remove_from_list();
		}
	}
}

void RenderForwardClustered::_geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker) {
	switch (p_notification) {
		case Dependency::DEPENDENCY_CHANGED_MATERIAL:
		case Dependency::DEPENDENCY_CHANGED_MESH:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES_INSTANCES:
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH:
		case Dependency::DEPENDENCY_CHANGED_SKELETON_DATA: {
			static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
			static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata)->data->dirty_dependencies = true;
		} break;
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH_DATA:
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH_VISIBLE_INSTANCES: {
			GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata);
			if (ginstance->scene_data->base_type == RSE::INSTANCE_MULTIMESH) {
				ginstance->instance_count = RendererRD::MeshStorage::get_singleton()->multimesh_get_instances_to_draw(ginstance->scene_data->base);
				ginstance->_mark_instance_data_dirty();
			}
		} break;
		default: {
			//rest of notifications of no interest
		} break;
	}
}
void RenderForwardClustered::_geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker) {
	static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
	static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata)->data->dirty_dependencies = true;
}

RenderGeometryInstance *RenderForwardClustered::geometry_instance_create(RID p_base, RenderSceneInstanceData *p_scene_data) {
	RSE::InstanceType type = RSG::utilities->get_base_type(p_base);
	ERR_FAIL_COND_V(!((1 << type) & RSE::INSTANCE_GEOMETRY_MASK), nullptr);

	GeometryInstanceForwardClustered *ginstance = geometry_instance_alloc.alloc();
	ginstance->data = memnew(GeometryInstanceForwardClustered::Data);
	ginstance->scene_data = p_scene_data;

	ginstance->scene_data->base = p_base;
	ginstance->scene_data->base_type = type;
	ginstance->data->dirty_dependencies = true;
	ginstance->data->dependency_tracker.userdata = ginstance;
	ginstance->data->dependency_tracker.changed_callback = _geometry_instance_dependency_changed;
	ginstance->data->dependency_tracker.deleted_callback = _geometry_instance_dependency_deleted;

	ginstance->_mark_dirty();

	return ginstance;
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) {
	const bool moved = transform != p_transform || memcmp(origin, scene_data->origin, sizeof(origin)) != 0;
	uint64_t frame = RSG::rasterizer->get_frame_number();
	if (moved && frame != prev_transform_change_frame) {
		prev_transform = transform;
		memcpy(prev_origin, origin, sizeof(origin));
		prev_transform_change_frame = frame;
		transform_status = TransformStatus::MOVED;
	} else if (moved && unlikely(transform_status == TransformStatus::TELEPORTED)) {
		prev_transform = transform;
		memcpy(prev_origin, origin, sizeof(origin));
	}

	RenderGeometryInstanceBase::set_transform(p_transform, p_aabb, p_transformed_aabb);
}

void RenderForwardClustered::GeometryInstanceForwardClustered::reset_motion_vectors() {
	_mark_instance_data_dirty();
	prev_transform = transform;
	memcpy(prev_origin, origin, sizeof(origin));
	transform_status = TransformStatus::TELEPORTED;
}

void RenderForwardClustered::GeometryInstanceForwardClustered::age_out_motion(uint64_t p_frame) {
	if (last_aged_frame == p_frame) {
		return; // Already processed this frame (e.g. appears in both raster and RT lists).
	}
	last_aged_frame = p_frame;
	if (transform_status != TransformStatus::NONE && p_frame > prev_transform_change_frame + 1 && prev_transform_change_frame) {
		prev_transform = transform;
		memcpy(prev_origin, origin, sizeof(origin));
		transform_status = TransformStatus::NONE;
		_mark_instance_data_dirty();
	}
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) {
	lightmap_instance = p_lightmap_instance;
	lightmap_uv_scale = p_lightmap_uv_scale;
	lightmap_slice_index = p_lightmap_slice_index;

	_mark_dirty();
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_lightmap_capture(const Color *p_sh9) {
	if (p_sh9) {
		if (lightmap_sh == nullptr) {
			lightmap_sh = RenderForwardClustered::get_singleton()->geometry_instance_lightmap_sh.alloc();
		}

		memcpy(lightmap_sh->sh, p_sh9, sizeof(Color) * 9);
	} else {
		if (lightmap_sh != nullptr) {
			RenderForwardClustered::get_singleton()->geometry_instance_lightmap_sh.free(lightmap_sh);
			lightmap_sh = nullptr;
		}
	}
	_mark_dirty();
}

RTProceduralState *RenderForwardClustered::GeometryInstanceForwardClustered::_ensure_procedural_state() {
	if (!rt_procedural) {
		rt_procedural = memnew(RTProceduralState);
	}
	return rt_procedural;
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_rt_procedural(bool p_procedural, const AABB &p_aabb) {
	if (p_procedural) {
		RTProceduralState *s = _ensure_procedural_state();
		if (s->culling_aabb != p_aabb) {
			s->dirty = true;
		}
		s->culling_aabb = p_aabb;
	} else if (rt_procedural) {
		_free_procedural_state();
	}
	_mark_instance_data_dirty();
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_rt_procedural_bounds(const Vector<float> &p_aabb_data, bool p_expose_bounds) {
	RTProceduralState *s = _ensure_procedural_state();
	if (s->aabb_data != p_aabb_data || s->expose_bounds != p_expose_bounds) {
		s->dirty = true;
	}
	s->aabb_data = p_aabb_data;
	s->expose_bounds = p_expose_bounds;
	_mark_instance_data_dirty();
}

void RenderForwardClustered::GeometryInstanceForwardClustered::_free_procedural_state() {
	if (!rt_procedural) {
		return;
	}
	if (rt_procedural->blas.is_valid()) {
		RD::get_singleton()->free_rid(rt_procedural->blas);
	}
	if (rt_procedural->gpu_buffer.is_valid()) {
		RD::get_singleton()->free_rid(rt_procedural->gpu_buffer);
	}
	memdelete(rt_procedural);
	rt_procedural = nullptr;
}

void RenderForwardClustered::geometry_instance_free(RenderGeometryInstance *p_geometry_instance) {
	GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_geometry_instance);
	ERR_FAIL_NULL(ginstance);
	if (raytracing) {
		raytracing->release_persistent_instance(ginstance->persistent_instance, ginstance->persistent_surfaces);
	}
	if (ginstance->lightmap_sh != nullptr) {
		geometry_instance_lightmap_sh.free(ginstance->lightmap_sh);
	}
	ginstance->_free_procedural_state();
	GeometryInstanceSurfaceDataCache *surf = ginstance->surface_caches;
	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		if (surf->micro_geometry_element.in_list()) {
			micro_geometry_generation++;
		}
		if (surf->micro_geometry_rt_element.in_list()) {
			micro_geometry_rt_generation++;
		}
		geometry_instance_surface_alloc.free(surf);
		surf = next;
	}
	memdelete(ginstance->data);
	geometry_instance_alloc.free(ginstance);
}

uint32_t RenderForwardClustered::geometry_instance_get_pair_mask() {
	return (1 << RSE::INSTANCE_VOXEL_GI);
}

void RenderForwardClustered::mesh_generate_pipelines(RID p_mesh, bool p_background_compilation) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);
	uint32_t surface_count = 0;
	const RID *materials = mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);
	Vector<ShaderPipelinePair> pipeline_pairs;
	for (uint32_t i = 0; i < surface_count; i++) {
		if (materials[i].is_null()) {
			continue;
		}

		void *mesh_surface = mesh_storage->mesh_get_surface(p_mesh, i);
		void *mesh_surface_shadow = mesh_surface;
		SceneShaderForwardClustered::MaterialData *material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(materials[i], RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (material == nullptr || !material->shader_data->is_valid()) {
			continue;
		}

		SceneShaderForwardClustered::ShaderData *shader = material->shader_data;
		SceneShaderForwardClustered::ShaderData *shader_shadow = shader;
		if (material->shader_data->uses_shared_shadow_material()) {
			SceneShaderForwardClustered::MaterialData *material_shadow = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
			if (material_shadow != nullptr) {
				shader_shadow = material_shadow->shader_data;
				if (shadow_mesh.is_valid()) {
					mesh_surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, i);
				}
			}
		}

		if (!shader->is_valid()) {
			continue;
		}

		SurfacePipelineData surface;
		surface.mesh_surface = mesh_surface;
		surface.mesh_surface_shadow = mesh_surface_shadow;
		surface.shader = shader;
		surface.shader_shadow = shader_shadow;
		surface.instanced = mesh_storage->mesh_needs_instance(p_mesh, true);
		surface.uses_depth = !shader->uses_alpha_pass() || shader->uses_depth_in_alpha_pass();
		_mesh_compile_pipelines_for_surface(surface, global_pipeline_data_required, RSE::PIPELINE_SOURCE_MESH, &pipeline_pairs);
	}

	// Wait for all the pipelines that were compiled. This will force the loader to wait on all ubershader pipelines to be ready.
	if (!p_background_compilation && !pipeline_pairs.is_empty()) {
		for (ShaderPipelinePair pair : pipeline_pairs) {
			pair.first->pipeline_hash_map.wait_for_pipeline(pair.second.hash());
		}
	}
}

uint32_t RenderForwardClustered::get_pipeline_compilations(RSE::PipelineSource p_source) {
	return scene_shader.get_pipeline_compilations(p_source);
}

void RenderForwardClustered::enable_features(BitField<FeatureBits> p_feature_bits) {
	if (p_feature_bits.has_flag(FEATURE_MULTIVIEW_BIT)) {
		scene_shader.enable_multiview_shader_group();
	}

	if (p_feature_bits.has_flag(FEATURE_ADVANCED_BIT)) {
		scene_shader.enable_advanced_shader_group(p_feature_bits.has_flag(FEATURE_MULTIVIEW_BIT));
	}

	if (p_feature_bits.has_flag(FEATURE_VRS_BIT)) {
		gi.enable_vrs_shader_group();
	}
}

String RenderForwardClustered::get_name() const {
	return "forward_clustered";
}

void RenderForwardClustered::GeometryInstanceForwardClustered::pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) {
	if (voxel_gi_instances[0] != (p_voxel_gi_instance_count > 0 ? p_voxel_gi_instances[0] : RID()) || voxel_gi_instances[1] != (p_voxel_gi_instance_count > 1 ? p_voxel_gi_instances[1] : RID())) {
		RenderForwardClustered::get_singleton()->micro_geometry_generation++;
	}
	if (p_voxel_gi_instance_count > 0) {
		voxel_gi_instances[0] = p_voxel_gi_instances[0];
	} else {
		voxel_gi_instances[0] = RID();
	}

	if (p_voxel_gi_instance_count > 1) {
		voxel_gi_instances[1] = p_voxel_gi_instances[1];
	} else {
		voxel_gi_instances[1] = RID();
	}
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) {
	using_projectors = p_projector;
	using_softshadows = p_softshadow;
	_mark_dirty();
}

void RenderForwardClustered::_update_shader_quality_settings() {
	SceneShaderForwardClustered::ShaderSpecialization specialization = {};
	specialization.decal_use_mipmaps = decals_get_filter() == RSE::DECAL_FILTER_NEAREST_MIPMAPS ||
			decals_get_filter() == RSE::DECAL_FILTER_LINEAR_MIPMAPS ||
			decals_get_filter() == RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			decals_get_filter() == RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;
	;
	specialization.projector_use_mipmaps = light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	specialization.soft_shadow_samples = soft_shadow_samples_get();
	specialization.penumbra_shadow_samples = penumbra_shadow_samples_get();
	specialization.directional_soft_shadow_samples = directional_soft_shadow_samples_get();
	specialization.directional_penumbra_shadow_samples = directional_penumbra_shadow_samples_get();
	specialization.use_lightmap_bicubic_filter = lightmap_filter_bicubic_get();
	specialization.fog_use_legacy_blending = fog_use_legacy_blending_get();
	scene_shader.set_default_specialization(specialization);

	base_uniforms_changed(); //also need this
}

RenderForwardClustered::RenderForwardClustered() {
	singleton = this;
#ifdef DEBUG_ENABLED
	const String visibility_mode = OS::get_singleton()->get_environment("GODOT_PRIMARY_VISIBILITY");
	if (visibility_mode == "T") {
		primary_visibility_mode = PRIMARY_VISIBILITY_TRACE;
	} else if (visibility_mode == "H-R") {
		primary_visibility_mode = PRIMARY_VISIBILITY_RASTER_TRACE;
	} else if (visibility_mode == "H-T") {
		primary_visibility_mode = PRIMARY_VISIBILITY_TRACE_RASTER;
	} else if (!visibility_mode.is_empty() && visibility_mode != "R") {
		primary_visibility_mode = PRIMARY_VISIBILITY_INVALID;
	}
#endif

	/* SCENE SHADER */

	{
		String defines;
		defines += "\n#define MAX_ROUGHNESS_LOD " + itos(get_roughness_layers() - 1) + ".0\n";
		if (is_using_radiance_octmap_array()) {
			defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY \n";
		}
		defines += "\n#define SDFGI_OCT_SIZE " + itos(gi.sdfgi_get_lightprobe_octahedron_size()) + "\n";
		defines += "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(MAX_DIRECTIONAL_LIGHTS) + "\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (force_vertex_shading) {
			defines += "\n#define USE_VERTEX_LIGHTING\n";
		}

		bool specular_occlusion = GLOBAL_GET("rendering/reflections/specular_occlusion/enabled");
		if (!specular_occlusion) {
			defines += "\n#define SPECULAR_OCCLUSION_DISABLED\n";
		}

		{
			//lightmaps
			scene_state.max_lightmaps = MAX_LIGHTMAPS;
			defines += "\n#define MAX_LIGHTMAP_TEXTURES " + itos(scene_state.max_lightmaps) + "\n";
			defines += "\n#define MAX_LIGHTMAPS " + itos(scene_state.max_lightmaps) + "\n";

			scene_state.lightmap_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapData) * scene_state.max_lightmaps);
		}
		{
			//captures
			scene_state.max_lightmap_captures = 2048;
			scene_state.lightmap_captures = memnew_arr(LightmapCaptureData, scene_state.max_lightmap_captures);
			scene_state.lightmap_capture_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapCaptureData) * scene_state.max_lightmap_captures);
		}
		{
			defines += "\n#define MATERIAL_UNIFORM_SET " + itos(MATERIAL_UNIFORM_SET) + "\n";
		}
		{
			defines += "\n#define USE_DOUBLE_PRECISION \n";
		}

		scene_shader.init(defines);
	}

	/* shadow sampler */
	{
		RD::SamplerState sampler;
		sampler.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_GREATER;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		best_fit_normal.shader.initialize(modes);
		best_fit_normal.shader_version = best_fit_normal.shader.version_create();
		best_fit_normal.pipeline = RD::get_singleton()->compute_pipeline_create(best_fit_normal.shader.version_get_shader(best_fit_normal.shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R8_UNORM;
		tformat.width = 1024;
		tformat.height = 1024;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		best_fit_normal.texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = best_fit_normal.shader.version_get_shader(best_fit_normal.shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(best_fit_normal.texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, best_fit_normal.pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();
	}

	/* DFG LUT */
	{
		Vector<String> modes;
		modes.push_back("\n");
		dfg_lut.shader.initialize(modes);
		dfg_lut.shader_version = dfg_lut.shader.version_create();
		dfg_lut.pipeline = RD::get_singleton()->compute_pipeline_create(dfg_lut.shader.version_get_shader(dfg_lut.shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tformat.width = 128;
		tformat.height = 128;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		dfg_lut.texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = dfg_lut.shader.version_get_shader(dfg_lut.shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dfg_lut.texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, dfg_lut.pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();
	}

	_update_shader_quality_settings();
	_update_global_pipeline_data_requirements_from_project();

	taa = memnew(RendererRD::TAA);
	fsr2_effect = memnew(RendererRD::FSR2Effect);
	dlss_effect = memnew(RendererRD::DLSSEffect);
	ss_effects = memnew(RendererRD::SSEffects);
	motion_vectors_store = memnew(RendererRD::MotionVectorsStore);
	raytracing = memnew(RenderRaytracing);
	raytracing->initialize(this);
	rtxdi = memnew(RenderRTXDI);
	rtxdi->initialize(raytracing, is_using_radiance_octmap_array(), get_roughness_layers());
	nrd_effect = memnew(RendererRD::NRDEffect(is_using_radiance_octmap_array(), get_roughness_layers()));
#ifdef METAL_MFXTEMPORAL_ENABLED
	mfx_temporal_effect = memnew(RendererRD::MFXTemporalEffect);
#endif
}

RenderForwardClustered::~RenderForwardClustered() {
	_discard_shadow_preparations();
	RD::get_singleton()->flush_and_stall();
	for (auto &list : render_list) {
		list.clear();
	}
	for (auto *pass : micro_geometry_passes) {
		if (pass) {
			memdelete(pass);
		}
	}
	if (micro_geometry) {
		memdelete(micro_geometry);
	}
	if (nrd_effect) {
		memdelete(nrd_effect);
		nrd_effect = nullptr;
	}
	if (rtxdi != nullptr) {
		memdelete(rtxdi);
		rtxdi = nullptr;
	}
	if (raytracing != nullptr) {
		memdelete(raytracing);
		raytracing = nullptr;
	}

	if (ss_effects != nullptr) {
		memdelete(ss_effects);
		ss_effects = nullptr;
	}

	if (taa != nullptr) {
		memdelete(taa);
		taa = nullptr;
	}

	if (fsr2_effect) {
		memdelete(fsr2_effect);
		fsr2_effect = nullptr;
	}

	if (motion_vectors_store) {
		memdelete(motion_vectors_store);
		motion_vectors_store = nullptr;
	}

#ifdef METAL_MFXTEMPORAL_ENABLED
	if (mfx_temporal_effect) {
		memdelete(mfx_temporal_effect);
		mfx_temporal_effect = nullptr;
	}

#endif

	if (dlss_effect) {
		memdelete(dlss_effect);
		dlss_effect = nullptr;
	}

	RD::get_singleton()->free_rid(shadow_sampler);
	RSG::light_storage->directional_shadow_atlas_set_size(0);

	RD::get_singleton()->free_rid(best_fit_normal.pipeline);
	RD::get_singleton()->free_rid(best_fit_normal.texture);
	best_fit_normal.shader.version_free(best_fit_normal.shader_version);

	RD::get_singleton()->free_rid(dfg_lut.pipeline);
	RD::get_singleton()->free_rid(dfg_lut.texture);
	dfg_lut.shader.version_free(dfg_lut.shader_version);

	if (ltc.lut1_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc.lut1_texture);
	}
	if (ltc.lut2_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc.lut2_texture);
	}

	{
		for (const RID &rid : scene_state.uniform_buffers) {
			RD::get_singleton()->free_rid(rid);
		}
		for (const RID &rid : scene_state.implementation_uniform_buffers) {
			RD::get_singleton()->free_rid(rid);
		}
		RD::get_singleton()->free_rid(scene_state.lightmap_buffer);
		RD::get_singleton()->free_rid(scene_state.lightmap_capture_buffer);
		for (uint32_t i = 0; i < RENDER_LIST_MAX; i++) {
			scene_state.instance_buffer[i].uninit();
		}
		memdelete_arr(scene_state.lightmap_captures);
	}

	while (sdfgi_framebuffer_size_cache.begin()) {
		RD::get_singleton()->free_rid(sdfgi_framebuffer_size_cache.begin()->value);
		sdfgi_framebuffer_size_cache.remove(sdfgi_framebuffer_size_cache.begin());
	}
}

void RenderForwardClustered::GeometryInstanceForwardClustered::_mark_instance_data_dirty() {
	RenderForwardClustered *renderer = RenderForwardClustered::get_singleton();
	if (!instance_data_dirty_element.in_list()) {
		renderer->instance_data_dirty_list.add(&instance_data_dirty_element);
	}
	if (data && (transform_status != NONE || scene_data->base_type == RSE::INSTANCE_MULTIMESH) && !motion_update_element.in_list()) {
		renderer->instance_motion_update_list.add(&motion_update_element);
		renderer->instance_motion_update_frame = UINT64_MAX;
	}
}
