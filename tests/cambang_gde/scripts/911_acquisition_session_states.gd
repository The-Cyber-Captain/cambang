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

# OPERATOR-PACED. Every state waits on the PROCEED button; there is no timed
# mode. An unattended run will sit at the first gate until the harness timeout
# kills it and classifies it an error, which is the honest outcome -- this scene
# reports what a person saw, so with nobody watching it has nothing to report.

# Rows of the running results list kept on screen. Bounded so the list cannot
# grow into the framing above it.
const RESULT_ROWS_SHOWN := 6

const CAPTURE_SETTLE_MS := 8000
const STREAM_LIVE_MS := 5000
const SETTLE_PAUSE_SEC := 1.0
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
# Operator-facing framing. A state is only reviewable if the person watching
# knows, before it runs, what has been set up, what is being asked, and what
# should happen -- otherwise the artifacts are just two pictures.

func _stage(number: int, title: String, set_up: String, asking: String,
		contract: String, watch: String) -> void:
	%StepLabel.text = "STATE %d of 6 -- %s\n  set up   : %s\n  asking   : %s\n  contract : %s\n  watch    : %s" % [
		number, title, set_up, asking, contract, watch,
	]


var _proceed_pressed := false


func _on_proceed_pressed() -> void:
	_proceed_pressed = true


# Nothing happens until the operator says so. There is no timed alternative:
# the artifacts are the evidence, and any hold short enough to keep a run brief
# is too short to judge one in.
func _gate(prompt: String) -> void:
	%ProceedButton.text = prompt
	%ProceedButton.disabled = false
	_proceed_pressed = false
	_log("gate waiting: %s" % prompt)
	while not _proceed_pressed and not _done:
		await get_tree().process_frame
	_log("gate released: %s" % prompt)
	_proceed_pressed = false
	%ProceedButton.disabled = true
	%ProceedButton.text = "working..."


# Opens a state: clears BOTH slots, and shows a BEFORE only where this state has
# one of its OWN.
#
# THE STATES ARE DISCRETE. Several used to open by displaying the previous
# state's capture as their BEFORE, which invented a continuity the scene does not
# have. Each state establishes its own condition and takes its own BEFORE
# immediately before its request -- either a capture at the currently retained
# profile (_take_before) or, in state 3, the live viewfinder that IS the
# condition under test.
#
# Deliberately does NOT gate. Each caller awaits _gate directly afterwards: a
# gate nested one call deep did not suspend its caller, so the first state ran
# without ever waiting for the button while the un-nested gates blocked
# correctly. Keeping every await at the same level makes that impossible.
func _open_state(kind: String, geom: Vector2i, texture: Texture2D) -> void:
	_show("before", "", Vector2i.ZERO, null)
	_show("after", "", Vector2i.ZERO, null)
	if kind != "":
		_show("before", kind, geom, texture)


# Takes THIS state's own BEFORE: a capture at whatever profile is currently
# retained, made immediately before the state issues its request. That is what
# BEFORE means -- the pair on screen brackets the request and nothing else.
#
# Not a recorded step. It is setup for the comparison, not an assertion; if it
# fails to deliver the panel is empty and the log says so.
func _take_before(geom: Vector2i) -> void:
	if int(_device.trigger_capture()) != OK:
		_log("before-capture at %dx%d was not triggered" % [geom.x, geom.y])
		return
	if not await _await_capture(CAPTURE_SETTLE_MS):
		_log("before-capture at %dx%d did not deliver" % [geom.x, geom.y])
		return
	_show("before", "Capture", geom, _capture_artifact())


