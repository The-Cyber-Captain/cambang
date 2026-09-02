extends Node

## Verifies the delivered-image calibration on whichever provider is running.
##
## The other fact scenes only list `intrinsics_delivered` and
## `delivered_image_region` as PERMITTED keys, which is a no-unexpected-key
## test: a provider publishing neither fact passes them both. This scene proves
## publication and arithmetic instead.
##
## Rules under test, none of which name a device or a geometry:
##   - the delivered calibration is in the delivered-image domain, and its
##     reference dimensions are the dimensions of the image in hand -- that is
##     what makes it checkable against a picture;
##   - the region carries a coordinate domain, because a rectangle with no
##     frame of reference cannot be used for anything;
##   - where the sensor-domain intrinsics, the region and the delivered
##     calibration are ALL present, the delivered values are what that region
##     and that scale imply. This is the real proof: the one assertion that
##     fails if a provider publishes a plausible-looking calibration it did not
##     actually derive from the crop it reported;
##   - unity: when the region is the whole reference frame and the delivered
##     size equals it, the two calibrations must agree exactly, so a caller
##     moving between the surfaces is never silently rescaled.
##
## Every raw value is printed on its own line, so the arithmetic can be checked
## by hand from a log rather than taken on the scene's word -- and so Android
## logcat truncation cannot hide a fact behind a long composite line.
##
## A camera reporting no calibration at all is expected_unsupported, not a
## failure: most webcams do not, and that is truthful absence.

const SCENE_LABEL := "575_delivered_calibration_verify"
const TOTAL_TIMEOUT_MS := 120000
# Providers derive in double and publish through a float boundary; a part in
# 10^4 is far tighter than any real disagreement and far looser than rounding.
const REL_TOL := 1.0e-4

var _step := 0
var _done := false
var _terminal_verdict_emitted := false
var _quit_requested := false
var _started_ms := 0
var _provider_arg := "synthetic"
var _phase := "start"
var _device = null
var _stream = null
var _frames := 0
var _checked_surfaces := 0
var _surfaces_with_delivered := 0
var _capture_triggered := false
var _capture_id := ""
var _capture_done := false
var _null_result_polls := 0
var _endpoints: Array = []
var _ep_index := 0
var _engage_attempts := 0


func _ready() -> void:
	_started_ms = Time.get_ticks_msec()
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
		_phase_open()
	elif _phase == "stream":
		_phase_stream()
	elif _phase == "capture":
		_phase_capture()


func _phase_open() -> void:
	if _endpoints.is_empty():
		var eps = CamBANGServer.enumerate_devices()
		if typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty():
			return
		_endpoints = eps as Array
		print("FACTS enumerated %d endpoint(s)" % _endpoints.size())
	# Cameras on one device do not all report calibration -- on a headset some
	# endpoints are tracking sensors that report none. Walk them rather than
	# judging the whole provider by whichever happens to be first.
	if _ep_index >= _endpoints.size():
		_finish()
		return
	var hw := str((_endpoints[_ep_index] as Dictionary).get("hardware_id", ""))
	_device = CamBANGServer.get_device_for_hardware_id(hw)
	if _device == null:
		_error("no device for %s" % hw, "device_missing")
		return
	if int(_device.engage()) != OK:
		_engage_attempts += 1
		if _engage_attempts > 300:
			print("NOTE: endpoint %s never engaged; moving on" % hw)
			_advance_endpoint()
		return
	_engage_attempts = 0
	_stream = _device.create_stream({"intent": CamBANGStream.INTENT_PREVIEW})
	if _stream == null:
		print("NOTE: endpoint %s refused a preview stream; moving on" % hw)
		_advance_endpoint()
		return
	if int(_stream.start()) != OK:
		print("NOTE: endpoint %s would not start a stream; moving on" % hw)
		_advance_endpoint()
		return
	_step_ok("engaged %s and started a preview stream" % hw)
	_frames = 0
	_phase = "stream"


