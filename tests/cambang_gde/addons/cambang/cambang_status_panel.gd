@tool
class_name CamBANGStatusPanel
extends PanelContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"
const SUPPORTED_SCHEMA_VERSION := 1
const STATUS_ENTRY_SCENE := preload("res://addons/cambang/internal/status_entry.tscn")
const TOUCH_SCROLL_SCRIPT := preload("res://addons/cambang/internal/touch_scroll_container.gd")
# PROVISIONAL: placeholder until runtime exports authoritative retention-window policy.
const PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC := 5000
const RETAINED_PRESENTATION_ROOT_ID := "retained_presentation/prior_authoritative"
const DEBUG_EVIDENCE_ENV := "CAMBANG_STATUS_PANEL_DEBUG_DUMP"
const DEBUG_DISCLOSURE_ENV := "CAMBANG_STATUS_PANEL_DEBUG_DISCLOSURE"


class PanelModel extends RefCounted:
	var entries: Array[StatusEntryModel] = []


class StatusEntryModel extends RefCounted:
	var id: String = ""
	var parent_id: String = ""
	var depth: int = 0
	var label: String = ""
	var visual_object_class: String = ""
	var materialized_native_id: int = 0
	var expanded: bool = false
	var can_expand: bool = false
	var badges: Array[BadgeModel] = []
	var counters: Array[CounterModel] = []
	var info_lines: Array[String] = []
	var summary_info_lines: Array[String] = []
	var detail_info_lines: Array[String] = []
	var anomaly_info_lines: Array[String] = []


class BadgeModel extends RefCounted:
	var role: String = "neutral"
	var label: String = ""
	var kind: String = ""


class CounterModel extends RefCounted:
	var name: String = ""
	var value: int = 0
	var text_value: String = ""
	var digits: int = 1
	var visibility: String = "core"


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


class RetainedSubtreeState extends RefCounted:
	var panel_model: PanelModel = null
	var retained_from_gen: int = -1
	var source_snapshot_timestamp_ns: int = -1
	var source_snapshot_version: int = -1
	var source_topology_version: int = -1
	var retained_at_msec: int = -1
	var provider_root_id: String = ""
	var provider_identity_hint: String = ""
	var root_status: String = "orphaned"



@export var panel_style_overrides: Dictionary = {}
@export var shared_style_overrides: Dictionary = {}

var _title_label: Label
var _provider_mode_value: Label
var _schema_version_value: Label
var _counts_value: Label
var _timestamp_value: Label
var _status_rows_scroll: ScrollContainer
var _status_rows: VBoxContainer
var _expanded_by_row_id: Dictionary = {}
var _dev_parent_by_id: Dictionary = {}
var _row_nodes_by_id: Dictionary = {}
var _server: Object = null
var _last_snapshot_meta: Dictionary = {}
var _last_panel_model: PanelModel = null
var _last_active_panel_model: PanelModel = null
var _last_active_panel_is_authoritative: bool = false
var _last_active_snapshot_meta: Dictionary = {}
var _last_authoritative_panel_model: PanelModel = null
var _last_authoritative_snapshot_meta: Dictionary = {}
var _retained_subtrees: Array[RetainedSubtreeState] = []


func _ready() -> void:
	set_process(true)
	_build_ui_if_needed()
	_apply_panel_style(_resolve_style())
	_connect_server()
	_refresh_from_server()


func _enter_tree() -> void:
	set_process(true)
	if is_node_ready():
		_build_ui_if_needed()
		_apply_panel_style(_resolve_style())
		_connect_server()
		_refresh_from_server()


func _exit_tree() -> void:
	_disconnect_server()


func _process(_delta: float) -> void:
	if _reconcile_post_stop_nil_boundary():
		return
	if _last_active_panel_model == null:
		return
	if _retained_subtrees.is_empty():
		return
	if not _expire_retained_subtrees_by_policy():
		return
	_last_panel_model = _compose_presented_panel_model(
		_last_active_panel_model,
		_last_active_panel_is_authoritative,
		_last_active_snapshot_meta,
		false
	)
	var snapshot_for_render: Variant = null
	if _last_active_panel_is_authoritative:
		snapshot_for_render = _fetch_snapshot()
	_render_panel_and_maybe_dump(_last_panel_model, snapshot_for_render)


func _reconcile_post_stop_nil_boundary() -> bool:
	if not _last_active_panel_is_authoritative:
		return false
	if _server == null:
		_server = _get_server()
	if _server == null:
		return false

	var snapshot := _fetch_snapshot()
	if snapshot != null:
		return false

	_last_snapshot_meta.clear()
	_apply_snapshot_read(_read_snapshot(null))
	var nil_panel := _build_nil_panel_model("No published snapshot yet.")
	_set_last_active_panel_state(nil_panel, false, {})
	_last_panel_model = _compose_presented_panel_model(nil_panel, false, {})
	_render_panel_and_maybe_dump(_last_panel_model, null)
	return true


func force_refresh() -> void:
	_refresh_from_server()


func apply_fixture_expanded_rows(row_ids: Array) -> void:
	for raw_row_id in row_ids:
		var row_id := str(raw_row_id)
		if row_id.is_empty():
			continue
		_expanded_by_row_id[row_id] = true


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
		_set_last_active_panel_state(no_server_panel, false, {})
		_last_panel_model = _compose_presented_panel_model(no_server_panel, false, {})
		_render_panel_and_maybe_dump(_last_panel_model, null)
		return

	var provider_mode := str(_server.get_provider_mode())
	_provider_mode_value.text = provider_mode
	var snapshot := _fetch_snapshot()
	var reading := _read_snapshot(snapshot)
	_apply_snapshot_read(reading)

	if snapshot == null:
		_last_snapshot_meta.clear()
		var nil_panel := _build_nil_panel_model("No published snapshot yet.")
		_set_last_active_panel_state(nil_panel, false, {})
		_last_panel_model = _compose_presented_panel_model(nil_panel, false, {})
		_render_panel_and_maybe_dump(_last_panel_model, null)
		return

	if typeof(snapshot) != TYPE_DICTIONARY:
		_last_snapshot_meta.clear()
		var bad_type_panel := _build_nil_panel_model("Contract gap: snapshot must be Dictionary; got type=%d." % typeof(snapshot))
		_set_last_active_panel_state(bad_type_panel, false, {})
		_last_panel_model = _compose_presented_panel_model(bad_type_panel, false, {})
		_render_panel_and_maybe_dump(_last_panel_model, snapshot)
		return

	var compat := _check_snapshot_runtime_compat(snapshot)
	if not bool(compat.get("ok", false)):
		_last_snapshot_meta.clear()
		var compat_panel := _build_runtime_compat_fallback_panel(
			compat.get("contract_gaps", []),
			compat.get("projection_gaps", [])
		)
		_set_last_active_panel_state(compat_panel, false, {})
		_last_panel_model = _compose_presented_panel_model(compat_panel, false, {})
		_render_panel_and_maybe_dump(_last_panel_model, snapshot)
		return

	var snapshot_meta := _extract_authoritative_snapshot_meta(snapshot)
	var category := _categorize_snapshot_update(snapshot)
	match category:
		"none":
			if _last_active_panel_model == null:
				_set_last_active_panel_state(
					_project_snapshot_to_panel_model(snapshot, provider_mode),
					true,
					snapshot_meta
				)
			_last_panel_model = _compose_presented_panel_model(_last_active_panel_model, true, snapshot_meta)
			_render_panel_and_maybe_dump(_last_panel_model, snapshot)
		"value_refresh":
			var value_panel := _project_snapshot_to_panel_model(snapshot, provider_mode)
			_set_last_active_panel_state(value_panel, true, snapshot_meta)
			_last_panel_model = _compose_presented_panel_model(value_panel, true, snapshot_meta)
			_render_panel_and_maybe_dump(_last_panel_model, snapshot, "value_refresh")
		_:
			var full_panel := _project_snapshot_to_panel_model(snapshot, provider_mode)
			_set_last_active_panel_state(full_panel, true, snapshot_meta)
			_last_panel_model = _compose_presented_panel_model(full_panel, true, snapshot_meta)
			_render_panel_and_maybe_dump(_last_panel_model, snapshot)


func _render_panel_and_maybe_dump(
		model: PanelModel,
		snapshot: Variant,
		update_category: String = "structural_rebuild"
	) -> void:
	_render_panel_model(model, update_category)
	var model_changed := false
	if _apply_render_native_coverage_summary(snapshot, model):
		model_changed = true
	if _apply_server_health_summary(model):
		model_changed = true
	if model_changed:
		_render_panel_model(model, update_category)
	_debug_dump_runtime_evidence_if_enabled(snapshot, model)


func _debug_dump_runtime_evidence_if_enabled(snapshot: Variant, model: PanelModel) -> void:
	if not OS.has_environment(DEBUG_EVIDENCE_ENV):
		return
	var env_value := OS.get_environment(DEBUG_EVIDENCE_ENV).strip_edges().to_lower()
	if not ["1", "true", "yes", "on"].has(env_value):
		return
	print(_build_debug_runtime_evidence_dump(snapshot, model))


func _build_debug_runtime_evidence_dump(snapshot: Variant, model: PanelModel) -> String:
	var lines: Array[String] = []
	lines.append("=== CAMBANG STATUS PANEL DEBUG EVIDENCE BEGIN ===")
	lines.append("SNAPSHOT_NATIVE_OBJECTS:")
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
		lines.append("  <snapshot unavailable>")
	else:
		var snapshot_dict := snapshot as Dictionary
		var native_objects := snapshot_dict.get("native_objects", [])
		if typeof(native_objects) != TYPE_ARRAY or native_objects.is_empty():
			lines.append("  <none>")
		else:
			for raw_rec in native_objects:
				if typeof(raw_rec) != TYPE_DICTIONARY:
					lines.append("  <non-dictionary native_object>")
					continue
				var rec := raw_rec as Dictionary
				lines.append(
					"  native_id=%d type=%s phase=%s creation_gen=%s owner_device_instance_id=%s owner_stream_id=%s owner_provider_native_id=%s root_id=%s"
					% [
						int(rec.get("native_id", 0)),
						str(rec.get("type", "")),
						str(rec.get("phase", "")),
						str(rec.get("creation_gen", "")),
						str(rec.get("owner_device_instance_id", "")),
						str(rec.get("owner_stream_id", "")),
						str(rec.get("owner_provider_native_id", "")),
						str(rec.get("root_id", "")),
					]
				)
	lines.append("SNAPSHOT_NATIVE_IDS: %s" % [_sorted_int_keys(_snapshot_native_records(snapshot).keys())])
	lines.append("PROJECTION_ROWS:")
	if model == null:
		lines.append("  <model unavailable>")
	else:
		for entry in model.entries:
			if entry == null:
				continue
			lines.append(
				"  row_id=%s parent_id=%s label=%s materialized_native_id=%d"
				% [entry.id, entry.parent_id, entry.label, int(entry.materialized_native_id)]
			)
	lines.append("PROJECTED_NATIVE_IDS: %s" % [_collect_projected_native_ids(model)])
	lines.append("RENDERED_NATIVE_IDS: %s" % [_collect_rendered_native_ids(model)])
	lines.append("VISIBLE_NATIVE_IDS: %s" % [_collect_visible_native_ids(model)])
	lines.append("RENDERED_ROW_IDS: %s" % [_collect_debug_rendered_row_ids()])
	lines.append("VISIBLE_ROW_IDS: %s" % [_collect_debug_visible_row_ids()])
	lines.append("=== CAMBANG STATUS PANEL DEBUG EVIDENCE END ===")
	return "\n".join(lines)


func _collect_debug_rendered_row_ids() -> Array[String]:
	var row_ids: Array[String] = []
	if _status_rows == null:
		return row_ids
	for child in _status_rows.get_children():
		if child == null:
			continue
		var entry_id := _extract_debug_row_entry_id(child)
		if entry_id != "":
			row_ids.append(entry_id)
	return row_ids


func _collect_debug_visible_row_ids() -> Array[String]:
	var visible_ids: Array[String] = []
	if _status_rows == null:
		return visible_ids
	for child in _status_rows.get_children():
		if child == null or not bool(child.visible):
			continue
		var entry_id := _extract_debug_row_entry_id(child)
		if entry_id != "":
			visible_ids.append(entry_id)
	return visible_ids


