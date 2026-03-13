@tool
extends Button

const TOUCH_SLOP := 10.0

var _touch_id := -1
var _touch_start := Vector2.ZERO
var _touch_armed := false


func _gui_input(event: InputEvent) -> void:
	if event is InputEventScreenTouch:
		_handle_screen_touch(event)
		return
	if event is InputEventScreenDrag:
		_handle_screen_drag(event)
		return


func _handle_screen_touch(event: InputEventScreenTouch) -> void:
	if event.pressed:
		if _touch_id != -1:
			return
		if not get_global_rect().has_point(event.position):
			return
		_touch_id = event.index
		_touch_start = event.position
		_touch_armed = true
		button_pressed = true
		accept_event()
		return

	if event.index != _touch_id:
		return
	var inside := get_global_rect().has_point(event.position)
	var should_toggle := _touch_armed and inside
	_touch_id = -1
	_touch_armed = false
	if should_toggle:
		button_pressed = not button_pressed
		emit_signal("pressed")
	else:
		button_pressed = false
	accept_event()


func _handle_screen_drag(event: InputEventScreenDrag) -> void:
	if event.index != _touch_id:
		return
	if event.position.distance_to(_touch_start) > TOUCH_SLOP:
		_touch_armed = false
	button_pressed = _touch_armed
	accept_event()
