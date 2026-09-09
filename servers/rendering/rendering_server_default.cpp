/**************************************************************************/
/*  rendering_server_default.cpp                                          */
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

#include "rendering_server_default.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "drivers/streamline/streamline.h"
#include "servers/display/display_server.h"
#include "servers/rendering/renderer_canvas_cull.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

#ifndef XR_DISABLED
#include "servers/xr/xr_server.h"
#endif

// careful, these may run in different threads than the rendering server

SafeNumeric<uint64_t> RenderingServerDefault::changes;
RenderingServerGlobals::FrameContext RenderingServerGlobals::frame;

/* FREE */

void RenderingServerDefault::_free(RID p_rid) {
	if (unlikely(p_rid.is_null())) {
		return;
	}
	if (RSG::utilities->free(p_rid)) {
		return;
	}
	if (RSG::canvas->free(p_rid)) {
		return;
	}
	if (RSG::viewport->free(p_rid)) {
		return;
	}
	if (RSG::scene->free(p_rid)) {
		return;
	}
}

/* EVENT QUEUING */

void RenderingServerDefault::texture_2d_update(RID p_texture, const Ref<Image> &p_image, int p_layer) {
	redraw_request();
	if (Thread::get_caller_id() == server_thread) {
		command_queue.flush_if_pending();
		RSG::texture_storage->texture_2d_update(p_texture, p_image, p_layer);
	} else {
		ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
		Ref<Image> image = Image::create_from_data(p_image->get_width(), p_image->get_height(), p_image->has_mipmaps(), p_image->get_format(), p_image->get_data());
		command_queue.push(RSG::texture_storage, &RendererTextureStorage::texture_2d_update, p_texture, image, p_layer);
	}
}

void RenderingServerDefault::texture_3d_update(RID p_texture, const Vector<Ref<Image>> &p_data) {
	redraw_request();
	if (Thread::get_caller_id() == server_thread) {
		command_queue.flush_if_pending();
		RSG::texture_storage->texture_3d_update(p_texture, p_data);
	} else {
		Vector<Ref<Image>> images;
		images.resize(p_data.size());
		for (int i = 0; i < p_data.size(); i++) {
			const Ref<Image> &image = p_data[i];
			ERR_FAIL_COND(image.is_null() || image->is_empty());
			images.write[i] = Image::create_from_data(image->get_width(), image->get_height(), image->has_mipmaps(), image->get_format(), image->get_data());
		}
		command_queue.push(RSG::texture_storage, &RendererTextureStorage::texture_3d_update, p_texture, images);
	}
}

void RenderingServerDefault::request_frame_drawn_callback(const Callable &p_callable) {
	MutexLock lock(callbacks_mutex);
	frame_drawn_callbacks.push_back(p_callable);
}

void RenderingServerDefault::_draw(bool p_swap_buffers, double frame_step, RenderingServerGlobals::FrameContext p_frame, Vector<Callable> p_callbacks, uint64_t p_queued_usec) {
	RSG::frame = p_frame;
	const bool main_overlap = main_iteration_active.is_set();
	const uint64_t profile_cpu_begin = print_gpu_profile ? OS::get_singleton()->get_ticks_usec() : 0;
	if (RenderingDevice::get_singleton()) {
		RenderingDevice::get_singleton()->begin_cpu_frame_profile(print_gpu_profile);
	}
	GodotProfileZoneGroupedFirst(_profile_zone, "rasterizer->begin_frame");
	RSG::rasterizer->begin_frame(frame_step);

	TIMESTAMP_BEGIN()

	uint64_t time_usec = OS::get_singleton()->get_ticks_usec();

	RENDER_TIMESTAMP("Prepare Render Frame");

#ifndef XR_DISABLED
	GodotProfileZoneGrouped(_profile_zone, "xr_server->pre_render");
	XRServer *xr_server = XRServer::get_singleton();
	if (xr_server != nullptr) {
		// Let XR server know we're about to render a frame.
		xr_server->pre_render();
	}
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "scene->update");
	RSG::scene->update(); //update scenes stuff before updating instances
	GodotProfileZoneGrouped(_profile_zone, "canvas->update");
	RSG::canvas->update();

	frame_setup_time = double(OS::get_singleton()->get_ticks_usec() - time_usec) / 1000.0;

	GodotProfileZoneGrouped(_profile_zone, "particles_storage->update_particles");
	RSG::particles_storage->update_particles(); //need to be done after instances are updated (colliders and particle transforms), and colliders are rendered

	GodotProfileZoneGrouped(_profile_zone, "scene->render_probes");
	RSG::scene->render_probes();

	GodotProfileZoneGrouped(_profile_zone, "viewport->draw_viewports");
	RSG::viewport->draw_viewports(p_swap_buffers);

	GodotProfileZoneGrouped(_profile_zone, "canvas_render->update");
	RSG::canvas_render->update();

	GodotProfileZoneGrouped(_profile_zone, "rasterizer->end_frame");
	RSG::rasterizer->end_frame(p_swap_buffers);
	const double profile_cpu_render_wall = print_gpu_profile ? double(OS::get_singleton()->get_ticks_usec() - profile_cpu_begin) / 1000.0 : 0.0;

