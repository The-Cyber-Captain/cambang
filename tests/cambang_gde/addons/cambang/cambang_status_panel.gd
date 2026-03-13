@tool
class_name CamBANGStatusPanel
extends PanelContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"
const STATUS_ENTRY_SCENE := preload("res://addons/cambang/internal/status_entry.tscn")


class PanelModel extends RefCounted:
	var entries: Array[StatusEntryModel] = []


class StatusEntryModel extends RefCounted:
	var id: String = ""
	var parent_id: String = ""
	var depth: int = 0
	var label: String = ""
	var expanded: bool = false
	var can_expand: bool = false
	var badges: Array[BadgeModel] = []
	var counters: Array[CounterModel] = []
	var info_lines: Array[String] = []


class BadgeModel extends RefCounted:
	var role: String = "neutral"
	var label: String = ""


class CounterModel extends RefCounted:
	var name: String = ""
	var value: int = 0
	var digits: int = 1


class StatusPanelStyle extends RefCounted:
	var panel_bg: Color
	var identity_font: Font
	var identity_font_size: int
	var identity_font_color: Color
	var identity_outline_color: Color
	var identity_outline_size: int

	var info_font: Font
	var info_panel_bg: Color
	var counter_box_bg: Color

	var state_font_size: int
	var state_font_color: Color
	var state_outline_color: Color
	var state_outline_size: int

	var counter_font: Font
	var counter_font_size: int
	var counter_font_color: Color

	var row_shell_bg: Color
	var row_shell_radius: int
	var row_shell_padding: Vector4
	var identity_info_gap: int
	var info_panel_outer_inset: Vector4
	var info_panel_inner_padding: Vector4
	var counter_box_radius: int
	var counter_box_h_padding: int
	var counter_box_v_padding: int
	var disclosure_slot_width: int
	var disclosure_visual_scale: float
	var badge_strip_width: int


@export var panel_style_overrides: Dictionary = {}
@export var shared_style_overrides: Dictionary = {}

var _title_label: Label
var _provider_mode_value: Label
var _snapshot_state_value: Label
var _gen_value: Label
var _version_value: Label
var _topology_version_value: Label
var _schema_version_value: Label
var _counts_value: Label
var _timestamp_value: Label
var _status_rows_scroll: ScrollContainer
var _status_rows: VBoxContainer
var _dev_expanded_by_id: Dictionary = {}
var _dev_parent_by_id: Dictionary = {}
var _server: Object = null


func _ready() -> void:
	_build_ui_if_needed()
	_apply_panel_style(_resolve_style())
	_connect_server()
	_refresh_from_server()
	_render_panel_model(_build_fake_panel_model())


func _enter_tree() -> void:
	if is_node_ready():
		_build_ui_if_needed()
		_apply_panel_style(_resolve_style())
		_connect_server()
		_refresh_from_server()


func _exit_tree() -> void:
	_disconnect_server()


func force_refresh() -> void:
	_refresh_from_server()


func _build_ui_if_needed() -> void:
	if _title_label != null:
		return

	custom_minimum_size = Vector2(320, 0)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var outer := MarginContainer.new()
	outer.add_theme_constant_override("margin_left", 10)
	outer.add_theme_constant_override("margin_top", 10)
	outer.add_theme_constant_override("margin_right", 10)
	outer.add_theme_constant_override("margin_bottom", 10)
	add_child(outer)

	var root := VBoxContainer.new()
	root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	outer.add_child(root)

	_title_label = Label.new()
	_title_label.text = "CamBANG Status"
	root.add_child(_title_label)

	var grid := GridContainer.new()
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.add_child(grid)

	_provider_mode_value = _add_row(grid, "Provider Mode")
	_snapshot_state_value = _add_row(grid, "Snapshot State")
	_gen_value = _add_row(grid, "Generation")
	_version_value = _add_row(grid, "Version")
	_topology_version_value = _add_row(grid, "Topology Version")
	_schema_version_value = _add_row(grid, "Schema Version")
	_counts_value = _add_row(grid, "Entity Counts")
	_timestamp_value = _add_row(grid, "timestamp_ns")

	_status_rows_scroll = ScrollContainer.new()
	_status_rows_scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status_rows_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_status_rows_scroll.custom_minimum_size = Vector2(0, 220)
	root.add_child(_status_rows_scroll)

	_status_rows = VBoxContainer.new()
	_status_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status_rows.add_theme_constant_override("separation", 2)
	_status_rows_scroll.add_child(_status_rows)


func _add_row(grid: GridContainer, label_text: String) -> Label:
	var key := Label.new()
	key.text = "%s:" % label_text
	grid.add_child(key)

	var value := Label.new()
	value.text = "-"
	value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	value.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	grid.add_child(value)
	return value


