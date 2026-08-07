extends Control
# TRANSIENT DIAGNOSTIC SCENE -- NOT PART OF THE VALIDATION SUITE.
# Disposable, like 999. Numbered 911 for the same reason: no production scene
# number expects to know about it, and it may be deleted whenever the coverage
# it stands in for exists somewhere durable.
#
# Question this scene exists to answer
# ------------------------------------
# What state can a device's AcquisitionSession be in when a valid request
# arrives that needs the session reconfigured, and does the provider answer the
# way the seam-claim policy says it should?
#
# The policy itself (imaging/api/acquisition_seam_claims.h) is pure and is
# already pinned host-native with a mutation proof. What no host verifier can
# reach is whether the REAL providers consult it: Camera2CameraProvider needs an
# Android device, WinrtCameraProvider needs MSVC, WinRT and a camera. That is
# the whole reason this runs on hardware.
#
# The state that prompted it: a retained still profile holds the capture-parent
# claim, and a stream then starts at a different geometry. That was refused on
# Camera2, where every geometry change is a session replacement, and it must not
# be -- the retained-profile claim exists so the profile can shape the seam.
#
# Geometry is asked for two ways only: set_still_capture_profile(G) for the
# capture parent, create_stream(G) + start() for a stream. trigger_capture()
# takes no arguments and uses whatever profile is retained.
#
# Geometry pair
# -------------
# 1280x720 and 640x480, both advertised by S20+ camera 0 (YUV_420_888) and by
# the eMeet C970 (NV12), confirmed by querying each device. Do not change these
# without re-querying.

const SCENE_LABEL := "911_acquisition_session_states"

# Which hardware this run targets. The device ids are per-machine and there is
# no universal default; see the geometry note above before changing anything.
#   "s20plus" -- Camera2, camera 0 (the back-facing one, confirmed by
#                ACAMERA_LENS_FACING; camera 1 is front)
#   "windows" -- WinRT. Ids are machine-specific symbolic links and cannot be
#                written into a constant, so the device is picked by name.
const TARGET := "windows"
const S20PLUS_CAMERA_ID := "0"
const WINDOWS_NAME_HINT := "eMeet"

const GEOM_A := Vector2i(1280, 720)
const GEOM_B := Vector2i(640, 480)

const CAPTURE_SETTLE_MS := 8000
const STREAM_LIVE_MS := 5000
const SETTLE_PAUSE_SEC := 1.0
const HOLD_ARTIFACT_SEC := 1.5
const WAIT_BEFORE_QUIT := 2.0

# Outcome a step is asserted against. OBSERVE records without judging, and is
# used where the Godot-boundary mapping of a provider refusal is not something
# this scene established -- guessing an expectation there would manufacture a
# pass or a failure out of ignorance.
enum Expect { OK, REFUSED, OBSERVE }

var _device: CamBANGDevice = null
var _hardware_id := ""
var _stream: CamBANGStream = null
var _stream_geom := Vector2i.ZERO
# No is_started() on CamBANGStream, and stop() on an already-stopped stream is
# refused with ERR_BAD_STATE, so the scene tracks it rather than double-stopping.
var _stream_started := false
var _last_capture_id := 0
var _steps: Array = []
var _failed := false
var _done := false


func _still_profile(geom: Vector2i) -> Dictionary:
	return {
		"width": geom.x,
		"height": geom.y,
		"still_image_bundle": {
			"members": [
				{
					"image_member_index": 0,
					"role": CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED,
					"intended_exposure_compensation_milli_ev": 0,
				},
			]
		},
	}


func _stream_request(geom: Vector2i) -> Dictionary:
	return {
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": geom.x,
			"height": geom.y,
			"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
			"target_fps_min": 30,
			"target_fps_max": 30,
		},
	}


func _log(msg: String) -> void:
	print("[911] %s" % msg)


# ---------------------------------------------------------------------------
# Artifact display, before and after side by side, each labelled with what it is
# and at what size.
#
# THE SIZE SHOWN IS MEASURED FROM THE TEXTURE, never the geometry the step asked
# for. Printing the requested constant made a wrong artifact indistinguishable
# from a right one. A disagreement is shown on the panel and fails the run.