#ifndef XR_DISABLED
	if (xr_server != nullptr) {
		GodotProfileZone("xr_server->end_frame");
		// let our XR server know we're done so we can get our frame timing
		xr_server->end_frame();
	}
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "update_visibility_notifiers");
	RSG::canvas->update_visibility_notifiers();
	RSG::scene->update_visibility_notifiers();

	if (RSG::utilities->get_captured_timestamps_count()) {
		GodotProfileZoneGrouped(_profile_zone, "frame_profile");
		Vector<RenderingServerTypes::FrameProfileArea> new_profile;
		if (RSG::utilities->capturing_timestamps) {
			new_profile.resize(RSG::utilities->get_captured_timestamps_count());
		}

		uint64_t base_cpu = RSG::utilities->get_captured_timestamp_cpu_time(0);
		uint64_t base_gpu = RSG::utilities->get_captured_timestamp_gpu_time(0);
		for (uint32_t i = 0; i < RSG::utilities->get_captured_timestamps_count(); i++) {
			uint64_t time_cpu = RSG::utilities->get_captured_timestamp_cpu_time(i);
			uint64_t time_gpu = RSG::utilities->get_captured_timestamp_gpu_time(i);

			String name = RSG::utilities->get_captured_timestamp_name(i);

			if (name.begins_with("vp_")) {
				RSG::viewport->handle_timestamp(name, time_cpu, time_gpu);
			}

			if (RSG::utilities->capturing_timestamps) {
				new_profile.write[i].gpu_msec = double((time_gpu - base_gpu) / 1000) / 1000.0;
				new_profile.write[i].cpu_msec = double(time_cpu - base_cpu) / 1000.0;
				new_profile.write[i].name = RSG::utilities->get_captured_timestamp_name(i);
			}
		}

		frame_profile = new_profile;
	}

	frame_profile_frame = RSG::utilities->get_captured_timestamps_frame();

	if (print_gpu_profile) {
		GodotProfileZoneGrouped(_profile_zone, "gpu_profile");
		static const char *const cpu_device_phases[RenderingDevice::CPU_PROFILE_MAX] = {
			"CPU Device End Frame",
			"CPU Device Execute Frame",
			"CPU Device Frame Recycle Inclusive",
			"CPU Device Fence Wait",
			"CPU Device Download Copy",
			"CPU Device Download Callbacks",
			"CPU AS Dependency Union",
			"CPU AS Dependency Usage",
			"CPU Cluster BLAS Build Inclusive",
		};
		static const char *const cpu_device_counters[RenderingDevice::CPU_PROFILE_COUNTER_MAX] = {
			"AS Dependency Calls",
			"Tracker Candidates",
			"AS Dependency Union Builds",
			"AS Dependency Usages",
		};
		if (RenderingDevice::get_singleton()) {
			for (uint32_t counter = 0; counter < RenderingDevice::CPU_PROFILE_COUNTER_MAX; counter++) {
				print_cpu_profile_work_counts[cpu_device_counters[counter]] += RenderingDevice::get_singleton()->get_cpu_frame_profile_count(RenderingDevice::CPUProfileCounter(counter));
			}
			for (uint32_t phase = 0; phase < RenderingDevice::CPU_PROFILE_MAX; phase++) {
				print_cpu_profile_task_time[cpu_device_phases[phase]] += double(RenderingDevice::get_singleton()->get_cpu_frame_profile_usec(RenderingDevice::CPUProfilePhase(phase))) / 1000.0;
			}
		}
		if (print_frame_profile_ticks_from == 0) {
			print_frame_profile_ticks_from = OS::get_singleton()->get_ticks_usec();
		}
		double total_time = 0.0;

		for (int i = 0; i < frame_profile.size() - 1; i++) {
			String name = frame_profile[i].name;
			if (name[0] == '<' || name[0] == '>') {
				continue;
			}

			double time = frame_profile[i + 1].gpu_msec - frame_profile[i].gpu_msec;
			print_cpu_profile_task_time[name] += frame_profile[i + 1].cpu_msec - frame_profile[i].cpu_msec;

			if (print_gpu_profile_task_time.has(name)) {
				print_gpu_profile_task_time[name] += time;
			} else {
				print_gpu_profile_task_time[name] = time;
			}
		}

		if (frame_profile.size()) {
			total_time = frame_profile[frame_profile.size() - 1].gpu_msec;
		}

		uint64_t ticks_elapsed = OS::get_singleton()->get_ticks_usec() - print_frame_profile_ticks_from;
		print_frame_profile_frame_count++;
		print_cpu_profile_task_time["CPU Frame Queue Delay"] += double(profile_cpu_begin - p_queued_usec) / 1000.0;
		print_cpu_profile_work_counts["Main Render Overlap"] += main_overlap;
		if (ticks_elapsed > 1000000) {
			print_line("GPU PROFILE (total " + rtos(total_time) + "ms): ");
			print_line(vformat("GPU TIMESTAMPS (count %d)", frame_profile.size()));
			const double cpu_span = frame_profile.is_empty() ? 0.0 : frame_profile[frame_profile.size() - 1].cpu_msec;
			print_line("CPU PROFILE (timestamp span " + rtos(cpu_span) + "ms, current render wall " + rtos(profile_cpu_render_wall) + "ms):");

			float print_threshold = 0.01;
			for (const KeyValue<String, float> &E : print_gpu_profile_task_time) {
				double time = E.value / double(print_frame_profile_frame_count);
				double cpu_time = print_cpu_profile_task_time[E.key] / double(print_frame_profile_frame_count);
				if (time > print_threshold || cpu_time > print_threshold) {
					print_line("\t-" + E.key + ": " + rtos(time) + "ms (CPU " + rtos(cpu_time) + "ms)");
				}
			}
			for (const char *phase : cpu_device_phases) {
				print_line("\t-" + String(phase) + ": " + rtos(print_cpu_profile_task_time[phase] / double(print_frame_profile_frame_count)) + "ms");
			}
			for (const char *counter : cpu_device_counters) {
				print_line("\t-CPU Work " + String(counter) + ": " + rtos(double(print_cpu_profile_work_counts[counter]) / double(print_frame_profile_frame_count)) + " per frame");
			}
			print_line(vformat("CPU FRAME OWNERSHIP (render thread %d, main thread %d): queue delay %.3fms, main overlap %.3f draws per frame", Thread::get_caller_id(), Thread::MAIN_ID, print_cpu_profile_task_time["CPU Frame Queue Delay"] / double(print_frame_profile_frame_count), double(print_cpu_profile_work_counts["Main Render Overlap"]) / double(print_frame_profile_frame_count)));
			print_cpu_profile_work_counts.clear();
			print_gpu_profile_task_time.clear();
			print_cpu_profile_task_time.clear();
			print_frame_profile_ticks_from = OS::get_singleton()->get_ticks_usec();
			print_frame_profile_frame_count = 0;
		}
	}

	GodotProfileZoneGrouped(_profile_zone, "memory_info");
	RSG::utilities->update_memory_info();
	RSG::viewport->publish_frame_stats();
	{
		MutexLock lock(frame_stats_mutex);
		for (int i = 0; i < RSE::RENDERING_INFO_MAX; i++) {
			completed_rendering_info[i] = _get_rendering_info(RSE::RenderingInfo(i));
		}
		pending_frame_profile_frame = frame_profile_frame;
		pending_frame_profile = frame_profile;
		completed_frame_setup_time = frame_setup_time;
	}
	{
		MutexLock lock(callbacks_mutex);
		completed_draw_callbacks.push_back(p_callbacks);
	}
}

