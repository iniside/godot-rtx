extends Node3D

const INSTANCES_PER_MESH := 5000
const COLUMNS := 100
const ROWS := 100
const OBJECT_HEIGHT := 2.0
const CELL_MARGIN := 0.25

@export var lucy_scene: PackedScene
@export var thai_scene: PackedScene

@onready var camera: Camera3D = $Camera

var orbit := false
var orbit_angle := 0.0
var camera_radius := 0.0
var hud_sample_started_usec := 0
var hud_frames := 0
var viewport_rid: RID

func _ready() -> void:
	set_process(false)
	var inspect_import := false
	var capture_path := ""
	var capture_delay := 10.0
	for argument in OS.get_cmdline_user_args():
		if argument == "--inspect-import":
			inspect_import = true
		elif argument == "--orbit":
			orbit = true
		elif argument.begins_with("--capture="):
			capture_path = argument.trim_prefix("--capture=")
		elif argument.begins_with("--delay="):
			capture_delay = maxf(0.0, float(argument.trim_prefix("--delay=")))
	var lucy := _imported_mesh(lucy_scene, "Lucy")
	var thai := _imported_mesh(thai_scene, "Thai")
	if lucy == null or thai == null:
		get_tree().quit(1)
		return
	if inspect_import:
		get_tree().quit()
		return
	var setup_started := Time.get_ticks_usec()
	var lucy_basis := Basis(Vector3.RIGHT, -PI / 2.0)
	var lucy_bounds: AABB = Transform3D(lucy_basis, Vector3.ZERO) * lucy.get_aabb()
	var thai_bounds := thai.get_aabb()
	if lucy_bounds.size.y <= 0.0 or thai_bounds.size.y <= 0.0:
		push_error("MICRO_STRESS: imported mesh has no height")
		get_tree().quit(1)
		return
	lucy_basis = lucy_basis.scaled(Vector3.ONE * OBJECT_HEIGHT / lucy_bounds.size.y)
	var thai_basis := Basis.IDENTITY.scaled(Vector3.ONE * OBJECT_HEIGHT / thai_bounds.size.y)
	lucy_bounds = Transform3D(lucy_basis, Vector3.ZERO) * lucy.get_aabb()
	thai_bounds = Transform3D(thai_basis, Vector3.ZERO) * thai.get_aabb()
	var spacing := Vector2(maxf(lucy_bounds.size.x, thai_bounds.size.x), maxf(lucy_bounds.size.z, thai_bounds.size.z)) + Vector2.ONE * CELL_MARGIN
	var lucy_offset := Vector3(-lucy_bounds.get_center().x, -lucy_bounds.position.y, -lucy_bounds.get_center().z)
	var thai_offset := Vector3(-thai_bounds.get_center().x, -thai_bounds.position.y, -thai_bounds.get_center().z)
	for row in ROWS:
		for column in COLUMNS:
			var is_lucy := (row + column) % 2 == 0
			var instance := MeshInstance3D.new()
			instance.name = "%s_%04d" % ["Lucy" if is_lucy else "Thai", (row * COLUMNS + column) >> 1]
			instance.mesh = lucy if is_lucy else thai
			var origin := Vector3((column - (COLUMNS - 1) * 0.5) * spacing.x, 0.0, (row - (ROWS - 1) * 0.5) * spacing.y)
			instance.transform = Transform3D(lucy_basis if is_lucy else thai_basis, origin + (lucy_offset if is_lucy else thai_offset))
			$Instances.add_child(instance)
	var extent := Vector2(COLUMNS, ROWS) * spacing
	var floor_mesh := $Floor.mesh as BoxMesh
	floor_mesh.size = Vector3(extent.x + 4.0, 0.2, extent.y + 4.0)
	camera_radius = maxf(extent.x, extent.y)
	camera.position = Vector3(0.0, camera_radius * 0.8, camera_radius)
	camera.far = camera_radius * 4.0
	camera.look_at(Vector3(0.0, OBJECT_HEIGHT * 0.5, 0.0))
	var counts := Vector2i.ZERO
	var native_instances := 0
	for child in $Instances.get_children():
		var instance := child as MeshInstance3D
		if instance.mesh == lucy:
			counts.x += 1
		elif instance.mesh == thai:
			counts.y += 1
		if instance.get_instance().is_valid():
			native_instances += 1
	print("MICRO_STRESS_POPULATED lucy=", counts.x, " thai=", counts.y, " native_instance_rids=", native_instances, " shared_mesh_resources=2 setup_ms=", (Time.get_ticks_usec() - setup_started) / 1000.0, " cell_spacing=", spacing, " field_extent=", extent)
	if counts != Vector2i(INSTANCES_PER_MESH, INSTANCES_PER_MESH) or native_instances != INSTANCES_PER_MESH * 2:
		push_error("MICRO_STRESS: incomplete instance population")
		get_tree().quit(1)
		return
	$HUD/State.text = "Lucy: %d | Thai: %d | Native mesh instances: %d\nShared imported DAGs | %s camera | 2 m object height" % [counts.x, counts.y, native_instances, "orbiting" if orbit else "still"]
	print("MICRO_STRESS_RENDERER method=", RenderingServer.get_current_rendering_method(), " driver=", RenderingServer.get_current_rendering_driver_name(), " orbit=", orbit)
	viewport_rid = get_viewport().get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(viewport_rid, true)
	hud_sample_started_usec = Time.get_ticks_usec()
	set_process(true)
	if not capture_path.is_empty():
		_capture(capture_path, capture_delay)

