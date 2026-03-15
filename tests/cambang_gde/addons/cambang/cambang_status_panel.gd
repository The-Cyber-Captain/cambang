@tool
class_name CamBANGStatusPanel
extends PanelContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"
const STATUS_ENTRY_SCENE := preload("res://addons/cambang/internal/status_entry.tscn")
const TOUCH_SCROLL_SCRIPT := preload("res://addons/cambang/internal/touch_scroll_container.gd")
# PROVISIONAL: placeholder until runtime exports authoritative retention-window policy.
const PROVISIONAL_RETAINED_PRESENTATION_TTL_NS := -1
const RETAINED_PRESENTATION_ROOT_ID := "retained_presentation/prior_authoritative"


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
	var counter_box_border: Color

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
	var counter_box_border_width: int
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
var _last_snapshot_meta: Dictionary = {}
var _last_panel_model: PanelModel = null
var _last_active_panel_model: PanelModel = null
var _last_authoritative_panel_model: PanelModel = null
var _last_authoritative_gen: int = -1
var _retained_presentation_model: PanelModel = null
var _retained_source_gen: int = -1
var _retained_recorded_timestamp_ns: int = -1


func _ready() -> void:
	_build_ui_if_needed()
	_apply_panel_style(_resolve_style())
	_connect_server()
	_refresh_from_server()


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
	_status_rows_scroll.set_script(TOUCH_SCROLL_SCRIPT)
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

	style.panel_bg = _resolve_panel_color("status_panel_bg", Color(0.07, 0.09, 0.12, 0.9))
	style.identity_font = _resolve_shared_font("status_identity_font", fallback_font)
	style.identity_font_size = _resolve_shared_int("status_identity_font_size", 13)
	style.identity_font_color = _resolve_panel_color("status_identity_font_color", Color(0.88, 0.90, 0.93, 1.0))
	style.identity_outline_color = _resolve_panel_color("status_identity_outline_color", Color(0.02, 0.03, 0.04, 0.55))
	style.identity_outline_size = maxi(
		1,
		int(round(float(style.identity_font_size) * _resolve_shared_float("status_identity_outline_ratio", 0.08)))
	)

	style.info_font = _resolve_shared_font("status_info_font", fallback_font)
	style.info_panel_bg = _resolve_shared_color("status_info_panel_bg", Color(0.10, 0.12, 0.15, 0.88))
	style.counter_box_bg = _resolve_shared_color("status_counter_box_bg", Color(0.1, 0.11, 0.14, 0.95))
	style.counter_box_border = _resolve_shared_color("status_counter_box_border", Color(0.34, 0.38, 0.44, 0.85))

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

	style.row_shell_bg = Color(0.16, 0.18, 0.22, 0.68)
	style.row_shell_radius = 5
	style.row_shell_padding = Vector4(3, 2, 3, 2)
	style.identity_info_gap = 6
	style.info_panel_outer_inset = Vector4(3, 2, 3, 2)
	style.info_panel_inner_padding = Vector4(5, 2, 5, 2)
	style.counter_box_radius = 3
	style.counter_box_h_padding = 4
	style.counter_box_v_padding = 2
	style.counter_box_border_width = 1
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
		_apply_snapshot_read({"state": "No server", "counts": "-", "timestamp": "-"})
		_last_snapshot_meta.clear()
		var no_server_panel := _build_nil_panel_model("No server singleton available.")
		_last_active_panel_model = no_server_panel
		_last_panel_model = _compose_presented_panel_model(no_server_panel)
		_render_panel_model(_last_panel_model)
		return

	var provider_mode := str(_server.get_provider_mode())
	_provider_mode_value.text = provider_mode
	var snapshot := _fetch_snapshot()
	var reading := _read_snapshot(snapshot)
	_apply_snapshot_read(reading)

	if snapshot == null:
		_last_snapshot_meta.clear()
		var nil_panel := _build_nil_panel_model("No published snapshot yet.")
		_last_active_panel_model = nil_panel
		_last_panel_model = _compose_presented_panel_model(nil_panel)
		_render_panel_model(_last_panel_model)
		return

	if typeof(snapshot) != TYPE_DICTIONARY:
		_last_snapshot_meta.clear()
		var bad_type_panel := _build_nil_panel_model("Contract gap: snapshot must be Dictionary; got type=%d." % typeof(snapshot))
		_last_active_panel_model = bad_type_panel
		_last_panel_model = _compose_presented_panel_model(bad_type_panel)
		_render_panel_model(_last_panel_model)
		return

	var category := _categorize_snapshot_update(snapshot)
	match category:
		"none":
			if _last_panel_model != null:
				_render_panel_model(_last_panel_model)
		"value_refresh":
			# Stage 1 keeps refresh behavior simple by rebuilding from snapshot,
			# while preserving explicit category separation for later optimization.
			var value_panel := _project_snapshot_to_panel_model(snapshot, provider_mode)
			_last_active_panel_model = value_panel
			_last_panel_model = _compose_presented_panel_model(value_panel)
			_render_panel_model(_last_panel_model)
		_:
			var full_panel := _project_snapshot_to_panel_model(snapshot, provider_mode)
			_last_active_panel_model = full_panel
			_last_panel_model = _compose_presented_panel_model(full_panel)
			_render_panel_model(_last_panel_model)