func _show(slot: String, kind: String, geom: Vector2i, texture: Texture2D) -> void:
	var tex_node: TextureRect = %BeforeTexture if slot == "before" else %AfterTexture
	var label_node: Label = %BeforeLabel if slot == "before" else %AfterLabel
	tex_node.texture = texture
	if texture == null:
		label_node.text = "%s\n(none)" % slot.to_upper()
		return
	var actual := texture.get_size()
	var actual_i := Vector2i(int(actual.x), int(actual.y))
	if geom == Vector2i.ZERO or actual_i == geom:
		label_node.text = "%s\n%s  %dx%d" % [slot.to_upper(), kind, actual_i.x, actual_i.y]
		return
	label_node.text = "%s\n%s  %dx%d  MISMATCH (asked %dx%d)" % [
		slot.to_upper(), kind, actual_i.x, actual_i.y, geom.x, geom.y,
	]
	_failed = true
	_log("MISMATCH %s %s: artifact is %dx%d, step asked for %dx%d" % [
		slot, kind, actual_i.x, actual_i.y, geom.x, geom.y,
	])


func _clear_slots() -> void:
	_show("before", "", Vector2i.ZERO, null)
	_show("after", "", Vector2i.ZERO, null)


func _stream_artifact() -> Texture2D:
	if _stream == null or not _stream.result_live:
		return null
	var sr := _stream.get_result()
	if sr == null:
		return null
	return sr.get_display_view()


func _capture_artifact() -> Texture2D:
	if _device == null:
		return null
	var res: CamBANGCaptureResult = _device.get_result()
	if res == null or not res.can_to_image():
		return null
	return ImageTexture.create_from_image(res.to_image())


# How the published snapshot describes this scene's stream. A step that merely
# returns OK cannot show which mechanism carried it, nor that a preempted stream
# is reported as preempted rather than as a caller stop or a failure.
func _stream_stop_reason() -> String:
	if _stream == null:
		return "no_stream"
	var snap = CamBANGServer.get_state_snapshot()
	if snap == null:
		return "no_snapshot"
	for s in snap.get("streams", []):
		if int(s.get("stream_id", 0)) == _stream.get_stream_id():
			return "%s/%s" % [str(s.get("mode", "?")), str(s.get("stop_reason", "?"))]
	return "not_in_snapshot"


# ---------------------------------------------------------------------------

func _record(name: String, expect: Expect, err: int, note: String) -> void:
	var got := "OK" if err == OK else error_string(err)
	var verdict := "OBSERVED"
	if expect == Expect.OK:
		verdict = "pass" if err == OK else "FAIL"
	elif expect == Expect.REFUSED:
		verdict = "pass" if err != OK else "FAIL"
	if verdict == "FAIL":
		_failed = true
	_steps.append({"name": name, "expect": expect, "got": got, "verdict": verdict})
	_log("%-46s expected=%-8s got=%-22s %s%s" % [
		name,
		["OK", "REFUSED", "OBSERVE"][expect],
		got,
		verdict,
		("  -- " + note) if note != "" else "",
	])
	var lines := []
	for s in _steps:
		lines.append("%s  %s (%s)" % [s["verdict"], s["name"], s["got"]])
	%ResultsLabel.text = "\n".join(lines)


# A capture step is satisfied only when an image ARRIVES AT THE SIZE ASKED FOR.
# The trigger's return code alone was actively misleading -- a WinRT capture
# returned OK, delivered the wrong resolution, and the step passed. Admission
# and delivery are separate facts, so each gets its own row.
func _record_delivery(step: String, trigger_err: int, want: Vector2i) -> void:
	if trigger_err != OK:
		_record("%s: capture delivered %dx%d" % [step, want.x, want.y], Expect.OK,
			FAILED, "not triggered")
		_show("after", "Stream", _stream_geom, _stream_artifact())
		return
	if not await _await_capture(CAPTURE_SETTLE_MS):
		_record("%s: capture delivered %dx%d" % [step, want.x, want.y], Expect.OK,
			FAILED, "no result arrived")
		_show("after", "Stream", _stream_geom, _stream_artifact())
		return
	var tex := _capture_artifact()
	if tex == null:
		_record("%s: capture delivered %dx%d" % [step, want.x, want.y], Expect.OK,
			FAILED, "result carried no image")
		return
	var got := Vector2i(int(tex.get_size().x), int(tex.get_size().y))
	_record("%s: capture delivered %dx%d" % [step, want.x, want.y], Expect.OK,
		OK if got == want else FAILED,
		"" if got == want else "arrived %dx%d" % [got.x, got.y])
	_show("after", "Capture", want, tex)


