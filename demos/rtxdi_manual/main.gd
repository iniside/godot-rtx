extends Node3D

var camera: Camera3D
var alternate_camera: Camera3D
var environment: Environment
var mover: MeshInstance3D
var deformer: MeshInstance3D
var lights: Array[Light3D] = []
var hud: Label
var elapsed := 0.0
var animate := true
var capture_path := ""
var capture_delay := 10.0
var capturing := false
@export_enum("gallery", "shadows", "energy", "unsupported") var preset := "gallery"
@export_enum("all", "directional", "point", "spot", "area", "environment", "emission") var light_mode := "all"
@export var expanded_multimesh := false
var secondary: SubViewportContainer
var source_meshes: Dictionary[String, Mesh] = {}

func _ready() -> void:
	if expanded_multimesh:
		ProjectSettings.set_setting("rendering/raytracing/multimesh_merged_blas_max_triangles", 0)
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--capture="):
			capture_path = argument.trim_prefix("--capture=")
		elif argument.begins_with("--delay="):
			capture_delay = float(argument.trim_prefix("--delay="))
		elif argument.begins_with("--preset="):
			preset = argument.trim_prefix("--preset=")
		elif argument.begins_with("--light="):
			light_mode = argument.trim_prefix("--light=")
		elif argument == "--still":
			animate = false
		elif argument == "--expanded":
			ProjectSettings.set_setting("rendering/raytracing/multimesh_merged_blas_max_triangles", 0)
	_build_environment()
	if preset == "shadows":
		_build_shadows()
	elif preset == "energy":
		_build_energy()
	else:
		_build_gallery()
		if preset == "unsupported":
			_build_unsupported()
	_build_cameras()
	_build_hud()
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	for light in lights:
		light.visible = light_mode == "all" or light.name.to_lower() == light_mode
	if light_mode != "all" and light_mode != "environment":
		environment.background_mode = Environment.BG_COLOR
		environment.sky = null
	if has_node("TexturedEmitter"):
		get_node("TexturedEmitter").visible = light_mode in ["all", "emission"]
	if "--fog" in OS.get_cmdline_user_args():
		environment.volumetric_fog_enabled = true
	if "--dual" in OS.get_cmdline_user_args():
		_toggle_secondary()
	if "--upscale" in OS.get_cmdline_user_args():
		get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
		get_viewport().scaling_3d_scale = 0.67
	print("MANUAL_DEMO preset=", preset, " light=", light_mode, " binary=", Engine.get_version_info(), " renderer=", RenderingServer.get_current_rendering_method(), " driver=", RenderingServer.get_current_rendering_driver_name(), " size=", get_viewport().get_visible_rect().size)

func _material(color: Color, metal: float = 0.0, rough: float = 0.65) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.metallic = metal
	material.roughness = rough
	return material

func _mesh(mesh: Mesh, material: Material, at: Vector3, node_name: String) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.name = node_name
	if mesh is BoxMesh:
		instance.mesh = _source_mesh("Cube")
		instance.scale = mesh.size
	elif mesh is PlaneMesh:
		instance.mesh = _source_mesh("Plane")
		instance.scale = Vector3(mesh.size.x, 1.0, mesh.size.y)
	elif mesh is SphereMesh:
		instance.mesh = _source_mesh("Sphere")
		instance.scale = Vector3(mesh.radius, mesh.height * 0.5, mesh.radius)
	else:
		instance.mesh = mesh
	instance.material_override = material
	instance.position = at
	add_child(instance)
	return instance

func _source_mesh(node_name: String) -> Mesh:
	if source_meshes.is_empty():
		var source: Node = load("res://geometry.gltf").instantiate()
		for child in source.get_children():
			if child is MeshInstance3D:
				source_meshes[child.name] = child.mesh
		source.free()
	return source_meshes[node_name]

func _box(size: Vector3, at: Vector3, material: Material, node_name: String) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	return _mesh(mesh, material, at, node_name)

func _pattern(masked: bool) -> ImageTexture:
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var bright := (x / 8 + y / 8) % 2 == 0
			image.set_pixel(x, y, Color(1.0, 0.5, 0.1, 1.0) if bright else Color(0.08, 0.25, 1.0, 0.0 if masked else 1.0))
	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)

func _build_environment() -> void:
	environment = Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.tonemap_mode = Environment.TONE_MAPPER_ACES
	environment.background_energy_multiplier = 0.3
	environment.volumetric_fog_density = 0.025
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.12, 0.2, 0.4)
	sky_material.sky_horizon_color = Color(0.4, 0.45, 0.5)
	sky_material.ground_bottom_color = Color(0.03, 0.03, 0.03)
	sky_material.ground_horizon_color = Color(0.4, 0.45, 0.5)
	sky.sky_material = sky_material
	environment.sky = sky
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)