void RenderingServerDefault::_run_post_draw_steps() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (processing_callbacks) {
		return;
	}
	processing_callbacks = true;
	{
		MutexLock lock(frame_stats_mutex);
		completed_frame_profile_frame = pending_frame_profile_frame;
		completed_frame_profile = pending_frame_profile;
	}
	Vector<Vector<Callable>> callbacks;
	{
		MutexLock lock(callbacks_mutex);
		SWAP(callbacks, completed_draw_callbacks);
	}
	for (const Vector<Callable> &batch : callbacks) {
		for (const Callable &c : batch) {
			Variant result;
			Callable::CallError ce;
			c.callp(nullptr, 0, result, ce);
			if (ce.error != Callable::CallError::CALL_OK) {
				ERR_PRINT("Error calling frame drawn function: " + Variant::get_callable_error_text(c, nullptr, 0, ce));
			}
		}
		emit_signal(SNAME("frame_post_draw"));
	}
	processing_callbacks = false;
}

double RenderingServerDefault::get_frame_setup_time_cpu() const {
	MutexLock lock(frame_stats_mutex);
	return completed_frame_setup_time;
}

bool RenderingServerDefault::has_changed() const {
	return changes.get() > 0;
}

void RenderingServerDefault::_init() {
	RSG::threaded = create_thread;
	initialization_result = ERR_UNAVAILABLE;

	const String &rendering_driver = OS::get_singleton()->get_current_rendering_driver_name();
	// test_setup selects RasterizerDummy directly without setting an OS driver name.
	const bool dummy_driver = rendering_driver == "dummy" || (rendering_driver.is_empty() && RenderingDevice::get_singleton() == nullptr);
	if (!dummy_driver && rendering_driver != "vulkan") {
		ERR_PRINT(vformat("The RTXDI renderer requires the Vulkan rendering driver; '%s' cannot initialize rendering.", rendering_driver));
		return;
	}

	RSG::camera_attributes = memnew(RendererCameraAttributes);
	RSG::rasterizer = RendererCompositor::create();
	if (RSG::rasterizer == nullptr) {
		return;
	}
	RSG::canvas = memnew(RendererCanvasCull);
	RSG::viewport = memnew(RendererViewport);
	RendererSceneCull *sr = memnew(RendererSceneCull);
	RSG::scene = sr;
	RSG::utilities = RSG::rasterizer->get_utilities();
	RSG::rasterizer->initialize();
	rasterizer_initialized = true;
	RSG::light_storage = RSG::rasterizer->get_light_storage();
	RSG::material_storage = RSG::rasterizer->get_material_storage();
	RSG::mesh_storage = RSG::rasterizer->get_mesh_storage();
	RSG::particles_storage = RSG::rasterizer->get_particles_storage();
	RSG::texture_storage = RSG::rasterizer->get_texture_storage();
	RSG::gi = RSG::rasterizer->get_gi();
	RSG::fog = RSG::rasterizer->get_fog();
	RSG::canvas_render = RSG::rasterizer->get_canvas();
	sr->set_scene_render(RSG::rasterizer->get_scene());
	initialized = true;
	initialization_result = OK;
}

