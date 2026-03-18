@tool
extends MarginContainer

signal disclosure_toggled(entry_id: String, expanded: bool)

const INDENT_WIDTH := 14.0

var _entry_id: String = ""
var _style: CamBANGStatusPanel.StatusPanelStyle

var _indent_region: Control
var _disclosure_slot: Control
var _disclosure_button: Button
var _disclosure_placeholder: Control
var _disclosure_indicator: Control
var _name_label: Label
var _row_content: HBoxContainer
var _identity_segment: HBoxContainer
var _state_segment: HBoxContainer
var _counter_segment: HBoxContainer
var _info_lines_container: VBoxContainer
var _info_panel_inset: MarginContainer
var _info_margin: MarginContainer
var _info_panel: PanelContainer
var _row_shell: PanelContainer
var _accent_bar: ColorRect

var _badge_pairs: Array[HBoxContainer] = []
var _counter_widgets: Array[VBoxContainer] = []
var _info_line_rows: Array[HBoxContainer] = []


func _ready() -> void:
	_bind_nodes()


func set_style(style: CamBANGStatusPanel.StatusPanelStyle) -> void:
	_bind_nodes()
	_style = style
	_apply_style()


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

	_apply_row_palette(model)
	_render_badges(_badges_for_render(model))
	_render_counters(model.counters, model.expanded)
	_render_info_lines(
		model.depth,
		model.summary_info_lines,
		model.detail_info_lines,
		model.anomaly_info_lines,
		model.expanded
	)
	_apply_stable_row_metrics()


func _apply_style() -> void:
	if _style == null:
		return

	_disclosure_slot.custom_minimum_size = Vector2(_style.disclosure_slot_width, 22)
	_disclosure_indicator.scale = Vector2(_style.disclosure_visual_scale, _style.disclosure_visual_scale)
	_row_content.add_theme_constant_override("separation", _style.identity_info_gap)
	_identity_segment.add_theme_constant_override("separation", 6)
	_state_segment.add_theme_constant_override("separation", 8)
	_counter_segment.add_theme_constant_override("separation", 8)
	_info_lines_container.add_theme_constant_override("separation", 2)

	_info_panel_inset.add_theme_constant_override("margin_left", int(_style.info_panel_outer_inset.x))
	_info_panel_inset.add_theme_constant_override("margin_top", int(_style.info_panel_outer_inset.y))
	_info_panel_inset.add_theme_constant_override("margin_right", int(_style.info_panel_outer_inset.z))
	_info_panel_inset.add_theme_constant_override("margin_bottom", int(_style.info_panel_outer_inset.w))

	_info_margin.add_theme_constant_override("margin_left", int(_style.info_panel_inner_padding.x))
	_info_margin.add_theme_constant_override("margin_top", int(_style.info_panel_inner_padding.y))
	_info_margin.add_theme_constant_override("margin_right", int(_style.info_panel_inner_padding.z))
	_info_margin.add_theme_constant_override("margin_bottom", int(_style.info_panel_inner_padding.w))

	_name_label.label_settings = _identity_label_settings()

	var info_style := StyleBoxFlat.new()
	info_style.bg_color = _style.info_panel_bg
	info_style.corner_radius_top_left = 4
	info_style.corner_radius_top_right = 4
	info_style.corner_radius_bottom_right = 4
	info_style.corner_radius_bottom_left = 4
	info_style.border_width_left = 1
	info_style.border_width_top = 1
	info_style.border_width_right = 1
	info_style.border_width_bottom = 1
	info_style.border_color = Color(1, 1, 1, 0.05)
	_info_panel.add_theme_stylebox_override("panel", info_style)

	var shell_style := StyleBoxFlat.new()
	shell_style.bg_color = _style.row_shell_bg
	shell_style.corner_radius_top_left = _style.row_shell_radius
	shell_style.corner_radius_top_right = _style.row_shell_radius
	shell_style.corner_radius_bottom_right = _style.row_shell_radius
	shell_style.corner_radius_bottom_left = _style.row_shell_radius
	shell_style.content_margin_left = _style.row_shell_padding.x
	shell_style.content_margin_top = _style.row_shell_padding.y
	shell_style.content_margin_right = _style.row_shell_padding.z
	shell_style.content_margin_bottom = _style.row_shell_padding.w
	shell_style.border_width_left = 1
	shell_style.border_width_top = 1
	shell_style.border_width_right = 1
	shell_style.border_width_bottom = 1
	shell_style.border_color = Color(1, 1, 1, 0.06)
	_row_shell.add_theme_stylebox_override("panel", shell_style)


