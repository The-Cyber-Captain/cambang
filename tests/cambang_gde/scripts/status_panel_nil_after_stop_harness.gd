extends SceneTree

const DEFAULT_VIEWPORT_SIZE := Vector2i(1280, 720)

class MockServer:
	extends Node
	signal state_published(gen, version, topology_version)

	var snapshot: Variant = null
	var provider_mode := "synthetic"

	func get_state_snapshot() -> Variant:
		return snapshot

	func get_provider_mode() -> String:
		return provider_mode


func _initialize() -> void:
	var window := Window.new()
	window.title = "status_panel_nil_after_stop_harness"
	window.size = DEFAULT_VIEWPORT_SIZE
	window.mode = Window.MODE_WINDOWED
	window.visible = true
	get_root().add_child(window)

	var panel_script: Variant = load("res://addons/cambang/cambang_status_panel.gd")
	if panel_script == null or not (panel_script is GDScript):
		_fail("failed to load status panel script")
		return

	var panel: Variant = panel_script.new()
	if panel == null:
		_fail("failed to instantiate status panel")
		return

	panel.name = "CamBANGStatusPanel"
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	window.add_child(panel)

	var server := MockServer.new()
	server.name = "MockCamBANGServer"
	get_root().add_child(server)

	await process_frame
	panel.call("_disconnect_server")
	panel.set("_server", server)

	server.snapshot = _authoritative_snapshot()
	panel.call("_refresh_from_server")
	await process_frame

	if not bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("panel failed to enter authoritative mode for initial snapshot")
		return

	server.snapshot = null
	await process_frame
	await process_frame

	if bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("panel remained authoritative after boundary snapshot became NIL")
		return

	var snapshot_state_label: Label = panel.get("_snapshot_state_value")
	if snapshot_state_label == null or snapshot_state_label.text.find("No snapshot") == -1:
		_fail("panel did not update snapshot state label to NIL/no snapshot")
		return

	var rendered_model: Variant = panel.get("_last_panel_model")
	var row_ids := _collect_entry_ids(rendered_model)
	if row_ids.has("provider/100"):
		_fail("authoritative provider row remained visible after NIL reconciliation")
		return
	if row_ids.has("device/1"):
		_fail("authoritative device row remained visible after NIL reconciliation")
		return
	if row_ids.has("stream/1"):
		_fail("authoritative stream row remained visible after NIL reconciliation")
		return

	var retained_states: Array = panel.get("_retained_subtrees")
	if retained_states.is_empty():
		_fail("previous authoritative state was not preserved through retained continuity")
		return

	print("OK: status panel NIL-after-stop reconciliation PASS")
	quit(0)


func _authoritative_snapshot() -> Dictionary:
	return {
		"schema_version": 1,
		"gen": 2,
		"version": 21,
		"topology_version": 1,
		"timestamp_ns": 40096504700,
		"imaging_spec_version": 1,
		"rigs": [],
		"devices": [
			{
				"instance_id": 1,
				"hardware_id": "synthetic:0",
				"phase": "LIVE",
				"mode": "STREAMING",
				"errors_count": 0
			}
		],
		"streams": [
			{
				"stream_id": 1,
				"device_instance_id": 1,
				"phase": "LIVE",
				"mode": "FLOWING",
				"intent": "PREVIEW",
				"stop_reason": "NONE",
				"target_fps_max": 30,
				"frames_received": 1024
			}
		],
		"native_objects": [
			{
				"native_id": 100,
				"type": "provider",
				"phase": "LIVE",
				"creation_gen": 2
			},
			{
				"native_id": 101,
				"type": "device",
				"owner_device_instance_id": 1,
				"phase": "LIVE",
				"creation_gen": 2
			},
			{
				"native_id": 102,
				"type": "stream",
				"owner_device_instance_id": 1,
				"owner_stream_id": 1,
				"phase": "LIVE",
				"creation_gen": 2
			},
			{
				"native_id": 103,
				"type": "frameproducer",
				"owner_device_instance_id": 1,
				"owner_stream_id": 1,
				"phase": "LIVE",
				"creation_gen": 2
			}
		],
		"detached_root_ids": []
	}


func _collect_entry_ids(model: Variant) -> Array[String]:
	var ids: Array[String] = []
	if model == null:
		return ids
	var entries: Array = model.get("entries") if typeof(model) == TYPE_DICTIONARY else model.get("entries")
	for entry in entries:
		if entry == null:
			continue
		ids.append(str(entry.get("id")))
	return ids


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	quit(1)
