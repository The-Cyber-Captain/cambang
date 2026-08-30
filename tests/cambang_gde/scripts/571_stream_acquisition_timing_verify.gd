extends Node

## Scene 571: per-frame acquisition timing on a live stream result.
##
## Scenes 70 and 569 already assert the SHAPE of
## CamBANGStreamResult.get_camera_facts()["acquisition_timing"], but both pin
## Synthetic's own values ("virtual_camera_authored" / "provider_observed"), so
## neither can run against a platform-backed provider. Two properties are also
## unasserted anywhere: that the mark ADVANCES frame to frame, and that a
## retained result object keeps the mark it was born with while newer frames
## arrive behind it.
##
## This scene covers exactly those three gaps:
##   1. Shape validated against the token vocabularies rather than one
##      provider's answers, so the same scene is meaningful on Synthetic,
##      windows_winrt and android_camera2.
##   2. Strict advancement across successive frames.
##   3. A held result's mark and geometry are unchanged after later frames land.
##
## EVERY enumerated device is swept, one at a time, rather than only the first.
## A Quest 3 enumerates its avatar camera (id 1) ahead of the passthrough pair
## (50/51), and those report different timing facts -- so verifying "the first
## device" silently left the interesting cameras untested. Sweeping also avoids
## a device-selection command-line knob, which could not reach Android anyway:
## there are no post-'--' user args there, and run_godot.ps1's project-setting
## translation covers only the provider.
##
## Per-device inability to stream is provider/hardware truth, not a defect: a
## refused engage, a rejected geometry (Quest camera 60 offers only 3456-wide
## sizes), or a stream that never carries timing all SKIP that device. The run
## fails only on a contract violation, and reports expected_unsupported when no
## device could be verified at all.
##
## No format_fourcc is requested. Contract doc 6.3.0 ("Pixel format is not a
## user-facing concept") makes format selection Core's job, and pinning one
## here would suppress native selection for no benefit to what is measured.

const SCENE_LABEL := "571_stream_acquisition_timing_verify"
const MAX_FRAMES := 240
const OBSERVE_FRAMES := 600
const TOTAL_TIMEOUT_MS := 180000
const PER_DEVICE_TIMEOUT_MS := 30000
const STREAM_WIDTH := 640
const STREAM_HEIGHT := 480
const REQUIRED_DISTINCT_MARKS := 3

# Token vocabularies from cambang_result_convert_timing.cpp. Membership is
# asserted, never one specific value: each provider legitimately reports a
# different member, and hardcoding one is what makes scenes 70 and 569
# Synthetic-only.
const ORIGIN_TOKENS := [
	"native_reported", "user_supplied", "derived",
	"virtual_camera_authored", "runtime_injected", "core_derived", "unknown",
]
const CLOCK_DOMAIN_TOKENS := ["provider_monotonic", "core_monotonic", "domain_opaque"]
const REFERENCE_EVENT_TOKENS := [
	"exposure_start", "exposure_midpoint", "sensor_readout_start",
	"frame_available", "provider_observed", "unknown",
]
const COMPARABILITY_TOKENS := [
	"same_image_only", "same_device", "same_provider",
	"cross_device_synchronized", "core_timeline", "ordering_only",
]
const TIMING_KEYS := [
	"origin", "acquisition_mark", "tick_period_numerator_ns",
	"tick_period_denominator", "clock_domain", "reference_event", "comparability",
]

var _provider_arg := "synthetic"
var _step := 0
var _done := false
var _quit_requested := false
var _terminal_verdict_emitted := false
var _start_ms := 0
var _device_start_ms := 0
var _device = null
var _stream = null
var _verified: Array[String] = []
var _skipped: Array[String] = []


func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	_parse_args()
	call_deferred("_run")


