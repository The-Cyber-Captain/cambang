extends Node

## Scene 572: the stream compute-texture surface.
##
## Verifies the ADDITIONAL raw plane surface on CamBANGStreamResult -- the
## stream counterpart of CamBANGCaptureResult.get_compute_texture_plane() --
## and that it coexists with get_display_view() rather than replacing it.
##
## Properties asserted, in the order they matter:
##   1. Capability is honest: UNSUPPORTED without a RenderingDevice, never a
##      silently degraded object, and a plane count that agrees with it.
##   2. Plane count follows the frame's own delivered format -- packed is one
##      plane, NV12/NV21 two, I420/YV12 three -- and every plane materialises.
##   3. Frozen, not aliased. This is the property the live display view cannot
##      offer: a held result keeps its planes while newer frames land, which is
##      what lets a caller pair pixels with that frame's acquisition mark.
##   4. get_display_view() still works alongside it.
##
## Two phases in one run, no command-line knob (the pattern scene 74 uses for
## its two payload kinds): phase A takes whatever format Core selects, phase B
## pins NV12 so the multi-plane path is covered even where Core chose packed.
## Contract 6.3.0 makes format selection Core's job and names verification
## scenes as the place a format may legitimately be pinned.
##
## Provider-agnostic: run it on synthetic or platform-backed. Colour values are
## never asserted -- what a camera sees is not this scene's business.

const SCENE_LABEL := "572_stream_compute_texture_verify"
const MAX_FRAMES := 240
const OBSERVE_FRAMES := 600
const TOTAL_TIMEOUT_MS := 120000
const STREAM_WIDTH := 640
const STREAM_HEIGHT := 480

const CAPABILITY_UNSUPPORTED := 3

const COLORIMETRY_KEYS := ["range", "matrix", "transfer", "primaries", "declared"]

const FOURCC_RGBA := 1094862674
const FOURCC_BGRA := 1095911234
const FOURCC_NV12 := 842094158
const FOURCC_NV21 := 825382478
const FOURCC_I420 := 808596553
const FOURCC_YV12 := 842094169

var _provider_arg := "synthetic"
var _step := 0
var _done := false
var _quit_requested := false
var _terminal_verdict_emitted := false
var _start_ms := 0
var _device = null
var _stream = null
var _phases_verified := 0


func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	_parse_args()
	call_deferred("_run")


