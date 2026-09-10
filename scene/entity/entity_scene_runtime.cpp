#include "entity_scene_runtime.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/io/image_loader.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/string/translation_server.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/particle_process_material.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

Error EntitySceneRuntime::setup() {
	ERR_FAIL_COND_V(world, ERR_ALREADY_IN_USE);
	world = memnew(EntityWorld(catalog));
	Error error = world->initialize_services();
	if (error != OK) {
		_release();
		return error;
	}
	RenderingServer *server = RenderingServer::get_singleton();
	viewport = server->viewport_create();
	if (viewport.is_null()) {
		_release();
		return ERR_CANT_CREATE;
	}
	server->viewport_set_scenario(viewport, world->get_scenario());
	server->viewport_attach_camera(viewport, world->get_camera());
	server->viewport_set_disable_2d(viewport, true);
	server->viewport_set_update_mode(viewport, RSE::VIEWPORT_UPDATE_ALWAYS);
	server->viewport_set_scaling_3d_mode(viewport, RSE::ViewportScaling3DMode(int(GLOBAL_GET("rendering/scaling_3d/mode"))));
	server->viewport_set_scaling_3d_scale(viewport, GLOBAL_GET("rendering/scaling_3d/scale"));
	server->viewport_set_fsr_sharpness(viewport, GLOBAL_GET("rendering/scaling_3d/fsr_sharpness"));
	server->viewport_set_texture_mipmap_bias(viewport, GLOBAL_GET("rendering/textures/default_filters/texture_mipmap_bias"));
	server->viewport_set_anisotropic_filtering_level(viewport, RSE::ViewportAnisotropicFiltering(int(GLOBAL_GET("rendering/textures/default_filters/anisotropic_filtering_level"))));
	server->viewport_set_msaa_3d(viewport, RSE::ViewportMSAA(int(GLOBAL_GET("rendering/anti_aliasing/quality/msaa_3d"))));
	server->viewport_set_screen_space_aa(viewport, RSE::ViewportScreenSpaceAA(int(GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "rendering/anti_aliasing/quality/screen_space_aa", PROPERTY_HINT_ENUM, "Disabled (Fastest),FXAA (Fast),SMAA (Average)"), 0))));
	server->viewport_set_use_taa(viewport, GLOBAL_DEF_BASIC("rendering/anti_aliasing/quality/use_taa", false));
	server->viewport_set_use_debanding(viewport, GLOBAL_GET("rendering/anti_aliasing/quality/use_debanding"));
	server->viewport_set_use_occlusion_culling(viewport, GLOBAL_DEF("rendering/occlusion_culling/use_occlusion_culling", false));
	server->viewport_set_mesh_lod_threshold(viewport, GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/mesh_lod/lod_change/threshold_pixels", PROPERTY_HINT_RANGE, "0,1024,0.1"), 1.0));
	server->viewport_set_transparent_background(viewport, GLOBAL_DEF("rendering/viewport/transparent_background", false));
	server->viewport_set_positional_shadow_atlas_size(viewport, GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/lights_and_shadows/positional_shadow/atlas_size", PROPERTY_HINT_RANGE, "256,16384"), 4096), GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/atlas_16_bits"));
	GLOBAL_DEF("rendering/lights_and_shadows/positional_shadow/atlas_size.mobile", 2048);
	for (int i = 0; i < 4; i++) {
		int subdivision = GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/lights_and_shadows/positional_shadow/atlas_quadrant_" + itos(i) + "_subdiv", PROPERTY_HINT_ENUM, "Disabled,1 Shadow,4 Shadows,16 Shadows,64 Shadows,256 Shadows,1024 Shadows"), i < 2 ? 2 : i + 1);
		server->viewport_set_positional_shadow_atlas_quadrant_subdivision(viewport, i, subdivision ? 1 << (2 * (subdivision - 1)) : 0);
	}
	int vrs_mode = GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/vrs/mode", PROPERTY_HINT_ENUM, String::utf8("Disabled,Texture,XR")), 0);
	String vrs_path = String(GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/vrs/texture", PROPERTY_HINT_FILE, "*.bmp,*.png,*.tga,*.webp"), String())).strip_edges();
	if (vrs_mode == RSE::VIEWPORT_VRS_TEXTURE && !vrs_path.is_empty()) {
		Ref<Image> image;
		image.instantiate();
		error = ImageLoader::load_image(vrs_path, image);
		if (error != OK) {
			_release();
			return error;
		}
		vrs_texture = ImageTexture::create_from_image(image);
		server->viewport_set_vrs_texture(viewport, vrs_texture->get_rid());
		server->viewport_set_vrs_mode(viewport, RSE::VIEWPORT_VRS_TEXTURE);
	}
	String environment_path = String(GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/environment/defaults/default_environment", PROPERTY_HINT_FILE, "*.tres,*.res"), "")).strip_edges();
	if (!environment_path.is_empty()) {
		fallback_environment = ResourceLoader::load(environment_path);
		if (fallback_environment.is_null()) {
			_release();
			return ERR_CANT_OPEN;
		}
		server->scenario_set_fallback_environment(world->get_scenario(), fallback_environment->get_rid());
	}
	interpolation_enabled = GLOBAL_DEF("physics/common/physics_interpolation", false);
	if (interpolation_enabled) {
		Engine::get_singleton()->set_physics_jitter_fix(0);
	}
	DisplayServer *display = DisplayServer::get_singleton();
	display->window_set_rect_changed_callback(callable_mp(this, &EntitySceneRuntime::_window_resized));
	display->window_set_window_event_callback(callable_mp(this, &EntitySceneRuntime::_window_event));
	display->window_set_input_event_callback(callable_mp(this, &EntitySceneRuntime::_input_event));
	callbacks_connected = true;
	display->window_request_hdr_output(GLOBAL_GET("display/window/hdr/request_hdr_output"));
	server->viewport_set_use_hdr_2d(viewport, display->window_is_hdr_output_requested());
	String title = TranslationServer::get_singleton()->translate(GLOBAL_GET("application/config/name"));
#ifdef DEBUG_ENABLED
	title += " (DEBUG)";
#endif
	display->window_set_title(title);
	_window_resized();
	return OK;
}

void EntitySceneRuntime::_window_resized() {
	if (viewport.is_null()) {
		return;
	}
	Size2i window_size = DisplayServer::get_singleton()->window_get_size();
	Size2i size = window_size;
	Rect2 rect(Point2(), window_size);
	String mode = GLOBAL_GET("display/window/stretch/mode");
	double scale = GLOBAL_GET("display/window/stretch/scale");
	if (mode == "viewport") {
		size = Size2i(GLOBAL_GET("display/window/size/viewport_width"), GLOBAL_GET("display/window/size/viewport_height"));
		if (size.x > 0 && size.y > 0 && window_size.x > 0 && window_size.y > 0) {
			String aspect = GLOBAL_GET("display/window/stretch/aspect");
			double sx = double(window_size.x) / size.x;
			double sy = double(window_size.y) / size.y;
			if (aspect == "keep") {
				double factor = MIN(sx, sy);
				if (String(GLOBAL_GET("display/window/stretch/scale_mode")) == "integer") {
					factor = MAX(1.0, Math::floor(factor));
				}
				rect.size = Size2(size) * factor;
				rect.position = (Size2(window_size) - rect.size) * 0.5;
			} else if (aspect == "keep_width") {
				size.y = Math::round(window_size.y / sx);
			} else if (aspect == "keep_height") {
				size.x = Math::round(window_size.x / sy);
			} else if (aspect == "expand") {
				size = Size2i(Size2(window_size) / MIN(sx, sy));
			}
		}
	}
	scale = MAX(scale, 0.01);
	size = Size2i(MAX(1, int(size.x / scale)), MAX(1, int(size.y / scale)));
	RenderingServer::get_singleton()->viewport_set_size(viewport, size.x, size.y);
	RenderingServer::get_singleton()->viewport_attach_to_screen(viewport, rect);
}

void EntitySceneRuntime::_window_event(int p_event) {
	if ((p_event == DisplayServerEnums::WINDOW_EVENT_CLOSE_REQUEST && bool(GLOBAL_GET("application/config/auto_accept_quit"))) || (p_event == DisplayServerEnums::WINDOW_EVENT_GO_BACK_REQUEST && bool(GLOBAL_GET("application/config/quit_on_go_back")))) {
		quit();
	} else if (p_event == DisplayServerEnums::WINDOW_EVENT_FOCUS_IN) {
		Input::get_singleton()->ensure_touch_mouse_raised();
	} else if (p_event == DisplayServerEnums::WINDOW_EVENT_FOCUS_OUT) {
		Input::get_singleton()->release_pressed_events();
	}
}

void EntitySceneRuntime::_notification(int p_what) {
	if (p_what == NOTIFICATION_APPLICATION_FOCUS_IN || p_what == NOTIFICATION_APPLICATION_FOCUS_OUT) {
		Input::get_singleton()->application_focused = p_what == NOTIFICATION_APPLICATION_FOCUS_IN;
		Input::get_singleton()->release_pressed_events();
	}
}

void EntitySceneRuntime::_input_event(const Ref<InputEvent> &p_event) {
	input_events.push_back(p_event);
}

Vector<Ref<InputEvent>> EntitySceneRuntime::take_input_events() {
	Vector<Ref<InputEvent>> result = input_events;
	input_events.clear();
	return result;
}

void EntitySceneRuntime::quit(int p_exit_code) {
	quit_requested = true;
	OS::get_singleton()->set_exit_code(p_exit_code);
}

void EntitySceneRuntime::initialize() {
	ERR_FAIL_NULL(world);
	RenderingServer::get_singleton()->viewport_set_active(viewport, true);
	world->get_transforms().update();
}

void EntitySceneRuntime::iteration_prepare() {
	MessageQueue::get_singleton()->flush();
	world->get_transforms().begin_tick();
}

bool EntitySceneRuntime::physics_process(double p_time) {
	world->get_transforms().update();
	return quit_requested;
}

void EntitySceneRuntime::iteration_end() {
	world->get_transforms().update();
}

bool EntitySceneRuntime::process(double p_time) {
	MessageQueue::get_singleton()->flush();
	world->get_transforms().interpolate(interpolation_enabled ? Engine::get_singleton()->get_physics_interpolation_fraction() : 1.0);
	BaseMaterial3D::flush_changes();
	ParticleProcessMaterial::flush_changes();
	input_events.clear();
	return quit_requested;
}

void EntitySceneRuntime::_release() {
	if (callbacks_connected) {
		DisplayServer *display = DisplayServer::get_singleton();
		display->window_set_rect_changed_callback(Callable());
		display->window_set_window_event_callback(Callable());
		display->window_set_input_event_callback(Callable());
		callbacks_connected = false;
	}
	RenderingServer *server = RenderingServer::get_singleton();
	if (viewport.is_valid()) {
		server->viewport_set_active(viewport, false);
		server->viewport_attach_camera(viewport, RID());
		server->viewport_set_scenario(viewport, RID());
		server->viewport_attach_to_screen(viewport, Rect2(), DisplayServerEnums::INVALID_WINDOW_ID);
		server->sync();
	}
	if (world) {
		memdelete(world);
		world = nullptr;
	}
	if (viewport.is_valid()) {
		server->free_rid(viewport);
		viewport = RID();
	}
	fallback_environment.unref();
	vrs_texture.unref();
	input_events.clear();
}

void EntitySceneRuntime::finalize() {
	_release();
}

EntitySceneRuntime::~EntitySceneRuntime() {
	_release();
}