func _render_badges(badges: Array[CamBANGStatusPanel.BadgeModel]) -> void:
	for i in range(badges.size()):
		var pair := _ensure_badge_pair(i)
		pair.visible = true
		var indicator := pair.get_child(0) as ColorRect
		var label := pair.get_child(1) as Label
		pair.custom_minimum_size = Vector2(0, 18)
		pair.alignment = BoxContainer.ALIGNMENT_CENTER
		indicator.color = _badge_color_for_role(_badge_role_for_render(badges[i]))
		var indicator_size := max((_style.badge_strip_width if _style != null else 7), 8)
		indicator.custom_minimum_size = Vector2(indicator_size, indicator_size)
		label.text = _badge_display_label(badges[i].label)
		label.label_settings = _state_label_settings()

	for i in range(badges.size(), _badge_pairs.size()):
		_badge_pairs[i].visible = false

	_state_segment.visible = not badges.is_empty()


func _ensure_badge_pair(index: int) -> HBoxContainer:
	if index < _badge_pairs.size():
		return _badge_pairs[index]

	var pair := HBoxContainer.new()
	pair.add_theme_constant_override("separation", 4)
	pair.custom_minimum_size = Vector2(0, 18)

	var indicator := ColorRect.new()
	indicator.custom_minimum_size = Vector2(7, 7)
	pair.add_child(indicator)

	var label := Label.new()
	label.clip_text = true
	pair.add_child(label)

	_state_segment.add_child(pair)
	_badge_pairs.append(pair)
	return pair


func _render_counters(counters: Array[CamBANGStatusPanel.CounterModel], expanded: bool) -> void:
	var visible_counters: Array[CamBANGStatusPanel.CounterModel] = []
	for counter in counters:
		if counter == null:
			continue
		if counter.visibility == "detail" and not expanded:
			continue
		visible_counters.append(counter)

	for i in range(visible_counters.size()):
		var widget := _ensure_counter_widget(i)
		widget.visible = true
		var name_label := widget.get_child(0) as Label
		var value_box := widget.get_child(1) as PanelContainer
		var value_label := value_box.get_child(0) as Label

		name_label.text = visible_counters[i].name
		name_label.label_settings = _counter_label_settings()
		value_label.text = _format_counter_value(visible_counters[i].value, visible_counters[i].digits)
		value_label.label_settings = _counter_label_settings()
		value_label.custom_minimum_size = Vector2(max(visible_counters[i].digits, 1) * 10.0 + 12.0, 0)

	for i in range(visible_counters.size(), _counter_widgets.size()):
		_counter_widgets[i].visible = false

	_counter_segment.visible = not visible_counters.is_empty()


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


func _render_info_lines(
		depth: int,
		summary_info_lines: Array[String],
		detail_info_lines: Array[String],
		anomaly_info_lines: Array[String],
		expanded: bool
	) -> void:
	var visible_lines: Array[String] = []
	for line in summary_info_lines:
		visible_lines.append(line)
	for line in anomaly_info_lines:
		visible_lines.append(line)
	if expanded:
		for line in detail_info_lines:
			visible_lines.append(line)

	for i in range(visible_lines.size()):
		var row := _ensure_info_line_row(i)
		row.visible = true
		var spacer := row.get_child(0) as Control
		var info := row.get_child(1) as Label
		spacer.custom_minimum_size = Vector2(max(depth, 0) * INDENT_WIDTH + (_style.disclosure_slot_width if _style != null else 28) + 8.0, 0)
		info.text = "• %s" % visible_lines[i]
		info.label_settings = _state_label_settings()
		if _is_anomaly_line(visible_lines[i]):
			info.modulate = Color(1.0, 0.87, 0.72, 1.0)
		elif _is_detail_line(visible_lines[i], detail_info_lines):
			info.modulate = Color(0.80, 0.84, 0.90, 0.94)
		else:
			info.modulate = Color(0.90, 0.93, 0.96, 1.0)

	for i in range(visible_lines.size(), _info_line_rows.size()):
		_info_line_rows[i].visible = false

	_info_lines_container.visible = not visible_lines.is_empty()


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


