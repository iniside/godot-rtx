extends Node3D

@export_enum("gallery", "shadows", "energy", "unsupported") var preset := "gallery"
@export_enum("all", "directional", "point", "spot", "area", "environment", "emission") var light_mode := "all"
@export var expanded_multimesh := false

var camera: Camera3D
var alternate_camera: Camera3D
var environment: Environment
var mover: Node3D
var deformer: MeshInstance3D
var lights: Array[Light3D] = []
var hud: Label
var elapsed := 0.0
var animate := true
var capture_path := ""
var capture_delay := 10.0
var capturing := false
var secondary: SubViewportContainer

func _ready() -> void:
	if expanded_multimesh:
		ProjectSettings.set_setting("rendering/raytracing/multimesh_merged_blas_max_triangles", 0)
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--capture="):
			capture_path = argument.trim_prefix("--capture=")
		elif argument.begins_with("--delay="):
			capture_delay = float(argument.trim_prefix("--delay="))
		elif argument.begins_with("--light="):
			light_mode = argument.trim_prefix("--light=")
		elif argument == "--still":
			animate = false
		elif argument == "--expanded":
			ProjectSettings.set_setting("rendering/raytracing/multimesh_merged_blas_max_triangles", 0)
	camera = get_node("Cameras/Camera")
	alternate_camera = get_node("Cameras/AlternateCamera")
	environment = get_node("WorldEnvironment").environment
	mover = get_node_or_null("Geometry/MovingCaster")
	deformer = get_node_or_null("Geometry/DeformingCaster/Deformer")
	if deformer != null:
		deformer.mesh = deformer.mesh.duplicate() as ArrayMesh
	hud = get_node("HUD/Label")
	var light_group := get_node("Lights")
	for child in light_group.get_children():
		if child is Light3D:
			lights.append(child)
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	for light in lights:
		light.visible = light_mode == "all" or light.name.to_lower() == light_mode
	if light_mode != "all" and light_mode != "environment":
		environment.background_mode = Environment.BG_COLOR
		environment.sky = null
	var emitter := get_node_or_null("Geometry/TexturedEmitter")
	if emitter != null:
		emitter.visible = light_mode in ["all", "emission"]
	if "--fog" in OS.get_cmdline_user_args():
		environment.volumetric_fog_enabled = true
	if "--dual" in OS.get_cmdline_user_args():
		_toggle_secondary()
	if "--upscale" in OS.get_cmdline_user_args():
		get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
		get_viewport().scaling_3d_scale = 0.67
	if preset == "shadows":
		print("MANUAL_SHADOWS columns=back/front/disabled/double-sided/mirrored/negative-local-MultiMesh rows=up/down mm_threshold=", ProjectSettings.get_setting("rendering/raytracing/multimesh_merged_blas_max_triangles"))
	print("MANUAL_DEMO preset=", preset, " light=", light_mode, " binary=", Engine.get_version_info(), " renderer=", RenderingServer.get_current_rendering_method(), " driver=", RenderingServer.get_current_rendering_driver_name(), " size=", get_viewport().get_visible_rect().size)

func _toggle_secondary() -> void:
	if is_instance_valid(secondary):
		secondary.queue_free()
		secondary = null
		return
	secondary = SubViewportContainer.new()
	secondary.position = Vector2(875, 470)
	secondary.size = Vector2(384, 216)
	add_child(secondary)
	var viewport := SubViewport.new()
	viewport.size = Vector2i(384, 216)
	viewport.world_3d = get_world_3d()
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	secondary.add_child(viewport)
	var other_camera := Camera3D.new()
	other_camera.position = Vector3(-10, 6, 10)
	viewport.add_child(other_camera)
	other_camera.look_at(Vector3(0, 1, 0))
	other_camera.current = true
	RenderingServer.viewport_set_measure_render_time(viewport.get_viewport_rid(), true)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_ESCAPE:
				get_tree().quit()
			KEY_SPACE:
				animate = not animate
			KEY_F:
				environment.volumetric_fog_enabled = not environment.volumetric_fog_enabled
			KEY_V:
				_toggle_secondary()
			KEY_C:
				if camera.current:
					alternate_camera.make_current()
				else:
					camera.make_current()
			KEY_L:
				if not lights.is_empty():
					var removed: Light3D = lights.pop_front()
					removed.queue_free()
			KEY_R:
				lights.reverse()
				for light in lights:
					var parent := light.get_parent()
					parent.remove_child(light)
					parent.add_child(light)
			KEY_Z:
				get_window().size = Vector2i(1024, 640) if get_window().size.x == 1280 else Vector2i(1280, 720)
			KEY_B:
				for light in lights:
					light.shadow_enabled = not light.shadow_enabled
			KEY_O:
				var offscreen := get_node_or_null("Geometry/OffscreenCaster")
				if offscreen != null:
					offscreen.visible = not offscreen.visible
			KEY_DELETE:
				if is_instance_valid(deformer):
					deformer.queue_free()
					deformer = null
			KEY_U:
				get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
				get_viewport().scaling_3d_scale = 0.67 if get_viewport().scaling_3d_scale == 1.0 else 1.0
			KEY_F12:
				_capture("user://manual-%d.png" % Time.get_unix_time_from_system(), false)
		print("MANUAL_ACTION key=", event.keycode, " frame=", Engine.get_frames_drawn(), " lights=", lights.size(), " size=", get_window().size)
	if event is InputEventMouseMotion and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		var active := get_viewport().get_camera_3d()
		active.rotate_y(-event.relative.x * 0.003)
		active.rotate_object_local(Vector3.RIGHT, -event.relative.y * 0.003)

func _process(delta: float) -> void:
	elapsed += delta
	if is_instance_valid(secondary):
		secondary.position = get_viewport().get_visible_rect().size - secondary.size - Vector2(20, 20)
	if animate and is_instance_valid(mover):
		mover.position.x = sin(elapsed) * 2.0
		mover.rotation.y = elapsed
	if animate and is_instance_valid(deformer):
		deformer.set_blend_shape_value(0, sin(elapsed * 1.3) * 0.5 + 0.5)
	var active := get_viewport().get_camera_3d()
	var direction := Vector3(float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)), float(Input.is_physical_key_pressed(KEY_E)) - float(Input.is_physical_key_pressed(KEY_Q)), float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
	active.position += active.basis * direction * delta * 6.0
	hud.text = "RTXDI / NRD manual demo  |  %s / %s\nRMB + WASD/QE move  |  Space motion  C cut  V second view\nF fog  U FSR2  Z resize  L remove light  R reorder  B shadows\nO offscreen caster  Del remove deformer  F12 capture  Esc quit\n%s  |  motion %s" % [preset, light_mode, get_viewport().get_visible_rect().size, animate]
	if not capture_path.is_empty() and elapsed >= capture_delay and not capturing:
		capturing = true
		_capture(capture_path, true)

func _capture(path: String, quit_after: bool) -> void:
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	var result := image.save_png(path)
	print("MANUAL_CAPTURE path=", ProjectSettings.globalize_path(path), " result=", result, " frame=", Engine.get_frames_drawn(), " gpu_ms=", RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()))
	print("MANUAL_MEMORY total=", Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED), " textures=", Performance.get_monitor(Performance.RENDER_TEXTURE_MEM_USED), " buffers=", Performance.get_monitor(Performance.RENDER_BUFFER_MEM_USED))
	if quit_after:
		get_tree().quit()
