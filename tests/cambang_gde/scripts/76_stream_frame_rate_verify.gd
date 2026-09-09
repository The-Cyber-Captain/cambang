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
## TWO PHASES, AND THE FIRST IS WHAT MAKES THE SECOND READABLE.
##
##   A. No rate requested. Core selects one from what the provider reports, the
##      same way it selects a pixel format the caller did not name. This must
##      start and deliver on any working device, so a failure here is a real
##      failure and never a capability answer.
##   B. A fixed rate requested. A frame rate is specified configuration, not a
##      preference: a device that does not offer it REFUSES, exactly as it
##      refuses an unobtainable width, and nothing is substituted. So a refusal
##      in phase B is expected_unsupported -- a fact about the camera -- while
##      the same refusal in phase A would be a defect.
##
## Without phase A a refusal is unattributable, because the catalog advertises
## geometry and format but deliberately not selectable rates: max_fps there is a
## capability and explicitly not a request (brief 9A). A caller cannot know in
## advance whether a rate is on offer, which is exactly why omitting the rate is
## the portable choice and asking for one states a requirement.
##
## WHAT IS NOT ASSERTED. A measured cadence. That is hardware behaviour, not
## CamBANG's. Delivery is observed for a duration so the provider logs at least
## two of its 30-frame diagnostics, which is what makes a realized rate
## computable from the log afterwards.
##
## WHERE THE EVIDENCE IS. Provider log lines, which reach logcat on Android and
## stderr on Windows:
##   fpsapply  ... the effective rate was set on the backend
##   fpsreject ... the effective rate is not one this backend offers
## A run showing neither, with a rate requested, is a run in which the rate was
## never asked for -- the regression this scene guards.

const SCENE_LABEL := "76_stream_frame_rate_verify"
const TOTAL_TIMEOUT_MS := 120000
# Both curated handsets advertise 15 as a FIXED range -- S20+ [15-15] among
# [7-24],[24-24],[7-30],[30-30]; Quest 3 [15-15] among [1-15],[1-30],[30-30] --
# so this is reachable on hardware we own rather than a rate we hope exists. The
# WinRT host camera offers no 15 at its advertised geometries, which is the
# expected_unsupported path and worth exercising rather than avoiding.
const WANTED_FPS := 15
# Long enough at any plausible rate for the provider's every-30-frames frame
# diagnostic to appear twice: two marks are the minimum from which an interval,
# and so a realized rate, can be derived.
const OBSERVE_MS := 3000

var _done := false
var _terminal_verdict_emitted := false
var _quit_requested := false
var _started_ms := 0
var _provider_arg := "synthetic"
var _phase := "start"
var _device = null
var _stream = null
var _hardware_id := ""
var _base_profile: Dictionary = {}
var _frames := 0
var _first_result_ms := 0
var _rate_requested := false


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
	print("RUN: %s provider=%s wanted_fps=%d" % [SCENE_LABEL, _provider_arg, WANTED_FPS])

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
	match _phase:
		"start":
			_phase = "discover"
		"discover":
			_phase_discover()
		"engage":
			_phase_engage()
		"open_unrated":
			_phase_open(false)
		"observe_unrated":
			_phase_observe()
		"open_rated":
			_phase_open(true)
		"observe_rated":
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
		_base_profile = ((profiles[0] as Dictionary)["profile"] as Dictionary).duplicate()
		_phase = "open_unrated"
		return
	if err != ERR_BUSY:
		_fail("engage() failed (%d)" % err, "engage_failed")


func _phase_open(with_rate: bool) -> void:
	_rate_requested = with_rate
	var profile: Dictionary = _base_profile.duplicate()
	if with_rate:
		# Whole ints only: the boundary parser requires Variant::INT exactly, and
		# a float rejects the ENTIRE definition, not just this key.
		profile["target_fps"] = int(WANTED_FPS)

	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": profile,
	})
	if _stream == null:
		if with_rate:
			_expected_unsupported("create_stream refused target_fps=%d on this device: %s"
				% [WANTED_FPS, JSON.stringify(profile)], "rate_refused_at_create")
		else:
			_fail("create_stream refused a catalog profile carrying no rate: %s"
				% JSON.stringify(profile), "create_stream_null")
		return

	var err := int(_stream.start())
	if err != OK:
		# Phase A proved this device can start an unrated stream at this geometry,
		# so a refusal HERE is the device declining the rate, not a broken stream.
		if with_rate:
			_stream.destroy()
			_stream = null
			_expected_unsupported("device does not offer %dfps at %dx%d (start rc=%d)"
				% [WANTED_FPS, int(profile.get("width", 0)), int(profile.get("height", 0)), err],
				"rate_not_offered")
		else:
			_fail("stream.start() failed (%d) with no rate requested: %s"
				% [err, JSON.stringify(profile)], "unrated_start_failed")
		return

	print("STEP OK: stream started at %dx%d %s" % [
		int(profile.get("width", 0)), int(profile.get("height", 0)),
		("target_fps=%d" % WANTED_FPS) if with_rate else "no rate requested",
	])
	_frames = 0
	_first_result_ms = 0
	_phase = "observe_rated" if with_rate else "observe_unrated"


func _phase_observe() -> void:
	# Sustained delivery proves the configuration did not break acquisition. How
	# FAST frames arrive is the sensor's business and is not asserted here. A
	# stream that stops delivering mid-window never satisfies this and the scene
	# times out, which is correct.
	var result = _stream.get_result()
	if result == null:
		return
	_frames += 1
	if _first_result_ms == 0:
		_first_result_ms = Time.get_ticks_msec()
		return
	var observed_ms := Time.get_ticks_msec() - _first_result_ms
	if observed_ms < OBSERVE_MS:
		return
	print("STEP OK: delivery sustained %d ms (%d retrievals) %s" % [
		observed_ms, _frames,
		"with a rate-carrying profile" if _rate_requested else "with no rate requested",
	])
	_stream.stop()
	_stream.destroy()
	_stream = null
	if _rate_requested:
		_pass("pass_fps_%d" % WANTED_FPS)
	else:
		_phase = "open_rated"


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
