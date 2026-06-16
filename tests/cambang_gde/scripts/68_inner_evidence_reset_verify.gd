extends Node

## Scene 68: inner evidence reset verify
##
## Purpose:
## - verify the raw, un-arbitrated inner result-access evidence layer only
## - verify internally calibrated stream and capture result-access evidence
## - first prove that materialization-route evidence can appear without any
##   public to_image()/to_image_member() calls from the scene itself
## - prove that result_access_timing_evidence is cleared by stop()
## - prove that fresh evidence is recorded again after restart
##
## Scope guardrail:
## - this is a verification scene / harness scene, not a scenario semantic owner
## - this scene intentionally does not verify any future outer posture chooser
## - it must stay at the real result-operation seam and must not turn into a
##   rendering/teaching scene like Scene 70
##
## Design choice:
## - use a minimal external synthetic scenario only to realize one deterministic
##   repeating stream; scenario semantics remain provider-owned
## - the access-only session must not seed materialisation evidence through
##   public to_image()/to_image_member() calls
## - do not rely on UI rendering to seed evidence
## - intentionally verify the default still-capture profile path rather than
##   submitting an explicit still profile; this keeps Scene 68 grounded on the
##   default case and helps guard against evidence polling accidentally becoming
##   coupled to manual still-profile submission

const SCENE_LABEL := "inner_evidence_reset_verify"
const SCENARIO_PATH := "res://scenarios/68_inner_evidence_reset_live.json"

const TOTAL_TIMEOUT_MS := 12000
const BASELINE_TIMEOUT_FRAMES := 900
const ID_TIMEOUT_FRAMES := 900
const STREAM_RESULT_TIMEOUT_FRAMES := 900
const DEFAULT_STILL_PROFILE_TIMEOUT_FRAMES := 900
const CAPTURE_RESULT_TIMEOUT_FRAMES := 900
const STOP_TIMEOUT_FRAMES := 240

var _step := 0
var _done := false
var _quit_requested := false
var _start_ms := 0
var _baseline_gens: Array[int] = []
var _display_refs: Array = []


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
	await _run_impl()


func _run_impl() -> void:
	var session_0 := await _run_session_access_only(-1, "session_0_access_only")
	if _done:
		return
	_print_evidence("session_0_access_only_post_access", session_0.get("evidence", {}))

	await _stop_and_verify_reset()
	if _done:
		return

	var session_1 := await _run_session(int(session_0.get("gen", 0)), "session_1")
	if _done:
		return
	_print_evidence("session_1_post_access", session_1.get("evidence", {}))

	var gen_0 := int(session_0.get("gen", 0))
	var gen_1 := int(session_1.get("gen", 0))
	_require(gen_1 > gen_0, "generation did not advance across first restart (%d -> %d)" % [gen_0, gen_1])
	if _done:
		return

	await _stop_and_verify_reset()
	if _done:
		return

	var session_2 := await _run_session(int(session_1.get("gen", 0)), "session_2")
	if _done:
		return
	_print_evidence("session_2_post_access", session_2.get("evidence", {}))

	var gen_2 := int(session_2.get("gen", 0))
	_require(gen_2 > gen_1, "generation did not advance across second restart (%d -> %d)" % [gen_1, gen_2])
	if _done:
		return

	_step_ok("inner evidence reset verified")
	_cleanup_and_quit(0)


func _run_session_access_only(previous_gen: int, label: String) -> Dictionary:
	_display_refs.clear()
	_bootstrap_runtime_and_stage_external_scenario(label)
	if _done:
		return {}

	var gen := await _wait_for_new_baseline(previous_gen, label)
	if _done:
		return {}

	var ids := await _wait_for_runtime_ids(gen, label)
	if _done:
		return {}
	_step_ok("%s identifiers latched (device_instance_id=%d stream_id=%d)" % [
		label,
		int(ids.get("device_instance_id", 0)),
		int(ids.get("stream_id", 0)),
	])

	var hardware_id := str(ids.get("hardware_id", ""))
	var device_instance_id := int(ids.get("device_instance_id", 0))
	var stream_id := int(ids.get("stream_id", 0))

	_require(device_instance_id != 0, "%s: snapshot device_instance_id is 0" % label)
	if _done:
		return {}
	var device = CamBANGServer.get_device(device_instance_id)
	_require(device != null, "%s: get_device(%d) returned null" % [label, device_instance_id])
	if _done:
		return {}
	_print_device_handle_diag(label, "post_get_device", device, ids)

	var stream_result = await _wait_for_stream_result(stream_id, label)
	if _done:
		return {}
	await _exercise_stream_access_without_public_to_image(stream_id, stream_result, label)
	if _done:
		return {}
	_step_ok("%s stream access-only evidence seeded" % label)

	await _require_default_still_profile_visible(device_instance_id, label)
	if _done:
		return {}
	_step_ok("%s default still profile snapshot-visible" % label)

	var capture_result = await _trigger_and_wait_capture(device, ids, label)
	if _done:
		return {}
	var access_probe := _exercise_capture_access_without_public_to_image(capture_result, label)
	if _done:
		return {}
	_step_ok("%s capture access-only evidence seeded" % label)

	var evidence := await _wait_for_access_only_measurement_evidence(label)
	if _done:
		return {}
	_assert_expected_evidence_family(evidence, label)
	if _done:
		return {}
	_assert_access_only_measurement_evidence(evidence, label, access_probe)
	if _done:
		return {}
	_step_ok("%s access-only evidence verified" % label)

	return {
		"gen": gen,
		"hardware_id": hardware_id,
		"device_instance_id": device_instance_id,
		"stream_id": stream_id,
		"evidence": evidence,
	}


