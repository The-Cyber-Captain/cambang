extends Node

const SCENE_LABEL := "82_capture_gpu_backing_verify"
const CAPTURE_WAIT_MS := 10000

var _announcements := {}
var _finished := {}
var _timing_was_early := {}


func _ready() -> void:
	CamBANGServer.capture_image_acquired.connect(_on_acquired)
	CamBANGServer.capture_finished.connect(_on_finished)
	await _run()


func _on_acquired(capture_id: String, info: Dictionary) -> void:
	if int(info.get("image_member_index", -1)) == 0:
		_announcements[capture_id] = info.get("acquisition_timing", {})
		_timing_was_early[capture_id] = not _finished.has(capture_id)


func _on_finished(capture_id: String, _info: Dictionary) -> void:
	_finished[capture_id] = true


func _finish(reason: String = "") -> void:
	var code := 0 if reason.is_empty() else 1
	var status := "ok" if code == 0 else "fail"
	var renderer := RenderingServer.get_current_rendering_method()
	var detail := "renderer=%s device=%s" % [renderer, OS.get_model_name()] if code == 0 else reason
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, code, detail.replace(" ", "_")])
	CamBANGServer.stop()
	get_tree().quit(code)


func _capture(device: CamBANGDevice, keep_gpu: bool) -> CamBANGCaptureResult:
	var deadline := Time.get_ticks_msec() + CAPTURE_WAIT_MS
	var receipt: Dictionary = {}
	while Time.get_ticks_msec() < deadline:
		receipt = device.trigger_capture({"keep_gpu_backing": true}) if keep_gpu else device.trigger_capture()
		if int(receipt.get("error", FAILED)) != ERR_BUSY:
			break
		await get_tree().process_frame
	if int(receipt.get("error", FAILED)) != OK:
		_finish("capture trigger failed: %s" % [receipt])
		return null
	var id := str(receipt["id"])
	while not _finished.has(id) and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	var result := CamBANGServer.get_capture_result_by_id(id)
	if not _finished.has(id) or result == null:
		_finish("capture did not settle with a result: %s" % id)
		return null
	if OS.get_name() == "Android":
		var timing: Dictionary = result.get_image_member(0).get("camera_facts", {}).get("acquisition_timing", {})
		if timing.is_empty() or _announcements.get(id, {}) != timing or not _timing_was_early.get(id, false):
			_finish("timing did not match the exact capture member: %s" % id)
			return null
	return result


func _run() -> void:
	var android := OS.get_name() == "Android"
	var error: int
	if android:
		error = CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED)
	else:
		error = CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
			CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME, CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED)
	if error != OK:
		_finish("server start failed: %d" % error)
		return
	var deadline := Time.get_ticks_msec() + CAPTURE_WAIT_MS
	while CamBANGServer.get_state_snapshot() == null and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	var device: CamBANGDevice
	if android:
		device = CamBANGServer.get_device_for_hardware_id("50")
		if device != null:
			error = device.engage()
			while error == ERR_BUSY and Time.get_ticks_msec() < deadline:
				await get_tree().process_frame
				error = device.engage()
			if error != OK:
				_finish("Quest still camera did not engage: %d" % error)
				return
	else:
		if CamBANGServer.select_builtin_scenario("stream_inspection_live") != OK or CamBANGServer.start_scenario() != OK:
			_finish("synthetic scenario did not start")
			return
		while device == null and Time.get_ticks_msec() < deadline:
			await get_tree().process_frame
			var snapshot = CamBANGServer.get_state_snapshot()
			if snapshot == null:
				continue
			for candidate in snapshot.get("devices", []):
				device = CamBANGServer.get_device(int(candidate.get("instance_id", 0)))
				if device != null:
					break
	if device == null:
		_finish("test device unavailable")
		return
	if android:
		var profile := {"width": 1280, "height": 1280, "format_fourcc": CamBANGServer.PIXEL_FORMAT_NV12}
		error = device.set_still_capture_profile(profile)
		if error != OK:
			_finish("Quest still profile refused: %d" % error)
			return
		while int(device.get_still_capture_profile().get("width", 0)) != 1280 and Time.get_ticks_msec() < deadline:
			await get_tree().process_frame
	var invalid := device.trigger_capture({"keep_gpu_backing": 1})
	if int(invalid.get("error", FAILED)) != ERR_INVALID_PARAMETER or not str(invalid.get("id", "")).is_empty():
		_finish("invalid capture options accepted")
		return

	var baseline := await _capture(device, false)
	if baseline == null:
		return
	if baseline.to_image() == null or int(baseline.claim_gpu_backing()["error"]) != ERR_UNAVAILABLE:
		_finish("ordinary capture changed behavior")
		return
	var opted := await _capture(device, true)
	if opted == null:
		return
	var claim := opted.claim_gpu_backing()
	if not android:
		if int(claim["error"]) != ERR_UNAVAILABLE or claim["backing"] != null or opted.to_image() == null:
			_finish("unsupported provider did not preserve the ordinary result")
			return
		_finish()
		return
	if int(claim["error"]) != OK or not claim["backing"] is CamBANGGpuBacking:
		_finish("Quest backing could not be claimed: %s" % [claim])
		return
	if int(opted.claim_gpu_backing(0)["error"]) != ERR_ALREADY_IN_USE or 			int(opted.claim_gpu_backing(1)["error"]) != ERR_INVALID_PARAMETER:
		_finish("claim rules did not follow capture member identity")
		return
	if opted.to_image() == null:
		_finish("opt-in capture lost ordinary CPU pixels")
		return
	print("STEP OK: acquisition timing matched both results; GPU backing claimed exactly once")
	var later := await _capture(device, true)
	if later == null:
		return
	await get_tree().create_timer(5.5).timeout
	if int(later.claim_gpu_backing()["error"]) != ERR_TIMEOUT or later.to_image() == null:
		_finish("unclaimed GPU backing did not expire independently of the ordinary result")
		return
	print("STEP OK: unclaimed backing expired while ordinary pixels remained")
	_finish()
