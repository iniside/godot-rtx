extends Node3D

@onready var environment: Environment = $WorldEnvironment.environment
@onready var glowing_box: Node3D = $Geometry/GlowingBox
@onready var lights: Node3D = $Lights
@onready var camera: Camera3D = $Cameras/Camera
@onready var fps: Label = $HUD/FPS
@onready var state: Label = $HUD/State

const DDGI_SEQUENCE_FRAMES := 96
const DDGI_MOTION_FRAMES := 32

var elapsed := 0.0
var animate := true
var capture_path := ""
var capture_delay := 8.0
var capturing := false
var ddgi_motion := ""
var ddgi_distance := 4.0
var last_process_delta := 0.0
var sequence_initial_transform := Transform3D()
var sequence_previous_animate := true
var sequence_previous_process := true
var sequence_previous_process_input := true

func _ready() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--capture="):
			capture_path = argument.trim_prefix("--capture=")
		elif argument.begins_with("--delay="):
			capture_delay = float(argument.trim_prefix("--delay="))
		elif argument.begins_with("--ddgi-view="):
			if not _set_ddgi_view(argument.trim_prefix("--ddgi-view=")):
				_quit_invalid_argument("unknown --ddgi-view value")
				return
		elif argument == "--ddgi-freeze":
			get_viewport().ddgi_debug_freeze_anchor = true
		elif argument.begins_with("--ddgi-motion="):
			ddgi_motion = argument.trim_prefix("--ddgi-motion=")
		elif argument.begins_with("--ddgi-distance="):
			ddgi_distance = float(argument.trim_prefix("--ddgi-distance="))
	if not ddgi_motion.is_empty():
		if ddgi_motion not in ["forward-return", "rotation"]:
			_quit_invalid_argument("--ddgi-motion must be forward-return or rotation")
			return
		if ddgi_distance <= 0.0:
			_quit_invalid_argument("--ddgi-distance must be greater than zero")
			return
		if capture_path.is_empty():
			_quit_invalid_argument("--ddgi-motion requires --capture=<path>.png")
			return
		if capture_path.get_extension().to_lower() != "png":
			_quit_invalid_argument("--ddgi-motion capture path must end in .png")
			return
		var parent_path := ProjectSettings.globalize_path(capture_path).get_base_dir()
		if parent_path.is_empty() or not DirAccess.dir_exists_absolute(parent_path):
			_quit_invalid_argument("capture parent directory does not exist: " + parent_path)
			return
		_prepare_ddgi_sequence()
	print("MIGRATED_GI_DEMO renderer=", RenderingServer.get_current_rendering_method(), " driver=", RenderingServer.get_current_rendering_driver_name(), " map_instances=1 ", _runtime_settings_log())

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_SPACE:
				animate = not animate
			KEY_B:
				for light in lights.get_children():
					if light is Light3D:
						light.shadow_enabled = not light.shadow_enabled
			KEY_F:
				environment.volumetric_fog_enabled = not environment.volumetric_fog_enabled
			KEY_0:
				_set_ddgi_view("normal")
			KEY_6:
				_set_ddgi_view("probes")
			KEY_7:
				_set_ddgi_view("state")
			KEY_8:
				_set_ddgi_view("weights")
			KEY_9:
				_set_ddgi_view("indirect")
			KEY_G:
				get_viewport().ddgi_debug_freeze_anchor = not get_viewport().ddgi_debug_freeze_anchor
		print("MIGRATED_DDGI_ACTION key=", event.keycode, " frame=", Engine.get_frames_drawn(), " ", _runtime_settings_log())

func _process(delta: float) -> void:
	last_process_delta = delta
	elapsed += delta
	if animate:
		glowing_box.position.y = -0.108265 + sin(elapsed * 1.2) * 0.6
		glowing_box.rotation.y = sin(elapsed * 0.4) * 0.25
	state.text = "%s / %s | DDGI %s: %d cascades, %.2f m, %d rays, %d updates/frame\nView %s | anchor %s | PT %d spp / %d bounces | fog %s | cube motion %s\n0 normal  6 probes  7 state  8 weights  9 indirect  G freeze\nSpace cube motion  B shadows  F fog  Esc/F10 mouse | WASD/arrows/Q/E move" % [_rendering_mode_name(), _denoiser_name(), "on" if environment.ddgi_enabled else "off", environment.ddgi_cascade_count, environment.ddgi_probe_spacing, environment.ddgi_rays_per_probe, environment.ddgi_updates_per_frame, _ddgi_view_name(), "frozen" if get_viewport().ddgi_debug_freeze_anchor else "moving", environment.pathtracing_samples_per_pixel, environment.pathtracing_max_bounces, environment.volumetric_fog_enabled, animate]
	if not ddgi_motion.is_empty() and elapsed >= capture_delay and not capturing:
		capturing = true
		_run_ddgi_sequence()
	elif ddgi_motion.is_empty() and not capture_path.is_empty() and elapsed >= capture_delay and not capturing:
		capturing = true
		_capture(capture_path)

func _physics_process(_delta: float) -> void:
	var frame_rate := maxf(Engine.get_frames_per_second(), 0.001)
	fps.text = "%d FPS (%.2f mspf)" % [frame_rate, 1000.0 / frame_rate]

func _capture(path: String) -> void:
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	var result := image.save_png(path)
	print("MIGRATED_CAPTURE path=", ProjectSettings.globalize_path(path), " result=", result, " frame=", Engine.get_frames_drawn(), " wall_unix=", Time.get_unix_time_from_system(), " frame_delta=", last_process_delta, " camera_position=", camera.global_position, " camera_rotation=", camera.global_rotation, " ", _runtime_settings_log())
	get_tree().quit()