func _compose_presented_panel_model(active_panel: PanelModel) -> PanelModel:
	var active_gen := _extract_panel_generation(active_panel)
	_capture_authoritative_for_retained(active_panel, active_gen)
	_maybe_expire_retained_presentation()

	var composed := _clone_panel_model(active_panel)
	if _retained_presentation_model != null:
		_append_retained_presentation_subtree(composed, _retained_presentation_model)
	return composed


func _capture_authoritative_for_retained(active_panel: PanelModel, active_gen: int) -> void:
	var should_promote_to_retained := false
	if _last_authoritative_panel_model != null:
		if active_gen < 0:
			should_promote_to_retained = true
		elif _last_authoritative_gen >= 0 and active_gen != _last_authoritative_gen:
			should_promote_to_retained = true

	if should_promote_to_retained:
		_retain_panel_for_presentation(_last_authoritative_panel_model, _last_authoritative_gen)

	if active_gen >= 0:
		_last_authoritative_panel_model = _clone_panel_model(active_panel)
		_last_authoritative_gen = active_gen


func _retain_panel_for_presentation(source_model: PanelModel, source_gen: int) -> void:
	if source_model == null:
		return
	_retained_presentation_model = _clone_panel_model(source_model)
	_retained_source_gen = source_gen
	_retained_recorded_timestamp_ns = Time.get_ticks_usec() * 1000


func _extract_panel_generation(panel: PanelModel) -> int:
	if panel == null:
		return -1
	for entry in panel.entries:
		if entry.id != "server/main":
			continue
		for counter in entry.counters:
			if counter.name == "gen":
				return counter.value
	return -1


func _maybe_expire_retained_presentation() -> void:
	if _retained_presentation_model == null:
		return
	if PROVISIONAL_RETAINED_PRESENTATION_TTL_NS < 0:
		return
	var elapsed_ns := (Time.get_ticks_usec() * 1000) - _retained_recorded_timestamp_ns
	if elapsed_ns >= PROVISIONAL_RETAINED_PRESENTATION_TTL_NS:
		_clear_retained_presentation()


func _clear_retained_presentation() -> void:
	_retained_presentation_model = null
	_retained_source_gen = -1
	_retained_recorded_timestamp_ns = -1


func _clone_panel_model(source: PanelModel) -> PanelModel:
	var cloned := PanelModel.new()
	if source == null:
		return cloned
	for entry in source.entries:
		var cloned_badges: Array[BadgeModel] = []
		for badge in entry.badges:
			cloned_badges.append(_badge(badge.role, badge.label))
		var cloned_counters: Array[CounterModel] = []
		for counter in entry.counters:
			cloned_counters.append(_counter(counter.name, counter.value, counter.digits))
		var cloned_info_lines: Array[String] = []
		for line in entry.info_lines:
			cloned_info_lines.append(line)
		cloned.entries.append(_entry(
			entry.id,
			entry.parent_id,
			entry.depth,
			entry.label,
			entry.expanded,
			entry.can_expand,
			cloned_badges,
			cloned_counters,
			cloned_info_lines
		))
	return cloned


func _append_retained_presentation_subtree(target_panel: PanelModel, retained_source: PanelModel) -> void:
	if target_panel == null or retained_source == null:
		return

	target_panel.entries.append(_entry(
		RETAINED_PRESENTATION_ROOT_ID,
		"",
		0,
		"retained prior-authoritative view",
		false,
		true,
		[_badge("warning", "retained")],
		[_counter("source_gen", _retained_source_gen, 1)],
		[
			"Presentation continuity only. Not active snapshot truth.",
			"Rows below are copied from the last authoritative projected model.",
		]
	))

	for retained_entry in retained_source.entries:
		var cloned_entry := _clone_status_entry(retained_entry)
		cloned_entry.id = "retained_presentation/%s" % retained_entry.id
		if retained_entry.parent_id == "":
			cloned_entry.parent_id = RETAINED_PRESENTATION_ROOT_ID
			cloned_entry.depth = 1
		else:
			cloned_entry.parent_id = "retained_presentation/%s" % retained_entry.parent_id
			cloned_entry.depth = retained_entry.depth + 1
		cloned_entry.label = "%s [retained]" % retained_entry.label
		cloned_entry.badges.append(_badge("warning", "retained"))
		target_panel.entries.append(cloned_entry)

	_ensure_expandability(target_panel)