func _apply_render_native_coverage_summary(snapshot: Variant, model: PanelModel) -> bool:
	if model == null:
		return false
	var server_entry := _find_panel_entry_by_id(model, "server/main")
	if server_entry == null:
		return false

	var coverage := _compute_global_native_coverage(snapshot, model)
	var next_badges := _badges_with_render_native_coverage(server_entry.badges, coverage)
	var next_info_lines := _info_lines_with_render_native_coverage(server_entry.info_lines, coverage)

	var next_badge_labels := _badge_labels(next_badges)
	var current_badge_labels := _badge_labels(server_entry.badges)
	var changed := next_badge_labels != current_badge_labels or next_info_lines != server_entry.info_lines
	if not changed:
		return false

	server_entry.badges = next_badges
	server_entry.info_lines = next_info_lines
	_apply_detail_policy_to_entry(server_entry)
	return true


func _compute_global_native_coverage(snapshot: Variant, model: PanelModel) -> Dictionary:
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY or model == null:
		return {
			"state": "UNKNOWN",
			"role": "neutral",
		}

	var snapshot_dict := snapshot as Dictionary
	var snapshot_gen := int(snapshot_dict.get("gen", -1))
	var native_objects := snapshot_dict.get("native_objects", [])
	if typeof(native_objects) != TYPE_ARRAY:
		return {
			"state": "UNKNOWN",
			"role": "neutral",
		}
	var rendered_native_ids := _collect_rendered_native_ids(model)
	return _compute_native_coverage_from_ids(native_objects, rendered_native_ids, snapshot_gen)


func _badges_with_render_native_coverage(existing_badges: Array[BadgeModel], coverage: Dictionary) -> Array[BadgeModel]:
	var next_badges: Array[BadgeModel] = []
	for badge in existing_badges:
		if badge == null:
			continue
		if badge.label.begins_with("NATIVE COVERAGE:"):
			continue
		next_badges.append(_badge(badge.role, badge.label, badge.kind))
	var state_label := str(coverage.get("state", "UNKNOWN"))
	var state_role := str(coverage.get("role", "neutral"))
	next_badges.append(_badge(state_role, "NATIVE COVERAGE: %s" % state_label))
	return _normalize_badges(next_badges)


func _info_lines_with_render_native_coverage(existing_info_lines: Array[String], coverage: Dictionary) -> Array[String]:
	var next_info_lines: Array[String] = []
	for line in existing_info_lines:
		if line.begins_with("native coverage:"):
			continue
		if line.begins_with("Projection gap: native coverage"):
			continue
		next_info_lines.append(line)

	var state := str(coverage.get("state", "UNKNOWN"))
	if state == "UNKNOWN":
		return next_info_lines

	var total_native := int(coverage.get("total", 0))
	var rendered_native := int(coverage.get("rendered", 0))
	var missing_native := int(coverage.get("missing", 0))
	if missing_native > 0:
		next_info_lines.append(
			"Projection gap: native coverage %d/%d rendered (missing=%d)."
			% [
				rendered_native,
				total_native,
				missing_native,
			]
		)
	return next_info_lines


func _badge_labels(badges: Array[BadgeModel]) -> Array[String]:
	var labels: Array[String] = []
	for badge in badges:
		if badge == null:
			continue
		labels.append("%s|%s" % [badge.role, badge.label])
	return labels


func _apply_server_health_summary(model: PanelModel) -> bool:
	if model == null:
		return false
	var server_entry := _find_panel_entry_by_id(model, "server/main")
	if server_entry == null:
		return false
	var next_health_label := _derive_server_health_label(model, server_entry)
	var next_health_role := _health_role_for_label(next_health_label)
	var next_badges := _with_health_badge(server_entry.badges, next_health_role, next_health_label)
	var next_badge_labels := _badge_labels(next_badges)
	var current_badge_labels := _badge_labels(server_entry.badges)
	if next_badge_labels == current_badge_labels:
		return false
	server_entry.badges = next_badges
	_apply_detail_policy_to_entry(server_entry)
	return true


func _derive_server_health_label(model: PanelModel, server_entry: StatusEntryModel) -> String:
	# Priority order: BAD > UNKNOWN > ATTN > OK
	if _server_has_contract_or_projection_failure(model, server_entry):
		return "BAD"
	if _server_has_badge_label(server_entry, "NO SNAPSHOT"):
		return "UNKNOWN"
	var native_coverage_state := _server_native_coverage_state(server_entry)
	if native_coverage_state == "OK":
		return "OK"
	if native_coverage_state == "UNKNOWN":
		return "UNKNOWN"
	if native_coverage_state != "":
		return "ATTN"
	return "UNKNOWN"


func _server_has_contract_or_projection_failure(model: PanelModel, server_entry: StatusEntryModel) -> bool:
	var native_coverage_state := _server_native_coverage_state(server_entry)
	if native_coverage_state == "MISSING":
		return true
	if _server_has_badge_label(server_entry, "contract-gap"):
		return true
	if _server_has_badge_label(server_entry, "CONTRACT GAP"):
		return true
	if _server_has_badge_label(server_entry, "snapshot-incompatible"):
		return true
	if _entry_exists(model.entries, "server/main/contract_gaps"):
		return true
	if _entry_exists(model.entries, "server/main/projection_gaps"):
		return true
	return false


func _server_native_coverage_state(server_entry: StatusEntryModel) -> String:
	for badge in server_entry.badges:
		if badge == null:
			continue
		if not badge.label.begins_with("NATIVE COVERAGE:"):
			continue
		return badge.label.substr("NATIVE COVERAGE: ".length()).strip_edges()
	return ""


func _server_has_badge_label(server_entry: StatusEntryModel, label: String) -> bool:
	for badge in server_entry.badges:
		if badge == null:
			continue
		if badge.label == label:
			return true
	return false


func _health_role_for_label(label: String) -> String:
	match label:
		"OK":
			return "success"
		"ATTN":
			return "warning"
		"BAD":
			return "error"
		_:
			return "neutral"


func _with_health_badge(
		badges: Array[BadgeModel],
		health_role: String,
		health_label: String
	) -> Array[BadgeModel]:
	var next_badges: Array[BadgeModel] = [_badge(health_role, health_label, "health")]
	for badge in badges:
		if badge == null:
			continue
		if badge.kind == "health":
			continue
		next_badges.append(_badge(badge.role, badge.label, badge.kind))
	return next_badges


func _snapshot_native_records(snapshot: Variant) -> Dictionary:
	var records := {}
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
		return records
	var native_objects := (snapshot as Dictionary).get("native_objects", [])
	if typeof(native_objects) != TYPE_ARRAY:
		return records
	for item in native_objects:
		if typeof(item) != TYPE_DICTIONARY:
			continue
		var rec := item as Dictionary
		var native_id := int(rec.get("native_id", 0))
		if native_id <= 0:
			continue
		records[native_id] = rec
	return records


func _collect_projected_native_ids(model: PanelModel) -> Array[int]:
	var ids := {}
	if model == null:
		return []
	for entry in model.entries:
		if entry == null:
			continue
		var native_id := int(entry.materialized_native_id)
		if native_id <= 0:
			continue
		ids[native_id] = true
	return _sorted_int_keys(ids.keys())


func _collect_rendered_native_ids(model: PanelModel) -> Array[int]:
	var ids := {}
	if model == null or _status_rows == null:
		return []
	var entries_by_id := _entries_by_id(model)
	for row_id in _collect_debug_rendered_row_ids():
		var entry: StatusEntryModel = entries_by_id.get(row_id, null)
		if entry == null:
			continue
		var native_id := int(entry.materialized_native_id)
		if native_id <= 0:
			continue
		ids[native_id] = true
	return _sorted_int_keys(ids.keys())


func _collect_visible_native_ids(model: PanelModel) -> Array[int]:
	var ids := {}
	if model == null or _status_rows == null:
		return []
	var entries_by_id := _entries_by_id(model)
	for row_id in _collect_debug_visible_row_ids():
		var entry: StatusEntryModel = entries_by_id.get(row_id, null)
		if entry == null:
			continue
		var native_id := int(entry.materialized_native_id)
		if native_id <= 0:
			continue
		ids[native_id] = true
	return _sorted_int_keys(ids.keys())


func _entries_by_id(model: PanelModel) -> Dictionary:
	var by_id := {}
	if model == null:
		return by_id
	for entry in model.entries:
		if entry == null:
			continue
		by_id[entry.id] = entry
	return by_id


func _sorted_int_keys(raw_keys: Array) -> Array[int]:
	var out: Array[int] = []
	for raw_key in raw_keys:
		out.append(int(raw_key))
	out.sort()
	return out


func _extract_debug_row_entry_id(child: Variant) -> String:
	if child == null:
		return ""
	if child.has_method("get_entry_id"):
		return str(child.call("get_entry_id"))
	return str(child.get("_entry_id"))


func _set_last_active_panel_state(active_panel: PanelModel, is_authoritative_snapshot: bool, snapshot_meta: Dictionary) -> void:
	_last_active_panel_model = active_panel
	_last_active_panel_is_authoritative = is_authoritative_snapshot
	_last_active_snapshot_meta = snapshot_meta.duplicate(true)


func _is_provider_pending_panel(panel_model: PanelModel) -> bool:
	if panel_model == null:
		return false
	var has_provider_pending := false
	var has_provider_row := false
	for entry in panel_model.entries:
		if entry == null:
			continue
		if entry.id == "server/main/provider_pending":
			has_provider_pending = true
		elif entry.id.begins_with("provider/"):
			has_provider_row = true
	return has_provider_pending and not has_provider_row


func _is_retained_eligible(panel_model: PanelModel) -> bool:
	return not _is_provider_pending_panel(panel_model)


func _compose_presented_panel_model(
		active_panel: PanelModel,
		is_authoritative_snapshot: bool,
		snapshot_meta: Dictionary,
		update_retained_lifecycle: bool = true
	) -> PanelModel:
	if update_retained_lifecycle:
		_update_retained_lifecycle(active_panel, is_authoritative_snapshot, snapshot_meta)
	_reconcile_retained_subtrees(active_panel, is_authoritative_snapshot)
	_expire_retained_subtrees_by_policy()

	var composed := _clone_panel_model(active_panel)
	_append_retained_presentation_subtrees(composed)
	var projection_issues := _validate_projection_invariants(composed)
	_append_projection_gaps_row(composed, projection_issues)
	_reorder_panel_entries_depth_first(composed)
	_apply_detail_policy_to_panel(composed)
	return composed


func _validate_projection_invariants(panel: PanelModel) -> Array[String]:
	var issues: Array[String] = []
	if panel == null:
		issues.append("Projection invariant: composed panel model is null.")
		return issues

	var by_id := {}
	for entry in panel.entries:
		if not by_id.has(entry.id):
			by_id[entry.id] = []
		by_id[entry.id].append(entry)

	var server_rows: Array = by_id.get("server/main", [])
	if server_rows.size() != 1:
		issues.append("Projection invariant: expected exactly one server/main row, found %d." % server_rows.size())
	else:
		var server_row: StatusEntryModel = server_rows[0]
		if server_row.parent_id != "":
			issues.append("Projection invariant: server/main must be a root row.")

	for entry in panel.entries:
		if not _is_retained_projection_entry(entry.id):
			continue
		if entry.id.ends_with("/server/main"):
			issues.append("Projection invariant: retained projection must not include server/main as retained content.")
			break

	var retained_roots: Array[StatusEntryModel] = []
	for entry in panel.entries:
		if _has_badge_label(entry, "retained-root"):
			retained_roots.append(entry)
			if entry.parent_id != "server/main":
				issues.append("Projection invariant: retained root %s must be a direct child of server/main." % entry.id)

	var expected_retained_root_count := _retained_subtrees.size()
	if retained_roots.size() != expected_retained_root_count:
		issues.append(
			"Projection invariant: retained root row count (%d) does not match retained state count (%d)."
			% [retained_roots.size(), expected_retained_root_count]
		)

	var id_to_entry := {}
	for entry in panel.entries:
		id_to_entry[entry.id] = entry

	var active_provider_ids := {}
	for entry in panel.entries:
		if entry.parent_id == "server/main" and entry.id.begins_with("provider/") and not _has_badge_label(entry, "retained-root"):
			active_provider_ids[entry.id] = true

	for entry in panel.entries:
		var current_parent := entry.parent_id
		while current_parent != "":
			var parent_entry: StatusEntryModel = id_to_entry.get(current_parent, null)
			if parent_entry == null:
				break
			if _has_badge_label(parent_entry, "retained-root") and not _is_retained_projection_entry(entry.id) and entry.id != parent_entry.id:
				issues.append("Projection invariant: active row %s appears under retained root %s." % [entry.id, parent_entry.id])
				break
			if active_provider_ids.has(parent_entry.id) and _is_retained_projection_entry(entry.id):
				issues.append("Projection invariant: retained row %s appears inside active provider tree %s." % [entry.id, parent_entry.id])
				break
			current_parent = parent_entry.parent_id

	if _retained_subtrees.size() > 1:
		for i in range(1, _retained_subtrees.size()):
			var newer := _retained_subtrees[i - 1]
			var older := _retained_subtrees[i]
			if newer.retained_from_gen >= 0 and older.retained_from_gen >= 0 and older.retained_from_gen > newer.retained_from_gen:
				issues.append("Projection invariant: retained generations are not ordered newest-first.")
				break

	for retained_entry in panel.entries:
		if retained_entry.parent_id != "server/main":
			continue
		if not retained_entry.label.begins_with("retained_orphan/gen_"):
			continue
		var orphan_gen := _parse_orphan_retained_gen(retained_entry.label)
		if orphan_gen < 0:
			continue
		for retained_state in _retained_subtrees:
			if retained_state.retained_from_gen == orphan_gen and retained_state.root_status == "destroyed_provider":
				issues.append(
					"Projection invariant: orphan retained root used for gen=%d despite truthful destroyed provider root availability."
					% orphan_gen
				)
				break

	return issues