func _imported_mesh(packed: PackedScene, label: String) -> ArrayMesh:
	if packed == null:
		push_error("MICRO_STRESS: missing imported scene " + label)
		return null
	var source := packed.instantiate()
	var meshes := source.find_children("*", "MeshInstance3D", true, false)
	if source is MeshInstance3D:
		meshes.append(source)
	if meshes.size() != 1:
		push_error("MICRO_STRESS: expected one mesh in " + label)
		source.free()
		return null
	var mesh := (meshes[0] as MeshInstance3D).mesh as ArrayMesh
	source.free()
	if mesh == null or mesh.micro_geometry == null:
		push_error("MICRO_STRESS: missing imported microgeometry for " + label)
		return null
	var statistics := mesh.micro_geometry.get_statistics()
	print("MICRO_STRESS_IMPORT mesh=", label, " source=", packed.resource_path, " mesh_rid=", mesh.get_rid(), " aabb=", mesh.get_aabb(), " content_id=", mesh.micro_geometry.get_content_id(), " geometry=", mesh.micro_geometry.resource_path, " statistics=", JSON.stringify(statistics))
	if int(statistics.get("levels", 0)) == 0 or int(statistics.get("pages", 0)) == 0:
		push_error("MICRO_STRESS: empty imported DAG or pages for " + label)
		return null
	return mesh

func _process(delta: float) -> void:
	if orbit:
		orbit_angle += delta * 0.08
		camera.position = Vector3(sin(orbit_angle) * camera_radius, camera_radius * 0.8, cos(orbit_angle) * camera_radius)
		camera.look_at(Vector3(0.0, OBJECT_HEIGHT * 0.5, 0.0))
	hud_frames += 1
	var now := Time.get_ticks_usec()
	var elapsed := now - hud_sample_started_usec
	if elapsed >= 250000:
		var fps := hud_frames * 1000000.0 / elapsed
		var frame_ms := elapsed / (hud_frames * 1000.0)
		var cpu_ms := RenderingServer.viewport_get_measured_render_time_cpu(viewport_rid)
		var gpu_ms := RenderingServer.viewport_get_measured_render_time_gpu(viewport_rid)
		$HUD/Performance.text = "FPS: %.1f | Frame: %.2f ms\nCPU render (incl. waits): %.2f ms | GPU: %.2f ms" % [fps, frame_ms, cpu_ms, gpu_ms]
		hud_frames = 0
		hud_sample_started_usec = now

func _capture(path: String, delay: float) -> void:
	await get_tree().create_timer(delay).timeout
	await RenderingServer.frame_post_draw
	var result := get_viewport().get_texture().get_image().save_png(path)
	print("MICRO_STRESS_CAPTURE path=", path, " result=", result)