void RenderingServerDefault::_finish() {
	if (test_cube.is_valid() && RSG::utilities != nullptr) {
		free_rid(test_cube);
		test_cube = RID();
	}

	if (RSG::canvas != nullptr && rasterizer_initialized) {
		RSG::canvas->finalize();
	}
	if (RSG::canvas != nullptr) {
		memdelete(RSG::canvas);
	}
	if (RSG::rasterizer != nullptr && rasterizer_initialized) {
		RSG::rasterizer->finalize();
	}
	if (RSG::viewport != nullptr) {
		memdelete(RSG::viewport);
	}
	if (RSG::rasterizer != nullptr) {
		memdelete(RSG::rasterizer);
	}
	if (RSG::scene != nullptr) {
		memdelete(RSG::scene);
	}
	if (RSG::camera_attributes != nullptr) {
		memdelete(RSG::camera_attributes);
	}

	RSG::canvas = nullptr;
	RSG::viewport = nullptr;
	RSG::rasterizer = nullptr;
	RSG::scene = nullptr;
	RSG::camera_attributes = nullptr;
	RSG::utilities = nullptr;
	RSG::light_storage = nullptr;
	RSG::material_storage = nullptr;
	RSG::mesh_storage = nullptr;
	RSG::particles_storage = nullptr;
	RSG::texture_storage = nullptr;
	RSG::gi = nullptr;
	RSG::fog = nullptr;
	RSG::canvas_render = nullptr;
	RSG::threaded = false;
	initialized = false;
	rasterizer_initialized = false;
}