func _resolve_style() -> StatusPanelStyle:
	var style := StatusPanelStyle.new()
	var fallback_font := ThemeDB.fallback_font

	style.panel_bg = _resolve_panel_color("status_panel_bg", Color(0.10, 0.12, 0.15, 0.88))
	style.identity_font = _resolve_shared_font("status_identity_font", fallback_font)
	style.identity_font_size = _resolve_shared_int("status_identity_font_size", 13)
	style.identity_font_color = _resolve_panel_color("status_identity_font_color", Color(0.88, 0.90, 0.93, 1.0))
	style.identity_outline_color = _resolve_panel_color("status_identity_outline_color", Color(0.02, 0.03, 0.04, 0.55))
	style.identity_outline_size = maxi(
		1,
		int(round(float(style.identity_font_size) * _resolve_shared_float("status_identity_outline_ratio", 0.08)))
	)

	style.info_font = _resolve_shared_font("status_info_font", fallback_font)
	style.info_panel_bg = _resolve_shared_color("status_info_panel_bg", Color(0.10, 0.11, 0.13, 0.82))
	style.counter_box_bg = _resolve_shared_color("status_counter_box_bg", Color(0.17, 0.18, 0.21, 0.95))

	style.state_font_size = maxi(
		10,
		int(round(float(style.identity_font_size) * _resolve_shared_float("status_state_font_ratio", 0.86)))
	)
	style.state_font_color = _resolve_shared_color("status_state_font_color", Color(0.84, 0.87, 0.90, 1.0))
	style.state_outline_color = _resolve_shared_color("status_state_outline_color", Color(0.01, 0.02, 0.03, 0.45))
	style.state_outline_size = maxi(
		1,
		int(round(float(style.identity_font_size) * _resolve_shared_float("status_state_outline_ratio", 0.07)))
	)

	style.counter_font = _make_bold_font(style.info_font)
	style.counter_font_size = maxi(
		10,
		int(round(float(style.identity_font_size) * _resolve_shared_float("status_counter_font_ratio", 0.84)))
	)
	style.counter_font_color = _resolve_shared_color("status_counter_font_color", Color(0.90, 0.92, 0.95, 1.0))

	style.row_shell_bg = Color(0.11, 0.11, 0.13, 0.4)
	style.row_shell_radius = 5
	style.row_shell_padding = Vector4(3, 2, 3, 2)
	style.identity_info_gap = 6
	style.info_panel_outer_inset = Vector4(2, 1, 2, 1)
	style.info_panel_inner_padding = Vector4(5, 2, 5, 2)
	style.counter_box_radius = 3
	style.counter_box_h_padding = 4
	style.counter_box_v_padding = 2
	style.disclosure_slot_width = 28
	style.disclosure_visual_scale = 1.0
	style.badge_strip_width = 7

	return style


func _make_bold_font(base_font: Font) -> Font:
	if base_font == null:
		return ThemeDB.fallback_font
	var variation := FontVariation.new()
	variation.base_font = base_font
	variation.variation_embolden = 0.8
	return variation


func _resolve_panel_color(token: StringName, fallback: Color) -> Color:
	if panel_style_overrides.has(token):
		return panel_style_overrides[token]
	if panel_style_overrides.has(str(token)):
		return panel_style_overrides[str(token)]
	if has_theme_color(token, "CamBANGStatusPanel"):
		return get_theme_color(token, "CamBANGStatusPanel")
	return fallback


func _resolve_shared_color(token: StringName, fallback: Color) -> Color:
	if shared_style_overrides.has(token):
		return shared_style_overrides[token]
	if shared_style_overrides.has(str(token)):
		return shared_style_overrides[str(token)]
	if has_theme_color(token, "CamBANGStatusPanel"):
		return get_theme_color(token, "CamBANGStatusPanel")
	return fallback


func _resolve_shared_font(token: StringName, fallback: Font) -> Font:
	if shared_style_overrides.has(token):
		return shared_style_overrides[token]
	if shared_style_overrides.has(str(token)):
		return shared_style_overrides[str(token)]
	if has_theme_font(token, "CamBANGStatusPanel"):
		return get_theme_font(token, "CamBANGStatusPanel")
	return fallback


func _resolve_shared_int(token: StringName, fallback: int) -> int:
	if shared_style_overrides.has(token):
		return int(shared_style_overrides[token])
	if shared_style_overrides.has(str(token)):
		return int(shared_style_overrides[str(token)])
	if has_theme_constant(token, "CamBANGStatusPanel"):
		return get_theme_constant(token, "CamBANGStatusPanel")
	return fallback


