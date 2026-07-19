extends SceneTree

const BOOTSTRAP_MAX_FRAMES := 300
const WRAPPER_BURST := 64


func _initialize() -> void:
	ProjectSettings.set_setting(
		"cambang/maintainer/synthetic_producer_output_form", "cpu_only"
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
		display_view = result.get_display_view()
		if display_view == null:
			_finish_fail("cpu_display_view_unavailable")
			return
		wrappers.push_back(display_view)

	# Give at least one accepted create callback an opportunity to realize its
	# RID, then drop every public borrow immediately before stop so the accepted
	# release callback races stop/extension teardown without leaking user refs.
	await process_frame
	await process_frame
	display_view = null
	result = null
	wrapper_release_burst(wrappers)
	CamBANGServer.stop()
	print("[CamBANG][RaceStress] kind=cpu stream_id=%d wrappers=%d stop_complete=true" % [stream_id, WRAPPER_BURST])
	_finish_ok("accepted_create_and_release_teardown")


func wrapper_release_burst(wrappers: Array) -> void:
	wrappers.clear()


func _latch_stream_id() -> int:
	for _frame_index in range(BOOTSTRAP_MAX_FRAMES):
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot != null:
			var streams: Array = snapshot.get("streams", [])
			if not streams.is_empty():
				var candidate := int((streams[0] as Dictionary).get("stream_id", 0))
				if candidate > 0:
					return candidate
		await process_frame
	return 0


func _finish_ok(reason: String) -> void:
	print("[CamBANG][HarnessVerdict] scene=cpu_display_teardown_race_stress status=ok exit_code=0 reason=%s" % reason)
	quit(0)


func _finish_fail(reason: String) -> void:
	print("[CamBANG][HarnessVerdict] scene=cpu_display_teardown_race_stress status=fail exit_code=1 reason=%s" % reason)
	quit(1)
