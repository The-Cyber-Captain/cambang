extends Node

const MAX_FRAMES := 180
const SCENE_LABEL := "66_public_lifecycle_verify"
#const FOURCC_RGBA := 1094862674

var _done := false
var _quit_requested := false
var _terminal_verdict_emitted := false
var _handle_a = null
var _handle_b = null
var _stream = null
var _endpoints: Array = []
var _published_count := 0


func _ready() -> void:
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()

	if not CamBANGServer.has_method("enumerate_devices"):
		_fail("FAIL: CamBANGServer.enumerate_devices() missing")
		return
	if not CamBANGServer.has_method("get_device_for_hardware_id"):
		_fail("FAIL: CamBANGServer.get_device_for_hardware_id() missing")
		return

	var start_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)
	if start_err != OK:
		_error("ERROR: start(SYNTHETIC) rejected", "runtime_start_rejected")
		return


	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)
	var baseline_ready := await _wait_for_baseline()
	if not baseline_ready:
		return
	_endpoints = CamBANGServer.enumerate_devices()
	if typeof(_endpoints) != TYPE_ARRAY or _endpoints.size() < 1:
		_fail("FAIL: synthetic enumerate_devices() must return at least one endpoint")
		return
	var endpoint0 = _endpoints[0]
	if typeof(endpoint0) != TYPE_DICTIONARY:
		_fail("FAIL: enumerate_devices() entries must be Dictionary")
		return
	var hardware_id := str(endpoint0.get("hardware_id", ""))
	if hardware_id.is_empty():
		_fail("FAIL: enumerate_devices() endpoint hardware_id must be non-empty")
		return

	_handle_a = CamBANGServer.get_device_for_hardware_id(hardware_id)
	_handle_b = CamBANGServer.get_device_for_hardware_id(hardware_id)
	if _handle_a == null or _handle_b == null:
		_fail("FAIL: get_device_for_hardware_id() must return non-null handles for known hardware_id")
		return
	if not _handle_a.has_method("engage") or not _handle_a.has_method("disengage") or not _handle_a.has_method("create_stream") or not _handle_a.has_method("set_warm_policy"):
		_fail("FAIL: endpoint CamBANGDevice lifecycle methods missing")
		return
	if _handle_a.set_warm_policy({"warm_hold_ms": 1500}) == OK:
		_fail("FAIL: pre-engage set_warm_policy() must not return OK")
		return
	if str(_handle_a.get_hardware_id()) != hardware_id or str(_handle_b.get_hardware_id()) != hardware_id:
		_fail("FAIL: endpoint handles must expose matching hardware_id")
		return
	if int(_handle_a.get_instance_id()) != 0 or int(_handle_b.get_instance_id()) != 0:
		_fail("FAIL: endpoint handles must have instance_id == 0")
		return

	var engage_a_err := ERR_BUSY
	for _engage_i in range(MAX_FRAMES):
		engage_a_err = _handle_a.engage()
		if engage_a_err == OK:
			break
		if engage_a_err != ERR_BUSY and engage_a_err != ERR_UNAVAILABLE:
			break
		await get_tree().process_frame
	if engage_a_err != OK:
		_fail("FAIL: endpoint handle engage() must return OK for known synthetic endpoint")
		return

	var engaged_instance_id := int(_handle_a.get_instance_id())
	if engaged_instance_id == 0:
		_fail("FAIL: endpoint handle get_instance_id() must become nonzero after engage()")
		return
	if int(_handle_b.get_instance_id()) != engaged_instance_id:
		_fail("FAIL: endpoint handles for same hardware_id must resolve the same instance id after engage()")
		return
	if _handle_b.engage() != OK:
		_fail("FAIL: second endpoint handle engage() must return OK for already engaged endpoint")
		return
	if int(_handle_b.get_instance_id()) != engaged_instance_id:
		_fail("FAIL: second endpoint handle engage() must not change resolved instance id")
		return

	var realized_in_snapshot := false
	for _realize_i in range(MAX_FRAMES):
		var realize_snap = CamBANGServer.get_state_snapshot()
		if typeof(realize_snap) == TYPE_DICTIONARY:
			var realize_devices = realize_snap.get("devices", [])
			if typeof(realize_devices) == TYPE_ARRAY:
				for realize_dev in realize_devices:
					if typeof(realize_dev) != TYPE_DICTIONARY:
						continue
					if int(realize_dev.get("instance_id", -1)) != engaged_instance_id:
						continue
					var has_engaged: bool = bool(realize_dev.has("engaged"))
					var has_phase: bool = bool(realize_dev.has("phase"))
					if has_engaged and bool(realize_dev.get("engaged", false)):
						realized_in_snapshot = true
					elif has_phase and str(realize_dev.get("phase", "")) == "LIVE":
						realized_in_snapshot = true
					elif not has_engaged and not has_phase:
						# Fallback precondition for older snapshot shapes: matching row presence.
						realized_in_snapshot = true
					if realized_in_snapshot:
						break
		if realized_in_snapshot:
			break
		await get_tree().process_frame
	if not realized_in_snapshot:
		_fail("FAIL: snapshot must report engaged device before set_warm_policy")
		return

	var warm_set_err: int = int(_handle_a.set_warm_policy({"warm_hold_ms": 1500}))
	if warm_set_err != OK:
		_fail("FAIL: set_warm_policy({\"warm_hold_ms\":1500}) must return OK after engage (err=%d)" % warm_set_err)
		return
	print("INFO: warm policy set accepted for instance_id=%d" % engaged_instance_id)

	var publish_baseline := _published_count
	for _publish_i in range(MAX_FRAMES):
		if _published_count > publish_baseline:
			break
		await get_tree().process_frame

	var warm_visible := false
	var last_warm_hold_ms := -1
	var last_has_warm_remaining := false
	var last_matching_device := {}
	var last_has_engaged_key := false
	var last_has_phase_key := false
	var last_has_open_key := false
	var last_snap_gen := -1
	var last_snap_version := -1
	var last_snap_topology := -1
	for _warm_i in range(MAX_FRAMES):
		var warm_snap = CamBANGServer.get_state_snapshot()
		if typeof(warm_snap) == TYPE_DICTIONARY:
			last_snap_gen = int(warm_snap.get("gen", -1))
			last_snap_version = int(warm_snap.get("version", -1))
			last_snap_topology = int(warm_snap.get("topology_version", -1))
			var warm_devices = warm_snap.get("devices", [])
			if typeof(warm_devices) == TYPE_ARRAY:
				for warm_dev in warm_devices:
					if typeof(warm_dev) == TYPE_DICTIONARY and int(warm_dev.get("instance_id", -1)) == engaged_instance_id:
						last_matching_device = warm_dev
						last_warm_hold_ms = int(warm_dev.get("warm_hold_ms", -1))
						last_has_warm_remaining = warm_dev.has("warm_remaining_ms")
						last_has_engaged_key = warm_dev.has("engaged")
						last_has_phase_key = warm_dev.has("phase")
						last_has_open_key = warm_dev.has("open")
						if int(warm_dev.get("warm_hold_ms", -1)) == 1500 and warm_dev.has("warm_remaining_ms"):
							warm_visible = true
							break
		if warm_visible:
			break
		await get_tree().process_frame
	if not warm_visible:
		var warm_fail_message: String = (
			"FAIL: snapshot must eventually report warm_hold_ms=1500 for engaged device"
			+ " instance_id=" + str(engaged_instance_id)
			+ " last_warm_hold_ms=" + str(last_warm_hold_ms)
			+ " has_warm_remaining_ms=" + str(last_has_warm_remaining)
			+ " has_engaged_key=" + str(last_has_engaged_key)
			+ " has_phase_key=" + str(last_has_phase_key)
			+ " has_open_key=" + str(last_has_open_key)
			+ " last_snap(gen=" + str(last_snap_gen)
			+ ",version=" + str(last_snap_version)
			+ ",topology=" + str(last_snap_topology)
			+ ") last_device=" + str(last_matching_device)
		)
		_fail(warm_fail_message)
		return

	if _handle_a.create_stream({"intent": "NOT_A_STREAM_INTENT"}) != null:
		_fail("FAIL: create_stream() with invalid Stream Definition intent must return null")
		return
	if int(CamBANGStream.INTENT_VIEWFINDER) != 1:
		_fail("FAIL: CamBANGStream.INTENT_VIEWFINDER constant must be 1")
		return
	_stream = _handle_a.create_stream({
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": 640,
			"height": 360,
			"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
			"target_fps": 15,
		},
	})
	if _stream == null:
		_fail("FAIL: create_stream(definition) must return a non-null CamBANGStream handle for engaged endpoint")
		return
	if not _stream.has_method("get_stream_id") or not _stream.has_method("get_device_instance_id") or not _stream.has_method("get_hardware_id") or not _stream.has_method("is_valid_stream_handle"):
		_fail("FAIL: CamBANGStream handle missing required identity methods")
		return
	if int(_stream.get_stream_id()) == 0:
		_fail("FAIL: CamBANGStream.get_stream_id() must be nonzero")
		return
	if int(_stream.get_device_instance_id()) != engaged_instance_id:
		_fail("FAIL: CamBANGStream.get_device_instance_id() must match engaged device instance id")
		return
	if str(_stream.get_hardware_id()) != hardware_id:
		_fail("FAIL: CamBANGStream.get_hardware_id() must match endpoint hardware_id")
		return
	if not bool(_stream.is_valid_stream_handle()):
		_fail("FAIL: CamBANGStream.is_valid_stream_handle() must return true")
		return
	var stream_state := await _wait_for_stream_state(int(_stream.get_stream_id()))
	if stream_state.is_empty():
		_fail("FAIL: snapshot must expose stream created from Stream Definition")
		return
	if str(stream_state.get("intent", "")) != "VIEWFINDER":
		_fail("FAIL: Stream Definition intent must apply as VIEWFINDER; got %s" % str(stream_state.get("intent", "")))
		return
	if int(stream_state.get("width", -1)) != 640 or int(stream_state.get("height", -1)) != 360:
		_fail("FAIL: Stream Definition profile dimensions must apply; got %sx%s" % [str(stream_state.get("width", "?")), str(stream_state.get("height", "?"))])
		return
	if int(stream_state.get("format", -1)) != CamBANGServer.PIXEL_FORMAT_RGBA:
		_fail("FAIL: Stream Definition format_fourcc must apply")
		return
	if int(stream_state.get("target_fps_min", -1)) != 15 or int(stream_state.get("target_fps_max", -1)) != 15:
		_fail("FAIL: Stream Definition target_fps must apply to min/max")
		return
	if _stream.start() != OK:
		_fail("FAIL: CamBANGStream.start() must return OK")
		return
	if _stream.start() != OK:
		_fail("FAIL: CamBANGStream.start() second call must be idempotent and return OK")
		return
	if _stream.stop() != OK:
		_fail("FAIL: CamBANGStream.stop() must return OK")
		return
	if _stream.stop() != OK:
		_fail("FAIL: CamBANGStream.stop() second call must be idempotent and return OK")
		return
	if _stream.destroy() != OK:
		_fail("FAIL: CamBANGStream.destroy() must return OK")
		return
	if _stream.destroy() != OK:
		_fail("FAIL: CamBANGStream.destroy() second call must be idempotent and return OK")
		return
	if bool(_stream.is_valid_stream_handle()):
		_fail("FAIL: CamBANGStream.is_valid_stream_handle() must return false after destroy")
		return

	_stream = null
	if _handle_a.disengage() != OK:
		_fail("FAIL: endpoint handle disengage() must return OK")
		return
	if _handle_b.disengage() != OK:
		_fail("FAIL: second endpoint handle disengage() must return OK")
		return

	var disengaged_to_zero := false
	for _close_i in range(MAX_FRAMES):
		if int(_handle_a.get_instance_id()) == 0 and int(_handle_b.get_instance_id()) == 0:
			disengaged_to_zero = true
			break
		await get_tree().process_frame
	if not disengaged_to_zero:
		_fail("FAIL: endpoint handles must eventually resolve get_instance_id() == 0 after disengage close truth")
		return

	_ok("OK: godot public lifecycle verify PASS")


