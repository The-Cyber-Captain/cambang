extends Node

## Scene 568: backing-plan evaluation verify
##
## Purpose:
## - replace legacy Scene 68 with a smaller, source-faithful verifier for the
##   current backing-plan evaluation model
## - prove stream evaluation is scoped to the stream parent and capture
##   evaluation is scoped to the capture parent seam
## - prove access-only public probes can still seed measured evidence for both
##   stream display and capture materialization without this scene calling
##   public to_image() methods itself
## - prove result-access evidence and backing-plan evaluation reports are
##   cleared by stop() and re-established after restart
## - prove a paused advance_timeline() path can safely handle exact-same-time
##   device+stream realization and later stream teardown/recreation
##
## Scope guardrail:
## - this is an automatable behavioral verifier, not a user-flow or teaching
##   scene
## - it uses a dual-device fixture as the minimum multi-device proof only; the
##   runtime contract remains N-device, not dual-device

const SCENE_LABEL := "backing_plan_evaluation_verify"
const SINGLE_SCENARIO_PATH := "res://scenarios/568_backing_plan_single_access_live.json"
const DUAL_SCENARIO_PATH := "res://scenarios/568_backing_plan_dual_live.json"
const CLOCKED_SCENARIO_PATH := "res://scenarios/568_backing_plan_edge_clocked.json"

const TOTAL_TIMEOUT_MS := 20000
const WAIT_TIMEOUT_FRAMES := 1200
const CAPTURE_RESULT_TIMEOUT_MS := 5000
const DEFAULT_STREAM_FRAME_PERIOD_NS := 33333333
const MAX_BACKING_PLAN_POSTURES := 3
const EVALUATION_FRAMES_PER_POSTURE := 2
const CLOCKED_FINAL_STREAM_DECISION_BUDGET_FRAMES := 24
const ADVANCE_INITIAL_NS := 1000000001
const ADVANCE_STREAM_START_NS := 1050000001
const ADVANCE_CAPTURE_SETTLE_NS := 1150000000
const CLOCKED_INITIAL_STREAM_DECISION_DEADLINE_NS := 1299999999
const ADVANCE_AFTER_TEARDOWN_NS := 1380000000
const ADVANCE_REOPEN_NS := 1600000001
const ADVANCE_FINAL_STREAM_NS := 1650000001

var _step := 0
var _done := false
var _quit_requested := false
var _start_ms := 0
var _baseline_gens: Array[int] = []
var _display_refs: Array = []
var _reported_evaluation_signatures := {}


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
	_log_maintainer_output_form_probe()
	if _synthetic_producer_output_form_effective_selection() == "gpu_only" and _scene568_is_compatibility_renderer():
		_info("EXPECTED_UNSUPPORTED: Scene 568 Compatibility gpu_only unsupported")
		_fail("Scene 568 requires a capture-capable Synthetic producer output form on the compatibility renderer; run with -- --cambang-synth-producer-output-form=cpu_gpu or cpu_only")
		return
	await _run_impl()


func _run_impl() -> void:
	var previous_gen := -1

	var single_gen := await _run_single_natural_pass(previous_gen)
	if _done:
		return
	previous_gen = single_gen
	await _stop_and_verify_reset("single_natural_reset")
	if _done:
		return

	var dual_gen := await _run_dual_natural_pass(previous_gen)
	if _done:
		return
	previous_gen = dual_gen
	await _stop_and_verify_reset("dual_natural_reset")
	if _done:
		return

	var clocked_gen := await _run_clocked_edge_pass(previous_gen)
	if _done:
		return
	previous_gen = clocked_gen
	await _stop_and_verify_reset("clocked_edge_reset")
	if _done:
		return

	_step_ok("backing-plan evaluation lifecycle, scoping, and clocked cleanup verified")
	_info("PASS: backing-plan evaluation lifecycle, scoping, and clocked cleanup verified")
	_cleanup_and_quit(0)


func _run_single_natural_pass(previous_gen: int) -> int:
	const LABEL := "single_natural"
	_bootstrap_runtime_and_stage_external_scenario(
		LABEL,
		SINGLE_SCENARIO_PATH,
		"568_backing_plan_single_access_live",
		false
	)
	if _done:
		return previous_gen
	var gen := await _wait_for_new_baseline(previous_gen, LABEL)
	if _done:
		return previous_gen

	var device_record := await _wait_for_device_record(gen, "synthetic:0", LABEL)
	if _done:
		return previous_gen
	await _wait_for_stream_absent(gen, "synthetic:0", "PREVIEW", LABEL)
	if _done:
		return previous_gen

	var device_instance_id := int(device_record.get("instance_id", 0))
	var capture_report := await _wait_for_capture_report(device_instance_id, LABEL)
	_assert_capture_report_shape(capture_report, device_instance_id, LABEL, false, false)
	if _done:
		return previous_gen
	_step_ok("%s capture parent report visible before explicit capture" % LABEL)

	await _trigger_capture_access_only(device_instance_id, LABEL)
	if _done:
		return previous_gen
	capture_report = await _complete_capture_parent_evaluation_after_probe(
		device_instance_id,
		capture_report,
		LABEL,
		1
	)
	_assert_capture_report_shape(capture_report, device_instance_id, LABEL, false)
	if _done:
		return previous_gen

	var stream_record := await _wait_for_stream_record(gen, "synthetic:0", "PREVIEW", "FLOWING", LABEL)
	if _done:
		return previous_gen
	var stream_id := int(stream_record.get("stream_id", 0))
	var stream_result: Variant = await _wait_for_stream_result(stream_id, LABEL)
	if _done:
		return previous_gen
	await _exercise_stream_access_without_public_to_image(stream_id, stream_result, LABEL)
	if _done:
		return previous_gen
	var evidence := await _wait_for_evidence_prefixes([
		"capture_to_image.",
		"stream_display_view.",
		"stream_to_image.",
	], LABEL)
	if _done:
		return previous_gen
	_assert_route_prefix_success(evidence, "capture_to_image.", LABEL)
	_assert_route_prefix_success(evidence, "stream_display_view.", LABEL)
	_assert_route_prefix_success(evidence, "stream_to_image.", LABEL)

	var stream_report := await _wait_for_stream_report_decided(stream_id, LABEL)
	_assert_stream_report_shape(stream_report, stream_id, device_instance_id, LABEL)
	if _done:
		return previous_gen
	_step_ok("%s access-only evaluation and measurement established" % LABEL)
	return gen


func _run_dual_natural_pass(previous_gen: int) -> int:
	const LABEL := "dual_natural"
	_bootstrap_runtime_and_stage_external_scenario(
		LABEL,
		DUAL_SCENARIO_PATH,
		"568_backing_plan_dual_live",
		false
	)
	if _done:
		return previous_gen
	var gen := await _wait_for_new_baseline(previous_gen, LABEL)
	if _done:
		return previous_gen

	var cam0_device := await _wait_for_device_record(gen, "synthetic:0", LABEL)
	if _done:
		return previous_gen
	var cam1_device := await _wait_for_device_record(gen, "synthetic:1", LABEL)
	if _done:
		return previous_gen

	var cam0_device_id := int(cam0_device.get("instance_id", 0))
	var cam1_device_id := int(cam1_device.get("instance_id", 0))

	var cam0_capture_report := await _wait_for_capture_report(cam0_device_id, LABEL)
	if _done:
		return previous_gen
	var cam1_capture_report := await _wait_for_capture_report(cam1_device_id, LABEL)
	if _done:
		return previous_gen
	_assert_capture_report_shape(cam0_capture_report, cam0_device_id, LABEL + "_cam0", false, false)
	_assert_capture_report_shape(cam1_capture_report, cam1_device_id, LABEL + "_cam1", false, false)
	if _done:
		return previous_gen
	_assert_reports_distinct(
		[cam0_capture_report, cam1_capture_report],
		"%s capture reports" % LABEL
	)
	if _done:
		return previous_gen

	await _trigger_capture_access_only(cam0_device_id, LABEL + "_cam0")
	if _done:
		return previous_gen
	await _trigger_capture_access_only(cam1_device_id, LABEL + "_cam1")
	if _done:
		return previous_gen
	cam0_capture_report = await _complete_capture_parent_evaluation_after_probe(
		cam0_device_id,
		cam0_capture_report,
		LABEL + "_cam0",
		1
	)
	if _done:
		return previous_gen
	cam1_capture_report = await _complete_capture_parent_evaluation_after_probe(
		cam1_device_id,
		cam1_capture_report,
		LABEL + "_cam1",
		1
	)
	if _done:
		return previous_gen

	var cam0_stream_record := await _wait_for_stream_record(gen, "synthetic:0", "PREVIEW", "FLOWING", LABEL)
	if _done:
		return previous_gen
	var cam1_stream_record := await _wait_for_stream_record(gen, "synthetic:1", "VIEWFINDER", "FLOWING", LABEL)
	if _done:
		return previous_gen
	var cam0_stream_id := int(cam0_stream_record.get("stream_id", 0))
	var cam1_stream_id := int(cam1_stream_record.get("stream_id", 0))

	var cam0_stream_result: Variant = await _wait_for_stream_result(cam0_stream_id, LABEL + "_cam0")
	if _done:
		return previous_gen
	var cam1_stream_result: Variant = await _wait_for_stream_result(cam1_stream_id, LABEL + "_cam1")
	if _done:
		return previous_gen
	await _exercise_stream_access_without_public_to_image(cam0_stream_id, cam0_stream_result, LABEL + "_cam0")
	if _done:
		return previous_gen
	await _exercise_stream_access_without_public_to_image(cam1_stream_id, cam1_stream_result, LABEL + "_cam1")
	if _done:
		return previous_gen

	var evidence := await _wait_for_evidence_prefixes([
		"capture_to_image.",
		"stream_display_view.",
		"stream_to_image.",
	], LABEL)
	if _done:
		return previous_gen
	_assert_route_prefix_success(evidence, "capture_to_image.", LABEL)
	_assert_route_prefix_success(evidence, "stream_display_view.", LABEL)
	_assert_route_prefix_success(evidence, "stream_to_image.", LABEL)

	var cam0_stream_report := await _wait_for_stream_report_decided(cam0_stream_id, LABEL + "_cam0")
	if _done:
		return previous_gen
	var cam1_stream_report := await _wait_for_stream_report_decided(cam1_stream_id, LABEL + "_cam1")
	if _done:
		return previous_gen
	_assert_stream_report_shape(cam0_stream_report, cam0_stream_id, cam0_device_id, LABEL + "_cam0")
	_assert_stream_report_shape(cam1_stream_report, cam1_stream_id, cam1_device_id, LABEL + "_cam1")
	if _done:
		return previous_gen

	_assert_reports_distinct(
		[cam0_stream_report, cam1_stream_report],
		"%s stream reports" % LABEL
	)
	if _done:
		return previous_gen
	_step_ok("%s multi-device scoping verified" % LABEL)
	return gen


