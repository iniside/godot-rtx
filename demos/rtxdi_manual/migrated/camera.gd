extends Camera3D

const MOUSE_SENSITIVITY := 0.002
const MOVE_SPEED := 6.0

var rotation_input := Vector3()
var velocity := Vector3()

func _ready() -> void:
	rotation_input = rotation
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func _input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		rotation_input.y -= event.screen_relative.x * MOUSE_SENSITIVITY
		rotation_input.x = clamp(rotation_input.x - event.screen_relative.y * MOUSE_SENSITIVITY, -1.57, 1.57)
		transform.basis = Basis.from_euler(rotation_input)
	if event is InputEventKey and event.pressed and not event.echo and event.keycode in [KEY_ESCAPE, KEY_F10]:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED)

func _process(delta: float) -> void:
	var motion := Vector3(
		float(Input.is_physical_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT)) - float(Input.is_physical_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT)),
		float(Input.is_physical_key_pressed(KEY_E)) - float(Input.is_physical_key_pressed(KEY_Q)),
		float(Input.is_physical_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN)) - float(Input.is_physical_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP))
	).normalized()
	velocity += MOVE_SPEED * delta * (transform.basis * motion)
	velocity *= 0.85
	position += velocity
