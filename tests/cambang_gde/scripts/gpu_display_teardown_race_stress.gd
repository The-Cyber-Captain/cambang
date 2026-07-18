extends SceneTree

const BOOTSTRAP_MAX_FRAMES := 300
const WRAPPER_BURST := 64
const PAYLOAD_KIND_GPU_SURFACE := 2


func _initialize() -> void:
	ProjectSettings.set_setting(
		"cambang/maintainer/synthetic_producer_output_form", "gpu_only"
	)

	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	if start_err != OK:
		_finish_fail("server_start_%d" % start_err)
		return

	var stage_err := CamBANGServer.select_builtin_scenario("stream_inspection_live")
	if stage_err != OK:
		_finish_fail("scenario_stage_%d" % stage_err)
		return
	var scenario_start_err := CamBANGServer.start_scenario()
	if scenario_start_err != OK:
		_finish_fail("scenario_start_%d" % scenario_start_err)
		return

	var stream_id := await _latch_stream_id()
	if stream_id == 0:
		_finish_fail("stream_not_observed")
		return

	var wrappers: Array = []
	var result = null
	var display_view = null
	for _index in range(WRAPPER_BURST):
		result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if result == null:
			await process_frame
			continue
		if int(result.get_payload_kind()) != PAYLOAD_KIND_GPU_SURFACE:
			_finish_fail("result_not_gpu_primary")
			return
		display_view = result.get_display_view()
		if display_view == null:
			_finish_fail("gpu_display_view_unavailable")
			return
		wrappers.push_back(display_view)

	await process_frame
	display_view = null
	result = null
	wrappers.clear()
	CamBANGServer.stop()
	print("[CamBANG][RaceStress] kind=gpu stream_id=%d wrappers=%d stop_complete=true" % [stream_id, WRAPPER_BURST])
	_finish_ok("accepted_wrapper_and_rid_teardown")


func _latch_stream_id() -> int:
	for _frame_index in range(BOOTSTRAP_MAX_FRAMES):
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot != null:
			var streams: Array = snapshot.get("streams", [])
			if not streams.is_empty():
				var candidate := int((streams[0] as Dictionary).get("stream_id", 0))
				if candidate > 0:
					var observed_result = CamBANGServer.get_stream_result_by_stream_id(candidate)
					if observed_result != null:
						# Do not retain the latching probe in the suspended coroutine
						# frame through immediate engine shutdown. The teardown race
						# itself creates and explicitly releases its own result/views.
						observed_result = null
						return candidate
		await process_frame
	return 0


func _finish_ok(reason: String) -> void:
	print("[CamBANG][HarnessVerdict] scene=gpu_display_teardown_race_stress status=ok exit_code=0 reason=%s" % reason)
	quit(0)


func _finish_fail(reason: String) -> void:
	print("[CamBANG][HarnessVerdict] scene=gpu_display_teardown_race_stress status=fail exit_code=1 reason=%s" % reason)
	quit(1)