func _identity_label_settings() -> LabelSettings:
	var settings := LabelSettings.new()
	if _style != null:
		settings.font = _style.identity_font
		settings.font_size = _style.identity_font_size
		settings.font_color = _style.identity_font_color
		settings.outline_color = _style.identity_outline_color
		settings.outline_size = _style.identity_outline_size
	return settings


func _state_label_settings() -> LabelSettings:
	var settings := LabelSettings.new()
	if _style != null:
		settings.font = _style.info_font
		settings.font_size = _style.state_font_size
		settings.font_color = _style.state_font_color
		settings.outline_color = _style.state_outline_color
		settings.outline_size = _style.state_outline_size
	return settings


func _counter_label_settings() -> LabelSettings:
	var settings := LabelSettings.new()
	if _style != null:
		settings.font = _style.counter_font
		settings.font_size = _style.counter_font_size
		settings.font_color = _style.counter_font_color
	return settings


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
	style.bg_color = (_style.counter_box_bg if _style != null else Color(0.16, 0.16, 0.18, 0.85))
	var radius := (_style.counter_box_radius if _style != null else 3)
	style.corner_radius_top_left = radius
	style.corner_radius_top_right = radius
	style.corner_radius_bottom_right = radius
	style.corner_radius_bottom_left = radius
	style.content_margin_left = (_style.counter_box_h_padding if _style != null else 4)
	style.content_margin_right = (_style.counter_box_h_padding if _style != null else 4)
	style.content_margin_top = (_style.counter_box_v_padding if _style != null else 2)
	style.content_margin_bottom = (_style.counter_box_v_padding if _style != null else 2)
	style.border_width_left = (_style.counter_box_border_width if _style != null else 1)
	style.border_width_top = (_style.counter_box_border_width if _style != null else 1)
	style.border_width_right = (_style.counter_box_border_width if _style != null else 1)
	style.border_width_bottom = (_style.counter_box_border_width if _style != null else 1)
	style.border_color = (_style.counter_box_border if _style != null else Color(0.34, 0.38, 0.44, 0.85))
	return style


func _apply_stable_row_metrics() -> void:
	var stable_row_height := _stable_row_height_budget()
	var outer_vertical := _panel_outer_vertical_inset()
	var inner_vertical := _panel_inner_vertical_padding()
	var info_panel_height := max(0.0, stable_row_height - outer_vertical)
	var info_content_height := max(0.0, info_panel_height - inner_vertical)

	_row_content.custom_minimum_size = Vector2(_row_content.custom_minimum_size.x, stable_row_height)
	_identity_segment.custom_minimum_size = Vector2(_identity_segment.custom_minimum_size.x, stable_row_height)
	_info_panel_inset.custom_minimum_size = Vector2(_info_panel_inset.custom_minimum_size.x, stable_row_height)
	_info_panel.custom_minimum_size = Vector2(_info_panel.custom_minimum_size.x, info_panel_height)
	_state_segment.custom_minimum_size = Vector2(_state_segment.custom_minimum_size.x, info_content_height)
	_counter_segment.custom_minimum_size = Vector2(_counter_segment.custom_minimum_size.x, info_content_height)