func _await_capture(timeout_ms: int) -> bool:
	# A capture is finished when a result with a NEW id is retained. Polling the
	# accessor is the only route: CamBANGDevice has no completion signal, which
	# is the gap the capture identity/lifecycle work exists to close.
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		var res: CamBANGCaptureResult = _device.get_result()
		if res != null and int(res.get_capture_id()) != _last_capture_id:
			_last_capture_id = int(res.get_capture_id())
			return true
		await get_tree().process_frame
	return false


func _await_stream_live(timeout_ms: int) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		if _stream != null and _stream.result_live:
			return true
		await get_tree().process_frame
	return false


func _drop_stream() -> void:
	if _stream == null:
		return
	if _stream.is_valid_stream_handle():
		if _stream_started:
			_stream.stop()
		_stream.destroy()
	_stream = null
	_stream_started = false
	_stream_geom = Vector2i.ZERO


# ---------------------------------------------------------------------------

func _ready() -> void:
	var camera_permission := "android.permission.CAMERA"
	if not OS.get_granted_permissions().has(camera_permission):
		OS.request_permissions()
		await get_tree().create_timer(5.0).timeout

	_clear_slots()
	%StepLabel.text = "starting"
	CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED)

	_hardware_id = await _resolve_device_id()
	if _hardware_id == "":
		_finish("no target device")
		return
	_device = CamBANGServer.get_device_for_hardware_id(_hardware_id)
	if _device == null:
		_finish("get_device_for_hardware_id returned null for %s" % _hardware_id)
		return
	_log("target=%s hardware_id=%s  A=%dx%d  B=%dx%d" % [
		TARGET, _hardware_id, GEOM_A.x, GEOM_A.y, GEOM_B.x, GEOM_B.y])

	# Engaged with NO retained profile, so the first step genuinely meets a
	# device with no seam rather than one primed by setup.
	_device.engage()
	await get_tree().create_timer(SETTLE_PAUSE_SEC).timeout

	await _run_states()
	_finish("complete")


func _resolve_device_id() -> String:
	if TARGET == "s20plus":
		return S20PLUS_CAMERA_ID
	var deadline := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline:
		for ep in CamBANGServer.enumerate_devices():
			if typeof(ep) != TYPE_DICTIONARY:
				continue
			var d: Dictionary = ep
			var hw := str(d.get("hardware_id", ""))
			var nm := str(d.get("name", ""))
			if hw != "" and nm.findn(WINDOWS_NAME_HINT) != -1:
				_log("matched %s by name: %s" % [WINDOWS_NAME_HINT, nm])
				return hw
		await get_tree().process_frame
	_log("no endpoint name contained %s" % WINDOWS_NAME_HINT)
	return ""


# ---------------------------------------------------------------------------
# The states. Each one puts the seam into a known condition, issues a request
# that needs it reconfigured, and records what came back.

