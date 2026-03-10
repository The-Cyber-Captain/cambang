@tool
class_name CamBANGEditorDock
extends VBoxContainer

const SERVER_SINGLETON_NAME := "CamBANGServer"

var _status_panel: Control
var _provider_mode_option: OptionButton
var _message_label: Label


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

	var start_button := Button.new()
	start_button.text = "Start Server"
	start_button.pressed.connect(_on_start_pressed)
	controls.add_child(start_button)

	var stop_button := Button.new()
	stop_button.text = "Stop Server"
	stop_button.pressed.connect(_on_stop_pressed)
	controls.add_child(stop_button)

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
		_message_label.text = "CamBANGServer singleton not available."
		_provider_mode_option.disabled = true
		return

	_provider_mode_option.disabled = false
	var mode := str(server.get_provider_mode())
	for index in range(_provider_mode_option.item_count):
		if _provider_mode_option.get_item_text(index) == mode:
			_provider_mode_option.select(index)
			break


func _on_start_pressed() -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot start: CamBANGServer singleton not available."
		return
	server.start()
	_message_label.text = "Start requested via CamBANGServer.start()."
	_refresh_from_server()


func _on_stop_pressed() -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot stop: CamBANGServer singleton not available."
		return
	server.stop()
	_message_label.text = "Stop requested via CamBANGServer.stop()."
	_refresh_from_server()


func _on_provider_mode_selected(index: int) -> void:
	var server := _get_server()
	if server == null:
		_message_label.text = "Cannot set provider mode: CamBANGServer singleton not available."
		return

	var selected_mode := _provider_mode_option.get_item_text(index)
	var err: int = server.set_provider_mode(selected_mode)
	if err == OK:
		_message_label.text = "Provider mode set via CamBANGServer.set_provider_mode('%s')." % selected_mode
	else:
		_message_label.text = "Provider mode change rejected with error %d. Stop the server before changing mode." % err
	_refresh_from_server()