func _clone_status_entry(source: StatusEntryModel) -> StatusEntryModel:
	var cloned_badges: Array[BadgeModel] = []
	for badge in source.badges:
		cloned_badges.append(_badge(badge.role, badge.label))
	var cloned_counters: Array[CounterModel] = []
	for counter in source.counters:
		cloned_counters.append(_counter(counter.name, counter.value, counter.digits))
	var cloned_info_lines: Array[String] = []
	for line in source.info_lines:
		cloned_info_lines.append(line)
	return _entry(
		source.id,
		source.parent_id,
		source.depth,
		source.label,
		source.expanded,
		source.can_expand,
		cloned_badges,
		cloned_counters,
		cloned_info_lines
	)


func _fetch_snapshot() -> Variant:
	if _server == null:
		return null
	if not _server.has_method("get_state_snapshot"):
		return null
	return _server.get_state_snapshot()


func _categorize_snapshot_update(snapshot: Dictionary) -> String:
	var gen := int(snapshot.get("gen", -1))
	var version := int(snapshot.get("version", -1))
	var topology_version := int(snapshot.get("topology_version", -1))

	if _last_snapshot_meta.is_empty():
		_last_snapshot_meta = {
			"gen": gen,
			"version": version,
			"topology_version": topology_version,
		}
		return "structural_rebuild"

	var last_gen := int(_last_snapshot_meta.get("gen", -1))
	var last_version := int(_last_snapshot_meta.get("version", -1))
	var last_topology := int(_last_snapshot_meta.get("topology_version", -1))

	var category := "none"
	if gen != last_gen:
		category = "structural_rebuild"
	elif topology_version != last_topology:
		category = "structural_rebuild"
	elif version != last_version:
		category = "value_refresh"

	_last_snapshot_meta = {
		"gen": gen,
		"version": version,
		"topology_version": topology_version,
	}
	return category


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


func _build_nil_panel_model(reason: String) -> PanelModel:
	var panel := PanelModel.new()
	panel.entries.append(_entry(
		"server/main",
		"",
		0,
		"server/main",
		true,
		true,
		[_badge("warning", "snapshot-unavailable")],
		[],
		[reason]
	))
	return panel


