extends Node3D

@export var dragon_emissive_material: StandardMaterial3D
@export var dragon_masked_material: StandardMaterial3D

const DRAGON_PATH := "res://microgeometry/xyzrgb_dragon.glb"
const DEFAULT_NATIVE_PATH := "res://microgeometry/native_roundtrip.res"
const RT_ERRORS := [0.5, 4.0, 16.0]
const OFFSCREEN_MULTIPLIERS := [1.0, 2.0, 8.0]

@onready var environment: Environment = $WorldEnvironment.environment
@onready var geometry: Node3D = $Geometry
@onready var camera: Camera3D = $Cameras/Camera
@onready var secondary_camera: Camera3D = $SecondaryView/SubViewport/Camera
@onready var secondary_container: SubViewportContainer = $SecondaryView
@onready var secondary_viewport: SubViewport = $SecondaryView/SubViewport
@onready var multimesh_instance: MultiMeshInstance3D = $Geometry/DragonMultiMesh
@onready var hud: Label = $HUD/State

var primary_dragon: Node3D
var moving_dragon: Node3D
var offscreen_dragon: Node3D
var primary_mesh: MeshInstance3D
var moving_mesh: MeshInstance3D
var deformer: MeshInstance3D
var animate := true
var emissive := false
var frozen := false
var dragons_loaded := true
var elapsed := 0.0
var debug_mode := Viewport.DEBUG_DRAW_DISABLED
var rt_error_index := 1
var offscreen_multiplier_index := 1
var capture_path := ""
var capture_delay := 10.0
var capturing := false
var freeze_after := -1.0
var freeze_after_triggered := false
var orbit := false
var primary_transform: Transform3D
var moving_transform: Transform3D
var offscreen_transform: Transform3D

func _ready() -> void:
	primary_dragon = $Geometry/Dragon
	moving_dragon = $Geometry/MovingDragon
	offscreen_dragon = $Geometry/OffscreenDragon
	primary_transform = primary_dragon.transform
	moving_transform = moving_dragon.transform
	offscreen_transform = offscreen_dragon.transform
	primary_mesh = _find_mesh_instance(primary_dragon)
	moving_mesh = _find_mesh_instance(moving_dragon)
	if moving_mesh != null:
		moving_mesh.material_override = dragon_masked_material
	deformer = $Geometry/DeformingCaster/Deformer
	if deformer != null and deformer.mesh is ArrayMesh:
		deformer.mesh = deformer.mesh.duplicate() as ArrayMesh
	camera.look_at(Vector3(0.0, 1.2, 0.0))
	secondary_camera.look_at(Vector3(0.0, 1.0, 0.0))
	secondary_viewport.world_3d = get_world_3d()
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	RenderingServer.viewport_set_measure_render_time(secondary_viewport.get_viewport_rid(), true)
	_rebuild_multimesh()
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--capture="):
			capture_path = argument.trim_prefix("--capture=")
		elif argument.begins_with("--delay="):
			capture_delay = float(argument.trim_prefix("--delay="))
		elif argument.begins_with("--freeze-after="):
			freeze_after = float(argument.trim_prefix("--freeze-after="))
		elif argument.begins_with("--save-native="):
			_save_native(argument.trim_prefix("--save-native="))
		elif argument.begins_with("--rt-error="):
			environment.raytracing_geometry_error = float(argument.trim_prefix("--rt-error="))
		elif argument.begins_with("--offscreen-multiplier="):
			environment.raytracing_geometry_offscreen_multiplier = float(argument.trim_prefix("--offscreen-multiplier="))
		elif argument.begins_with("--far="):
			camera.far = float(argument.trim_prefix("--far="))
		elif argument == "--view=normal":
			_set_debug_draw(Viewport.DEBUG_DRAW_DISABLED)
		elif argument == "--view=raster":
			_set_debug_draw(Viewport.DEBUG_DRAW_MICRO_GEOMETRY_RASTER)
		elif argument == "--view=rt":
			_set_debug_draw(Viewport.DEBUG_DRAW_MICRO_GEOMETRY_RT)
		elif argument == "--freeze":
			_set_frozen(true)
		elif argument == "--still":
			animate = false
		elif argument == "--emissive":
			_toggle_emissive()
		elif argument == "--secondary":
			_set_secondary_visible(true)
		elif argument == "--orbit":
			orbit = true
	print("MICROGEOMETRY_MANUAL renderer=", RenderingServer.get_current_rendering_method(), " driver=", RenderingServer.get_current_rendering_driver_name(), " dragon=", DRAGON_PATH)

