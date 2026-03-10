@tool
extends EditorPlugin

const DOCK_SCRIPT := preload("res://addons/cambang_editor/cambang_editor_dock.gd")
const SERVER_SINGLETON_NAME := "CamBANGServer"

var _dock: Control


func _enter_tree() -> void:
	if _dock == null:
		_dock = DOCK_SCRIPT.new()
	add_control_to_dock(DOCK_SLOT_RIGHT_UL, _dock)
	_dock.refresh_from_server()


func _exit_tree() -> void:
	_stop_server_for_editor_boundary()
	if _dock != null:
		remove_control_from_docks(_dock)
		_dock.queue_free()
		_dock = null


func _build() -> bool:
	_stop_server_for_editor_boundary()
	if _dock != null:
		_dock.refresh_from_server()
	return true


func _stop_server_for_editor_boundary() -> void:
	var server := _get_server()
	if server != null:
		server.stop()


func _get_server() -> Object:
	if Engine.has_singleton(SERVER_SINGLETON_NAME):
		return Engine.get_singleton(SERVER_SINGLETON_NAME)
	if get_tree() != null and get_tree().root != null and get_tree().root.has_node(SERVER_SINGLETON_NAME):
		return get_tree().root.get_node(SERVER_SINGLETON_NAME)
	return null
