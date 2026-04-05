@tool
class_name CamBANGEditorDock
extends VBoxContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"

const UI_STATE_STOPPED := "stopped"
const UI_STATE_STARTING := "starting"
const UI_STATE_RUNNING := "running"
const UI_STATE_STOPPING := "stopping"

const TRANSITION_NONE := "none"
const TRANSITION_STARTING := "starting"
const TRANSITION_STOPPING := "stopping"

const TRANSITION_POLL_INTERVAL_SEC := 0.1

var _status_panel: Control
var _provider_mode_option: OptionButton
var _message_label: Label
var _start_button: Button
var _stop_button: Button
var _transition_timer: Timer

var _transition_pending := TRANSITION_NONE
var _server_connected := false


func _ready() -> void:
	_build_ui_if_needed()
	_connect_server_if_needed()
	_refresh_from_server()


func _exit_tree() -> void:
	_disconnect_server_if_needed()


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
	mode_label.text = "Provider Config"
	controls.add_child(mode_label)

	_provider_mode_option = OptionButton.new()
	_provider_mode_option.item_selected.connect(_on_provider_mode_selected)
	_provider_mode_option.add_item("platform_backed")
	_provider_mode_option.add_item("synthetic_timeline_virtual_time")
	controls.add_child(_provider_mode_option)

	_message_label = Label.new()
	_message_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_message_label.text = "Editor controls call only the CamBANGServer public API."
	add_child(_message_label)

	_status_panel = CamBANGStatusPanel.new()
	_status_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_status_panel)

	_transition_timer = Timer.new()
	_transition_timer.wait_time = TRANSITION_POLL_INTERVAL_SEC
	_transition_timer.one_shot = false
	_transition_timer.autostart = false
	_transition_timer.timeout.connect(_on_transition_timer_timeout)
	add_child(_transition_timer)


func _get_server() -> Object:
	if Engine.has_singleton(SERVER_SINGLETON_NAME):
		return Engine.get_singleton(SERVER_SINGLETON_NAME)
	if has_node("/root/%s" % SERVER_SINGLETON_NAME):
		return get_node("/root/%s" % SERVER_SINGLETON_NAME)
	return null


func _connect_server_if_needed() -> void:
	if _server_connected:
		return

	var server := _get_server()
	if server == null:
		return

	if not server.state_published.is_connected(_on_state_published):
		server.state_published.connect(_on_state_published)

	_server_connected = true


func _disconnect_server_if_needed() -> void:
	if not _server_connected:
		return

	var server := _get_server()
	if server != null and server.state_published.is_connected(_on_state_published):
		server.state_published.disconnect(_on_state_published)

	_server_connected = false


func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	_refresh_from_server()


func _on_transition_timer_timeout() -> void:
	_refresh_from_server()


func _set_transition_polling(enabled: bool) -> void:
	if _transition_timer == null:
		return

	if enabled:
		if _transition_timer.is_stopped():
			_transition_timer.start()
	else:
		if not _transition_timer.is_stopped():
			_transition_timer.stop()


func _refresh_from_server() -> void:
	_build_ui_if_needed()
	_connect_server_if_needed()

	if _status_panel != null:
		_status_panel.force_refresh()

	var server := _get_server()
	if server == null:
		_transition_pending = TRANSITION_NONE
		_set_transition_polling(false)
		_start_button.disabled = true
		_stop_button.disabled = true
		_provider_mode_option.disabled = true
		_message_label.text = "CamBANGServer singleton not available."
		return

	var snapshot: Variant = server.get_state_snapshot()
	var has_snapshot := typeof(snapshot) == TYPE_DICTIONARY
	var running := bool(server.is_running())
	var state := _derive_ui_state(running, has_snapshot)

	match state:
		UI_STATE_RUNNING:
			_start_button.disabled = true
			_stop_button.disabled = false
			_provider_mode_option.disabled = true
			_set_transition_polling(false)
		UI_STATE_STARTING:
			_start_button.disabled = true
			_stop_button.disabled = true
			_provider_mode_option.disabled = true
			_set_transition_polling(true)
		UI_STATE_STOPPING:
			_start_button.disabled = true
			_stop_button.disabled = true
			_provider_mode_option.disabled = true
			_set_transition_polling(true)
		_:
			_start_button.disabled = false
			_stop_button.disabled = true
			_provider_mode_option.disabled = false
			_set_transition_polling(false)

	_message_label.text = _state_message(state)


func _derive_ui_state(running: bool, has_snapshot: bool) -> String:
	if running and has_snapshot:
		_transition_pending = TRANSITION_NONE
		return UI_STATE_RUNNING
	if running:
		return UI_STATE_STARTING

	match _transition_pending:
		TRANSITION_STOPPING:
			_transition_pending = TRANSITION_NONE
			return UI_STATE_STOPPED
		_:
			return UI_STATE_STOPPED


func _state_message(state: String) -> String:
	match state:
		UI_STATE_STOPPED:
			return "Stopped. Provider configuration can be changed."
		UI_STATE_STARTING:
			return "Starting… waiting for baseline snapshot."
		UI_STATE_RUNNING:
			return "Running. Provider configuration is locked until stopped."
		UI_STATE_STOPPING:
			return "Stopping… waiting for snapshot to clear."
		_:
			return "Stopped. Provider configuration can be changed."


func _on_start_pressed() -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot start: CamBANGServer singleton not available."
		return

	if _start_button.disabled:
		return

	_transition_pending = TRANSITION_STARTING
	var selected_mode := _provider_mode_option.get_item_text(_provider_mode_option.selected)
	var err: int = ERR_INVALID_PARAMETER
	if selected_mode == "platform_backed":
		err = server.start(server.PROVIDER_KIND_PLATFORM_BACKED)
	elif selected_mode == "synthetic_timeline_virtual_time":
		err = server.start(server.PROVIDER_KIND_SYNTHETIC, server.SYNTHETIC_ROLE_TIMELINE, server.TIMING_DRIVER_VIRTUAL_TIME)
	else:
		err = server.start()
	_refresh_from_server()
	if err == OK:
		_message_label.text = "Start requested; waiting for baseline snapshot."
	else:
		_message_label.text = "Start request rejected with error %d." % err


func _on_stop_pressed() -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot stop: CamBANGServer singleton not available."
		return

	if _stop_button.disabled:
		return

	_transition_pending = TRANSITION_STOPPING
	server.stop()
	_refresh_from_server()
	_message_label.text = "Stop requested; waiting for snapshot to clear."


func _on_provider_mode_selected(index: int) -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot set provider configuration: CamBANGServer singleton not available."
		return

	if _provider_mode_option.disabled:
		return

	var selected_mode := _provider_mode_option.get_item_text(index)
	_message_label.text = "Selected start provider mode: %s. Press Start Server to apply." % selected_mode
