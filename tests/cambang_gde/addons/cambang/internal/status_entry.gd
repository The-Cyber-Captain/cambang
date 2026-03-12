@tool
extends MarginContainer

const INDENT_WIDTH := 16.0

var _indent_spacer: Control
var _disclosure_label: Label
var _name_label: Label
var _state_segment: HBoxContainer
var _counter_segment: HBoxContainer
var _info_lines_container: VBoxContainer


func _ready() -> void:
	_bind_nodes()


func set_model(model: CamBANGStatusPanel.StatusEntryModel) -> void:
	_bind_nodes()
	if model == null:
		visible = false
		return
	visible = true

	_indent_spacer.custom_minimum_size = Vector2(max(model.depth, 0) * INDENT_WIDTH, 0)

	if model.can_expand:
		_disclosure_label.text = "▾" if model.expanded else "▸"
	else:
		_disclosure_label.text = "•"

	_name_label.text = model.label
	_render_badges(model.badges)
	_render_counters(model.counters)
	_render_info_lines(model.info_lines)


func _render_badges(badges: Array[CamBANGStatusPanel.BadgeModel]) -> void:
	for child in _state_segment.get_children():
		child.queue_free()

	for badge in badges:
		var pair := HBoxContainer.new()
		pair.add_theme_constant_override("separation", 4)

		var indicator := ColorRect.new()
		indicator.custom_minimum_size = Vector2(8, 8)
		indicator.color = _badge_color_for_role(badge.role)
		pair.add_child(indicator)

		var label := Label.new()
		label.text = badge.label
		pair.add_child(label)

		_state_segment.add_child(pair)


func _render_counters(counters: Array[CamBANGStatusPanel.CounterModel]) -> void:
	for child in _counter_segment.get_children():
		child.queue_free()

	for counter in counters:
		var counter_box := VBoxContainer.new()
		counter_box.add_theme_constant_override("separation", 2)

		var name_label := Label.new()
		name_label.text = counter.name
		name_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		counter_box.add_child(name_label)

		var value_panel := PanelContainer.new()
		value_panel.add_theme_stylebox_override("panel", _counter_value_style())
		counter_box.add_child(value_panel)

		var value_label := Label.new()
		value_label.text = _format_counter_value(counter.value, counter.digits)
		value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		value_label.custom_minimum_size = Vector2(max(counter.digits, 1) * 10.0 + 12.0, 0)
		value_panel.add_child(value_label)

		_counter_segment.add_child(counter_box)


func _render_info_lines(info_lines: Array[String]) -> void:
	for child in _info_lines_container.get_children():
		child.queue_free()

	for line in info_lines:
		var info := Label.new()
		info.text = "• %s" % line
		info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_info_lines_container.add_child(info)

	_info_lines_container.visible = not info_lines.is_empty()


func _format_counter_value(value: int, digits: int) -> String:
	var bounded_digits := maxi(digits, 1)
	var raw := str(value)
	if raw.length() <= bounded_digits:
		return raw
	if bounded_digits <= 2:
		return "%s+" % ("9" if bounded_digits == 1 else "99")

	var kilo := int(value / 1000)
	var kilo_text := str(kilo)
	var max_kilo_digits := bounded_digits - 1
	if kilo_text.length() > max_kilo_digits:
		kilo_text = ""
		for _i in range(max_kilo_digits):
			kilo_text += "9"
	return "%sk+" % kilo_text


func _badge_color_for_role(role: String) -> Color:
	match role:
		"success":
			return Color(0.36, 0.78, 0.39, 1.0)
		"warning":
			return Color(0.98, 0.74, 0.18, 1.0)
		"error":
			return Color(0.90, 0.32, 0.30, 1.0)
		"info":
			return Color(0.27, 0.64, 0.90, 1.0)
		_:
			return Color(0.65, 0.65, 0.65, 1.0)


func _counter_value_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.16, 0.16, 0.18, 0.85)
	style.corner_radius_top_left = 3
	style.corner_radius_top_right = 3
	style.corner_radius_bottom_right = 3
	style.corner_radius_bottom_left = 3
	style.content_margin_left = 4
	style.content_margin_right = 4
	style.content_margin_top = 2
	style.content_margin_bottom = 2
	return style


func _bind_nodes() -> void:
	if _indent_spacer != null:
		return
	_indent_spacer = $MainRow/IdentitySegment/Indent
	_disclosure_label = $MainRow/IdentitySegment/Disclosure
	_name_label = $MainRow/IdentitySegment/NameLabel
	_state_segment = $MainRow/InfoPanel/InfoContent/StateSegment
	_counter_segment = $MainRow/InfoPanel/InfoContent/CounterSegment
	_info_lines_container = $InfoLines
