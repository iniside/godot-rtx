extends Node3D

## Renders the path traced scene to PNGs and quits, so the result can be
## inspected without a human at the window.

const WARMUP_FRAMES := 150
const SETTLE_FRAMES := 30

@export var out_dir: String = "user://capture"

var _env: Environment


func _ready() -> void:
	# Label the run from the first user arg (after `--`) so comparison runs do
	# not overwrite each other in the shared user:// directory.
	var user_args := OS.get_cmdline_user_args()
	if user_args.size() > 0:
		out_dir = "user://capture_%s" % user_args[0]
	_env = $WorldEnvironment.environment
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(out_dir))
	_run.call_deferred()


func _run() -> void:
	await _frames(WARMUP_FRAMES)
	_save("00_beauty")

	# Mode 23 is RT_DEBUG_CLUSTER_ID: hashed colour per cluster, black when the
	# surface is not clustered.
	_env.pathtracing_debug_mode = 23
	await _frames(SETTLE_FRAMES)
	_save("01_cluster_id")

	_env.pathtracing_debug_mode = 2
	await _frames(SETTLE_FRAMES)
	_save("02_geometry_normals")

	_env.pathtracing_debug_mode = 7
	await _frames(SETTLE_FRAMES)
	_save("03_uv")

	print("CAPTURE_DONE ", ProjectSettings.globalize_path(out_dir))
	get_tree().quit()


func _frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().process_frame


func _save(p_name: String) -> void:
	var img := get_viewport().get_texture().get_image()
	var path := "%s/%s.png" % [out_dir, p_name]
	var err := img.save_png(path)
	if err != OK:
		printerr("CAPTURE_FAIL ", p_name, " err=", err)
		return
	print("CAPTURE_SAVED ", ProjectSettings.globalize_path(path), " ", img.get_width(), "x", img.get_height())
