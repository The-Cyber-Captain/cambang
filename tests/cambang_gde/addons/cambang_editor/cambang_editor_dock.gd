@tool
class_name CamBANGEditorDock
extends VBoxContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"

const UI_STATE_STOPPED := "stopped"
const UI_STATE_STARTING := "starting"
const UI_STATE_RUNNING := "running"
const UI_STATE_STOPPING := "stopping"

var _status_panel: Control
var _provider_mode_option: OptionButton
var _message_label: Label
var _start_button: Button
var _stop_button: Button

var _start_pending := false
var _stop_pending := false


func _ready() -> void:
	_build_ui_if_needed()
	_refresh_from_server()


func refresh_from_server() -> void:
	_refresh_from_server()


func _build_ui_if_needed() -> void:
	if _status_panel != null:
		return

	name = "CamBANG"
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var controls := HBoxContainer.new()
	controls.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(controls)

	_start_button = Button.new()
	_start_button.text = "Start Server"
	_start_button.pressed.connect(_on_start_pressed)
	controls.add_child(_start_button)

	_stop_button = Button.new()
	_stop_button.text = "Stop Server"
	_stop_button.pressed.connect(_on_stop_pressed)
	controls.add_child(_stop_button)

	var mode_label := Label.new()
	mode_label.text = "Provider Mode"
	controls.add_child(mode_label)

	_provider_mode_option = OptionButton.new()
	_provider_mode_option.item_selected.connect(_on_provider_mode_selected)
	_provider_mode_option.add_item("platform_backed")
	_provider_mode_option.add_item("synthetic")
	controls.add_child(_provider_mode_option)

	_message_label = Label.new()
	_message_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_message_label.text = "Editor controls call only the CamBANGServer public API."
	add_child(_message_label)

	_status_panel = CamBANGStatusPanel.new()
	_status_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_status_panel)


func _get_server() -> Object:
	if Engine.has_singleton(SERVER_SINGLETON_NAME):
		return Engine.get_singleton(SERVER_SINGLETON_NAME)
	if has_node("/root/%s" % SERVER_SINGLETON_NAME):
		return get_node("/root/%s" % SERVER_SINGLETON_NAME)
	return null


func _refresh_from_server() -> void:
	_build_ui_if_needed()
	if _status_panel != null:
		_status_panel.force_refresh()

	var server := _get_server()
	if server == null:
		_start_pending = false
		_stop_pending = false
		_start_button.disabled = true
		_stop_button.disabled = true
		_provider_mode_option.disabled = true
		_message_label.text = "CamBANGServer singleton not available."
		return

	var snapshot: Variant = server.get_state_snapshot()
	var has_snapshot := typeof(snapshot) == TYPE_DICTIONARY
	var state := _derive_ui_state(has_snapshot)

	_start_button.disabled = has_snapshot or _start_pending
	_stop_button.disabled = not has_snapshot and not _stop_pending
	_provider_mode_option.disabled = has_snapshot or _start_pending or _stop_pending

	var mode := str(server.get_provider_mode())
	for index in range(_provider_mode_option.item_count):
		if _provider_mode_option.get_item_text(index) == mode:
			_provider_mode_option.select(index)
			break

	_message_label.text = _state_message(state)


func _derive_ui_state(has_snapshot: bool) -> String:
	if has_snapshot:
		if _stop_pending:
			_start_pending = false
			return UI_STATE_STOPPING
		_start_pending = false
		_stop_pending = false
		return UI_STATE_RUNNING

	if _stop_pending:
		_stop_pending = false
		return UI_STATE_STOPPED

	if _start_pending:
		return UI_STATE_STARTING

	return UI_STATE_STOPPED


func _state_message(state: String) -> String:
	match state:
		UI_STATE_STOPPED:
			return "Stopped. Provider mode can be changed."
		UI_STATE_STARTING:
			return "Starting… waiting for baseline snapshot."
		UI_STATE_RUNNING:
			return "Running. Provider mode is locked until stopped."
		UI_STATE_STOPPING:
			return "Stopping… waiting for snapshot to clear."
		_:
			return "Stopped. Provider mode can be changed."


func _on_start_pressed() -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot start: CamBANGServer singleton not available."
		return

	if _start_button.disabled:
		return

	server.start()
	_start_pending = true
	_stop_pending = false
	_refresh_from_server()
	_message_label.text = "Start requested; waiting for baseline snapshot."


func _on_stop_pressed() -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot stop: CamBANGServer singleton not available."
		return

	if _stop_button.disabled:
		return

	server.stop()
	_stop_pending = true
	_start_pending = false
	_refresh_from_server()
	_message_label.text = "Stop requested; waiting for snapshot to clear."


func _on_provider_mode_selected(index: int) -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot set provider mode: CamBANGServer singleton not available."
		return

	if _provider_mode_option.disabled:
		return

	var selected_mode := _provider_mode_option.get_item_text(index)
	var err: int = server.set_provider_mode(selected_mode)
	if err == OK:
		_message_label.text = "Provider mode set via CamBANGServer.set_provider_mode('%s')." % selected_mode
	else:
		_message_label.text = "Provider mode change rejected with error %d. Stop the server before changing mode." % err
	_refresh_from_server()