func _resolve_shared_float(token: StringName, fallback: float) -> float:
	if shared_style_overrides.has(token):
		return float(shared_style_overrides[token])
	if shared_style_overrides.has(str(token)):
		return float(shared_style_overrides[str(token)])
	if has_theme_constant(token, "CamBANGStatusPanel"):
		return float(get_theme_constant(token, "CamBANGStatusPanel"))
	return fallback


func _apply_panel_style(style: StatusPanelStyle) -> void:
	var panel_style := StyleBoxFlat.new()
	panel_style.bg_color = style.panel_bg
	panel_style.corner_radius_top_left = 6
	panel_style.corner_radius_top_right = 6
	panel_style.corner_radius_bottom_right = 6
	panel_style.corner_radius_bottom_left = 6
	add_theme_stylebox_override("panel", panel_style)


func _connect_server() -> void:
	var next_server := _get_server()
	if next_server == _server and _server != null:
		if not _server.state_published.is_connected(_on_state_published):
			_server.state_published.connect(_on_state_published)
		return

	_disconnect_server()
	_server = next_server
	if _server != null and not _server.state_published.is_connected(_on_state_published):
		_server.state_published.connect(_on_state_published)


func _disconnect_server() -> void:
	if _server != null and _server.state_published.is_connected(_on_state_published):
		_server.state_published.disconnect(_on_state_published)
	_server = null


func _get_server() -> Object:
	if Engine.has_singleton(SERVER_SINGLETON_NAME):
		return Engine.get_singleton(SERVER_SINGLETON_NAME)
	if has_node("/root/%s" % SERVER_SINGLETON_NAME):
		return get_node("/root/%s" % SERVER_SINGLETON_NAME)
	return null


func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	_refresh_from_server()


func _refresh_from_server() -> void:
	_build_ui_if_needed()
	if _server == null:
		_server = _get_server()
	if _server == null:
		_provider_mode_value.text = "unavailable"
		_apply_snapshot_read({"state": "No snapshot", "counts": "-", "timestamp": "-"})
		return

	_provider_mode_value.text = str(_server.get_provider_mode())
	_apply_snapshot_read(_read_snapshot(_server.get_state_snapshot()))


func _read_snapshot(snapshot: Variant) -> Dictionary:
	if snapshot == null:
		return {
			"state": "No snapshot",
			"counts": "rigs=0  devices=0  streams=0  native_objects=0  detached_roots=0",
			"timestamp": "-",
		}

	if typeof(snapshot) != TYPE_DICTIONARY:
		return {
			"state": "Unexpected snapshot type=%d" % typeof(snapshot),
			"counts": "-",
			"timestamp": "-",
		}

	var d: Dictionary = snapshot
	var rigs := (d.get("rigs", []) as Array).size()
	var devices := (d.get("devices", []) as Array).size()
	var streams := (d.get("streams", []) as Array).size()
	var native_objects := (d.get("native_objects", []) as Array).size()
	var detached_roots := (d.get("detached_root_ids", []) as Array).size()

	return {
		"state": "Snapshot available",
		"gen": str(d.get("gen", "?")),
		"version": str(d.get("version", "?")),
		"topology_version": str(d.get("topology_version", "?")),
		"schema_version": str(d.get("schema_version", "?")),
		"counts": "rigs=%d  devices=%d  streams=%d  native_objects=%d  detached_roots=%d" % [
			rigs,
			devices,
			streams,
			native_objects,
			detached_roots,
		],
		"timestamp": str(d.get("timestamp_ns", "-")),
	}


func _apply_snapshot_read(reading: Dictionary) -> void:
	_snapshot_state_value.text = str(reading.get("state", "No snapshot"))
	_gen_value.text = str(reading.get("gen", "-"))
	_version_value.text = str(reading.get("version", "-"))
	_topology_version_value.text = str(reading.get("topology_version", "-"))
	_schema_version_value.text = str(reading.get("schema_version", "-"))
	_counts_value.text = str(reading.get("counts", "-"))
	_timestamp_value.text = "%s (monotonic publish timestamp)" % str(reading.get("timestamp", "-"))