func _stable_row_height_budget() -> float:
	var identity_line_height := max(
		_font_line_height(
			(_style.identity_font if _style != null else null),
			(_style.identity_font_size if _style != null else 13),
			22.0
		),
		22.0
	)
	var badge_line_height := max(
		_font_line_height(
			(_style.info_font if _style != null else null),
			(_style.state_font_size if _style != null else 11),
			18.0
		),
		18.0
	)
	var counter_label_height := _font_line_height(
		(_style.counter_font if _style != null else null),
		(_style.counter_font_size if _style != null else 11),
		12.0
	)
	var counter_value_height := _font_line_height(
		(_style.counter_font if _style != null else null),
		(_style.counter_font_size if _style != null else 11),
		12.0
	) + float((_style.counter_box_v_padding if _style != null else 2) * 2 + (_style.counter_box_border_width if _style != null else 1) * 2)
	var counter_stack_height := counter_label_height + 1.0 + counter_value_height
	var content_line_height := max(badge_line_height, counter_stack_height)
	return max(identity_line_height, content_line_height + _panel_outer_vertical_inset() + _panel_inner_vertical_padding())


func _panel_outer_vertical_inset() -> float:
	if _style == null:
		return 4.0
	return _style.info_panel_outer_inset.y + _style.info_panel_outer_inset.w


func _panel_inner_vertical_padding() -> float:
	if _style == null:
		return 4.0
	return _style.info_panel_inner_padding.y + _style.info_panel_inner_padding.w


func _font_line_height(font: Font, font_size: int, fallback: float) -> float:
	if font == null:
		return fallback
	return max(float(font.get_height(font_size)), fallback)


func _apply_row_palette(model: CamBANGStatusPanel.StatusEntryModel) -> void:
	var object_class := _object_class(model)
	var accent := _class_color(object_class)
	var shell_bg := (_style.row_shell_bg if _style != null else Color(0.16, 0.18, 0.22, 0.68))
	var info_bg := (_style.info_panel_bg if _style != null else Color(0.10, 0.12, 0.15, 0.88))
	var border := accent.darkened(0.42)

	var shell_style := StyleBoxFlat.new()
	shell_style.bg_color = shell_bg
	shell_style.corner_radius_top_left = (_style.row_shell_radius if _style != null else 5)
	shell_style.corner_radius_top_right = (_style.row_shell_radius if _style != null else 5)
	shell_style.corner_radius_bottom_right = (_style.row_shell_radius if _style != null else 5)
	shell_style.corner_radius_bottom_left = (_style.row_shell_radius if _style != null else 5)
	shell_style.content_margin_left = (_style.row_shell_padding.x if _style != null else 3) + 2
	shell_style.content_margin_top = (_style.row_shell_padding.y if _style != null else 2)
	shell_style.content_margin_right = (_style.row_shell_padding.z if _style != null else 3)
	shell_style.content_margin_bottom = (_style.row_shell_padding.w if _style != null else 2)
	shell_style.border_width_left = 1
	shell_style.border_width_top = 1
	shell_style.border_width_right = 1
	shell_style.border_width_bottom = 1
	shell_style.border_color = border
	_row_shell.add_theme_stylebox_override("panel", shell_style)

	var info_style := StyleBoxFlat.new()
	info_style.bg_color = info_bg
	info_style.corner_radius_top_left = 4
	info_style.corner_radius_top_right = 4
	info_style.corner_radius_bottom_right = 4
	info_style.corner_radius_bottom_left = 4
	info_style.border_width_left = 1
	info_style.border_width_top = 1
	info_style.border_width_right = 1
	info_style.border_width_bottom = 1
	info_style.border_color = border.lightened(0.1)
	_info_panel.add_theme_stylebox_override("panel", info_style)

	if _accent_bar != null:
		_accent_bar.color = accent


func _row_kind(model: CamBANGStatusPanel.StatusEntryModel) -> String:
	if model == null:
		return "authoritative"
	if model.id.begins_with("retained_presentation/") or _has_badge(model, "retained"):
		return "retained"
	if model.label == "contract_gaps" or model.label == "projection_gaps":
		return "contract_gap"
	for line in model.anomaly_info_lines:
		if _is_anomaly_line(line):
			return "contract_gap"
	if (
		model.id.begins_with("native_object/")
		or model.id.begins_with("frameproducer/")
		or model.id.contains("orphaned_native_objects")
		or _has_badge(model, "detached")
		or _has_badge(model, "orphaned")
	):
		return "fallback"
	return "authoritative"


