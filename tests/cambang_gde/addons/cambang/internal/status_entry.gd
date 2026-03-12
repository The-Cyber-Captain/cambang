@tool
extends MarginContainer

var _indent_spacer: Control
var _disclosure_label: Label
var _name_label: Label
var _badges_container: HBoxContainer
var _counters_container: HBoxContainer
var _info_lines_container: VBoxContainer


func _ready() -> void:
	_bind_nodes()


func set_model(model: CamBANGStatusPanel.StatusEntryModel) -> void:
	_bind_nodes()
	if model == null:
		visible = false
		return
	visible = true

	_indent_spacer.custom_minimum_size = Vector2(max(model.depth, 0) * 18.0, 0)

	if model.can_expand:
		_disclosure_label.text = "▾" if model.expanded else "▸"
	else:
		_disclosure_label.text = "•"

	_name_label.text = model.label

	for child in _badges_container.get_children():
		child.queue_free()
	for badge_model in model.badges:
		_badges_container.add_child(_build_pill("[%s]" % badge_model.text, badge_model.theme))

	for child in _counters_container.get_children():
		child.queue_free()
	for counter_model in model.counters:
		_counters_container.add_child(_build_pill("%s=%s" % [counter_model.name, counter_model.value], counter_model.theme))

	for child in _info_lines_container.get_children():
		child.queue_free()
	for line in model.info_lines:
		var info := Label.new()
		info.text = "• %s" % line
		info.modulate = Color(0.8, 0.8, 0.9, 1.0)
		info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_info_lines_container.add_child(info)

	_info_lines_container.visible = not model.info_lines.is_empty()


func _build_pill(text_value: String, theme_name: String) -> Label:
	var pill := Label.new()
	pill.text = text_value
	match theme_name:
		"ok":
			pill.modulate = Color(0.7, 1.0, 0.7, 1.0)
		"warn":
			pill.modulate = Color(1.0, 0.85, 0.5, 1.0)
		"error":
			pill.modulate = Color(1.0, 0.6, 0.6, 1.0)
		_:
			pill.modulate = Color(0.85, 0.9, 1.0, 1.0)
	return pill


func _bind_nodes() -> void:
	if _indent_spacer != null:
		return
	_indent_spacer = $Root/MainRow/Indent
	_disclosure_label = $Root/MainRow/Disclosure
	_name_label = $Root/MainRow/Name
	_badges_container = $Root/MainRow/Badges
	_counters_container = $Root/MainRow/Counters
	_info_lines_container = $Root/InfoLines