## Tears the current endpoint down and moves to the next.
func _advance_endpoint() -> void:
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	if _device != null:
		_device.disengage()
		_device = null
	_ep_index += 1
	_engage_attempts = 0
	_frames = 0
	_phase = "start"


func _phase_stream() -> void:
	var r = _stream.get_result()
	if r == null:
		return
	_frames += 1
	# Let the provider settle: the first frames of a Camera2 session can carry
	# partial metadata while 3A converges.
	if _frames < 10:
		return
	var before := _surfaces_with_delivered
	_inspect("stream[%s]" % str((_endpoints[_ep_index] as Dictionary).get("hardware_id", "")),
		r.get_camera_facts(), int(r.get_width()), int(r.get_height()))
	if _done:
		return
	if _surfaces_with_delivered == before:
		# This camera reports nothing to check. Try the next one rather than
		# concluding the provider publishes nothing.
		_advance_endpoint()
		return
	_phase = "capture"


func _phase_capture() -> void:
	if not _capture_triggered:
		# Take the still on a quiet device. Whether a capture may run beside a
		# same-geometry stream is a live arbitration question on some providers,
		# and it is a separate subject from calibration -- this scene must not
		# silently become a test of it.
		if _stream != null:
			_stream.stop()
			_stream.destroy()
			_stream = null
			_device.capture_finished.connect(_on_capture_finished)
			return
		var trigger: Dictionary = _device.trigger_capture()
		var err := int(trigger.get("error", FAILED))
		if err != OK:
			# A refusal here is not this scene's subject; the stream surface was
			# already proven, so report it rather than fail on it. ERR_BUSY is
			# expected where a provider will not run a capture beside a stream.
			print("NOTE: trigger_capture refused (%d); stream surface only" % err)
			_finish()
			return
		_capture_triggered = true
		_capture_id = str(trigger.get("id", ""))
		_step_ok("capture triggered id=%s" % _capture_id)
		return
	var cap = _device.get_result()
	if cap == null:
		_null_result_polls += 1
		if _null_result_polls == 1 or _null_result_polls == 120:
			print("NOTE: device.get_result() null after %d poll(s); finished=%s delivered_const=%d"
				% [_null_result_polls, _capture_done, int(CamBANGServer.DISPOSITION_DELIVERED)])
		return
	# A capture's facts are per IMAGE MEMBER, not per result: a bracket is
	# several images from one trigger and each was exposed differently, so each
	# carries its own record. Members share the result's geometry.
	var count := int(cap.get_image_count())
	print("FACTS capture members=%d" % count)
	for i in range(count):
		var member: Dictionary = cap.get_image_member(i)
		if not member.has("camera_facts"):
			print("NOTE: capture member %d carries no camera_facts" % i)
			continue
		_inspect("capture[%d]" % i, member["camera_facts"] as Dictionary,
			int(cap.get_width()), int(cap.get_height()))
		if _done:
			return
	_finish()


func _finish() -> void:
	if _surfaces_with_delivered == 0:
		_expected_unsupported(
			"no surface published a delivered-image calibration (%d inspected)" % _checked_surfaces,
			"no_delivered_calibration")
		return
	_ok()


