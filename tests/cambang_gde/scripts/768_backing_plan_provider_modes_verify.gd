extends Node

## Scene 768: backing-plan evaluation across provider modes
##
## Purpose:
## - prove backing-plan evaluation reaches a decision, and is cleared by stop(),
##   under BOTH the synthetic provider and a platform-backed provider
## - give the platform-backed providers (windows_winrt, android_camera2) their
##   first machine-checkable coverage: until this scene, every platform-backed
##   capture path was verified by eye or by reading provider logs
##
## Relationship to 568:
## - 568 is the synthetic-only backing-plan verifier and stays authoritative for
##   parent scoping, clocked edges, restart evidence detail, and the full
##   multi-candidate decision breadth (synthetic advertises GPU backing, so its
##   evaluation has several candidate postures to work through)
## - 768 is wider (it runs against real platform providers) but narrower in
##   candidate breadth by necessity: the platform providers advertise CPU-backed
##   only today, so their backing-plan evaluation is SINGLE-CANDIDATE. 768
##   asserts stream and capture each reach a decision, and that stop() clears the
##   evidence, across whichever providers this build and machine can run. When a
##   platform provider gains a GPU-fill payload path it becomes multi-candidate,
##   at which point 768 grows to match (see the capture-decision TODO below).
##
## Topology is built through the public GDScript API in BOTH modes -- the same
## enumerate -> engage -> create_stream -> start path scene 66 already exercises
## for synthetic -- differing only in which provider start() selects. A synthetic
## scenario was considered for the synthetic pass, but a scenario is a
## virtual-time timeline replay that must be advanced by explicit
## advance_timeline() choreography (the ADVANCE_*_NS sequence in 568); a
## free-running API topology needs none of that, keeps the two modes symmetric,
## and touches nothing 568 relies on. The scenario's only real virtue here --
## binding topology by endpoint_index rather than device identity -- is matched
## by taking the first enumerated endpoint in each mode.
##
## Mode selection:
## - default "auto": run every mode this build and machine support. Synthetic
##   always runs; platform-backed runs when the build compiled a platform
##   provider and at least one endpoint enumerates.
## - override with --cambang-scene768-provider-mode=synthetic|platform_backed|auto
## - a mode that cannot run is reported and skipped, not failed: absent hardware
##   is not a defect. Only "no mode could run at all" is expected_unsupported.
##
## Scope guardrail:
## - windowless, strictly PASS/FAIL through the harness verdict line
## - no absolute or device-specific expectations: every assertion is about the
##   contract (a decision is reached, evidence clears on stop), never about a
##   particular exposure, geometry, or camera

const SCENE_LABEL := "backing_plan_provider_modes_verify"
const PROVIDER_MODE_CMDLINE_PREFIX := "--cambang-scene768-provider-mode="

const TOTAL_TIMEOUT_MS := 90000
const WAIT_TIMEOUT_FRAMES := 1800
const PLATFORM_STREAM_WIDTH := 640
const PLATFORM_STREAM_HEIGHT := 360

var _step := 0
var _done := false
var _quit_requested := false
var _start_ms := 0
var _baseline_gens: Array[int] = []
var _display_refs: Array = []
var _modes_run: Array[String] = []
var _modes_skipped: Array[String] = []


func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)
	call_deferred("_run")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if version == 0 and topology_version == 0 and not _baseline_gens.has(gen):
		_baseline_gens.append(gen)