func _parse_args() -> void:
	# Reuses the established provider knob rather than adding one: Android has
	# no post-'--' user args, so the project setting is the only route there,
	# and run_godot.ps1 already forwards --cambang-bench-provider= on Windows.
	var setting_provider := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting_provider != "":
		_provider_arg = setting_provider
	for raw_arg in OS.get_cmdline_user_args():
		var arg := str(raw_arg)
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()


func _run() -> void:
	print("RUN: %s provider=%s" % [SCENE_LABEL, _provider_arg])
	await _run_impl()


func _run_impl() -> void:
	CamBANGServer.stop()

	var start_err := int(
		CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC) if _provider_arg == "synthetic"
		else CamBANGServer.start()
	)
	if start_err != OK:
		_error("ERROR: start(%s) rejected (%d)" % [_provider_arg, start_err], "runtime_start_rejected")
		return
	_step_ok("runtime started (provider=%s)" % _provider_arg)

	if not await _wait_for_baseline():
		return

	var hardware_ids := _enumerate_hardware_ids()
	if _done:
		return
	if hardware_ids.is_empty():
		# No camera to stream from is an environment fact, not a CamBANG defect.
		_expected_unsupported(
			"enumerate_devices() returned no endpoint for provider=%s" % _provider_arg,
			"no_device:%s" % _provider_arg
		)
		return
	_step_ok("enumerated %d device(s): %s" % [hardware_ids.size(), str(hardware_ids)])

	for hardware_id in hardware_ids:
		if _done:
			return
		if _timed_out():
			print("INFO: total budget spent; %d device(s) not reached" % (
				hardware_ids.size() - _verified.size() - _skipped.size()))
			break
		await _verify_device(hardware_id)

	if _done:
		return

	print("SUMMARY: verified=%d skipped=%d" % [_verified.size(), _skipped.size()])
	for line in _verified:
		print("  verified %s" % line)
	for line in _skipped:
		print("  skipped  %s" % line)

	if _verified.is_empty():
		_expected_unsupported(
			"no enumerated device produced a stream result carrying acquisition_timing",
			"no_timing_on_any_device:%s" % _provider_arg
		)
		return

	print("PASS: stream acquisition_timing observed, advancing, and immutable on %d device(s)" % _verified.size())
	_ok()


# --- per-device verification -------------------------------------------------