func _find_mesh_instance(root: Node) -> MeshInstance3D:
	if root is MeshInstance3D:
		return root as MeshInstance3D
	for child in root.get_children():
		var found := _find_mesh_instance(child)
		if found != null:
			return found
	return null

func _set_debug_draw(mode: int) -> void:
	debug_mode = mode
	get_viewport().debug_draw = mode
	secondary_viewport.debug_draw = mode
	print("MICROGEOMETRY_DEBUG mode=", mode, " frozen=", frozen)

func _set_frozen(enabled: bool) -> void:
	frozen = enabled
	get_viewport().micro_geometry_debug_freeze = enabled
	secondary_viewport.micro_geometry_debug_freeze = enabled
	print("MICROGEOMETRY_FREEZE enabled=", enabled)

func _set_secondary_visible(enabled: bool) -> void:
	secondary_container.visible = enabled
	secondary_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS if enabled else SubViewport.UPDATE_DISABLED

func _rebuild_multimesh() -> void:
	if primary_mesh == null or primary_mesh.mesh == null:
		multimesh_instance.multimesh = null
		return
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.instance_count = 3
	multimesh.mesh = primary_mesh.mesh
	multimesh.set_instance_transform(0, Transform3D(Basis.IDENTITY.scaled(Vector3.ONE * 0.007), Vector3(-3.0, 0.39209, -4.5)))
	multimesh.set_instance_transform(1, Transform3D(Basis(Vector3.UP, 2.1).scaled(Vector3.ONE * 0.007), Vector3(0.0, 0.39209, -4.5)))
	multimesh.set_instance_transform(2, Transform3D(Basis(Vector3.UP, 4.2).scaled(Vector3.ONE * 0.007), Vector3(3.0, 0.39209, -4.5)))
	multimesh_instance.multimesh = multimesh

func _toggle_emissive() -> void:
	emissive = not emissive
	if primary_mesh != null:
		primary_mesh.material_override = dragon_emissive_material if emissive else null
	print("MICROGEOMETRY_EMISSIVE enabled=", emissive)

func _toggle_dragons() -> void:
	if dragons_loaded:
		multimesh_instance.multimesh = null
		for dragon in [primary_dragon, moving_dragon, offscreen_dragon]:
			if is_instance_valid(dragon):
				dragon.queue_free()
		primary_dragon = null
		moving_dragon = null
		offscreen_dragon = null
		primary_mesh = null
		moving_mesh = null
		dragons_loaded = false
		print("MICROGEOMETRY_INSTANCES loaded=false")
		return
	var packed := ResourceLoader.load(DRAGON_PATH, "PackedScene", ResourceLoader.CACHE_MODE_REPLACE) as PackedScene
	if packed == null:
		push_error("Could not reload " + DRAGON_PATH)
		return
	primary_dragon = _add_dragon(packed, "Dragon", primary_transform)
	moving_dragon = _add_dragon(packed, "MovingDragon", moving_transform)
	offscreen_dragon = _add_dragon(packed, "OffscreenDragon", offscreen_transform)
	primary_mesh = _find_mesh_instance(primary_dragon)
	moving_mesh = _find_mesh_instance(moving_dragon)
	if moving_mesh != null:
		moving_mesh.material_override = dragon_masked_material
	if emissive and primary_mesh != null:
		primary_mesh.material_override = dragon_emissive_material
	dragons_loaded = true
	_rebuild_multimesh()
	print("MICROGEOMETRY_INSTANCES loaded=true cache=replace")

func _add_dragon(packed: PackedScene, node_name: String, node_transform: Transform3D) -> Node3D:
	var instance := packed.instantiate() as Node3D
	instance.name = node_name
	instance.transform = node_transform
	geometry.add_child(instance)
	return instance