func _project_snapshot_to_panel_model(snapshot: Dictionary, provider_mode: String) -> PanelModel:
	var panel := PanelModel.new()
	var issues: Array[String] = []

	var server_badges: Array[BadgeModel] = [_badge("success", "snapshot")]
	var server_counters: Array[CounterModel] = [
		_counter("gen", int(snapshot.get("gen", 0)), 1),
		_counter("version", int(snapshot.get("version", 0)), 1),
		_counter("topology", int(snapshot.get("topology_version", 0)), 1),
	]
	var server_info_lines: Array[String] = []
	var server_entry := _entry(
		"server/main",
		"",
		0,
		"server/main",
		true,
		true,
		server_badges,
		server_counters,
		server_info_lines
	)
	panel.entries.append(server_entry)

	var devices := _safe_array(snapshot.get("devices", []), issues, "devices")
	var rigs := _safe_array(snapshot.get("rigs", []), issues, "rigs")
	var streams := _safe_array(snapshot.get("streams", []), issues, "streams")
	var native_objects := _safe_array(snapshot.get("native_objects", []), issues, "native_objects")
	var detached_root_ids := _safe_array(snapshot.get("detached_root_ids", []), issues, "detached_root_ids")
	var snapshot_gen := int(snapshot.get("gen", -1))
	var native_partition := _partition_native_objects_by_generation(snapshot_gen, native_objects, issues)
	var current_native_objects: Array = native_partition.get("current", [])
	var prior_native_objects: Array = native_partition.get("prior", [])
	var native_dead_count := _count_native_destroyed(native_objects)
	var current_provider_native_objects: Array = []
	for i in range(current_native_objects.size()):
		var current_rec := _safe_dict(current_native_objects[i], issues, "native_objects[current][%d]" % i)
		if current_rec.is_empty():
			continue
		if _native_object_is_provider(current_rec):
			current_provider_native_objects.append(current_rec)

	if current_provider_native_objects.size() != 1:
		issues.append(
			"Contract ambiguity: expected exactly one current-generation provider native object (type=Provider), found %d."
			% current_provider_native_objects.size()
		)
		panel.entries.append(_entry(
			"server/main/contract_gaps",
			"server/main",
			1,
			"contract_gaps",
			true,
			false,
			[_badge("warning", "schema")],
			[_counter("count", issues.size(), 1)],
			issues
		))
		_ensure_expandability(panel)
		return panel

	var provider_native_rec: Dictionary = current_provider_native_objects[0]
	var provider_native_id := int(provider_native_rec.get("native_id", 0))
	if provider_native_id <= 0:
		issues.append("Contract ambiguity: provider native object missing valid native_id.")
		panel.entries.append(_entry(
			"server/main/contract_gaps",
			"server/main",
			1,
			"contract_gaps",
			true,
			false,
			[_badge("warning", "schema")],
			[_counter("count", issues.size(), 1)],
			issues
		))
		_ensure_expandability(panel)
		return panel

	var provider_id := "provider/%d" % provider_native_id

	var provider_counters: Array[CounterModel] = [
		_counter("rigs", rigs.size(), 1),
		_counter("devices", devices.size(), 1),
		_counter("streams", streams.size(), 1),
		_counter("native_all", native_objects.size(), 1),
		_counter("native_cur", current_native_objects.size(), 1),
		_counter("native_prev", prior_native_objects.size(), 1),
		_counter("native_dead", native_dead_count, 1),
	]
	var provider_badges: Array[BadgeModel] = [
		_badge("info", "published"),
		_badge("neutral", "phase=%d" % int(provider_native_rec.get("phase", -1))),
	]
	var provider_info_lines: Array[String] = []
	var provider_entry := _entry(
		provider_id,
		"server/main",
		1,
		"provider/%s" % _safe_label_component(provider_mode, "unknown"),
		true,
		true,
		provider_badges,
		provider_counters,
		provider_info_lines
	)
	panel.entries.append(provider_entry)

	var promoted_native_ids := {}
	var current_device_native_matches_by_instance := {}
	var current_stream_native_matches_by_stream_id := {}
	var current_non_live_device_native_matches_by_instance := {}
	var current_non_live_stream_native_matches_by_stream_id := {}
	for i in range(current_native_objects.size()):
		var native_rec := _safe_dict(current_native_objects[i], issues, "native_objects[current][%d]" % i)
		if native_rec.is_empty():
			continue
		var native_type_key := _native_object_type_key(native_rec)
		if native_type_key == "device":
			var owner_instance_id := int(native_rec.get("owner_device_instance_id", 0))
			if owner_instance_id > 0:
				if not current_device_native_matches_by_instance.has(owner_instance_id):
					current_device_native_matches_by_instance[owner_instance_id] = []
				current_device_native_matches_by_instance[owner_instance_id].append(native_rec)
				if int(native_rec.get("phase", -1)) >= 2:
					if not current_non_live_device_native_matches_by_instance.has(owner_instance_id):
						current_non_live_device_native_matches_by_instance[owner_instance_id] = []
					current_non_live_device_native_matches_by_instance[owner_instance_id].append(native_rec)
		elif native_type_key == "stream":
			var owner_stream_id := int(native_rec.get("owner_stream_id", 0))
			if owner_stream_id > 0:
				if not current_stream_native_matches_by_stream_id.has(owner_stream_id):
					current_stream_native_matches_by_stream_id[owner_stream_id] = []
				current_stream_native_matches_by_stream_id[owner_stream_id].append(native_rec)
				if int(native_rec.get("phase", -1)) >= 2:
					if not current_non_live_stream_native_matches_by_stream_id.has(owner_stream_id):
						current_non_live_stream_native_matches_by_stream_id[owner_stream_id] = []
					current_non_live_stream_native_matches_by_stream_id[owner_stream_id].append(native_rec)

	var devices_by_instance := {}
	var provider_device_ids_by_instance := {}
	for i in range(devices.size()):
		var rec := _safe_dict(devices[i], issues, "devices[%d]" % i)
		if rec.is_empty():
			continue
		var instance_id := int(rec.get("instance_id", 0))
		if instance_id <= 0:
			issues.append("Contract gap: devices[%d] missing valid instance_id." % i)
			continue
		devices_by_instance[instance_id] = rec
		var device_label := "device/%s" % _safe_device_name(rec)
		var device_entry_id := "device/%d" % instance_id
		provider_device_ids_by_instance[instance_id] = device_entry_id
		var device_badges: Array[BadgeModel] = [_badge("neutral", "phase=%d" % int(rec.get("phase", -1)))]
		var device_info: Array[String] = []
		var device_matches: Array = current_device_native_matches_by_instance.get(instance_id, [])
		if device_matches.size() == 1:
			var device_native_rec: Dictionary = device_matches[0]
			device_badges.append(_badge("neutral", "native_phase=%d" % int(device_native_rec.get("phase", -1))))
			promoted_native_ids[int(device_native_rec.get("native_id", 0))] = true
		elif device_matches.size() > 1:
			device_info.append(
				"Contract ambiguity: device row has %d matching current-generation Device native objects."
				% device_matches.size()
			)
		panel.entries.append(_entry(
			device_entry_id,
			provider_id,
			2,
			device_label,
			true,
			true,
			device_badges,
			[
				_counter("mode", int(rec.get("mode", 0)), 1),
				_counter("errors", int(rec.get("errors_count", 0)), 1),
			],
			device_info
		))

	var streams_by_device := {}
	for i in range(streams.size()):
		var rec := _safe_dict(streams[i], issues, "streams[%d]" % i)
		if rec.is_empty():
			continue
		var stream_id := int(rec.get("stream_id", 0))
		var owner_instance := int(rec.get("device_instance_id", 0))
		if stream_id <= 0:
			issues.append("Contract gap: streams[%d] missing valid stream_id." % i)
			continue
		if owner_instance <= 0:
			issues.append("Contract gap: stream/%d missing owner device_instance_id." % stream_id)
			continue
		if not streams_by_device.has(owner_instance):
			streams_by_device[owner_instance] = []
		streams_by_device[owner_instance].append(rec)

	for instance_id in provider_device_ids_by_instance.keys():
		var parent_entry_id := str(provider_device_ids_by_instance[instance_id])
		var per_device_streams: Array = streams_by_device.get(instance_id, [])
		for rec in per_device_streams:
			var stream_id := int(rec.get("stream_id", 0))
			var stream_badges: Array[BadgeModel] = [_badge("neutral", "phase=%d" % int(rec.get("phase", -1)))]
			var stream_info: Array[String] = []
			var stream_matches: Array = current_stream_native_matches_by_stream_id.get(stream_id, [])
			if stream_matches.size() == 1:
				var stream_native_rec: Dictionary = stream_matches[0]
				stream_badges.append(_badge("neutral", "native_phase=%d" % int(stream_native_rec.get("phase", -1))))
				promoted_native_ids[int(stream_native_rec.get("native_id", 0))] = true
			elif stream_matches.size() > 1:
				stream_info.append(
					"Contract ambiguity: stream row has %d matching current-generation Stream native objects."
					% stream_matches.size()
				)
			panel.entries.append(_entry(
				"stream/%d" % stream_id,
				parent_entry_id,
				3,
				"stream/%d" % stream_id,
				false,
				true,
				stream_badges,
				[
					_counter("mode", int(rec.get("mode", 0)), 1),
					_counter("fps_max", int(rec.get("target_fps_max", 0)), 2),
					_counter("frames", int(rec.get("frames_received", 0)), 3),
				],
				stream_info
			))

	var current_grounded_destroyed_device_row_id_by_instance := {}
	for instance_key in current_non_live_device_native_matches_by_instance.keys():
		var canonical_device_id := "device/%d" % int(instance_key)
		if _entry_exists(panel.entries, canonical_device_id):
			continue
		var destroyed_device_matches: Array = current_non_live_device_native_matches_by_instance[instance_key]
		if destroyed_device_matches.size() == 1:
			var destroyed_device_native: Dictionary = destroyed_device_matches[0]
			var destroyed_device_native_id := int(destroyed_device_native.get("native_id", 0))
			if destroyed_device_native_id <= 0:
				issues.append("Contract ambiguity: current-generation DESTROYED Device native object missing valid native_id.")
				continue
			current_grounded_destroyed_device_row_id_by_instance[int(instance_key)] = canonical_device_id
			promoted_native_ids[destroyed_device_native_id] = true
			panel.entries.append(_entry(
				canonical_device_id,
				provider_id,
				2,
				"device/%d [destroyed]" % int(instance_key),
				true,
				true,
				[
					_badge("warning", "destroyed"),
					_badge("neutral", "native_phase=%d" % int(destroyed_device_native.get("phase", -1))),
				],
				[],
				[]
			))
		else:
			issues.append(
				"Contract ambiguity: current-generation non-live device owner_instance_id=%d has %d matching Device native objects."
				% [int(instance_key), destroyed_device_matches.size()]
			)

	for stream_key in current_non_live_stream_native_matches_by_stream_id.keys():
		var canonical_stream_id := "stream/%d" % int(stream_key)
		if _entry_exists(panel.entries, canonical_stream_id):
			continue
		var destroyed_stream_matches: Array = current_non_live_stream_native_matches_by_stream_id[stream_key]
		if destroyed_stream_matches.size() == 1:
			var destroyed_stream_native: Dictionary = destroyed_stream_matches[0]
			var destroyed_stream_native_id := int(destroyed_stream_native.get("native_id", 0))
			if destroyed_stream_native_id <= 0:
				issues.append("Contract ambiguity: current-generation DESTROYED Stream native object missing valid native_id.")
				continue
			var destroyed_stream_parent_id := provider_id
			var destroyed_stream_owner_instance := int(destroyed_stream_native.get("owner_device_instance_id", 0))
			if _entry_exists(panel.entries, "device/%d" % destroyed_stream_owner_instance):
				destroyed_stream_parent_id = "device/%d" % destroyed_stream_owner_instance
			promoted_native_ids[destroyed_stream_native_id] = true
			panel.entries.append(_entry(
				canonical_stream_id,
				destroyed_stream_parent_id,
				3 if destroyed_stream_parent_id != provider_id else 2,
				"stream/%d [destroyed]" % int(stream_key),
				true,
				true,
				[
					_badge("warning", "destroyed"),
					_badge("neutral", "native_phase=%d" % int(destroyed_stream_native.get("phase", -1))),
				],
				[],
				[]
			))
		else:
			issues.append(
				"Contract ambiguity: current-generation non-live stream owner_stream_id=%d has %d matching Stream native objects."
				% [int(stream_key), destroyed_stream_matches.size()]
			)
	for i in range(rigs.size()):
		var rec := _safe_dict(rigs[i], issues, "rigs[%d]" % i)
		if rec.is_empty():
			continue
		var rig_id := int(rec.get("rig_id", 0))
		if rig_id <= 0:
			issues.append("Contract gap: rigs[%d] missing valid rig_id." % i)
			continue
		var rig_entry_id := "rig/%d" % rig_id
		panel.entries.append(_entry(
			rig_entry_id,
			provider_id,
			2,
			"rig/%s" % _safe_rig_name(rec),
			false,
			true,
			[_badge("neutral", "phase=%d" % int(rec.get("phase", -1)))],
			[
				_counter("mode", int(rec.get("mode", 0)), 1),
				_counter("members", _safe_array(rec.get("member_hardware_ids", []), issues, "rig/%d.member_hardware_ids" % rig_id).size(), 1),
			],
			[]
		))

		var members := _safe_array(rec.get("member_hardware_ids", []), issues, "rig/%d.member_hardware_ids" % rig_id)
		for j in range(members.size()):
			var hardware_id := str(members[j])
			var member_device := _find_device_by_hardware(devices, hardware_id, issues)
			var member_info: Array[String] = []
			if member_device.is_empty():
				member_info.append("Contract gap: rig member hardware_id not present in devices list.")
			panel.entries.append(_entry(
				"rig/%d/device/%s" % [rig_id, _safe_slug(hardware_id, "unknown")],
				rig_entry_id,
				3,
				"device/%s" % _safe_label_component(hardware_id, "unknown"),
				false,
				false,
				[_badge("info", "rig-context")],
				[],
				member_info
			))

	var orphan_row_id := "%s/orphaned_native_objects" % provider_id
	var orphan_rows: Array[StatusEntryModel] = []
	for i in range(current_native_objects.size()):
		var rec := _safe_dict(current_native_objects[i], issues, "native_objects[current][%d]" % i)
		if rec.is_empty():
			continue
		var current_native_id := int(rec.get("native_id", 0))
		if _native_object_is_provider(rec) and current_native_id == provider_native_id:
			continue
		if bool(promoted_native_ids.get(current_native_id, false)):
			continue
		var native_entry := _build_native_object_entry(
			rec,
			provider_id,
			orphan_row_id,
			detached_root_ids,
			devices_by_instance,
			panel.entries,
			issues,
			snapshot_gen,
			false
		)
		if native_entry == null:
			continue
		if native_entry.parent_id == orphan_row_id:
			orphan_rows.append(native_entry)
		else:
			panel.entries.append(native_entry)

	if orphan_rows.size() > 0:
		panel.entries.append(_entry(
			orphan_row_id,
			provider_id,
			2,
			"orphaned native objects",
			true,
			true,
			[_badge("warning", "detached")],
			[_counter("roots", detached_root_ids.size(), 1)],
			[]
		))
		for row in orphan_rows:
			panel.entries.append(row)

	if prior_native_objects.size() > 0:
		var retained_group_id := "%s/retained_prior_generation_native_objects" % provider_id
		panel.entries.append(_entry(
			retained_group_id,
			provider_id,
			2,
			"retained prior-generation native objects",
			true,
			true,
			[_badge("warning", "retained")],
			[_counter("count", prior_native_objects.size(), 1)],
			[]
		))
		for i in range(prior_native_objects.size()):
			var rec := _safe_dict(prior_native_objects[i], issues, "native_objects[prior][%d]" % i)
			if rec.is_empty():
				continue
			var retained_entry := _build_native_object_entry(
				rec,
				provider_id,
				orphan_row_id,
				detached_root_ids,
				devices_by_instance,
				panel.entries,
				issues,
				snapshot_gen,
				true
			)
			if retained_entry == null:
				continue
			retained_entry.parent_id = retained_group_id
			retained_entry.depth = 3
			panel.entries.append(retained_entry)

	if issues.size() > 0:
		panel.entries.append(_entry(
			"%s/contract_gaps" % provider_id,
			provider_id,
			2,
			"contract_gaps",
			true,
			false,
			[_badge("warning", "schema")],
			[_counter("count", issues.size(), 1)],
			issues
		))

	_ensure_expandability(panel)
	return panel