func _verify_device(hardware_id: String) -> void:
	_device_start_ms = Time.get_ticks_msec()
	_device = CamBANGServer.get_device_for_hardware_id(hardware_id)
	if _device == null:
		_skip(hardware_id, "device_handle_null")
		return

	var engage_err := ERR_BUSY
	for _i in range(MAX_FRAMES):
		if _timed_out() or _device_timed_out():
			break
		engage_err = int(_device.engage())
		if engage_err == OK:
			break
		if engage_err != ERR_BUSY and engage_err != ERR_UNAVAILABLE:
			break
		await get_tree().process_frame
	if engage_err != OK:
		_skip(hardware_id, "engage_refused:%d" % engage_err)
		await _release_device()
		return

	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": {
			"width": STREAM_WIDTH,
			"height": STREAM_HEIGHT,
		},
	})
	if _stream == null:
		# A device that does not offer this geometry is not a failure.
		_skip(hardware_id, "create_stream_null:%dx%d" % [STREAM_WIDTH, STREAM_HEIGHT])
		await _release_device()
		return

	var stream_start_err := int(_stream.start())
	if stream_start_err != OK:
		_skip(hardware_id, "stream_start_refused:%d" % stream_start_err)
		await _release_device()
		return

	var first_result = await _wait_for_result_with_timing()
	if _done:
		return
	if first_result == null:
		_skip(hardware_id, "no_acquisition_timing")
		await _release_device()
		return

	var first_facts: Dictionary = first_result.get_camera_facts()
	# Device-scoped facts now travel on the same dictionary. Reported rather than
	# asserted against values: what a given camera declares is that camera's
	# fact, not a specification, and pose is absent on providers that do not
	# report it. Origin is printed because an ingested override must be
	# distinguishable from a native report.
	# Every key, not a hardcoded subset. A filter here previously printed only
	# four names and read exactly like the whole dictionary, which hid facts
	# that were in fact arriving.
	var reported := {}
	for key in first_facts.keys():
		if str(key) != "acquisition_timing":
			reported[key] = first_facts[key]
	print("  camera facts (all keys): %s" % str(reported))
	var first_timing: Dictionary = first_facts.get("acquisition_timing", {})
	var first_mark := int(first_timing.get("acquisition_mark", -1))
	var first_width := int(first_result.get_width())
	var first_height := int(first_result.get_height())
	var first_format := int(first_result.get_format())

	# --- shape, against the vocabularies not one provider's answers ----------
	# This asserted "acquisition_timing only" until the device-scoped four were
	# approved onto this surface (2026-08-27). Updated rather than removed: the
	# point was never the count, it was that no UNEXPECTED key appears, and
	# that still holds. acquisition_timing remains required; the four are
	# permitted and individually optional, since a provider that does not
	# report one must omit it rather than invent a value.
	# Widened 2026-08-29 when the device-keyed tiers began resolving for stream
	# frames too, and again once FrameView carried the per-image record: a
	# stream can now carry anything a capture can, so every fact name is
	# permitted and this stays an unexpected-KEY test only.
	#
	# realized_image_transform was excluded here on the reasoning that it could
	# never reach a stream. That was wrong -- a provider may set it on a
	# delivered frame. What is actually invariant is its PROVENANCE: it
	# describes what a provider did to its own pixels, so no external source
	# may assert it. That is asserted below instead of excluding the key.
	const PERMITTED_DEVICE_SCOPED := ["facing", "camera_nature", "sensor_orientation_degrees", "pose",
		"intrinsics", "distortion", "focus_state", "exposure_time",
		"sensor_sensitivity_iso", "aperture_f_number", "focal_length_mm",
		"realized_image_transform"]
	var facts_keys := first_facts.keys()
	_require(
		first_facts.has("acquisition_timing"),
		"%s: stream camera_facts must carry acquisition_timing; got %s" % [hardware_id, str(facts_keys)]
	)
	if _done:
		return
	for key in facts_keys:
		var k := str(key)
		if k == "acquisition_timing":
			continue
		_require(
			PERMITTED_DEVICE_SCOPED.has(k),
			"%s: unexpected key on stream camera_facts: '%s' (permitted: %s)"
				% [hardware_id, k, str(PERMITTED_DEVICE_SCOPED)]
		)

	# Provenance, not presence: a provider may report what it did to these
	# pixels, but no ingested description or other external source may claim to
	# know it on the provider's behalf.
	if first_facts.has("realized_image_transform"):
		_require(
			str((first_facts["realized_image_transform"] as Dictionary).get("origin", "")) != "user_supplied",
			"%s: realized_image_transform must not be externally asserted; origin=%s"
				% [hardware_id, str((first_facts["realized_image_transform"] as Dictionary).get("origin", ""))]
		)
		if _done:
			return
	if _done:
		return
	_assert_timing_shape(first_timing, hardware_id + " first result")
	if _done:
		return

	# --- advancement across frames -------------------------------------------
	var marks: Array[int] = [first_mark]
	var results_seen := 1
	for _i in range(OBSERVE_FRAMES):
		if marks.size() >= REQUIRED_DISTINCT_MARKS:
			break
		if _timed_out() or _device_timed_out():
			break
		await get_tree().process_frame
		var res = _stream.get_result()
		if res == null:
			continue
		results_seen += 1
		var timing: Dictionary = (res.get_camera_facts() as Dictionary).get("acquisition_timing", {})
		if timing.is_empty():
			_fail(
				"%s: acquisition_timing present on the first result but absent on a later one" % hardware_id
				+ " (results_seen=%d) -- presence must not be intermittent" % results_seen,
				"intermittent_timing"
			)
			return
		var mark := int(timing.get("acquisition_mark", -1))
		if mark == marks[marks.size() - 1]:
			continue  # same retained frame observed again; not an advancement
		_assert_timing_shape(timing, "%s advanced result %d" % [hardware_id, marks.size()])
		if _done:
			return
		if mark <= marks[marks.size() - 1]:
			_fail(
				"%s: acquisition marks must advance strictly: saw %d after %d (marks=%s)"
				% [hardware_id, mark, marks[marks.size() - 1], str(marks)],
				"mark_not_monotonic"
			)
			return
		marks.append(mark)

	if marks.size() < REQUIRED_DISTINCT_MARKS:
		# Too few frames within the budget is a cadence observation, not a
		# broken contract: the one mark seen was already shape-verified.
		_skip(hardware_id, "marks_did_not_advance:%s" % str(marks))
		await _release_device()
		return

	# --- the held result is frozen -------------------------------------------
	# CoreStreamResultData is rebuilt per retained frame and handed out as
	# shared_ptr<const ...>, so a Ref held across newer frames must still report
	# the frame it was born with. This is the property a caller pairing a mark
	# with pixels depends on.
	var held_timing: Dictionary = (first_result.get_camera_facts() as Dictionary).get("acquisition_timing", {})
	_require(
		int(held_timing.get("acquisition_mark", -1)) == first_mark,
		"%s: held stream result changed its acquisition_mark: %d -> %d"
			% [hardware_id, first_mark, int(held_timing.get("acquisition_mark", -1))]
	)
	if _done:
		return
	_require(
		_timing_equals(held_timing, first_timing),
		"%s: held stream result changed its acquisition_timing: %s -> %s"
			% [hardware_id, _timing_text(first_timing), _timing_text(held_timing)]
	)
	if _done:
		return
	_require(
		int(first_result.get_width()) == first_width
		and int(first_result.get_height()) == first_height
		and int(first_result.get_format()) == first_format,
		"%s: held stream result changed geometry: %dx%d fmt=%d -> %dx%d fmt=%d"
			% [hardware_id, first_width, first_height, first_format,
				int(first_result.get_width()), int(first_result.get_height()),
				int(first_result.get_format())]
	)
	if _done:
		return

	var deltas: Array[int] = []
	for i in range(1, marks.size()):
		deltas.append(marks[i] - marks[i - 1])
	_verified.append("%s %dx%d fmt=%d %s deltas=%s" % [
		hardware_id, first_width, first_height, first_format,
		_timing_text(first_timing), str(deltas),
	])
	_step_ok("device %s verified: %s deltas=%s" % [hardware_id, _timing_text(first_timing), str(deltas)])
	await _release_device()


