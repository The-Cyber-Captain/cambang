extends Node

## Scene 68: backing-plan evaluation and timing evidence verify
##
## Purpose:
## - verify backing-plan evaluation lifecycle and reporting
## - verify internally calibrated stream and capture result-access timing evidence
## - first prove that materialization-route evidence can appear without any
##   public to_image()/to_image_member() calls from the scene itself
## - prove backing-plan evaluation state and timing evidence stay correctly scoped to the
##   owning stream/capture parent context in both single-device and dual-device
##   structures
## - prove that result_access_timing_evidence and backing_plan_evaluation_reports
##   are cleared by stop() and re-established after restart
##
## Scope guardrail:
## - this is a verification scene / harness scene, not a scenario semantic owner
## - it stays at the retained-result operation seam and must not turn into a
##   rendering/teaching scene like Scene 70
## 
## Design choice:
## - use minimal external synthetic scenarios only to realize focused
##   single-device and dual-device live structures; scenario semantics remain
##   provider-owned
## - the access-only session must not seed materialisation evidence through
##   public to_image()/to_image_member() calls
## - do not rely on UI rendering to seed evidence
## - intentionally verify the default still-capture profile path rather than
##   submitting an explicit still profile; this keeps Scene 68 grounded on the
##   default case and helps guard against evidence polling accidentally becoming
##   coupled to manual still-profile submission

const SCENE_LABEL := "inner_evidence_reset_verify"
const SINGLE_SCENARIO_PATH := "res://scenarios/68_inner_evidence_reset_live.json"
const DUAL_SCENARIO_PATH := "res://scenarios/68_inner_evidence_reset_dual_live.json"
const SINGLE_TARGET_SPECS := [
	{"tag": "cam0", "hardware_id": "synthetic:0", "intent": "PREVIEW", "mode": "FLOWING"},
]
const DUAL_TARGET_SPECS := [
	{"tag": "cam0", "hardware_id": "synthetic:0", "intent": "PREVIEW", "mode": "FLOWING"},
	{"tag": "cam1", "hardware_id": "synthetic:1", "intent": "VIEWFINDER", "mode": "FLOWING"},
]

const TOTAL_TIMEOUT_MS := 18000
const BASELINE_TIMEOUT_FRAMES := 900
const ID_TIMEOUT_FRAMES := 900
const STREAM_RESULT_TIMEOUT_FRAMES := 900
const DEFAULT_STILL_PROFILE_TIMEOUT_FRAMES := 900
const CAPTURE_RESULT_TIMEOUT_FRAMES := 900
const STOP_TIMEOUT_FRAMES := 240
const ERR_UNAVAILABLE := 2

var _step := 0
var _done := false
var _quit_requested := false
var _start_ms := 0
var _baseline_gens: Array[int] = []
var _display_refs: Array = []



func _synthetic_producer_output_form_setting() -> String:
	return str(ProjectSettings.get_setting("cambang/maintainer/synthetic_producer_output_form", "runtime_default"))


func _synthetic_stream_capability_downgrade_setting() -> String:
	return str(ProjectSettings.get_setting("cambang/maintainer/synthetic_stream_capability_downgrades", ""))


func _synthetic_capture_capability_downgrade_setting() -> String:
	return str(ProjectSettings.get_setting("cambang/maintainer/synthetic_capture_capability_downgrades", ""))


func _synthetic_producer_output_form_cmdline_selection() -> String:
	const PREFIX := "--cambang-synth-producer-output-form="
	var found := ""
	for arg in OS.get_cmdline_user_args():
		var text := str(arg)
		if not text.begins_with(PREFIX):
			continue
		var value := text.substr(PREFIX.length())
		if found != "":
			return "<duplicate>"
		found = value
	return found


func _synthetic_stream_capability_downgrade_cmdline_selection() -> String:
	const PREFIX := "--cambang-synth-stream-capability-downgrades="
	var found := ""
	for arg in OS.get_cmdline_user_args():
		var text := str(arg)
		if not text.begins_with(PREFIX):
			continue
		var value := text.substr(PREFIX.length())
		if found != "":
			return "<duplicate>"
		found = value
	return found


func _synthetic_capture_capability_downgrade_cmdline_selection() -> String:
	const PREFIX := "--cambang-synth-capture-capability-downgrades="
	var found := ""
	for arg in OS.get_cmdline_user_args():
		var text := str(arg)
		if not text.begins_with(PREFIX):
			continue
		var value := text.substr(PREFIX.length())
		if found != "":
			return "<duplicate>"
		found = value
	return found


func _synthetic_producer_output_form_effective_selection() -> String:
	var cmdline_selection := _synthetic_producer_output_form_cmdline_selection()
	if cmdline_selection != "":
		return cmdline_selection
	return _synthetic_producer_output_form_setting()


func _synthetic_stream_capability_downgrade_effective_selection() -> String:
	var cmdline_selection := _synthetic_stream_capability_downgrade_cmdline_selection()
	if cmdline_selection != "":
		return cmdline_selection
	return _synthetic_stream_capability_downgrade_setting()


func _synthetic_capture_capability_downgrade_effective_selection() -> String:
	var cmdline_selection := _synthetic_capture_capability_downgrade_cmdline_selection()
	if cmdline_selection != "":
		return cmdline_selection
	return _synthetic_capture_capability_downgrade_setting()


func _log_maintainer_config_probe() -> void:
	print("Synthetic producer output-form stored project setting: %s" % _synthetic_producer_output_form_setting())
	print("Synthetic producer output-form effective runtime selection: %s" % _synthetic_producer_output_form_effective_selection())
	print("Synthetic stream capability downgrades stored project setting: %s" % _synthetic_stream_capability_downgrade_setting())
	print("Synthetic stream capability downgrades effective runtime selection: %s" % _synthetic_stream_capability_downgrade_effective_selection())
	print("Synthetic capture capability downgrades stored project setting: %s" % _synthetic_capture_capability_downgrade_setting())
	print("Synthetic capture capability downgrades effective runtime selection: %s" % _synthetic_capture_capability_downgrade_effective_selection())


func _ready() -> void:
	_log_maintainer_config_probe()
	_start_ms = Time.get_ticks_msec()
	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)
	call_deferred("_run")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if version == 0 and topology_version == 0 and not _baseline_gens.has(gen):
		_baseline_gens.append(gen)


func _run() -> void:
	print("RUN: %s" % SCENE_LABEL)
	await _run_impl()


func _run_impl() -> void:
	var session_0 := await _run_single_device_access_only_pass(-1, "single_device_access_only")
	if _done:
		return
	_print_evidence("single_device_access_only_post_access", session_0.get("evidence", {}))

	await _stop_and_verify_reset()
	if _done:
		return

	var gen_0 := int(session_0.get("gen", 0))
	var session_1 := await _run_dual_device_materialized_pass(gen_0, "dual_device_materialized")
	if _done:
		return
	_print_evidence("dual_device_materialized_post_access", session_1.get("evidence", {}))

	var gen_1 := int(session_1.get("gen", 0))
	_require(gen_1 > gen_0, "generation did not advance across restart (%d -> %d)" % [gen_0, gen_1])
	if _done:
		return

	await _stop_and_verify_reset()
	if _done:
		return

	_step_ok("backing-plan evaluation lifecycle and timing evidence verified")
	_cleanup_and_quit(0)