func _mesh_statistics(mesh: ArrayMesh) -> Dictionary:
	var vertices := 0
	var triangles := 0
	for surface in mesh.get_surface_count():
		var surface_vertices := mesh.surface_get_array_len(surface)
		var surface_indices := mesh.surface_get_array_index_len(surface)
		vertices += surface_vertices
		triangles += (surface_indices if surface_indices > 0 else surface_vertices) / 3
	var micro_statistics := {}
	if mesh.micro_geometry != null:
		micro_statistics = mesh.micro_geometry.get_statistics()
	return { "surfaces": mesh.get_surface_count(), "vertices": vertices, "triangles": triangles, "micro_geometry": micro_statistics }

func _save_native(path: String) -> void:
	if not path.begins_with("res://") or path.get_extension().to_lower() != "res":
		push_error("Native mesh path must be a res:// .res path: " + path)
		return
	var mesh := primary_mesh.mesh as ArrayMesh if primary_mesh != null else null
	if mesh == null:
		push_error("No imported ArrayMesh is available for native save/load.")
		return
	var save_result := ResourceSaver.save(mesh, path)
	print("MICROGEOMETRY_NATIVE_SAVE path=", path, " result=", save_result, " statistics=", _mesh_statistics(mesh))
	if save_result != OK:
		return
	var loaded := ResourceLoader.load(path, "ArrayMesh", ResourceLoader.CACHE_MODE_REPLACE) as ArrayMesh
	if loaded == null:
		push_error("Could not reload native ArrayMesh from " + path)
		return
	primary_mesh.mesh = loaded
	_rebuild_multimesh()
	print("MICROGEOMETRY_NATIVE_LOAD path=", path, " statistics=", _mesh_statistics(loaded))

func _cycle_rt_error() -> void:
	rt_error_index = (rt_error_index + 1) % RT_ERRORS.size()
	environment.raytracing_geometry_error = RT_ERRORS[rt_error_index]
	print("MICROGEOMETRY_RT_ERROR value=", environment.raytracing_geometry_error)

func _cycle_offscreen_multiplier() -> void:
	offscreen_multiplier_index = (offscreen_multiplier_index + 1) % OFFSCREEN_MULTIPLIERS.size()
	environment.raytracing_geometry_offscreen_multiplier = OFFSCREEN_MULTIPLIERS[offscreen_multiplier_index]
	print("MICROGEOMETRY_OFFSCREEN_MULTIPLIER value=", environment.raytracing_geometry_offscreen_multiplier)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_F1:
				_set_debug_draw(Viewport.DEBUG_DRAW_DISABLED)
			KEY_F2:
				_set_debug_draw(Viewport.DEBUG_DRAW_MICRO_GEOMETRY_RASTER)
			KEY_F3:
				_set_debug_draw(Viewport.DEBUG_DRAW_MICRO_GEOMETRY_RT)
			KEY_F4:
				_set_frozen(not frozen)
			KEY_SPACE:
				animate = not animate
			KEY_M:
				multimesh_instance.visible = not multimesh_instance.visible
			KEY_N:
				_save_native(DEFAULT_NATIVE_PATH)
			KEY_V:
				_set_secondary_visible(not secondary_container.visible)
			KEY_I:
				_toggle_emissive()
			KEY_O:
				if is_instance_valid(offscreen_dragon):
					offscreen_dragon.visible = not offscreen_dragon.visible
			KEY_G:
				_cycle_rt_error()
			KEY_H:
				_cycle_offscreen_multiplier()
			KEY_P:
				camera.far = 18.0 if camera.far > 18.0 else 200.0
			KEY_U:
				_toggle_dragons()
			KEY_F12:
				_capture("user://microgeometry-%d.png" % Time.get_unix_time_from_system(), false)
			KEY_ESCAPE:
				get_tree().quit()
	if event is InputEventMouseMotion and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		camera.rotate_y(-event.relative.x * 0.003)
		camera.rotate_object_local(Vector3.RIGHT, -event.relative.y * 0.003)

