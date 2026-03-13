@tool
extends ScrollContainer

const TOUCH_SLOP := 6.0
const TOUCH_SCROLLBAR_SIZE := 24.0

var _touch_id := -1
var _touch_start := Vector2.ZERO
var _touch_last := Vector2.ZERO
var _touch_mode := ""
var _v_bar_start_value := 0.0
var _h_bar_start_value := 0.0


func _ready() -> void:
	_update_touch_targets()


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		_update_touch_targets()


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
		_touch_id = event.index
		_touch_start = event.position
		_touch_last = event.position
		_touch_mode = _pick_touch_mode(event.position)
		_v_bar_start_value = float(scroll_vertical)
		_h_bar_start_value = float(scroll_horizontal)
		accept_event()
		return

	if event.index != _touch_id:
		return
	_touch_id = -1
	_touch_mode = ""
	accept_event()


func _handle_screen_drag(event: InputEventScreenDrag) -> void:
	if event.index != _touch_id:
		return
	if _touch_mode == "content":
		if event.position.distance_to(_touch_start) >= TOUCH_SLOP:
			var delta := event.position - _touch_last
			scroll_vertical = maxi(0, int(scroll_vertical - delta.y))
			scroll_horizontal = maxi(0, int(scroll_horizontal - delta.x))
			_touch_last = event.position
			accept_event()
	elif _touch_mode == "vbar":
		var v_bar := get_v_scroll_bar()
		var bar_len := maxf(v_bar.size.y, 1.0)
		var range := maxf(v_bar.max_value - v_bar.min_value, 1.0)
		var delta_ratio := (event.position.y - _touch_start.y) / bar_len
		scroll_vertical = int(clampf(_v_bar_start_value + range * delta_ratio, v_bar.min_value, v_bar.max_value))
		accept_event()
	elif _touch_mode == "hbar":
		var h_bar := get_h_scroll_bar()
		var bar_len := maxf(h_bar.size.x, 1.0)
		var range := maxf(h_bar.max_value - h_bar.min_value, 1.0)
		var delta_ratio := (event.position.x - _touch_start.x) / bar_len
		scroll_horizontal = int(clampf(_h_bar_start_value + range * delta_ratio, h_bar.min_value, h_bar.max_value))
		accept_event()


func _pick_touch_mode(screen_pos: Vector2) -> String:
	var v_bar := get_v_scroll_bar()
	if _expanded_bar_rect(v_bar, true).has_point(screen_pos):
		return "vbar"
	var h_bar := get_h_scroll_bar()
	if _expanded_bar_rect(h_bar, false).has_point(screen_pos):
		return "hbar"
	return "content"


func _expanded_bar_rect(bar: Range, vertical: bool) -> Rect2:
	var control := bar as Control
	if control == null:
		return Rect2()
	var rect := control.get_global_rect()
	if vertical:
		rect.position.x -= TOUCH_SCROLLBAR_SIZE * 0.5
		rect.size.x += TOUCH_SCROLLBAR_SIZE
	else:
		rect.position.y -= TOUCH_SCROLLBAR_SIZE * 0.5
		rect.size.y += TOUCH_SCROLLBAR_SIZE
	return rect


func _update_touch_targets() -> void:
	if not DisplayServer.is_touchscreen_available():
		return
	var v_bar := get_v_scroll_bar()
	var h_bar := get_h_scroll_bar()
	if v_bar != null:
		v_bar.custom_minimum_size.x = maxf(v_bar.custom_minimum_size.x, TOUCH_SCROLLBAR_SIZE)
	if h_bar != null:
		h_bar.custom_minimum_size.y = maxf(h_bar.custom_minimum_size.y, TOUCH_SCROLLBAR_SIZE)
