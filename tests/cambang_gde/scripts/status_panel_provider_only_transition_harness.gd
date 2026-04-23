extends SceneTree

const DEFAULT_VIEWPORT_SIZE := Vector2i(1280, 720)

var _window: Window = null
var _panel: Node = null
var _server: Node = null

class MockServer:
	extends Node
	signal state_published(gen, version, topology_version)
	const PROVIDER_KIND_PLATFORM_BACKED := 0
	const PROVIDER_KIND_SYNTHETIC := 1
	const SYNTHETIC_ROLE_NOMINAL := 0
	const SYNTHETIC_ROLE_TIMELINE := 1
	const TIMING_DRIVER_REAL_TIME := 0
	const TIMING_DRIVER_VIRTUAL_TIME := 1

	var snapshot: Variant = null
	var active_provider_config: Variant = {
		"provider_kind": PROVIDER_KIND_SYNTHETIC,
		"synthetic_role": SYNTHETIC_ROLE_TIMELINE,
		"timing_driver": TIMING_DRIVER_VIRTUAL_TIME,
		"timeline_reconciliation": null
	}

	func get_state_snapshot() -> Variant:
		return snapshot

	func get_active_provider_config() -> Variant:
		return active_provider_config


func _initialize() -> void:
	_window = Window.new()
	_window.title = "status_panel_provider_only_transition_harness"
	_window.size = DEFAULT_VIEWPORT_SIZE
	_window.mode = Window.MODE_WINDOWED
	_window.visible = true
	get_root().add_child(_window)

	var panel_script: Variant = load("res://addons/cambang/cambang_status_panel.gd")
	if panel_script == null or not (panel_script is GDScript):
		_fail("failed to load status panel script")
		return

	var panel: Variant = panel_script.new()
	if panel == null:
		_fail("failed to instantiate status panel")
		return

	_panel = panel
	panel.name = "CamBANGStatusPanel"
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_window.add_child(panel)

	var server := MockServer.new()
	_server = server
	server.name = "MockCamBANGServer"
	get_root().add_child(server)

	await process_frame
	panel.call("_disconnect_server")
	panel.set("_server", server)

	await _refresh_panel_with_snapshot(panel, server, _provider_pending_snapshot())
	if panel.get("_last_authoritative_panel_model") != null:
		_fail("provider_pending incorrectly seeded _last_authoritative_panel_model")
		return
	if not (panel.get("_last_authoritative_snapshot_meta") as Dictionary).is_empty():
		_fail("provider_pending incorrectly seeded _last_authoritative_snapshot_meta")
		return
	if not (panel.get("_retained_subtrees") as Array).is_empty():
		_fail("provider_pending incorrectly seeded retained history")
		return
	_assert_rows(
		panel.get("_last_panel_model"),
		["server/main", "server/main/provider_pending"],
		["provider/500", "device/1", "stream/1", "native_object/103"]
	)

	await _refresh_panel_with_snapshot(panel, server, _provider_only_snapshot(10, 500))
	if not bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("provider-only authoritative snapshot was not treated as authoritative")
		return
	if not (panel.get("_retained_subtrees") as Array).is_empty():
		_fail("provider-only authoritative snapshot incorrectly created retained history")
		return
	if panel.get("_last_authoritative_panel_model") == null:
		_fail("provider-only authoritative snapshot failed to seed retained-eligible authoritative state")
		return
	if _model_contains_row_id(panel.get("_last_authoritative_panel_model"), "server/main/provider_pending"):
		_fail("provider_pending poisoned retained-eligible authoritative cache")
		return
	_assert_rows(
		panel.get("_last_panel_model"),
		["server/main", "provider/500"],
		["server/main/provider_pending", "device/1", "stream/1", "native_object/103"]
	)

	await _refresh_panel_with_snapshot(panel, server, _realized_snapshot(10, 500))
	_assert_rows(
		panel.get("_last_panel_model"),
		["server/main", "provider/500", "device/1", "acquisition_session/1", "stream/1", "native_object/103"],
		["server/main/provider_pending"]
	)
	if _retained_generation_list(panel) != []:
		_fail("provider-only -> realized transition unexpectedly demoted current-generation state into retained history")
		return

	await _refresh_panel_with_snapshot(panel, server, _realized_snapshot(11, 600))
	if _retained_generation_list(panel) != [10]:
		_fail("realized -> next generation realized did not retain generation 10 subtree")
		return
	_assert_rows(
		panel.get("_last_panel_model"),
		["server/main", "provider/600", "device/1", "acquisition_session/1", "stream/1", "native_object/103"],
		["server/main/provider_pending"]
	)
	if _retained_history_contains_provider_pending(panel):
		_fail("provider_pending leaked into retained history after realized -> next generation realized transition")
		return

	await _refresh_panel_with_snapshot(panel, server, null)
	if bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("panel remained authoritative after realized sequence returned to NIL")
		return
	if _retained_history_contains_provider_pending(panel):
		_fail("provider_pending leaked into retained history across NIL transition")
		return

	print("OK: status panel provider-only transition harness PASS")
	_quit_with_cleanup(0)