func _parse_args() -> void:
	var setting_provider := str(ProjectSettings.get_setting(
		"cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
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
		_error("start(%s) rejected (%d)" % [_provider_arg, start_err], "runtime_start_rejected")
		return
	_step_ok("runtime started (provider=%s)" % _provider_arg)

	for phase in [{"label": "core-selected", "format": 0},
			{"label": "pinned-nv12", "format": FOURCC_NV12}]:
		if _done:
			return
		print("=== phase: %s ===" % str(phase["label"]))
		await _verify_phase(int(phase["format"]), str(phase["label"]))
		if _done:
			return
		_teardown_stream()
		await get_tree().process_frame

	if _phases_verified == 0:
		_expected_unsupported("no phase could be verified", "no_phase_verified")
		return
	print("PASS: stream compute-texture surface verified in %d phase(s)" % _phases_verified)
	_ok()


func _verify_phase(pin_format: int, phase_label: String) -> void:
	if not await _start_stream(pin_format, phase_label):
		return

	var result = await _wait_for_result()
	if _done:
		return
	if result == null:
		if pin_format != 0:
			print("INFO: %s: no result within budget -- phase skipped" % phase_label)
			return
		_expected_unsupported("no stream result within budget", "no_stream_result")
		return

	# --- 1. accessors exist, capability is honest ---------------------------
	for m in ["can_get_compute_texture", "get_compute_texture_plane_count",
			"get_compute_texture_plane", "get_colorimetry"]:
		_require(result.has_method(m), "CamBANGStreamResult must expose %s()" % m)
		if _done:
			return

	var support := int(result.can_get_compute_texture())
	var plane_count := int(result.get_compute_texture_plane_count())
	var fmt := int(result.get_format())
	print("  support=%d plane_count=%d format=%d" % [support, plane_count, fmt])

	if support == CAPABILITY_UNSUPPORTED:
		# Honest unsupported: no RenderingDevice, or a GPU-primary frame with no
		# current CPU sidecar (this surface has no GPU-wrap path -- see
		# stream_compute_texture.h). Either way the count must AGREE rather than
		# promising planes it cannot produce, which is worth asserting even in
		# the phase that then skips.
		_require(plane_count == 0,
			"UNSUPPORTED must report zero planes; got %d" % plane_count)
		if _done:
			return
		# A phase, not the run. Another phase may reach a supported payload, and
		# the run only reports expected_unsupported if none of them did.
		print("INFO: %s: unsupported (support=%d, format=%d) -- phase skipped"
			% [phase_label, support, fmt])
		return
	_step_ok("%s: capability reported support=%d" % [phase_label, support])

	# --- 2. plane count follows the delivered format ------------------------
	var expected := _expected_plane_count(fmt)
	_require(expected > 0, "unrecognised delivered format %d" % fmt)
	if _done:
		return
	_require(plane_count == expected,
		"format %d implies %d plane(s); surface reports %d" % [fmt, expected, plane_count])
	if _done:
		return
	_step_ok("%s: plane count %d matches delivered format %d" % [phase_label, plane_count, fmt])

	# --- 3. every plane materialises, and only the valid indices -------------
	var first_planes: Array = []
	for i in range(plane_count):
		var tex = result.get_compute_texture_plane(i)
		_require(tex != null, "plane %d returned null despite support=%d" % [i, support])
		if _done:
			return
		var size: Vector2i = tex.get_size()
		_require(size.x > 0 and size.y > 0, "plane %d has empty size %s" % [i, str(size)])
		if _done:
			return
		print("  plane %d: %dx%d" % [i, size.x, size.y])
		first_planes.append(tex)

	_require(result.get_compute_texture_plane(plane_count) == null,
		"a plane index past the end must return null, not an object")
	if _done:
		return
	_require(result.get_compute_texture_plane(-1) == null,
		"a negative plane index must return null, not an object")
	if _done:
		return
	_step_ok("%s: all %d plane(s) materialised; out-of-range refused" % [phase_label, plane_count])

	# --- colorimetry travels with the planes --------------------------------
	var colorimetry: Dictionary = result.get_colorimetry()
	for key in COLORIMETRY_KEYS:
		_require(colorimetry.has(key), "colorimetry missing key '%s'" % key)
		if _done:
			return
	_step_ok("%s: colorimetry %s" % [phase_label, str(colorimetry)])

	# --- 4. frozen, not aliased ---------------------------------------------
	# Two assertions, and the SECOND is the one that matters.
	#
	# Holding a result and re-reading its planes proves stability, but a cache
	# keyed on stream_id alone would satisfy that too: nothing would have
	# evicted the entry, so the same object comes back regardless. The test that
	# actually validates frame-keyed identity is that a DIFFERENT frame yields a
	# DIFFERENT texture -- under stream-only keying the newer result would hit
	# the same entry and be served the older frame's pixels.
	var newer = await _find_newer_result(result)
	if _done:
		return
	var advanced := newer != null
	if advanced:
		_require(int(result.get_compute_texture_plane_count()) == plane_count,
			"held result changed its plane count after newer frames landed")
		if _done:
			return
		for i in range(plane_count):
			var again = result.get_compute_texture_plane(i)
			_require(again != null, "plane %d became null after newer frames landed" % i)
			if _done:
				return
			_require(again == first_planes[i],
				"plane %d is not frozen: a held result returned a different texture after newer frames landed" % i)
			if _done:
				return
		_step_ok("%s: held result kept all %d plane(s) across newer frames" % [phase_label, plane_count])

		# The discriminating assertion: a newer frame must not be served the
		# held frame's textures.
		var newer_count := int(newer.get_compute_texture_plane_count())
		_require(newer_count == plane_count,
			"newer frame reports %d plane(s) against the held frame's %d"
				% [newer_count, plane_count])
		if _done:
			return
		for i in range(newer_count):
			var newer_plane = newer.get_compute_texture_plane(i)
			_require(newer_plane != null, "newer frame plane %d returned null" % i)
			if _done:
				return
			_require(newer_plane != first_planes[i],
				("plane %d of a NEWER frame is the same texture object as the held " % i)
				+ "frame's -- the cache is not keyed on frame identity, so this frame "
				+ "is being served the previous frame's pixels")
			if _done:
				return
		_step_ok("%s: newer frame produced distinct plane textures" % phase_label)
	else:
		print("INFO: %s: no newer frame observed; freeze check not exercised" % phase_label)

	# --- display view still works alongside ---------------------------------
	_require(int(result.can_get_display_view()) != CAPABILITY_UNSUPPORTED,
		"adding compute textures must not disturb get_display_view()")
	if _done:
		return
	_step_ok("%s: display view unaffected (can_get_display_view=%d)"
		% [phase_label, int(result.can_get_display_view())])
	_phases_verified += 1


func _expected_plane_count(fourcc: int) -> int:
	match fourcc:
		FOURCC_RGBA, FOURCC_BGRA:
			return 1
		FOURCC_NV12, FOURCC_NV21:
			return 2
		FOURCC_I420, FOURCC_YV12:
			return 3
	return 0


func _teardown_stream() -> void:
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null


func _start_stream(pin_format: int, phase_label: String) -> bool:
	# The device is engaged once and reused; only the stream is rebuilt, because
	# the phases differ solely in requested format.
	if _device == null:
		for _i in range(MAX_FRAMES):
			if _timed_out():
				break
			var snap = CamBANGServer.get_state_snapshot()
			if typeof(snap) == TYPE_DICTIONARY and int(snap.get("version", -1)) >= 0:
				break
			await get_tree().process_frame

		var endpoints = CamBANGServer.enumerate_devices()
		if typeof(endpoints) != TYPE_ARRAY or (endpoints as Array).is_empty():
			_expected_unsupported("no devices enumerated", "no_device:%s" % _provider_arg)
			return false
		var hw := str(((endpoints as Array)[0] as Dictionary).get("hardware_id", ""))
		_device = CamBANGServer.get_device_for_hardware_id(hw)
		if _device == null:
			_fail("no device handle for '%s'" % hw, "device_handle_null")
			return false

		var engage_err := ERR_BUSY
		for _i in range(MAX_FRAMES):
			if _timed_out():
				break
			engage_err = int(_device.engage())
			if engage_err == OK:
				break
			await get_tree().process_frame
		if engage_err != OK:
			_expected_unsupported("engage refused (%d)" % engage_err,
				"engage_refused:%d" % engage_err)
			return false

	var profile := {"width": STREAM_WIDTH, "height": STREAM_HEIGHT}
	if pin_format != 0:
		profile["format_fourcc"] = pin_format
	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": profile,
	})
	if _stream == null:
		# A provider that cannot produce the pinned format is provider truth,
		# not a defect in this surface.
		if pin_format != 0:
			print("INFO: %s: create_stream refused format %d -- phase skipped"
				% [phase_label, pin_format])
			return false
		_fail("create_stream() returned null", "create_stream_null")
		return false

	var start_err := int(_stream.start())
	if start_err != OK:
		if pin_format != 0:
			print("INFO: %s: stream.start() returned %d -- phase skipped"
				% [phase_label, start_err])
			_teardown_stream()
			return false
		_expected_unsupported("stream.start() returned %d" % start_err,
			"stream_start_refused:%d" % start_err)
		return false
	_step_ok("%s: stream started %dx%d" % [phase_label, STREAM_WIDTH, STREAM_HEIGHT])
	return true