## Prints and checks one surface's facts.
func _inspect(label: String, facts: Dictionary, w: int, h: int) -> void:
	_checked_surfaces += 1
	print("FACTS %s delivered_image=%dx%d keys=%d" % [label, w, h, facts.size()])

	var has_native: bool = facts.has("intrinsics")
	var has_delivered: bool = facts.has("intrinsics_delivered")
	var has_region: bool = facts.has("delivered_image_region")
	print("FACTS %s present intrinsics=%s intrinsics_delivered=%s delivered_image_region=%s"
		% [label, has_native, has_delivered, has_region])

	if has_native:
		var n: Dictionary = facts["intrinsics"]
		print("NATIVE    %s f=(%.6f,%.6f)" % [label,
			float(n.get("focal_length_x_px", 0.0)), float(n.get("focal_length_y_px", 0.0))])
		print("NATIVE    %s c=(%.6f,%.6f)" % [label,
			float(n.get("principal_point_x_px", 0.0)), float(n.get("principal_point_y_px", 0.0))])
		print("NATIVE    %s ref=%dx%d domain=%s origin=%s" % [label,
			int(n.get("reference_width_px", 0)), int(n.get("reference_height_px", 0)),
			str(n.get("coordinate_domain", "?")), str(n.get("origin", "?"))])
	if has_region:
		var g: Dictionary = facts["delivered_image_region"]
		print("REGION    %s rect=(%d,%d,%d,%d)" % [label,
			int(g.get("left", -1)), int(g.get("top", -1)),
			int(g.get("width", -1)), int(g.get("height", -1))])
		print("REGION    %s domain=%s origin=%s" % [label,
			str(g.get("coordinate_domain", "?")), str(g.get("origin", "?"))])
	if has_delivered:
		var d: Dictionary = facts["intrinsics_delivered"]
		print("DELIVERED %s f=(%.6f,%.6f)" % [label,
			float(d.get("focal_length_x_px", 0.0)), float(d.get("focal_length_y_px", 0.0))])
		print("DELIVERED %s c=(%.6f,%.6f)" % [label,
			float(d.get("principal_point_x_px", 0.0)), float(d.get("principal_point_y_px", 0.0))])
		print("DELIVERED %s ref=%dx%d domain=%s origin=%s" % [label,
			int(d.get("reference_width_px", 0)), int(d.get("reference_height_px", 0)),
			str(d.get("coordinate_domain", "?")), str(d.get("origin", "?"))])

	if not has_delivered:
		# Truthful absence. But a region without the calibration it exists to
		# support is a half-published fact, and worth saying so.
		if has_region:
			print("NOTE: %s published a region but no delivered calibration" % label)
		return
	_surfaces_with_delivered += 1

	var d2: Dictionary = facts["intrinsics_delivered"]
	_require(str(d2.get("coordinate_domain", "")) == "delivered_image",
		"%s: intrinsics_delivered domain is %s, not delivered_image"
			% [label, str(d2.get("coordinate_domain", ""))])
	if _done: return
	_require(int(d2.get("reference_width_px", 0)) == w and int(d2.get("reference_height_px", 0)) == h,
		"%s: intrinsics_delivered ref %dx%d does not match the delivered image %dx%d"
			% [label, int(d2.get("reference_width_px", 0)), int(d2.get("reference_height_px", 0)), w, h])
	if _done: return
	_require(float(d2.get("focal_length_x_px", 0.0)) > 0.0 and float(d2.get("focal_length_y_px", 0.0)) > 0.0,
		"%s: intrinsics_delivered has non-positive focal length" % label)
	if _done: return
	_step_ok("%s delivered calibration is in-domain and matches the image size" % label)

	if has_region:
		var g2: Dictionary = facts["delivered_image_region"]
		_require(int(g2.get("width", 0)) > 0 and int(g2.get("height", 0)) > 0,
			"%s: delivered_image_region has non-positive extent" % label)
		if _done: return
		_require(str(g2.get("coordinate_domain", "")) != "",
			"%s: delivered_image_region carries no coordinate domain" % label)
		if _done: return
		_step_ok("%s region is well-formed and framed" % label)

	# The assertion that actually proves derivation rather than decoration.
	if has_native and has_region:
		var n2: Dictionary = facts["intrinsics"]
		var g3: Dictionary = facts["delivered_image_region"]
		var rw := float(g3.get("width", 0))
		var rh := float(g3.get("height", 0))
		if rw <= 0.0 or rh <= 0.0:
			return
		var sx := float(w) / rw
		var sy := float(h) / rh
		var efx := float(n2.get("focal_length_x_px", 0.0)) * sx
		var efy := float(n2.get("focal_length_y_px", 0.0)) * sy
		var ecx := (float(n2.get("principal_point_x_px", 0.0)) - float(g3.get("left", 0))) * sx
		var ecy := (float(n2.get("principal_point_y_px", 0.0)) - float(g3.get("top", 0))) * sy
		print("EXPECT    %s scale=(%.6f,%.6f)" % [label, sx, sy])
		print("EXPECT    %s f=(%.6f,%.6f)" % [label, efx, efy])
		print("EXPECT    %s c=(%.6f,%.6f)" % [label, ecx, ecy])
		_close(label, "focal_length_x_px", float(d2.get("focal_length_x_px", 0.0)), efx)
		if _done: return
		_close(label, "focal_length_y_px", float(d2.get("focal_length_y_px", 0.0)), efy)
		if _done: return
		_close(label, "principal_point_x_px", float(d2.get("principal_point_x_px", 0.0)), ecx)
		if _done: return
		_close(label, "principal_point_y_px", float(d2.get("principal_point_y_px", 0.0)), ecy)
		if _done: return
		_step_ok("%s delivered calibration equals what the region and scale imply" % label)

		# Unity: no crop and no resize must mean no change at all.
		if int(g3.get("left", -1)) == 0 and int(g3.get("top", -1)) == 0 \
			and int(g3.get("width", 0)) == int(n2.get("reference_width_px", -1)) \
			and int(g3.get("height", 0)) == int(n2.get("reference_height_px", -1)) \
			and w == int(n2.get("reference_width_px", -1)) \
			and h == int(n2.get("reference_height_px", -1)):
			_close(label, "unity focal_length_x_px",
				float(d2.get("focal_length_x_px", 0.0)), float(n2.get("focal_length_x_px", 0.0)))
			if _done: return
			_close(label, "unity principal_point_x_px",
				float(d2.get("principal_point_x_px", 0.0)), float(n2.get("principal_point_x_px", 0.0)))
			if _done: return
			_step_ok("%s unity holds: full-frame, unscaled, identical" % label)