Error RenderingServerDefault::init() {
	if (create_thread) {
		print_verbose("RenderingServerWrapMT: Starting render thread");
		exit = false;
		DisplayServer::get_singleton()->release_rendering_thread();
		WorkerThreadPool::TaskID tid = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &RenderingServerDefault::_thread_loop), true, "Rendering Server pump task", true);
		command_queue.set_pump_task_id(tid);
		command_queue.push(this, &RenderingServerDefault::_assign_mt_ids, tid);
		command_queue.push_and_sync(this, &RenderingServerDefault::_init);
		DEV_ASSERT(server_task_id == tid);
		if (initialization_result != OK) {
			finish();
		}
	} else {
		server_thread = Thread::MAIN_ID;
		_init();
		if (initialization_result != OK) {
			_finish();
		}
	}
	return initialization_result;
}

void RenderingServerDefault::finish() {
	if (main_frame_active) {
		end_frame();
	}
	if (initialized) {
		finish_frames();
	}
	if (create_thread) {
		if (server_task_id != WorkerThreadPool::INVALID_TASK_ID) {
			command_queue.push(this, &RenderingServerDefault::_finish);
			command_queue.push(this, &RenderingServerDefault::_thread_exit);
			WorkerThreadPool::get_singleton()->wait_for_task_completion(server_task_id);
			server_task_id = WorkerThreadPool::INVALID_TASK_ID;
			command_queue.set_pump_task_id(WorkerThreadPool::INVALID_TASK_ID);
		}
		server_thread = Thread::MAIN_ID;
		if (RenderingDevice::get_singleton()) {
			RenderingDevice::get_singleton()->make_current();
		}
	} else {
		if (initialized || RSG::rasterizer != nullptr) {
			_finish();
		}
	}
}

/* STATUS INFORMATION */

uint64_t RenderingServerDefault::get_rendering_info(RSE::RenderingInfo p_info) {
	ERR_FAIL_INDEX_V(p_info, RSE::RENDERING_INFO_MAX, 0);
	if (Thread::get_caller_id() == server_thread) {
		return _get_rendering_info(p_info);
	}
	MutexLock lock(frame_stats_mutex);
	return completed_rendering_info[p_info];
}

uint64_t RenderingServerDefault::_get_rendering_info(RSE::RenderingInfo p_info) {
	if (p_info == RSE::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME) {
		return RSG::viewport->get_total_objects_drawn();
	} else if (p_info == RSE::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME) {
		return RSG::viewport->get_total_primitives_drawn();
	} else if (p_info == RSE::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME) {
		return RSG::viewport->get_total_draw_calls_used();
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_CANVAS);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_MESH) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_MESH) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_MESH);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE) {
		return RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SURFACE);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_DRAW) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_DRAW);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SPECIALIZATION) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SPECIALIZATION);
	}
	return RSG::utilities->get_rendering_info(p_info);
}

int RenderingServerDefault::viewport_get_render_info(RID p_viewport, RSE::ViewportRenderInfoType p_type, RSE::ViewportRenderInfo p_info) {
	return RSG::viewport->viewport_get_render_info(p_viewport, p_type, p_info);
}

double RenderingServerDefault::viewport_get_measured_render_time_cpu(RID p_viewport) const {
	return RSG::viewport->viewport_get_measured_render_time_cpu(p_viewport);
}