func _build_fake_panel_model() -> PanelModel:
	var panel := PanelModel.new()

	panel.entries.append(_entry(
		"server/main",
		"",
		0,
		"server/main",
		true,
		true,
		[_badge("success", "up")],
		[_counter("providers", 1, 1)],
		[]
	))
	panel.entries.append(_entry(
		"provider/windows-mf",
		"server/main",
		1,
		"provider/windows-mf",
		true,
		true,
		[_badge("info", "windows-mf")],
		[_counter("devices", 1, 1)],
		[]
	))
	panel.entries.append(_entry(
		"device/cam-front",
		"provider/windows-mf",
		2,
		"device/cam-front",
		true,
		true,
		[_badge("success", "active")],
		[_counter("streams", 1, 1)],
		[]
	))
	panel.entries.append(_entry(
		"stream/video0",
		"device/cam-front",
		3,
		"stream/video0",
		false,
		false,
		[_badge("neutral", "rgb8")],
		[_counter("fps", 60, 2)],
		[]
	))

	panel.entries.append(_entry(
		"rig/stereo-a",
		"server/main",
		1,
		"rig/stereo-a",
		false,
		true,
		[_badge("neutral", "dual-device")],
		[_counter("devices", 2, 1)],
		[]
	))
	panel.entries.append(_entry(
		"device/left-eye",
		"rig/stereo-a",
		2,
		"device/left-eye",
		false,
		false,
		[_badge("success", "attached")],
		[_counter("streams", 1, 1)],
		[]
	))
	panel.entries.append(_entry(
		"device/right-eye",
		"rig/stereo-a",
		2,
		"device/right-eye",
		false,
		false,
		[_badge("success", "attached")],
		[_counter("streams", 1, 1)],
		[]
	))

	panel.entries.append(_entry(
		"native_object/orphan-42",
		"",
		0,
		"native_object/orphan-42",
		true,
		false,
		[
			_badge("warning", "orphan"),
			_badge("error", "unbound"),
		],
		[
			_counter("refs", 15, 1),
			_counter("watchers", 237, 2),
			_counter("frames", 2378, 3),
			_counter("bytes", 37367, 3),
			_counter("events", 104552, 4),
		],
		[
			"Contract gap: stream published before rig association.",
		]
	))
	panel.entries.append(_entry(
		"stream/unclaimed",
		"native_object/orphan-42",
		1,
		"stream/unclaimed",
		false,
		false,
		[_badge("error", "contract-gap")],
		[_counter("frames", 0, 2)],
		[
			"Projection path not implemented in prototype.",
		]
	))

	return panel


func _render_panel_model(model: PanelModel) -> void:
	if _status_rows == null:
		return

	for child in _status_rows.get_children():
		child.queue_free()

	var style := _resolve_style()
	_dev_parent_by_id.clear()
	for entry_model in model.entries:
		_dev_parent_by_id[entry_model.id] = entry_model.parent_id

	for entry_model in model.entries:
		if not _dev_expanded_by_id.has(entry_model.id):
			_dev_expanded_by_id[entry_model.id] = entry_model.expanded
		entry_model.expanded = bool(_dev_expanded_by_id.get(entry_model.id, entry_model.expanded))

		if not _is_entry_visible(entry_model):
			continue

		var entry := STATUS_ENTRY_SCENE.instantiate()
		_status_rows.add_child(entry)
		entry.set_style(style)
		entry.set_model(entry_model)
		if entry.has_signal("disclosure_toggled"):
			entry.disclosure_toggled.connect(_on_entry_disclosure_toggled)


func _is_entry_visible(entry_model: StatusEntryModel) -> bool:
	if entry_model.parent_id == "":
		return true

	var current_parent := entry_model.parent_id
	while current_parent != "":
		if not bool(_dev_expanded_by_id.get(current_parent, true)):
			return false
		current_parent = _find_parent_id(current_parent)
	return true


func _find_parent_id(entry_id: String) -> String:
	return str(_dev_parent_by_id.get(entry_id, ""))


func _on_entry_disclosure_toggled(entry_id: String, expanded: bool) -> void:
	_dev_expanded_by_id[entry_id] = expanded
	_render_panel_model(_build_fake_panel_model())


func _entry(
		id: String,
		parent_id: String,
		depth: int,
		label: String,
		expanded: bool,
		can_expand: bool,
		badges: Array[BadgeModel],
		counters: Array[CounterModel],
		info_lines: Array[String]
	) -> StatusEntryModel:
	var model := StatusEntryModel.new()
	model.id = id
	model.parent_id = parent_id
	model.depth = depth
	model.label = label
	model.expanded = expanded
	model.can_expand = can_expand
	model.badges = badges
	model.counters = counters
	model.info_lines = info_lines
	return model


func _badge(role: String, label: String) -> BadgeModel:
	var model := BadgeModel.new()
	model.role = role
	model.label = label
	return model


func _counter(name: String, value: int, digits: int) -> CounterModel:
	var model := CounterModel.new()
	model.name = name
	model.value = value
	model.digits = digits
	return model