## Evidence only: says whether the capture completed at all, and how. Without
## it a stalled capture and a mis-fetched result look identical from the log.
func _on_capture_finished(id: String, disposition: int, error_code: int) -> void:
	print("CAPTURE   finished id=%s disposition=%d error=%d" % [id, disposition, error_code])
	_capture_done = true


func _close(label: String, what: String, got: float, want: float) -> void:
	var scale: float = max(abs(want), 1.0)
	if abs(got - want) / scale <= REL_TOL:
		return
	_fail("%s: %s is %.6f but the region implies %.6f" % [label, what, got, want],
		"derivation_mismatch")


func _require(condition: bool, message: String) -> void:
	if condition or _done:
		return
	_fail(message, "assertion_failed")


func _step_ok(message: String) -> void:
	_step += 1
	print("step %d OK: %s" % [_step, message])


func _ok() -> void:
	if _done: return
	_done = true
	_emit_harness_verdict("ok", 0, "pass")
	_cleanup_and_quit(0)


func _fail(message: String, reason: String) -> void:
	if _done: return
	_done = true
	_emit_harness_verdict("fail", 1, reason)
	push_error("FAIL: %s" % message)
	print("FAIL: %s" % message)
	_cleanup_and_quit(1)


func _error(message: String, reason: String) -> void:
	if _done: return
	_done = true
	_emit_harness_verdict("error", 1, reason)
	push_error(message)
	print(message)
	_cleanup_and_quit(1)


func _expected_unsupported(message: String, reason: String) -> void:
	if _done: return
	_done = true
	print("EXPECTED_UNSUPPORTED: %s" % message)
	_emit_harness_verdict("expected_unsupported", 0, reason)
	_cleanup_and_quit(0)


func _emit_harness_verdict(status: String, exit_code: int, reason: String) -> void:
	if _terminal_verdict_emitted: return
	_terminal_verdict_emitted = true
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, exit_code, reason,
	])


func _cleanup_and_quit(code: int) -> void:
	if _quit_requested: return
	_quit_requested = true
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	CamBANGServer.stop()
	get_tree().quit(code)