func _object_class(model: CamBANGStatusPanel.StatusEntryModel) -> String:
	if model == null:
		return "generic"
	if model.id == "server/main":
		return "server"
	if model.label == "contract_gaps" or model.label == "projection_gaps":
		return "contract_gap"
	if model.id.contains("orphaned_native_objects") or model.label.begins_with("retained_orphan/"):
		return "orphan"
	if model.id.begins_with("provider/") or model.id.contains("/provider/"):
		return "provider"
	if model.id.begins_with("stream/") or model.id.contains("/stream/"):
		return "stream"
	if model.id.begins_with("device/") or model.id.contains("/device/"):
		return "device"
	if model.id.begins_with("rig/") or model.id.contains("/rig/"):
		return "rig"
	if (
		model.id.begins_with("native_object/")
		or model.id.contains("/native_object/")
		or model.id.begins_with("frameproducer/")
		or model.id.contains("/frameproducer/")
	):
		return "native_object"
	return "generic"


func _class_color(object_class: String) -> Color:
	match object_class:
		"server":
			return _resolve_class_theme_color("status_row_server_accent", Color(0.73, 0.81, 0.90, 0.96))
		"provider":
			return _resolve_class_theme_color("status_row_provider_accent", Color(0.47, 0.71, 0.96, 0.96))
		"device":
			return _resolve_class_theme_color("status_row_device_accent", Color(0.41, 0.82, 0.61, 0.96))
		"stream":
			return _resolve_class_theme_color("status_row_stream_accent", Color(0.63, 0.70, 0.97, 0.96))
		"rig":
			return _resolve_class_theme_color("status_row_rig_accent", Color(0.83, 0.68, 0.96, 0.96))
		"native_object":
			return _resolve_class_theme_color("status_row_native_accent", Color(0.44, 0.82, 0.85, 0.96))
		"contract_gap":
			return _resolve_class_theme_color("status_row_contract_gap_accent", Color(0.97, 0.54, 0.42, 0.98))
		"orphan":
			return _resolve_class_theme_color("status_row_orphan_accent", Color(0.98, 0.71, 0.33, 0.98))
		_:
			return _resolve_class_theme_color("status_row_generic_accent", Color(0.70, 0.74, 0.79, 0.94))


func _resolve_class_theme_color(token: StringName, fallback: Color) -> Color:
	if has_theme_color(token, "CamBANGStatusPanel"):
		return get_theme_color(token, "CamBANGStatusPanel")
	return fallback


func _has_badge(model: CamBANGStatusPanel.StatusEntryModel, label: String) -> bool:
	for badge in model.badges:
		if badge.label == label:
			return true
	return false


func _badges_for_render(model: CamBANGStatusPanel.StatusEntryModel) -> Array[CamBANGStatusPanel.BadgeModel]:
	var rendered: Array[CamBANGStatusPanel.BadgeModel] = []
	for badge in model.badges:
		rendered.append(badge)

	var row_kind := _row_kind(model)
	if row_kind == "contract_gap" and not _contains_badge_label(rendered, "contract-gap"):
		rendered.append(_make_badge("error", "contract-gap"))
	elif row_kind == "fallback" and not _contains_badge_label(rendered, "fallback"):
		rendered.append(_make_badge("info", "fallback"))
	elif row_kind == "retained" and not _contains_badge_label(rendered, "retained"):
		rendered.append(_make_badge("warning", "retained"))

	return rendered


func _contains_badge_label(badges: Array[CamBANGStatusPanel.BadgeModel], label: String) -> bool:
	for badge in badges:
		if badge.label == label:
			return true
	return false


func _make_badge(role: String, label: String) -> CamBANGStatusPanel.BadgeModel:
	var badge := CamBANGStatusPanel.BadgeModel.new()
	badge.role = role
	badge.label = label
	return badge


