extends Node

## Scene 78: an endpoint handle is canonical across stop()/start().
##
## capture_identity_and_lifecycle.md 4.2: canonical means for as long as the id
## means the same thing, and a hardware_id names the same camera in every
## session. This scene holds one CamBANGDevice from get_device_for_hardware_id()
## through a restart and proves:
## - get_device_for_hardware_id() returns that same object after the restart,
## - its live_changed and capture_finished connections carry over,
## - it forgot the previous session's capture (get_result() is null until a
##   capture exists in the new session),
## - each capture is reported once on it, in each session.
##
## Before this, stop() minted a second wrapper for the camera while the caller's
## original stayed tracked, so a caller connected to both saw every signal twice
## and `old == new` was false.
##
## Synthetic-only, self-terminating: emits [CamBANG][HarnessVerdict].

const SCENE_LABEL := "78_handle_identity_across_restart"
const HW := "synthetic:0"
const TIMEOUT_MS := 5000

var _verdict_emitted := false
var _finished_ids: Array[String] = []
var _live_true_count := 0


func _ready() -> void:
	print("RUN: %s" % SCENE_LABEL)
	await _run()


func _emit(status: String, reason: String) -> void:
	if _verdict_emitted:
		return
	_verdict_emitted = true
	var code := 0 if status == "ok" else 1
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [SCENE_LABEL, status, code, reason])
	CamBANGServer.stop()
	get_tree().quit(code)


func _fail(reason: String) -> void:
	_emit("fail", reason)


func _exit_tree() -> void:
	CamBANGServer.stop()


func _on_capture_finished(capture_id: String, _disposition: int, _error_code: int) -> void:
	_finished_ids.append(capture_id)


func _on_live_changed(live: bool) -> void:
	if live:
		_live_true_count += 1


func _start_session(which: String) -> bool:
	if int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)) != OK:
		_fail("%s: start(synthetic) failed" % which)
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() != null:
			return true
	_fail("%s: no baseline snapshot" % which)
	return false


func _engage_until_live(dev, which: String) -> bool:
	if int(dev.engage()) != OK:
		_fail("%s: engage() refused" % which)
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if bool(dev.is_live()):
			return true
	_fail("%s: device never became live" % which)
	return false


# Triggers one capture and waits for its completion on the held handle. Returns
# the capture id, or "" after failing the scene.
func _capture_once(dev, which: String) -> String:
	var trigger: Dictionary = dev.trigger_capture()
	var err := int(trigger.get("error", FAILED))
	var id := str(trigger.get("id", ""))
	# Synthetic may still be finishing its first frames; retry briefly on BUSY.
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while err == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		trigger = dev.trigger_capture()
		err = int(trigger.get("error", FAILED))
		id = str(trigger.get("id", ""))
	if err != OK or id.is_empty():
		_fail("%s: trigger_capture() err=%d" % [which, err])
		return ""
	deadline = Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if _finished_ids.has(id):
			return id
	_fail("%s: capture_finished never arrived on the held handle for %s" % [which, id])
	return ""


# A few frames more, so a second emission for the same capture would be seen.
func _settle_frames(n: int) -> void:
	for _i in range(n):
		await get_tree().process_frame


func _run() -> void:
	CamBANGServer.stop()

	# --- Session 1 -------------------------------------------------------------
	if not await _start_session("session 1"):
		return
	var held = CamBANGServer.get_device_for_hardware_id(HW)
	if held == null:
		_fail("session 1: no handle for %s" % HW)
		return
	held.capture_finished.connect(_on_capture_finished)
	held.live_changed.connect(_on_live_changed)
	if not await _engage_until_live(held, "session 1"):
		return
	var first_id := await _capture_once(held, "session 1")
	if first_id.is_empty():
		return
	await _settle_frames(10)
	if _finished_ids.count(first_id) != 1:
		_fail("session 1: capture_finished for %s seen %d times" % [first_id, _finished_ids.count(first_id)])
		return
	if held.get_result() == null:
		_fail("session 1: get_result() null after a delivered capture")
		return

	# --- Restart ---------------------------------------------------------------
	CamBANGServer.stop()
	if bool(held.is_live()):
		_fail("held handle still reports live after stop()")
		return
	if not await _start_session("session 2"):
		return

	var again = CamBANGServer.get_device_for_hardware_id(HW)
	if again == null:
		_fail("session 2: no handle for %s" % HW)
		return
	if again != held:
		_fail("get_device_for_hardware_id(%s) returned a different object after restart" % HW)
		return
	if held.get_result() != null:
		_fail("session 2: held handle still returns the previous session's result")
		return

	var live_before := _live_true_count
	if not await _engage_until_live(held, "session 2"):
		return
	if _live_true_count != live_before + 1:
		_fail("session 2: live_changed(true) seen %d times on the held handle, expected 1" % (_live_true_count - live_before))
		return

	var second_id := await _capture_once(held, "session 2")
	if second_id.is_empty():
		return
	await _settle_frames(10)
	if _finished_ids.count(second_id) != 1:
		_fail("session 2: capture_finished for %s seen %d times" % [second_id, _finished_ids.count(second_id)])
		return
	if _finished_ids.size() != 2:
		_fail("held handle saw %d completions over two sessions, expected 2: %s" % [_finished_ids.size(), str(_finished_ids)])
		return
	if held.get_result() == null:
		_fail("session 2: get_result() null after a delivered capture")
		return

	_emit("ok", "same-object-after-restart+signals-carry-over+session-state-reset+single-emission")