func _add_light(light: Light3D, node_name: String, at: Vector3, energy: float, color: Color) -> void:
	light.name = node_name
	light.position = at
	light.light_energy = energy
	light.light_color = color
	light.shadow_enabled = true
	add_child(light)
	lights.append(light)

func _build_gallery() -> void:
	_box(Vector3(22, 0.3, 18), Vector3(0, -0.15, 0), _material(Color(0.55, 0.55, 0.55)), "Floor")
	_box(Vector3(22, 6, 0.3), Vector3(0, 3, -7), _material(Color(0.35, 0.4, 0.48)), "BackWall")
	for row in range(2):
		for column in range(4):
			var sphere := SphereMesh.new()
			sphere.radius = 0.75
			sphere.height = 1.5
			_mesh(sphere, _material(Color(0.7, 0.38, 0.12) if row == 1 else Color(0.65, 0.7, 0.75), float(row), 0.12 + column * 0.25), Vector3(-4.5 + column * 3.0, 0.9, -3.0 + row * 3.0), "PBR_%d_%d" % [row, column])
	var masked := _material(Color.WHITE)
	masked.albedo_texture = _pattern(true)
	masked.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	masked.alpha_scissor_threshold = 0.5
	masked.cull_mode = BaseMaterial3D.CULL_DISABLED
	var panel := PlaneMesh.new()
	panel.size = Vector2(2.5, 2.5)
	var cutout := _mesh(panel, masked, Vector3(-7, 2.5, 0), "MaskedPanel")
	cutout.rotation_degrees.x = 65
	var emission := _material(Color(0.0, 0.0, 0.0))
	emission.emission_enabled = true
	emission.emission = Color.WHITE
	emission.emission_energy_multiplier = 5.0
	emission.emission_operator = BaseMaterial3D.EMISSION_OP_MULTIPLY
	emission.emission_texture = _pattern(false)
	var emitter := _mesh(panel, emission, Vector3(7, 2.5, 0), "TexturedEmitter")
	emitter.rotation_degrees = Vector3(60, -25, 0)
	mover = _box(Vector3(0.8, 1.5, 0.8), Vector3(0, 2.5, 2.5), _material(Color(0.15, 0.65, 0.3)), "MovingCaster")
	deformer = _mesh(_source_mesh("Deformer"), _material(Color(0.6, 0.15, 0.5), 0.0, 0.3), Vector3(3, 2.3, 3.5), "DeformingCaster")
	deformer.scale = Vector3.ONE * 0.6
	deformer.set_blend_shape_value(0, 0.65)
	_box(Vector3(2.0, 1.0, 2.0), Vector3(0, 12, 1), _material(Color(0.3, 0.3, 0.3)), "OffscreenCaster")
	var sun := DirectionalLight3D.new()
	_add_light(sun, "Directional", Vector3.ZERO, 0.8, Color(1.0, 0.95, 0.85))
	sun.rotation_degrees = Vector3(-55, -25, 0)
	var point := OmniLight3D.new()
	point.omni_range = 20
	_add_light(point, "Point", Vector3(-4, 4, 1), 6, Color(1.0, 0.3, 0.15))
	var spot := SpotLight3D.new()
	spot.spot_range = 22
	spot.spot_angle = 40
	_add_light(spot, "Spot", Vector3(4, 5, 3), 9, Color(0.2, 0.45, 1.0))
	spot.look_at(Vector3(1, 0, 0))
	var area := AreaLight3D.new()
	area.area_range = 18
	area.area_size = Vector2(3, 2)
	_add_light(area, "Area", Vector3(0, 5, -4), 12, Color(0.5, 1.0, 0.7))
	area.look_at(Vector3(0, 0, 0))