func _append_projection_gaps_row(panel: PanelModel, issues: Array[String]) -> void:
	if panel == null or issues.is_empty():
		return
	if _entry_exists(panel.entries, "server/main/projection_gaps"):
		return
	panel.entries.append(_entry(
		"server/main/projection_gaps",
		"server/main",
		1,
		"projection_gaps",
		true,
		false,
		[_badge("warning", "projection")],
		[_counter("count", issues.size(), 1)],
		issues
	))
	_ensure_expandability(panel)


func _has_badge_label(entry: StatusEntryModel, label: String) -> bool:
	for badge in entry.badges:
		if badge.label == label:
			return true
	return false


func _is_retained_projection_entry(entry_id: String) -> bool:
	return entry_id.begins_with("%s/" % RETAINED_PRESENTATION_ROOT_ID)


func _parse_orphan_retained_gen(label: String) -> int:
	var prefix := "retained_orphan/gen_"
	if not label.begins_with(prefix):
		return -1
	var suffix := label.substr(prefix.length())
	if suffix.is_empty() or not suffix.is_valid_int():
		return -1
	return int(suffix)


func _update_retained_lifecycle(active_panel: PanelModel, is_authoritative_snapshot: bool, snapshot_meta: Dictionary) -> void:
	if not is_authoritative_snapshot:
		_consume_last_authoritative_into_retained()
		return

	var active_gen := int(snapshot_meta.get("gen", -1))
	var last_gen := int(_last_authoritative_snapshot_meta.get("gen", -1))
	if _last_authoritative_panel_model != null and last_gen >= 0 and active_gen >= 0 and active_gen != last_gen:
		_consume_last_authoritative_into_retained()

	if _is_retained_eligible(active_panel):
		_last_authoritative_panel_model = _clone_panel_model(active_panel)
		_last_authoritative_snapshot_meta = snapshot_meta.duplicate(true)
	else:
		_last_authoritative_panel_model = null
		_last_authoritative_snapshot_meta.clear()


func _consume_last_authoritative_into_retained() -> void:
	if _last_authoritative_panel_model == null or _last_authoritative_snapshot_meta.is_empty():
		return
	_add_retained_subtree_if_new(_last_authoritative_panel_model, _last_authoritative_snapshot_meta)
	_last_authoritative_panel_model = null
	_last_authoritative_snapshot_meta.clear()


func _add_retained_subtree_if_new(source_model: PanelModel, source_meta: Dictionary) -> void:
	if source_model == null or source_meta.is_empty():
		return
	if _retained_subtree_exists(source_meta):
		return

	var retained := RetainedSubtreeState.new()
	retained.retained_from_gen = int(source_meta.get("gen", -1))
	retained.source_snapshot_timestamp_ns = int(source_meta.get("timestamp_ns", -1))
	retained.source_snapshot_version = int(source_meta.get("version", -1))
	retained.source_topology_version = int(source_meta.get("topology_version", -1))
	retained.retained_at_msec = Time.get_ticks_msec()
	var root_info := _resolve_retained_provider_root(source_model)
	retained.provider_root_id = str(root_info.get("provider_root_id", ""))
	retained.root_status = str(root_info.get("root_status", "orphaned"))
	retained.panel_model = _extract_retained_panel_subtree(source_model, retained.provider_root_id, retained.root_status)
	retained.provider_identity_hint = _resolve_retained_provider_identity_hint(retained.panel_model, retained.provider_root_id)
	_retained_subtrees.insert(0, retained)
	_reconcile_retained_subtrees_for_supersession()


func _retained_root_strength(root_status: String) -> int:
	match root_status:
		"destroyed_provider":
			return 3
		"ambiguous_provider":
			return 2
		"orphaned":
			return 1
		_:
			return 0


func _resolve_retained_provider_identity_hint(panel: PanelModel, provider_root_id: String) -> String:
	if not provider_root_id.is_empty():
		return provider_root_id
	if panel == null:
		return ""

	var provider_ids: Array[String] = []
	var seen := {}
	for entry in panel.entries:
		if entry == null:
			continue
		if not entry.id.begins_with("provider/"):
			continue
		if seen.has(entry.id):
			continue
		seen[entry.id] = true
		provider_ids.append(entry.id)

	if provider_ids.size() == 1:
		return provider_ids[0]
	return ""


func _retained_family_key_for_gen(retained_from_gen: int) -> String:
	return "gen=%d" % retained_from_gen


func _retained_family_key(retained: RetainedSubtreeState) -> String:
	if retained == null:
		return _retained_family_key_for_gen(-1)
	return _retained_family_key_for_gen(retained.retained_from_gen)


func _retained_primary_key(retained: RetainedSubtreeState) -> String:
	var family_key := _retained_family_key(retained)
	if retained == null:
		return family_key
	var provider_hint := str(retained.provider_identity_hint)
	if provider_hint.is_empty():
		return family_key
	return "%s|provider=%s" % [family_key, provider_hint]


func _reconcile_retained_subtrees(active_panel: PanelModel, is_authoritative_snapshot: bool) -> void:
	var authoritative_destroyed_provider_keys := {}
	if is_authoritative_snapshot:
		authoritative_destroyed_provider_keys = _collect_authoritative_destroyed_provider_reconciliation_keys(active_panel)
	_replace_retained_subtrees_with_reconciled_keys(authoritative_destroyed_provider_keys)


func _reconcile_retained_subtrees_for_supersession() -> bool:
	return _replace_retained_subtrees_with_reconciled_keys({})


func _replace_retained_subtrees_with_reconciled_keys(authoritative_destroyed_provider_keys: Dictionary) -> bool:
	var reconciled := _build_reconciled_retained_subtrees(authoritative_destroyed_provider_keys)
	var changed := reconciled.size() != _retained_subtrees.size()
	if not changed:
		for i in range(reconciled.size()):
			if reconciled[i] != _retained_subtrees[i]:
				changed = true
				break
	_retained_subtrees = reconciled
	return changed


func _build_reconciled_retained_subtrees(authoritative_destroyed_provider_keys: Dictionary) -> Array[RetainedSubtreeState]:
	var groups_by_family := {}
	var family_order: Array[String] = []
	for retained in _retained_subtrees:
		if retained == null:
			continue
		var family_key := _retained_family_key(retained)
		if not groups_by_family.has(family_key):
			groups_by_family[family_key] = []
			family_order.append(family_key)
		groups_by_family[family_key].append(retained)

	var reconciled: Array[RetainedSubtreeState] = []
	for family_key in family_order:
		if authoritative_destroyed_provider_keys.has(family_key):
			continue
		var group := _typed_retained_subtree_group(groups_by_family.get(family_key, []))
		var selected := _select_visible_retained_subtrees_for_family(
			group,
			authoritative_destroyed_provider_keys
		)
		for retained in selected:
			reconciled.append(retained)
	return reconciled


func _typed_retained_subtree_group(group_value: Variant) -> Array[RetainedSubtreeState]:
	var typed_group: Array[RetainedSubtreeState] = []
	if typeof(group_value) != TYPE_ARRAY:
		return typed_group
	for item in group_value:
		if item is RetainedSubtreeState:
			typed_group.append(item)
	return typed_group


func _select_visible_retained_subtrees_for_family(
		group: Array[RetainedSubtreeState],
		authoritative_destroyed_provider_keys: Dictionary
	) -> Array[RetainedSubtreeState]:
	var best_by_primary := {}
	var primary_order: Array[String] = []
	var family_max_strength := 0
	for retained in group:
		if retained == null:
			continue
		var primary_key := _retained_primary_key(retained)
		var required_strength := int(authoritative_destroyed_provider_keys.get(primary_key, 0))
		var retained_strength := _retained_root_strength(retained.root_status)
		if retained_strength < required_strength:
			continue
		var best: RetainedSubtreeState = best_by_primary.get(primary_key, null)
		if best == null:
			best_by_primary[primary_key] = retained
			primary_order.append(primary_key)
		elif _is_stronger_retained_subtree(retained, best):
			best_by_primary[primary_key] = retained
		family_max_strength = maxi(family_max_strength, retained_strength)

	var selected: Array[RetainedSubtreeState] = []
	for primary_key in primary_order:
		var best: RetainedSubtreeState = best_by_primary.get(primary_key, null)
		if best == null:
			continue
		var best_strength := _retained_root_strength(best.root_status)
		if primary_key == _retained_family_key(best) and family_max_strength > best_strength:
			continue
		selected.append(best)
	return selected


func _is_stronger_retained_subtree(candidate: RetainedSubtreeState, incumbent: RetainedSubtreeState) -> bool:
	if incumbent == null:
		return true
	if candidate == null:
		return false

	var candidate_strength := _retained_root_strength(candidate.root_status)
	var incumbent_strength := _retained_root_strength(incumbent.root_status)
	if candidate_strength != incumbent_strength:
		return candidate_strength > incumbent_strength
	if candidate.source_snapshot_version != incumbent.source_snapshot_version:
		return candidate.source_snapshot_version > incumbent.source_snapshot_version
	if candidate.source_topology_version != incumbent.source_topology_version:
		return candidate.source_topology_version > incumbent.source_topology_version
	if candidate.source_snapshot_timestamp_ns != incumbent.source_snapshot_timestamp_ns:
		return candidate.source_snapshot_timestamp_ns > incumbent.source_snapshot_timestamp_ns
	return candidate.retained_at_msec > incumbent.retained_at_msec


func _collect_authoritative_destroyed_provider_reconciliation_keys(panel: PanelModel) -> Dictionary:
	var keys := {}
	if panel == null:
		return keys
	for entry in panel.entries:
		if entry == null:
			continue
		if not entry.parent_id.contains("/retained_prior_generation_native_objects"):
			continue
		if not entry.id.begins_with("provider/"):
			continue
		if not _entry_has_destroyed_provider_badge(entry):
			continue
		var creation_gen := _extract_creation_gen_from_info_lines(entry.info_lines)
		if creation_gen < 0:
			continue
		var family_key := _retained_family_key_for_gen(creation_gen)
		keys[family_key] = _retained_root_strength("destroyed_provider")
		keys["%s|provider=%s" % [family_key, entry.id]] = _retained_root_strength("destroyed_provider")
	return keys


func _entry_has_destroyed_provider_badge(entry: StatusEntryModel) -> bool:
	if entry == null:
		return false
	for badge in entry.badges:
		if _badge_label_is_destroyed_phase(badge.label):
			return true
	return false


func _extract_creation_gen_from_info_lines(info_lines: Array[String]) -> int:
	var prefix := "Preserved record: creation_gen="
	for line in info_lines:
		if not line.begins_with(prefix):
			continue
		var suffix := line.substr(prefix.length())
		var end_idx := suffix.find(",")
		if end_idx >= 0:
			suffix = suffix.substr(0, end_idx)
		if suffix.is_valid_int():
			return int(suffix)
	return -1


func _extract_retained_panel_subtree(source_model: PanelModel, provider_root_id: String, root_status: String) -> PanelModel:
	var extracted := PanelModel.new()
	if source_model == null:
		return extracted

	if root_status == "destroyed_provider" and not provider_root_id.is_empty():
		var provider_entries := _collect_retained_entries_under_root(source_model, provider_root_id)
		for entry in provider_entries:
			extracted.entries.append(_clone_status_entry(entry))
		return extracted

	for entry in source_model.entries:
		if entry.id == "server/main":
			continue
		extracted.entries.append(_clone_status_entry(entry))
	return extracted


