extends SceneTree

const DEFAULT_VIEWPORT_SIZE := Vector2i(1280, 720)

var _window: Window = null
var _panel: Node = null
var _server: Node = null
var _done := false

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
	if not _setup_panel_with_mock_server("status_panel_nil_after_stop_harness"):
		return

	await _refresh_panel_with_snapshot(_authoritative_snapshot(2, 100, 21, 1))
	if not bool(_panel.get("_last_active_panel_is_authoritative")):
		_fail("panel failed to enter authoritative mode for initial snapshot")
		return

	await _refresh_panel_with_snapshot(null)
	if bool(_panel.get("_last_active_panel_is_authoritative")):
		_fail("panel remained authoritative after NIL/no snapshot")
		return

	var schema_value_label: Label = _panel.get("_schema_version_value")
	if schema_value_label == null or schema_value_label.text != "-":
		_fail("panel did not clear schema-version display after NIL/no snapshot")
		return
	var timestamp_value_label: Label = _panel.get("_timestamp_value")
	if timestamp_value_label == null or not timestamp_value_label.text.begins_with("- "):
		_fail("panel did not clear timestamp display after NIL/no snapshot")
		return
	var last_snapshot_meta: Dictionary = _panel.get("_last_snapshot_meta")
	if not last_snapshot_meta.is_empty():
		_fail("panel did not clear last snapshot metadata after NIL/no snapshot")
		return

	var rendered_model: Variant = _panel.get("_last_panel_model")
	var row_ids := _collect_entry_ids(rendered_model)
	for forbidden_id in ["provider/100", "device/1", "stream/1", "acquisition_session/1"]:
		if row_ids.has(forbidden_id):
			_fail("authoritative row remained visible after NIL reconciliation: %s" % forbidden_id)
			return

	if _retained_generation_list() != [2]:
		_fail("expected retained generations [2] after authoritative->NIL")
		return

	var retained_provider_id := "retained_presentation/prior_authoritative/subtree/0/gen_2/provider/100"
	var retained_provider_entry := _find_entry_by_id(rendered_model, retained_provider_id)
	if retained_provider_entry == null:
		_fail("missing retained continuity provider row after authoritative->NIL")
		return
	if not _entry_has_badge_label(retained_provider_entry, "continuity-only"):
		_fail("retained continuity provider row missing continuity-only badge")
		return
	if not _entry_has_counter(retained_provider_entry, "retained_from_gen", 2):
		_fail("retained continuity provider row missing retained_from_gen=2 counter")
		return

	await _refresh_panel_with_snapshot(_authoritative_snapshot(3, 200, 31, 2))
	if not bool(_panel.get("_last_active_panel_is_authoritative")):
		_fail("panel failed to re-enter authoritative mode")
		return
	var row_ids_after_b := _collect_entry_ids(_panel.get("_last_panel_model"))
	if not row_ids_after_b.has("provider/200"):
		_fail("provider/200 missing after later authoritative refresh")
		return
	if row_ids_after_b.has("provider/100"):
		_fail("provider/100 resurfaced as current truth after later authoritative refresh")
		return
	if _retained_generation_list() != [2]:
		_fail("retained generation ordering/regression failed across NIL->authoritative")
		return

	await _refresh_panel_with_snapshot(_authoritative_snapshot(4, 300, 41, 3))
	if _retained_generation_list() != [3, 2]:
		_fail("expected retained generations [3, 2] after authoritative churn")
		return

	# Deterministic TTL seam usage: manipulate retained_at_msec then invoke policy expiry.
	var retained_after_c: Array = _panel.get("_retained_subtrees")
	if retained_after_c.size() < 2:
		_fail("expected two retained generations before deterministic TTL expiry check")
		return
	retained_after_c[1].retained_at_msec = Time.get_ticks_msec() - 5001
	if not bool(_panel.call("_expire_retained_subtrees_by_policy")):
		_fail("expected retained TTL expiry policy to evict aged retained generation")
		return
	if _retained_generation_list() != [3]:
		_fail("retained TTL expiry removed wrong generation or kept expired entry")
		return

	_ok("OK: status panel nil-after-stop harness PASS")


func _setup_panel_with_mock_server(window_title: String) -> bool:
	_window = Window.new()
	_window.title = window_title
	_window.size = DEFAULT_VIEWPORT_SIZE
	_window.mode = Window.MODE_WINDOWED
	_window.visible = true
	get_root().add_child(_window)

	var panel_script: Variant = load("res://addons/cambang/cambang_status_panel.gd")
	if panel_script == null or not (panel_script is GDScript):
		_fail("failed to load status panel script")
		return false

	var panel: Variant = panel_script.new()
	if panel == null:
		_fail("failed to instantiate status panel")
		return false

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
	return true