func _run_single_device_access_only_pass(previous_gen: int, label: String) -> Dictionary:
	_display_refs.clear()
	_bootstrap_runtime_and_stage_external_scenario(
		label,
		SINGLE_SCENARIO_PATH,
		"68_inner_evidence_reset_live"
	)
	if _done:
		return {}

	var gen := await _wait_for_new_baseline(previous_gen, label)
	if _done:
		return {}

	var target_hardware_id := str(SINGLE_TARGET_SPECS[0].get("hardware_id", ""))
	var target_intent := str(SINGLE_TARGET_SPECS[0].get("intent", ""))
	var initial_open_snapshot := await _wait_for_authored_structure_state(
		gen,
		"%s initial open/no-authored-stream plateau" % label,
		[target_hardware_id],
		[],
		[],
		[{"hardware_id": target_hardware_id, "intent": target_intent}]
	)
	if _done:
		return {}
	_step_ok("%s observed initial open/no-authored-stream plateau" % label)
	await _seed_capture_access_only_evidence_from_snapshot(
		initial_open_snapshot,
		target_hardware_id,
		"%s_initial_open_capture_access_only" % label
	)
	if _done:
		return {}
	await _emit_capture_decision_from_snapshot(
		initial_open_snapshot,
		target_hardware_id,
		"capture_ready_and_materialize",
		"%s_initial_open_capture_default" % label,
		label
	)
	if _done:
		return {}

	var first_stream_snapshot := await _wait_for_authored_structure_state(
		gen,
		"%s first authored stream-context plateau" % label,
		[target_hardware_id],
		[],
		[{"hardware_id": target_hardware_id, "intent": target_intent, "mode": "FLOWING"}],
		[]
	)
	if _done:
		return {}
	_step_ok("%s observed first authored stream-context plateau" % label)
	var initial_stream_id := _stream_id_from_snapshot(first_stream_snapshot, target_hardware_id, target_intent, "FLOWING")
	_require(initial_stream_id != 0, "%s: initial authored stream id missing" % label)
	if _done:
		return {}

	var post_teardown_observation := await _wait_for_single_device_post_teardown_transition(
		gen,
		"%s post-teardown single-device transition" % label,
		target_hardware_id,
		target_intent,
		initial_stream_id
	)
	if _done:
		return {}
	var final_phase_snapshot: Dictionary = {}
	var post_teardown_variant_index := int(post_teardown_observation.get("variant_index", 0))
	if post_teardown_variant_index == 0:
		_step_ok("%s observed post-teardown absent plateau" % label)
		var final_phase_observation := await _wait_for_any_authored_structure_state(
			gen,
			"%s final single-device phase" % label,
			[
				{
					"tag": "final pre-stream plateau",
					"required_device_hardware_ids": [target_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [],
					"absent_stream_specs": [{"hardware_id": target_hardware_id, "intent": target_intent}],
				},
				{
					"tag": "final flowing stream-context (coalesced pre-stream plateau)",
					"required_device_hardware_ids": [target_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [{"hardware_id": target_hardware_id, "intent": target_intent, "mode": "FLOWING"}],
					"absent_stream_specs": [],
				},
			]
		)
		if _done:
			return {}
		final_phase_snapshot = final_phase_observation.get("snapshot", {})
		_step_ok("%s observed %s" % [label, str(final_phase_observation.get("variant_tag", "final single-device phase"))])
	else:
		final_phase_snapshot = post_teardown_observation.get("snapshot", {})
		_step_ok("%s observed %s" % [label, str(post_teardown_observation.get("variant_tag", "post-teardown single-device transition"))])
	await _emit_capture_decision_from_snapshot(
		final_phase_snapshot,
		target_hardware_id,
		"capture_ready_and_materialize",
		"%s_final_capture_default" % label,
		label
	)
	if _done:
		return {}

	var contexts := await _wait_for_runtime_contexts(gen, SINGLE_TARGET_SPECS, label)
	if _done:
		return {}
	var context: Dictionary = contexts[0]
	_step_ok("%s target latched (%s device_instance_id=%d stream_id=%d intent=%s)" % [
		label,
		str(context.get("tag", "")),
		int(context.get("device_instance_id", 0)),
		int(context.get("stream_id", 0)),
		str(context.get("intent", "")),
	])

	var device_instance_id := int(context.get("device_instance_id", 0))
	var stream_id := int(context.get("stream_id", 0))
	var target_label := "%s_%s" % [label, str(context.get("tag", "target"))]
	_require(stream_id != initial_stream_id, "%s: teardown did not produce a new authored stream identity" % label)
	if _done:
		return {}

	_require(device_instance_id != 0, "%s: snapshot device_instance_id is 0" % label)
	if _done:
		return {}
	var device = await _wait_for_device_handle(device_instance_id, label)
	if _done:
		return {}
	_print_device_handle_diag(label, "post_get_device", device, context)

	var stream_result = await _wait_for_stream_result(stream_id, label)
	if _done:
		return {}
	var initial_stream_report := _get_stream_backing_plan_evaluation_report(stream_id)
	_print_backing_plan_evaluation_report("%s_stream_initial" % label, initial_stream_report)
	var stream_plan_shape := _classify_and_assert_stream_backing_plan_shape(initial_stream_report, target_label)
	if _done:
		return {}
	var stream_candidate_source_report := initial_stream_report

	await _exercise_stream_access_without_public_to_image(stream_id, stream_result, label)
	if _done:
		return {}
	_step_ok("%s stream access-only evidence seeded" % label)

	var stream_report_after_stream_access := initial_stream_report
	if stream_plan_shape == "multi":
		var stream_observation := await _observe_multi_candidate_backing_plan_until_steady(
			stream_id,
			initial_stream_report,
			target_label,
			"stream"
		)
		if _done:
			return {}
		stream_candidate_source_report = stream_observation.get(
			"candidate_source_report",
			initial_stream_report
		)
		stream_report_after_stream_access = stream_observation.get(
			"steady_report",
			initial_stream_report
		)
		_print_backing_plan_evaluation_report("%s_stream_after_multi_observation" % label, stream_report_after_stream_access)

	var steady_stream_report := await _resolve_stream_steady_backing_plan(
		stream_id,
		stream_report_after_stream_access,
		stream_plan_shape,
		target_label,
		"stream"
	)
	if _done:
		return {}
	_print_backing_plan_evaluation_report("%s_stream_steady" % label, steady_stream_report)
	await _assert_backing_plan_steady_reused(stream_id, steady_stream_report, target_label, "stream")
	if _done:
		return {}

	await _require_default_still_profile_visible(device_instance_id, label)
	if _done:
		return {}
	_step_ok("%s default still profile snapshot-visible" % label)
	var capture_report := await _wait_for_capture_backing_plan_evaluation_ready(
		device_instance_id,
		target_label,
		str(context.get("tag", "capture"))
	)
	_print_backing_plan_evaluation_report("%s_capture_parent_ready" % label, capture_report)
	if _done:
		return {}
	_step_ok("%s capture parent evaluation remained capture-scoped" % label)

	var capture_result = await _trigger_and_wait_capture(device, context, label)
	if _done:
		return {}
	var access_probe := _exercise_capture_access_without_public_to_image(capture_result, label)
	if _done:
		return {}
	_step_ok("%s capture access-only evidence seeded" % label)

	var route_expectations := _build_expected_route_counts([
		{
			"context": context,
			"stream_report": steady_stream_report,
			"capture_report": capture_report,
		}
	])
	var evidence := await _wait_for_expected_evidence_routes(label, route_expectations)
	if _done:
		return {}
	_assert_expected_evidence_family(evidence, target_label, steady_stream_report, capture_report)
	if _done:
		return {}
	_assert_expected_route_posture_counts(evidence, route_expectations, target_label)
	if _done:
		return {}
	_assert_access_only_probe_contract(label, access_probe)
	if _done:
		return {}
	_print_backing_plan_decision_summary(
		"%s_stream_final" % label,
		stream_candidate_source_report,
		steady_stream_report,
		evidence,
		"stream"
	)
	_print_backing_plan_decision_summary(
		"%s_capture_reused_final" % label,
		stream_candidate_source_report,
		capture_report,
		evidence,
		"capture"
	)
	_step_ok("%s access-only evidence verified" % label)

	return {
		"gen": gen,
		"hardware_id": target_hardware_id,
		"device_instance_id": device_instance_id,
		"stream_id": stream_id,
		"evidence": evidence,
	}


func _run_dual_device_materialized_pass(previous_gen: int, label: String) -> Dictionary:
	_display_refs.clear()
	_bootstrap_runtime_and_stage_external_scenario(
		label,
		DUAL_SCENARIO_PATH,
		"68_inner_evidence_reset_dual_live"
	)
	if _done:
		return {}

	var gen := await _wait_for_new_baseline(previous_gen, label)
	if _done:
		return {}

	var cam0_hardware_id := str(DUAL_TARGET_SPECS[0].get("hardware_id", ""))
	var cam0_intent := str(DUAL_TARGET_SPECS[0].get("intent", ""))
	var cam1_hardware_id := str(DUAL_TARGET_SPECS[1].get("hardware_id", ""))
	var cam1_intent := str(DUAL_TARGET_SPECS[1].get("intent", ""))
	var initial_cam0_open_snapshot := await _wait_for_authored_structure_state(
		gen,
		"%s initial cam0 open/no-authored-stream plateau" % label,
		[cam0_hardware_id],
		[],
		[],
		[{"hardware_id": cam0_hardware_id, "intent": cam0_intent}]
	)
	if _done:
		return {}
	_step_ok("%s observed initial cam0 open/no-authored-stream plateau" % label)
	await _seed_capture_access_only_evidence_from_snapshot(
		initial_cam0_open_snapshot,
		cam0_hardware_id,
		"%s_initial_cam0_capture_access_only" % label
	)
	if _done:
		return {}
	await _emit_capture_decision_from_snapshot(
		initial_cam0_open_snapshot,
		cam0_hardware_id,
		"capture_ready_and_materialize",
		"%s_initial_cam0_capture_default" % label,
		label
	)
	if _done:
		return {}

	var first_cam0_stream_snapshot := await _wait_for_authored_structure_state(
		gen,
		"%s first cam0 stream-context plateau" % label,
		[cam0_hardware_id],
		[],
		[{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"}],
		[]
	)
	if _done:
		return {}
	_step_ok("%s observed first cam0 stream-context plateau" % label)
	var initial_cam0_stream_id := _stream_id_from_snapshot(first_cam0_stream_snapshot, cam0_hardware_id, cam0_intent, "FLOWING")
	_require(initial_cam0_stream_id != 0, "%s: initial cam0 authored stream id missing" % label)
	if _done:
		return {}

	var post_teardown_dual_observation := await _wait_for_dual_device_post_teardown_transition(
		gen,
		"%s post-teardown dual-device transition" % label,
		cam0_hardware_id,
		cam0_intent,
		cam1_hardware_id,
		cam1_intent,
		initial_cam0_stream_id
	)
	if _done:
		return {}
	var dual_phase_snapshot: Dictionary = {}
	var dual_phase_variant_index := 0
	var post_teardown_dual_variant_index := int(post_teardown_dual_observation.get("variant_index", 0))
	if post_teardown_dual_variant_index == 0:
		_step_ok("%s observed post-teardown cam0 absent plateau" % label)
		var dual_phase_observation := await _wait_for_any_authored_structure_state(
			gen,
			"%s dual-device second-open phase" % label,
			[
				{
					"tag": "dual-device no-authored-stream plateau",
					"required_device_hardware_ids": [cam0_hardware_id, cam1_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [],
					"absent_stream_specs": [
						{"hardware_id": cam0_hardware_id, "intent": cam0_intent},
						{"hardware_id": cam1_hardware_id, "intent": cam1_intent},
					],
				},
				{
					"tag": "mixed dual-device plateau (coalesced no-stream plateau)",
					"required_device_hardware_ids": [cam0_hardware_id, cam1_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"}],
					"absent_stream_specs": [{"hardware_id": cam1_hardware_id, "intent": cam1_intent}],
				},
				{
					"tag": "full dual-device stream-context plateau (coalesced no-stream and mixed plateaus)",
					"required_device_hardware_ids": [cam0_hardware_id, cam1_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [
						{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"},
						{"hardware_id": cam1_hardware_id, "intent": cam1_intent, "mode": "FLOWING"},
					],
					"absent_stream_specs": [],
				},
			]
		)
		if _done:
			return {}
		dual_phase_snapshot = dual_phase_observation.get("snapshot", {})
		dual_phase_variant_index = int(dual_phase_observation.get("variant_index", 0))
		_step_ok("%s observed %s" % [label, str(dual_phase_observation.get("variant_tag", "dual-device second-open phase"))])
	else:
		dual_phase_snapshot = post_teardown_dual_observation.get("snapshot", {})
		dual_phase_variant_index = post_teardown_dual_variant_index - 1
		_step_ok("%s observed %s" % [label, str(post_teardown_dual_observation.get("variant_tag", "post-teardown dual-device transition"))])
	await _seed_capture_access_only_evidence_from_snapshot(
		dual_phase_snapshot,
		cam0_hardware_id,
		"%s_dual_second_open_cam0_capture_access_only" % label
	)
	if _done:
		return {}
	await _seed_capture_access_only_evidence_from_snapshot(
		dual_phase_snapshot,
		cam1_hardware_id,
		"%s_dual_second_open_cam1_capture_access_only" % label
	)
	if _done:
		return {}
	await _emit_capture_decision_from_snapshot(
		dual_phase_snapshot,
		cam0_hardware_id,
		"capture_ready_and_materialize",
		"%s_dual_second_open_cam0_capture_default" % label,
		label
	)
	if _done:
		return {}
	await _emit_capture_decision_from_snapshot(
		dual_phase_snapshot,
		cam1_hardware_id,
		"capture_ready_and_materialize",
		"%s_dual_second_open_cam1_capture_default" % label,
		label
	)
	if _done:
		return {}

	var mixed_dual_snapshot: Dictionary = dual_phase_snapshot
	if dual_phase_variant_index == 0:
		var mixed_dual_observation := await _wait_for_any_authored_structure_state(
			gen,
			"%s mixed dual-device follow-up phase" % label,
			[
				{
					"tag": "mixed dual-device plateau",
					"required_device_hardware_ids": [cam0_hardware_id, cam1_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"}],
					"absent_stream_specs": [{"hardware_id": cam1_hardware_id, "intent": cam1_intent}],
				},
				{
					"tag": "full dual-device stream-context plateau (coalesced mixed plateau)",
					"required_device_hardware_ids": [cam0_hardware_id, cam1_hardware_id],
					"absent_device_hardware_ids": [],
					"required_stream_specs": [
						{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"},
						{"hardware_id": cam1_hardware_id, "intent": cam1_intent, "mode": "FLOWING"},
					],
					"absent_stream_specs": [],
				},
			]
		)
		if _done:
			return {}
		mixed_dual_snapshot = mixed_dual_observation.get("snapshot", {})
		_step_ok("%s observed %s" % [label, str(mixed_dual_observation.get("variant_tag", "mixed dual-device follow-up phase"))])
	await _emit_capture_decision_from_snapshot(
		mixed_dual_snapshot,
		cam1_hardware_id,
		"capture_ready_and_materialize",
		"%s_cam1_with_cam0_stream_capture_default" % label,
		label
	)
	if _done:
		return {}

	var contexts := await _wait_for_runtime_contexts(gen, DUAL_TARGET_SPECS, label)
	if _done:
		return {}
	_assert_context_identity_scope(contexts, label)
	if _done:
		return {}

	var records: Array = []
	for context_v in contexts:
		var context: Dictionary = context_v
		var target_label := "%s_%s" % [label, str(context.get("tag", "target"))]
		_step_ok("%s target latched (%s device_instance_id=%d stream_id=%d intent=%s)" % [
			label,
			str(context.get("tag", "")),
			int(context.get("device_instance_id", 0)),
			int(context.get("stream_id", 0)),
			str(context.get("intent", "")),
		])
		var device_instance_id := int(context.get("device_instance_id", 0))
		var stream_id := int(context.get("stream_id", 0))
		if str(context.get("tag", "")) == "cam0":
			_require(stream_id != initial_cam0_stream_id, "%s: cam0 teardown did not produce a new authored stream identity" % label)
			if _done:
				return {}
		_require(device_instance_id != 0 and stream_id != 0, "%s: target ids missing for %s" % [label, str(context.get("tag", ""))])
		if _done:
			return {}
		var device = await _wait_for_device_handle(device_instance_id, "%s_%s" % [label, str(context.get("tag", ""))])
		if _done:
			return {}
		_print_device_handle_diag(label, "post_get_device", device, context)

		var stream_result = await _wait_for_stream_result(stream_id, target_label)
		if _done:
			return {}
		var initial_stream_report := _get_stream_backing_plan_evaluation_report(stream_id)
		_print_backing_plan_evaluation_report("%s_stream_initial" % target_label, initial_stream_report)
		var stream_plan_shape := _classify_and_assert_stream_backing_plan_shape(initial_stream_report, target_label)
		if _done:
			return {}
		var stream_candidate_source_report := initial_stream_report
		await _exercise_stream_access(stream_id, stream_result, target_label)
		if _done:
			return {}
		_step_ok("%s stream evidence seeded" % target_label)

		var observed_stream_report := initial_stream_report
		if stream_plan_shape == "multi":
			var stream_observation := await _observe_multi_candidate_backing_plan_until_steady(
				stream_id,
				initial_stream_report,
				target_label,
				"stream"
			)
			if _done:
				return {}
			stream_candidate_source_report = stream_observation.get(
				"candidate_source_report",
				initial_stream_report
			)
			observed_stream_report = stream_observation.get(
				"steady_report",
				initial_stream_report
			)
			_print_backing_plan_evaluation_report("%s_stream_after_multi_observation" % target_label, observed_stream_report)
		var steady_stream_report := await _resolve_stream_steady_backing_plan(
			stream_id,
			observed_stream_report,
			stream_plan_shape,
			target_label,
			"stream"
		)
		if _done:
			return {}
		_print_backing_plan_evaluation_report("%s_stream_steady" % target_label, steady_stream_report)
		await _assert_backing_plan_steady_reused(stream_id, steady_stream_report, target_label, "stream")
		if _done:
			return {}

		await _require_default_still_profile_visible(device_instance_id, target_label)
		if _done:
			return {}
		_step_ok("%s default still profile snapshot-visible" % target_label)
		var capture_report := await _wait_for_capture_backing_plan_evaluation_ready(
			device_instance_id,
			target_label,
			str(context.get("tag", "capture"))
		)
		if _done:
			return {}
		_print_backing_plan_evaluation_report("%s_capture_parent_ready" % target_label, capture_report)
		_step_ok("%s capture parent evaluation remained capture-scoped" % target_label)

		var capture_result = await _trigger_and_wait_capture(device, context, target_label)
		if _done:
			return {}
		_exercise_capture_access(capture_result, target_label)
		if _done:
			return {}
		_step_ok("%s capture evidence seeded" % target_label)

		records.append({
			"context": context,
			"stream_candidate_source_report": stream_candidate_source_report,
			"stream_report": steady_stream_report,
			"capture_report": capture_report,
		})

	var evidence := await _wait_for_expected_evidence_routes(
		label,
		_build_expected_route_counts(records)
	)
	if _done:
		return {}
	var route_expectations := _build_expected_route_counts(records)
	_assert_backing_plan_evaluation_reports_distinct(records, label)
	if _done:
		return {}
	for record_v in records:
		var record: Dictionary = record_v
		var context: Dictionary = record.get("context", {})
		var target_label := "%s_%s" % [label, str(context.get("tag", "target"))]
		var stream_candidate_source_report: Dictionary = record.get("stream_candidate_source_report", {})
		var stream_report: Dictionary = record.get("stream_report", {})
		var capture_report: Dictionary = record.get("capture_report", {})
		_assert_expected_evidence_family(evidence, target_label, stream_report, capture_report)
		if _done:
			return {}
		_print_backing_plan_decision_summary(
			"%s_stream_final" % target_label,
			stream_candidate_source_report,
			stream_report,
			evidence,
			"stream"
		)
		_print_backing_plan_decision_summary(
			"%s_capture_reused_final" % target_label,
			stream_candidate_source_report,
			capture_report,
			evidence,
			"capture"
		)
	_assert_expected_route_posture_counts(evidence, route_expectations, label)
	if _done:
		return {}
	_step_ok("%s dual-device backing-plan evaluation and evidence scoping verified" % label)

	return {
		"gen": gen,
		"contexts": contexts,
		"evidence": evidence,
	}


func _bootstrap_runtime_and_stage_external_scenario(label: String, scenario_path: String, scenario_name: String) -> void:
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
	_step_ok("%s scenario started" % label)


func _wait_for_new_baseline(previous_gen: int, label: String) -> int:
	for _i in range(BASELINE_TIMEOUT_FRAMES):
		if _timed_out():
			return -1
		await get_tree().process_frame
		for gen in _baseline_gens:
			if gen > previous_gen:
				return gen
	_fail("%s: timed out waiting for new baseline publish after gen=%d" % [label, previous_gen])
	return -1


func _wait_for_authored_structure_state(
	expected_gen: int,
	label: String,
	required_device_hardware_ids: Array,
	absent_device_hardware_ids: Array,
	required_stream_specs: Array,
	absent_stream_specs: Array
) -> Dictionary:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
			continue
		if int(snapshot.get("gen", -1)) != expected_gen:
			continue
		if _snapshot_matches_authored_structure_state(
			snapshot,
			required_device_hardware_ids,
			absent_device_hardware_ids,
			required_stream_specs,
			absent_stream_specs
		):
			return snapshot
	_fail("%s: timed out waiting for authored structure state" % label)
	return {}


func _wait_for_any_authored_structure_state(expected_gen: int, label: String, variants: Array) -> Dictionary:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
			continue
		if int(snapshot.get("gen", -1)) != expected_gen:
			continue
		for variant_index in range(variants.size()):
			var variant_v = variants[variant_index]
			if typeof(variant_v) != TYPE_DICTIONARY:
				continue
			var variant: Dictionary = variant_v
			if _snapshot_matches_authored_structure_state(
				snapshot,
				variant.get("required_device_hardware_ids", []),
				variant.get("absent_device_hardware_ids", []),
				variant.get("required_stream_specs", []),
				variant.get("absent_stream_specs", [])
			):
				return {
					"snapshot": snapshot,
					"variant_index": variant_index,
					"variant_tag": str(variant.get("tag", "authored structure state")),
				}
	_fail("%s: timed out waiting for authored structure state" % label)
	return {}


func _wait_for_single_device_post_teardown_transition(
	expected_gen: int,
	label: String,
	hardware_id: String,
	intent: String,
	initial_stream_id: int
) -> Dictionary:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
			continue
		if int(snapshot.get("gen", -1)) != expected_gen:
			continue
		if _snapshot_matches_authored_structure_state(snapshot, [], [hardware_id], [], []):
			return {
				"snapshot": snapshot,
				"variant_index": 0,
				"variant_tag": "post-teardown absent plateau",
			}
		if _snapshot_matches_authored_structure_state(
			snapshot,
			[hardware_id],
			[],
			[],
			[{"hardware_id": hardware_id, "intent": intent}]
		):
			return {
				"snapshot": snapshot,
				"variant_index": 1,
				"variant_tag": "final pre-stream plateau (coalesced absent plateau)",
			}
		var current_stream_id := _stream_id_from_snapshot(snapshot, hardware_id, intent, "FLOWING")
		if current_stream_id != 0 and current_stream_id != initial_stream_id:
			return {
				"snapshot": snapshot,
				"variant_index": 2,
				"variant_tag": "final flowing stream-context (coalesced absent and pre-stream plateaus)",
			}
	_fail("%s: timed out waiting for authored structure state" % label)
	return {}


func _wait_for_dual_device_post_teardown_transition(
	expected_gen: int,
	label: String,
	cam0_hardware_id: String,
	cam0_intent: String,
	cam1_hardware_id: String,
	cam1_intent: String,
	initial_cam0_stream_id: int
) -> Dictionary:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
			continue
		if int(snapshot.get("gen", -1)) != expected_gen:
			continue
		if _snapshot_matches_authored_structure_state(snapshot, [], [cam0_hardware_id], [], []):
			return {
				"snapshot": snapshot,
				"variant_index": 0,
				"variant_tag": "post-teardown cam0 absent plateau",
			}
		if _snapshot_matches_authored_structure_state(
			snapshot,
			[cam0_hardware_id, cam1_hardware_id],
			[],
			[],
			[
				{"hardware_id": cam0_hardware_id, "intent": cam0_intent},
				{"hardware_id": cam1_hardware_id, "intent": cam1_intent},
			]
		):
			return {
				"snapshot": snapshot,
				"variant_index": 1,
				"variant_tag": "dual-device no-authored-stream plateau (coalesced cam0-absent plateau)",
			}
		var cam0_stream_id := _stream_id_from_snapshot(snapshot, cam0_hardware_id, cam0_intent, "FLOWING")
		if cam0_stream_id == 0 or cam0_stream_id == initial_cam0_stream_id:
			continue
		if _snapshot_matches_authored_structure_state(
			snapshot,
			[cam0_hardware_id, cam1_hardware_id],
			[],
			[{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"}],
			[{"hardware_id": cam1_hardware_id, "intent": cam1_intent}]
		):
			return {
				"snapshot": snapshot,
				"variant_index": 2,
				"variant_tag": "mixed dual-device plateau (coalesced cam0-absent and no-stream plateaus)",
			}
		if _snapshot_matches_authored_structure_state(
			snapshot,
			[cam0_hardware_id, cam1_hardware_id],
			[],
			[
				{"hardware_id": cam0_hardware_id, "intent": cam0_intent, "mode": "FLOWING"},
				{"hardware_id": cam1_hardware_id, "intent": cam1_intent, "mode": "FLOWING"},
			],
			[]
		):
			return {
				"snapshot": snapshot,
				"variant_index": 3,
				"variant_tag": "full dual-device stream-context plateau (coalesced cam0-absent, no-stream, and mixed plateaus)",
			}
	_fail("%s: timed out waiting for authored structure state" % label)
	return {}


func _snapshot_matches_authored_structure_state(
	snapshot: Dictionary,
	required_device_hardware_ids: Array,
	absent_device_hardware_ids: Array,
	required_stream_specs: Array,
	absent_stream_specs: Array
) -> bool:
	for hardware_id_v in required_device_hardware_ids:
		var hardware_id := str(hardware_id_v)
		if _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id).is_empty():
			return false

	for hardware_id_v in absent_device_hardware_ids:
		var hardware_id := str(hardware_id_v)
		if not _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id).is_empty():
			return false

	for spec_v in required_stream_specs:
		if typeof(spec_v) != TYPE_DICTIONARY:
			return false
		var spec: Dictionary = spec_v
		var hardware_id := str(spec.get("hardware_id", ""))
		var intent := str(spec.get("intent", ""))
		var mode := str(spec.get("mode", ""))
		var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
		if device_record.is_empty():
			return false
		var device_instance_id := int(device_record.get("instance_id", 0))
		if device_instance_id == 0:
			return false
		if _find_stream_snapshot_record(snapshot, device_instance_id, intent, mode).is_empty():
			return false

	for spec_v in absent_stream_specs:
		if typeof(spec_v) != TYPE_DICTIONARY:
			return false
		var spec: Dictionary = spec_v
		var hardware_id := str(spec.get("hardware_id", ""))
		var intent := str(spec.get("intent", ""))
		var mode := str(spec.get("mode", ""))
		var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
		if device_record.is_empty():
			continue
		var device_instance_id := int(device_record.get("instance_id", 0))
		if device_instance_id == 0:
			return false
		if not _find_stream_snapshot_record(snapshot, device_instance_id, intent, mode).is_empty():
			return false

	return true


func _wait_for_runtime_contexts(expected_gen: int, target_specs: Array, label: String) -> Array:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return []
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
			continue
		if int(snapshot.get("gen", -1)) != expected_gen:
			continue

		var contexts: Array = []
		var all_ready := true
		for spec_v in target_specs:
			if typeof(spec_v) != TYPE_DICTIONARY:
				all_ready = false
				break
			var spec: Dictionary = spec_v
			var hardware_id := str(spec.get("hardware_id", ""))
			var intent := str(spec.get("intent", ""))
			var mode := str(spec.get("mode", ""))
			var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
			if device_record.is_empty():
				all_ready = false
				break
			var device_instance_id := int(device_record.get("instance_id", 0))
			var stream_record := _find_stream_snapshot_record(snapshot, device_instance_id, intent, mode)
			if stream_record.is_empty():
				all_ready = false
				break
			var stream_id := int(stream_record.get("stream_id", 0))
			if device_instance_id == 0 or stream_id == 0:
				all_ready = false
				break
			contexts.append({
				"tag": str(spec.get("tag", hardware_id)),
				"hardware_id": hardware_id,
				"device_instance_id": device_instance_id,
				"stream_id": stream_id,
				"intent": intent,
			})
		if all_ready and contexts.size() == target_specs.size():
			return contexts
	_fail("%s: timed out waiting for authored device/stream identities in gen=%d snapshot" % [label, expected_gen])
	return []


func _wait_for_stream_result(stream_id: int, label: String):
	for _i in range(STREAM_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if result != null:
			return result
	_fail("%s: timed out waiting for stream result for stream_id=%d" % [label, stream_id])
	return null


func _capability_is_supported(capability: int, unsupported_capability: int) -> bool:
	return capability != unsupported_capability


func _wait_for_stream_result_with_to_image_support(stream_id: int, label: String):
	for _i in range(STREAM_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var latest_stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if latest_stream_result == null:
			continue
		var to_image_capability := int(latest_stream_result.can_to_image())
		if _capability_is_supported(to_image_capability, int(CamBANGStreamResult.CAPABILITY_UNSUPPORTED)):
			return latest_stream_result
	_fail("%s: timed out waiting for stream result with supported to_image() capability for stream_id=%d" % [label, stream_id])
	return null


func _exercise_stream_access(stream_id: int, stream_result, label: String) -> void:
	_require(stream_result != null, "%s: stream_result is null" % label)
	if _done:
		return
	var supported_stream_result = stream_result
	var to_image_capability := int(supported_stream_result.can_to_image())
	if not _capability_is_supported(to_image_capability, int(CamBANGStreamResult.CAPABILITY_UNSUPPORTED)):
		supported_stream_result = await _wait_for_stream_result_with_to_image_support(stream_id, label)
		if _done:
			return
		_require(supported_stream_result != null, "%s: stream_result with supported to_image() capability did not arrive" % label)
		if _done:
			return
		to_image_capability = int(supported_stream_result.can_to_image())
	_require(_capability_is_supported(to_image_capability, int(CamBANGStreamResult.CAPABILITY_UNSUPPORTED)), "%s: stream_result.can_to_image() remained unsupported" % label)
	if _done:
		return
	var image = supported_stream_result.to_image()
	_require(image != null, "%s: stream_result.to_image() returned null" % label)
	if _done:
		return
	await _establish_stream_display_view_demand(stream_id, supported_stream_result, label)


func _exercise_stream_access_without_public_to_image(stream_id: int, stream_result, label: String) -> void:
	_require(stream_result != null, "%s: stream_result is null" % label)
	if _done:
		return
	# Access-only session guardrail: do not call to_image(), but do exercise the
	# public capability queries so the inner polling/calibration layer can launch
	# from the realized posture without a user materialization call.
	stream_result.can_to_image()
	await _establish_stream_display_view_demand(stream_id, stream_result, label)


func _establish_stream_display_view_demand(stream_id: int, stream_result, label: String) -> void:
	# Mimic the display-oriented sequencing used by the existing result-retrieval
	# harnesses: ask for display_view to establish display demand, then allow a
	# fresh retained frame to arrive before requiring display-view truth. Under
	# the default display_demanded GPU update policy, a GPU-capable first frame
	# may not yet advertise display_view until after that demand has been seen.
	var display_view = stream_result.get_display_view()
	if display_view is Texture2D:
		_display_refs.append(display_view)
		return

	var display_ready := await _wait_for_stream_display_view_after_demand(stream_id, label)
	if _done:
		return
	var latest_display_view = display_ready.get("display_view", null)
	_require(latest_display_view is Texture2D, "%s: stream_result.get_display_view() did not become available after demand" % label)
	if _done:
		return
	_display_refs.append(latest_display_view)


func _wait_for_stream_display_view_after_demand(stream_id: int, label: String) -> Dictionary:
	for _i in range(STREAM_RESULT_TIMEOUT_FRAMES):
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

	_fail("%s: timed out waiting for fresh stream display_view after establishing demand" % label)
	return {}
func _print_device_handle_diag(phase_label: String, where: String, device, ids: Dictionary) -> void:
	if device == null:
		print("DIAG: %s %s device=null ids=%s" % [phase_label, where, JSON.stringify(ids)])
		return

	var snapshot_device_instance_id := int(ids.get("device_instance_id", 0))
	var hardware_id := str(ids.get("hardware_id", ""))
	var handle_instance_id := int(device.get_instance_id())

	print(
		"DIAG: %s %s hardware_id=%s snapshot_device_instance_id=%d handle_instance_id=%d" % [
			phase_label,
			where,
			hardware_id,
			snapshot_device_instance_id,
			handle_instance_id
		]
	)


func _wait_for_device_handle(device_instance_id: int, label: String):
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var device = CamBANGServer.get_device(device_instance_id)
		if device != null:
			return device
	_fail("%s: get_device(%d) did not become available" % [label, device_instance_id])
	return null


func _require_default_still_profile_visible(device_instance_id: int, label: String) -> void:
	for _i in range(DEFAULT_STILL_PROFILE_TIMEOUT_FRAMES):
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


func _trigger_and_wait_capture(device, ids: Dictionary, label: String):
	_print_device_handle_diag(label, "pre_trigger_capture", device, ids)
	var device_instance_id := int(ids.get("device_instance_id", 0))
	# In the default open/no-stream phases, Synthetic does not realize the
	# AcquisitionSession seam until stream creation or capture admission. That
	# means pre-trigger capture progress is not guaranteed to exist here.
	var baseline_progress := _get_capture_progress_snapshot(device_instance_id)
	var baseline_failed := int(baseline_progress.get("captures_failed", 0))
	var baseline_completed := int(baseline_progress.get("captures_completed", 0))

	var trigger_err := int(device.trigger_capture())
	if trigger_err == ERR_UNAVAILABLE:
		_fail("%s: device.trigger_capture() explicitly unavailable before result publication" % label)
		return null
	_require(trigger_err == OK, "%s: device.trigger_capture() failed err=%d" % [label, trigger_err])
	if _done:
		return null

	for _i in range(CAPTURE_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var progress := _get_capture_progress_snapshot(device_instance_id)
		if bool(progress.get("available", false)) and int(progress.get("captures_failed", 0)) > baseline_failed:
			_fail("%s: capture failed after trigger (baseline=%s progress=%s)" % [label, str(baseline_progress), str(progress)])
			return null
		var capture_result = device.get_result()
		if capture_result != null and int(capture_result.get_image_count()) > 0:
			return capture_result
		if not bool(progress.get("available", false)):
			continue
		if int(progress.get("captures_completed", 0)) <= baseline_completed:
			continue
	_fail("%s: timed out waiting for completed capture result" % label)
	return null


func _exercise_capture_access(capture_result, label: String) -> void:
	_require(capture_result != null, "%s: capture_result is null" % label)
	if _done:
		return
	var to_image_capability := int(capture_result.can_to_image())
	_require(_capability_is_supported(to_image_capability, int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED)), "%s: capture_result.can_to_image() returned unsupported" % label)
	if _done:
		return
	var image = capture_result.to_image()
	_require(image != null, "%s: capture_result.to_image() returned null" % label)
	if _done:
		return
	var member0_capability := int(capture_result.can_to_image_member(0))
	_require(_capability_is_supported(member0_capability, int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED)), "%s: capture_result.can_to_image_member(0) returned unsupported" % label)
	if _done:
		return
	var member0 = capture_result.to_image_member(0)
	_require(member0 != null, "%s: capture_result.to_image_member(0) returned null" % label)
	if _done:
		return
	var image_count := int(capture_result.get_image_count())
	if image_count > 1:
		var member1_capability := int(capture_result.can_to_image_member(1))
		if _capability_is_supported(member1_capability, int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED)):
			var member1 = capture_result.to_image_member(1)
			_require(member1 != null, "%s: capture_result.to_image_member(1) returned null" % label)


func _exercise_capture_access_without_public_to_image(capture_result, label: String) -> Dictionary:
	_require(capture_result != null, "%s: capture_result is null" % label)
	if _done:
		return {}
	# Access-only session guardrail: do not materialize images, but do exercise
	# the public capture capability/access probes that should be sufficient for
	# the inner polling/calibration layer to populate route evidence.
	var to_image_capability = int(capture_result.can_to_image())
	var member0_capability = int(capture_result.can_to_image_member(0))
	var encoded_capability = null
	if capture_result.has_method("can_get_encoded_bytes"):
		encoded_capability = int(capture_result.can_get_encoded_bytes())
	var image_count := int(capture_result.get_image_count())
	var member1_capability = null
	if image_count > 1:
		member1_capability = int(capture_result.can_to_image_member(1))
	return {
		"to_image_capability": to_image_capability,
		"member0_capability": member0_capability,
		"member1_capability": member1_capability,
		"encoded_capability": encoded_capability,
	}


func _wait_for_access_only_measurement_evidence(label: String) -> Dictionary:
	for _i in range(CAPTURE_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var evidence := _get_result_access_timing_evidence()
		if evidence.is_empty():
			continue
		if not _evidence_has_prefix(evidence, "stream_display_view."):
			continue
		if not _evidence_has_prefix(evidence, "stream_to_image."):
			continue
		if not _evidence_has_prefix(evidence, "capture_to_image."):
			continue
		return evidence
	_fail("%s: timed out waiting for access-only measurement evidence to appear without public to_image calls" % label)
	return {}


func _wait_for_evidence_route(route: String, label: String) -> Dictionary:
	for _i in range(CAPTURE_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var evidence := _get_result_access_timing_evidence()
		if evidence.is_empty():
			continue
		var entry_v = evidence.get(route, null)
		if typeof(entry_v) != TYPE_DICTIONARY:
			continue
		return evidence
	_fail("%s: timed out waiting for evidence route %s" % [label, route])
	return {}


func _wait_for_evidence_route_prefix(prefix: String, label: String) -> Dictionary:
	for _i in range(CAPTURE_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var evidence := _get_result_access_timing_evidence()
		if evidence.is_empty():
			continue
		for key in evidence.keys():
			var route := str(key)
			if not route.begins_with(prefix):
				continue
			var entry_v = evidence.get(route, null)
			if typeof(entry_v) != TYPE_DICTIONARY:
				continue
			return {
				"route": route,
				"evidence": evidence,
			}
	_fail("%s: timed out waiting for evidence route prefix %s" % [label, prefix])
	return {}


func _evidence_has_prefix(evidence: Dictionary, prefix: String) -> bool:
	for key in evidence.keys():
		if str(key).begins_with(prefix):
			return true
	return false


func _assert_access_only_probe_contract(label: String, access_probe: Dictionary) -> void:
	# This session must never call public to_image* methods itself. The evidence
	# dictionary is verified separately by _assert_expected_evidence_family(); this
	# helper only checks the non-materialising probe contract used by the access-only path.
	var encoded_capability = access_probe.get("encoded_capability", null)
	if encoded_capability != null:
		_require(encoded_capability == int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED), "%s: encoded bytes unexpectedly supported by this verifier path" % label)


func _stop_and_verify_reset() -> void:
	_display_refs.clear()
	CamBANGServer.stop()
	for _i in range(STOP_TIMEOUT_FRAMES):
		if _timed_out():
			return
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() == null:
			var evidence := _get_result_access_timing_evidence()
			_print_evidence("post_stop", evidence)
			_require(evidence.is_empty(), "post-stop result_access_timing_evidence was not cleared by stop()")
			if _done:
				return
			var evaluation_reports := _get_backing_plan_evaluation_reports()
			print("BACKING_PLAN_EVAL: post_stop %s" % JSON.stringify(evaluation_reports))
			_require(evaluation_reports.is_empty(), "post-stop backing_plan_evaluation_reports was not cleared by stop()")
			if _done:
				return
			_step_ok("stop/reset verified")
			return
	_fail("stop/reset: get_state_snapshot() never returned NIL after stop()")


func _get_device_snapshot_record(device_instance_id: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return {}
	var devices: Array = snapshot.get("devices", [])
	for dv in devices:
		if typeof(dv) != TYPE_DICTIONARY:
			continue
		var d: Dictionary = dv
		if int(d.get("instance_id", 0)) == device_instance_id:
			return d
	return {}


func _find_device_snapshot_record_by_hardware_id(snapshot: Dictionary, hardware_id: String) -> Dictionary:
	var devices: Array = snapshot.get("devices", [])
	for dv in devices:
		if typeof(dv) != TYPE_DICTIONARY:
			continue
		var device_record: Dictionary = dv
		if str(device_record.get("hardware_id", "")) == hardware_id:
			return device_record
	return {}


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
	if snapshot == null:
		return {}
	var sessions: Array = snapshot.get("acquisition_sessions", [])
	for sv in sessions:
		if typeof(sv) != TYPE_DICTIONARY:
			continue
		var s: Dictionary = sv
		if int(s.get("device_instance_id", 0)) == device_instance_id:
			return s
	return {}


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
	var session_progress := _capture_progress_from_record(_get_acquisition_session_snapshot_record(device_instance_id), "acquisition_session")
	if bool(session_progress.get("available", false)):
		return session_progress
	return {"available": false, "source": "none"}



func _get_backing_plan_evaluation_reports() -> Array:
	var metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(metrics) != TYPE_DICTIONARY:
		return []
	var reports = metrics.get("backing_plan_evaluation_reports", [])
	if typeof(reports) != TYPE_ARRAY:
		return []
	return reports


func _get_stream_backing_plan_evaluation_report(stream_id: int) -> Dictionary:
	for report_v in _get_backing_plan_evaluation_reports():
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if str(report.get("target_kind", "")) == "stream" and int(report.get("target_id", 0)) == stream_id:
			return report
	return {}


func _wait_for_stream_backing_plan_evaluation_with_primary_function(
	stream_id: int,
	label: String,
	target_label: String,
	expected_primary_function: String
) -> Dictionary:
	var last_primary_function := ""
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_stream_backing_plan_evaluation_report(stream_id)
		if report.is_empty():
			continue
		last_primary_function = str(report.get("primary_function", ""))
		if last_primary_function != expected_primary_function:
			continue
		return report
	_fail("%s: timed out waiting for %s stream backing-plan evaluation report with primary_function %s (last_primary_function=%s)" % [label, target_label, expected_primary_function, last_primary_function])
	return {}


func _get_capture_backing_plan_evaluation_report(device_instance_id: int) -> Dictionary:
	for report_v in _get_backing_plan_evaluation_reports():
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if str(report.get("target_kind", "")) == "capture" and int(report.get("target_id", 0)) == device_instance_id:
			return report
	return {}


func _wait_for_capture_backing_plan_evaluation_report(device_instance_id: int, label: String) -> Dictionary:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if not report.is_empty():
			return report
	_fail("%s: timed out waiting for capture backing-plan evaluation report for device_instance_id=%d" % [label, device_instance_id])
	return {}


func _wait_for_capture_backing_plan_evaluation_with_primary_function(
	device_instance_id: int,
	label: String,
	target_label: String,
	expected_primary_function: String
) -> Dictionary:
	var last_primary_function := ""
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if report.is_empty():
			continue
		last_primary_function = str(report.get("primary_function", ""))
		if last_primary_function != expected_primary_function:
			continue
		return report
	_fail("%s: timed out waiting for %s capture backing-plan evaluation report with primary_function %s (last_primary_function=%s)" % [label, target_label, expected_primary_function, last_primary_function])
	return {}


func _wait_for_capture_backing_plan_evaluation_ready(
	device_instance_id: int,
	label: String,
	target_label: String
) -> Dictionary:
	if _done:
		return {}
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_capture_backing_plan_evaluation_report(device_instance_id)
		if report.is_empty():
			continue
		_assert_backing_plan_primary_function(
			report,
			label,
			"%s capture" % target_label,
			"capture_ready_and_materialize"
		)
		if _done:
			return {}
		if not _backing_plan_valid(report, "requested"):
			continue
		return report
	_fail("%s: timed out waiting for %s capture backing-plan requested posture to become available" % [label, target_label])
	return {}


func _backing_plan_posture(report: Dictionary, key: String) -> String:
	var plan_v = report.get(key, {})
	if typeof(plan_v) != TYPE_DICTIONARY:
		return ""
	return str((plan_v as Dictionary).get("posture", ""))


func _backing_plan_valid(report: Dictionary, key: String) -> bool:
	var plan_v = report.get(key, {})
	if typeof(plan_v) != TYPE_DICTIONARY:
		return false
	return bool((plan_v as Dictionary).get("valid", false))


func _print_backing_plan_evaluation_report(tag: String, report: Dictionary) -> void:
	print("BACKING_PLAN_EVAL: %s %s" % [tag, JSON.stringify(report)])


func _backing_plan_candidate_postures(report: Dictionary) -> Array:
	var postures: Array = []
	var candidates_v = report.get("candidate_sequence", [])
	if typeof(candidates_v) != TYPE_ARRAY:
		return postures
	for candidate_v in candidates_v:
		if typeof(candidate_v) == TYPE_DICTIONARY:
			var posture := str((candidate_v as Dictionary).get("posture", ""))
			if posture != "":
				postures.append(posture)
		else:
			var candidate_posture := str(candidate_v)
			if candidate_posture != "":
				postures.append(candidate_posture)
	return postures


func _backing_plan_decision_candidate_postures(report: Dictionary) -> Array:
	var postures: Array = []
	var candidates_v = report.get("decision_candidate_sequence", [])
	if typeof(candidates_v) != TYPE_ARRAY:
		return postures
	for candidate_v in candidates_v:
		if typeof(candidate_v) == TYPE_DICTIONARY:
			var posture := str((candidate_v as Dictionary).get("posture", ""))
			if posture != "":
				postures.append(posture)
		else:
			var candidate_posture := str(candidate_v)
			if candidate_posture != "":
				postures.append(candidate_posture)
	return postures


func _backing_plan_decision_from_evaluation(report: Dictionary) -> bool:
	return bool(report.get("decision_from_evaluation", false))


func _backing_plan_has_made_decision(report: Dictionary) -> bool:
	if report.is_empty():
		return false
	if _backing_plan_valid(report, "decision_selected"):
		return true
	if bool(report.get("evaluator_active", false)):
		return false
	if not _backing_plan_valid(report, "requested") or not _backing_plan_valid(report, "steady"):
		return false
	return _backing_plan_posture(report, "requested") == _backing_plan_posture(report, "steady")


func _backing_plan_selection_posture(report: Dictionary) -> String:
	var steady_posture := _backing_plan_posture(report, "steady")
	if _backing_plan_valid(report, "steady") and steady_posture != "":
		return steady_posture
	if _backing_plan_valid(report, "requested"):
		return _backing_plan_posture(report, "requested")
	return ""


func _to_image_route_for_posture(posture: String) -> String:
	match posture:
		"CPU-primary":
			return "cpu_packed"
		"GPU-primary, no CPU sidecar":
			return "gpu_primary_no_cpu_sidecar_materializer"
		"GPU-primary, with CPU sidecar":
			return "gpu_primary_cpu_sidecar"
		_:
			return ""


func _auxiliary_gpu_materializer_route_for_posture(posture: String) -> String:
	match posture:
		"GPU-primary, with CPU sidecar":
			return "gpu_primary_cpu_sidecar_materializer"
		_:
			return ""


func _estimated_ns_per_byte(ns_value: int, bytes_value: int) -> float:
	if ns_value <= 0 or bytes_value <= 0:
		return 0.0
	return float(ns_value) / float(bytes_value)


func _summarize_to_image_evidence_entry(posture: String, route: String, entry: Dictionary) -> Dictionary:
	var last_bytes := int(entry.get("last_bytes", 0))
	var fresh_successes := int(entry.get("fresh_result_successes", 0))
	var repeat_successes := int(entry.get("repeat_successes", 0))
	var fresh_total_ns := int(entry.get("fresh_result_total_ns", 0))
	var repeat_total_ns := int(entry.get("repeat_total_ns", 0))
	var first_success_ns := int(entry.get("first_success_ns", 0))
	var fresh_avg_ns := 0
	if fresh_successes > 0:
		fresh_avg_ns = int(fresh_total_ns / fresh_successes)
	var repeat_avg_ns := 0
	if repeat_successes > 0:
		repeat_avg_ns = int(repeat_total_ns / repeat_successes)
	return {
		"posture": posture,
		"route": route,
		"first_success_ns": first_success_ns,
		"first_success_ns_per_byte_est": _estimated_ns_per_byte(first_success_ns, last_bytes),
		"fresh_result_successes": fresh_successes,
		"fresh_result_avg_ns": fresh_avg_ns,
		"fresh_result_avg_ns_per_byte_est": _estimated_ns_per_byte(fresh_avg_ns, last_bytes),
		"repeat_successes": repeat_successes,
		"repeat_result_avg_ns": repeat_avg_ns,
		"repeat_result_avg_ns_per_byte_est": _estimated_ns_per_byte(repeat_avg_ns, last_bytes),
		"last_bytes": last_bytes,
		"last_reported_capability": int(entry.get("last_reported_capability", 0)),
		"posture_count": int(entry.get("posture_count", 0)),
	}


func _selected_to_image_evidence(
	posture: String,
	evidence: Dictionary,
	evidence_prefix: String
) -> Dictionary:
	var route := _to_image_route_for_posture(posture)
	if route == "":
		return {}
	var key := "%s.%s" % [evidence_prefix, route]
	if not evidence.has(key):
		return {}
	var entry_v = evidence.get(key, {})
	if typeof(entry_v) != TYPE_DICTIONARY:
		return {}
	return _summarize_to_image_evidence_entry(posture, route, entry_v as Dictionary)


func _selected_auxiliary_gpu_materializer_evidence(
	posture: String,
	evidence: Dictionary,
	evidence_prefix: String
) -> Dictionary:
	var route := _auxiliary_gpu_materializer_route_for_posture(posture)
	if route == "":
		return {}
	var key := "%s.%s" % [evidence_prefix, route]
	if not evidence.has(key):
		return {}
	var entry_v = evidence.get(key, {})
	if typeof(entry_v) != TYPE_DICTIONARY:
		return {}
	return _summarize_to_image_evidence_entry(posture, route, entry_v as Dictionary)


func _compared_to_image_evidence(candidate_postures: Array, evidence: Dictionary, evidence_prefix: String) -> Array:
	var compared: Array = []
	for posture_v in candidate_postures:
		var posture := str(posture_v)
		var route := _to_image_route_for_posture(str(posture))
		if route == "":
			continue
		var key := "%s.%s" % [evidence_prefix, route]
		if not evidence.has(key):
			continue
		var entry_v = evidence.get(key, {})
		if typeof(entry_v) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = entry_v
		compared.append(_summarize_to_image_evidence_entry(str(posture), route, entry))
	return compared


func _compared_auxiliary_gpu_materializer_evidence(
	candidate_postures: Array,
	evidence: Dictionary,
	evidence_prefix: String
) -> Array:
	var compared: Array = []
	for posture_v in candidate_postures:
		var posture := str(posture_v)
		var route := _auxiliary_gpu_materializer_route_for_posture(posture)
		if route == "":
			continue
		var key := "%s.%s" % [evidence_prefix, route]
		if not evidence.has(key):
			continue
		var entry_v = evidence.get(key, {})
		if typeof(entry_v) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = entry_v
		compared.append(_summarize_to_image_evidence_entry(posture, route, entry))
	return compared


func _print_backing_plan_decision_summary(
	context_tag: String,
	candidate_source_report: Dictionary,
	selected_report: Dictionary,
	evidence: Dictionary,
	evidence_scope: String
) -> void:
	if selected_report.is_empty():
		return
	if not _backing_plan_has_made_decision(selected_report):
		return
	var selection := _backing_plan_selection_posture(selected_report)
	if selection == "":
		return
	var candidates := _backing_plan_candidate_postures(candidate_source_report)
	if candidates.is_empty():
		candidates = _backing_plan_decision_candidate_postures(selected_report)
	if candidates.is_empty():
		candidates = _backing_plan_candidate_postures(selected_report)
	if candidates.is_empty():
		candidates = [selection]
	var mode := "single_viable_selection"
	if _backing_plan_decision_from_evaluation(selected_report) or candidates.size() > 1:
		mode = "multiple_viable_selection"

	var fields: Array = [
		"context=%s" % context_tag,
		"target_kind=%s" % str(selected_report.get("target_kind", "")),
		"target_id=%d" % int(selected_report.get("target_id", 0)),
		"primary_function=%s" % str(selected_report.get("primary_function", "")),
		"mode=%s" % mode,
		"selection=%s" % JSON.stringify(selection),
		"candidates=%s" % JSON.stringify(candidates),
	]
	var evidence_prefix := "%s_to_image" % evidence_scope
	var measurement_fields := [
		"first_success_ns",
		"first_success_ns_per_byte_est",
		"fresh_result_avg_ns",
		"fresh_result_avg_ns_per_byte_est",
		"repeat_result_avg_ns",
		"repeat_result_avg_ns_per_byte_est",
		"last_bytes",
		"last_reported_capability",
		"posture_count",
	]
	var selected_to_image := _selected_to_image_evidence(selection, evidence, evidence_prefix)
	if not selected_to_image.is_empty():
		fields.append("selected_to_image_measurement_fields=%s" % JSON.stringify(measurement_fields))
		fields.append("selected_to_image=%s" % JSON.stringify(selected_to_image))
	var selected_auxiliary_gpu_materializer := _selected_auxiliary_gpu_materializer_evidence(
		selection,
		evidence,
		evidence_prefix
	)
	if not selected_auxiliary_gpu_materializer.is_empty():
		fields.append("selected_auxiliary_gpu_materializer_measurement_fields=%s" % JSON.stringify(measurement_fields))
		fields.append("selected_auxiliary_gpu_materializer=%s" % JSON.stringify(selected_auxiliary_gpu_materializer))
	if mode == "multiple_viable_selection":
		fields.append("to_image_measurement_fields=%s" % JSON.stringify(measurement_fields))
		fields.append("compared_to_image=%s" % JSON.stringify(_compared_to_image_evidence(candidates, evidence, evidence_prefix)))
		var auxiliary_gpu_materializer := _compared_auxiliary_gpu_materializer_evidence(
			candidates,
			evidence,
			evidence_prefix
		)
		if not auxiliary_gpu_materializer.is_empty():
			fields.append("auxiliary_gpu_materializer_measurement_fields=%s" % JSON.stringify(measurement_fields))
			fields.append("auxiliary_gpu_materializer=%s" % JSON.stringify(auxiliary_gpu_materializer))
	fields.append("stored_selection=%s" % _synthetic_producer_output_form_setting())
	fields.append("effective_selection=%s" % _synthetic_producer_output_form_effective_selection())
	fields.append("stream_downgrades=%s" % _synthetic_stream_capability_downgrade_effective_selection())
	fields.append("capture_downgrades=%s" % _synthetic_capture_capability_downgrade_effective_selection())
	print("BACKING_PLAN_DECISION: %s" % " ".join(fields))


func _emit_capture_decision_from_snapshot(
	snapshot: Dictionary,
	hardware_id: String,
	expected_primary_function: String,
	context_tag: String,
	label: String
) -> void:
	if snapshot.is_empty():
		return
	var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
	_require(not device_record.is_empty(), "%s: device snapshot missing for hardware_id=%s" % [label, hardware_id])
	if _done:
		return
	var device_instance_id := int(device_record.get("instance_id", 0))
	_require(device_instance_id != 0, "%s: device instance id missing for hardware_id=%s" % [label, hardware_id])
	if _done:
		return
	var report := await _wait_for_capture_backing_plan_evaluation_with_primary_function(
		device_instance_id,
		label,
		context_tag,
		expected_primary_function
	)
	if _done:
		return
	if _backing_plan_has_made_decision(report):
		var evidence := _get_result_access_timing_evidence()
		_print_backing_plan_decision_summary(
			context_tag,
			report,
			report,
			evidence,
			"capture"
		)


func _emit_stream_decision_from_snapshot(
	snapshot: Dictionary,
	hardware_id: String,
	stream_intent: String,
	context_tag: String,
	label: String
) -> void:
	if snapshot.is_empty():
		return
	var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
	_require(not device_record.is_empty(), "%s: device snapshot missing for hardware_id=%s" % [label, hardware_id])
	if _done:
		return
	var device_instance_id := int(device_record.get("instance_id", 0))
	_require(device_instance_id != 0, "%s: device instance id missing for hardware_id=%s" % [label, hardware_id])
	if _done:
		return
	var stream_record := _find_stream_snapshot_record(snapshot, device_instance_id, stream_intent)
	_require(not stream_record.is_empty(), "%s: stream snapshot missing for hardware_id=%s intent=%s" % [label, hardware_id, stream_intent])
	if _done:
		return
	var stream_id := int(stream_record.get("stream_id", 0))
	_require(stream_id != 0, "%s: stream id missing for hardware_id=%s intent=%s" % [label, hardware_id, stream_intent])
	if _done:
		return
	var report := await _wait_for_stream_backing_plan_evaluation_with_primary_function(
		stream_id,
		label,
		context_tag,
		"display_view"
	)
	if _done:
		return
	if _backing_plan_has_made_decision(report):
		var evidence := _get_result_access_timing_evidence()
		_print_backing_plan_decision_summary(
			context_tag,
			report,
			report,
			evidence,
			"stream"
		)


func _seed_capture_access_only_evidence_from_snapshot(
	snapshot: Dictionary,
	hardware_id: String,
	label: String
) -> void:
	if snapshot.is_empty():
		return
	var device_record := _find_device_snapshot_record_by_hardware_id(snapshot, hardware_id)
	_require(not device_record.is_empty(), "%s: device snapshot missing for hardware_id=%s" % [label, hardware_id])
	if _done:
		return
	var device_instance_id := int(device_record.get("instance_id", 0))
	_require(device_instance_id != 0, "%s: device instance id missing for hardware_id=%s" % [label, hardware_id])
	if _done:
		return
	var device = await _wait_for_device_handle(device_instance_id, label)
	if _done:
		return
	await _require_default_still_profile_visible(device_instance_id, label)
	if _done:
		return
	var capture_result = await _trigger_and_wait_capture(
		device,
		{"device_instance_id": device_instance_id},
		label
	)
	if _done:
		return
	var access_probe := _exercise_capture_access_without_public_to_image(capture_result, label)
	if _done:
		return
	_assert_access_only_probe_contract(label, access_probe)
	if _done:
		return
	var capture_route_match := await _wait_for_evidence_route_prefix("capture_to_image.", label)
	if _done:
		return
	_require(not capture_route_match.is_empty(), "%s: capture_to_image.* evidence missing after access-only capture" % label)


func _assert_backing_plan_primary_function(report: Dictionary, label: String, target_label: String, expected_primary_function: String) -> void:
	_require(not report.is_empty(), "%s: %s backing-plan evaluation report missing" % [label, target_label])
	if _done:
		return
	_require(str(report.get("primary_function", "")) == expected_primary_function, "%s: %s backing-plan primary_function expected %s got %s" % [label, target_label, expected_primary_function, str(report.get("primary_function", ""))])


func _classify_and_assert_stream_backing_plan_shape(report: Dictionary, label: String) -> String:
	_assert_backing_plan_primary_function(report, label, "stream", "display_view")
	if _done:
		return ""
	_require(_backing_plan_valid(report, "requested"), "%s: stream backing-plan requested posture missing" % label)
	if _done:
		return ""

	var candidates: Array = report.get("candidate_sequence", [])
	var evaluator_active := bool(report.get("evaluator_active", false))
	var requested_posture := _backing_plan_posture(report, "requested")
	var steady_valid := _backing_plan_valid(report, "steady")
	var steady_posture := _backing_plan_posture(report, "steady")

	if candidates.size() <= 1 and not evaluator_active:
		_require(steady_valid, "%s: stream single-viable selection must expose steady posture" % label)
		if _done:
			return ""
		_require(requested_posture == steady_posture, "%s: stream single-viable selection requested posture must equal steady posture" % label)
		if _done:
			return ""
		_step_ok("%s stream backing-plan evaluation classified single-viable selection" % label)
		return "single"

	_require(candidates.size() > 1, "%s: stream multi-candidate evaluator must expose candidate sequence" % label)
	if _done:
		return ""
	_require(evaluator_active, "%s: stream multi-candidate evaluator must be active while steady posture is unset" % label)
	if _done:
		return ""
	_require(not steady_valid, "%s: stream multi-candidate evaluator should not expose steady posture before settlement" % label)
	if _done:
		return ""
	_require(requested_posture != steady_posture, "%s: stream requested posture did not differ from steady posture during evaluation" % label)
	if _done:
		return ""
	_assert_candidate_index_bounded(report, label, "stream")
	if _done:
		return ""
	_step_ok("%s stream backing-plan evaluation classified multi-candidate evaluation path" % label)
	return "multi"


func _assert_candidate_index_bounded(report: Dictionary, label: String, target_label: String) -> void:
	var candidates: Array = report.get("candidate_sequence", [])
	var idx := int(report.get("current_candidate_index", 0))
	_require(idx >= 0 and idx < candidates.size(), "%s: %s backing-plan candidate index out of bounds (idx=%d size=%d)" % [label, target_label, idx, candidates.size()])


func _observe_multi_candidate_backing_plan_until_steady(stream_id: int, initial_report: Dictionary, label: String, target_label: String) -> Dictionary:
	var last_active_report := initial_report
	var last_index := int(initial_report.get("current_candidate_index", 0))
	var observed_active_transition := false
	for _i in range(STREAM_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_stream_backing_plan_evaluation_report(stream_id)
		if report.is_empty():
			continue
		_assert_backing_plan_primary_function(report, label, target_label, "display_view")
		if _done:
			return {}
		if bool(report.get("evaluator_active", false)):
			_assert_candidate_index_bounded(report, label, target_label)
			if _done:
				return {}
			var idx := int(report.get("current_candidate_index", 0))
			_require(idx >= last_index, "%s: %s backing-plan candidate index regressed (%d -> %d); prev=%s current=%s" % [label, target_label, last_index, idx, JSON.stringify(last_active_report), JSON.stringify(report)])
			if _done:
				return {}
			_require(idx <= last_index + 1, "%s: %s backing-plan evaluation progressed by more than one candidate (%d -> %d); prev=%s current=%s" % [label, target_label, last_index, idx, JSON.stringify(last_active_report), JSON.stringify(report)])
			if _done:
				return {}
			if idx != last_index:
				print("BACKING_PLAN_EVAL: %s_%s_progress_compare previous=%s current=%s" % [label, target_label, JSON.stringify(last_active_report), JSON.stringify(report)])
				observed_active_transition = true
			last_index = idx
			last_active_report = report
			continue
		if _backing_plan_valid(report, "steady"):
			_require(_backing_plan_posture(report, "requested") == _backing_plan_posture(report, "steady"), "%s: %s requested posture did not settle to steady posture" % [label, target_label])
			if _done:
				return {}
			if observed_active_transition:
				_step_ok("%s %s backing-plan active progression observed as monotonic and bounded" % [label, target_label])
			else:
				print("BACKING_PLAN_EVAL: %s_%s_no_distinct_active_transition initial=%s steady=%s" % [label, target_label, JSON.stringify(initial_report), JSON.stringify(report)])
				_step_ok("%s %s backing-plan evaluation remained bounded from first observed active candidate to steady" % [label, target_label])
			_step_ok("%s %s backing-plan evaluation reached steady posture" % [label, target_label])
			return {
				"candidate_source_report": last_active_report,
				"steady_report": report,
			}
	_fail("%s: timed out waiting for %s backing-plan steady posture settlement" % [label, target_label])
	return {}


func _wait_for_backing_plan_steady(stream_id: int, label: String, target_label: String) -> Dictionary:
	for _i in range(STREAM_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var report := _get_stream_backing_plan_evaluation_report(stream_id)
		if report.is_empty():
			continue
		if bool(report.get("evaluator_active", false)):
			continue
		if not _backing_plan_valid(report, "steady"):
			continue
		_require(_backing_plan_posture(report, "requested") == _backing_plan_posture(report, "steady"), "%s: %s requested posture did not settle to steady posture" % [label, target_label])
		if _done:
			return {}
		_step_ok("%s %s backing-plan evaluation reached steady posture" % [label, target_label])
		return report
	_fail("%s: timed out waiting for %s backing-plan steady posture settlement" % [label, target_label])
	return {}


func _assert_backing_plan_steady_reused(stream_id: int, steady_report: Dictionary, label: String, target_label: String) -> void:
	var steady_posture := _backing_plan_posture(steady_report, "steady")
	for _i in range(3):
		await get_tree().process_frame
		var report := _get_stream_backing_plan_evaluation_report(stream_id)
		_require(not report.is_empty(), "%s: %s backing-plan evaluation report missing while checking steady reuse" % [label, target_label])
		if _done:
			return
		_require(not bool(report.get("evaluator_active", false)), "%s: %s evaluator reactivated while checking steady reuse" % [label, target_label])
		if _done:
			return
		_require(_backing_plan_posture(report, "requested") == steady_posture and _backing_plan_posture(report, "steady") == steady_posture, "%s: %s steady posture was not reused" % [label, target_label])
		if _done:
			return
	_step_ok("%s %s backing-plan steady posture reused" % [label, target_label])


func _resolve_stream_steady_backing_plan(
	stream_id: int,
	observed_report: Dictionary,
	stream_plan_shape: String,
	label: String,
	target_label: String
) -> Dictionary:
	if stream_plan_shape == "single":
		var current := _get_stream_backing_plan_evaluation_report(stream_id)
		if not current.is_empty() and not bool(current.get("evaluator_active", false)) and _backing_plan_valid(current, "steady"):
			_require(_backing_plan_posture(current, "requested") == _backing_plan_posture(current, "steady"), "%s: %s requested posture did not settle to steady posture" % [label, target_label])
			if _done:
				return {}
			return current
		return await _wait_for_backing_plan_steady(stream_id, label, target_label)

	if not observed_report.is_empty() and not bool(observed_report.get("evaluator_active", false)) and _backing_plan_valid(observed_report, "steady"):
		_require(_backing_plan_posture(observed_report, "requested") == _backing_plan_posture(observed_report, "steady"), "%s: %s requested posture did not settle to steady posture" % [label, target_label])
		if _done:
			return {}
		return observed_report
	return await _wait_for_backing_plan_steady(stream_id, label, target_label)


func _assert_context_identity_scope(contexts: Array, label: String) -> void:
	_require(contexts.size() >= 2, "%s: dual-device pass requires at least two authored contexts" % label)
	if _done:
		return

	var tags := {}
	var hardware_ids := {}
	var device_instance_ids := {}
	var stream_ids := {}
	for context_v in contexts:
		if typeof(context_v) != TYPE_DICTIONARY:
			_fail("%s: context record is not a Dictionary" % label)
			return
		var context: Dictionary = context_v
		var tag := str(context.get("tag", ""))
		var hardware_id := str(context.get("hardware_id", ""))
		var device_instance_id := int(context.get("device_instance_id", 0))
		var stream_id := int(context.get("stream_id", 0))
		_require(tag != "" and hardware_id != "" and device_instance_id != 0 and stream_id != 0, "%s: context identity incomplete %s" % [label, JSON.stringify(context)])
		if _done:
			return
		_require(not tags.has(tag), "%s: duplicate target tag %s" % [label, tag])
		if _done:
			return
		_require(not hardware_ids.has(hardware_id), "%s: duplicate hardware_id %s" % [label, hardware_id])
		if _done:
			return
		_require(not device_instance_ids.has(device_instance_id), "%s: duplicate device_instance_id %d" % [label, device_instance_id])
		if _done:
			return
		_require(not stream_ids.has(stream_id), "%s: duplicate stream_id %d" % [label, stream_id])
		if _done:
			return
		tags[tag] = true
		hardware_ids[hardware_id] = true
		device_instance_ids[device_instance_id] = true
		stream_ids[stream_id] = true

	_step_ok("%s dual-device target identity scope established" % label)


func _build_expected_route_counts(records: Array) -> Dictionary:
	var route_counts := {}
	for record_v in records:
		if typeof(record_v) != TYPE_DICTIONARY:
			continue
		var record: Dictionary = record_v
		var stream_report: Dictionary = record.get("stream_report", {})
		var capture_report: Dictionary = record.get("capture_report", {})
		var stream_posture := _backing_plan_selection_posture(stream_report)
		var capture_posture := _backing_plan_selection_posture(capture_report)

		var display_view_key := _display_view_evidence_key_for_posture(stream_posture)
		if display_view_key != "":
			_increment_route_count(route_counts, display_view_key)

		var stream_route := _to_image_route_for_posture(stream_posture)
		if stream_route != "":
			_increment_route_count(route_counts, "stream_to_image.%s" % stream_route)

		var capture_route := _to_image_route_for_posture(capture_posture)
		if capture_route != "":
			_increment_route_count(route_counts, "capture_to_image.%s" % capture_route)

	return route_counts


func _increment_route_count(route_counts: Dictionary, route: String) -> void:
	route_counts[route] = int(route_counts.get(route, 0)) + 1


func _wait_for_expected_evidence_routes(label: String, route_expectations: Dictionary) -> Dictionary:
	for _i in range(CAPTURE_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var evidence := _get_result_access_timing_evidence()
		if evidence.is_empty():
			continue
		var all_ready := true
		for route in route_expectations.keys():
			var key := str(route)
			if not evidence.has(key):
				all_ready = false
				break
			var entry_v = evidence.get(key, {})
			if typeof(entry_v) != TYPE_DICTIONARY:
				all_ready = false
				break
			if int((entry_v as Dictionary).get("successes", 0)) <= 0:
				all_ready = false
				break
		if all_ready:
			return evidence
	_fail("%s: timed out waiting for expected retained-result evidence routes %s" % [label, JSON.stringify(route_expectations)])
	return {}


func _assert_expected_route_posture_counts(evidence: Dictionary, route_expectations: Dictionary, label: String) -> void:
	for route_v in route_expectations.keys():
		var route := str(route_v)
		_require(evidence.has(route), "%s: expected evidence route missing %s" % [label, route])
		if _done:
			return
		var entry_v = evidence.get(route, {})
		_require(typeof(entry_v) == TYPE_DICTIONARY, "%s: evidence route %s is not a Dictionary entry" % [label, route])
		if _done:
			return
		var entry: Dictionary = entry_v
		var expected_count := int(route_expectations.get(route, 0))
		_require(int(entry.get("posture_count", 0)) >= expected_count, "%s: evidence route %s posture_count=%d expected_at_least=%d" % [label, route, int(entry.get("posture_count", 0)), expected_count])
		if _done:
			return
	_step_ok("%s expected evidence posture counts verified" % label)


func _assert_backing_plan_evaluation_reports_distinct(records: Array, label: String) -> void:
	var reports := _get_backing_plan_evaluation_reports()
	var stream_reports := {}
	var capture_reports := {}
	for report_v in reports:
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		var target_kind := str(report.get("target_kind", ""))
		var target_id := int(report.get("target_id", 0))
		if target_kind == "stream":
			stream_reports[target_id] = report
		elif target_kind == "capture":
			capture_reports[target_id] = report

	for record_v in records:
		if typeof(record_v) != TYPE_DICTIONARY:
			continue
		var record: Dictionary = record_v
		var context: Dictionary = record.get("context", {})
		var stream_id := int(context.get("stream_id", 0))
		var device_instance_id := int(context.get("device_instance_id", 0))
		_require(stream_reports.has(stream_id), "%s: missing distinct stream backing-plan evaluation report for stream_id=%d" % [label, stream_id])
		if _done:
			return
		_require(capture_reports.has(device_instance_id), "%s: missing distinct capture backing-plan evaluation report for device_instance_id=%d" % [label, device_instance_id])
		if _done:
			return
	_step_ok("%s backing-plan evaluation report target identity verified" % label)


func _get_result_access_timing_evidence() -> Dictionary:
	var metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(metrics) != TYPE_DICTIONARY:
		return {}
	return metrics.get("result_access_timing_evidence", {})


func _print_evidence(tag: String, evidence: Dictionary) -> void:
	print("EVIDENCE: %s" % tag)
	print(JSON.stringify(evidence))


func _assert_expected_evidence_family(
	evidence: Dictionary,
	label: String,
	stream_report: Dictionary,
	capture_report: Dictionary
) -> void:
	var stored_selection := _synthetic_producer_output_form_setting()
	var effective_selection := _synthetic_producer_output_form_effective_selection()
	print("INFO: %s stored_selection=%s effective_selection=%s stream_downgrades=%s capture_downgrades=%s" % [
		label,
		stored_selection,
		effective_selection,
		_synthetic_stream_capability_downgrade_effective_selection(),
		_synthetic_capture_capability_downgrade_effective_selection(),
	])

	_require(not stream_report.is_empty(), "%s: stream backing-plan evaluation report missing for evidence verification" % label)
	if _done:
		return
	_require(not capture_report.is_empty(), "%s: capture backing-plan evaluation report missing for evidence verification" % label)
	if _done:
		return

	var stream_posture := _backing_plan_selection_posture(stream_report)
	var capture_posture := _backing_plan_selection_posture(capture_report)
	_require(stream_posture != "", "%s: stream backing-plan selection posture missing" % label)
	if _done:
		return
	_require(capture_posture != "", "%s: capture backing-plan selection posture missing" % label)
	if _done:
		return

	_assert_expected_display_view_entry_for_posture(evidence, label, "stream", stream_posture)
	if _done:
		return
	_assert_expected_to_image_entry_for_posture(evidence, label, "stream", stream_posture)
	if _done:
		return
	_assert_expected_to_image_entry_for_posture(evidence, label, "capture", capture_posture)
	if _done:
		return
	_assert_materializer_diagnostic_routes_if_present(evidence, label)


func _assert_materializer_diagnostic_routes_if_present(
	evidence: Dictionary,
	label: String
) -> void:
	var diagnostic_routes := [
		"stream_to_image.gpu_primary_cpu_sidecar_materializer",
		"capture_to_image.gpu_primary_cpu_sidecar_materializer",
	]
	for key in diagnostic_routes:
		if not evidence.has(key):
			continue
		_assert_expected_entry(
			evidence,
			label,
			key,
			true,
			true,
			true
		)
		if _done:
			return


func _display_view_evidence_key_for_posture(posture: String) -> String:
	match posture:
		"CPU-primary":
			return "stream_display_view.cpu_live_display_view"
		"GPU-primary, no CPU sidecar":
			return "stream_display_view.retained_gpu_backing"
		"GPU-primary, with CPU sidecar":
			return "stream_display_view.retained_gpu_backing"
		_:
			return ""


func _expected_entry_contract_for_route(route: String) -> Dictionary:
	match route:
		"cpu_packed":
			return {
				"expect_cpu_payload": true,
				"expect_gpu_backing": false,
				"expect_gpu_materialization": false,
			}
		"gpu_primary_no_cpu_sidecar_materializer":
			return {
				"expect_cpu_payload": false,
				"expect_gpu_backing": true,
				"expect_gpu_materialization": true,
			}
		"gpu_primary_cpu_sidecar_materializer":
			return {
				"expect_cpu_payload": true,
				"expect_gpu_backing": true,
				"expect_gpu_materialization": true,
			}
		"gpu_primary_cpu_sidecar":
			return {
				"expect_cpu_payload": true,
				"expect_gpu_backing": true,
				"expect_gpu_materialization": null,
			}
		"retained_gpu_backing":
			return {
				"expect_cpu_payload": null,
				"expect_gpu_backing": true,
				"expect_gpu_materialization": null,
			}
		"cpu_live_display_view":
			return {
				"expect_cpu_payload": true,
				"expect_gpu_backing": false,
				"expect_gpu_materialization": false,
			}
		_:
			return {}


func _assert_expected_display_view_entry_for_posture(
	evidence: Dictionary,
	label: String,
	scope: String,
	posture: String
) -> void:
	_require(scope == "stream", "%s: unsupported display_view scope %s" % [label, scope])
	if _done:
		return
	var key := _display_view_evidence_key_for_posture(posture)
	_require(key != "", "%s: unsupported %s display_view posture %s" % [label, scope, posture])
	if _done:
		return
	var route := key.trim_prefix("stream_display_view.")
	var contract := _expected_entry_contract_for_route(route)
	_assert_expected_entry(
		evidence,
		label,
		key,
		contract.get("expect_cpu_payload", null),
		contract.get("expect_gpu_backing", null),
		contract.get("expect_gpu_materialization", null)
	)


func _assert_expected_to_image_entry_for_posture(
	evidence: Dictionary,
	label: String,
	scope: String,
	posture: String
) -> void:
	var route := _to_image_route_for_posture(posture)
	_require(route != "", "%s: unsupported %s to_image posture %s" % [label, scope, posture])
	if _done:
		return
	var key := "%s_to_image.%s" % [scope, route]
	var contract := _expected_entry_contract_for_route(route)
	_assert_expected_entry(
		evidence,
		label,
		key,
		contract.get("expect_cpu_payload", null),
		contract.get("expect_gpu_backing", null),
		contract.get("expect_gpu_materialization", null)
	)


func _assert_expected_entry(
	evidence: Dictionary,
	label: String,
	key: String,
	expect_cpu_payload,
	expect_gpu_backing,
	expect_gpu_materialization
) -> void:
	_require(evidence.has(key), "%s: expected evidence key missing: %s" % [label, key])
	if _done:
		return
	_assert_entry_backing_facts(
		evidence[key],
		expect_cpu_payload,
		expect_gpu_backing,
		expect_gpu_materialization,
		"%s: %s" % [label, key]
	)
	_assert_entry_has_success_timing(evidence[key], "%s: %s" % [label, key])


func _assert_any_successful_timed_entry_with_prefix(evidence: Dictionary, label: String, prefix: String) -> void:
	for key in evidence.keys():
		if not str(key).begins_with(prefix):
			continue
		_assert_entry_has_success_timing(evidence[key], "%s: %s" % [label, str(key)])
		return
	_fail("%s: expected successful timed evidence with prefix %s" % [label, prefix])


func _assert_entry_has_success_timing(entry, context: String) -> void:
	_require(typeof(entry) == TYPE_DICTIONARY, "%s: evidence entry is not a Dictionary" % context)
	if _done:
		return
	_require(int(entry.get("successes", 0)) > 0, "%s: expected at least one successful retained-result access" % context)
	if _done:
		return
	_require(int(entry.get("total_ns", 0)) > 0 or int(entry.get("first_success_ns", 0)) > 0 or int(entry.get("fresh_result_total_ns", 0)) > 0, "%s: expected non-zero timing evidence" % context)


func _assert_entry_backing_facts(entry, expect_cpu_payload, expect_gpu_backing, expect_gpu_materialization, context: String) -> void:
	_require(typeof(entry) == TYPE_DICTIONARY, "%s: evidence entry is not a Dictionary" % context)
	if _done:
		return
	var got_cpu_payload := bool(entry.get("last_has_retained_cpu_payload", false))
	var got_gpu_backing := bool(entry.get("last_has_retained_gpu_backing", false))
	var got_gpu_materialization := bool(entry.get("last_gpu_materialization_available", false))
	if expect_cpu_payload != null:
		_require(got_cpu_payload == bool(expect_cpu_payload), "%s: expected last_has_retained_cpu_payload=%s got=%s" % [context, str(expect_cpu_payload), str(got_cpu_payload)])
		if _done:
			return
	if expect_gpu_backing != null:
		_require(got_gpu_backing == bool(expect_gpu_backing), "%s: expected last_has_retained_gpu_backing=%s got=%s" % [context, str(expect_gpu_backing), str(got_gpu_backing)])
		if _done:
			return
	if expect_gpu_materialization != null:
		_require(got_gpu_materialization == bool(expect_gpu_materialization), "%s: expected last_gpu_materialization_available=%s got=%s" % [context, str(expect_gpu_materialization), str(got_gpu_materialization)])


func _timed_out() -> bool:
	if _done:
		return true
	if Time.get_ticks_msec() - _start_ms <= TOTAL_TIMEOUT_MS:
		return false
	_fail("total verification timeout")
	return true


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	_fail(message)


func _step_ok(message: String) -> void:
	print("step %d OK: %s" % [_step, message])
	_step += 1


func _fail(message: String) -> void:
	if _done:
		return
	_done = true
	push_error("step %d FAIL: %s" % [_step, message])
	printerr("FAIL: %s" % message)
	_cleanup_and_quit(1)


func _cleanup_and_quit(code: int) -> void:
	_display_refs.clear()
	if _quit_requested:
		return
	_quit_requested = true
	print("INFO: quit requested code=%d" % code)
	print(CamBANGServer.get_synthetic_metrics_snapshot())
	CamBANGServer.stop_and_quit(code)