func _run_states() -> void:
	# STATE 1 -- no seam exists. Engage alone realizes nothing, so the first
	# request must create one rather than reconfigure anything.
	%StepLabel.text = "1: no seam exists -> capture at A"
	_clear_slots()
	var err := int(_device.set_still_capture_profile(_still_profile(GEOM_A)))
	if err != OK:
		_record("1 no seam: set profile A", Expect.OK, err, "cannot proceed")
		return
	err = int(_device.trigger_capture())
	_record("1 no seam: trigger capture A", Expect.OK, err, "")
	await _record_delivery("1 no seam", err, GEOM_A)
	await get_tree().create_timer(HOLD_ARTIFACT_SEC).timeout

	# STATE 2 -- a seam exists at A and nothing holds it: the capture that built
	# it has terminated. This is the state a finished capture leaves behind, and
	# it is why parking a capture in flight is unnecessary -- the session owns
	# the configuration, the capture does not.
	%StepLabel.text = "2: seam at A, unheld -> capture at B"
	_show("before", "Capture", GEOM_A, _capture_artifact())
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_B)))
	_record("2 unheld seam: set profile B", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("2 unheld seam: trigger capture B", Expect.OK, err, "")
	await _record_delivery("2 unheld seam", err, GEOM_B)
	await get_tree().create_timer(HOLD_ARTIFACT_SEC).timeout

	# STATE 3 -- THE ONE THIS SCENE EXISTS FOR. A retained still profile holds
	# the capture-parent claim at B, and a stream now asks for A. On Camera2
	# that is a session replacement; the capture-parent claim must not refuse
	# it, or a device can never reach a geometry other than its retained
	# profile.
	%StepLabel.text = "3: capture-parent holds B -> stream at A"
	_show("before", "Capture", GEOM_B, _capture_artifact())
	_stream = _device.create_stream(_stream_request(GEOM_A))
	if _stream == null:
		_record("3 parent-held: create_stream A", Expect.OK, FAILED, "returned null")
	else:
		err = int(_stream.start())
		_record("3 parent-held: start stream A", Expect.OK, err, "")
		if err == OK and await _await_stream_live(STREAM_LIVE_MS):
			_stream_started = true
			_stream_geom = GEOM_A
			_show("after", "Stream", GEOM_A, _stream_artifact())
	await get_tree().create_timer(HOLD_ARTIFACT_SEC).timeout

	# STATE 4 -- THE ONE THIS SCENE EXISTS FOR. A VIEWFINDER stream is producing
	# at A and a capture wants B. It must SUCCEED: arbitration_policy.md 2 ranks
	# triggered capture above repeating streams, and 6.2 has the viewfinder
	# preempted to STOPPED rather than the capture refused.
	%StepLabel.text = "4: stream producing A -> capture at B (capture should preempt)"
	_show("before", "Stream", GEOM_A, _stream_artifact())
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_B)))
	# A retention, not a capture: a conflict with the live stream is resolved
	# when the capture is triggered, so this must still succeed.
	_record("4 stream live: set profile B", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("4 stream live: trigger capture B", Expect.OK, err, "")
	# Which mechanism carried it, and the two backends differ by design:
	# FLOWING/NONE means both were served (Camera2, having declared the still
	# output at session creation), STOPPED/PREEMPTED means the stream gave way
	# (WinRT, measured -- forced to coexist there, the capture is never
	# delivered). STOPPED/USER or STOPPED/PROVIDER would be a misattributed stop.
	# Read after a publish tick: snapshots are tick-bounded, and an immediate
	# read still shows the pre-capture state.
	await get_tree().process_frame
	await get_tree().process_frame
	_log("4 stream state after capture: %s" % _stream_stop_reason())
	await _record_delivery("4 stream live", err, GEOM_B)
	await get_tree().create_timer(HOLD_ARTIFACT_SEC).timeout

	# STATE 5 -- stream STOPPED but not destroyed, capture parent still holding.
	# A stream's claim is taken at start and released in stop_stream, not in
	# destroy, so stopping alone should be enough while the handle still exists.
	# If the claim really releases only at destroy, this step fails and says so.
	%StepLabel.text = "5: stream stopped (not destroyed) -> capture at B"
	_show("before", "Stream", GEOM_A, _stream_artifact())
	if _stream != null:
		err = int(_stream.stop())
		_stream_started = false
		_record("5 stopped stream: stop()", Expect.OK, err, "")
	await get_tree().create_timer(SETTLE_PAUSE_SEC).timeout
	err = int(_device.trigger_capture())
	_record("5 stopped stream: trigger capture B", Expect.OK, err, "")
	await _record_delivery("5 stopped stream", err, GEOM_B)
	await get_tree().create_timer(HOLD_ARTIFACT_SEC).timeout

	# RECOVERY -- destroy the stream outright and go back the other way, to A.
	# Reconfiguring B -> A as well as A -> B is what shows the seam follows
	# whatever is asked of it, rather than happening to tolerate one direction.
	%StepLabel.text = "6: stream destroyed -> capture back at A"
	_show("before", "Capture", GEOM_B, _capture_artifact())
	_drop_stream()
	await get_tree().create_timer(SETTLE_PAUSE_SEC).timeout
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_A)))
	_record("6 recovery: set profile A", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("6 recovery: trigger capture A", Expect.OK, err, "")
	await _record_delivery("6 recovery", err, GEOM_A)
	await get_tree().create_timer(HOLD_ARTIFACT_SEC).timeout


# ---------------------------------------------------------------------------

func _finish(reason: String) -> void:
	if _done:
		return
	_done = true
	%StepLabel.text = "done: %s" % reason

	var passed := 0
	var failed := 0
	var observed := 0
	for s in _steps:
		match s["verdict"]:
			"pass": passed += 1
			"FAIL": failed += 1
			_: observed += 1
	_log("summary: pass=%d fail=%d observed=%d" % [passed, failed, observed])

	var status := "fail" if (_failed or reason != "complete") else "ok"
	var exit_code := 1 if status == "fail" else 0
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, exit_code, reason if status == "fail" else "complete"])

	_drop_stream()
	if _device != null:
		_device.disengage()
	await get_tree().create_timer(WAIT_BEFORE_QUIT).timeout
	CamBANGServer.stop()
	get_tree().quit(exit_code)


func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if (event as InputEventKey).keycode == KEY_ESCAPE:
			_finish("escape")