double RenderingServerDefault::viewport_get_measured_render_time_gpu(RID p_viewport) const {
	return RSG::viewport->viewport_get_measured_render_time_gpu(p_viewport);
}

RenderingDeviceEnums::DeviceType RenderingServerDefault::get_video_adapter_type() const {
	return RSG::utilities->get_video_adapter_type();
}

void RenderingServerDefault::set_frame_profiling_enabled(bool p_enable) {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::set_frame_profiling_enabled, p_enable);
		return;
	}
	RSG::utilities->capturing_timestamps = p_enable;
}

uint64_t RenderingServerDefault::get_frame_profile_frame() {
	MutexLock lock(frame_stats_mutex);
	return completed_frame_profile_frame;
}

Vector<RenderingServerTypes::FrameProfileArea> RenderingServerDefault::get_frame_profile() {
	MutexLock lock(frame_stats_mutex);
	return completed_frame_profile;
}

/* TESTING */

Color RenderingServerDefault::get_default_clear_color() {
	if (Thread::get_caller_id() != server_thread) {
		Color color;
		command_queue.push_and_ret(this, &RenderingServerDefault::get_default_clear_color, &color);
		return color;
	}
	return RSG::texture_storage->get_default_clear_color();
}

void RenderingServerDefault::set_default_clear_color(const Color &p_color) {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::set_default_clear_color, p_color);
		return;
	}
	RSG::texture_storage->set_default_clear_color(p_color);
}

#ifndef DISABLE_DEPRECATED
bool RenderingServerDefault::has_feature(RSE::Features p_feature) const {
	return false;
}
#endif

void RenderingServerDefault::sdfgi_set_debug_probe_select(const Vector3 &p_position, const Vector3 &p_dir) {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::sdfgi_set_debug_probe_select, p_position, p_dir);
		return;
	}
	RSG::scene->sdfgi_set_debug_probe_select(p_position, p_dir);
}

void RenderingServerDefault::set_print_gpu_profile(bool p_enable) {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::set_print_gpu_profile, p_enable);
		return;
	}
	RSG::utilities->capturing_timestamps = p_enable;
	print_gpu_profile = p_enable;
}

RID RenderingServerDefault::get_test_cube() {
	if (!test_cube.is_valid()) {
		test_cube = _make_test_cube();
	}
	return test_cube;
}

bool RenderingServerDefault::has_os_feature(const String &p_feature) const {
	if (RSG::utilities) {
		return RSG::utilities->has_os_feature(p_feature);
	} else {
		return false;
	}
}

void RenderingServerDefault::set_debug_generate_wireframes(bool p_generate) {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::set_debug_generate_wireframes, p_generate);
		return;
	}
	RSG::utilities->set_debug_generate_wireframes(p_generate);
}

bool RenderingServerDefault::is_low_end() const {
	return RendererCompositor::is_low_end();
}

Size2i RenderingServerDefault::get_maximum_viewport_size() const {
	if (RSG::utilities) {
		return RSG::utilities->get_maximum_viewport_size();
	} else {
		return Size2i();
	}
}

void RenderingServerDefault::_assign_mt_ids(WorkerThreadPool::TaskID p_pump_task_id) {
	server_thread = Thread::get_caller_id();
	server_task_id = p_pump_task_id;

	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd) {
		// This is needed because the main RD is created on the main thread.
		rd->make_current();
	}
}

void RenderingServerDefault::_thread_exit() {
	exit = true;
}

void RenderingServerDefault::_thread_loop() {
	DisplayServer::get_singleton()->gl_window_make_current(DisplayServerEnums::MAIN_WINDOW_ID); // Move GL to this thread.

	while (!exit) {
		WorkerThreadPool::get_singleton()->yield();
		command_queue.flush_all();
	}

	DisplayServer::get_singleton()->release_rendering_thread();
}

/* INTERPOLATION */

void RenderingServerDefault::set_physics_interpolation_enabled(bool p_enabled) {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::set_physics_interpolation_enabled, p_enabled);
		return;
	}
	RSG::canvas->set_physics_interpolation_enabled(p_enabled);
	RSG::scene->set_physics_interpolation_enabled(p_enabled);
}

/* EVENT QUEUING */