func _retained_subtree_exists(source_meta: Dictionary) -> bool:
	var target_gen := int(source_meta.get("gen", -1))
	var target_version := int(source_meta.get("version", -1))
	var target_topology := int(source_meta.get("topology_version", -1))
	var target_timestamp := int(source_meta.get("timestamp_ns", -1))
	for retained in _retained_subtrees:
		if retained.retained_from_gen != target_gen:
			continue
		if retained.source_snapshot_version != target_version:
			continue
		if retained.source_topology_version != target_topology:
			continue
		if retained.source_snapshot_timestamp_ns != target_timestamp:
			continue
		return true
	return false


func _resolve_retained_provider_root(panel: PanelModel) -> Dictionary:
	if panel == null:
		return {"provider_root_id": "", "root_status": "orphaned"}
	var destroyed_provider_ids: Array[String] = []
	for entry in panel.entries:
		if not entry.id.begins_with("provider/"):
			continue
		for badge in entry.badges:
			if _badge_label_is_destroyed_phase(badge.label):
				destroyed_provider_ids.append(entry.id)
				break
	if destroyed_provider_ids.size() == 1:
		return {"provider_root_id": destroyed_provider_ids[0], "root_status": "destroyed_provider"}
	if destroyed_provider_ids.size() > 1:
		return {"provider_root_id": "", "root_status": "ambiguous_provider"}
	return {"provider_root_id": "", "root_status": "orphaned"}


func _expire_retained_subtrees_by_policy() -> bool:
	if _retained_subtrees.is_empty():
		return false
	if PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC < 0:
		return false
	var now_msec := Time.get_ticks_msec()
	var kept: Array[RetainedSubtreeState] = []
	for retained in _retained_subtrees:
		if not _is_retained_subtree_expired(retained, now_msec):
			kept.append(retained)
	var changed := kept.size() != _retained_subtrees.size()
	_retained_subtrees = kept
	return changed


func _is_retained_subtree_expired(retained: RetainedSubtreeState, now_msec: int) -> bool:
	if PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC < 0:
		return false
	if retained == null:
		return true
	if retained.retained_at_msec < 0:
		return true
	return (now_msec - retained.retained_at_msec) >= PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC


func _append_retained_presentation_subtrees(target_panel: PanelModel) -> void:
	if target_panel == null:
		return
	if _retained_subtrees.is_empty():
		return

	for idx in range(_retained_subtrees.size()):
		_append_single_retained_subtree(target_panel, _retained_subtrees[idx], idx)

	_ensure_expandability(target_panel)


func _append_single_retained_subtree(target_panel: PanelModel, retained: RetainedSubtreeState, order_index: int) -> void:
	if retained == null or retained.panel_model == null:
		return
	var subtree_prefix := "%s/subtree/%d/gen_%d" % [RETAINED_PRESENTATION_ROOT_ID, order_index, retained.retained_from_gen]
	var retained_entries: Array[StatusEntryModel] = []
	var retained_root_source_id := ""
	var orphan_provider_root_ids: Array[String] = []
	var primary_orphan_provider_root_id := ""
	if retained.root_status == "destroyed_provider" and not retained.provider_root_id.is_empty():
		retained_root_source_id = retained.provider_root_id
		retained_entries = _collect_retained_entries_under_root(retained.panel_model, retained.provider_root_id)
	else:
		retained_entries = _collect_retained_orphan_entries(retained.panel_model)
		orphan_provider_root_ids = _collect_retained_provider_root_ids(retained_entries)
		if not orphan_provider_root_ids.is_empty():
			primary_orphan_provider_root_id = orphan_provider_root_ids[0]
			if not retained.provider_identity_hint.is_empty() and orphan_provider_root_ids.has(retained.provider_identity_hint):
				primary_orphan_provider_root_id = retained.provider_identity_hint
		else:
			retained_root_source_id = "%s/orphan_root" % subtree_prefix

	var included_ids := {}
	for source_entry in retained_entries:
		included_ids[source_entry.id] = true

	var retained_root_projected_id := "%s/%s" % [subtree_prefix, retained_root_source_id]
	var orphan_reason_line := ""
	if retained.root_status == "ambiguous_provider":
		orphan_reason_line = "Retained subtree is orphaned: multiple DESTROYED provider rows were present."
	else:
		orphan_reason_line = "Retained subtree is orphaned: no truthful DESTROYED provider root was available."

	if (retained.root_status != "destroyed_provider" or retained.provider_root_id.is_empty()) and orphan_provider_root_ids.is_empty():
		var orphan_info := _retained_metadata_info_lines(retained)
		orphan_info.append(orphan_reason_line)
		target_panel.entries.append(_entry(
			retained_root_projected_id,
			"server/main",
			1,
			"retained_orphan/gen_%d" % retained.retained_from_gen,
			false,
			true,
			[
				_badge("warning", "retained"),
				_badge("warning", "retained-root"),
				_badge("info", "continuity-only"),
				_badge("warning", "orphaned"),
			],
			[
				_counter("retained_from_gen", retained.retained_from_gen, 1),
				_counter("source_version", retained.source_snapshot_version, 1),
				_counter("source_topology", retained.source_topology_version, 1),
			],
			orphan_info
		))

	for source_entry in retained_entries:
		var cloned_entry := _clone_status_entry(source_entry)
		var projected_id := "%s/%s" % [subtree_prefix, source_entry.id]
		cloned_entry.id = projected_id
		if source_entry.id == retained.provider_root_id and retained.root_status == "destroyed_provider":
			cloned_entry.parent_id = "server/main"
			cloned_entry.depth = 1
			cloned_entry.info_lines = _append_lines(cloned_entry.info_lines, _retained_metadata_info_lines(retained))
			cloned_entry.badges.append(_badge("warning", "retained"))
			cloned_entry.badges.append(_badge("warning", "retained-root"))
			cloned_entry.badges.append(_badge("info", "continuity-only"))
			cloned_entry.label = "%s [retained]" % source_entry.label
		elif orphan_provider_root_ids.has(source_entry.id):
			cloned_entry.parent_id = "server/main"
			cloned_entry.depth = 1
			cloned_entry.badges.append(_badge("warning", "retained"))
			cloned_entry.badges.append(_badge("info", "continuity-only"))
			cloned_entry.badges.append(_badge("warning", "orphaned"))
			if source_entry.id == primary_orphan_provider_root_id:
				cloned_entry.badges.append(_badge("warning", "retained-root"))
				cloned_entry.counters.append(_counter("retained_from_gen", retained.retained_from_gen, 1))
				cloned_entry.counters.append(_counter("source_version", retained.source_snapshot_version, 1))
				cloned_entry.counters.append(_counter("source_topology", retained.source_topology_version, 1))
				cloned_entry.info_lines = _append_lines(cloned_entry.info_lines, _retained_metadata_info_lines(retained))
				cloned_entry.info_lines.append(orphan_reason_line)
			cloned_entry.label = "%s [retained]" % source_entry.label
		elif source_entry.parent_id.is_empty() or not included_ids.has(source_entry.parent_id):
			cloned_entry.parent_id = retained_root_projected_id
			cloned_entry.depth = 2
		else:
			cloned_entry.parent_id = "%s/%s" % [subtree_prefix, source_entry.parent_id]
			cloned_entry.depth = source_entry.depth
		target_panel.entries.append(cloned_entry)


func _retained_metadata_info_lines(retained: RetainedSubtreeState) -> Array[String]:
	var lines: Array[String] = [
		"Continuity-only retained view; not active snapshot truth.",
		"continuity: copied from a previously rendered authoritative panel.",
		"retained_from_gen=%d" % retained.retained_from_gen,
		"source timestamp_ns=%d" % retained.source_snapshot_timestamp_ns,
		"source version=%d, source topology=%d" % [retained.source_snapshot_version, retained.source_topology_version],
		"retained_at_msec=%d" % retained.retained_at_msec,
	]
	for timing_line in _retained_timing_info_lines(retained):
		lines.append(timing_line)
	return lines


func _retained_timing_info_lines(retained: RetainedSubtreeState) -> Array[String]:
	var lines: Array[String] = []
	var now_msec := Time.get_ticks_msec()
	var age_msec := maxi(0, now_msec - retained.retained_at_msec)
	lines.append("retained_age_msec=%d" % age_msec)

	# PROVISIONAL: uses panel-local retention TTL until authoritative runtime retention window is exposed.
	if PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC < 0:
		lines.append("retained_expiry=disabled (provisional local TTL policy)")
	else:
		var remaining_msec := PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC - age_msec
		if remaining_msec > 0:
			lines.append("retained_expires_in_msec=%d (provisional local TTL policy)" % remaining_msec)
		else:
			lines.append("retained_expired_by_msec=%d (provisional local TTL policy)" % abs(remaining_msec))
		lines.append("retained_ttl_msec=%d" % PROVISIONAL_RETAINED_PRESENTATION_TTL_MSEC)
	return lines


func _append_lines(base_lines: Array[String], extra_lines: Array[String]) -> Array[String]:
	var out: Array[String] = []
	for line in base_lines:
		out.append(line)
	for line in extra_lines:
		out.append(line)
	return out


func _collect_retained_entries_under_root(panel: PanelModel, root_entry_id: String) -> Array[StatusEntryModel]:
	var included_ids := {}
	included_ids[root_entry_id] = true
	var changed := true
	while changed:
		changed = false
		for entry in panel.entries:
			if included_ids.has(entry.id):
				continue
			if entry.parent_id.is_empty():
				continue
			if included_ids.has(entry.parent_id):
				included_ids[entry.id] = true
				changed = true
	var out: Array[StatusEntryModel] = []
	for entry in panel.entries:
		if included_ids.has(entry.id):
			out.append(entry)
	return out


func _collect_retained_orphan_entries(panel: PanelModel) -> Array[StatusEntryModel]:
	if panel == null:
		return []
	var provider_root_ids := _collect_retained_provider_root_ids(panel.entries)
	if not provider_root_ids.is_empty():
		return _collect_retained_entries_under_roots(panel, provider_root_ids)

	var out: Array[StatusEntryModel] = []
	for entry in panel.entries:
		if entry.id == "server/main":
			continue
		out.append(entry)
	return out


func _collect_retained_provider_root_ids(entries: Array) -> Array[String]:
	var provider_root_ids: Array[String] = []
	var seen := {}
	for entry in entries:
		if not (entry is StatusEntryModel):
			continue
		var typed_entry: StatusEntryModel = entry
		if not typed_entry.id.begins_with("provider/"):
			continue
		if not typed_entry.parent_id.is_empty() and typed_entry.parent_id != "server/main":
			continue
		if seen.has(typed_entry.id):
			continue
		seen[typed_entry.id] = true
		provider_root_ids.append(typed_entry.id)
	return provider_root_ids


func _collect_retained_entries_under_roots(panel: PanelModel, root_entry_ids: Array[String]) -> Array[StatusEntryModel]:
	var included_ids := {}
	for root_entry_id in root_entry_ids:
		if root_entry_id.is_empty():
			continue
		included_ids[root_entry_id] = true
	var changed := true
	while changed:
		changed = false
		for entry in panel.entries:
			if included_ids.has(entry.id):
				continue
			if entry.parent_id.is_empty():
				continue
			if included_ids.has(entry.parent_id):
				included_ids[entry.id] = true
				changed = true
	var out: Array[StatusEntryModel] = []
	for entry in panel.entries:
		if included_ids.has(entry.id):
			out.append(entry)
	return out


func _extract_authoritative_snapshot_meta(snapshot: Dictionary) -> Dictionary:
	return {
		"gen": int(snapshot.get("gen", -1)),
		"version": int(snapshot.get("version", -1)),
		"topology_version": int(snapshot.get("topology_version", -1)),
		"timestamp_ns": int(snapshot.get("timestamp_ns", -1)),
	}


func _clone_panel_model(source: PanelModel) -> PanelModel:
	var cloned := PanelModel.new()
	if source == null:
		return cloned
	for entry in source.entries:
		cloned.entries.append(_clone_status_entry(entry))
	return cloned