func _wait_for_stream_state(stream_id: int) -> Dictionary:
	for _i in range(MAX_FRAMES):
		var snap = CamBANGServer.get_state_snapshot()
		if typeof(snap) == TYPE_DICTIONARY:
			var streams = snap.get("streams", [])
			if typeof(streams) == TYPE_ARRAY:
				for stream_state in streams:
					if typeof(stream_state) == TYPE_DICTIONARY and int(stream_state.get("stream_id", -1)) == stream_id:
						return stream_state
		await get_tree().process_frame
	return {}


func _wait_for_baseline() -> bool:
	for _i in range(MAX_FRAMES):
		var snap = CamBANGServer.get_state_snapshot()
		if typeof(snap) == TYPE_DICTIONARY and int(snap.get("version", -1)) == 0 and int(snap.get("topology_version", -1)) == 0:
			return true
		await get_tree().process_frame
	_fail("FAIL: timed out waiting for initial baseline before lifecycle command use", "timeout")
	return false


func _ok(msg: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("ok", 0, "pass")
	print(msg)
	_cleanup_and_quit(0)


func _fail(msg: String, reason: String = "assertion_failed") -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("fail", 1, reason)
	push_error(msg)
	print(msg)
	_cleanup_and_quit(1)


func _error(msg: String, reason: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("error", 1, reason)
	push_error(msg)
	print(msg)
	_cleanup_and_quit(1)


func _emit_harness_verdict(status: String, exit_code: int, reason: String) -> void:
	if _terminal_verdict_emitted:
		return
	_terminal_verdict_emitted = true
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL,
		status,
		exit_code,
		reason,
	])


func _cleanup_and_quit(code: int) -> void:
	set_process(false)
	_stream = null
	_handle_a = null
	_handle_b = null
	_endpoints = []
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	if not _quit_requested:
		_quit_requested = true
		CamBANGServer.stop_and_quit(code)



func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	_published_count += 1