func _run() -> void:
	print("RUN: %s" % SCENE_LABEL)
	var requested_mode := _requested_provider_mode()
	_info("requested provider mode: %s" % requested_mode)

	var run_synthetic := requested_mode == "auto" or requested_mode == "synthetic"
	var run_platform := requested_mode == "auto" or requested_mode == "platform_backed"

	if run_synthetic:
		if _synthetic_supported():
			await _run_synthetic_pass()
			if _done:
				return
			_modes_run.append("synthetic")
		else:
			_modes_skipped.append("synthetic:not_supported_in_build")
			_info("SKIP synthetic: provider support reports synthetic unavailable in this build")

	if run_platform:
		var skip_reason := _platform_backed_skip_reason()
		if skip_reason == "":
			await _run_platform_backed_pass()
			if _done:
				return
			_modes_run.append("platform_backed")
		else:
			_modes_skipped.append("platform_backed:%s" % skip_reason)
			_info("SKIP platform_backed: %s" % skip_reason)

	if _modes_run.is_empty():
		# No mode could run. That is an environment statement, not a failure:
		# a synthetic-only build with no hardware is a legitimate configuration.
		_emit_harness_verdict("expected_unsupported", 0, "no_provider_mode_available")
		_cleanup_and_quit(0)
		return

	_step_ok("backing-plan decision and reset verified for: %s" % ", ".join(_modes_run))
	if not _modes_skipped.is_empty():
		_info("modes skipped: %s" % ", ".join(_modes_skipped))
	_info("PASS: backing-plan evaluation verified across %d provider mode(s)" % _modes_run.size())
	_emit_harness_verdict("ok", 0, "modes=%s" % "+".join(_modes_run))
	_cleanup_and_quit(0)


# ---------------------------------------------------------------------------
# Provider passes: identical API-driven topology, only start() differs
# ---------------------------------------------------------------------------

func _run_synthetic_pass() -> void:
	await _run_api_topology_pass("synthetic", true)


func _run_platform_backed_pass() -> void:
	await _run_api_topology_pass("platform_backed", false)


func _run_api_topology_pass(label: String, is_synthetic: bool) -> void:
	_display_refs.clear()
	CamBANGServer.stop()

	# The only difference between the modes: synthetic selects its provider
	# explicitly (free-running, no scenario), platform-backed takes the default.
	var start_err := (
		CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC) if is_synthetic
		else CamBANGServer.start()
	)
	_require(start_err == OK, "%s: start() rejected (%d)" % [label, start_err])
	if _done:
		return
	_step_ok("%s runtime started" % label)

	var gen := await _wait_for_new_baseline(-1, label)
	if _done:
		return

	var endpoints: Array = CamBANGServer.enumerate_devices()
	_require(endpoints.size() > 0, "%s: no endpoints enumerated after start" % label)
	if _done:
		return
	var endpoint: Dictionary = endpoints[0]
	var hardware_id := str(endpoint.get("hardware_id", ""))
	var display_name := str(endpoint.get("name", ""))
	_require(hardware_id != "", "%s: first endpoint has empty hardware_id" % label)
	if _done:
		return
	_info("%s selected endpoint: hardware_id=%s name=%s (gen=%d)" % [label, hardware_id, display_name, gen])

	var device = CamBANGServer.get_device_for_hardware_id(hardware_id)
	_require(device != null, "%s: get_device_for_hardware_id() returned null for %s" % [label, hardware_id])
	if _done:
		return
	var engage_err := int(device.engage())
	_require(engage_err == OK, "%s: device.engage() failed (%d)" % [label, engage_err])
	if _done:
		return
	var device_instance_id := int(device.get_instance_id())
	_require(device_instance_id != 0, "%s: engaged device reported instance_id 0" % label)
	if _done:
		return
	_step_ok("%s device engaged (instance_id=%d)" % [label, device_instance_id])

	# A provider-neutral baseline geometry. Any provider (synthetic or platform)
	# that cannot serve it fails start() deterministically, which is itself part
	# of the contract being checked; it is not a device-specific choice.
	var stream = device.create_stream({
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": PLATFORM_STREAM_WIDTH,
			"height": PLATFORM_STREAM_HEIGHT,
			"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
		},
	})
	_require(stream != null, "%s: create_stream() returned null" % label)
	if _done:
		return
	var stream_id := int(stream.get_stream_id())
	_require(stream_id != 0, "%s: created stream reported stream_id 0" % label)
	if _done:
		return

	var stream_start_err := int(stream.start())
	_require(stream_start_err == OK, "%s: stream.start() failed (%d)" % [label, stream_start_err])
	if _done:
		return
	_step_ok("%s stream started (stream_id=%d)" % [label, stream_id])

	await _wait_for_live_stream_result(stream_id, label)
	if _done:
		return

	await _verify_stream_backing_plan_decides(stream_id, label)
	if _done:
		return

	# Release any display-view reference the stream access probe took before
	# tearing the stream down. Holding a live GPU display view across stop()/
	# destroy() is what raises the DisplayLifetime borrow-count warning (a
	# deferred-release path handles it, but a verification scene should not
	# provoke it): drop the reference first so teardown is clean.
	_display_refs.clear()

	# Stop and destroy the stream BEFORE capturing. On backends that couple
	# still and stream geometry (Camera2 rebuilds one capture session for both,
	# so a still whose geometry differs from a live stream is refused), a
	# standalone capture avoids the coupling -- and it exercises the
	# capture-without-a-stream path, which is the harder one to get right.
	var stop_err := int(stream.stop())
	_require(stop_err == OK, "%s: stream.stop() failed (%d)" % [label, stop_err])
	if _done:
		return
	var destroy_err := int(stream.destroy())
	_require(destroy_err == OK, "%s: stream.destroy() failed (%d)" % [label, destroy_err])
	if _done:
		return
	_step_ok("%s stream stopped and destroyed" % label)

	await _verify_capture_produces_result_and_decides(device_instance_id, label)
	if _done:
		return

	await _verify_reset_clears_reports(label)