func _clone_status_entry(source: StatusEntryModel) -> StatusEntryModel:
	var cloned_badges: Array[BadgeModel] = []
	for badge in source.badges:
		cloned_badges.append(_badge(badge.role, badge.label, badge.kind))
	var cloned_counters: Array[CounterModel] = []
	for counter in source.counters:
		cloned_counters.append(_counter(counter.name, counter.value, counter.digits, counter.visibility))
	var cloned_info_lines: Array[String] = []
	for line in source.info_lines:
		cloned_info_lines.append(line)
	var cloned := _entry(
		source.id,
		source.parent_id,
		source.depth,
		source.label,
		source.expanded,
		source.can_expand,
		cloned_badges,
		cloned_counters,
		cloned_info_lines,
		source.visual_object_class
	)
	cloned.materialized_native_id = source.materialized_native_id
	cloned.summary_info_lines = source.summary_info_lines.duplicate()
	cloned.detail_info_lines = source.detail_info_lines.duplicate()
	cloned.anomaly_info_lines = source.anomaly_info_lines.duplicate()
	return cloned


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
	var schema_text := str(d.get("schema_version", "?"))
	if d.has("schema_version") and int(d.get("schema_version", -1)) != SUPPORTED_SCHEMA_VERSION:
		schema_text = "%s (unsupported)" % schema_text

	var rigs := _array_size_or_negative(d.get("rigs", null))
	var devices := _array_size_or_negative(d.get("devices", null))
	var streams := _array_size_or_negative(d.get("streams", null))
	var native_objects := _array_size_or_negative(d.get("native_objects", null))
	var detached_roots := _array_size_or_negative(d.get("detached_root_ids", null))
	var counts_text := "rigs=%s  devices=%s  streams=%s  native_objects=%s  detached_roots=%s" % [
		_count_text_or_type_gap(rigs),
		_count_text_or_type_gap(devices),
		_count_text_or_type_gap(streams),
		_count_text_or_type_gap(native_objects),
		_count_text_or_type_gap(detached_roots),
	]

	return {
		"state": "Snapshot available",
		"gen": str(d.get("gen", "?")),
		"version": str(d.get("version", "?")),
		"topology_version": str(d.get("topology_version", "?")),
		"schema_version": schema_text,
		"counts": counts_text,
		"timestamp": str(d.get("timestamp_ns", "-")),
	}


func _array_size_or_negative(value: Variant) -> int:
	if typeof(value) == TYPE_ARRAY:
		return (value as Array).size()
	return -1


func _count_text_or_type_gap(count: int) -> String:
	if count < 0:
		return "type-gap"
	return str(count)


func _check_snapshot_runtime_compat(snapshot: Dictionary) -> Dictionary:
	var contract_gaps: Array[String] = []
	var projection_gaps: Array[String] = []

	if not snapshot.has("schema_version"):
		contract_gaps.append("Contract gap: snapshot missing schema_version.")
	else:
		var schema_version := snapshot.get("schema_version")
		if typeof(schema_version) not in [TYPE_INT, TYPE_FLOAT]:
			contract_gaps.append(
				"Contract gap: schema_version has unexpected type=%d; expected integer." % typeof(schema_version)
			)
		else:
			var as_int := int(schema_version)
			if as_int != SUPPORTED_SCHEMA_VERSION:
				projection_gaps.append(
					"Projection gap: unsupported schema_version=%d; supported=%d."
					% [as_int, SUPPORTED_SCHEMA_VERSION]
				)

	_check_required_numeric_field(snapshot, "gen", contract_gaps)
	_check_required_numeric_field(snapshot, "version", contract_gaps)
	_check_required_numeric_field(snapshot, "topology_version", contract_gaps)
	_check_required_numeric_field(snapshot, "timestamp_ns", contract_gaps)

	_check_required_array_field(snapshot, "rigs", contract_gaps)
	_check_required_array_field(snapshot, "devices", contract_gaps)
	_check_required_array_field(snapshot, "streams", contract_gaps)
	_check_required_array_field(snapshot, "native_objects", contract_gaps)
	_check_required_array_field(snapshot, "detached_root_ids", contract_gaps)

	return {
		"ok": contract_gaps.is_empty() and projection_gaps.is_empty(),
		"contract_gaps": contract_gaps,
		"projection_gaps": projection_gaps,
	}


func _check_required_numeric_field(snapshot: Dictionary, key: String, gaps: Array[String]) -> void:
	if not snapshot.has(key):
		gaps.append("Contract gap: snapshot missing required top-level field '%s'." % key)
		return
	var value := snapshot.get(key)
	if typeof(value) not in [TYPE_INT, TYPE_FLOAT]:
		gaps.append(
			"Contract gap: snapshot.%s has unexpected type=%d; expected integer-compatible value."
			% [key, typeof(value)]
		)


func _check_required_array_field(snapshot: Dictionary, key: String, gaps: Array[String]) -> void:
	if not snapshot.has(key):
		gaps.append("Contract gap: snapshot missing required top-level field '%s'." % key)
		return
	if typeof(snapshot.get(key)) != TYPE_ARRAY:
		gaps.append("Contract gap: snapshot.%s expected Array, got type=%d." % [key, typeof(snapshot.get(key))])


func _build_runtime_compat_fallback_panel(contract_gaps: Array, projection_gaps: Array) -> PanelModel:
	var panel := PanelModel.new()
	var root_lines: Array[String] = []
	var contract_gap_lines: Array[String] = []
	var projection_gap_lines: Array[String] = []
	for raw_gap in contract_gaps:
		contract_gap_lines.append(str(raw_gap))
	for raw_projection_gap in projection_gaps:
		projection_gap_lines.append(str(raw_projection_gap))
	root_lines.append("Runtime payload is unsupported or malformed; projection skipped safely.")
	if not contract_gap_lines.is_empty():
		root_lines.append("See contract_gaps row for details.")
	if not projection_gap_lines.is_empty():
		root_lines.append("See projection_gaps row for details.")

	panel.entries.append(_entry(
		"server/main",
		"",
		0,
		"server/main",
		true,
		true,
		[_badge("warning", "snapshot-incompatible")],
		[],
		root_lines
	))

	if not contract_gap_lines.is_empty():
		panel.entries.append(_entry(
			"server/main/contract_gaps",
			"server/main",
			1,
			"contract_gaps",
			true,
			false,
			[_badge("warning", "schema")],
			[_counter("count", contract_gap_lines.size(), 1)],
			contract_gap_lines
		))

	_append_projection_gaps_row(panel, projection_gap_lines)
	_ensure_expandability(panel)
	return panel


func _apply_snapshot_read(reading: Dictionary) -> void:
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


func _phase_display_label(value: Variant) -> String:
	# Canonical snapshot contract: lifecycle phase is a string enum token.
	if typeof(value) == TYPE_STRING or typeof(value) == TYPE_STRING_NAME:
		var canonical := str(value).strip_edges().to_upper()
		if ["CREATED", "LIVE", "TEARING_DOWN", "DESTROYED"].has(canonical):
			return canonical
		return "UNKNOWN"
	# Transitional runtime-boundary compatibility only:
	# some runtime/dev paths may still surface integer phase values.
	var phase_enum := _phase_enum_value(value)
	return _phase_label_from_enum(phase_enum)


func _phase_is_destroyed(value: Variant) -> bool:
	return _phase_display_label(value) == "DESTROYED"


func _phase_is_non_live(value: Variant) -> bool:
	var canonical := _phase_display_label(value)
	return canonical == "TEARING_DOWN" or canonical == "DESTROYED"


func _phase_enum_value(value: Variant) -> int:
	if typeof(value) == TYPE_INT or typeof(value) == TYPE_FLOAT:
		return int(value)
	return -1


func _phase_label_from_enum(phase_enum: int) -> String:
	match phase_enum:
		0:
			return "CREATED"
		1:
			return "LIVE"
		2:
			return "TEARING_DOWN"
		3:
			return "DESTROYED"
		_:
			return "UNKNOWN"


func _badge_label_is_destroyed_phase(label: String) -> bool:
	if label.begins_with("phase="):
		return label.substr("phase=".length()) == "DESTROYED"
	if label.begins_with("native_phase="):
		return label.substr("native_phase=".length()) == "DESTROYED"
	return false


func _build_nil_panel_model(reason: String) -> PanelModel:
	var panel := PanelModel.new()
	var info_lines: Array[String] = []
	if reason != "No published snapshot yet.":
		info_lines.append(reason)
	panel.entries.append(_entry(
		"server/main",
		"",
		0,
		"server/main",
		true,
		true,
		[_badge("warning", "NO SNAPSHOT")],
		[
			_counter("gen", -1, 3),
			_counter("version", -1, 5),
			_counter("topology", -1, 3),
		],
		info_lines
	))
	return panel