func _native_object_is_provider(rec: Dictionary) -> bool:
	return _native_object_type_key(rec) == "provider"


#func _native_object_is_provider(rec: Dictionary) -> bool:
	#if not rec.has("type"):
		#return false
	#var type_value := rec.get("type")
	#if typeof(type_value) in [TYPE_INT, TYPE_FLOAT]:
		#return int(type_value) == 1
	#if typeof(type_value) == TYPE_STRING or typeof(type_value) == TYPE_STRING_NAME:
		#return str(type_value).to_lower() == "provider"
	#return false


func _partition_native_objects_by_generation(snapshot_gen: int, native_objects: Array, issues: Array[String]) -> Dictionary:
	var current: Array = []
	var prior: Array = []
	for i in range(native_objects.size()):
		var rec := _safe_dict(native_objects[i], issues, "native_objects[%d]" % i)
		if rec.is_empty():
			continue
		if _native_object_is_current_gen(rec, snapshot_gen, issues, "native_objects[%d]" % i):
			current.append(rec)
		else:
			prior.append(rec)
	return {"current": current, "prior": prior}


func _native_object_is_current_gen(rec: Dictionary, snapshot_gen: int, issues: Array[String], path: String) -> bool:
	if not rec.has("creation_gen"):
		issues.append("Contract gap: %s missing creation_gen; treating as prior-generation retained record." % path)
		return false
	var value := rec.get("creation_gen")
	if typeof(value) not in [TYPE_INT, TYPE_FLOAT]:
		issues.append("Contract gap: %s creation_gen has unexpected type=%d; treating as prior-generation retained record." % [path, typeof(value)])
		return false
	return int(value) == snapshot_gen


