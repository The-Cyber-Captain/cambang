extends Node

## Asks for a specific frame rate and proves the request reaches the backend.
##
## WHY THIS EXISTS. target_fps / target_fps_min / target_fps_max were parsed at
## the boundary, stored in the stream record, and published in the state
## snapshot -- and never asked of any backend. Camera2 read them only to seed a
## template default; WinRT selected its MediaFrameFormat on geometry alone and
## took the first match. A caller asking for 15fps got whatever the sensor chose
## and was told it had 15. Nothing in the suite noticed, because nothing asked
## for a rate at all.
##
## WHAT THIS ASSERTS HERE. That a rate-carrying definition is accepted, starts,
## and produces frames -- the boundary and lifecycle half. It deliberately does
## NOT assert a measured rate: the realized measurement lives in Core, whose
## diagnostics are not visible on Android, and asserting a sensor's exact
## cadence would be asserting hardware behaviour rather than CamBANG's.
##
## WHERE THE REAL EVIDENCE IS. The provider's own log lines, which do reach
## logcat:
##   fpssel  ... requested=[15-15] outcome=exact chosen=[15-15]   (the decision)
##   fpsapply ... ae_target_fps_range=[15-15] status=set          (the API call)
## outcome names come from imaging/api/frame_rate_selection.h. `exact` is the
## only one that promises a rate; `satisfied` means the backend was handed a
## span it may sit anywhere inside, and `clamped` means the request could not be
## served. A run whose log shows none of these lines is a run in which the rate
## was never asked for -- which is precisely the regression this guards.

const SCENE_LABEL := "76_stream_frame_rate_verify"
const TOTAL_TIMEOUT_MS := 90000
# Chosen because both curated handsets advertise it as a FIXED range -- S20+
# [15-15] among [7-24],[24-24],[7-30],[30-30]; Quest 3 [15-15] among
# [1-15],[1-30],[30-30] -- so `exact` is reachable on hardware we own rather
# than being a rate we hope someone supports.
const WANTED_FPS := 15
# Overridable so the scene can run as an A/B on one device: --cambang-wanted-fps=0
# omits target_fps entirely, which is the pre-change behaviour (no rate asked of
# the backend). Without that control, "no frames arrived" cannot be attributed --
# a silent camera and a rate request that broke acquisition look identical.
var _wanted_fps := WANTED_FPS

var _done := false
var _terminal_verdict_emitted := false
var _quit_requested := false
var _started_ms := 0
var _provider_arg := "synthetic"
var _phase := "start"
var _device = null
var _stream = null
var _hardware_id := ""
var _chosen_profile: Dictionary = {}
var _frames := 0


func _ready() -> void:
	_started_ms = Time.get_ticks_msec()
	var setting_provider := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting_provider != "":
		_provider_arg = setting_provider
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	for arg in args:
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
		if arg.begins_with("--cambang-wanted-fps="):
			_wanted_fps = int(arg.substr("--cambang-wanted-fps=".length()).strip_edges())
	print("RUN: %s provider=%s wanted_fps=%d" % [SCENE_LABEL, _provider_arg, _wanted_fps])

	var err := 0
	if _provider_arg == "synthetic":
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC))
	else:
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
	if err != OK:
		_error("runtime start failed (%d)" % err, "runtime_start_failed")
		return
	print("STEP OK: runtime started (provider=%s)" % _provider_arg)


func _process(_delta: float) -> void:
	if _done:
		return
	if Time.get_ticks_msec() - _started_ms > TOTAL_TIMEOUT_MS:
		_error("timed out in phase %s" % _phase, "timeout")
		return
	if _phase == "start":
		_phase = "discover"
	elif _phase == "discover":
		_phase_discover()
	elif _phase == "engage":
		_phase_engage()
	elif _phase == "create":
		_phase_create()
	elif _phase == "observe":
		_phase_observe()


func _phase_discover() -> void:
	var eps = CamBANGServer.enumerate_devices()
	if typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty():
		return
	_hardware_id = str(((eps as Array)[0] as Dictionary).get("hardware_id", ""))
	if _hardware_id.is_empty():
		_fail("endpoint hardware_id must be non-empty", "no_hardware_id")
		return
	_device = CamBANGServer.get_device_for_hardware_id(_hardware_id)
	if _device == null:
		_fail("get_device_for_hardware_id() returned null", "no_device_handle")
		return
	_phase = "engage"


func _phase_engage() -> void:
	var err := int(_device.engage())
	if err == OK:
		_phase = "create"
		return
	if err != ERR_BUSY:
		_fail("engage() failed (%d)" % err, "engage_failed")


func _phase_create() -> void:
	# Geometry comes from the catalog, never invented: an unadvertised size is
	# refused by the provider and would fail this scene for a reason that has
	# nothing to do with frame rate.
	var caps: Dictionary = CamBANGServer.get_supported_stream_profiles(_hardware_id)
	if not caps.has("profiles"):
		_expected_unsupported("provider cannot enumerate stream profiles for %s" % _hardware_id,
			"catalog_not_enumerable")
		return
	var profiles: Array = caps["profiles"]
	if profiles.is_empty():
		_expected_unsupported("endpoint advertises no stream profiles", "catalog_empty")
		return

	_chosen_profile = ((profiles[0] as Dictionary)["profile"] as Dictionary).duplicate()
	# Whole ints only: the boundary parser requires Variant::INT exactly, and a
	# float here rejects the ENTIRE definition, not just this key.
	if _wanted_fps > 0:
		_chosen_profile["target_fps"] = int(_wanted_fps)

	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": _chosen_profile,
	})
	if _stream == null:
		_fail("create_stream refused a catalog profile carrying target_fps=%d: %s"
			% [_wanted_fps, JSON.stringify(_chosen_profile)], "create_stream_null")
		return
	print("STEP OK: create_stream accepted %s" % JSON.stringify(_chosen_profile))

	var err := int(_stream.start())
	if err != OK:
		_fail("stream.start() failed (%d) for %s" % [err, JSON.stringify(_chosen_profile)],
			"stream_start_failed")
		return
	print("STEP OK: stream started at %dx%d target_fps=%d" % [
		int(_chosen_profile.get("width", 0)), int(_chosen_profile.get("height", 0)), _wanted_fps,
	])
	_phase = "observe"


func _phase_observe() -> void:
	# Frames prove the rate request did not break acquisition. How FAST they
	# arrive is the sensor's business and is not asserted here; see the header.
	var result = _stream.get_result()
	if result == null:
		return
	_frames += 1
	if _frames < 5:
		return
	print("STEP OK: %d results retrieved with a rate-carrying profile" % _frames)
	_pass("pass_fps_%d" % _wanted_fps)


func _pass(reason: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("ok", 0, reason)
	_cleanup_and_quit(0)


func _fail(message: String, reason: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("fail", 1, reason)
	push_error("FAIL: %s" % message)
	print("FAIL: %s" % message)
	_cleanup_and_quit(1)


func _error(message: String, reason: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("error", 1, reason)
	push_error(message)
	print(message)
	_cleanup_and_quit(1)


func _expected_unsupported(message: String, reason: String) -> void:
	if _done:
		return
	_done = true
	print("EXPECTED_UNSUPPORTED: %s" % message)
	_emit_harness_verdict("expected_unsupported", 0, reason)
	_cleanup_and_quit(0)


func _emit_harness_verdict(status: String, exit_code: int, reason: String) -> void:
	if _terminal_verdict_emitted:
		return
	_terminal_verdict_emitted = true
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, exit_code, reason,
	])


func _cleanup_and_quit(code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	CamBANGServer.stop()
	get_tree().quit(code)
