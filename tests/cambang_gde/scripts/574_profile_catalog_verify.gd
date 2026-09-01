extends Node

## Verifies the endpoint profile catalogs a provider advertises.
##
## Asserts the SHAPE and the rules, never specific geometries: what a given
## camera offers is truth for that camera, not a specification, and a scene
## expecting 1280x720 would fail on hardware that is simply different.
##
## The rules under test:
##   - a catalog is readable WITHOUT engaging the device (it describes the
##     endpoint, not a session), which is why it resolves at enumerate time;
##   - "profiles" absent means the provider cannot enumerate, a different
##     answer from an empty list, and an unknown id must produce the former;
##   - entries carry non-zero geometry and a pixel format CamBANG names;
##   - max_fps is optional per entry and, when present, positive;
##   - a catalog entry can be handed straight back to create_stream().
##
## Where a provider cannot enumerate for a real endpoint -- WinRT reports this
## while the camera is closed, by design -- that is expected_unsupported rather
## than failure.

const SCENE_LABEL := "574_profile_catalog_verify"
const TOTAL_TIMEOUT_MS := 90000
const ORIGIN_TOKENS := [
	"native_reported", "user_supplied", "derived",
	"virtual_camera_authored", "runtime_injected", "core_derived", "unknown",
]

var _step := 0
var _done := false
var _terminal_verdict_emitted := false
var _quit_requested := false
var _started_ms := 0
var _provider_arg := "synthetic"
var _phase := "start"
var _endpoints: Array = []
var _chosen_profile: Dictionary = {}
var _chosen_hw := ""
var _device = null
var _stream = null
var _frames := 0
# Read from the server constants rather than restated, so this cannot drift.
var _known_formats: Array = []


func _ready() -> void:
	_started_ms = Time.get_ticks_msec()
	_known_formats = [
		CamBANGServer.PIXEL_FORMAT_RGBA, CamBANGServer.PIXEL_FORMAT_BGRA,
		CamBANGServer.PIXEL_FORMAT_NV12, CamBANGServer.PIXEL_FORMAT_NV21,
		CamBANGServer.PIXEL_FORMAT_I420, CamBANGServer.PIXEL_FORMAT_YV12,
		CamBANGServer.PIXEL_FORMAT_YUY2, CamBANGServer.PIXEL_FORMAT_UYVY,
	]
	var setting_provider := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting_provider != "":
		_provider_arg = setting_provider
	# Both lists: the launcher passes maintainer switches after `--`, which Godot
	# routes to get_cmdline_user_args() and NOT to get_cmdline_args(). Reading
	# only the latter makes the provider switch silently do nothing.
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	for arg in args:
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
	print("RUN: %s provider=%s" % [SCENE_LABEL, _provider_arg])

	var err := 0
	if _provider_arg == "synthetic":
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC))
	else:
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
	if err != OK:
		_error("runtime start failed (%d)" % err, "runtime_start_failed")
		return
	_step_ok("runtime started (provider=%s)" % _provider_arg)


func _process(_delta: float) -> void:
	if _done:
		return
	if Time.get_ticks_msec() - _started_ms > TOTAL_TIMEOUT_MS:
		_error("timed out in phase %s" % _phase, "timeout")
		return
	if _phase == "start":
		_phase_discover()
	elif _phase == "inspect":
		_phase_inspect()
	elif _phase == "use":
		_phase_use()
	elif _phase == "observe":
		_phase_observe()


func _phase_discover() -> void:
	var eps = CamBANGServer.enumerate_devices()
	if typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty():
		return
	_endpoints = eps as Array
	_step_ok("enumerated %d endpoint(s)" % _endpoints.size())
	_phase = "inspect"


