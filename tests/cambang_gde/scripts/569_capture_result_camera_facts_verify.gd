extends Node

## Scene 569: CaptureResult camera facts and admission-context boundary verifier.
## This is a headless-only, bounded harness following Scene 568's verdict and
## cleanup contract. It deliberately exercises public Godot objects only.

const SCENE_LABEL := "capture_result_camera_facts_verify"
const TOTAL_TIMEOUT_MS := 20000
const WAIT_TIMEOUT_FRAMES := 1200
const CAPTURE_TIMEOUT_MS := 5000
const HARDWARE_ID := "synthetic:0"

const LOCATION_A := {
	"latitude_degrees": 51.5,
	"longitude_degrees": -0.12,
	"altitude_meters": 35.0,
}
const LOCATION_B := {
	"latitude_degrees": -33.8688,
	"longitude_degrees": 151.2093,
}

var _step := 0
var _done := false
var _quit_requested := false
var _start_ms := 0


func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	call_deferred("_run")


func _run() -> void:
	print("RUN: %s" % SCENE_LABEL)
	await _run_impl()


func _run_impl() -> void:
	CamBANGServer.stop()
	_require(CamBANGServer.has_method("set_capture_geolocation"), "CamBANGServer.set_capture_geolocation is not registered")
	if _done:
		return
	_require(CamBANGServer.set_capture_geolocation(LOCATION_A) == OK, "stopped-time valid geolocation replacement failed")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": 51.5}) != OK, "missing longitude must be rejected")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": "bad", "longitude_degrees": 0.0}) != OK, "malformed latitude must be rejected")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": 91.0, "longitude_degrees": 0.0}) != OK, "out-of-range latitude must be rejected")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": 0.0, "longitude_degrees": 181.0}) != OK, "out-of-range longitude must be rejected")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": NAN, "longitude_degrees": 0.0}) != OK, "non-finite latitude must be rejected")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": 0.0, "longitude_degrees": 0.0, "altitude_meters": INF}) != OK, "non-finite altitude must be rejected")
	_step_ok("stopped-time geolocation validation and transactional rejection verified")

	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	_require(start_err == OK, "synthetic timeline start failed (%d)" % start_err)
	if _done:
		return
	_require(CamBANGServer.select_builtin_scenario("stream_inspection_live") == OK, "unable to stage stream_inspection_live")
	_require(CamBANGServer.start_scenario() == OK, "unable to start stream_inspection_live")
	_step_ok("synthetic runtime and deterministic scenario started")

	var device = await _wait_for_device()
	var stream_result = await _wait_for_stream_result()
	if _done:
		return
	_require(stream_result.has_method("get_camera_facts"), "CamBANGStreamResult must expose camera facts")
	_require(not stream_result.has_method("get_capture_timestamp"), "CamBANGStreamResult legacy capture timestamp accessor must be absent")
	_assert_stream_camera_facts(stream_result, "initial stream")
	_require(_set_three_member_profile(device) == OK, "three-member still profile request failed")
	await _wait_for_three_member_profile()
	if _done:
		return
	_step_ok("three-member synthetic bracket profile is active")

	var first = await _capture(device, "initial")
	if _done:
		return
	_assert_present_result(first, LOCATION_A, true, "initial")
	if _done:
		return
	var first_timing := _get_default_member_acquisition_timing(first)
	var first_datetime := int(first.get_capture_datetime_unix_nanoseconds())
	_step_ok("stopped-time location reached the completed three-member result")

	_require(CamBANGServer.set_capture_geolocation(LOCATION_B) == OK, "running-time geolocation replacement failed")
	_require(CamBANGServer.set_capture_geolocation({"latitude_degrees": -91.0, "longitude_degrees": 0.0}) != OK, "running invalid replacement must be rejected")
	_assert_present_result(first, LOCATION_A, true, "initial immutable after running replacement")
	_require(_timing_dict_equals(_get_default_member_acquisition_timing(first), first_timing), "existing acquisition timing changed after replacement")
	_require(int(first.get_capture_datetime_unix_nanoseconds()) == first_datetime, "existing admission datetime changed after replacement")
	var second = await _capture(device, "replacement")
	if _done:
		return
	_assert_present_result(second, LOCATION_B, false, "replacement")
	if _done:
		return
	_step_ok("running replacement affects only later admissions and preserves prior result")

	_require(CamBANGServer.set_capture_geolocation({}) == OK, "running geolocation clear failed")
	var third = await _capture(device, "clear")
	if _done:
		return
	_assert_absent_result(third, "clear")
	_assert_present_result(second, LOCATION_B, false, "replacement immutable after clear")
	if _done:
		return
	_step_ok("running clear affects only later admissions")

	_info("PASS: CaptureResult camera facts and capture geolocation boundary verified")
	_emit_harness_verdict("ok", 0, "complete")
	_cleanup_and_quit(0)


