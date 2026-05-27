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
	_window.title = "status_panel_nil_after_stop_harness"
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

	await _refresh_panel_with_snapshot(panel, server, _authoritative_snapshot(2, 100, 21, 1))

	if not bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("panel failed to enter authoritative mode for initial snapshot")
		return

	server.snapshot = null
	await process_frame
	await process_frame

	if bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("panel remained authoritative after boundary snapshot became NIL")
		return

	var last_snapshot_meta: Dictionary = panel.get("_last_snapshot_meta")
	if not last_snapshot_meta.is_empty():
		_fail("panel did not clear last snapshot metadata after NIL/no snapshot")
		return

	var rendered_model: Variant = panel.get("_last_panel_model")
	var row_ids: Array[String] = _collect_entry_ids(rendered_model)
	if not row_ids.has("server/main"):
		_fail("server/main missing after NIL/no snapshot")
		return
	var server_main_entry: Variant = _find_entry(rendered_model, "server/main")
	if server_main_entry == null:
		_fail("missing server/main entry object after NIL/no snapshot")
		return
	if not _entry_has_badge(server_main_entry, "NO SNAPSHOT"):
		_fail("server/main missing NO SNAPSHOT badge after NIL/no snapshot")
		return
	if _entry_has_counter(server_main_entry, "schema_version"):
		_fail("server/main still exposes schema_version counter after NIL/no snapshot")
		return
	if _entry_has_counter(server_main_entry, "imaging_spec_version"):
		_fail("server/main still exposes imaging_spec_version counter after NIL/no snapshot")
		return
	if row_ids.has("provider/100"):
		_fail("authoritative provider row remained visible after NIL reconciliation")
		return
	if row_ids.has("device/1"):
		_fail("authoritative device row remained visible after NIL reconciliation")
		return
	if row_ids.has("stream/1"):
		_fail("authoritative stream row remained visible after NIL reconciliation")
		return
	if row_ids.has("acquisition_session/1"):
		_fail("authoritative acquisition_session row remained visible after NIL reconciliation")
		return

	var retained_states: Array = panel.get("_retained_subtrees")
	if retained_states.is_empty():
		_fail("previous authoritative state was not preserved through retained continuity")
		return

	if _retained_generation_list(panel) != [2]:
		_fail("expected exactly one retained generation [2] after authoritative→NIL transition")
		return

	var retained_provider_entry: Variant = _find_entry(
		rendered_model,
		"retained_presentation/prior_authoritative/subtree/0/gen_2/provider/100"
	)
	if retained_provider_entry == null:
		_fail("expected retained continuity provider row after authoritative→NIL transition")
		return
	if not _entry_has_badge(retained_provider_entry, "continuity-only"):
		_fail("retained continuity provider row missing continuity-only badge")
		return
	if not _entry_info_contains(retained_provider_entry, "Continuity-only retained view"):
		_fail("retained continuity provider row missing continuity-only wording")
		return
	if not _entry_info_contains(retained_provider_entry, "copied from a previously rendered authoritative panel"):
		_fail("retained continuity provider row missing retained-provenance wording")
		return
	if not _entry_info_contains(retained_provider_entry, "not active snapshot truth"):
		_fail("retained continuity provider row missing not-active-snapshot-truth wording")
		return

	await _refresh_panel_with_snapshot(panel, server, _authoritative_snapshot(3, 200, 31, 2))
	if not bool(panel.get("_last_active_panel_is_authoritative")):
		_fail("panel failed to re-enter authoritative mode for later snapshot B")
		return

	var row_ids_after_b: Array[String] = _collect_entry_ids(panel.get("_last_panel_model"))
	if not row_ids_after_b.has("provider/200"):
		_fail("current authoritative provider/200 missing after later authoritative refresh")
		return
	if row_ids_after_b.has("provider/100"):
		_fail("authoritative provider/100 reappeared as current truth after later authoritative refresh")
		return
	if _retained_generation_list(panel) != [2]:
		_fail("generation 2 was demoted more than once across NIL/non-authoritative transition")
		return

	await _refresh_panel_with_snapshot(panel, server, _authoritative_snapshot(4, 300, 41, 3))
	var retained_generations_after_c := _retained_generation_list(panel)
	if retained_generations_after_c != [3, 2]:
		_fail("expected retained generations [3, 2] newest-first after authoritative A->B->C churn; got %s" % [retained_generations_after_c])
		return

	var retained_after_c: Array = panel.get("_retained_subtrees")
	if retained_after_c.size() < 2:
		_fail("expected two retained generations before TTL expiry check")
		return
	retained_after_c[1].retained_at_msec = Time.get_ticks_msec() - 5001
	if not bool(panel.call("_expire_retained_subtrees_by_policy")):
		_fail("expected retained TTL expiry to remove the aged retained generation")
		return
	if _retained_generation_list(panel) != [3]:
		_fail("retained TTL expiry removed the wrong generation or left expired history visible")
		return

	print("OK: status panel retained lifecycle reconciliation PASS")
	_quit_with_cleanup(0)