func _project_snapshot_to_panel_model(snapshot: Dictionary, provider_mode: String) -> PanelModel:
	var panel := PanelModel.new()
	var issues: Array[String] = []

	var server_badges: Array[BadgeModel] = [_badge("success", "snapshot")]
	var server_counters: Array[CounterModel] = [
		_counter("gen", int(snapshot.get("gen", 0)), 3),
		_counter("version", int(snapshot.get("version", 0)), 5),
		_counter("topology", int(snapshot.get("topology_version", 0)), 3),
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

	var snapshot_version := int(snapshot.get("version", -1))
	var topology_version := int(snapshot.get("topology_version", -1))
	if current_provider_native_objects.is_empty() and issues.is_empty() and snapshot_version == 0 and topology_version == 0:
		panel.entries.append(_entry(
			"server/main/provider_pending",
			"server/main",
			1,
			"provider_pending",
			true,
			false,
			[_badge("info", "startup")],
			[_counter("providers", 0, 1)],
			["Startup baseline published before any current-generation provider native object is visible."]
		))
		_ensure_expandability(panel)
		return panel

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
	var provider_phase: Variant = provider_native_rec.get("phase", -1)
	var provider_badges: Array[BadgeModel] = [
		_badge("info", "published"),
		_badge("neutral", "native_phase=%s" % _phase_display_label(provider_phase)),
	]
	if _phase_is_destroyed(provider_phase):
		provider_badges.append(_badge("warning", "destroyed"))
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
	provider_entry.materialized_native_id = provider_native_id
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
				if _phase_is_non_live(native_rec.get("phase", -1)):
					if not current_non_live_device_native_matches_by_instance.has(owner_instance_id):
						current_non_live_device_native_matches_by_instance[owner_instance_id] = []
					current_non_live_device_native_matches_by_instance[owner_instance_id].append(native_rec)
		elif native_type_key == "stream":
			var owner_stream_id := int(native_rec.get("owner_stream_id", 0))
			if owner_stream_id > 0:
				if not current_stream_native_matches_by_stream_id.has(owner_stream_id):
					current_stream_native_matches_by_stream_id[owner_stream_id] = []
				current_stream_native_matches_by_stream_id[owner_stream_id].append(native_rec)
				if _phase_is_non_live(native_rec.get("phase", -1)):
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
		if devices_by_instance.has(instance_id):
			issues.append("Contract gap: duplicate device instance_id=%d." % instance_id)
			continue
		devices_by_instance[instance_id] = rec
		var device_label := "device/%s" % _safe_device_name(rec)
		var device_entry_id := "device/%d" % instance_id
		provider_device_ids_by_instance[instance_id] = device_entry_id
		var device_phase: Variant = rec.get("phase", -1)
		var device_badges: Array[BadgeModel] = [
			_badge("neutral", "phase=%s" % _phase_display_label(device_phase)),
			_badge("neutral", "mode=%s" % _device_mode_display_label(rec.get("mode", "UNKNOWN"))),
		]
		if _phase_is_destroyed(device_phase):
			device_badges.append(_badge("warning", "destroyed"))
		var device_info: Array[String] = []
		var device_matches: Array = current_device_native_matches_by_instance.get(instance_id, [])
		if device_matches.size() == 1:
			var device_native_rec: Dictionary = device_matches[0]
			var device_native_phase: Variant = device_native_rec.get("phase", -1)
			if _phase_display_label(device_native_phase) != _phase_display_label(device_phase):
				device_badges.append(_badge("neutral", "native_phase=%s" % _phase_display_label(device_native_phase)))
			promoted_native_ids[int(device_native_rec.get("native_id", 0))] = true
		elif device_matches.size() > 1:
			device_info.append(
				"Contract ambiguity: device row has %d matching current-generation Device native objects."
				% device_matches.size()
			)
		var device_entry := _entry(
			device_entry_id,
			provider_id,
			2,
			device_label,
			true,
			true,
			device_badges,
			_counters_from_record(
				rec,
				[
					["errors", "errors_count", 1],
					["still_w", "capture_width", 4],
					["still_h", "capture_height", 4],
					["still_fmt", "capture_format", 4],
					["still_prof", "capture_profile_version", 2],
				]
			),
			device_info
		)
		if device_matches.size() == 1:
			device_entry.materialized_native_id = int(device_matches[0].get("native_id", 0))
		panel.entries.append(device_entry)

	var streams_by_device := {}
	var seen_stream_ids := {}
	for i in range(streams.size()):
		var rec := _safe_dict(streams[i], issues, "streams[%d]" % i)
		if rec.is_empty():
			continue

		var stream_id := int(rec.get("stream_id", 0))
		var owner_instance := int(rec.get("device_instance_id", 0))

		if stream_id <= 0:
			issues.append("Contract gap: streams[%d] missing valid stream_id." % i)
			continue

		if seen_stream_ids.has(stream_id):
			issues.append("Contract gap: duplicate stream_id=%d." % stream_id)
			continue

		seen_stream_ids[stream_id] = true

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
			var stream_phase: Variant = rec.get("phase", -1)
			var stream_badges: Array[BadgeModel] = [
				_badge("neutral", "phase=%s" % _phase_display_label(stream_phase)),
				_badge("neutral", "mode=%s" % _stream_mode_display_label(rec.get("mode", "UNKNOWN"))),
			]
			if _phase_is_destroyed(stream_phase):
				stream_badges.append(_badge("warning", "destroyed"))
			var stream_info: Array[String] = []
			var stream_matches: Array = current_stream_native_matches_by_stream_id.get(stream_id, [])
			if stream_matches.size() == 1:
				var stream_native_rec: Dictionary = stream_matches[0]
				var stream_native_phase: Variant = stream_native_rec.get("phase", -1)
				if _phase_display_label(stream_native_phase) != _phase_display_label(stream_phase):
					stream_badges.append(_badge("neutral", "native_phase=%s" % _phase_display_label(stream_native_phase)))
				promoted_native_ids[int(stream_native_rec.get("native_id", 0))] = true
			elif stream_matches.size() > 1:
				stream_info.append(
					"Contract ambiguity: stream row has %d matching current-generation Stream native objects."
					% stream_matches.size()
				)
			var stream_entry := _entry(
				"stream/%d" % stream_id,
				parent_entry_id,
				3,
				"stream/%d" % stream_id,
				false,
				true,
				stream_badges,
				_counters_from_record(
					rec,
					[
						["width", "width", 4],
						["height", "height", 4],
						["fmt", "format", 4],
						["fps_min", "target_fps_min", 2],
						["fps_max", "target_fps_max", 2],
						["prof", "profile_version", 2],
						["recv", "frames_received", 3],
						["deliv", "frames_delivered", 3],
						["drop", "frames_dropped", 3],
						["queue", "queue_depth", 2],
						["last_ts", "last_frame_ts_ns", 5],
						["shown", "visibility_frames_presented", 3],
						["rej_fmt", "visibility_frames_rejected_unsupported", 2],
						["rej_inv", "visibility_frames_rejected_invalid", 2],
					]
				),
				stream_info
			)
			if stream_matches.size() == 1:
				stream_entry.materialized_native_id = int(stream_matches[0].get("native_id", 0))
			panel.entries.append(stream_entry)

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
			var destroyed_device_entry := _entry(
				canonical_device_id,
				provider_id,
				2,
				"device/%d [destroyed]" % int(instance_key),
				true,
				true,
				[
					_badge("warning", "destroyed"),
				],
				[],
				[]
			)
			destroyed_device_entry.materialized_native_id = destroyed_device_native_id
			panel.entries.append(destroyed_device_entry)
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
			var destroyed_stream_entry := _entry(
				canonical_stream_id,
				destroyed_stream_parent_id,
				3 if destroyed_stream_parent_id != provider_id else 2,
				"stream/%d [destroyed]" % int(stream_key),
				true,
				true,
				[
					_badge("warning", "destroyed"),
				],
				[],
				[]
			)
			destroyed_stream_entry.materialized_native_id = destroyed_stream_native_id
			panel.entries.append(destroyed_stream_entry)
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
		var rig_info: Array[String] = []
		var rig_badges: Array[BadgeModel] = [
			_badge("neutral", "phase=%s" % _phase_display_label(rec.get("phase", -1))),
		]
		if rec.has("mode"):
			rig_badges.append(_badge("neutral", "mode=%s" % str(rec.get("mode"))))
		panel.entries.append(_entry(
			rig_entry_id,
			provider_id,
			2,
			"rig/%s" % _safe_rig_name(rec),
			false,
			true,
			rig_badges,
			_counters_from_record(
				rec,
				[
					["still_w", "capture_width", 4],
					["still_h", "capture_height", 4],
				],
				[
					_counter("members", _safe_array(rec.get("member_hardware_ids", []), issues, "rig/%d.member_hardware_ids" % rig_id).size(), 1),
				]
			),
			rig_info
		))

		var members := _safe_array(rec.get("member_hardware_ids", []), issues, "rig/%d.member_hardware_ids" % rig_id)
		for j in range(members.size()):
			var hardware_id := str(members[j])
			var member_device := _find_device_by_hardware(devices, hardware_id, issues)
			var member_context_lines: Array[String] = ["context: rig member."]
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
				[],
				[],
				_append_lines(member_context_lines, member_info)
			))

	var orphan_row_id := "%s/orphaned_native_objects" % provider_id
	var orphan_rows: Array[StatusEntryModel] = []
	var orphan_rows_by_id := {}
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
		var entry_parent_id := str(native_entry.parent_id)
		var detached_root_id := int(rec.get("root_id", 0))
		var should_orphan := entry_parent_id == orphan_row_id
		if not should_orphan and not entry_parent_id.is_empty() and not _entry_exists(panel.entries, entry_parent_id):
			should_orphan = _contains_int(detached_root_ids, detached_root_id)
		if should_orphan:
			orphan_rows.append(native_entry)
			orphan_rows_by_id[native_entry.id] = native_entry
		else:
			panel.entries.append(native_entry)

	if orphan_rows.size() > 0:
		var orphan_row_id_by_native_id := {}
		for orphan_entry in orphan_rows:
			var orphan_native_id := int(orphan_entry.materialized_native_id)
			if orphan_native_id > 0 and not orphan_row_id_by_native_id.has(orphan_native_id):
				orphan_row_id_by_native_id[orphan_native_id] = orphan_entry.id

		for orphan_entry in orphan_rows:
			var desired_parent_id := str(orphan_entry.parent_id)
			var resolved_parent_id := orphan_row_id
			if orphan_rows_by_id.has(desired_parent_id) and desired_parent_id != orphan_entry.id:
				resolved_parent_id = desired_parent_id
			else:
				var desired_parent_native_id := _trailing_positive_int_from_row_id(desired_parent_id)
				if desired_parent_native_id > 0 and orphan_row_id_by_native_id.has(desired_parent_native_id):
					var aliased_parent_id := str(orphan_row_id_by_native_id.get(desired_parent_native_id, ""))
					if aliased_parent_id != "" and aliased_parent_id != orphan_entry.id and orphan_rows_by_id.has(aliased_parent_id):
						resolved_parent_id = aliased_parent_id
			orphan_entry.parent_id = resolved_parent_id

		_apply_detached_orphan_subtree_depths(orphan_rows, orphan_rows_by_id, orphan_row_id)

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
				[
					_badge("warning", "retained"),
					_badge("info", "prior-gen"),
				],
				[_counter("count", prior_native_objects.size(), 1)],
				["truth: authoritative prior-generation snapshot truth retained in current snapshot."]
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


func _compute_native_coverage_from_ids(native_objects: Array, observed_native_ids: Array[int], snapshot_gen: int) -> Dictionary:
	var snapshot_native_records := {}
	for item in native_objects:
		if typeof(item) != TYPE_DICTIONARY:
			continue
		var rec := item as Dictionary
		var native_id := int(rec.get("native_id", 0))
		if native_id <= 0:
			continue
		snapshot_native_records[native_id] = rec

	var observed_native_id_set := {}
	for native_id in observed_native_ids:
		if int(native_id) <= 0:
			continue
		observed_native_id_set[int(native_id)] = true

	var observed_snapshot_native_count := 0
	var missing_current := 0
	var missing_prior := 0
	var missing_destroyed := 0
	var missing_non_destroyed := 0
	for native_id in snapshot_native_records.keys():
		if observed_native_id_set.has(native_id):
			observed_snapshot_native_count += 1
			continue
		var rec: Dictionary = snapshot_native_records[native_id]
		var creation_gen := int(rec.get("creation_gen", snapshot_gen))
		if creation_gen == snapshot_gen:
			missing_current += 1
		elif creation_gen < snapshot_gen:
			missing_prior += 1
		else:
			missing_current += 1

		if _phase_is_destroyed(rec.get("phase", -1)):
			missing_destroyed += 1
		else:
			missing_non_destroyed += 1

	var missing_count := snapshot_native_records.size() - observed_snapshot_native_count
	var coverage_state := "OK"
	var coverage_role := "success"
	if missing_count > 0:
		coverage_state = "MISSING"
		coverage_role = "error"

	return {
		"state": coverage_state,
		"role": coverage_role,
		"total": snapshot_native_records.size(),
		"rendered": observed_snapshot_native_count,
		"missing": missing_count,
		"missing_current": missing_current,
		"missing_prior": missing_prior,
		"missing_destroyed": missing_destroyed,
		"missing_non_destroyed": missing_non_destroyed,
	}


func _find_panel_entry_by_id(panel: PanelModel, entry_id: String) -> StatusEntryModel:
	if panel == null:
		return null
	for entry in panel.entries:
		if entry != null and entry.id == entry_id:
			return entry
	return null


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
		if _phase_is_destroyed((item as Dictionary).get("phase", -1)):
			count += 1
	return count