func _run_session(previous_gen: int, label: String) -> Dictionary:
	_display_refs.clear()
	_bootstrap_runtime_and_stage_external_scenario(label)
	if _done:
		return {}

	var gen := await _wait_for_new_baseline(previous_gen, label)
	if _done:
		return {}

	var ids := await _wait_for_runtime_ids(gen, label)
	if _done:
		return {}
	_step_ok("%s identifiers latched (device_instance_id=%d stream_id=%d)" % [
		label,
		int(ids.get("device_instance_id", 0)),
		int(ids.get("stream_id", 0)),
	])

	var hardware_id := str(ids.get("hardware_id", ""))
	var device_instance_id := int(ids.get("device_instance_id", 0))
	var stream_id := int(ids.get("stream_id", 0))

	_require(device_instance_id != 0, "%s: snapshot device_instance_id is 0" % label)
	if _done:
		return {}
	var device = CamBANGServer.get_device(device_instance_id)
	_require(device != null, "%s: get_device(%d) returned null" % [label, device_instance_id])
	if _done:
		return {}
	_print_device_handle_diag(label, "post_get_device", device, ids)

	var stream_result = await _wait_for_stream_result(stream_id, label)
	if _done:
		return {}
	await _exercise_stream_access(stream_id, stream_result, label)
	if _done:
		return {}
	_step_ok("%s stream evidence seeded" % label)

	await _require_default_still_profile_visible(device_instance_id, label)
	if _done:
		return {}
	_step_ok("%s default still profile snapshot-visible" % label)

	var capture_result = await _trigger_and_wait_capture(device, ids, label)
	if _done:
		return {}
	_exercise_capture_access(capture_result, label)
	if _done:
		return {}
	_step_ok("%s capture evidence seeded" % label)

	await get_tree().process_frame
	var evidence := _get_result_access_timing_evidence()
	_require(not evidence.is_empty(), "%s: result_access_timing_evidence is empty after result access" % label)
	if _done:
		return {}
	_assert_expected_evidence_family(evidence, label)
	if _done:
		return {}
	_step_ok("%s evidence verified" % label)

	return {
		"gen": gen,
		"hardware_id": hardware_id,
		"device_instance_id": device_instance_id,
		"stream_id": stream_id,
		"evidence": evidence,
	}


func _bootstrap_runtime_and_stage_external_scenario(label: String) -> void:
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

	var scenario_text := FileAccess.get_file_as_string(SCENARIO_PATH)
	_require(scenario_text != "", "%s: scenario missing at %s" % [label, SCENARIO_PATH])
	if _done:
		return
	var stage_err := CamBANGServer.load_external_scenario(scenario_text)
	_require(stage_err == OK, "%s: unable to load external scenario (%d)" % [label, stage_err])
	if _done:
		return
	_step_ok("%s external scenario staged (68_inner_evidence_reset_live)" % label)

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


func _wait_for_runtime_ids(expected_gen: int, label: String) -> Dictionary:
	for _i in range(ID_TIMEOUT_FRAMES):
		if _timed_out():
			return {}
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null or typeof(snapshot) != TYPE_DICTIONARY:
			continue
		if int(snapshot.get("gen", -1)) != expected_gen:
			continue

		var devices: Array = snapshot.get("devices", [])
		var streams: Array = snapshot.get("streams", [])
		if devices.is_empty() or streams.is_empty():
			continue
		var first_device = devices[0]
		var first_stream = streams[0]
		if typeof(first_device) != TYPE_DICTIONARY or typeof(first_stream) != TYPE_DICTIONARY:
			continue

		var hardware_id := str(first_device.get("hardware_id", ""))
		var device_instance_id := int(first_device.get("instance_id", 0))
		var stream_id := int(first_stream.get("stream_id", 0))
		if hardware_id != "" and device_instance_id != 0 and stream_id != 0:
			return {
				"hardware_id": hardware_id,
				"device_instance_id": device_instance_id,
				"stream_id": stream_id,
			}
	_fail("%s: timed out waiting for device/stream ids in gen=%d snapshot" % [label, expected_gen])
	return {}


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


func _exercise_stream_access(stream_id: int, stream_result, label: String) -> void:
	_require(stream_result != null, "%s: stream_result is null" % label)
	if _done:
		return
	_require(stream_result.can_to_image(), "%s: stream_result.can_to_image() returned false" % label)
	if _done:
		return
	var image = stream_result.to_image()
	_require(image != null, "%s: stream_result.to_image() returned null" % label)
	if _done:
		return
	await _establish_stream_display_view_demand(stream_id, stream_result, label)