# ---------------------------------------------------------------------------
# Artifact display, before and after side by side, each labelled with what it is
# and at what size.
#
# THE SIZE SHOWN IS MEASURED FROM THE TEXTURE, never the geometry the step asked
# for. Printing the requested constant made a wrong artifact indistinguishable
# from a right one. A disagreement is shown on the panel and fails the run.
#
# Only CAPTURES are size-asserted. A capture artifact is an ImageTexture built
# from a delivered image, so it is a still record and its label stays true. A
# stream display view is a live texture the stream keeps writing to and resizes
# in place, so any size printed beside it is only true for an instant -- those
# panels are labelled "Stream (live)" and pass Vector2i.ZERO, which suppresses
# the assertion rather than making a claim that cannot hold.

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
	# Only the most recent rows are shown. A Label's minimum height is driven by
	# its content, so an unbounded list grows until it crowds the framing off the
	# screen -- which it did, by the later states. The full sequence is in the
	# log; the panel only needs enough to see where you are.
	var lines := []
	for s in _steps:
		lines.append("%s  %s (%s)" % [s["verdict"], s["name"], s["got"]])
	if lines.size() > RESULT_ROWS_SHOWN:
		var hidden := lines.size() - RESULT_ROWS_SHOWN
		lines = lines.slice(hidden)
		lines.insert(0, "... %d earlier row(s), see the log" % hidden)
	%ResultsLabel.text = "\n".join(lines)