func _count_native_destroyed(native_objects: Array) -> int:
	var count := 0
	for item in native_objects:
		if typeof(item) != TYPE_DICTIONARY:
			continue
		if int((item as Dictionary).get("phase", -1)) == 3:
			count += 1
	return count


func _append_native_generation_note(info_lines: Array[String], rec: Dictionary, snapshot_gen: int) -> void:
	var creation_gen_text := "unknown"
	if rec.has("creation_gen"):
		creation_gen_text = str(rec.get("creation_gen"))
	info_lines.append("Retained record: creation_gen=%s, current snapshot.gen=%d." % [creation_gen_text, snapshot_gen])
	if int(rec.get("phase", -1)) == 3:
		info_lines.append("Retained DESTROYED record from prior generation.")


func _build_native_object_entry(
		rec: Dictionary,
		provider_id: String,
		orphan_row_id: String,
		detached_root_ids: Array,
		devices_by_instance: Dictionary,
		existing_entries: Array[StatusEntryModel],
		issues: Array[String],
		snapshot_gen: int,
		is_prior_generation: bool
	) -> StatusEntryModel:
	var native_id := int(rec.get("native_id", 0))
	if native_id <= 0:
		issues.append("Contract gap: native object missing valid native_id.")
		return null
	var owner_stream_id := int(rec.get("owner_stream_id", 0))
	var owner_device_instance_id := int(rec.get("owner_device_instance_id", 0))
	var root_id := int(rec.get("root_id", 0))
	var parent_id := provider_id
	var info_lines: Array[String] = []

	if owner_stream_id > 0:
		parent_id = "stream/%d" % owner_stream_id
		if not _entry_exists(existing_entries, parent_id):
			info_lines.append("Contract gap: owner stream not present in snapshot streams.")
			parent_id = orphan_row_id if _contains_int(detached_root_ids, root_id) else provider_id
	elif owner_device_instance_id > 0:
		parent_id = "device/%d" % owner_device_instance_id
		if not _entry_exists(existing_entries, parent_id):
			info_lines.append("Contract gap: owner device not present in snapshot devices.")
			parent_id = orphan_row_id if _contains_int(detached_root_ids, root_id) else provider_id
		else:
			var owner_device: Dictionary = devices_by_instance.get(owner_device_instance_id, {})
			if not owner_device.is_empty() and int(owner_device.get("rig_id", 0)) > 0:
				info_lines.append("Contract gap: native object may be rig-triggered but schema does not publish origin context.")
	elif _contains_int(detached_root_ids, root_id):
		parent_id = orphan_row_id
	else:
		info_lines.append("Contract gap: native object has no stream/device owner; placed under provider.")

	if is_prior_generation:
		_append_native_generation_note(info_lines, rec, snapshot_gen)

	var native_type_key := _native_object_type_key(rec)
	var row_id := "native_object/%d" % native_id
	var row_label := "native_object/%d" % native_id
	if native_type_key == "frameproducer":
		row_id = "frameproducer/%d" % native_id
		row_label = "frameproducer/%d" % native_id
		if owner_stream_id > 0:
			info_lines.append("FrameProducer owner_stream_id=%d." % owner_stream_id)
		if rec.has("creation_gen"):
			info_lines.append("FrameProducer creation_gen=%s." % str(rec.get("creation_gen")))

	var target_depth := _depth_for_parent(parent_id)
	var native_badges: Array[BadgeModel] = [_badge("neutral", "phase=%d" % int(rec.get("phase", -1)))]
	if native_type_key == "frameproducer":
		native_badges.append(_badge("info", "type=FrameProducer"))
	var native_counters: Array[CounterModel] = [
		_counter("bytes", int(rec.get("bytes_allocated", 0)), 3),
		_counter("buffers", int(rec.get("buffers_in_use", 0)), 2),
	]
	return _entry(
		row_id,
		parent_id,
		target_depth,
		row_label,
		false,
		false,
		native_badges,
		native_counters,
		info_lines
	)