# ---------------------------------------------------------------------------
# Shared contract assertions -- identical for every provider mode
# ---------------------------------------------------------------------------

func _verify_stream_backing_plan_decides(stream_id: int, label: String) -> void:
	var stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
	if stream_result != null:
		# Access-only probe. Evaluation is meant to arm from live applied
		# posture without waiting for public demand, so this seeds the same
		# evidence a real consumer would without asserting a demand contract.
		stream_result.can_to_image()
		var display_view = stream_result.get_display_view()
		if display_view is Texture2D:
			_display_refs.append(display_view)

	var report := await _wait_for_report_decided("stream", stream_id, label)
	if _done:
		return
	_step_ok("%s stream backing-plan decided (posture=%s)" % [
		label, _plan_posture(report, "requested")
	])


func _verify_capture_produces_result_and_decides(device_instance_id: int, label: String) -> void:
	## Two things, both carrying platform-provider signal:
	##   1. the capture produces an accessible result with its members -- the
	##      silent-failure guard (trigger returns OK, then nothing) real platform
	##      bugs have hit;
	##   2. the capture backing-plan evaluation reaches a DECISION.
	##
	## Reaching a decision needs the evaluator driven, not merely observed: even
	## after a candidate's evidence completes it latches the decision on the next
	## probe cycle, not passively (this is 568's completion behaviour). So each
	## iteration triggers a capture and accesses its member -- seeding evidence --
	## then checks whether the report has decided. Camera2 and WinRT advertise
	## CPU-backed only, so their capture evaluation is SINGLE-CANDIDATE and latches
	## in very few cycles; this loop is the single-candidate case of 568's
	## multi-probe completion, not the full per-candidate breadth.
	##
	## TODO(tranche: platform GPU backing): when a platform provider gains a
	## GPU-fill payload path (AHardwareBuffer on Camera2, IDirect3DSurface on
	## WinRT) it will advertise {cpu,gpu,sidecar} and become MULTI-candidate --
	## at which point this needs 568's per-candidate probe breadth, and the
	## concurrent capture+stream parent-scoping case (deliberately not exercised
	## here) becomes a real stress test rather than the trivial single-posture
	## agreement it is today. Both are their own future work, not this tranche.
	const MAX_CAPTURE_DECISION_CYCLES := 4
	var device = CamBANGServer.get_device(device_instance_id)
	_require(device != null, "%s: get_device(%d) returned null" % [label, device_instance_id])
	if _done:
		return

	var member_count := 0
	var report := {}
	for cycle in range(MAX_CAPTURE_DECISION_CYCLES):
		# Capture readiness is asynchronous after engage: a platform provider must
		# realize its still-capture pipeline (Camera2 rejects a capture as
		# unavailable until it has). Synthetic is ready at once, so this is a no-op
		# there. Gate on the advertised still-capture profile rather than racing.
		await _wait_for_capture_ready(device, label)
		if _done:
			return

		var capture_err := int(device.trigger_capture())
		_require(capture_err == OK, "%s: device.trigger_capture() rejected (%d)" % [label, capture_err])
		if _done:
			return

		var capture_result = await _wait_for_capture_result(device_instance_id, label)
		if _done:
			return

		# Access seeds the materialization evidence the evaluator measures.
		member_count = int(capture_result.get_image_count())
		_require(member_count >= 1, "%s: capture result reported %d image members" % [label, member_count])
		if _done:
			return
		capture_result.can_to_image()
		var member0_capability := int(capture_result.can_to_image_member(0))
		_require(
			member0_capability != int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED),
			"%s: capture_result.can_to_image_member(0) returned unsupported" % label
		)
		if _done:
			return
		var member0_image: Variant = capture_result.to_image_member(0)
		_require(member0_image != null, "%s: capture_result.to_image_member(0) returned null" % label)
		if _done:
			return

		report = await _wait_for_report_present_or_decided("capture", device_instance_id, label)
		if _done:
			return
		if _report_has_decision(report):
			_step_ok("%s capture produced accessible result and evaluation decided in %d cycle(s) (members=%d posture=%s)" % [
				label, cycle + 1, member_count, _plan_posture(report, "requested")
			])
			return

	_fail("%s: capture backing-plan did not decide within %d cycle(s); last report=%s" % [
		label, MAX_CAPTURE_DECISION_CYCLES, JSON.stringify(report)
	])