func _exercise_stream_access_without_public_to_image(stream_id: int, stream_result, label: String) -> void:
	_require(stream_result != null, "%s: stream_result is null" % label)
	if _done:
		return
	# Access-only session guardrail: do not call to_image(), but do exercise the
	# public capability queries so the inner polling/calibration layer can launch
	# from the realized posture without a user materialization call.
	var _stream_to_image_capability = int(stream_result.can_to_image())
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
	var baseline_progress := _get_capture_progress_snapshot(device_instance_id)
	_require(bool(baseline_progress.get("available", false)), "%s: capture progress snapshot unavailable before trigger" % label)
	if _done:
		return null

	var trigger_err := int(device.trigger_capture())
	_require(trigger_err == OK, "%s: device.trigger_capture() failed err=%d" % [label, trigger_err])
	if _done:
		return null

	for _i in range(CAPTURE_RESULT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var progress := _get_capture_progress_snapshot(device_instance_id)
		if not bool(progress.get("available", false)):
			continue
		if int(progress.get("captures_failed", 0)) > int(baseline_progress.get("captures_failed", 0)):
			_fail("%s: capture failed after trigger (baseline=%s progress=%s)" % [label, str(baseline_progress), str(progress)])
			return null
		if int(progress.get("captures_completed", 0)) <= int(baseline_progress.get("captures_completed", 0)):
			continue
		var capture_result = device.get_result()
		if capture_result != null and int(capture_result.get_image_count()) > 0:
			return capture_result
	_fail("%s: timed out waiting for completed capture result" % label)
	return null


func _exercise_capture_access(capture_result, label: String) -> void:
	_require(capture_result != null, "%s: capture_result is null" % label)
	if _done:
		return
	_require(capture_result.can_to_image(), "%s: capture_result.can_to_image() returned false" % label)
	if _done:
		return
	var image = capture_result.to_image()
	_require(image != null, "%s: capture_result.to_image() returned null" % label)
	if _done:
		return
	_require(capture_result.can_to_image_member(0), "%s: capture_result.can_to_image_member(0) returned false" % label)
	if _done:
		return
	var member0 = capture_result.to_image_member(0)
	_require(member0 != null, "%s: capture_result.to_image_member(0) returned null" % label)
	if _done:
		return
	var image_count := int(capture_result.get_image_count())
	if image_count > 1 and capture_result.can_to_image_member(1):
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


func _evidence_has_prefix(evidence: Dictionary, prefix: String) -> bool:
	for key in evidence.keys():
		if str(key).begins_with(prefix):
			return true
	return false


func _assert_access_only_measurement_evidence(evidence: Dictionary, label: String, access_probe: Dictionary) -> void:
	# This session must never call public to_image* methods itself. It exists to
	# prove that inner measurement hooks can still populate the materialization
	# route evidence independently.
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


func _get_result_access_timing_evidence() -> Dictionary:
	var metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(metrics) != TYPE_DICTIONARY:
		return {}
	return metrics.get("result_access_timing_evidence", {})


func _print_evidence(tag: String, evidence: Dictionary) -> void:
	print("EVIDENCE: %s" % tag)
	print(JSON.stringify(evidence))


func _assert_expected_evidence_family(evidence: Dictionary, label: String) -> void:
	var stored_selection := str(ProjectSettings.get_setting(
		"cambang/maintainer/synthetic_producer_output_form",
		"runtime_default"
	))
	print("INFO: %s stored_selection=%s" % [label, stored_selection])

	match stored_selection:
		"cpu_only":
			_assert_expected_entry(evidence, label, "stream_display_view.cpu_live_display_view", true, false, false)
			if _done:
				return
			_assert_expected_entry(evidence, label, "stream_to_image.cpu_packed", true, false, false)
			if _done:
				return
			_assert_expected_entry(evidence, label, "capture_to_image.cpu_packed", true, false, false)

		"cpu_gpu":
			_assert_expected_entry(evidence, label, "stream_display_view.retained_gpu_backing", null, true, null)
			if _done:
				return
			_assert_expected_entry(evidence, label, "stream_to_image.gpu_primary_cpu_sidecar", true, true, null)
			if _done:
				return
			_assert_expected_entry(evidence, label, "capture_to_image.cpu_packed", true, null, null)

		"gpu_only":
			_assert_expected_entry(evidence, label, "stream_display_view.retained_gpu_backing", false, true, null)
			if _done:
				return
			_assert_expected_entry(evidence, label, "stream_to_image.gpu_synthetic_backing_materializer", false, true, true)
			if _done:
				return
			_assert_expected_entry(evidence, label, "capture_to_image.gpu_synthetic_backing_materializer", false, true, true)

		"runtime_default":
			_assert_any_successful_timed_entry_with_prefix(evidence, label, "stream_display_view.")
			if _done:
				return
			_assert_any_successful_timed_entry_with_prefix(evidence, label, "stream_to_image.")
			if _done:
				return
			_assert_any_successful_timed_entry_with_prefix(evidence, label, "capture_to_image.")

		_:
			_fail("%s: unsupported synthetic producer output-form selection %s" % [label, stored_selection])
			return


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