func _append_native_generation_note(info_lines: Array[String], rec: Dictionary, snapshot_gen: int) -> void:
	var creation_gen_text := "unknown"
	if rec.has("creation_gen"):
		creation_gen_text = str(rec.get("creation_gen"))
	info_lines.append("truth: authoritative prior-generation snapshot truth.")
	info_lines.append("Preserved record: creation_gen=%s, current snapshot.gen=%d." % [creation_gen_text, snapshot_gen])
	if _phase_is_destroyed(rec.get("phase", -1)):
		info_lines.append("destroyed: authoritative prior-generation snapshot truth.")


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
	var native_type_key := _native_object_type_key(rec)
	# Preserve provider identity even when the record is retained from a prior generation.
	# Destroyed providers must remain provider-root rows rather than degrading to generic native_object rows.
	var is_provider_native := native_type_key == "provider"
	var parent_id := provider_id
	var info_lines: Array[String] = []

	if not is_provider_native:
		var can_parent_to_stream := owner_stream_id > 0 and native_type_key != "stream"
		if can_parent_to_stream:
			parent_id = "stream/%d" % owner_stream_id
			if not _entry_exists(existing_entries, parent_id):
				info_lines.append("Contract gap: owner stream not present in snapshot streams.")
				if not _contains_int(detached_root_ids, root_id):
					parent_id = provider_id
		elif owner_device_instance_id > 0:
			parent_id = "device/%d" % owner_device_instance_id
			if not _entry_exists(existing_entries, parent_id):
				info_lines.append("Contract gap: owner device not present in snapshot devices.")
				if not _contains_int(detached_root_ids, root_id):
					parent_id = provider_id
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

	var row_id := "native_object/%d" % native_id
	var row_label := "native_object/%d" % native_id
	if is_provider_native:
		row_id = "provider/%d" % native_id
		row_label = "provider/%d" % native_id
	elif native_type_key == "frameproducer":
		row_id = "frameproducer/%d" % native_id
		row_label = "frameproducer/%d" % native_id
		if owner_stream_id > 0:
			info_lines.append("owner_stream_id=%d" % owner_stream_id)
		if rec.has("creation_gen"):
			info_lines.append("creation_gen=%d" % int(rec.get("creation_gen", 0)))

	var target_depth := _depth_for_parent(parent_id)
	var native_badges: Array[BadgeModel] = [_badge("neutral", "phase=%s" % _phase_display_label(rec.get("phase", -1)))]
	if _phase_is_destroyed(rec.get("phase", -1)):
		native_badges.append(_badge("warning", "destroyed"))
	if is_prior_generation:
		native_badges.append(_badge("info", "prior-gen"))
	var native_counters := _counters_from_record(
		rec,
		[
			["bytes", "bytes_allocated", 3],
			["buffers", "buffers_in_use", 2],
		]
	)
	var native_entry := _entry(
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
	native_entry.visual_object_class = _visual_object_class_for_native_type(native_type_key)
	native_entry.materialized_native_id = native_id
	return native_entry


func _visual_object_class_for_native_type(native_type_key: String) -> String:
	match native_type_key:
		"provider":
			return "provider"
		"device":
			return "device"
		"stream":
			return "stream"
		"rig":
			return "rig"
		"frameproducer":
			return "native_object"
		_:
			return "native_object"


func _trailing_positive_int_from_row_id(row_id: String) -> int:
	if row_id.is_empty():
		return -1
	var parts := row_id.split("/")
	if parts.is_empty():
		return -1
	var tail := str(parts[parts.size() - 1])
	if tail.is_empty() or not tail.is_valid_int():
		return -1
	var parsed := int(tail)
	return parsed if parsed > 0 else -1


func _apply_detached_orphan_subtree_depths(
		orphan_rows: Array[StatusEntryModel],
		orphan_rows_by_id: Dictionary,
		orphan_row_id: String
	) -> void:
	var children_by_parent := {}
	for orphan_entry in orphan_rows:
		if orphan_entry == null:
			continue
		var parent_id := str(orphan_entry.parent_id)
		if not children_by_parent.has(parent_id):
			children_by_parent[parent_id] = []
		children_by_parent[parent_id].append(orphan_entry)

	var orphan_roots: Array[StatusEntryModel] = []
	for orphan_entry in orphan_rows:
		if orphan_entry == null:
			continue
		var parent_id := str(orphan_entry.parent_id)
		if parent_id == orphan_row_id or not orphan_rows_by_id.has(parent_id):
			orphan_roots.append(orphan_entry)

	for orphan_root in orphan_roots:
		_assign_detached_orphan_depth(orphan_root, 3, children_by_parent)


func _assign_detached_orphan_depth(
		entry: StatusEntryModel,
		depth: int,
		children_by_parent: Dictionary
	) -> void:
	if entry == null:
		return
	entry.depth = depth
	var children: Array = children_by_parent.get(entry.id, [])
	for child in children:
		_assign_detached_orphan_depth(child, depth + 1, children_by_parent)


func _ensure_expandability(panel: PanelModel) -> void:
	var child_counts := {}
	for e in panel.entries:
		if str(e.parent_id) == "":
			continue
		child_counts[e.parent_id] = int(child_counts.get(e.parent_id, 0)) + 1
	for e in panel.entries:
		var child_count := int(child_counts.get(e.id, 0))
		e.can_expand = child_count > 0
		if child_count > 0 and _should_default_expand_entry(e):
			e.expanded = true


func _reorder_panel_entries_depth_first(panel: PanelModel) -> void:
	if panel == null or panel.entries.size() <= 1:
		return

	var ids := {}
	for entry in panel.entries:
		if entry == null:
			continue
		ids[entry.id] = true

	var roots: Array[StatusEntryModel] = []
	var children_by_parent := {}
	for entry in panel.entries:
		if entry == null:
			continue
		var parent_id := str(entry.parent_id)
		if parent_id.is_empty() or not ids.has(parent_id):
			roots.append(entry)
			continue
		if not children_by_parent.has(parent_id):
			children_by_parent[parent_id] = []
		children_by_parent[parent_id].append(entry)

	var ordered: Array[StatusEntryModel] = []
	var visited := {}
	for root_entry in roots:
		_append_entry_subtree_depth_first(root_entry, children_by_parent, ordered, visited)

	for entry in panel.entries:
		if entry != null and not visited.has(entry.id):
			ordered.append(entry)
			visited[entry.id] = true

	panel.entries = ordered


func _append_entry_subtree_depth_first(
		entry: StatusEntryModel,
		children_by_parent: Dictionary,
		ordered: Array[StatusEntryModel],
		visited: Dictionary
	) -> void:
	if entry == null:
		return
	if visited.has(entry.id):
		return
	visited[entry.id] = true
	ordered.append(entry)

	var children: Array = children_by_parent.get(entry.id, [])
	for child in children:
		_append_entry_subtree_depth_first(child, children_by_parent, ordered, visited)


func _should_default_expand_entry(entry: StatusEntryModel) -> bool:
	if entry == null:
		return false
	if entry.id.begins_with("stream/"):
		return true
	return false


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


func _render_panel_model(model: PanelModel, update_category: String = "structural_rebuild") -> void:
	if model == null:
		push_warning("CamBANGStatusPanel: null PanelModel render request ignored.")
		return
	if _status_rows == null:
		return

	var style := _resolve_style()
	_dev_parent_by_id.clear()
	for entry_model in model.entries:
		_dev_parent_by_id[entry_model.id] = entry_model.parent_id

	if update_category == "value_refresh":
		var mismatch_reason := _value_refresh_row_reconcile_mismatch(model)
		if mismatch_reason != "":
			_debug_log_row_reconcile_fallback(model, mismatch_reason)
			_reconcile_row_nodes(model, style, true, "structural_rebuild")
			return

	_reconcile_row_nodes(model, style, update_category != "value_refresh", update_category)


func _reconcile_row_nodes(
		model: PanelModel,
		style: StatusPanelStyle,
		allow_create_remove: bool,
		update_category: String
	) -> void:
	var desired_row_ids := {}
	for entry_model in model.entries:
		if entry_model == null:
			continue
		desired_row_ids[entry_model.id] = true

	if allow_create_remove:
		var stale_row_ids: Array[String] = []
		for existing_row_id in _row_nodes_by_id.keys():
			var row_id_text := str(existing_row_id)
			if desired_row_ids.has(row_id_text):
				continue
			stale_row_ids.append(row_id_text)
		for stale_row_id in stale_row_ids:
			var stale_row = _row_nodes_by_id.get(stale_row_id, null)
			if stale_row != null and is_instance_valid(stale_row):
				if stale_row.get_parent() == _status_rows:
					_status_rows.remove_child(stale_row)
				stale_row.queue_free()
			_row_nodes_by_id.erase(stale_row_id)

	for index in range(model.entries.size()):
		var entry_model: StatusEntryModel = model.entries[index]
		if entry_model == null:
			continue
		entry_model.expanded = _resolved_entry_expanded_state(entry_model)
		var row_visible := _is_entry_visible(entry_model)
		_debug_log_disclosure_render_state(entry_model, row_visible, model)

		var row_id := entry_model.id
		var entry = _row_nodes_by_id.get(row_id, null)
		var created := false
		if entry == null or not is_instance_valid(entry):
			entry = STATUS_ENTRY_SCENE.instantiate()
			_row_nodes_by_id[row_id] = entry
			created = true
			if entry.has_signal("disclosure_toggled") and not entry.disclosure_toggled.is_connected(_on_entry_disclosure_toggled):
				entry.disclosure_toggled.connect(_on_entry_disclosure_toggled)

		var bound_row_id: String = str(entry.get_entry_id())
		if bound_row_id != "" and bound_row_id != row_id:
			if not allow_create_remove:
				_debug_log_row_reconcile_fallback(
					model,
					"value_refresh row instance bound to wrong row_id: expected=%s actual=%s" % [row_id, bound_row_id]
				)
				_reconcile_row_nodes(model, style, true, "structural_rebuild")
				return
			_row_nodes_by_id.erase(row_id)
			if bound_row_id != "":
				_row_nodes_by_id.erase(bound_row_id)
			if entry.get_parent() == _status_rows:
				_status_rows.remove_child(entry)
			entry.queue_free()
			entry = STATUS_ENTRY_SCENE.instantiate()
			_row_nodes_by_id[row_id] = entry
			created = true
			if entry.has_signal("disclosure_toggled") and not entry.disclosure_toggled.is_connected(_on_entry_disclosure_toggled):
				entry.disclosure_toggled.connect(_on_entry_disclosure_toggled)

		if entry.get_parent() != _status_rows:
			_status_rows.add_child(entry)
		if _status_rows.get_child(index) != entry:
			_status_rows.move_child(entry, index)

		entry.set_style(style)
		entry.set_model(entry_model)
		entry.visible = row_visible
		_debug_log_row_lifecycle(entry_model, entry, created, update_category)


func _value_refresh_row_reconcile_mismatch(model: PanelModel) -> String:
	if model == null:
		return "panel model missing"
	if _status_rows.get_child_count() != model.entries.size():
		return "child count changed from %d to %d" % [_status_rows.get_child_count(), model.entries.size()]
	if _row_nodes_by_id.size() != model.entries.size():
		return "row node mapping count changed from %d to %d" % [_row_nodes_by_id.size(), model.entries.size()]

	for index in range(model.entries.size()):
		var entry_model: StatusEntryModel = model.entries[index]
		if entry_model == null:
			return "model.entries[%d] is null" % index
		if not _row_nodes_by_id.has(entry_model.id):
			return "missing persisted row node for row_id=%s" % entry_model.id
		var mapped_entry = _row_nodes_by_id.get(entry_model.id, null)
		if mapped_entry == null or not is_instance_valid(mapped_entry):
			return "invalid persisted row node for row_id=%s" % entry_model.id
		var mapped_entry_id: String = str(mapped_entry.get_entry_id())
		if mapped_entry_id != "" and mapped_entry_id != entry_model.id:
			return "persisted row node mismatch for row_id=%s actual=%s" % [entry_model.id, mapped_entry_id]
		if mapped_entry.get_parent() != _status_rows:
			return "persisted row node detached for row_id=%s" % entry_model.id

	for existing_row_id in _row_nodes_by_id.keys():
		var row_id_text := str(existing_row_id)
		var mapped_entry = _row_nodes_by_id.get(row_id_text, null)
		if mapped_entry == null or not is_instance_valid(mapped_entry):
			return "invalid row node tracked for row_id=%s" % row_id_text
		if not _entry_exists(model.entries, row_id_text):
			return "tracked row_id missing from value refresh model: %s" % row_id_text

	return ""


func _is_entry_visible(entry_model: StatusEntryModel) -> bool:
	if entry_model.parent_id == "":
		return true

	var current_parent := entry_model.parent_id
	while current_parent != "":
		if not bool(_expanded_by_row_id.get(current_parent, true)):
			return false
		current_parent = _find_parent_id(current_parent)
	return true


func _find_parent_id(entry_id: String) -> String:
	return str(_dev_parent_by_id.get(entry_id, ""))


func _on_entry_disclosure_toggled(entry_id: String, _expanded: bool) -> void:
	var next_expanded := not _current_expanded_state_for_row(entry_id)
	_debug_log_disclosure_click(entry_id, next_expanded)
	_expanded_by_row_id[entry_id] = next_expanded
	if _last_panel_model != null:
		_render_panel_model(_last_panel_model)
	else:
		_render_panel_model(_build_fake_panel_model())


func _resolved_entry_expanded_state(entry_model: StatusEntryModel) -> bool:
	if entry_model == null:
		return false
	if not entry_model.can_expand:
		return false
	if _expanded_by_row_id.has(entry_model.id):
		return bool(_expanded_by_row_id[entry_model.id])
	return entry_model.expanded


func _current_expanded_state_for_row(entry_id: String) -> bool:
	if _expanded_by_row_id.has(entry_id):
		return bool(_expanded_by_row_id[entry_id])
	if _last_panel_model == null:
		return false
	for entry_model in _last_panel_model.entries:
		if entry_model != null and entry_model.id == entry_id:
			return _resolved_entry_expanded_state(entry_model)
	return false


func _debug_log_disclosure_click(entry_id: String, expanded: bool) -> void:
	if not _debug_disclosure_enabled():
		return
	var previous_expanded := _current_expanded_state_for_row(entry_id)
	print(
		"[CAMBANG disclosure click] row_id=%s before=%s after=%s persisted_before=%s persisted_after=%s"
		% [
			entry_id,
			previous_expanded,
			expanded,
			_expanded_by_row_id.has(entry_id),
			true,
		]
	)


func _debug_log_disclosure_render_state(entry_model: StatusEntryModel, row_visible: bool, model: PanelModel) -> void:
	if not _debug_disclosure_enabled() or entry_model == null:
		return
	if not entry_model.id.begins_with("stream/"):
		return
	var hidden_child_ids: Array[String] = []
	if not entry_model.expanded:
		for child_entry in model.entries:
			if child_entry == null:
				continue
			if child_entry.parent_id == entry_model.id:
				hidden_child_ids.append(child_entry.id)
	print(
		"[CAMBANG disclosure render] row_id=%s model_expanded=%s persisted_expanded=%s persisted_override=%s row_visible=%s child_rows=%d hidden_child_rows=%s"
		% [
			entry_model.id,
			entry_model.expanded,
			bool(_expanded_by_row_id.get(entry_model.id, entry_model.expanded)),
			_expanded_by_row_id.has(entry_model.id),
			row_visible,
			_count_direct_child_rows(model, entry_model.id),
			hidden_child_ids,
		]
	)


func _count_direct_child_rows(model: PanelModel, parent_id: String) -> int:
	if model == null:
		return 0
	var count := 0
	for entry in model.entries:
		if entry != null and entry.parent_id == parent_id:
			count += 1
	return count


func _debug_disclosure_enabled() -> bool:
	if not OS.has_environment(DEBUG_DISCLOSURE_ENV):
		return false
	var env_value := OS.get_environment(DEBUG_DISCLOSURE_ENV).strip_edges().to_lower()
	return ["1", "true", "yes", "on"].has(env_value)


func _debug_log_row_lifecycle(
		entry_model: StatusEntryModel,
		entry,
		created: bool,
		update_category: String
	) -> void:
	if not _debug_disclosure_enabled():
		return
	if entry_model == null or entry == null:
		return
	print(
		"[CAMBANG row lifecycle] row_id=%s instance_id=%s state=%s update=%s gen=%s version=%s topology_version=%s"
		% [
			entry_model.id,
			str(entry.get_instance_id()),
			("created" if created else "reused"),
			update_category,
			str(_last_snapshot_meta.get("gen", "?")),
			str(_last_snapshot_meta.get("version", "?")),
			str(_last_snapshot_meta.get("topology_version", "?")),
		]
	)


func _debug_log_row_reconcile_fallback(model: PanelModel, reason: String) -> void:
	if not _debug_disclosure_enabled():
		return
	var row_count := 0
	if model != null:
		row_count = model.entries.size()
	print(
		"[CAMBANG row lifecycle fallback] reason=%s row_count=%d gen=%s version=%s topology_version=%s"
		% [
			reason,
			row_count,
			str(_last_snapshot_meta.get("gen", "?")),
			str(_last_snapshot_meta.get("version", "?")),
			str(_last_snapshot_meta.get("topology_version", "?")),
		]
	)


func _entry(
		id: String,
		parent_id: String,
		depth: int,
		label: String,
		expanded: bool,
		can_expand: bool,
		badges: Array[BadgeModel],
		counters: Array[CounterModel],
		info_lines: Array[String],
		visual_object_class: String = ""
	) -> StatusEntryModel:
	var model := StatusEntryModel.new()
	model.id = id
	model.parent_id = parent_id
	model.depth = depth
	model.label = label
	model.visual_object_class = visual_object_class
	model.expanded = expanded
	model.can_expand = can_expand
	model.badges = _normalize_badges(badges)
	model.counters = counters
	model.info_lines = info_lines
	return model


func _normalize_badges(badges: Array[BadgeModel]) -> Array[BadgeModel]:
	var dedup := {}
	var normalized: Array[BadgeModel] = []
	for badge in badges:
		if badge == null:
			continue
		if badge.kind == "health":
			continue
		var key := "%s|%s|%s" % [badge.role, badge.label, badge.kind]
		if dedup.has(key):
			continue
		dedup[key] = true
		normalized.append(_badge(badge.role, badge.label, badge.kind))
	normalized.sort_custom(_badge_less)
	var with_health: Array[BadgeModel] = [_health_badge()]
	for badge in normalized:
		with_health.append(badge)
	return with_health


func _badge_less(a: BadgeModel, b: BadgeModel) -> bool:
	return _badge_sort_key(a) < _badge_sort_key(b)


func _badge_sort_key(badge: BadgeModel) -> String:
	var label := badge.label
	var category := 9
	if label == "retained" or label == "retained-root" or label == "orphaned":
		category = 0
	elif label == "destroyed" or label.begins_with("phase=") or label.begins_with("native_phase="):
		category = 1
	elif label.begins_with("type="):
		category = 2
	else:
		category = 3
	return "%d|%s|%s" % [category, label, badge.role]


func _badge(role: String, label: String, kind: String = "") -> BadgeModel:
	# MODEL BADGES (authoritative projection truth):
	# projection emits only truth-carrying badges consumed by the renderer.
	var forbidden_snapshot_unavailable := "snapshot" + "-unavailable"
	if label == forbidden_snapshot_unavailable:
		push_error(forbidden_snapshot_unavailable + " is forbidden; use NO SNAPSHOT")
	var model := BadgeModel.new()
	model.role = role
	model.label = label
	model.kind = kind
	return model




func _health_badge() -> BadgeModel:
	return _badge("neutral", "UNKNOWN", "health")
func _counter(name: String, value: int, digits: int, visibility: String = "core") -> CounterModel:
	var model := CounterModel.new()
	model.name = name
	model.value = value
	model.text_value = ""
	model.digits = digits
	model.visibility = visibility
	return model


func _counters_from_record(rec: Dictionary, specs: Array, always_include: Array[CounterModel] = []) -> Array[CounterModel]:
	var counters: Array[CounterModel] = []
	for existing_counter in always_include:
		if existing_counter != null:
			counters.append(existing_counter)
	for raw_spec in specs:
		if typeof(raw_spec) != TYPE_ARRAY:
			continue
		var spec: Array = raw_spec
		if spec.size() < 3:
			continue
		var counter_name := str(spec[0])
		var source_field := str(spec[1])
		var digits := int(spec[2])
		var visibility := str(spec[3]) if spec.size() > 3 else "core"
		if not rec.has(source_field):
			continue
		counters.append(_counter(counter_name, int(rec.get(source_field)), digits, visibility))
	return counters


func _device_mode_display_label(value: Variant) -> String:
	if typeof(value) == TYPE_STRING or typeof(value) == TYPE_STRING_NAME:
		var canonical := str(value).strip_edges().to_upper()
		if ["IDLE", "STREAMING", "CAPTURING", "ERROR"].has(canonical):
			return canonical
		return "UNKNOWN"
	return "UNKNOWN"


func _stream_mode_display_label(value: Variant) -> String:
	if typeof(value) == TYPE_STRING or typeof(value) == TYPE_STRING_NAME:
		var canonical := str(value).strip_edges().to_upper()
		if ["STOPPED", "FLOWING", "STARVED", "ERROR"].has(canonical):
			return canonical
		return "UNKNOWN"
	return "UNKNOWN"


func _build_still_profile_info_line(rec: Dictionary) -> String:
	return _build_info_line_from_parts(
		rec,
		"still",
		[
			["capture_width", "capture_width", "int"],
			["capture_height", "capture_height", "int"],
			["capture_format", "capture_format", "fourcc"],
			["capture_profile_version", "capture_profile_version", "int"],
		]
	)


func _build_stream_visibility_info_line(rec: Dictionary) -> String:
	return _build_info_line_from_parts(
		rec,
		"visibility",
		[
			["visibility_frames_presented", "visibility_frames_presented", "int"],
			["visibility_frames_rejected_unsupported", "visibility_frames_rejected_unsupported", "int"],
			["visibility_frames_rejected_invalid", "visibility_frames_rejected_invalid", "int"],
			["visibility_last_path", "visibility_last_path", "visibility_path"],
		]
	)


func _build_info_line_from_parts(rec: Dictionary, prefix: String, specs: Array) -> String:
	var parts: Array[String] = []
	for raw_spec in specs:
		if typeof(raw_spec) != TYPE_ARRAY:
			continue
		var spec: Array = raw_spec
		if spec.size() < 2:
			continue
		var label := str(spec[0])
		var field := str(spec[1])
		var formatter := str(spec[2]) if spec.size() > 2 else "int"
		if not rec.has(field):
			continue
		parts.append("%s=%s" % [label, _format_info_value(rec.get(field), formatter)])
	if parts.is_empty():
		return ""
	return "%s: %s" % [prefix, " ".join(parts)]


func _format_info_value(raw_value: Variant, formatter: String) -> String:
	match formatter:
		"fourcc":
			return _format_fourcc_with_raw(int(raw_value))
		"visibility_path":
			return _visibility_path_display(int(raw_value))
		_:
			return str(int(raw_value))


func _apply_detail_policy_to_panel(panel: PanelModel) -> void:
	if panel == null:
		return
	for entry in panel.entries:
		_apply_detail_policy_to_entry(entry)
	_ensure_expandability(panel)


func _apply_detail_policy_to_entry(entry: StatusEntryModel) -> void:
	if entry == null:
		return

	entry.summary_info_lines.clear()
	entry.detail_info_lines.clear()
	entry.anomaly_info_lines.clear()

	for line in entry.info_lines:
		if _is_anomaly_info_line(line):
			entry.anomaly_info_lines.append(line)
		elif _should_show_line_in_summary(entry, line):
			entry.summary_info_lines.append(line)
		else:
			entry.detail_info_lines.append(line)

	for counter in entry.counters:
		counter.visibility = _counter_visibility_for_entry(entry, counter)


func _is_anomaly_info_line(line: String) -> bool:
	return (
		line.begins_with("Contract gap:")
		or line.begins_with("Contract ambiguity:")
		or line.begins_with("Projection invariant:")
		or line.begins_with("Projection gap:")
		or line.begins_with("Runtime payload")
		or line.find("contradiction") >= 0
		or line.find("unsupported") >= 0
		or line.find("malformed") >= 0
	)


func _should_show_line_in_summary(entry: StatusEntryModel, line: String) -> bool:
	if entry == null:
		return false
	if _entry_kind(entry) == "retained":
		return (
			line.begins_with("Panel-local continuity only.")
			or line.begins_with("continuity:")
		)
	if (
		line.begins_with("profile:")
		or line.begins_with("flow:")
		or line.begins_with("visibility:")
		or line.begins_with("still:")
		or line.begins_with("truth:")
	):
		return true
	if entry.id == "server/main":
		return true
	if entry.label == "contract_gaps" or entry.label == "projection_gaps":
		return false
	return entry.summary_info_lines.is_empty()


func _counter_visibility_for_entry(entry: StatusEntryModel, counter: CounterModel) -> String:
	if entry == null or counter == null:
		return "core"
	if entry.label == "contract_gaps" or entry.label == "projection_gaps":
		return "core"
	if _is_frameproducer_entry(entry):
		match counter.name:
			"buffers":
				return "summary"
			"bytes":
				return "detail"
			_:
				return "summary"
	if _is_native_object_entry(entry):
		match counter.name:
			"buffers":
				return "summary"
			"bytes":
				return "detail"
			_:
				return "summary"
	match counter.name:
		"gen", "version", "topology", "rigs", "devices", "streams", "mode", "errors", "count", "members", "retained_from_gen":
			return "core"
		"width", "height", "fps_min", "fps_max", "recv", "deliv", "drop", "queue", "shown", "rej_fmt", "rej_inv", "still_w", "still_h", "native_all", "native_cur", "buffers", "source_version":
			return "summary"
		"frames", "bytes", "native_prev", "native_dead", "source_topology":
			return "detail"
		_:
			return "summary"


func _is_frameproducer_entry(entry: StatusEntryModel) -> bool:
	if entry == null:
		return false
	return entry.id.begins_with("frameproducer/") or entry.id.contains("/frameproducer/")


func _is_native_object_entry(entry: StatusEntryModel) -> bool:
	if entry == null:
		return false
	return entry.id.begins_with("native_object/") or entry.id.contains("/native_object/")


func _entry_kind(entry: StatusEntryModel) -> String:
	if entry == null:
		return "authoritative"
	if _is_retained_projection_entry(entry.id) or _has_badge_label(entry, "retained"):
		return "retained"
	if entry.label == "contract_gaps" or entry.label == "projection_gaps":
		return "contract_gap"
	for line in entry.info_lines:
		if _is_anomaly_info_line(line):
			return "contract_gap"
	if (
		entry.id.begins_with("native_object/")
		or entry.id.begins_with("frameproducer/")
		or entry.id.contains("orphaned_native_objects")
		or _has_badge_label(entry, "detached")
		or _has_badge_label(entry, "orphaned")
	):
		return "fallback"
	return "authoritative"



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


func _format_fourcc_with_raw(value: int) -> String:
	var raw := int(value)
	return "%s (%d)" % [_fourcc_to_text(raw), raw]


func _fourcc_to_text(value: int) -> String:
	if value == 0:
		return "0x00000000"
	var chars: Array[String] = []
	for shift in [0, 8, 16, 24]:
		var code: int = int((value >> shift) & 0xFF)
		if code >= 32 and code <= 126:
			chars.append(char(code))
		else:
			chars.append(".")
	return "'%s'" % "".join(chars)


func _visibility_path_display(value: int) -> String:
	match value:
		0:
			return "NONE (0)"
		1:
			return "RGBA_DIRECT (1)"
		2:
			return "BGRA_SWIZZLED (2)"
		3:
			return "REJECTED_UNSUPPORTED (3)"
		4:
			return "REJECTED_INVALID (4)"
		_:
			return "UNKNOWN (%d)" % value