func _process(delta: float) -> void:
	elapsed += delta
	if freeze_after >= 0.0 and not freeze_after_triggered and elapsed >= freeze_after:
		freeze_after_triggered = true
		_set_frozen(true)
	if animate and is_instance_valid(moving_dragon):
		moving_dragon.position = Vector3(-4.5 + sin(elapsed) * 2.0, moving_transform.origin.y, 2.0)
		moving_dragon.rotation.y = elapsed * 0.5
	if animate and multimesh_instance.multimesh != null:
		var transform := Transform3D(Basis(Vector3.UP, elapsed).scaled(Vector3.ONE * 0.007), Vector3(-3.0, 0.39209 + sin(elapsed * 1.7) * 0.15, -4.5))
		multimesh_instance.multimesh.set_instance_transform(0, transform)
	if animate and deformer != null:
		deformer.set_blend_shape_value(0, sin(elapsed * 1.3) * 0.5 + 0.5)
	if orbit:
		camera.position = Vector3(sin(elapsed * 0.25) * 8.0, 2.8, cos(elapsed * 0.25) * 8.0)
		camera.look_at(Vector3(0.0, 1.2, 0.0))
	else:
		var direction := Vector3(float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)), float(Input.is_physical_key_pressed(KEY_E)) - float(Input.is_physical_key_pressed(KEY_Q)), float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
		camera.position += camera.basis * direction * delta * 6.0
	_update_hud()
	if not capture_path.is_empty() and elapsed >= capture_delay and not capturing:
		capturing = true
		_capture(capture_path, true)

func _update_hud() -> void:
	var viewport := get_viewport()
	var raster_clusters := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_RASTER_CLUSTERS)
	var raster_triangles := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_RASTER_TRIANGLES)
	var rt_clusters := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_RT_CLUSTERS)
	var rt_triangles := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_RT_TRIANGLES)
	var resident_pages := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_RESIDENT_PAGES)
	var pending_pages := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_PENDING_PAGES)
	var pool_kib := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_PAGE_POOL_KIB)
	var geometry_kib := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_GEOMETRY_MEMORY_KIB)
	var as_kib := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_AS_MEMORY_KIB)
	var clas_builds := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_CLAS_BUILDS)
	var blas_builds := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_BLAS_BUILDS)
	var pressure := viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_MICRO_GEOMETRY_RESIDENCY_PRESSURE)
	var debug_name := "normal"
	if debug_mode == Viewport.DEBUG_DRAW_MICRO_GEOMETRY_RASTER:
		debug_name = "raster clusters"
	elif debug_mode == Viewport.DEBUG_DRAW_MICRO_GEOMETRY_RT:
		debug_name = "RT clusters"
	hud.text = "Microgeometry dragon | %s | freeze %s | motion %s | orbit %s\nRaster %d clusters / %d triangles | RT %d clusters / %d triangles\nPages %d resident / %d pending | pool %d KiB | geometry %d KiB | AS %d KiB\nCLAS built (total) %d / BLAS builds %d | pressure %d | GPU %.2f ms\nRT error %.1f px | offscreen x%.1f | camera far %.0f\nF1 normal  F2 raster  F3 RT  F4 freeze  Space motion  M MultiMesh\nI emissive  O offscreen  G RT error  H offscreen error  P far plane\nV second view  U unload/reload instances  N native round-trip  F12 capture\nRMB + WASD/QE move" % [debug_name, frozen, animate, orbit, raster_clusters, raster_triangles, rt_clusters, rt_triangles, resident_pages, pending_pages, pool_kib, geometry_kib, as_kib, clas_builds, blas_builds, pressure, RenderingServer.viewport_get_measured_render_time_gpu(viewport.get_viewport_rid()), environment.raytracing_geometry_error, environment.raytracing_geometry_offscreen_multiplier, camera.far]

func _capture(path: String, quit_after: bool) -> void:
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	print(hud.text)
	var result := image.save_png(path)
	print("MICROGEOMETRY_CAPTURE path=", ProjectSettings.globalize_path(path), " result=", result, " frame=", Engine.get_frames_drawn())
	if quit_after:
		get_tree().quit()