func _set_three_member_profile(device) -> int:
	return int(device.set_still_capture_profile({
		"still_image_bundle": {
			"members": [
				{"image_member_index": 0, "role": CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED, "intended_exposure_compensation_milli_ev": 0},
				{"image_member_index": 1, "role": CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET, "intended_exposure_compensation_milli_ev": -1000},
				{"image_member_index": 2, "role": CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET, "intended_exposure_compensation_milli_ev": 1000},
			],
		},
	}))


func _wait_for_device():
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null:
			continue
		for record in snapshot.get("devices", []):
			if typeof(record) != TYPE_DICTIONARY or String(record.get("hardware_id", "")) != HARDWARE_ID:
				continue
			var device = CamBANGServer.get_device(int(record.get("instance_id", 0)))
			if device != null:
				return device
	_fail("timed out waiting for synthetic device")
	return null


func _wait_for_stream_result():
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null:
			continue
		for stream in snapshot.get("streams", []):
			if typeof(stream) != TYPE_DICTIONARY:
				continue
			var result = CamBANGServer.get_stream_result_by_stream_id(int(stream.get("stream_id", 0)))
			if result != null:
				return result
	_fail("timed out waiting for synthetic stream result")
	return null


func _wait_for_three_member_profile() -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null:
			continue
		for record in snapshot.get("devices", []):
			if typeof(record) != TYPE_DICTIONARY or String(record.get("hardware_id", "")) != HARDWARE_ID:
				continue
			var profile: Dictionary = record.get("capture_profile", {})
			var still: Dictionary = profile.get("still", {})
			var bundle: Dictionary = still.get("still_image_bundle", {})
			if bundle.get("members", []).size() == 3:
				return
	_fail("timed out waiting for three-member profile publication")


func _capture(device, label: String):
	var prior_capture_id := int(device.get_result().get_capture_id()) if device.get_result() != null else 0
	var capture_err := int(device.trigger_capture())
	_require(capture_err == OK, "%s: trigger_capture failed (%d)" % [label, capture_err])
	if _done:
		return null
	var started_ms := Time.get_ticks_msec()
	while not _timed_out():
		await get_tree().process_frame
		var result = device.get_result()
		if result != null and int(result.get_capture_id()) > prior_capture_id and int(result.get_image_count()) == 3:
			return result
		if Time.get_ticks_msec() - started_ms >= CAPTURE_TIMEOUT_MS:
			break
	_fail("%s: timed out waiting for completed three-member capture result" % label)
	return null


func _assert_present_result(result, expected: Dictionary, expect_altitude: bool, label: String) -> void:
	_require(result != null, "%s: capture result missing" % label)
	_require(result.has_method("get_capture_datetime_unix_nanoseconds"), "%s: admission datetime accessor missing" % label)
	_require(typeof(result.get_capture_datetime_unix_nanoseconds()) == TYPE_INT and int(result.get_capture_datetime_unix_nanoseconds()) > 0, "%s: admission datetime must be a retained integer" % label)
	_require(result.has_geolocation(), "%s: geolocation should be present" % label)
	var geolocation: Dictionary = result.get_geolocation()
	_require(float(geolocation.get("latitude_degrees", NAN)) == float(expected["latitude_degrees"]), "%s: latitude mismatch" % label)
	_require(float(geolocation.get("longitude_degrees", NAN)) == float(expected["longitude_degrees"]), "%s: longitude mismatch" % label)
	_require(geolocation.has("altitude_meters") == expect_altitude, "%s: altitude presence mismatch" % label)
	if expect_altitude:
		_require(float(geolocation.get("altitude_meters", NAN)) == float(expected["altitude_meters"]), "%s: altitude mismatch" % label)
	_assert_camera_facts(result, label)


func _assert_absent_result(result, label: String) -> void:
	_require(result != null, "%s: capture result missing" % label)
	_require(not result.has_geolocation(), "%s: cleared geolocation must be absent" % label)
	_require(result.get_geolocation().is_empty(), "%s: absent geolocation must return an empty Dictionary" % label)
	_assert_camera_facts(result, label)