func _run_clocked_edge_pass(previous_gen: int) -> int:
	const LABEL := "clocked_edge"
	var gen := await _bootstrap_runtime_and_stage_external_scenario_paused_after_baseline(
		previous_gen,
		LABEL,
		CLOCKED_SCENARIO_PATH,
		"568_backing_plan_edge_clocked"
	)
	if _done:
		return previous_gen

	await _advance_timeline_to(ADVANCE_STREAM_START_NS, LABEL)
	if _done:
		return previous_gen
	var initial_device := await _wait_for_device_record_clocked(gen, "synthetic:0", LABEL)
	if _done:
		return previous_gen
	var initial_device_id := int(initial_device.get("instance_id", 0))
	var initial_stream_record := await _wait_for_stream_record_clocked(
		gen,
		"synthetic:0",
		"PREVIEW",
		"FLOWING",
		LABEL
	)
	if _done:
		return previous_gen
	var initial_stream_id := int(initial_stream_record.get("stream_id", 0))
	_require(initial_stream_id != 0, "%s: initial stream_id missing after exact-same-time open/create plateau" % LABEL)
	if _done:
		return previous_gen
	_step_ok("%s exact-same-time device+stream realization observed" % LABEL)
	var initial_device_handle: Variant = await _wait_for_device_handle(initial_device_id, LABEL + "_initial")
	if _done:
		return previous_gen
	var initial_capture_report_before_probe := await _wait_for_capture_report(initial_device_id, LABEL + "_initial")
	_assert_capture_report_shape(
		initial_capture_report_before_probe,
		initial_device_id,
		LABEL + "_initial",
		false,
		false
	)
	if _done:
		return previous_gen
	await _wait_for_device_capture_ready(initial_device_id, LABEL + "_initial")
	if _done:
		return previous_gen
	var initial_baseline_progress := _get_capture_progress_snapshot(initial_device_id)
	var initial_baseline_failed := int(initial_baseline_progress.get("captures_failed", 0))
	var initial_baseline_completed := int(initial_baseline_progress.get("captures_completed", 0))
	var initial_capture_err := int(initial_device_handle.trigger_capture())
	_require(initial_capture_err == OK, "%s_initial: device.trigger_capture() failed err=%d" % [LABEL, initial_capture_err])
	if _done:
		return previous_gen
	_step_ok("%s capture trigger accepted during exact-same-time plateau" % LABEL)
	var initial_stream_result: Variant = await _wait_for_stream_result(initial_stream_id, LABEL + "_initial")
	if _done:
		return previous_gen
	await _exercise_stream_access_without_public_to_image(initial_stream_id, initial_stream_result, LABEL + "_initial")
	if _done:
		return previous_gen

	await _advance_timeline_to(ADVANCE_CAPTURE_SETTLE_NS, LABEL)
	if _done:
		return previous_gen
	var initial_capture_result: Variant = await _wait_for_capture_result(
		initial_device_handle,
		initial_device_id,
		initial_baseline_failed,
		initial_baseline_completed,
		LABEL + "_initial"
	)
	if _done:
		return previous_gen
	_probe_capture_result_access_only(initial_capture_result, LABEL + "_initial")
	if _done:
		return previous_gen

	var initial_post_probe_progress := _get_capture_progress_snapshot(initial_device_id)
	var initial_capture_report := await _complete_clocked_capture_parent_evaluation_after_probe(
		initial_device_handle,
		initial_device_id,
		int(initial_post_probe_progress.get("captures_failed", initial_baseline_failed)),
		int(initial_post_probe_progress.get("captures_completed", initial_baseline_completed + 1)),
		initial_capture_report_before_probe,
		LABEL + "_initial",
		CLOCKED_INITIAL_STREAM_DECISION_DEADLINE_NS,
		_stream_frame_period_ns(initial_stream_record),
		1
	)
	if _done:
		return previous_gen
	_assert_capture_report_shape(initial_capture_report, initial_device_id, LABEL + "_initial", true)
	if _done:
		return previous_gen
	var initial_stream_report := {}
	var initial_stream_report_still_present := not _get_stream_backing_plan_evaluation_report(initial_stream_id).is_empty()
	if _current_virtual_timeline_ns() >= ADVANCE_AFTER_TEARDOWN_NS or not initial_stream_report_still_present:
		await _wait_for_stream_report_absent_clocked(initial_stream_id, LABEL + "_initial")
		if _done:
			return previous_gen
		_step_ok("%s initial stream evaluation was overtaken by teardown and cleaned up" % LABEL)
	else:
		initial_stream_report = await _advance_clocked_stream_evaluation_until_decided(
			initial_stream_id,
			initial_stream_record,
			LABEL + "_initial",
			CLOCKED_INITIAL_STREAM_DECISION_DEADLINE_NS,
			0,
			true
		)
		if _done:
			return previous_gen
		if initial_stream_report.is_empty():
			_step_ok("%s initial stream evaluation did not decide before teardown window" % LABEL)
		else:
			_assert_stream_report_shape(initial_stream_report, initial_stream_id, initial_device_id, LABEL + "_initial")
			if _done:
				return previous_gen

	await _advance_timeline_to(ADVANCE_AFTER_TEARDOWN_NS, LABEL, true)
	if _done:
		return previous_gen
	await _wait_for_stream_report_absent_clocked(initial_stream_id, LABEL)
	if _done:
		return previous_gen
	_step_ok("%s teardown cleaned initial stream evaluation state" % LABEL)

	await _advance_timeline_to(ADVANCE_REOPEN_NS, LABEL, true)
	if _done:
		return previous_gen
	var final_device := await _wait_for_device_record_clocked(gen, "synthetic:0", LABEL + "_reopen")
	if _done:
		return previous_gen
	await _advance_timeline_to(ADVANCE_FINAL_STREAM_NS, LABEL, true)
	if _done:
		return previous_gen
	var final_stream_record := await _wait_for_stream_record_clocked(
		gen,
		"synthetic:0",
		"PREVIEW",
		"FLOWING",
		LABEL + "_final"
	)
	if _done:
		return previous_gen
	var final_stream_id := int(final_stream_record.get("stream_id", 0))
	_require(final_stream_id != 0 and final_stream_id != initial_stream_id, "%s: final stream did not realize as a distinct stream context" % LABEL)
	if _done:
		return previous_gen
	var final_stream_result: Variant = await _wait_for_stream_result(final_stream_id, LABEL + "_final")
	if _done:
		return previous_gen
	await _exercise_stream_access_without_public_to_image(final_stream_id, final_stream_result, LABEL + "_final")
	if _done:
		return previous_gen

	var final_capture_report := await _wait_for_capture_report(int(final_device.get("instance_id", 0)), LABEL + "_final")
	if _done:
		return previous_gen
	var final_stream_deadline_ns := (
		_current_virtual_timeline_ns()
		+ (_stream_frame_period_ns(final_stream_record) * CLOCKED_FINAL_STREAM_DECISION_BUDGET_FRAMES)
	)
	var final_stream_report := await _advance_clocked_stream_evaluation_until_decided(
		final_stream_id,
		final_stream_record,
		LABEL + "_final",
		final_stream_deadline_ns,
		0
	)
	if _done:
		return previous_gen
	_assert_capture_report_shape(final_capture_report, int(final_device.get("instance_id", 0)), LABEL + "_final", false, false)
	_assert_stream_report_shape(final_stream_report, final_stream_id, int(final_device.get("instance_id", 0)), LABEL + "_final")
	if _done:
		return previous_gen
	_step_ok("%s paused advance_timeline edge path verified" % LABEL)
	return gen


func _bootstrap_runtime_and_stage_external_scenario(
	label: String,
	scenario_path: String,
	scenario_name: String,
	start_paused: bool
) -> void:
	_display_refs.clear()
	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	_require(start_err == OK, "%s: synthetic timeline start rejected (%d)" % [label, start_err])
	if _done:
		return
	_require(CamBANGServer.get_state_snapshot() == null, "%s: expected NIL snapshot immediately after start() before baseline publish" % label)
	if _done:
		return
	_step_ok("%s synthetic runtime started" % label)

	var scenario_text := FileAccess.get_file_as_string(scenario_path)
	_require(scenario_text != "", "%s: scenario missing at %s" % [label, scenario_path])
	if _done:
		return
	var stage_err := CamBANGServer.load_external_scenario(scenario_text)
	_require(stage_err == OK, "%s: unable to load external scenario (%d)" % [label, stage_err])
	if _done:
		return
	_step_ok("%s external scenario staged (%s)" % [label, scenario_name])

	var scenario_start_err := CamBANGServer.start_scenario()
	_require(scenario_start_err == OK, "%s: unable to start staged scenario (%d)" % [label, scenario_start_err])
	if _done:
		return
	if start_paused:
		var pause_err := CamBANGServer.set_timeline_paused(true)
		_require(pause_err == OK, "%s: set_timeline_paused(true) failed (%d)" % [label, pause_err])
		if _done:
			return
	_step_ok("%s scenario started%s" % [label, (" paused" if start_paused else "")])


