@tool
extends MarginContainer

signal disclosure_toggled(entry_id: String, expanded: bool)

const INDENT_WIDTH := 14.0
const DISCLOSURE_WIDTH := 28.0

var _entry_id: String = ""
var _indent_region: Control
var _disclosure_button: Button
var _disclosure_placeholder: Control
var _disclosure_indicator: Control
var _name_label: Label
var _state_segment: HBoxContainer
var _counter_segment: HBoxContainer
var _info_lines_container: VBoxContainer
var _info_panel: PanelContainer
var _row_shell: PanelContainer

var _badge_pairs: Array[HBoxContainer] = []
var _counter_widgets: Array[VBoxContainer] = []
var _info_line_rows: Array[HBoxContainer] = []


func _ready() -> void:
	_bind_nodes()


func set_model(model: CamBANGStatusPanel.StatusEntryModel) -> void:
	_bind_nodes()
	if model == null:
		visible = false
		return
	visible = true

	_entry_id = model.id
	_indent_region.custom_minimum_size = Vector2(max(model.depth, 0) * INDENT_WIDTH, 0)
	_name_label.text = model.label

	_disclosure_button.visible = model.can_expand
	_disclosure_placeholder.visible = not model.can_expand
	_disclosure_button.button_pressed = model.expanded
	_disclosure_indicator.set_expanded(model.expanded)

	_render_badges(model.badges)
	_render_counters(model.counters)
	_render_info_lines(model.depth, model.info_lines)


func _render_badges(badges: Array[CamBANGStatusPanel.BadgeModel]) -> void:
	for i in range(badges.size()):
		var pair := _ensure_badge_pair(i)
		pair.visible = true
		var indicator := pair.get_child(0) as ColorRect
		var label := pair.get_child(1) as Label
		indicator.color = _badge_color_for_role(badges[i].role)
		label.text = badges[i].label

	for i in range(badges.size(), _badge_pairs.size()):
		_badge_pairs[i].visible = false


func _ensure_badge_pair(index: int) -> HBoxContainer:
	if index < _badge_pairs.size():
		return _badge_pairs[index]

	var pair := HBoxContainer.new()
	pair.add_theme_constant_override("separation", 3)

	var indicator := ColorRect.new()
	indicator.custom_minimum_size = Vector2(7, 7)
	pair.add_child(indicator)

	var label := Label.new()
	pair.add_child(label)

	_state_segment.add_child(pair)
	_badge_pairs.append(pair)
	return pair


func _render_counters(counters: Array[CamBANGStatusPanel.CounterModel]) -> void:
	for i in range(counters.size()):
		var widget := _ensure_counter_widget(i)
		widget.visible = true
		var name_label := widget.get_child(0) as Label
		var value_box := widget.get_child(1) as PanelContainer
		var value_label := value_box.get_child(0) as Label

		name_label.text = counters[i].name
		value_label.text = _format_counter_value(counters[i].value, counters[i].digits)
		value_label.custom_minimum_size = Vector2(max(counters[i].digits, 1) * 10.0 + 8.0, 0)

	for i in range(counters.size(), _counter_widgets.size()):
		_counter_widgets[i].visible = false


func _ensure_counter_widget(index: int) -> VBoxContainer:
	if index < _counter_widgets.size():
		return _counter_widgets[index]

	var counter_widget := VBoxContainer.new()
	counter_widget.add_theme_constant_override("separation", 1)

	var name_label := Label.new()
	name_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	counter_widget.add_child(name_label)

	var value_box := PanelContainer.new()
	value_box.add_theme_stylebox_override("panel", _counter_value_style())
	counter_widget.add_child(value_box)

	var value_label := Label.new()
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	value_box.add_child(value_label)

	_counter_segment.add_child(counter_widget)
	_counter_widgets.append(counter_widget)
	return counter_widget


func _render_info_lines(depth: int, info_lines: Array[String]) -> void:
	for i in range(info_lines.size()):
		var row := _ensure_info_line_row(i)
		row.visible = true
		var spacer := row.get_child(0) as Control
		var info := row.get_child(1) as Label
		spacer.custom_minimum_size = Vector2(max(depth, 0) * INDENT_WIDTH + DISCLOSURE_WIDTH + 6.0, 0)
		info.text = "• %s" % info_lines[i]

	for i in range(info_lines.size(), _info_line_rows.size()):
		_info_line_rows[i].visible = false

	_info_lines_container.visible = not info_lines.is_empty()


func _ensure_info_line_row(index: int) -> HBoxContainer:
	if index < _info_line_rows.size():
		return _info_line_rows[index]

	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 4)

	var spacer := Control.new()
	row.add_child(spacer)

	var info := Label.new()
	info.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	row.add_child(info)

	_info_lines_container.add_child(row)
	_info_line_rows.append(row)
	return row


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
	style.corner_radius_top_left = 2
	style.corner_radius_top_right = 2
	style.corner_radius_bottom_right = 2
	style.corner_radius_bottom_left = 2
	style.content_margin_left = 3
	style.content_margin_right = 3
	style.content_margin_top = 1
	style.content_margin_bottom = 1
	return style


func _info_panel_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.12, 0.12, 0.14, 0.55)
	style.corner_radius_top_left = 3
	style.corner_radius_top_right = 3
	style.corner_radius_bottom_right = 3
	style.corner_radius_bottom_left = 3
	return style


func _row_shell_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.11, 0.11, 0.13, 0.4)
	style.corner_radius_top_left = 5
	style.corner_radius_top_right = 5
	style.corner_radius_bottom_right = 5
	style.corner_radius_bottom_left = 5
	style.content_margin_left = 3
	style.content_margin_right = 3
	style.content_margin_top = 2
	style.content_margin_bottom = 2
	return style


func _on_disclosure_pressed() -> void:
	_disclosure_indicator.set_expanded(_disclosure_button.button_pressed)
	disclosure_toggled.emit(_entry_id, _disclosure_button.button_pressed)


func _bind_nodes() -> void:
	if _indent_region != null:
		return
	_indent_region = $StatusEntryRoot/MainRow/IndentRegion
	_disclosure_button = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot/DisclosureButton
	_disclosure_placeholder = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot/DisclosurePlaceholder
	_disclosure_indicator = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot/DisclosureButton/DisclosureIndicator
	_name_label = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/NameLabel
	_state_segment = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel/InfoMargin/InfoInner/StateSegment
	_counter_segment = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel/InfoMargin/InfoInner/CounterSegment
	_info_lines_container = $StatusEntryRoot/InfoLines
	_info_panel = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel
	_row_shell = $StatusEntryRoot/MainRow/EntryShell
	_disclosure_indicator.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_disclosure_placeholder.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_info_panel.add_theme_stylebox_override("panel", _info_panel_style())
	_row_shell.add_theme_stylebox_override("panel", _row_shell_style())
	if not _disclosure_button.pressed.is_connected(_on_disclosure_pressed):
		_disclosure_button.pressed.connect(_on_disclosure_pressed)