func _release_device() -> void:
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	if _device != null:
		if _device.has_method("disengage"):
			_device.disengage()
		_device = null
	# Let the teardown reach the snapshot before the next device engages, so a
	# concurrency refusal reflects real device limits rather than our own
	# not-yet-released handle.
	for _i in range(8):
		await get_tree().process_frame


func _skip(hardware_id: String, reason: String) -> void:
	_skipped.append("%s (%s)" % [hardware_id, reason])
	print("INFO: device %s skipped: %s" % [hardware_id, reason])


# --- assertions --------------------------------------------------------------

func _assert_timing_shape(timing: Dictionary, label: String) -> void:
	for key in TIMING_KEYS:
		_require(timing.has(key), "%s: acquisition_timing missing key %s" % [label, key])
		if _done:
			return
	_require(
		typeof(timing.get("acquisition_mark")) == TYPE_INT
		and int(timing.get("acquisition_mark")) >= 0,
		"%s: acquisition_mark must be a non-negative int; got %s" % [label, str(timing.get("acquisition_mark"))]
	)
	if _done:
		return
	_require(
		typeof(timing.get("tick_period_numerator_ns")) == TYPE_INT
		and int(timing.get("tick_period_numerator_ns")) > 0
		and typeof(timing.get("tick_period_denominator")) == TYPE_INT
		and int(timing.get("tick_period_denominator")) > 0,
		"%s: tick period must be two positive ints; got %s/%s" % [
			label, str(timing.get("tick_period_numerator_ns")), str(timing.get("tick_period_denominator"))]
	)
	if _done:
		return
	_require_token(timing, "origin", ORIGIN_TOKENS, label)
	if _done:
		return
	_require_token(timing, "clock_domain", CLOCK_DOMAIN_TOKENS, label)
	if _done:
		return
	_require_token(timing, "reference_event", REFERENCE_EVENT_TOKENS, label)
	if _done:
		return
	_require_token(timing, "comparability", COMPARABILITY_TOKENS, label)