func _phase_inspect() -> void:
	# Deliberately BEFORE any engage(): a catalog describes the endpoint.
	var enumerable := 0
	for e in _endpoints:
		var hw := str((e as Dictionary).get("hardware_id", ""))
		for want_capture in [false, true]:
			var caps: Dictionary = {}
			var kind := "stream"
			if want_capture:
				caps = CamBANGServer.get_supported_capture_profiles(hw)
				kind = "capture"
			else:
				caps = CamBANGServer.get_supported_stream_profiles(hw)
			if not caps.has("profiles"):
				print("  %s %s: not enumerable" % [hw, kind])
				continue
			enumerable += 1
			var origin := str(caps.get("origin", ""))
			_require(ORIGIN_TOKENS.has(origin),
				"%s %s: origin %s is not a known token" % [hw, kind, origin])
			if _done:
				return
			var sizes := {}
			for p in (caps["profiles"] as Array):
				var listing := p as Dictionary
				# The profile is nested and carries exactly the keys
				# create_stream() accepts; max_fps sits beside it because a
				# capability is not a request.
				_require(listing.has("profile"),
					"%s %s: entry has no nested profile" % [hw, kind])
				if _done:
					return
				var d := listing["profile"] as Dictionary
				_require(int(d.get("width", 0)) > 0 and int(d.get("height", 0)) > 0,
					"%s %s: entry has non-positive geometry" % [hw, kind])
				if _done:
					return
				_require(_known_formats.has(int(d.get("format_fourcc", 0))),
					"%s %s: entry format_fourcc %d is not a CamBANG pixel format"
						% [hw, kind, int(d.get("format_fourcc", 0))])
				if _done:
					return
				if listing.has("max_fps"):
					_require(float(listing["max_fps"]) > 0.0,
						"%s %s: max_fps present but not positive" % [hw, kind])
					if _done:
						return
				sizes["%dx%d" % [int(d["width"]), int(d["height"])]] = true
				if _chosen_profile.is_empty() and not want_capture:
					_chosen_profile = d
					_chosen_hw = hw
			print("  %s %s: %d entries, %d distinct sizes, origin=%s"
				% [hw, kind, (caps["profiles"] as Array).size(), sizes.keys().size(), origin])
	_step_ok("catalogs inspected without engaging any device")

	var ghost: Dictionary = CamBANGServer.get_supported_stream_profiles("cambang:no-such-endpoint")
	_require(not ghost.has("profiles"),
		"an unknown endpoint reported a catalog: %s" % JSON.stringify(ghost))
	if _done:
		return
	_step_ok("unknown endpoint reports no catalog")

	if enumerable == 0:
		_expected_unsupported("no endpoint could enumerate its profiles",
			"catalog_not_enumerable")
		return
	if _chosen_profile.is_empty():
		_expected_unsupported("no stream profile advertised to exercise",
			"no_stream_profile_advertised")
		return
	_phase = "use"


func _phase_use() -> void:
	# The point of the shape: an entry goes straight back to create_stream().
	if _device == null:
		_device = CamBANGServer.get_device_for_hardware_id(_chosen_hw)
		if _device == null:
			_error("no device for %s" % _chosen_hw, "device_missing")
			return
	if int(_device.engage()) != OK:
		return
	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": _chosen_profile,
	})
	if _stream == null:
		_fail("create_stream refused an advertised profile: %s" % JSON.stringify(_chosen_profile),
			"advertised_profile_refused")
		return
	if int(_stream.start()) != OK:
		_fail("stream.start() failed for an advertised profile",
			"advertised_profile_start_failed")
		return
	_step_ok("advertised profile %dx%d accepted by create_stream()"
		% [int(_chosen_profile["width"]), int(_chosen_profile["height"])])
	_phase = "observe"


func _phase_observe() -> void:
	var result = _stream.get_result()
	if result == null:
		return
	_frames += 1
	if _frames < 3:
		return
	_step_ok("stream produced frames on the advertised profile")
	_ok()


func _require(condition: bool, message: String) -> void:
	if condition or _done:
		return
	_fail(message, "assertion_failed")


func _step_ok(message: String) -> void:
	_step += 1
	print("step %d OK: %s" % [_step, message])


func _ok() -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("ok", 0, "pass")
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