func _refresh_panel_with_snapshot(panel: Variant, server: MockServer, snapshot: Variant) -> void:
	server.snapshot = snapshot
	panel.call("_refresh_from_server")
	await process_frame
	await process_frame


func _retained_generation_list(panel: Variant) -> Array[int]:
	var generations: Array[int] = []
	var retained_states: Array = panel.get("_retained_subtrees")
	for retained_state in retained_states:
		if retained_state == null:
			continue
		generations.append(int(retained_state.get("retained_from_gen")))
	return generations


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
				"capture_profile": {"still": {"version": 0, "width": 0, "height": 0, "format": 0, "still_image_bundle": {"members": []}}},
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
				"frames_received": 1024
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
	var entries: Array = _extract_entries(model)
	for entry in entries:
		var entry_id := _extract_entry_id(entry)
		if entry_id != "":
			ids.append(entry_id)
	return ids


func _extract_entries(model: Variant) -> Array:
	if model == null:
		return []
	if typeof(model) == TYPE_ARRAY:
		return model
	if typeof(model) == TYPE_DICTIONARY:
		var value: Variant = model.get("entries", [])
		return value if typeof(value) == TYPE_ARRAY else []
	if model is Object:
		for prop in model.get_property_list():
			if str(prop.get("name", "")) == "entries":
				var value: Variant = model.get("entries")
				return value if typeof(value) == TYPE_ARRAY else []
	return []


func _extract_entry_id(entry: Variant) -> String:
	if entry == null:
		return ""
	if typeof(entry) == TYPE_DICTIONARY:
		return str(entry.get("id", ""))
	if entry is Object:
		for prop in entry.get_property_list():
			if str(prop.get("name", "")) == "id":
				return str(entry.get("id"))
	return ""


func _find_entry(model: Variant, row_id: String) -> Variant:
	for entry in _extract_entries(model):
		if _extract_entry_id(entry) == row_id:
			return entry
	return null


func _entry_has_badge(entry: Variant, badge_text: String) -> bool:
	var badges: Variant = _extract_variant_field(entry, "badges", [])
	if typeof(badges) != TYPE_ARRAY:
		return false
	for badge in badges:
		if str(_extract_variant_field(badge, "label", "")) == badge_text:
			return true
	return false


func _entry_info_contains(entry: Variant, substring: String) -> bool:
	var info_lines: Variant = _extract_variant_field(entry, "info_lines", [])
	if typeof(info_lines) != TYPE_ARRAY:
		return false
	for line in info_lines:
		if str(line).findn(substring) != -1:
			return true
	return false


func _entry_has_counter(entry: Variant, counter_name: String) -> bool:
	var counters: Variant = _extract_variant_field(entry, "counters", [])
	if typeof(counters) != TYPE_ARRAY:
		return false
	for counter in counters:
		if str(_extract_variant_field(counter, "name", "")) == counter_name:
			return true
	return false


func _extract_variant_field(entry: Variant, field_name: String, fallback: Variant = null) -> Variant:
	if entry == null:
		return fallback
	if typeof(entry) == TYPE_DICTIONARY:
		return entry.get(field_name, fallback)
	if entry is Object:
		for prop in entry.get_property_list():
			if str(prop.get("name", "")) == field_name:
				return entry.get(field_name)
	return fallback


func _fail(message: String) -> void:
	push_error(message)
	print("FAIL: %s" % message)
	_quit_with_cleanup(1)


func _quit_with_cleanup(code: int) -> void:
	if _panel != null and is_instance_valid(_panel):
		_panel.call("_disconnect_server")
	if _window != null and is_instance_valid(_window):
		_window.queue_free()
	if _server != null and is_instance_valid(_server):
		_server.queue_free()
	quit(code)