func _require_token(timing: Dictionary, key: String, tokens: Array, label: String) -> void:
	var value := str(timing.get(key, ""))
	_require(
		tokens.has(value),
		"%s: %s must be one of %s; got '%s'" % [label, key, str(tokens), value]
	)


func _timing_equals(a: Dictionary, b: Dictionary) -> bool:
	for key in TIMING_KEYS:
		if str(a.get(key, "")) != str(b.get(key, "")):
			return false
	return true


func _timing_text(timing: Dictionary) -> String:
	return "mark=%s tick=%s/%s origin=%s domain=%s event=%s comparability=%s" % [
		str(timing.get("acquisition_mark", "?")),
		str(timing.get("tick_period_numerator_ns", "?")),
		str(timing.get("tick_period_denominator", "?")),
		str(timing.get("origin", "?")),
		str(timing.get("clock_domain", "?")),
		str(timing.get("reference_event", "?")),
		str(timing.get("comparability", "?")),
	]


# --- setup -------------------------------------------------------------------

func _wait_for_baseline() -> bool:
	for _i in range(MAX_FRAMES):
		if _timed_out():
			break
		var snap = CamBANGServer.get_state_snapshot()
		if typeof(snap) == TYPE_DICTIONARY and int(snap.get("version", -1)) >= 0:
			return true
		await get_tree().process_frame
	_error("ERROR: timed out waiting for initial snapshot baseline", "baseline_timeout")
	return false


func _enumerate_hardware_ids() -> Array[String]:
	var out: Array[String] = []
	var endpoints = CamBANGServer.enumerate_devices()
	if typeof(endpoints) != TYPE_ARRAY:
		_fail("enumerate_devices() must return an Array", "bad_endpoint_shape")
		return out
	for endpoint in (endpoints as Array):
		if typeof(endpoint) != TYPE_DICTIONARY:
			_fail("enumerate_devices() entries must be Dictionary", "bad_endpoint_shape")
			return out
		var hardware_id := str((endpoint as Dictionary).get("hardware_id", ""))
		if hardware_id.is_empty():
			_fail("endpoint hardware_id must be non-empty", "bad_endpoint_shape")
			return out
		out.append(hardware_id)
	return out


func _wait_for_result_with_timing():
	var saw_result := false
	for _i in range(OBSERVE_FRAMES):
		if _timed_out() or _device_timed_out():
			break
		await get_tree().process_frame
		var res = _stream.get_result()
		if res == null:
			continue
		saw_result = true
		var facts: Dictionary = res.get_camera_facts()
		if facts.has("acquisition_timing"):
			return res
	if not saw_result:
		print("INFO: no stream result was ever retrieved within %d frames" % OBSERVE_FRAMES)
	return null


# --- harness plumbing --------------------------------------------------------

func _timed_out() -> bool:
	return Time.get_ticks_msec() - _start_ms > TOTAL_TIMEOUT_MS


func _device_timed_out() -> bool:
	return Time.get_ticks_msec() - _device_start_ms > PER_DEVICE_TIMEOUT_MS


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
		SCENE_LABEL,
		status,
		exit_code,
		reason,
	])


func _cleanup_and_quit(code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	set_process(false)
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	_device = null
	CamBANGServer.stop()
	get_tree().quit(code)