func _ensure_expandability(panel: PanelModel) -> void:
	var child_counts := {}
	for e in panel.entries:
		if str(e.parent_id) == "":
			continue
		child_counts[e.parent_id] = int(child_counts.get(e.parent_id, 0)) + 1
	for e in panel.entries:
		e.can_expand = int(child_counts.get(e.id, 0)) > 0


func _depth_for_parent(parent_id: String) -> int:
	if parent_id.begins_with("stream/"):
		return 4
	if parent_id.begins_with("device/") or parent_id.begins_with("rig/"):
		return 3
	if parent_id.contains("orphaned_native_objects"):
		return 3
	if parent_id.begins_with("provider/"):
		return 2
	return 1


func _safe_array(value: Variant, issues: Array[String], path: String) -> Array:
	if typeof(value) == TYPE_ARRAY:
		return value
	issues.append("Contract gap: %s expected Array, got type=%d." % [path, typeof(value)])
	return []


func _safe_dict(value: Variant, issues: Array[String], path: String) -> Dictionary:
	if typeof(value) == TYPE_DICTIONARY:
		return value
	issues.append("Contract gap: %s expected Dictionary, got type=%d." % [path, typeof(value)])
	return {}


func _safe_device_name(rec: Dictionary) -> String:
	return _safe_label_component(str(rec.get("hardware_id", "")), "unknown")