func _wait_for_result():
	for _i in range(OBSERVE_FRAMES):
		if _timed_out():
			break
		await get_tree().process_frame
		var res = _stream.get_result()
		if res != null:
			return res
	return null


# Returns a result from a genuinely later frame, or null if none arrived.
func _find_newer_result(held):
	var held_mark := _acquisition_mark(held)
	for _i in range(OBSERVE_FRAMES):
		if _timed_out():
			break
		await get_tree().process_frame
		var res = _stream.get_result()
		if res == null or res == held:
			continue
		if held_mark < 0:
			return res  # no marks available; a distinct result object is enough
		var mark := _acquisition_mark(res)
		if mark >= 0 and mark != held_mark:
			return res
	return null


func _acquisition_mark(result) -> int:
	if result == null:
		return -1
	var facts: Dictionary = result.get_camera_facts()
	var timing: Variant = facts.get("acquisition_timing", null)
	if typeof(timing) != TYPE_DICTIONARY:
		return -1
	return int((timing as Dictionary).get("acquisition_mark", -1))


func _timed_out() -> bool:
	return Time.get_ticks_msec() - _start_ms > TOTAL_TIMEOUT_MS


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
	push_error("ERROR: %s" % message)
	print("ERROR: %s" % message)
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
		SCENE_LABEL, status, exit_code, reason])


func _cleanup_and_quit(code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	set_process(false)
	_teardown_stream()
	_device = null
	CamBANGServer.stop()
	get_tree().quit(code)