func _badge_role_for_render(badge: CamBANGStatusPanel.BadgeModel) -> String:
	if badge == null:
		return "neutral"
	match badge.label:
		"contract-gap", "schema", "projection":
			return "error"
		"retained", "retained-root", "orphaned", "detached", "fallback":
			return "warning"
		_:
			return badge.role


func _badge_display_label(raw_label: String) -> String:
	if raw_label.begins_with("phase="):
		return _phase_display_label(int(raw_label.substr("phase=".length())), false)
	if raw_label.begins_with("native_phase="):
		return _phase_display_label(int(raw_label.substr("native_phase=".length())), true)
	match raw_label:
		"snapshot":
			return "AUTHORITATIVE"
		"published":
			return "PUBLISHED"
		"retained":
			return "RETAINED"
		"retained-root":
			return "RETAINED ROOT"
		"fallback":
			return "FALLBACK"
		"contract-gap":
			return "CONTRACT GAP"
		"schema":
			return "CONTRACT GAP"
		"projection":
			return "PROJECTION GAP"
		"destroyed":
			return "DESTROYED"
		"orphaned":
			return "ORPHANED"
		"detached":
			return "DETACHED"
		"snapshot-unavailable":
			return "NO SNAPSHOT"
		"snapshot-incompatible":
			return "SNAPSHOT INCOMPATIBLE"
		"rig-context":
			return "RIG CONTEXT"
		"info":
			return "INFO"
		_:
			return raw_label.replace("-", " ").replace("_", " ").to_upper()


func _phase_display_label(phase_value: int, is_native: bool) -> String:
	var phase_text := "UNKNOWN"
	match phase_value:
		0:
			phase_text = "CREATED"
		1:
			phase_text = "LIVE"
		2:
			phase_text = "TEARING DOWN"
		3:
			phase_text = "DESTROYED"
	if is_native:
		return "NATIVE %s" % phase_text
	return phase_text


func _is_anomaly_line(line: String) -> bool:
	return (
		line.begins_with("Contract gap:")
		or line.begins_with("Contract ambiguity:")
		or line.begins_with("Projection invariant:")
		or line.begins_with("Projection gap:")
		or line.find("contradiction") >= 0
	)


func _is_detail_line(line: String, detail_info_lines: Array[String]) -> bool:
	for detail_line in detail_info_lines:
		if detail_line == line:
			return true
	return false


func _on_disclosure_pressed() -> void:
	_disclosure_indicator.set_expanded(_disclosure_button.button_pressed)
	disclosure_toggled.emit(_entry_id, _disclosure_button.button_pressed)


func _bind_nodes() -> void:
	if _indent_region != null:
		return
	_indent_region = $StatusEntryRoot/MainRow/IndentRegion
	_disclosure_slot = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot
	_disclosure_button = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot/DisclosureButton
	_disclosure_placeholder = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot/DisclosurePlaceholder
	_disclosure_indicator = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/DisclosureSlot/DisclosureButton/DisclosureIndicator
	_name_label = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment/NameLabel
	_identity_segment = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/IdentitySegment
	_row_content = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent
	_state_segment = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel/InfoMargin/InfoInner/StateSegment
	_counter_segment = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel/InfoMargin/InfoInner/CounterSegment
	_info_lines_container = $StatusEntryRoot/InfoLines
	_info_panel_inset = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset
	_info_margin = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel/InfoMargin
	_info_panel = $StatusEntryRoot/MainRow/EntryShell/ShellContent/RowContent/InfoPanelInset/InfoPanel
	_row_shell = $StatusEntryRoot/MainRow/EntryShell
	_accent_bar = $StatusEntryRoot/MainRow/EntryShell/AccentBar
	_disclosure_indicator.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_disclosure_placeholder.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_row_content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if not _disclosure_button.pressed.is_connected(_on_disclosure_pressed):
		_disclosure_button.pressed.connect(_on_disclosure_pressed)