void RenderingServerDefault::sync() {
	if (create_thread && Thread::get_caller_id() != server_thread) {
		command_queue.sync();
	} else {
		command_queue.flush_all(); // Flush all pending from other threads.
	}
}

uint64_t RenderingServerDefault::begin_frame() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), 0);
	ERR_FAIL_COND_V(main_frame_active, 0);
	_run_post_draw_steps();
	const uint64_t wait_begin = OS::get_singleton()->get_ticks_usec();
	{
		GodotProfileZone("Rendering frame admission wait");
		frame_slots.wait();
	}
	const uint64_t wait_usec = OS::get_singleton()->get_ticks_usec() - wait_begin;
	main_frame_active = true;
	main_iteration_active.set();
	if (Streamline::get_singleton()) {
		Streamline::get_singleton()->begin_frame();
	}
	return wait_usec;
}

void RenderingServerDefault::_end_frame() {
	frame_slots.post();
}

void RenderingServerDefault::finish_frames() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	uint64_t submitted;
	do {
		submitted = draw_requests;
		sync();
		_run_post_draw_steps();
	} while (submitted != draw_requests);
}

void RenderingServerDefault::end_frame() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(!main_frame_active);
	main_frame_active = false;
	main_iteration_active.clear();
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_end_frame);
	} else {
		command_queue.flush_all();
		_end_frame();
		_run_post_draw_steps();
	}
}

RenderingServerGlobals::FrameContext RenderingServerDefault::_capture_frame() const {
	RenderingServerGlobals::FrameContext frame;
	frame.interpolation_fraction = Engine::get_singleton()->get_physics_interpolation_fraction();
	frame.frames_drawn = Engine::get_singleton()->get_frames_drawn();
	frame.process_frames = Engine::get_singleton()->get_process_frames();
	if (Streamline::get_singleton()) {
		frame.streamline = Streamline::get_singleton()->get_frame_data();
	}
	return frame;
}

void RenderingServerDefault::_set_physics_frame(bool p_active) {
	RSG::in_physics_frame = p_active;
}

void RenderingServerDefault::set_physics_frame(bool p_active) {
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_set_physics_frame, p_active);
	} else {
		_set_physics_frame(p_active);
	}
}

void RenderingServerDefault::draw(bool p_present, double frame_step) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Manually triggering the draw function from the RenderingServer can only be done on the main thread. Call this function from the main thread or use call_deferred().");
	const bool manual_frame = !main_frame_active;
	if (manual_frame) {
		begin_frame();
	}
	RS::get_singleton()->emit_signal(SNAME("frame_pre_draw"));
	changes.set(0);
	Vector<Callable> callbacks;
	{
		MutexLock lock(callbacks_mutex);
		SWAP(callbacks, frame_drawn_callbacks);
	}
	const RenderingServerGlobals::FrameContext frame = _capture_frame();
	const uint64_t queued_usec = OS::get_singleton()->get_ticks_usec();
	draw_requests++;
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_draw, p_present, frame_step, frame, callbacks, queued_usec);
	} else {
		command_queue.flush_all();
		_draw(p_present, frame_step, frame, callbacks, queued_usec);
		_run_post_draw_steps();
	}
	if (manual_frame) {
		end_frame();
	}
}

void RenderingServerDefault::tick() {
	if (Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::tick);
		return;
	}
	RSG::canvas->tick();
	RSG::scene->tick();
}

void RenderingServerDefault::pre_draw(bool p_will_draw) {
	const RenderingServerGlobals::FrameContext frame = _capture_frame();
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_pre_draw, p_will_draw, frame);
	} else {
		command_queue.flush_all();
		_pre_draw(p_will_draw, frame);
	}
}

void RenderingServerDefault::_pre_draw(bool p_will_draw, RenderingServerGlobals::FrameContext p_frame) {
	RSG::frame = p_frame;
	RSG::scene->pre_draw(p_will_draw);
}

void RenderingServerDefault::_call_on_render_thread(const Callable &p_callable) {
	p_callable.call();
}

RenderingServerDefault::RenderingServerDefault(bool p_create_thread) {
	RenderingServer::init();

	create_thread = p_create_thread;
	frame_slots.post(2);
}

RenderingServerDefault::~RenderingServerDefault() {
}