func _assert_camera_facts(result, label: String) -> void:
	_require(not result.has_method("get_camera_facts"), "%s: CamBANGCaptureResult must not expose get_camera_facts" % label)
	_require(not result.has_method("get_capture_timestamp"), "%s: legacy capture timestamp accessor must be absent" % label)
	for member_index in range(3):
		var member: Dictionary = result.get_image_member(member_index)
		_require(int(member.get("image_member_index", -1)) == member_index, "%s: member identity mismatch at %d" % [label, member_index])
		_require(member.has("camera_facts"), "%s: camera_facts missing at member %d" % [label, member_index])
		_require(not member.has("capture_timestamp"), "%s: legacy member capture_timestamp must be absent at %d" % [label, member_index])
		var facts: Dictionary = member["camera_facts"]
		_require(not facts.has("sensor_orientation"), "%s: obsolete sensor_orientation key must not be exposed" % label)
		for classification in ["facing", "camera_nature", "sensor_orientation_degrees"]:
			var fact: Dictionary = facts.get(classification, {})
			_require(fact.has("value") and fact.has("origin"), "%s: %s classification shape invalid" % [label, classification])
		for atomic_name in ["intrinsics", "distortion", "pose"]:
			var atomic: Dictionary = facts.get(atomic_name, {})
			_require(not atomic.is_empty() and atomic.has("origin"), "%s: %s atomic record missing origin" % [label, atomic_name])
		_require(String(facts["intrinsics"].get("coordinate_domain", "")) != "", "%s: intrinsics must retain coordinate domain" % label)
		_require(String(facts["distortion"].get("model", "")) == "none", "%s: synthetic distortion must be explicit no-distortion" % label)
		var timing: Dictionary = facts.get("acquisition_timing", {})
		_require(
			timing.get("origin", "") == "virtual_camera_authored"
			and timing.has("acquisition_mark")
			and typeof(timing.get("acquisition_mark", null)) == TYPE_INT
			and int(timing.get("acquisition_mark", -1)) >= 0
			and typeof(timing.get("tick_period_numerator_ns", null)) == TYPE_INT
			and int(timing.get("tick_period_numerator_ns", 0)) > 0
			and int(timing.get("tick_period_numerator_ns", 0)) == 1
			and typeof(timing.get("tick_period_denominator", null)) == TYPE_INT
			and int(timing.get("tick_period_denominator", 0)) > 0
			and int(timing.get("tick_period_denominator", 0)) == 1
			and timing.get("clock_domain", "") == "provider_monotonic"
			and timing.get("reference_event", "") == "provider_observed"
			and timing.get("comparability", "") == "same_provider",
			"%s: member %d acquisition timing must retain its provider-domain mark" % [label, member_index]
		)
		var focus: Dictionary = facts.get("focus_state", {})
		_require(focus.get("origin", "") == "virtual_camera_authored" and focus.get("state", "") == "infinity", "%s: member %d focus state mismatch" % [label, member_index])
		var transform: Dictionary = facts.get("realized_image_transform", {})
		_require(
			transform.get("origin", "") == "virtual_camera_authored"
			and int(transform.get("rotation_degrees", -1)) == 0
			and transform.get("mirrored", true) == false
			and transform.get("pixels_already_transformed", false) == true,
			"%s: member %d realized image transform mismatch" % [label, member_index]
		)


func _assert_stream_camera_facts(stream_result, label: String) -> void:
	var facts: Dictionary = stream_result.get_camera_facts()
	var keys := facts.keys()
	_require(keys.size() == 1 and keys[0] == "acquisition_timing", "%s: stream camera_facts must initially contain acquisition_timing only" % label)
	var timing: Dictionary = facts.get("acquisition_timing", {})
	_require(
		timing.get("origin", "") == "virtual_camera_authored"
		and timing.has("acquisition_mark")
		and typeof(timing.get("acquisition_mark", null)) == TYPE_INT
		and int(timing.get("acquisition_mark", -1)) >= 0
		and typeof(timing.get("tick_period_numerator_ns", null)) == TYPE_INT
		and int(timing.get("tick_period_numerator_ns", 0)) > 0
		and int(timing.get("tick_period_numerator_ns", 0)) == 1
		and typeof(timing.get("tick_period_denominator", null)) == TYPE_INT
		and int(timing.get("tick_period_denominator", 0)) > 0
		and int(timing.get("tick_period_denominator", 0)) == 1
		and timing.get("clock_domain", "") == "provider_monotonic"
		and timing.get("reference_event", "") == "provider_observed"
		and timing.get("comparability", "") == "same_provider",
		"%s: stream acquisition timing must retain canonical semantics" % label
	)


func _get_default_member_acquisition_timing(result) -> Dictionary:
	var member: Dictionary = result.get_image_member(0)
	var facts: Dictionary = member.get("camera_facts", {})
	return facts.get("acquisition_timing", {})


func _timing_dict_equals(left: Dictionary, right: Dictionary) -> bool:
	return JSON.stringify(left) == JSON.stringify(right)


func _timed_out() -> bool:
	if _done:
		return true
	if Time.get_ticks_msec() - _start_ms <= TOTAL_TIMEOUT_MS:
		return false
	_fail("total verification timeout")
	return true


func _require(condition: bool, message: String) -> void:
	if condition or _done:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("fail", 1, "failure")
	push_error("step %d FAIL: %s" % [_step, message])
	printerr("FAIL: %s" % message)
	print("FAIL: %s" % message)
	_cleanup_and_quit(1)


func _step_ok(message: String) -> void:
	_step += 1
	print("step %d OK: %s" % [_step, message])


func _info(message: String) -> void:
	print(message)


func _emit_harness_verdict(status: String, exit_code: int, reason: String) -> void:
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL,
		status,
		exit_code,
		reason,
	])


func _cleanup_and_quit(exit_code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	CamBANGServer.stop()
	get_tree().quit(exit_code)