func _build_shadows() -> void:
	environment.background_mode = Environment.BG_COLOR
	_box(Vector3(30, 0.3, 18), Vector3(0, -0.15, 0), _material(Color(0.65, 0.65, 0.65)), "ShadowReceiver")
	var sun := DirectionalLight3D.new()
	_add_light(sun, "Directional", Vector3.ZERO, 1.0, Color.WHITE)
	sun.rotation_degrees = Vector3(-60, -20, 0)
	for row in range(2):
		for column in range(5):
			var material := _material(Color(0.2, 0.45, 0.6))
			material.cull_mode = column if column < 3 else BaseMaterial3D.CULL_BACK
			var plane := PlaneMesh.new()
			plane.size = Vector2(2, 2)
			var instance := _mesh(plane, material, Vector3(-10 + column * 4, 2.5, -3 + row * 6), "Facing_%d_%d" % [row, column])
			instance.rotation_degrees.z = 180 * row
			if column == 3:
				instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_DOUBLE_SIDED
			if column == 4:
				instance.scale.x *= -1
	var plane := _source_mesh("Plane").duplicate() as ArrayMesh
	plane.surface_set_material(0, _material(Color(0.6, 0.3, 0.1)))
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = plane
	multimesh.instance_count = 2
	multimesh.set_instance_transform(0, Transform3D(Basis.from_scale(Vector3(-2, 1, 2)), Vector3(10, 2.5, -3)))
	multimesh.set_instance_transform(1, Transform3D(Basis(Vector3.RIGHT, PI).scaled(Vector3(-2, 1, 2)), Vector3(10, 2.5, 3)))
	var instance := MultiMeshInstance3D.new()
	instance.multimesh = multimesh
	instance.name = "MirroredMultiMesh"
	add_child(instance)
	print("MANUAL_SHADOWS columns=back/front/disabled/double-sided/mirrored/negative-local-MultiMesh rows=up/down mm_threshold=", ProjectSettings.get_setting("rendering/raytracing/multimesh_merged_blas_max_triangles"))

func _build_energy() -> void:
	environment.background_mode = Environment.BG_COLOR
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var material := _material(Color(0.5, 0.5, 0.5))
	material.metallic_specular = 0.0
	var plane := PlaneMesh.new()
	plane.size = Vector2(10, 10)
	_mesh(plane, material, Vector3.ZERO, "EnergyReceiver")
	if light_mode == "emission":
		material.albedo_color = Color.BLACK
		material.emission_enabled = true
		material.emission = Color(0.125, 0.25, 0.5)
	else:
		var sun := DirectionalLight3D.new()
		_add_light(sun, "Directional", Vector3.ZERO, 1.0, Color.WHITE)
		sun.rotation_degrees.x = -90

func _build_unsupported() -> void:
	var custom := ShaderMaterial.new()
	custom.shader = Shader.new()
	custom.shader.code = "shader_type spatial; void fragment() { ALBEDO = vec3(0.2); } void light() { DIFFUSE_LIGHT += vec3(1.0); }"
	_box(Vector3.ONE * 2, Vector3(-3, 1, 4), custom, "UnsupportedCustomLight")
	var anisotropic := _material(Color(0.2, 0.3, 0.8))
	anisotropic.anisotropy_enabled = true
	anisotropic.anisotropy = 0.7
	_box(Vector3.ONE * 2, Vector3(0, 1, 4), anisotropic, "UnsupportedAnisotropy")
	var blend := _material(Color(0.8, 0.2, 0.2, 0.5))
	blend.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_box(Vector3.ONE * 2, Vector3(3, 1, 4), blend, "UnsupportedBlend")

func _build_cameras() -> void:
	camera = Camera3D.new()
	camera.position = Vector3(8, 6, 11)
	camera.far = 100
	add_child(camera)
	camera.look_at(Vector3(0, 1.0, -0.5))
	if preset == "shadows":
		camera.position = Vector3(0, 22, 0)
		camera.rotation_degrees = Vector3(-90, 0, 0)
		camera.projection = Camera3D.PROJECTION_ORTHOGONAL
		camera.size = 22
	elif preset == "energy":
		camera.position = Vector3(0, 6, 0)
		camera.rotation_degrees = Vector3(-90, 0, 0)
	camera.current = true
	alternate_camera = Camera3D.new()
	alternate_camera.position = Vector3(-10, 5, 9)
	add_child(alternate_camera)
	alternate_camera.look_at(Vector3(0, 1, -1))

func _build_hud() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	hud = Label.new()
	hud.position = Vector2(18, 14)
	hud.add_theme_font_size_override("font_size", 18)
	layer.add_child(hud)

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
					remove_child(light)
					add_child(light)
			KEY_Z:
				get_window().size = Vector2i(1024, 640) if get_window().size.x == 1280 else Vector2i(1280, 720)
			KEY_B:
				for light in lights:
					light.shadow_enabled = not light.shadow_enabled
			KEY_O:
				if has_node("OffscreenCaster"):
					get_node("OffscreenCaster").visible = not get_node("OffscreenCaster").visible
			KEY_DELETE:
				if is_instance_valid(deformer):
					deformer.queue_free()
					source_meshes.erase("Deformer")
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