func _safe_rig_name(rec: Dictionary) -> String:
	var name := str(rec.get("name", ""))
	if name.is_empty():
		return str(rec.get("rig_id", 0))
	return name


func _safe_label_component(raw: String, fallback: String) -> String:
	if raw.is_empty():
		return fallback
	return raw


func _safe_slug(raw: String, fallback: String) -> String:
	if raw.is_empty():
		return fallback
	return raw.replace("/", "-").replace(" ", "-")


func _entry_exists(entries: Array[StatusEntryModel], id: String) -> bool:
	for e in entries:
		if e.id == id:
			return true
	return false


func _contains_int(values: Array, needle: int) -> bool:
	for value in values:
		if int(value) == needle:
			return true
	return false


func _find_device_by_hardware(devices: Array, hardware_id: String, issues: Array[String]) -> Dictionary:
	for i in range(devices.size()):
		var rec := _safe_dict(devices[i], issues, "devices[%d]" % i)
		if str(rec.get("hardware_id", "")) == hardware_id:
			return rec
	return {}


func _render_panel_model(model: PanelModel) -> void:
	if model == null:
		push_warning("CamBANGStatusPanel: null PanelModel render request ignored.")
		return
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
	if _last_panel_model != null:
		_render_panel_model(_last_panel_model)
	else:
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



func _native_object_type_key(rec: Dictionary) -> String:
	if not rec.has("type"):
		return ""
	var type_value = rec.get("type")
	if typeof(type_value) in [TYPE_INT, TYPE_FLOAT]:
		match int(type_value):
			1:
				return "provider"
			2:
				return "device"
			3:
				return "stream"
			4:
				return "frameproducer"
			_:
				return ""
	if typeof(type_value) == TYPE_STRING or typeof(type_value) == TYPE_STRING_NAME:
		var s := str(type_value).strip_edges().to_lower()
		match s:
			"provider":
				return "provider"
			"device":
				return "device"
			"stream":
				return "stream"
			"frameproducer", "frame_producer", "frame producer":
				return "frameproducer"
			_:
				return ""
	return ""