func _verify_reset_clears_reports(label: String) -> void:
	CamBANGServer.stop()
	_display_refs.clear()
	await get_tree().process_frame
	if _done:
		return
	var reports := _get_backing_plan_evaluation_reports()
	_require(
		reports.is_empty(),
		"%s: %d backing-plan evaluation report(s) survived stop()" % [label, reports.size()]
	)
	if _done:
		return
	_require(
		CamBANGServer.get_state_snapshot() == null,
		"%s: snapshot must be NIL after stop()" % label
	)
	if _done:
		return
	_step_ok("%s stop() cleared backing-plan evaluation evidence" % label)


# ---------------------------------------------------------------------------
# Mode availability
# ---------------------------------------------------------------------------

func _requested_provider_mode() -> String:
	for arg in OS.get_cmdline_args():
		var text := str(arg)
		if text.begins_with(PROVIDER_MODE_CMDLINE_PREFIX):
			var value := text.substr(PROVIDER_MODE_CMDLINE_PREFIX.length()).strip_edges()
			if value == "synthetic" or value == "platform_backed" or value == "auto":
				return value
			_info("ignoring unrecognised provider mode '%s'; using auto" % value)
			return "auto"
	return "auto"


func _synthetic_supported() -> bool:
	var support = CamBANGServer.get_provider_support()
	if typeof(support) != TYPE_DICTIONARY:
		return false
	return bool((support as Dictionary).get("synthetic", false))


func _platform_backed_skip_reason() -> String:
	## Returns "" when platform-backed can be exercised, otherwise a short
	## stable reason. Absent hardware and synthetic-only builds are both
	## legitimate, so neither is a failure.
	var support = CamBANGServer.get_provider_support()
	if typeof(support) != TYPE_DICTIONARY:
		return "provider_support_unavailable"
	if not bool((support as Dictionary).get("platform_backed", false)):
		return "not_compiled_in_build"

	CamBANGServer.stop()
	var start_err := CamBANGServer.start()
	if start_err != OK:
		# Most commonly a permission or hardware-access refusal. Reported
		# rather than failed: the provider is behaving correctly by refusing.
		CamBANGServer.stop()
		return "start_rejected_err=%d" % start_err
	var endpoints: Array = CamBANGServer.enumerate_devices()
	var count := endpoints.size()
	CamBANGServer.stop()
	if count == 0:
		return "no_endpoints_enumerated"
	return ""


# ---------------------------------------------------------------------------
# Snapshot helpers
# ---------------------------------------------------------------------------

func _wait_for_new_baseline(previous_gen: int, label: String) -> int:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return previous_gen
		await get_tree().process_frame
		for gen in _baseline_gens:
			if gen != previous_gen:
				return gen
	_fail("%s: timed out waiting for baseline publication" % label)
	return previous_gen




