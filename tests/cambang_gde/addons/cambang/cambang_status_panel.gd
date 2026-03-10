@tool
class_name CamBANGStatusPanel
extends PanelContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"

var _title_label: Label
var _provider_mode_value: Label
var _status_value: Label
var _snapshot_value: Label
var _counts_value: Label
var _timestamp_value: Label
var _server: Object = null


func _ready() -> void:
	_build_ui_if_needed()
	_connect_server()
	_refresh_from_server()


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
	_status_value = _add_row(grid, "Server Status")
	_snapshot_value = _add_row(grid, "Snapshot")
	_counts_value = _add_row(grid, "Entity Counts")
	_timestamp_value = _add_row(grid, "Timestamp")


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
		_status_value.text = "CamBANGServer singleton not found"
		_snapshot_value.text = "none"
		_counts_value.text = "-"
		_timestamp_value.text = "-"
		return

	_provider_mode_value.text = str(_server.get_provider_mode())

	var snapshot = _server.get_state_snapshot()
	if snapshot == null:
		_status_value.text = "stopped or waiting for baseline"
		_snapshot_value.text = "none"
		_counts_value.text = "rigs=0 devices=0 streams=0 native_objects=0"
		_timestamp_value.text = "-"
		return

	if typeof(snapshot) != TYPE_DICTIONARY:
		_status_value.text = "unexpected snapshot type=%d" % typeof(snapshot)
		_snapshot_value.text = str(snapshot)
		_counts_value.text = "-"
		_timestamp_value.text = "-"
		return

	var d: Dictionary = snapshot
	_status_value.text = "snapshot available"
	_snapshot_value.text = "gen=%s version=%s topology_version=%s schema=%s" % [
		str(d.get("gen", "?")),
		str(d.get("version", "?")),
		str(d.get("topology_version", "?")),
		str(d.get("schema_version", "?")),
	]
	_counts_value.text = "rigs=%d devices=%d streams=%d native_objects=%d" % [
		(d.get("rigs", []) as Array).size(),
		(d.get("devices", []) as Array).size(),
		(d.get("streams", []) as Array).size(),
		(d.get("native_objects", []) as Array).size(),
	]
	_timestamp_value.text = str(d.get("timestamp_ns", "-"))
