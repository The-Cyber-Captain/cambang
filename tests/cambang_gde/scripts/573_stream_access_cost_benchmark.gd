extends Node

## Scene 573: comparative cost of the three stream result-access routes.
##
## Answers two questions a third-party performance app would ask:
##   1. What does the new compute-texture plane route cost against the existing
##      get_display_view() and to_image() routes, on this device?
##   2. Have the existing routes REGRESSED? Run this scene against an older GDE
##      build and compare its display-view and to_image numbers; the scene works
##      on a build with no compute-texture methods and simply omits that route.
##
## Measurement comes from CamBANG's own cost-evidence machinery
## (get_result_access_timing_evidence()), not from timing the getters here. That
## matters: get_display_view() returns a LIVE wrapper whose real cost is the
## per-tick refresh, so timing the call itself would measure almost nothing and
## flatter that route enormously. The evidence routes record the work actually
## done, which is the only comparison worth making.
##
## fresh_result_* is the number to read. It isolates accesses that produced a
## genuinely new frame from repeat accesses served from a cache, which is what a
## caller pays at its polling rate.

const SCENE_LABEL := "573_stream_access_cost_benchmark"
const MAX_FRAMES := 240
const WARMUP_FRAMES := 120
const DISPLAY_FRAMES := 300
const COMPUTE_FRAMES := 300
const TO_IMAGE_CALLS := 30
const TOTAL_TIMEOUT_MS := 180000
const STREAM_WIDTH := 640
const STREAM_HEIGHT := 480


const ROUTES_OF_INTEREST := [
	"stream_display_view.cpu_live_display_view",
	"stream_display_view.retained_gpu_backing",
	"stream_compute_texture.plane_upload",
	"stream_compute_texture.plane_cached",
	"stream_to_image.cpu_packed",
	"stream_to_image.cpu_planar_convert",
	"stream_to_image.gpu_primary_cpu_sidecar",
	"stream_to_image.gpu_primary_cpu_sidecar_materializer",
	"stream_to_image.gpu_primary_no_cpu_sidecar_materializer",
]

var _provider_arg := "synthetic"
var _done := false
var _quit_requested := false
var _terminal_verdict_emitted := false
var _start_ms := 0
var _device = null
var _stream = null
var _has_compute := false


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
	# Report the ACTIVE configuration, not the project's declared one. A
	# benchmark that misreports the conditions it ran under is worse than none,
	# and --rendering-method overrides the setting without changing it.
	var has_rd := RenderingServer.get_rendering_device() != null
	print("IDENTITY: os=%s model=%s project_renderer=%s rendering_device=%s" % [
		OS.get_name(),
		(OS.get_model_name() if OS.has_method("get_model_name") else "?"),
		str(ProjectSettings.get_setting("rendering/renderer/rendering_method", "?")),
		str(has_rd),
	])
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

	if not await _start_stream():
		return

	var probe = await _wait_for_result()
	if probe == null:
		_expected_unsupported("no stream result within budget", "no_stream_result")
		return

	_has_compute = probe.has_method("get_compute_texture_plane")
	var fmt := int(probe.get_format())
	var plane_count := 0
	if _has_compute:
		plane_count = int(probe.get_compute_texture_plane_count())
	print("BUILD: compute_texture_methods=%s format=%d plane_count=%d" % [
		str(_has_compute), fmt, plane_count])

	# Warm-up: first access on any route pays one-off costs the evidence system
	# records separately as first_success_ns, but the caches and any lazy setup
	# should be past before the measured phases.
	for _i in range(WARMUP_FRAMES):
		if _timed_out():
			break
		await get_tree().process_frame
		var r = _stream.get_result()
		if r != null:
			r.get_display_view()

	# --- route 1: display view, driven per tick -----------------------------
	var display_ticks := 0
	for _i in range(DISPLAY_FRAMES):
		if _timed_out():
			break
		await get_tree().process_frame
		var r = _stream.get_result()
		if r == null:
			continue
		r.get_display_view()
		display_ticks += 1
	print("DROVE: display_view over %d ticks" % display_ticks)

	# --- route 2: compute-texture planes, driven per tick -------------------
	var compute_ticks := 0
	if _has_compute and plane_count > 0:
		for _i in range(COMPUTE_FRAMES):
			if _timed_out():
				break
			await get_tree().process_frame
			var r = _stream.get_result()
			if r == null:
				continue
			for p in range(int(r.get_compute_texture_plane_count())):
				r.get_compute_texture_plane(p)
			compute_ticks += 1
		print("DROVE: compute_texture planes over %d ticks" % compute_ticks)
	else:
		print("DROVE: compute_texture skipped (methods_present=%s plane_count=%d)"
			% [str(_has_compute), plane_count])

	# --- route 3: to_image, fewer calls because it is the expensive one -----
	var to_image_calls := 0
	for _i in range(TO_IMAGE_CALLS):
		if _timed_out():
			break
		await get_tree().process_frame
		var r = _stream.get_result()
		if r == null:
			continue
		if int(r.can_to_image()) == CamBANGStreamResult.CAPABILITY_UNSUPPORTED:
			continue
		var img = r.to_image()
		if img != null:
			to_image_calls += 1
	print("DROVE: to_image over %d calls" % to_image_calls)

	_report()
	_ok()


func _report() -> void:
	var evidence: Dictionary = CamBANGServer.get_result_access_timing_evidence()
	print("")
	print("=== COST EVIDENCE (means in microseconds, per genuinely-new frame) ===")
	print("%-52s %8s %8s %12s %12s" % ["route", "calls", "fresh", "mean_fresh_us", "max_us"])
	for route in ROUTES_OF_INTEREST:
		if not evidence.has(route):
			continue
		var e: Dictionary = evidence[route]
		var fresh := int(e.get("fresh_result_successes", 0))
		var fresh_total := int(e.get("fresh_result_total_ns", 0))
		var mean_us := (float(fresh_total) / float(fresh) / 1000.0) if fresh > 0 else 0.0
		print("%-52s %8d %8d %12.1f %12.1f" % [
			route,
			int(e.get("calls", 0)),
			fresh,
			mean_us,
			float(int(e.get("max_ns", 0))) / 1000.0,
		])
		print("%-52s   last=%dx%d bytes=%d" % [
			"", int(e.get("last_width", 0)), int(e.get("last_height", 0)),
			int(e.get("last_bytes", 0))])

	if evidence.has("stream_compute_textures"):
		print("stream compute-texture cache: %s" % str(evidence["stream_compute_textures"]))
	print("=== END COST EVIDENCE ===")
	print("")


func _start_stream() -> bool:
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

	# No format pin: measure what a real caller gets, which is whatever Core
	# selects for this device.
	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": {"width": STREAM_WIDTH, "height": STREAM_HEIGHT},
	})
	if _stream == null:
		_fail("create_stream() returned null", "create_stream_null")
		return false
	var start_err := int(_stream.start())
	if start_err != OK:
		_expected_unsupported("stream.start() returned %d" % start_err,
			"stream_start_refused:%d" % start_err)
		return false
	print("STREAM: %dx%d on %s" % [STREAM_WIDTH, STREAM_HEIGHT, hw])
	return true


func _wait_for_result():
	for _i in range(MAX_FRAMES * 2):
		if _timed_out():
			break
		await get_tree().process_frame
		var res = _stream.get_result()
		if res != null:
			return res
	return null


func _timed_out() -> bool:
	return Time.get_ticks_msec() - _start_ms > TOTAL_TIMEOUT_MS


func _ok() -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("ok", 0, "measured")
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
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	_device = null
	CamBANGServer.stop()
	get_tree().quit(code)
