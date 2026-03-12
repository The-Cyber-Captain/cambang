@tool
class_name CamBANGStatusPanel
extends PanelContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"
const STATUS_ENTRY_SCENE := preload("res://addons/cambang/internal/status_entry.tscn")


class PanelModel extends RefCounted:
	var entries: Array[StatusEntryModel] = []


class StatusEntryModel extends RefCounted:
	var depth: int = 0
	var can_expand: bool = false
	var expanded: bool = false
	var label: String = ""
	var badges: Array[BadgeModel] = []
	var counters: Array[CounterModel] = []
	var info_lines: Array[String] = []


class BadgeModel extends RefCounted:
	var text: String = ""
	var theme: String = "neutral"


class CounterModel extends RefCounted:
	var name: String = ""
	var value: String = ""
	var theme: String = "neutral"

var _title_label: Label
var _provider_mode_value: Label
var _snapshot_state_value: Label
var _gen_value: Label
var _version_value: Label
var _topology_version_value: Label
var _schema_version_value: Label
var _counts_value: Label
var _timestamp_value: Label
var _status_rows: VBoxContainer
var _server: Object = null


func _ready() -> void:
	_build_ui_if_needed()
	_connect_server()
	_refresh_from_server()
	_render_panel_model(_build_fake_panel_model())


func _enter_tree() -> void:
	if is_node_ready():
		_build_ui_if_needed()
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

	_status_rows = VBoxContainer.new()
	_status_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.add_child(_status_rows)


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
		0,
		"server/main",
		true,
		true,
		[_badge("up", "ok")],
		[_counter("providers", "1", "neutral")],
		[]
	))
	panel.entries.append(_entry(
		1,
		"provider/windows-mf",
		true,
		true,
		[_badge("running", "ok")],
		[_counter("devices", "1", "neutral")],
		[]
	))
	panel.entries.append(_entry(
		2,
		"device/cam-front",
		true,
		true,
		[_badge("active", "ok")],
		[_counter("streams", "1", "neutral")],
		[]
	))
	panel.entries.append(_entry(
		3,
		"stream/video0",
		false,
		false,
		[_badge("rgb8", "neutral")],
		[_counter("fps", "60", "ok")],
		[]
	))

	panel.entries.append(_entry(
		1,
		"rig/stereo-a",
		true,
		true,
		[_badge("dual-device", "neutral")],
		[_counter("devices", "2", "neutral")],
		[]
	))
	panel.entries.append(_entry(
		2,
		"device/left-eye",
		false,
		false,
		[_badge("attached", "ok")],
		[_counter("streams", "1", "neutral")],
		[]
	))
	panel.entries.append(_entry(
		2,
		"device/right-eye",
		false,
		false,
		[_badge("attached", "ok")],
		[_counter("streams", "1", "neutral")],
		[]
	))

	panel.entries.append(_entry(
		0,
		"native_object/orphan-42",
		true,
		false,
		[_badge("orphan", "warn")],
		[_counter("refs", "0", "warn")],
		[]
	))
	panel.entries.append(_entry(
		1,
		"stream/unclaimed",
		false,
		false,
		[_badge("contract-gap", "error")],
		[_counter("frames", "0", "warn")],
		[
			"Provider reported stream before rig association.",
			"Projection path not implemented in prototype.",
		]
	))

	return panel


func _render_panel_model(model: PanelModel) -> void:
	if _status_rows == null:
		return

	for child in _status_rows.get_children():
		child.queue_free()

	for entry_model in model.entries:
		var entry := STATUS_ENTRY_SCENE.instantiate()
		_status_rows.add_child(entry)
		entry.set_model(entry_model)


func _entry(
		depth: int,
		label: String,
		can_expand: bool,
		expanded: bool,
		badges: Array[BadgeModel],
		counters: Array[CounterModel],
		info_lines: Array[String]
	) -> StatusEntryModel:
	var model := StatusEntryModel.new()
	model.depth = depth
	model.label = label
	model.can_expand = can_expand
	model.expanded = expanded
	model.badges = badges
	model.counters = counters
	model.info_lines = info_lines
	return model


func _badge(text_value: String, theme: String) -> BadgeModel:
	var model := BadgeModel.new()
	model.text = text_value
	model.theme = theme
	return model


func _counter(name: String, value: String, theme: String) -> CounterModel:
	var model := CounterModel.new()
	model.name = name
	model.value = value
	model.theme = theme
	return model