func _bootstrap_runtime_and_stage_external_scenario_paused_after_baseline(
	previous_gen: int,
	label: String,
	scenario_path: String,
	scenario_name: String
) -> int:
	_display_refs.clear()
	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	_require(start_err == OK, "%s: synthetic timeline start rejected (%d)" % [label, start_err])
	if _done:
		return previous_gen
	_require(CamBANGServer.get_state_snapshot() == null, "%s: expected NIL snapshot immediately after start() before baseline publish" % label)
	if _done:
		return previous_gen
	_step_ok("%s synthetic runtime started" % label)

	var scenario_text := FileAccess.get_file_as_string(scenario_path)
	_require(scenario_text != "", "%s: scenario missing at %s" % [label, scenario_path])
	if _done:
		return previous_gen
	var stage_err := CamBANGServer.load_external_scenario(scenario_text)
	_require(stage_err == OK, "%s: unable to load external scenario (%d)" % [label, stage_err])
	if _done:
		return previous_gen
	_step_ok("%s external scenario staged (%s)" % [label, scenario_name])

	var gen := await _wait_for_new_baseline(previous_gen, label)
	if _done:
		return previous_gen
	var scenario_start_err := CamBANGServer.start_scenario()
	_require(scenario_start_err == OK, "%s: unable to start staged scenario (%d)" % [label, scenario_start_err])
	if _done:
		return previous_gen
	await get_tree().process_frame
	if _done:
		return previous_gen
	var pause_err := CamBANGServer.set_timeline_paused(true)
	_require(pause_err == OK, "%s: set_timeline_paused(true) failed (%d)" % [label, pause_err])
	if _done:
		return previous_gen
	_step_ok("%s scenario started paused" % label)
	return gen


func _wait_for_new_baseline(previous_gen: int, label: String) -> int:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return previous_gen
		await get_tree().process_frame
		for gen in _baseline_gens:
			if gen > previous_gen:
				return gen
	_fail("%s: timed out waiting for new baseline publish after gen=%d" % [label, previous_gen])
	return previous_gen


func _wait_for_device_record(expected_gen: int, hardware_id: String, label: String) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot := _snapshot_for_gen(expected_gen)
		if snapshot.is_empty():
			continue
		var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
		if not device_record.is_empty():
			return device_record
	_fail("%s: timed out waiting for device %s" % [label, hardware_id])
	return {}


func _wait_for_device_record_clocked(expected_gen: int, hardware_id: String, label: String) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot := _snapshot_for_gen(expected_gen)
		if not snapshot.is_empty():
			var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
			if not device_record.is_empty():
				return device_record
		_drain_clocked_current_time(label)
		if _done:
			return {}
	_fail("%s: timed out waiting for clocked device %s" % [label, hardware_id])
	return {}


func _wait_for_no_device(expected_gen: int, hardware_id: String, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		var snapshot := _snapshot_for_gen(expected_gen)
		if snapshot.is_empty():
			continue
		if _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id).is_empty():
			return
	_fail("%s: timed out waiting for device %s to disappear" % [label, hardware_id])


func _wait_for_stream_record(
	expected_gen: int,
	hardware_id: String,
	intent: String,
	required_mode: String,
	label: String
) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot := _snapshot_for_gen(expected_gen)
		if snapshot.is_empty():
			continue
		var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
		if device_record.is_empty():
			continue
		var stream_record := _find_stream_snapshot_record(
			snapshot,
			int(device_record.get("instance_id", 0)),
			intent,
			required_mode
		)
		if not stream_record.is_empty():
			return stream_record
	_fail("%s: timed out waiting for stream hardware_id=%s intent=%s mode=%s" % [
		label,
		hardware_id,
		intent,
		required_mode
	])
	return {}


func _wait_for_stream_record_clocked(
	expected_gen: int,
	hardware_id: String,
	intent: String,
	required_mode: String,
	label: String
) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot := _snapshot_for_gen(expected_gen)
		if not snapshot.is_empty():
			var stream_id := _stream_id_from_snapshot(snapshot, hardware_id, intent, required_mode)
			if stream_id != 0:
				var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
				var device_instance_id := int(device_record.get("instance_id", 0))
				var stream_record := _find_stream_snapshot_record(snapshot, device_instance_id, intent, required_mode)
				if not stream_record.is_empty():
					return stream_record
		_drain_clocked_current_time(label)
		if _done:
			return {}
	_fail("%s: timed out waiting for clocked stream hardware_id=%s intent=%s mode=%s" % [
		label,
		hardware_id,
		intent,
		required_mode
	])
	return {}


func _wait_for_stream_absent(expected_gen: int, hardware_id: String, intent: String, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		var snapshot := _snapshot_for_gen(expected_gen)
		if snapshot.is_empty():
			continue
		if _stream_id_from_snapshot(snapshot, hardware_id, intent) == 0:
			return
	_fail("%s: timed out waiting for stream %s/%s to stay absent" % [label, hardware_id, intent])


func _wait_for_stream_result(stream_id: int, label: String):
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if stream_result != null:
			return stream_result
	_fail("%s: timed out waiting for stream result for stream_id=%d" % [label, stream_id])
	return null


func _trigger_clocked_capture_access_only(
	device,
	device_instance_id: int,
	baseline_failed: int,
	baseline_completed: int,
	label: String,
	deadline_ns: int,
	frame_step_ns: int
) -> Dictionary:
	var capture_err := int(device.trigger_capture())
	_require(capture_err == OK, "%s: device.trigger_capture() failed err=%d" % [label, capture_err])
	if _done:
		return {}
	var capture_result: Variant = await _wait_for_clocked_capture_result(
		device,
		device_instance_id,
		baseline_failed,
		baseline_completed,
		label,
		deadline_ns,
		frame_step_ns
	)
	if _done:
		return {}
	_probe_capture_result_access_only(capture_result, label)
	if _done:
		return {}
	return _get_capture_progress_snapshot(device_instance_id)


func _wait_for_clocked_capture_result(
	device,
	device_instance_id: int,
	baseline_failed: int,
	baseline_completed: int,
	label: String,
	deadline_ns: int,
	frame_step_ns: int
):
	while true:
		if _timed_out():
			return null
		await get_tree().process_frame
		var progress := _get_capture_progress_snapshot(device_instance_id)
		if bool(progress.get("available", false)) and int(progress.get("captures_failed", 0)) > baseline_failed:
			_fail("%s: capture failed after trigger (progress=%s)" % [label, str(progress)])
			return null
		var capture_result = device.get_result()
		if capture_result != null and int(capture_result.get_image_count()) > 0:
			return capture_result
		var current_ns := _current_virtual_timeline_ns()
		if deadline_ns > 0 and current_ns >= deadline_ns:
			break
		var step_ns := maxi(frame_step_ns, 1)
		var next_ns := current_ns + step_ns
		if deadline_ns > 0 and next_ns > deadline_ns:
			next_ns = deadline_ns
		var delta_ns := next_ns - current_ns
		if delta_ns <= 0:
			break
		var advance_err := CamBANGServer.advance_timeline(delta_ns)
		_require(
			advance_err == OK,
			"%s: advance_timeline(%d) failed while waiting for clocked capture result (%d)" % [
				label,
				delta_ns,
				advance_err,
			]
		)
		if _done:
			return null
		if bool(progress.get("available", false)) and int(progress.get("captures_completed", 0)) > baseline_completed:
			continue
	_fail(
		"%s: timed out waiting for completed clocked capture result before deadline_ns=%d baseline_completed=%d" % [
			label,
			deadline_ns,
			baseline_completed,
		]
	)
	return null


func _trigger_capture_access_only(
	device_instance_id: int,
	label: String,
	emit_step_ok: bool = true,
	require_completed_result: bool = true
) -> Dictionary:
	var device: Variant = await _wait_for_device_handle(device_instance_id, label)
	if _done:
		return {}
	await _wait_for_device_capture_ready(device_instance_id, label)
	if _done:
		return {}
	var baseline_progress := _get_capture_progress_snapshot(device_instance_id)
	var baseline_failed := int(baseline_progress.get("captures_failed", 0))
	var baseline_completed := int(baseline_progress.get("captures_completed", 0))
	var capture_err := int(device.trigger_capture())
	_require(capture_err == OK, "%s: device.trigger_capture() failed err=%d" % [label, capture_err])
	if _done:
		return {}
	var capture_result: Variant = await _wait_for_capture_result(
		device,
		device_instance_id,
		baseline_failed,
		baseline_completed,
		label,
		require_completed_result
	)
	if _done:
		return {}
	var capture_result_observed := capture_result != null
	var capture_id := 0
	var acquisition_session_id := 0
	if capture_result_observed:
		_probe_capture_result_access_only(capture_result, label)
		if _done:
			return {}
		capture_id = int(capture_result.get_capture_id())
		acquisition_session_id = _find_acquisition_session_id_for_capture(
			device_instance_id,
			capture_id
		)
	elif not require_completed_result:
		_info(
			"%s: capture result did not complete inside probe wait budget; continuing with report-driven follow-up" % [
				label
			]
		)
	if emit_step_ok:
		if capture_result_observed:
			_step_ok("%s capture access-only probe completed" % label)
		else:
			_step_ok("%s capture probe trigger accepted; continuing with report-driven follow-up" % label)
	return {
		"capture_id": capture_id,
		"acquisition_session_id": acquisition_session_id,
		"capture_result_observed": capture_result_observed,
	}


func _complete_capture_parent_evaluation_after_probe(
	device_instance_id: int,
	previous_report: Dictionary,
	label: String,
	probes_completed: int
) -> Dictionary:
	var report := await _wait_for_capture_report_change_or_decision(
		device_instance_id,
		previous_report,
		label
	)
	if _done:
		return {}
	_assert_capture_report_shape(report, device_instance_id, label, false)
	if _done:
		return {}
	var total_probes := probes_completed
	while not _report_has_decision(report):
		var candidate_postures := _decision_candidate_postures(report)
		var max_total_probes := candidate_postures.size()
		if max_total_probes <= 0:
			max_total_probes = 1
		max_total_probes = mini(max_total_probes, MAX_BACKING_PLAN_POSTURES)
		_require(
			total_probes < max_total_probes,
			"%s: capture report remained undecided after %d access-only probe(s); report=%s" % [
				label,
				total_probes,
				JSON.stringify(report),
			]
		)
		if _done:
			return {}
		var pre_probe_report := report
		var probe_info := await _trigger_capture_access_only(
			device_instance_id,
			"%s_probe_%d" % [label, total_probes + 1],
			false,
			false
		)
		if _done:
			return {}
		previous_report = report
		report = await _wait_for_capture_report_change_or_decision(
			device_instance_id,
			previous_report,
			label
		)
		if _done:
			return {}
		_assert_capture_report_shape(report, device_instance_id, label, false)
		if _done:
			return {}
		total_probes += 1
		if not _report_has_decision(report) and total_probes >= max_total_probes:
			var prior_viable_postures := _decision_candidate_postures(pre_probe_report)
			if _capture_report_has_clean_seed_only_epoch(report, prior_viable_postures):
				_info("%s: capture evaluation completed with clean seed-only epoch rollover" % label)
				return report
			if _capture_report_is_active_final_candidate_without_decision(report):
				report = await _wait_for_capture_report_follow_up_after_final_probe(
					device_instance_id,
					report,
					prior_viable_postures,
					label
				)
				if _done:
					return {}
				_assert_capture_report_shape(report, device_instance_id, label, false)
				if _done:
					return {}
				if _report_has_decision(report):
					return report
				if _capture_report_has_clean_seed_only_epoch(report, prior_viable_postures):
					_info("%s: capture evaluation completed with clean seed-only epoch rollover" % label)
					return report
			if _capture_report_is_clean_immediate_rollover(
				pre_probe_report,
				report,
				probe_info,
				label
			):
				_info("%s: capture evaluation completed with immediate parent-retirement rollover" % label)
				return report
			if _capture_report_is_clean_immediate_rollover(
				pre_probe_report,
				report,
				probe_info,
				label
			):
				_info("%s: capture evaluation completed with immediate parent-retirement rollover" % label)
				return report
	return report


func _complete_clocked_capture_parent_evaluation_after_probe(
	device,
	device_instance_id: int,
	baseline_failed: int,
	baseline_completed: int,
	previous_report: Dictionary,
	label: String,
	deadline_ns: int,
	frame_step_ns: int,
	probes_completed: int
) -> Dictionary:
	var report := await _wait_for_capture_report_change_or_decision(
		device_instance_id,
		previous_report,
		label
	)
	if _done:
		return {}
	_assert_capture_report_shape(report, device_instance_id, label, false)
	if _done:
		return {}
	var total_probes := probes_completed
	var current_failed := baseline_failed
	var current_completed := baseline_completed
	while not _report_has_decision(report):
		var candidate_postures := _decision_candidate_postures(report)
		var max_total_probes := candidate_postures.size()
		if max_total_probes <= 0:
			max_total_probes = 1
		max_total_probes = mini(max_total_probes, MAX_BACKING_PLAN_POSTURES)
		_require(
			total_probes < max_total_probes,
			"%s: clocked capture report remained undecided after %d access-only probe(s); report=%s" % [
				label,
				total_probes,
				JSON.stringify(report),
			]
		)
		if _done:
			return {}
		var progress := await _trigger_clocked_capture_access_only(
			device,
			device_instance_id,
			current_failed,
			current_completed,
			"%s_probe_%d" % [label, total_probes + 1],
			deadline_ns,
			frame_step_ns
		)
		if _done:
			return {}
		current_failed = int(progress.get("captures_failed", current_failed))
		current_completed = int(progress.get("captures_completed", current_completed))
		previous_report = report
		report = await _wait_for_capture_report_change_or_decision(
			device_instance_id,
			previous_report,
			label
		)
		if _done:
			return {}
		_assert_capture_report_shape(report, device_instance_id, label, false)
		if _done:
			return {}
		if not _report_has_decision(report):
			for _settle_i in range(EVALUATION_FRAMES_PER_POSTURE):
				if _timed_out():
					return report
				await get_tree().process_frame
		total_probes += 1
	return report


func _probe_capture_result_access_only(capture_result, label: String) -> void:
	_require(capture_result != null, "%s: capture_result is null" % label)
	if _done:
		return
	var to_image_capability := int(capture_result.can_to_image())
	var member0_capability := int(capture_result.can_to_image_member(0))
	_require(
		to_image_capability != int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED),
		"%s: capture_result.can_to_image() returned unsupported" % label
	)
	if _done:
		return
	_require(
		member0_capability != int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED),
		"%s: capture_result.can_to_image_member(0) returned unsupported" % label
	)