func _refresh_panel_with_snapshot(snapshot: Variant) -> void:
	_server.snapshot = snapshot
	_panel.call("_refresh_from_server")
	await process_frame
	await process_frame


func _collect_entry_ids(model: Variant) -> Array[String]:
	var row_ids: Array[String] = []
	if model == null:
		return row_ids
	var entries: Variant = model.get("entries") if model is Object else null
	if typeof(entries) != TYPE_ARRAY:
		return row_ids
	for entry in entries:
		if entry == null:
			continue
		var id_value: Variant = entry.get("id")
		if typeof(id_value) == TYPE_STRING and str(id_value) != "":
			row_ids.append(str(id_value))
	return row_ids


func _find_entry_by_id(model: Variant, row_id: String) -> Variant:
	if model == null:
		return null
	var entries: Variant = model.get("entries") if model is Object else null
	if typeof(entries) != TYPE_ARRAY:
		return null
	for entry in entries:
		if entry == null:
			continue
		if str(entry.get("id", "")) == row_id:
			return entry
	return null


func _entry_has_badge_label(entry: Variant, label: String) -> bool:
	if entry == null:
		return false
	var badges: Variant = entry.get("badges")
	if typeof(badges) != TYPE_ARRAY:
		return false
	for badge in badges:
		if badge == null:
			continue
		if str(badge.get("label", "")) == label:
			return true
	return false


func _entry_has_counter(entry: Variant, counter_name: String, expected_value: int) -> bool:
	if entry == null:
		return false
	var counters: Variant = entry.get("counters")
	if typeof(counters) != TYPE_ARRAY:
		return false
	for counter in counters:
		if counter == null:
			continue
		if str(counter.get("label", "")) == counter_name and int(counter.get("value", -999999)) == expected_value:
			return true
	return false


func _retained_generation_list() -> Array[int]:
	var generations: Array[int] = []
	var retained_states: Array = _panel.get("_retained_subtrees")
	for retained_state in retained_states:
		if retained_state == null:
			continue
		generations.append(int(retained_state.get("retained_from_gen")))
	return generations


func _ok(msg: String) -> void:
	if _done:
		return
	_done = true
	print(msg)
	_quit_with_cleanup(0)


func _fail(msg: String) -> void:
	if _done:
		return
	_done = true
	push_error(msg)
	print(msg)
	_quit_with_cleanup(1)


func _quit_with_cleanup(code: int) -> void:
	if _panel != null and is_instance_valid(_panel):
		_panel.call("_disconnect_server")
	if _window != null and is_instance_valid(_window):
		_window.queue_free()
	if _server != null and is_instance_valid(_server):
		_server.queue_free()
	quit(code)


func _authoritative_snapshot(gen: int, provider_native_id: int, version: int, topology_version: int) -> Dictionary:
	return {
		"schema_version": 1,
		"gen": gen,
		"version": version,
		"topology_version": topology_version,
		"timestamp_ns": 40096504700,
		"imaging_spec_version": 1,
		"scoped_resource_telemetry": [],
		"rigs": [],
		"devices": [{"instance_id": 1, "hardware_id": "synthetic:0", "phase": "LIVE", "mode": "STREAMING", "errors_count": 0}],
		"acquisition_sessions": [{"acquisition_session_id": 1, "device_instance_id": 1, "phase": "LIVE", "capture_profile": {"still": {"version": 0, "width": 0, "height": 0, "format": 0, "still_image_bundle": {"members": []}}}, "captures_triggered": 0, "captures_completed": 0, "captures_failed": 0, "last_capture_id": 0, "last_capture_latency_ns": 0, "error_code": 0}],
		"streams": [{"stream_id": 1, "device_instance_id": 1, "phase": "LIVE", "mode": "FLOWING", "intent": "PREVIEW", "stop_reason": "NONE", "target_fps_max": 30, "frames_received": 1024}],
		"native_objects": [
			{"native_id": provider_native_id, "type": "provider", "phase": "LIVE", "creation_gen": gen, "owner_acquisition_session_id": 0},
			{"native_id": 101, "type": "device", "owner_device_instance_id": 1, "phase": "LIVE", "creation_gen": gen, "owner_acquisition_session_id": 0},
			{"native_id": 104, "type": "acquisition_session", "owner_device_instance_id": 1, "owner_acquisition_session_id": 1, "owner_stream_id": 0, "phase": "LIVE", "creation_gen": gen},
			{"native_id": 102, "type": "stream", "owner_device_instance_id": 1, "owner_acquisition_session_id": 1, "owner_stream_id": 1, "phase": "LIVE", "creation_gen": gen},
			{"native_id": 103, "type": "stream", "owner_device_instance_id": 1, "owner_acquisition_session_id": 1, "owner_stream_id": 1, "phase": "LIVE", "creation_gen": gen}
		],
		"detached_root_ids": []
	}