func _wait_for_live_stream_result(stream_id: int, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		var stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if stream_result != null:
			_step_ok("%s stream delivered a result" % label)
			return
	_fail("%s: timed out waiting for a stream result (no frames delivered)" % label)


func _wait_for_capture_result(device_instance_id: int, label: String):
	var device = CamBANGServer.get_device(device_instance_id)
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		if device == null:
			device = CamBANGServer.get_device(device_instance_id)
			continue
		var result = device.get_result()
		if result != null:
			return result
	_fail("%s: timed out waiting for a capture result after trigger_capture() returned OK" % label)
	return null


func _wait_for_report_decided(target_kind: String, target_id: int, label: String) -> Dictionary:
	var last_report := {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_report(target_kind, target_id)
		if report.is_empty():
			continue
		last_report = report
		if not _plan_valid(report, "requested"):
			continue
		if _report_has_decision(report):
			return report
	_fail("%s: timed out waiting for a decided %s backing-plan report (target_id=%d): %s" % [
		label, target_kind, target_id, JSON.stringify(last_report)
	])
	return {}


func _wait_for_capture_ready(device, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		var profile = device.get_still_capture_profile()
		if typeof(profile) != TYPE_DICTIONARY:
			continue
		var p: Dictionary = profile
		if int(p.get("width", 0)) > 0 and int(p.get("height", 0)) > 0 and int(p.get("format_fourcc", 0)) != 0:
			return
	_fail("%s: device did not advertise a valid still-capture profile (capture never became ready)" % label)


func _wait_for_report_present_or_decided(target_kind: String, target_id: int, label: String) -> Dictionary:
	# Returns as soon as a report for this target exists with a valid requested
	# plan (evaluation engaged), whether or not it has decided yet. The caller's
	# probe loop checks _report_has_decision and re-probes if not, so this just
	# needs to hand back the current report promptly.
	var last_report := {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_report(target_kind, target_id)
		if report.is_empty():
			continue
		last_report = report
		if _plan_valid(report, "requested"):
			return report
	_fail("%s: timed out waiting for a %s backing-plan report (target_id=%d): %s" % [
		label, target_kind, target_id, JSON.stringify(last_report)
	])
	return {}


func _get_report(target_kind: String, target_id: int) -> Dictionary:
	for report_v in _get_backing_plan_evaluation_reports():
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if str(report.get("target_kind", "")) == target_kind and int(report.get("target_id", 0)) == target_id:
			return report
	return {}


func _get_backing_plan_evaluation_reports() -> Array:
	# Provider-neutral Core accessor -- available under synthetic and
	# platform-backed alike, unlike the synthetic-metrics crutch 568 uses.
	var reports = CamBANGServer.get_backing_plan_evaluation_diagnostics()
	return reports if typeof(reports) == TYPE_ARRAY else []



func _report_has_decision(report: Dictionary) -> bool:
	if _plan_valid(report, "decision_selected"):
		return true
	if bool(report.get("evaluator_active", false)):
		return false
	if not _plan_valid(report, "requested") or not _plan_valid(report, "steady"):
		return false
	return _plan_posture(report, "requested") == _plan_posture(report, "steady")


func _plan_valid(report: Dictionary, key: String) -> bool:
	var plan_v = report.get(key, {})
	if typeof(plan_v) != TYPE_DICTIONARY:
		return false
	return bool((plan_v as Dictionary).get("valid", false))


func _plan_posture(report: Dictionary, key: String) -> String:
	var plan_v = report.get(key, {})
	if typeof(plan_v) != TYPE_DICTIONARY:
		return ""
	return str((plan_v as Dictionary).get("posture", ""))


# ---------------------------------------------------------------------------
# Verdict plumbing (same contract as 568)
# ---------------------------------------------------------------------------

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
	var detail := message
	if detail.begins_with("FAIL: "):
		detail = detail.substr(6)
	_emit_harness_verdict("fail", 1, "failure")
	push_error("step %d FAIL: %s" % [_step, detail])
	printerr("FAIL: %s" % detail)
	print("FAIL: %s" % detail)
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
