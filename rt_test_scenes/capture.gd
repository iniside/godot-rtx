extends Node3D

## Compares the beauty pass with the denoiser on and off, at increasing sample
## counts, to separate "no light transport" from "denoiser eats the image".

const VIEW_POS := Vector3(-6.0, 2.0, 3.5)
const VIEW_TARGET := Vector3(8.0, 3.0, -1.0)

var out_dir: String = "user://capture"

var _env: Environment
var _cam: Camera3D


func _ready() -> void:
	var user_args := OS.get_cmdline_user_args()
	if user_args.size() > 0:
		out_dir = "user://capture_%s" % user_args[0]
	_env = $WorldEnvironment.environment
	_cam = $Camera
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(out_dir))
	_run.call_deferred()


func _run() -> void:
	_cam.global_position = VIEW_POS
	_cam.look_at(VIEW_TARGET, Vector3.UP)
	_env.pathtracing_debug_mode = 0

	_env.pathtracing_denoiser = 0
	_env.pathtracing_samples_per_pixel = 4

	var sun: DirectionalLight3D = $Sun
	for energy in [3.0, 100.0]:
		sun.light_energy = energy
		await _frames(200)
		_save("sun%d" % int(energy))
	sun.light_energy = 3.0

	sun.visible = false
	await _frames(200)
	_save("no_sun")
	sun.visible = true

	# Flat bright ambient, independent of the sky and of any light.
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(1.0, 1.0, 1.0)
	_env.ambient_light_energy = 5.0
	await _frames(200)
	_save("ambient_color")

	print("CAPTURE_DONE ", ProjectSettings.globalize_path(out_dir))
	get_tree().quit()


func _frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().process_frame


func _save(p_name: String) -> void:
	var img := get_viewport().get_texture().get_image()
	var err := img.save_png("%s/%s.png" % [out_dir, p_name])
	if err != OK:
		printerr("CAPTURE_FAIL ", p_name, " err=", err)
		return
	var luma := 0.0
	var peak := 0.0
	for y in range(0, img.get_height(), 8):
		for x in range(0, img.get_width(), 8):
			var l := img.get_pixel(x, y).get_luminance()
			luma += l
			peak = maxf(peak, l)
	print("CAPTURE_SAVED %-14s mean_luma=%.5f peak=%.3f" % [p_name, luma / (90.0 * 160.0), peak])