# A capture step is satisfied only when an image ARRIVES AT THE SIZE ASKED FOR.
# The trigger's return code alone was actively misleading -- a WinRT capture
# returned OK, delivered the wrong resolution, and the step passed. Admission
# and delivery are separate facts, so each gets its own row.
func _record_delivery(step: String, trigger_err: int, want: Vector2i) -> void:
	# Vector2i.ZERO means "a capture must arrive, but this scene has not
	# established which geometry is correct" -- state 5's case. Delivery is still
	# required; only the size claim is withheld.
	var asserted := want != Vector2i.ZERO
	var label := "%s: capture delivered %dx%d" % [step, want.x, want.y] if asserted \
		else "%s: capture delivered (size not asserted)" % step
	if trigger_err != OK:
		_record(label, Expect.OK, FAILED, "not triggered")
		_show("after", "Stream (live)", Vector2i.ZERO, _stream_artifact())
		return
	if not await _await_capture(CAPTURE_SETTLE_MS):
		_record(label, Expect.OK, FAILED, "no result arrived")
		_show("after", "Stream (live)", Vector2i.ZERO, _stream_artifact())
		return
	var tex := _capture_artifact()
	if tex == null:
		_record(label, Expect.OK, FAILED, "result carried no image")
		return
	var got := Vector2i(int(tex.get_size().x), int(tex.get_size().y))
	if not asserted:
		_record(label, Expect.OK, OK, "arrived %dx%d" % [got.x, got.y])
	else:
		_record(label, Expect.OK, OK if got == want else FAILED,
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

	%ProceedButton.pressed.connect(_on_proceed_pressed)
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
# THE STATES.
#
# One axis: what holds the acquisition seam when the request arrives. States 1-5
# all issue the SAME request -- set_still_capture_profile to the other geometry,
# then a capture to prove it took effect -- so the only thing varying between
# them is the seam's claim state.
#
# State 6 is on a different axis and is marked as such: the request there is a
# stream asking for a geometry, not a profile being set. It is kept because it
# is the case that prompted the scene.
#
# BEFORE and AFTER bracket THE REQUEST, nothing wider. Each state takes its own
# BEFORE immediately before issuing its request; no artifact is ever carried in
# from the state before it.
#
# Anything done to reach a state is SETUP and is named as such in the framing.
# Setup is not what is under test.

func _run_states() -> void:
	# STATE 1 -- no seam realized. Nothing has claimed one and none exists, so
	# the request must create a seam rather than reconfigure anything.
	_stage(1, "no seam realized",
		"device engaged; nothing retained, no stream, no capture",
		"set_still_capture_profile(1280x720), then capture",
		"the request CREATES a seam; there is nothing to reconfigure",
		"no BEFORE -- with no profile retained there is nothing to capture "
		+ "yet. AFTER = a 1280x720 capture")
	_open_state("", Vector2i.ZERO, null)
	await _gate("PROCEED  -  run this state")
	var err := int(_device.set_still_capture_profile(_still_profile(GEOM_A)))
	if err != OK:
		_record("1 no seam: set profile A", Expect.OK, err, "cannot proceed")
		return
	_record("1 no seam: set profile A", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("1 no seam: trigger capture", Expect.OK, err, "")
	await _record_delivery("1 no seam", err, GEOM_A)
	await _gate("PROCEED  -  next state")

	# STATE 2 -- the capture-parent holds the seam. A profile is retained and
	# nothing else claims it: no stream, no capture in flight.
	_stage(2, "seam held by the capture-parent alone",
		"1280x720 retained from state 1; no stream, no capture in flight",
		"set_still_capture_profile(640x480), then capture",
		"the retained-profile claim must not block its own replacement",
		"BEFORE = a capture at the retained 1280x720, taken now. AFTER = one "
		+ "at 640x480 -- a visibly different shape")
	_open_state("", Vector2i.ZERO, null)
	await _gate("PROCEED  -  run this state")
	await _take_before(GEOM_A)
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_B)))
	_record("2 parent-held: set profile B", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("2 parent-held: trigger capture", Expect.OK, err, "")
	await _record_delivery("2 parent-held", err, GEOM_B)
	await _gate("PROCEED  -  next state")

	# STATE 3 -- a producing stream holds the seam. THE ARBITRATION CASE: the
	# still geometry requested differs from the stream's, so a backend that
	# cannot serve both must yield the stream rather than refuse the capture
	# (arbitration_policy.md 2, 6.2).
	_stage(3, "seam held by a PRODUCING stream",
		"SETUP: a VIEWFINDER stream created and started at 640x480",
		"set_still_capture_profile(1280x720), then capture",
		"capture outranks the viewfinder. The stream yields where the backend "
		+ "cannot serve both; the capture is never refused for its sake",
		"BEFORE = the live 640x480 viewfinder. AFTER = a 1280x720 capture. "
		+ "Whether the stream had to stop differs by backend -- the log says")
	_open_state("", Vector2i.ZERO, null)
	_stream = _device.create_stream(_stream_request(GEOM_B))
	if _stream == null:
		_record("3 stream-held: SETUP create_stream", Expect.OK, FAILED, "returned null")
		return
	err = int(_stream.start())
	_record("3 stream-held: SETUP start stream", Expect.OK, err, "")
	if err == OK and await _await_stream_live(STREAM_LIVE_MS):
		_stream_started = true
		_stream_geom = GEOM_B
	_show("before", "Stream (live)", Vector2i.ZERO, _stream_artifact())
	await _gate("PROCEED  -  run this state")
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_A)))
	_record("3 stream-held: set profile A", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("3 stream-held: trigger capture", Expect.OK, err, "")
	# Read after a publish tick: snapshots are tick-bounded and an immediate
	# read still shows the pre-capture state.
	await get_tree().process_frame
	await get_tree().process_frame
	_log("3 stream state after capture: %s" % _stream_stop_reason())
	await _record_delivery("3 stream-held", err, GEOM_A)
	await _gate("PROCEED  -  next state")

	# STATE 4 -- the stream object still exists but is STOPPED. A stream's claim
	# is taken at start and released at stop, not at destroy, so the seam should
	# behave as though the stream were not there.
	_stage(4, "stream object alive but STOPPED",
		"SETUP: the state-3 stream stopped; the handle still exists",
		"set_still_capture_profile(640x480), then capture",
		"a stopped stream holds no claim, so this reconfigures freely",
		"BEFORE = a capture at the retained 1280x720. AFTER = one at 640x480")
	_open_state("", Vector2i.ZERO, null)
	if _stream != null and _stream_started:
		err = int(_stream.stop())
		_stream_started = false
		_record("4 stopped-stream: SETUP stop()", Expect.OK, err, "")
	await get_tree().create_timer(SETTLE_PAUSE_SEC).timeout
	await _gate("PROCEED  -  run this state")
	await _take_before(GEOM_A)
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_B)))
	_record("4 stopped-stream: set profile B", Expect.OK, err, "")
	err = int(_device.trigger_capture())
	_record("4 stopped-stream: trigger capture", Expect.OK, err, "")
	await _record_delivery("4 stopped-stream", err, GEOM_B)
	await _gate("PROCEED  -  next state")

	# STATE 5 -- a capture is IN FLIGHT and holds the seam. The profile request
	# arrives before that capture has settled.
	#
	# The outcome is OBSERVED, not asserted. set_still_capture_profile is a try
	# and Busy is a legitimate answer while a capture holds the seam; so is
	# accepting the retention and applying it once the capture lands. This scene
	# has not established which is correct, and guessing would manufacture a
	# verdict. For the same reason the AFTER capture is shown without a size
	# assertion: which profile is in force depends on that unsettled answer.
	_stage(5, "seam held by an IN-FLIGHT capture",
		"SETUP: the stream destroyed; a capture triggered and NOT waited for",
		"set_still_capture_profile(1280x720) while that capture is in flight",
		"unestablished -- Busy and deferred-retention are both defensible",
		"BEFORE = the in-flight 640x480 capture once it lands. AFTER = the "
		+ "next capture, at whichever profile actually took effect")
	_open_state("", Vector2i.ZERO, null)
	_drop_stream()
	await get_tree().create_timer(SETTLE_PAUSE_SEC).timeout
	await _gate("PROCEED  -  run this state")
	err = int(_device.trigger_capture())
	_record("5 capture-held: SETUP trigger capture", Expect.OK, err, "")
	# The request goes in WITHOUT awaiting the capture above -- that is the
	# whole state.
	var set_err := int(_device.set_still_capture_profile(_still_profile(GEOM_A)))
	_record("5 capture-held: set profile A mid-flight", Expect.OBSERVE, set_err, "")
	if err == OK and await _await_capture(CAPTURE_SETTLE_MS):
		_show("before", "Capture", GEOM_B, _capture_artifact())
	err = int(_device.trigger_capture())
	_record("5 capture-held: trigger capture", Expect.OK, err, "")
	await _record_delivery("5 capture-held", err, Vector2i.ZERO)
	await _gate("PROCEED  -  next state")

	# STATE 6 -- DIFFERENT AXIS. Here the reconfiguring request is a STREAM
	# asking for a geometry, not a profile being set. This is the case that
	# prompted the scene: a retained profile holds the capture-parent claim, and
	# that claim must not refuse a stream, or a device could never reach any
	# geometry but its retained one.
	_stage(6, "DIFFERENT REQUEST -- a stream asks, not a profile",
		"SETUP: 640x480 retained, holding the capture-parent claim; no stream",
		"create_stream(1280x720) and start it",
		"the capture-parent claim must NOT refuse a stream at another geometry",
		"BEFORE = a capture at the retained 640x480. AFTER = the live 1280x720 "
		+ "viewfinder")
	_open_state("", Vector2i.ZERO, null)
	err = int(_device.set_still_capture_profile(_still_profile(GEOM_B)))
	_record("6 stream-asks: SETUP retain 640x480", Expect.OK, err, "")
	await _gate("PROCEED  -  run this state")
	await _take_before(GEOM_B)
	_stream = _device.create_stream(_stream_request(GEOM_A))
	if _stream == null:
		_record("6 stream-asks: create_stream A", Expect.OK, FAILED, "returned null")
	else:
		err = int(_stream.start())
		_record("6 stream-asks: start stream A", Expect.OK, err, "")
		if err == OK and await _await_stream_live(STREAM_LIVE_MS):
			_stream_started = true
			_stream_geom = GEOM_A
			_show("after", "Stream (live)", Vector2i.ZERO, _stream_artifact())
	await _gate("PROCEED  -  next state")
	_drop_stream()



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