func _refresh_panel_with_snapshot(panel: Variant, server: MockServer, snapshot: Variant) -> void:
	server.snapshot = snapshot
	panel.call("_refresh_from_server")
	await process_frame
	await process_frame


func _assert_rows(model: Variant, required: Array[String], forbidden: Array[String]) -> void:
	var row_ids := _collect_entry_ids(model)
	for row_id in required:
		if not row_ids.has(row_id):
			_fail("missing expected row_id=%s; got=%s" % [row_id, row_ids])
			return
	for row_id in forbidden:
		if row_ids.has(row_id):
			_fail("unexpected row_id=%s; got=%s" % [row_id, row_ids])
			return


func _retained_generation_list(panel: Variant) -> Array[int]:
	var generations: Array[int] = []
	var retained_states: Array = panel.get("_retained_subtrees")
	for retained_state in retained_states:
		if retained_state == null:
			continue
		generations.append(int(retained_state.get("retained_from_gen")))
	return generations


func _retained_history_contains_provider_pending(panel: Variant) -> bool:
	var retained_states: Array = panel.get("_retained_subtrees")
	for retained_state in retained_states:
		if retained_state == null:
			continue
		if _model_contains_row_id(retained_state.get("panel_model"), "server/main/provider_pending"):
			return true
	return false


func _model_contains_row_id(model: Variant, row_id: String) -> bool:
	return _collect_entry_ids(model).has(row_id)


func _provider_pending_snapshot() -> Dictionary:
	return {
		"schema_version": 1,
		"gen": 9,
		"version": 0,
		"topology_version": 0,
		"timestamp_ns": 100,
		"imaging_spec_version": 1,
		"rigs": [],
		"devices": [],
		"acquisition_sessions": [],
		"streams": [],
		"native_objects": [],
		"detached_root_ids": []
	}


func _provider_only_snapshot(gen: int, provider_native_id: int) -> Dictionary:
	return {
		"schema_version": 1,
		"gen": gen,
		"version": 0,
		"topology_version": 0,
		"timestamp_ns": 200,
		"imaging_spec_version": 1,
		"rigs": [],
		"devices": [],
		"acquisition_sessions": [],
		"streams": [],
		"native_objects": [
			{
				"native_id": provider_native_id,
				"type": "provider",
				"phase": "LIVE",
				"creation_gen": gen,
				"owner_acquisition_session_id": 0
			}
		],
		"detached_root_ids": []
	}


func _realized_snapshot(gen: int, provider_native_id: int) -> Dictionary:
	return {
		"schema_version": 1,
		"gen": gen,
		"version": 3,
		"topology_version": 2,
		"timestamp_ns": 300,
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
		"acquisition_sessions": [
			{
				"acquisition_session_id": 1,
				"device_instance_id": 1,
				"phase": "LIVE",
				"capture_profile_version": 0,
				"capture_width": 0,
				"capture_height": 0,
				"capture_format": 0,
				"captures_triggered": 0,
				"captures_completed": 0,
				"captures_failed": 0,
				"last_capture_id": 0,
				"last_capture_latency_ns": 0,
				"error_code": 0
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
				"frames_received": 1
			}
		],
		"native_objects": [
			{
				"native_id": provider_native_id,
				"type": "provider",
				"phase": "LIVE",
				"creation_gen": gen,
				"owner_acquisition_session_id": 0
			},
			{
				"native_id": 101,
				"type": "device",
				"owner_device_instance_id": 1,
				"phase": "LIVE",
				"creation_gen": gen,
				"owner_acquisition_session_id": 0
			},
			{
				"native_id": 104,
				"type": "acquisition_session",
				"owner_device_instance_id": 1,
				"owner_acquisition_session_id": 1,
				"owner_stream_id": 0,
				"phase": "LIVE",
				"creation_gen": gen
			},
			{
				"native_id": 102,
				"type": "stream",
				"owner_device_instance_id": 1,
				"owner_acquisition_session_id": 1,
				"owner_stream_id": 1,
				"phase": "LIVE",
				"creation_gen": gen
			},
			{
				"native_id": 103,
				"type": "stream",
				"owner_device_instance_id": 1,
				"owner_acquisition_session_id": 1,
				"owner_stream_id": 1,
				"phase": "LIVE",
				"creation_gen": gen
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
	_quit_with_cleanup(1)


func _quit_with_cleanup(code: int) -> void:
	if _panel != null and is_instance_valid(_panel):
		_panel.queue_free()
	if _server != null and is_instance_valid(_server):
		_server.queue_free()
	if _window != null and is_instance_valid(_window):
		_window.queue_free()
	quit(code)