func _set_ddgi_view(view_name: String) -> bool:
	match view_name:
		"normal":
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
		"probes":
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DDGI_PROBES
		"state":
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DDGI_PROBE_STATE
		"weights":
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DDGI_CASCADE_WEIGHTS
		"indirect":
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DDGI_INDIRECT
		_:
			return false
	return true

func _ddgi_view_name() -> String:
	match get_viewport().debug_draw:
		Viewport.DEBUG_DRAW_DDGI_PROBES:
			return "probes"
		Viewport.DEBUG_DRAW_DDGI_PROBE_STATE:
			return "state"
		Viewport.DEBUG_DRAW_DDGI_CASCADE_WEIGHTS:
			return "weights"
		Viewport.DEBUG_DRAW_DDGI_INDIRECT:
			return "indirect"
		_:
			return "normal"

func _rendering_mode_name() -> String:
	return "path_traced" if environment.raytracing_rendering_mode == Environment.RAYTRACING_RENDERING_MODE_PATH_TRACED else "hybrid"

func _denoiser_name() -> String:
	match environment.raytracing_denoiser:
		Environment.RAYTRACING_DENOISER_DLSS_RR:
			return "dlss_rr"
		Environment.RAYTRACING_DENOISER_NONE:
			return "none"
		_:
			return "nrd"

func _runtime_settings_log() -> String:
	return "mode=%s denoiser=%s ddgi=%s cascades=%d spacing=%.3f rays=%d updates=%d pt_spp=%d pt_bounces=%d debug_view=%s freeze=%s fog=%s cube_motion=%s distance=%.3f" % [_rendering_mode_name(), _denoiser_name(), environment.ddgi_enabled, environment.ddgi_cascade_count, environment.ddgi_probe_spacing, environment.ddgi_rays_per_probe, environment.ddgi_updates_per_frame, environment.pathtracing_samples_per_pixel, environment.pathtracing_max_bounces, _ddgi_view_name(), get_viewport().ddgi_debug_freeze_anchor, environment.volumetric_fog_enabled, animate, ddgi_distance]

func _quit_invalid_argument(reason: String) -> void:
	print("MIGRATED_DDGI_INVALID_ARGUMENT reason=", reason)
	get_tree().quit(1)

func _prepare_ddgi_sequence() -> void:
	sequence_initial_transform = camera.global_transform
	sequence_previous_animate = animate
	sequence_previous_process = camera.is_processing()
	sequence_previous_process_input = camera.is_processing_input()
	animate = false
	camera.set_process(false)
	camera.set_process_input(false)
	print("MIGRATED_DDGI_SEQUENCE_READY motion=", ddgi_motion, " frames=", DDGI_SEQUENCE_FRAMES, " delay=", capture_delay, " capture=", ProjectSettings.globalize_path(capture_path), " ", _runtime_settings_log())

func _run_ddgi_sequence() -> void:
	for sequence_frame in DDGI_SEQUENCE_FRAMES:
		_apply_ddgi_sequence_pose(sequence_frame)
		await RenderingServer.frame_post_draw
		var frame_path := "%s-%03d.png" % [capture_path.get_basename(), sequence_frame]
		var result := get_viewport().get_texture().get_image().save_png(frame_path)
		print("MIGRATED_DDGI_FRAME sequence=", sequence_frame, " phase=", _ddgi_sequence_phase(sequence_frame), " engine_frame=", Engine.get_frames_drawn(), " wall_unix=", Time.get_unix_time_from_system(), " frame_delta=", last_process_delta, " camera_position=", camera.global_position, " camera_rotation=", camera.global_rotation, " capture=", ProjectSettings.globalize_path(frame_path), " result=", result, " motion=", ddgi_motion, " ", _runtime_settings_log())
	_restore_ddgi_sequence()
	print("MIGRATED_DDGI_SEQUENCE_DONE frames=", DDGI_SEQUENCE_FRAMES, " final_position=", camera.global_position, " final_rotation=", camera.global_rotation)
	get_tree().quit()

func _apply_ddgi_sequence_pose(sequence_frame: int) -> void:
	var amount := 0.0
	if sequence_frame < DDGI_MOTION_FRAMES:
		amount = float(sequence_frame + 1) / DDGI_MOTION_FRAMES
	elif sequence_frame < DDGI_MOTION_FRAMES * 2:
		amount = float(DDGI_MOTION_FRAMES * 2 - sequence_frame - 1) / DDGI_MOTION_FRAMES
	var pose := sequence_initial_transform
	if ddgi_motion == "forward-return":
		pose.origin += -sequence_initial_transform.basis.z.normalized() * ddgi_distance * amount
	else:
		pose.basis = Basis(Vector3.UP, deg_to_rad(24.0 * amount)) * sequence_initial_transform.basis
	camera.global_transform = pose

func _ddgi_sequence_phase(sequence_frame: int) -> String:
	if sequence_frame < DDGI_MOTION_FRAMES:
		return "forward" if ddgi_motion == "forward-return" else "rotate"
	if sequence_frame < DDGI_MOTION_FRAMES * 2:
		return "return"
	return "recovery"

func _restore_ddgi_sequence() -> void:
	camera.global_transform = sequence_initial_transform
	camera.set_process(sequence_previous_process)
	camera.set_process_input(sequence_previous_process_input)
	animate = sequence_previous_animate