func _wait_for_device_handle(device_instance_id: int, label: String):
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var device = CamBANGServer.get_device(device_instance_id)
		if device != null:
			return device
	_fail("%s: get_device(%d) did not become available" % [label, device_instance_id])
	return null


func _wait_for_device_capture_ready(device_instance_id: int, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		var still_profile := _extract_snapshot_still_profile(_get_device_snapshot_record(device_instance_id))
		if still_profile.is_empty():
			continue
		var version := int(still_profile.get("version", -1))
		var width := int(still_profile.get("width", 0))
		var height := int(still_profile.get("height", 0))
		var format := int(still_profile.get("format", 0))
		if version != 0 or width <= 0 or height <= 0 or format == 0:
			continue
		var members := _extract_still_image_bundle_members(still_profile)
		if members.size() != 1:
			continue
		if typeof(members[0]) != TYPE_DICTIONARY:
			continue
		var member: Dictionary = members[0]
		if int(member.get("image_member_index", -1)) != 0:
			continue
		if int(member.get("role", -1)) != CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED:
			continue
		if int(member.get("intended_exposure_compensation_milli_ev", 1)) != 0:
			continue
		return
	_fail("%s: default still profile did not become snapshot-visible with version=0 and one default metered member" % label)


func _wait_for_capture_result(
	device,
	device_instance_id: int,
	baseline_failed: int,
	baseline_completed: int,
	label: String,
	require_completed_result: bool = true
):
	var start_ms := Time.get_ticks_msec()
	while not _timed_out():
		if _timed_out():
			return null
		await get_tree().process_frame
		var progress := _get_capture_progress_snapshot(device_instance_id)
		if bool(progress.get("available", false)) and int(progress.get("captures_failed", 0)) > baseline_failed:
			_fail("%s: capture failed after trigger (progress=%s)" % [label, str(progress)])
			return null
		var capture_result = device.get_result()
		if capture_result != null and int(capture_result.get_image_count()) > 0:
			return capture_result
		if Time.get_ticks_msec() - start_ms >= CAPTURE_RESULT_TIMEOUT_MS:
			break
		if not bool(progress.get("available", false)):
			continue
		if int(progress.get("captures_completed", 0)) <= baseline_completed:
			continue
	if require_completed_result:
		_fail("%s: timed out waiting for completed capture result" % label)
	return null


func _exercise_stream_access_without_public_to_image(stream_id: int, stream_result, label: String) -> void:
	_require(stream_result != null, "%s: stream_result is null" % label)
	if _done:
		return
	stream_result.can_to_image()
	var display_view = stream_result.get_display_view()
	if display_view is Texture2D:
		_display_refs.append(display_view)
		_step_ok("%s stream display demand established immediately" % label)
		return

	var display_ready := await _wait_for_stream_display_view_after_demand(stream_id, label)
	if _done:
		return
	var latest_display_view = display_ready.get("display_view", null)
	_require(latest_display_view is Texture2D, "%s: stream_result.get_display_view() did not become available after demand" % label)
	if _done:
		return
	_display_refs.append(latest_display_view)
	_step_ok("%s stream display demand established after fresh result" % label)


func _wait_for_stream_display_view_after_demand(stream_id: int, label: String) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var latest_stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if latest_stream_result == null:
			continue
		if int(latest_stream_result.can_get_display_view()) == int(latest_stream_result.CAPABILITY_UNSUPPORTED):
			continue
		var latest_display_view = latest_stream_result.get_display_view()
		if latest_display_view is Texture2D:
			return {
				"stream_result": latest_stream_result,
				"display_view": latest_display_view,
			}
	_fail("%s: timed out waiting for fresh stream display_view after demand" % label)
	return {}


func _wait_for_capture_report(device_instance_id: int, label: String) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if not report.is_empty():
			_emit_evaluation_info_if_changed(report, label)
			return report
	_fail("%s: timed out waiting for capture backing-plan evaluation report for device_instance_id=%d" % [label, device_instance_id])
	return {}


func _wait_for_capture_report_decided(device_instance_id: int, label: String) -> Dictionary:
	var last_report := {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if report.is_empty():
			continue
		last_report = report
		_emit_evaluation_info_if_changed(report, label)
		if not _plan_valid(report, "requested"):
			continue
		if _report_has_decision(report):
			return report
	_fail("%s: timed out waiting for decided capture backing-plan evaluation report: %s" % [label, JSON.stringify(last_report)])
	return {}


func _wait_for_capture_report_change_or_decision(
	device_instance_id: int,
	previous_report: Dictionary,
	label: String
) -> Dictionary:
	var previous_signature := _report_signature(previous_report)
	var last_report := previous_report
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if report.is_empty():
			continue
		last_report = report
		_emit_evaluation_info_if_changed(report, label)
		if not _plan_valid(report, "requested"):
			continue
		if _report_has_decision(report):
			return report
		if _report_signature(report) != previous_signature:
			return report
	_fail(
		"%s: timed out waiting for capture backing-plan evaluation report progress after probe: %s" % [
			label,
			JSON.stringify(last_report),
		]
	)
	return {}


func _wait_for_capture_report_follow_up_after_final_probe(
	device_instance_id: int,
	previous_report: Dictionary,
	prior_viable_postures: Array,
	label: String
) -> Dictionary:
	var previous_signature := _report_signature(previous_report)
	var last_report := previous_report
	var device: Variant = CamBANGServer.get_device(device_instance_id)
	var follow_up_capture_result_probed := false
	while not _timed_out():
		if _timed_out():
			return {}
		await get_tree().process_frame
		if not follow_up_capture_result_probed:
			if device == null:
				device = CamBANGServer.get_device(device_instance_id)
			if device != null:
				var capture_result = device.get_result()
				if capture_result != null and int(capture_result.get_image_count()) > 0:
					_probe_capture_result_access_only(
						capture_result,
						"%s_follow_up_result" % label
					)
					if _done:
						return {}
					follow_up_capture_result_probed = true
					_info(
						"%s: follow-up capture result became available while waiting for final candidate report completion" % [
							label
						]
					)
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if report.is_empty():
			continue
		last_report = report
		_emit_evaluation_info_if_changed(report, label)
		if not _plan_valid(report, "requested"):
			continue
		if _report_has_decision(report):
			return report
		if _capture_report_has_clean_seed_only_epoch(report, prior_viable_postures):
			return report
		if _report_signature(report) != previous_signature:
			return report
	_fail(
		"%s: timed out waiting for capture backing-plan evaluation follow-up after final probe: %s" % [
			label,
			JSON.stringify(last_report),
		]
	)
	return {}


func _wait_for_stream_report_decided(stream_id: int, label: String) -> Dictionary:
	var last_report := {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_stream_backing_plan_evaluation_report(stream_id)
		if report.is_empty():
			continue
		last_report = report
		_emit_evaluation_info_if_changed(report, label)
		if _report_has_decision(report):
			return report
	_fail("%s: timed out waiting for decided stream backing-plan evaluation report: %s" % [label, JSON.stringify(last_report)])
	return {}


func _advance_clocked_stream_evaluation_until_decided(
	stream_id: int,
	stream_record: Dictionary,
	label: String,
	deadline_ns: int,
	max_frame_steps: int,
	allow_teardown_overtake: bool = false
) -> Dictionary:
	var frame_step_ns := _stream_frame_period_ns(stream_record)
	var steps_taken := 0
	var last_report := {}
	while true:
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_stream_backing_plan_evaluation_report(stream_id)
		if not report.is_empty():
			last_report = report
			_emit_evaluation_info_if_changed(report, label)
			if _report_has_decision(report):
				return report
		var current_ns := _current_virtual_timeline_ns()
		var deadline_reached := deadline_ns > 0 and current_ns >= deadline_ns
		var step_budget_exhausted := max_frame_steps > 0 and steps_taken >= max_frame_steps
		if deadline_reached or step_budget_exhausted:
			if allow_teardown_overtake and deadline_reached:
				return {}
			break
		var next_ns := current_ns + frame_step_ns
		if deadline_ns > 0 and next_ns > deadline_ns:
			next_ns = deadline_ns
		var delta_ns := next_ns - current_ns
		if delta_ns <= 0:
			break
		var advance_err := CamBANGServer.advance_timeline(delta_ns)
		_require(
			advance_err == OK,
			"%s: advance_timeline(%d) failed while spending explicit evaluation budget (%d)" % [
				label,
				delta_ns,
				advance_err,
			]
		)
		if _done:
			return {}
		steps_taken += 1
	_fail(
		"%s: stream backing-plan evaluation did not decide within explicit paused budget report=%s current_ns=%d deadline_ns=%d steps=%d frame_step_ns=%d" % [
			label,
			JSON.stringify(last_report),
			_current_virtual_timeline_ns(),
			deadline_ns,
			steps_taken,
			frame_step_ns,
		]
	)
	return {}


func _wait_for_stream_report_absent(stream_id: int, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		if _get_stream_backing_plan_evaluation_report(stream_id).is_empty():
			_emit_evaluation_cleared_info("stream", stream_id, label)
			return
	_fail("%s: timed out waiting for stream backing-plan report removal for stream_id=%d" % [label, stream_id])


func _wait_for_stream_report_absent_clocked(stream_id: int, label: String) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		if _get_stream_backing_plan_evaluation_report(stream_id).is_empty():
			_emit_evaluation_cleared_info("stream", stream_id, label)
			return
		_drain_clocked_current_time(label)
		if _done:
			return
	_fail("%s: timed out waiting for clocked stream backing-plan report removal for stream_id=%d" % [label, stream_id])


func _wait_for_evidence_prefixes(prefixes: Array, label: String) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var evidence := _get_result_access_timing_evidence()
		if evidence.is_empty():
			continue
		var all_present := true
		for prefix_v in prefixes:
			if not _evidence_has_prefix(evidence, str(prefix_v)):
				all_present = false
				break
		if all_present:
			return evidence
	_fail("%s: timed out waiting for evidence prefixes %s" % [label, str(prefixes)])
	return {}


func _advance_timeline_to(
	target_ns: int,
	label: String,
	allow_already_past: bool = false
) -> void:
	var current_ns := _current_virtual_timeline_ns()
	if current_ns >= target_ns:
		_require(
			allow_already_past,
			"%s: advance target regressed (%d -> %d)" % [label, current_ns, target_ns]
		)
		if _done:
			return
		var zero_advance_err := CamBANGServer.advance_timeline(0)
		_require(
			zero_advance_err == OK,
			"%s: advance_timeline(0) failed while draining already-due work (%d)" % [
				label,
				zero_advance_err,
			]
		)
		if _done:
			return
		await get_tree().process_frame
		_step_ok("%s advance_timeline already at or past %d ns (%d)" % [label, target_ns, current_ns])
		return
	if _done:
		return
	var delta_ns := target_ns - current_ns
	if delta_ns == 0:
		return
	var advance_err := CamBANGServer.advance_timeline(delta_ns)
	_require(advance_err == OK, "%s: advance_timeline(%d) failed (%d)" % [label, delta_ns, advance_err])
	if _done:
		return
	var drain_err := CamBANGServer.advance_timeline(0)
	_require(
		drain_err == OK,
		"%s: advance_timeline(0) failed while draining current-time descendant work (%d)" % [
			label,
			drain_err,
		]
	)
	if _done:
		return
	await get_tree().process_frame
	_step_ok("%s advance_timeline reached %d ns" % [label, target_ns])


func _drain_clocked_current_time(label: String) -> void:
	var advance_err := CamBANGServer.advance_timeline(0)
	_require(
		advance_err == OK,
		"%s: advance_timeline(0) failed while draining current-time descendant work (%d)" % [
			label,
			advance_err,
		]
	)


func _stream_frame_period_ns(stream_record: Dictionary) -> int:
	var fps_max := int(stream_record.get("target_fps_max", 0))
	var fps_min := int(stream_record.get("target_fps_min", 0))
	var fps := fps_max if fps_max > 0 else fps_min
	if fps <= 0:
		return DEFAULT_STREAM_FRAME_PERIOD_NS
	return max(1, int(1000000000.0 / float(fps)))


func _current_virtual_timeline_ns() -> int:
	var metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(metrics) != TYPE_DICTIONARY:
		return 0
	return int(metrics.get("current_virtual_timeline_ns", 0))


func _synthetic_producer_output_form_setting() -> String:
	return str(ProjectSettings.get_setting("cambang/maintainer/synthetic_producer_output_form", "runtime_default"))


func _synthetic_producer_output_form_cmdline_selection() -> String:
	const PREFIX := "--cambang-synth-producer-output-form="
	return _single_namespaced_cmdline_selection(PREFIX)


func _single_namespaced_cmdline_selection(prefix: String) -> String:
	var found := ""
	for arg in OS.get_cmdline_user_args():
		var text := str(arg)
		if not text.begins_with(prefix):
			continue
		var value := text.substr(prefix.length())
		if found != "":
			return "<duplicate>"
		found = value
	if found != "":
		return found
	for arg in OS.get_cmdline_args():
		var text := str(arg)
		if not text.begins_with(prefix):
			continue
		var value := text.substr(prefix.length())
		if found != "":
			return "<duplicate>"
		found = value
	return found


func _synthetic_producer_output_form_effective_selection() -> String:
	var cmdline_selection := _synthetic_producer_output_form_cmdline_selection()
	if cmdline_selection != "":
		return cmdline_selection
	return _synthetic_producer_output_form_setting()


func _log_maintainer_output_form_probe() -> void:
	_info(
		"Synthetic producer output-form stored project setting: %s" % [
			_synthetic_producer_output_form_setting()
		]
	)
	var effective_selection := _synthetic_producer_output_form_effective_selection()
	var suffix := "stored project setting"
	if effective_selection != _synthetic_producer_output_form_setting():
		suffix = "transient command-line override"
	_info(
		"Synthetic producer output-form effective runtime selection: %s (%s)" % [
			effective_selection,
			suffix,
		]
	)


func _scene568_current_rendering_method() -> String:
	var runtime_method := str(RenderingServer.get_current_rendering_method())
	if runtime_method != "":
		return runtime_method
	return str(ProjectSettings.get_setting("rendering/renderer/rendering_method", ""))


func _scene568_cmdline_arg_matches_any_value(flag: String, accepted_values: Array[String]) -> bool:
	var args := OS.get_cmdline_args()
	var flag_with_equals := "%s=" % flag
	var index := 0
	while index < args.size():
		var text := str(args[index])
		if text == flag:
			if index + 1 >= args.size():
				return false
			var split_value := str(args[index + 1])
			if accepted_values.has(split_value):
				return true
			index += 2
			continue
		if text.begins_with(flag_with_equals):
			var equals_value := text.substr(flag_with_equals.length())
			if accepted_values.has(equals_value):
				return true
		index += 1
	return false


func _scene568_rendering_method_is_compatibility(rendering_method: String) -> bool:
	return rendering_method == "compatibility" or rendering_method == "gl_compatibility"


func _scene568_is_compatibility_renderer() -> bool:
	var rendering_method := _scene568_current_rendering_method()
	if _scene568_rendering_method_is_compatibility(rendering_method):
		return true
	if _scene568_cmdline_arg_matches_any_value("--rendering-method", ["compatibility", "gl_compatibility"]):
		return true
	return _scene568_cmdline_arg_matches_any_value("--rendering-driver", ["opengl3"])


func _snapshot_for_gen(expected_gen: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
		return {}
	if int(snapshot.get("gen", -1)) != expected_gen:
		return {}
	return snapshot


func _get_result_access_timing_evidence() -> Dictionary:
	var metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(metrics) != TYPE_DICTIONARY:
		return {}
	var evidence = metrics.get("result_access_timing_evidence", {})
	return evidence if typeof(evidence) == TYPE_DICTIONARY else {}


func _get_backing_plan_evaluation_reports() -> Array:
	var metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(metrics) != TYPE_DICTIONARY:
		return []
	var reports = metrics.get("backing_plan_evaluation_reports", [])
	return reports if typeof(reports) == TYPE_ARRAY else []


func _get_stream_backing_plan_evaluation_report(stream_id: int) -> Dictionary:
	for report_v in _get_backing_plan_evaluation_reports():
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if str(report.get("target_kind", "")) == "stream" and int(report.get("target_id", 0)) == stream_id:
			return report
	return {}


func _get_capture_backing_plan_evaluation_report(device_instance_id: int) -> Dictionary:
	for report_v in _get_backing_plan_evaluation_reports():
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if str(report.get("target_kind", "")) == "capture" and int(report.get("target_id", 0)) == device_instance_id:
			return report
	return {}


func _emit_evaluation_info_if_changed(report: Dictionary, label: String) -> void:
	if report.is_empty():
		return
	var key := _report_identity_key(report)
	if key == "":
		return
	var signature := _report_signature(report)
	if _reported_evaluation_signatures.get(key, "") == signature:
		return
	_reported_evaluation_signatures[key] = signature
	_info(_format_evaluation_info_line(report, label))


func _emit_evaluation_cleared_info(target_kind: String, target_id: int, label: String) -> void:
	if target_id == 0:
		return
	var key := "%s:%d" % [target_kind, target_id]
	if not _reported_evaluation_signatures.has(key):
		return
	_reported_evaluation_signatures.erase(key)
	_info(
		"INFO: %s timeline_ns=%d %s evaluation cleared target_id=%d" % [
			label,
			_current_virtual_timeline_ns(),
			target_kind,
			target_id,
		]
	)


func _assert_stream_report_shape(report: Dictionary, stream_id: int, device_instance_id: int, label: String) -> void:
	_require(not report.is_empty(), "%s: stream report missing" % label)
	if _done:
		return
	_require(str(report.get("parent_kind", "")) == "stream", "%s: stream report parent_kind must be stream" % label)
	_require(str(report.get("primary_function", "")) == "display_view", "%s: stream report primary_function must be display_view" % label)
	_require(int(report.get("parent_id", 0)) == stream_id, "%s: stream report parent_id mismatch" % label)
	_require(int(report.get("stream_id", 0)) == stream_id, "%s: stream report stream_id mismatch" % label)
	_require(int(report.get("device_instance_id", 0)) == device_instance_id, "%s: stream report device_instance_id mismatch" % label)
	_require(_plan_valid(report, "requested"), "%s: stream report requested plan missing" % label)
	_require(_report_has_decision(report), "%s: stream report never resolved to a decision" % label)
	var decision_candidates := _decision_candidate_postures(report)
	_require(decision_candidates.size() <= 3, "%s: stream report decision_candidate_sequence exceeds supported posture count" % label)
	if _done:
		return
	_assert_stream_decision_consistency(report, label)

func _assert_capture_report_shape(
	report: Dictionary,
	device_instance_id: int,
	label: String,
	require_decision: bool,
	require_requested: bool = true
) -> void:
	_require(not report.is_empty(), "%s: capture report missing" % label)
	if _done:
		return
	var parent_kind := str(report.get("parent_kind", ""))
	_require(
		parent_kind == "capture_priming" or parent_kind == "acquisition_session",
		"%s: capture report parent_kind invalid (%s)" % [label, parent_kind]
	)
	_require(
		str(report.get("primary_function", "")) == "capture_ready_and_materialize",
		"%s: capture report primary_function must be capture_ready_and_materialize" % label
	)
	_require(int(report.get("target_id", 0)) == device_instance_id, "%s: capture report target_id mismatch" % label)
	_require(int(report.get("device_instance_id", 0)) == device_instance_id, "%s: capture report device_instance_id mismatch" % label)
	if bool(report.get("provisional_parent", false)):
		_require(parent_kind == "capture_priming", "%s: provisional capture parent must report parent_kind=capture_priming" % label)
	if require_requested:
		_require(_plan_valid(report, "requested"), "%s: capture report requested plan missing" % label)
	if require_decision:
		_require(_report_has_decision(report), "%s: capture report never resolved to a decision" % label)
		var decision_candidates := _decision_candidate_postures(report)
		_require(decision_candidates.size() <= 3, "%s: capture report decision_candidate_sequence exceeds supported posture count" % label)
		if _done:
			return
		_assert_capture_decision_consistency(report, label)


func _assert_reports_distinct(reports: Array, label: String) -> void:
	var target_ids := {}
	var parent_ids := {}
	for report_v in reports:
		_require(typeof(report_v) == TYPE_DICTIONARY, "%s: report entry not a Dictionary" % label)
		if _done:
			return
		var report: Dictionary = report_v
		var target_id := int(report.get("target_id", 0))
		var parent_id := int(report.get("parent_id", 0))
		_require(target_id != 0, "%s: zero target_id encountered" % label)
		if _done:
			return
		_require(not target_ids.has(target_id), "%s: duplicate target_id=%d" % [label, target_id])
		target_ids[target_id] = true
		_require(parent_id != 0, "%s: zero parent_id encountered" % label)
		if _done:
			return
		_require(not parent_ids.has(parent_id), "%s: duplicate parent_id=%d" % [label, parent_id])
		parent_ids[parent_id] = true


func _assert_route_prefix_success(evidence: Dictionary, prefix: String, label: String) -> void:
	for key in evidence.keys():
		var route := str(key)
		if not route.begins_with(prefix):
			continue
		var entry_v = evidence.get(route, null)
		if typeof(entry_v) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = entry_v
		_require(int(entry.get("successes", 0)) > 0, "%s: route %s recorded no successes" % [label, route])
		if _done:
			return
		_require(int(entry.get("posture_count", 0)) >= 1, "%s: route %s recorded no posture identity" % [label, route])
		return
	_fail("%s: no evidence route found for prefix %s" % [label, prefix])


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


func _report_candidate_postures(report: Dictionary, key: String) -> Array:
	var postures: Array = []
	var candidates_v = report.get(key, [])
	if typeof(candidates_v) != TYPE_ARRAY:
		return postures
	for candidate_v in candidates_v:
		if typeof(candidate_v) != TYPE_DICTIONARY:
			continue
		var posture := str((candidate_v as Dictionary).get("posture", ""))
		if posture != "":
			postures.append(posture)
	return postures


func _decision_candidate_postures(report: Dictionary) -> Array:
	var decision_postures := _report_candidate_postures(report, "decision_candidate_sequence")
	if not decision_postures.is_empty():
		return decision_postures
	return _report_candidate_postures(report, "candidate_sequence")


func _candidate_evidence_entries(report: Dictionary) -> Array:
	var entries: Array = []
	var entries_v = report.get("candidate_evidence", [])
	if typeof(entries_v) != TYPE_ARRAY:
		return entries
	for entry_v in entries_v:
		if typeof(entry_v) == TYPE_DICTIONARY:
			entries.append(entry_v)
	return entries


func _candidate_evidence_posture(entry: Dictionary) -> String:
	var candidate_v = entry.get("candidate", {})
	if typeof(candidate_v) != TYPE_DICTIONARY:
		return ""
	return str((candidate_v as Dictionary).get("posture", ""))


func _candidate_evidence_by_posture(report: Dictionary) -> Dictionary:
	var by_posture := {}
	for entry_v in _candidate_evidence_entries(report):
		var entry: Dictionary = entry_v
		var posture := _candidate_evidence_posture(entry)
		if posture != "":
			by_posture[posture] = entry
	return by_posture


func _real_capture_parent_session_id(report: Dictionary) -> int:
	if str(report.get("parent_kind", "")) != "acquisition_session":
		return 0
	var acquisition_session_id := int(report.get("acquisition_session_id", 0))
	if acquisition_session_id != 0:
		return acquisition_session_id
	return int(report.get("parent_id", 0))


func _candidate_observation_key(entry: Dictionary) -> String:
	if not bool(entry.get("has_observed_posture", false)):
		return ""
	return "%s|%d|%d|%d|%d|%s|%d|%d" % [
		str(entry.get("observed_posture", "")),
		int(entry.get("observed_access_posture_id", 0)),
		int(entry.get("observed_stream_id", 0)),
		int(entry.get("observed_capture_id", 0)),
		int(entry.get("observed_acquisition_session_id", 0)),
		str(entry.get("observed_payload_kind", "")),
		int(entry.get("observed_image_member_index", 0)),
		int(bool(entry.get("observed_has_retained_cpu_payload", false))),
	]


func _completion_reason(report: Dictionary) -> String:
	return str(report.get("completion_reason", "none"))


func _accepted_stream_candidate_entries(report: Dictionary) -> Array:
	var accepted: Array = []
	for entry_v in _candidate_evidence_entries(report):
		var entry: Dictionary = entry_v
		if bool(entry.get("evidence_accepted", false)) and bool(entry.get("has_display_view_elapsed_ns", false)):
			accepted.append(entry)
	return accepted


func _accepted_capture_candidate_entries(report: Dictionary) -> Array:
	var accepted: Array = []
	for entry_v in _candidate_evidence_entries(report):
		var entry: Dictionary = entry_v
		if bool(entry.get("evidence_accepted", false)) and bool(entry.get("evidence_complete", false)) and bool(entry.get("has_total_elapsed_ns", false)):
			accepted.append(entry)
	return accepted


func _zero_capture_candidate_evidence(entry: Dictionary) -> bool:
	return (
		not bool(entry.get("observation_seen", false))
		and not bool(entry.get("evidence_accepted", false))
		and not bool(entry.get("evidence_complete", false))
		and not bool(entry.get("has_total_elapsed_ns", false))
		and not bool(entry.get("has_capture_ready_elapsed_ns", false))
		and not bool(entry.get("has_materialization_elapsed_ns", false))
		and int(entry.get("observed_capture_id", 0)) == 0
		and int(entry.get("observed_acquisition_session_id", 0)) == 0
	)


func _capture_report_has_clean_seed_only_epoch(
	report: Dictionary,
	prior_viable_postures: Array
) -> bool:
	if str(report.get("parent_kind", "")) != "capture_priming":
		return false
	if not bool(report.get("provisional_parent", false)):
		return false
	if int(report.get("acquisition_session_id", 0)) != 0:
		return false
	if not _plan_valid(report, "requested"):
		return false
	if _plan_valid(report, "steady") or _plan_valid(report, "decision_selected"):
		return false
	if not bool(report.get("evaluator_active", false)):
		return false
	if int(report.get("current_candidate_index", -1)) != 0:
		return false
	var requested_posture := _plan_posture(report, "requested")
	if requested_posture == "":
		return false
	if not prior_viable_postures.has(requested_posture):
		return false
	for entry_v in _candidate_evidence_entries(report):
		var entry: Dictionary = entry_v
		if not _zero_capture_candidate_evidence(entry):
			return false
	return true


func _capture_report_is_active_final_candidate_without_decision(report: Dictionary) -> bool:
	if _report_has_decision(report):
		return false
	if not bool(report.get("evaluator_active", false)):
		return false
	var viable_postures := _decision_candidate_postures(report)
	if viable_postures.size() < 2:
		return false
	var current_candidate_index := int(report.get("current_candidate_index", -1))
	return current_candidate_index == viable_postures.size() - 1


func _final_capture_candidate_rollover_preconditions(
	report: Dictionary,
	probe_info: Dictionary
) -> bool:
	var real_session_id := _real_capture_parent_session_id(report)
	if real_session_id == 0:
		return false
	if bool(report.get("provisional_parent", false)):
		return false
	if not bool(report.get("evaluator_active", false)):
		return false
	var viable_postures := _decision_candidate_postures(report)
	if viable_postures.size() < 2:
		return false
	var current_candidate_index := int(report.get("current_candidate_index", -1))
	if current_candidate_index != viable_postures.size() - 1:
		return false
	if _plan_posture(report, "requested") != str(viable_postures[current_candidate_index]):
		return false
	var probe_session_id := int(probe_info.get("acquisition_session_id", 0))
	if probe_session_id != 0 and probe_session_id != real_session_id:
		return false
	var by_posture := _candidate_evidence_by_posture(report)
	for posture_v in viable_postures:
		var posture := str(posture_v)
		if not by_posture.has(posture):
			return false
		var entry: Dictionary = by_posture[posture]
		var accepted := bool(entry.get("evidence_accepted", false))
		var complete := bool(entry.get("evidence_complete", false))
		var observed_session_id := int(entry.get("observed_acquisition_session_id", 0))
		if accepted and observed_session_id != real_session_id:
			return false
		if posture == str(viable_postures[current_candidate_index]):
			continue
		if not accepted or not complete or observed_session_id != real_session_id:
			return false
	return true


func _capture_report_is_clean_immediate_rollover(
	pre_probe_report: Dictionary,
	post_probe_report: Dictionary,
	probe_info: Dictionary,
	label: String
) -> bool:
	var viable_postures := _decision_candidate_postures(pre_probe_report)
	if not _final_capture_candidate_rollover_preconditions(pre_probe_report, probe_info):
		return false
	if not _capture_report_has_clean_seed_only_epoch(post_probe_report, viable_postures):
		return false
	var previous_real_session_id := _real_capture_parent_session_id(pre_probe_report)
	for entry_v in _candidate_evidence_entries(post_probe_report):
		var entry: Dictionary = entry_v
		_require(
			int(entry.get("observed_acquisition_session_id", 0)) != previous_real_session_id,
			"%s: provisional rollover retained evidence from retired acquisition-session %d" % [
				label,
				previous_real_session_id,
			]
		)
		if _done:
			return false
	return true


func _assert_candidate_observations_distinct(report: Dictionary, label: String) -> void:
	var seen := {}
	for entry_v in _candidate_evidence_entries(report):
		var entry: Dictionary = entry_v
		var key := _candidate_observation_key(entry)
		if key == "":
			continue
		var posture := _candidate_evidence_posture(entry)
		_require(not seen.has(key), "%s: one result/access-posture observation was attributed to incompatible candidate plans (%s vs %s)" % [label, str(seen.get(key, "")), posture])
		if _done:
			return
		seen[key] = posture


func _assert_stream_decision_consistency(report: Dictionary, label: String) -> void:
	_assert_candidate_observations_distinct(report, label)
	if _done:
		return
	var viable_postures := _decision_candidate_postures(report)
	var by_posture := _candidate_evidence_by_posture(report)
	var completion := _completion_reason(report)
	var selected_posture := _plan_posture(report, "decision_selected")
	if not bool(report.get("decision_from_evaluation", false)):
		_require(viable_postures.size() == 1, "%s: direct stream decision must expose exactly one viable candidate" % label)
		if _done:
			return
		_require(completion == "single_viable_candidate", "%s: direct stream decision must report completion_reason=single_viable_candidate" % label)
		if _done:
			return
		_require(selected_posture == str(viable_postures[0]), "%s: direct stream decision selected posture mismatch" % label)
		return
	_require(completion == "all_viable_candidates_evaluated" or completion == "live_display_demand_family_crossing", "%s: measured stream decision missing completion reason" % label)
	if _done:
		return
	for posture_v in viable_postures:
		var posture := str(posture_v)
		_require(by_posture.has(posture), "%s: stream report missing candidate evidence for posture %s" % [label, posture])
		if _done:
			return
	var accepted := _accepted_stream_candidate_entries(report)
	_require(not accepted.is_empty(), "%s: stream report selected a winner without accepted display evidence" % label)
	if _done:
		return
	_require(by_posture.has(selected_posture), "%s: stream report missing selected candidate evidence" % label)
	if _done:
		return
	var selected_entry: Dictionary = by_posture[selected_posture]
	_require(bool(selected_entry.get("evidence_accepted", false)) and bool(selected_entry.get("has_display_view_elapsed_ns", false)), "%s: selected stream candidate lacks accepted display evidence" % label)
	if _done:
		return
	var selected_elapsed := int(selected_entry.get("display_view_elapsed_ns", 0))
	for entry_v in accepted:
		var entry: Dictionary = entry_v
		_require(selected_elapsed <= int(entry.get("display_view_elapsed_ns", 0)), "%s: stream winner does not match the lowest accepted display measurement" % label)
		if _done:
			return
	var unevaluated_viable := false
	for posture_v in viable_postures:
		var posture := str(posture_v)
		var entry: Dictionary = by_posture[posture]
		if not bool(entry.get("observation_seen", false)):
			unevaluated_viable = true
			break
	if unevaluated_viable:
		_require(completion == "live_display_demand_family_crossing", "%s: partial stream comparison must report the live-display-demand family-crossing guard" % label)
	else:
		_require(completion == "all_viable_candidates_evaluated", "%s: fully observed stream comparison must report all_viable_candidates_evaluated" % label)


func _assert_capture_decision_consistency(report: Dictionary, label: String) -> void:
	_assert_candidate_observations_distinct(report, label)
	if _done:
		return
	var viable_postures := _decision_candidate_postures(report)
	var by_posture := _candidate_evidence_by_posture(report)
	var completion := _completion_reason(report)
	var selected_posture := _plan_posture(report, "decision_selected")
	if not bool(report.get("decision_from_evaluation", false)):
		_require(viable_postures.size() == 1, "%s: direct capture decision must expose exactly one viable candidate" % label)
		if _done:
			return
		_require(completion == "single_viable_candidate", "%s: direct capture decision must report completion_reason=single_viable_candidate" % label)
		if _done:
			return
		_require(selected_posture == str(viable_postures[0]), "%s: direct capture decision selected posture mismatch" % label)
		return
	_require(completion == "all_viable_candidates_evaluated", "%s: measured capture decision must report all_viable_candidates_evaluated" % label)
	if _done:
		return
	for posture_v in viable_postures:
		var posture := str(posture_v)
		_require(by_posture.has(posture), "%s: capture report missing candidate evidence for posture %s" % [label, posture])
		if _done:
			return
	var accepted := _accepted_capture_candidate_entries(report)
	_require(not accepted.is_empty(), "%s: capture report selected a winner without complete readiness-plus-materialization evidence" % label)
	if _done:
		return
	_require(by_posture.has(selected_posture), "%s: capture report missing selected candidate evidence" % label)
	if _done:
		return
	var selected_entry: Dictionary = by_posture[selected_posture]
	_require(bool(selected_entry.get("evidence_complete", false)) and bool(selected_entry.get("evidence_accepted", false)) and bool(selected_entry.get("has_capture_ready_elapsed_ns", false)) and bool(selected_entry.get("has_materialization_elapsed_ns", false)) and bool(selected_entry.get("has_total_elapsed_ns", false)), "%s: selected capture candidate lacks a complete readiness-plus-materialization score" % label)
	if _done:
		return
	var selected_total := int(selected_entry.get("total_elapsed_ns", 0))
	for entry_v in accepted:
		var entry: Dictionary = entry_v
		_require(selected_total <= int(entry.get("total_elapsed_ns", 0)), "%s: capture winner does not match the defined readiness-plus-materialization score" % label)
		if _done:
			return
		_require(int(entry.get("observed_capture_id", 0)) != 0, "%s: accepted capture candidate missing capture identity" % label)
		if _done:
			return
		_require(int(entry.get("observed_acquisition_session_id", 0)) != 0, "%s: accepted capture candidate missing acquisition-session identity" % label)
		if _done:
			return
		_require(_candidate_evidence_posture(entry) == str(entry.get("observed_posture", "")), "%s: capture evidence posture attribution mismatch" % label)


func _report_signature(report: Dictionary) -> String:
	if report.is_empty():
		return ""
	return JSON.stringify({
		"parent_kind": str(report.get("parent_kind", "")),
		"parent_id": int(report.get("parent_id", 0)),
		"target_kind": str(report.get("target_kind", "")),
		"target_id": int(report.get("target_id", 0)),
		"requested_posture": _plan_posture(report, "requested"),
		"steady_posture": _plan_posture(report, "steady"),
		"decision_posture": _plan_posture(report, "decision_selected"),
		"evaluator_active": bool(report.get("evaluator_active", false)),
		"current_candidate_index": int(report.get("current_candidate_index", -1)),
		"completion_reason": _completion_reason(report),
		"candidate_sequence": _report_candidate_postures(report, "candidate_sequence"),
		"decision_candidate_sequence": _report_candidate_postures(report, "decision_candidate_sequence"),
		"candidate_evidence": _candidate_evidence_entries(report),
	})


func _report_identity_key(report: Dictionary) -> String:
	if report.is_empty():
		return ""
	return "%s:%d" % [str(report.get("target_kind", "")), int(report.get("target_id", 0))]


func _format_evaluation_info_line(report: Dictionary, label: String) -> String:
	var timeline_ns := _current_virtual_timeline_ns()
	var target_kind := str(report.get("target_kind", ""))
	var parent_kind := str(report.get("parent_kind", ""))
	var parent_id := int(report.get("parent_id", 0))
	var target_id := int(report.get("target_id", 0))
	var requested := _plan_posture(report, "requested")
	var steady := _plan_posture(report, "steady")
	var decision := _plan_posture(report, "decision_selected")
	var candidate_postures := _decision_candidate_postures(report)
	var completion := _completion_reason(report)
	var evidence_summary := _report_evidence_summary(report)
	if _report_has_decision(report):
		return (
			"INFO: %s timeline_ns=%d %s evaluation chose %s for %s %d; requested=%s steady=%s completion=%s candidates=%s evidence=%s" % [
				label,
				timeline_ns,
				target_kind,
				decision,
				parent_kind,
				parent_id,
				requested,
				steady,
				completion,
				JSON.stringify(candidate_postures),
				evidence_summary,
			]
		)
	return (
		"INFO: %s timeline_ns=%d %s evaluation active for %s %d; target_id=%d requested=%s steady=%s candidate_index=%d completion=%s candidates=%s evidence=%s" % [
			label,
			timeline_ns,
			target_kind,
			parent_kind,
			parent_id,
			target_id,
			requested,
			steady,
			int(report.get("current_candidate_index", -1)),
			completion,
			JSON.stringify(candidate_postures),
			evidence_summary,
		]
	)


func _report_evidence_summary(report: Dictionary) -> String:
	var summaries: Array[String] = []
	for entry_v in _candidate_evidence_entries(report):
		var entry: Dictionary = entry_v
		var posture := _candidate_evidence_posture(entry)
		var observed_key := _candidate_observation_key(entry)
		var total_text := str(int(entry.get("total_elapsed_ns", 0))) if bool(entry.get("has_total_elapsed_ns", false)) else "-"
		var display_text := str(int(entry.get("display_view_elapsed_ns", 0))) if bool(entry.get("has_display_view_elapsed_ns", false)) else "-"
		summaries.append(
			"%s{obs=%d,accepted=%d,complete=%d,observed=%s,display_ns=%s,total_ns=%s,access_posture=%d}" % [
				posture,
				int(bool(entry.get("observation_seen", false))),
				int(bool(entry.get("evidence_accepted", false))),
				int(bool(entry.get("evidence_complete", false))),
				observed_key if observed_key != "" else "none",
				display_text,
				total_text,
				int(entry.get("observed_access_posture_id", 0)),
			]
		)
	if summaries.is_empty():
		return "none"
	return "; ".join(summaries)


func _evidence_has_prefix(evidence: Dictionary, prefix: String) -> bool:
	for key in evidence.keys():
		if str(key).begins_with(prefix):
			return true
	return false


func _find_device_snapshot_record_by_hardware_id(snapshot: Dictionary, hardware_id: String) -> Dictionary:
	var devices: Array = snapshot.get("devices", [])
	for dv in devices:
		if typeof(dv) != TYPE_DICTIONARY:
			continue
		var device_record: Dictionary = dv
		if str(device_record.get("hardware_id", "")) == hardware_id:
			return device_record
	return {}


func _find_device_record_by_instance_id(snapshot: Dictionary, device_instance_id: int) -> Dictionary:
	var devices: Array = snapshot.get("devices", [])
	for dv in devices:
		if typeof(dv) != TYPE_DICTIONARY:
			continue
		var device_record: Dictionary = dv
		if int(device_record.get("instance_id", 0)) == device_instance_id:
			return device_record
	return {}


func _get_device_snapshot_record(device_instance_id: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
		return {}
	return _find_device_record_by_instance_id(snapshot, device_instance_id)


func _find_stream_snapshot_record(
	snapshot: Dictionary,
	device_instance_id: int,
	intent: String,
	required_mode: String = ""
) -> Dictionary:
	var streams: Array = snapshot.get("streams", [])
	for sv in streams:
		if typeof(sv) != TYPE_DICTIONARY:
			continue
		var stream_record: Dictionary = sv
		if int(stream_record.get("device_instance_id", 0)) != device_instance_id:
			continue
		if str(stream_record.get("intent", "")) != intent:
			continue
		if required_mode != "" and str(stream_record.get("mode", "")) != required_mode:
			continue
		return stream_record
	return {}


func _stream_id_from_snapshot(
	snapshot: Dictionary,
	hardware_id: String,
	intent: String,
	required_mode: String = ""
) -> int:
	var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
	if device_record.is_empty():
		return 0
	var device_instance_id := int(device_record.get("instance_id", 0))
	if device_instance_id == 0:
		return 0
	var stream_record := _find_stream_snapshot_record(snapshot, device_instance_id, intent, required_mode)
	if stream_record.is_empty():
		return 0
	return int(stream_record.get("stream_id", 0))


func _extract_snapshot_still_profile(device_snapshot: Dictionary) -> Dictionary:
	var capture_profile_v = device_snapshot.get("capture_profile", null)
	if typeof(capture_profile_v) != TYPE_DICTIONARY:
		return {}
	var capture_profile: Dictionary = capture_profile_v
	var still_v = capture_profile.get("still", null)
	if typeof(still_v) != TYPE_DICTIONARY:
		return {}
	return still_v


func _extract_still_image_bundle_members(still_profile: Dictionary) -> Array:
	var bundle_v = still_profile.get("still_image_bundle", null)
	if typeof(bundle_v) != TYPE_DICTIONARY:
		return []
	var bundle: Dictionary = bundle_v
	var members_v = bundle.get("members", null)
	if typeof(members_v) != TYPE_ARRAY:
		return []
	return members_v


func _get_acquisition_session_snapshot_record(device_instance_id: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
		return {}
	var sessions: Array = snapshot.get("acquisition_sessions", [])
	for sv in sessions:
		if typeof(sv) != TYPE_DICTIONARY:
			continue
		var session_record: Dictionary = sv
		if int(session_record.get("device_instance_id", 0)) == device_instance_id:
			return session_record
	return {}


func _find_acquisition_session_id_for_capture(
	device_instance_id: int,
	capture_id: int
) -> int:
	if capture_id == 0:
		return 0
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
		return 0
	var sessions: Array = snapshot.get("acquisition_sessions", [])
	for sv in sessions:
		if typeof(sv) != TYPE_DICTIONARY:
			continue
		var session_record: Dictionary = sv
		if int(session_record.get("device_instance_id", 0)) != device_instance_id:
			continue
		if int(session_record.get("last_capture_id", 0)) != capture_id:
			continue
		return int(session_record.get("acquisition_session_id", 0))
	return 0


func _capture_progress_from_record(record: Dictionary, source: String) -> Dictionary:
	if record.is_empty():
		return {"available": false, "source": source}
	if not record.has("captures_completed") and not record.has("captures_failed") and not record.has("last_capture_id"):
		return {"available": false, "source": source}
	return {
		"available": true,
		"source": source,
		"captures_triggered": int(record.get("captures_triggered", 0)),
		"captures_completed": int(record.get("captures_completed", 0)),
		"captures_failed": int(record.get("captures_failed", 0)),
		"last_capture_id": int(record.get("last_capture_id", 0)),
		"active_capture_id": int(record.get("active_capture_id", 0)),
	}


func _get_capture_progress_snapshot(device_instance_id: int) -> Dictionary:
	var device_progress := _capture_progress_from_record(_get_device_snapshot_record(device_instance_id), "device")
	if bool(device_progress.get("available", false)):
		return device_progress
	var session_progress := _capture_progress_from_record(
		_get_acquisition_session_snapshot_record(device_instance_id),
		"acquisition_session"
	)
	if bool(session_progress.get("available", false)):
		return session_progress
	return {"available": false, "source": "none"}


func _stop_and_verify_reset(label: String) -> void:
	_display_refs.clear()
	CamBANGServer.stop()
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() != null:
			continue
		var evidence := _get_result_access_timing_evidence()
		if not evidence.is_empty():
			continue
		var reports := _get_backing_plan_evaluation_reports()
		if not reports.is_empty():
			continue
		_reported_evaluation_signatures.clear()
		_info("INFO: %s backing-plan reports and timing evidence cleared after stop()" % label)
		_step_ok("%s stop() cleared snapshot, evidence, and reports" % label)
		return
	_fail("%s: stop() did not clear snapshot/evidence/reports in time" % label)


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
	push_error("step %d FAIL: %s" % [_step, detail])
	printerr("FAIL: %s" % detail)
	print("FAIL: %s" % detail)
	_cleanup_and_quit(1)


func _step_ok(message: String) -> void:
	_step += 1
	print("step %d OK: %s" % [_step, message])


func _info(message: String) -> void:
	print(message)


func _cleanup_and_quit(exit_code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	CamBANGServer.stop()
	get_tree().quit(exit_code)
