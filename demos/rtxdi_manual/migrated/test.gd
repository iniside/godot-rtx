extends Node3D

@onready var environment: Environment = $WorldEnvironment.environment
@onready var glowing_box: Node3D = $Geometry/GlowingBox
@onready var lights: Node3D = $Lights
@onready var fps: Label = $HUD/FPS
@onready var state: Label = $HUD/State

var elapsed := 0.0
var animate := true
var capture_path := ""
var capture_delay := 8.0
var capturing := false

func _ready() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--capture="):
			capture_path = argument.trim_prefix("--capture=")
		elif argument.begins_with("--delay="):
			capture_delay = float(argument.trim_prefix("--delay="))
	print("MIGRATED_GI_DEMO renderer=", RenderingServer.get_current_rendering_method(), " driver=", RenderingServer.get_current_rendering_driver_name(), " map_instances=1 path_tracing=false legacy_gi=false reflection_probe=false")

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

func _process(delta: float) -> void:
	elapsed += delta
	if animate:
		glowing_box.position.y = -0.108265 + sin(elapsed * 1.2) * 0.6
		glowing_box.rotation.y = sin(elapsed * 0.4) * 0.25
	state.text = "RTXDI + NRD  |  one zdm2 map  |  direct light + emission\nSpace: emissive cube motion  B: shadows  F: fog  Esc/F10: mouse capture\nWASD or arrows: move  Q/E: down/up  |  motion %s" % animate
	if not capture_path.is_empty() and elapsed >= capture_delay and not capturing:
		capturing = true
		_capture(capture_path)

func _physics_process(_delta: float) -> void:
	var frame_rate := maxf(Engine.get_frames_per_second(), 0.001)
	fps.text = "%d FPS (%.2f mspf)" % [frame_rate, 1000.0 / frame_rate]

func _capture(path: String) -> void:
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	var result := image.save_png(path)
	print("MIGRATED_CAPTURE path=", ProjectSettings.globalize_path(path), " result=", result, " frame=", Engine.get_frames_drawn())
	get_tree().quit()
