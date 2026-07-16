extends Node

const SCENE_LABEL := "render_resource_worker_release_verify"
const TOTAL_TIMEOUT_MS := 25000
const WAIT_TIMEOUT_FRAMES := 360

var _done := false
var _quit_requested := false
var _start_ms := 0


func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	call_deferred("_run")


func _run() -> void:
	print("RUN: %s" % SCENE_LABEL)
	var gpu_path := _is_gpu_path()
	var smoke = ClassDB.instantiate(&"CamBANGRenderReleaseInternalSmoke")
	_require(smoke != null, "internal smoke class is unavailable; build with internal_smoke=yes")
	if _done:
		return
	await _exercise_standalone_post_transfer_failure(smoke, gpu_path)
	if _done:
		return
	await _exercise_gpu_wrapper_post_transfer_failure(smoke, gpu_path)
	if _done:
		return

	_require(await _start_stream_scenario(), "initial stream scenario failed")
	if _done:
		return
	var stream_id := await _wait_for_stream_id()
	var display_view = await _wait_for_display_view(stream_id)
	_require(display_view is Texture2D, "get_display_view() did not return Texture2D")
	if _done:
		return

	_require(smoke.retain_texture(display_view), "internal smoke could not retain display view")
	display_view = null
	CamBANGServer.stop()
	await _wait_for_retained_only(smoke, gpu_path)
	smoke.reset_release_stats()
	if gpu_path:
		_require(smoke.retained_texture_is_invalidated(), "GPU wrapper remained drawable after normal stop")
	_require(smoke.release_retained_from_worker(), "worker final-owner release failed")
	if _done:
		return
	var final_stats: Dictionary = await _wait_for_final_owner_release(smoke, gpu_path)
	_assert_safe_release_stats(final_stats, gpu_path)
	if _done:
		return

	# Baseline restart after the prior wrapper has been completely released.
	_require(await _start_stream_scenario(), "restart stream scenario failed")
	if _done:
		return
	stream_id = await _wait_for_stream_id()
	display_view = await _wait_for_display_view(stream_id)
	_require(display_view is Texture2D, "restart did not produce a display view")
	display_view = null
	CamBANGServer.stop()
	await _wait_for_no_active_reservations(smoke)
	if _done:
		return

	await _exercise_display_demand_incarnation_isolation(gpu_path)
	if _done:
		return

	await _exercise_runtime_failure_probes(smoke)
	if _done:
		return
	await _exercise_preterminal_quiescence(smoke, gpu_path)
	if _done:
		return

	# Terminal quarantine is tested separately and last because it deliberately
	# removes the approved drain context for the remainder of this process.
	_require(await _start_stream_scenario(), "terminal-case stream scenario failed")
	if _done:
		return
	stream_id = await _wait_for_stream_id()
	display_view = await _wait_for_display_view(stream_id)
	_require(display_view is Texture2D, "terminal case did not produce a display view")
	if _done:
		return
	_require(smoke.retain_texture(display_view), "terminal case could not retain display view")
	display_view = null
	CamBANGServer.stop()
	await _wait_for_retained_only(smoke, gpu_path)
	smoke.reset_release_stats()
	smoke.enter_terminal_release_state()
	_require(smoke.release_retained_from_worker(), "terminal worker release failed")
	var terminal_stats: Dictionary = smoke.get_release_stats()
	_require(int(terminal_stats.get("phase", 0)) == 4, "release service did not enter terminal phase")
	_require(int(terminal_stats.get("terminal_closed_rid_quarantine", 0)) == 1,
		"terminal RID quarantine count was not exactly one")
	_require(int(terminal_stats.get("terminal_closed_wrapper_quarantine", 0)) == (1 if gpu_path else 0),
		"terminal wrapper quarantine count mismatch")
	_require(int(terminal_stats.get("freed_rendering_server_rids", 0)) == 0 and
		int(terminal_stats.get("freed_rendering_device_rids", 0)) == 0,
		"terminal-unavailable resource was incorrectly freed")
	_require(int(terminal_stats.get("handoffs_in_flight", 0)) == 0 and
		int(terminal_stats.get("pending", 0)) == 0 and
		int(terminal_stats.get("emergency_pending", 0)) == 0,
		"post-cutoff quarantine left an admitted or in-flight handoff")
	if _done:
		return

	print("EVIDENCE: final_owner=%s runtime_failures=full+allocation+scheduling terminal=%s" % [
		str(final_stats),
		str(terminal_stats),
	])
	_emit_harness_verdict("ok", 0, "worker_final_owner_release_and_failure_paths_complete")
	_cleanup_and_quit(0)


func _exercise_standalone_post_transfer_failure(smoke, gpu_path: bool) -> void:
	smoke.reset_release_stats()
	if gpu_path:
		_require(smoke.exercise_primary_gpu_post_transfer_failure(),
			"primary GPU post-transfer failure was not injected")
	else:
		_require(smoke.exercise_cpu_post_transfer_failure(),
			"CPU post-transfer failure was not injected")
	var stats := await _wait_for_transfer_release(smoke, gpu_path, false, 0)
	_assert_transfer_release(stats, gpu_path, false, 0)


func _exercise_gpu_wrapper_post_transfer_failure(smoke, gpu_path: bool) -> void:
	if not gpu_path:
		return
	# The isolated wrapper owns its Texture2DRD before the injected failure.
	# Dropping its retained backing then proves recursive wrapper-to-RID drain.
	smoke.reset_release_stats()
	_require(smoke.exercise_gpu_wrapper_post_transfer_failure(),
		"GPU wrapper post-transfer failure unexpectedly returned a display view")
	var stats := await _wait_for_recursive_wrapper_release(smoke)
	_require(int(stats.get("accepted_handoffs", 0)) == 2,
		"recursive wrapper failure did not admit exactly wrapper and RID releases")
	_require(int(stats.get("drained_texture2drd_wrappers", 0)) == 1,
		"recursive wrapper failure did not drain exactly one Texture2DRD")
	_require(int(stats.get("freed_rendering_device_rids", 0)) == 1,
		"recursive wrapper failure did not free exactly one RD RID")
	_require(int(stats.get("active_reservations", 0)) == 0 and
		int(stats.get("pending", 0)) == 0 and
		int(stats.get("emergency_pending", 0)) == 0 and
		int(stats.get("handoffs_in_flight", 0)) == 0,
		"recursive wrapper failure did not return to quiescence")
	print("EVIDENCE: post_transfer gpu=true wrapper=true recursive=true stats=%s" % str(stats))


func _wait_for_recursive_wrapper_release(smoke) -> Dictionary:
	var last: Dictionary = {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		await get_tree().process_frame
		last = smoke.get_release_stats()
		if int(last.get("drained_texture2drd_wrappers", 0)) == 1 and \
			int(last.get("freed_rendering_device_rids", 0)) == 1 and \
			int(last.get("active_reservations", 0)) == 0:
			return last
		if _timed_out():
			break
	_fail("timed out waiting for recursive wrapper release: %s" % str(last))
	return {}


func _wait_for_transfer_release(smoke, gpu_path: bool, wrapper_case: bool, baseline: int) -> Dictionary:
	var last: Dictionary = {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		await get_tree().process_frame
		last = smoke.get_release_stats()
		var released := int(last.get("drained_texture2drd_wrappers", 0)) == 1 if wrapper_case else \
			int(last.get("freed_rendering_device_rids" if gpu_path else "freed_rendering_server_rids", 0)) == 1
		if released and int(last.get("active_reservations", 0)) == baseline:
			return last
		if _timed_out():
			break
	_fail("timed out waiting for post-transfer release: %s" % str(last))
	return {}


func _assert_transfer_release(stats: Dictionary, gpu_path: bool, wrapper_case: bool, baseline: int) -> void:
	_require(int(stats.get("accepted_handoffs", 0)) == 1, "post-transfer release was not singular")
	_require(int(stats.get("drained_texture2drd_wrappers", 0)) == (1 if wrapper_case else 0),
		"post-transfer wrapper drain count mismatch")
	_require(int(stats.get("freed_rendering_device_rids", 0)) == (1 if gpu_path and not wrapper_case else 0),
		"post-transfer RD RID free count mismatch")
	_require(int(stats.get("freed_rendering_server_rids", 0)) == (1 if not gpu_path else 0),
		"post-transfer RS RID free count mismatch")
	_require(int(stats.get("active_reservations", 0)) == baseline,
		"post-transfer release did not return its credit")
	_require(int(stats.get("duplicate_release_attempts", 0)) == 0,
		"post-transfer path attempted a duplicate/uncredited release")
	_require(int(stats.get("terminal_closed_wrapper_quarantine", 0)) == 0 and
		int(stats.get("terminal_closed_rid_quarantine", 0)) == 0,
		"post-transfer failure entered terminal quarantine")
	print("EVIDENCE: post_transfer gpu=%s wrapper=%s stats=%s" % [gpu_path, wrapper_case, str(stats)])


func _start_stream_scenario() -> bool:
	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	if start_err != OK:
		return false
	if CamBANGServer.select_builtin_scenario("stream_inspection_live") != OK:
		return false
	return CamBANGServer.start_scenario() == OK


func _is_gpu_path() -> bool:
	for arg in OS.get_cmdline_user_args():
		if str(arg) == "--cambang-synth-producer-output-form=gpu_only":
			return true
	# Android helper launches cannot forward user arguments; they patch the
	# same requested producer form into the existing maintainer project setting.
	return str(ProjectSettings.get_setting(
		"cambang/maintainer/synthetic_producer_output_form", "")) == "gpu_only"


func _wait_for_final_owner_release(smoke, gpu_path: bool) -> Dictionary:
	var last_stats: Dictionary = {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		await get_tree().process_frame
		var stats: Dictionary = smoke.get_release_stats()
		last_stats = stats
		var complete := false
		if gpu_path:
			complete = int(stats.get("drained_texture2drd_wrappers", 0)) == 1 and \
				int(stats.get("freed_rendering_device_rids", 0)) == 1
		else:
			complete = int(stats.get("freed_rendering_server_rids", 0)) == 1
		if complete and int(stats.get("active_reservations", 0)) == 0:
			return stats
		if _timed_out():
			break
	_fail("timed out waiting for exact final-owner release counters: %s" % str(last_stats))
	return {}


func _assert_safe_release_stats(stats: Dictionary, gpu_path: bool) -> void:
	_require(int(stats.get("accepted_handoffs", 0)) == (2 if gpu_path else 1),
		"final-owner accepted handoff count mismatch")
	_require(int(stats.get("drained_texture2drd_wrappers", 0)) == (1 if gpu_path else 0),
		"final-owner Texture2DRD drain count mismatch")
	_require(int(stats.get("freed_rendering_server_rids", 0)) == (0 if gpu_path else 1),
		"final-owner RenderingServer RID free count mismatch")
	_require(int(stats.get("freed_rendering_device_rids", 0)) == (1 if gpu_path else 0),
		"final-owner RenderingDevice RID free count mismatch")
	_require(int(stats.get("terminal_closed_wrapper_quarantine", 0)) == 0 and
		int(stats.get("terminal_closed_rid_quarantine", 0)) == 0,
		"ordinary final-owner release entered terminal quarantine")
	_require(int(stats.get("wrong_thread_release_attempt", 0)) == 0,
		"wrong-thread release attempt observed")
	_require(int(stats.get("release_while_lock_held", 0)) == 0,
		"release occurred while queue/registry lock was held")


func _wait_for_no_active_reservations(smoke) -> void:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		await get_tree().process_frame
		var stats: Dictionary = smoke.get_release_stats()
		if int(stats.get("active_reservations", 0)) == 0 and \
			int(stats.get("pending", 0)) == 0 and int(stats.get("emergency_pending", 0)) == 0:
			return
		if _timed_out():
			break
	_fail("restart cleanup did not return all release credits")


func _wait_for_retained_only(smoke, gpu_path: bool) -> void:
	var expected := 2 if gpu_path else 1
	var last: Dictionary = {}
	for _i in range(WAIT_TIMEOUT_FRAMES):
		await get_tree().process_frame
		last = smoke.get_release_stats()
		if int(last.get("active_reservations", 0)) == expected and \
			int(last.get("pending", 0)) == 0 and int(last.get("emergency_pending", 0)) == 0:
			return
		if _timed_out():
			break
	_fail("retained wrapper was not isolated from stop cleanup: %s" % str(last))


func _exercise_preterminal_quiescence(smoke, gpu_path: bool) -> void:
	_require(await _start_stream_scenario(), "pre-terminal quiescence scenario failed")
	if _done:
		return
	var stream_id := await _wait_for_stream_id()
	var display_view = await _wait_for_display_view(stream_id)
	_require(display_view is Texture2D, "pre-terminal case did not produce a display view")
	_require(smoke.retain_texture(display_view), "pre-terminal case could not retain display view")
	display_view = null
	CamBANGServer.stop()
	await _wait_for_retained_only(smoke, gpu_path)
	smoke.reset_release_stats()
	_require(smoke.release_retained_from_worker(), "pre-terminal worker release failed")
	# No frame is yielded here: teardown itself must schedule and recursively
	# drain wrapper -> retained state -> RD RID before becoming Terminal.
	smoke.enter_terminal_release_state()
	var stats: Dictionary = smoke.get_release_stats()
	_require(int(stats.get("phase", 0)) == 4, "pre-terminal teardown did not reach Terminal")
	_require(int(stats.get("accepted_handoffs", 0)) == (2 if gpu_path else 1),
		"pre-terminal accepted handoff count mismatch")
	_require(int(stats.get("drained_texture2drd_wrappers", 0)) == (1 if gpu_path else 0),
		"pre-terminal wrapper was not drained exactly once")
	_require(int(stats.get("freed_rendering_device_rids", 0)) == (1 if gpu_path else 0) and
		int(stats.get("freed_rendering_server_rids", 0)) == (0 if gpu_path else 1),
		"pre-terminal RID was not freed exactly once")
	_require(int(stats.get("pending", 0)) == 0 and int(stats.get("emergency_pending", 0)) == 0 and
		int(stats.get("active_reservations", 0)) == 0,
		"pre-terminal teardown did not reach ownership quiescence")
	_require(int(stats.get("terminal_closed_wrapper_quarantine", 0)) == 0 and
		int(stats.get("terminal_closed_rid_quarantine", 0)) == 0,
		"safely releasable pre-terminal ownership was quarantined")
	_require(int(stats.get("wrong_thread_release_attempt", 0)) == 0 and
		int(stats.get("release_while_lock_held", 0)) == 0 and
		int(stats.get("duplicate_release_attempts", 0)) == 0,
		"pre-terminal teardown observed wrong-thread, under-lock, or duplicate release")
	print("EVIDENCE: preterminal_quiescence=%s" % str(stats))
	_require(smoke.restart_release_service_after_clean_terminal(),
		"clean pre-terminal service could not be restored for cutoff-race verification")
	if _done:
		return

	smoke.reset_release_stats()
	_require(smoke.exercise_terminal_cutoff_race(),
		"pre-cutoff worker handoff was not admitted across teardown cutoff")
	stats = smoke.get_release_stats()
	_require(int(stats.get("phase", 0)) == 4,
		"cutoff-race teardown did not reach Terminal")
	_require(int(stats.get("accepted_handoffs", 0)) == 1 and
		int(stats.get("drained_smoke_probes", 0)) == 1,
		"pre-cutoff handoff was not drained exactly once")
	_require(int(stats.get("terminal_closed_wrapper_quarantine", 0)) == 0 and
		int(stats.get("terminal_closed_rid_quarantine", 0)) == 0,
		"pre-cutoff handoff was incorrectly quarantined")
	_require(int(stats.get("pending", 0)) == 0 and
		int(stats.get("emergency_pending", 0)) == 0 and
		int(stats.get("handoffs_in_flight", 0)) == 0 and
		int(stats.get("active_reservations", 0)) == 0,
		"cutoff-race teardown did not finish quiescent with all credits returned")
	print("EVIDENCE: terminal_cutoff_race=%s" % str(stats))
	_require(smoke.restart_release_service_after_clean_terminal(),
		"clean cutoff-race service could not be restored for late-terminal verification")


func _exercise_display_demand_incarnation_isolation(gpu_path: bool) -> void:
	var smoke_a = ClassDB.instantiate(&"CamBANGRenderReleaseInternalSmoke")
	var smoke_b = ClassDB.instantiate(&"CamBANGRenderReleaseInternalSmoke")
	_require(smoke_a != null and smoke_b != null,
		"display-demand incarnation smoke helpers are unavailable")
	_require(await _start_stream_scenario(), "display-demand generation A failed to start")
	if _done:
		return
	var stream_id_a := await _wait_for_stream_id()
	var view_a = await _wait_for_display_view(stream_id_a)
	_require(view_a is Texture2D, "display-demand generation A produced no display wrapper")
	_require(smoke_a.retain_texture(view_a), "could not retain generation A display wrapper")
	view_a = null
	CamBANGServer.stop()
	await _wait_for_retained_only(smoke_a, gpu_path)

	_require(await _start_stream_scenario(), "display-demand generation B failed to start")
	if _done:
		return
	var stream_id_b := await _wait_for_stream_id()
	_require(stream_id_b == stream_id_a,
		"synthetic restart did not reuse the authored stream identity")
	var view_b = await _wait_for_display_view(stream_id_b)
	_require(view_b is Texture2D, "display-demand generation B produced no display wrapper")
	_require(smoke_b.retain_texture(view_b), "could not retain generation B display wrapper")
	view_b = null
	# Let GDScript release the completed call's temporary Ref before asserting
	# that the native smoke holder is the wrapper's final owner.
	await get_tree().process_frame
	_require(int(smoke_b.get_persistent_display_demand_refcount(stream_id_b)) == 1,
		"generation B persistent demand was not exactly one")

	_require(smoke_a.release_retained_from_worker(),
		"generation A worker release failed after restart")
	_require(int(smoke_b.get_persistent_display_demand_refcount(stream_id_b)) == 1,
		"stale generation A release decremented generation B demand")
	_require(smoke_b.release_retained_from_worker(),
		"generation B worker release failed")
	var final_demand_refcount := int(smoke_b.get_persistent_display_demand_refcount(stream_id_b))
	_require(final_demand_refcount == 0,
		"generation B demand did not reach zero exactly once; refcount=%d" % final_demand_refcount)
	CamBANGServer.stop()
	await _wait_for_no_active_reservations(smoke_b)
	print("EVIDENCE: display_demand_incarnation stream_id=%d path=%s refcount=1->1->0" % [
		stream_id_b,
		"gpu" if gpu_path else "cpu",
	])


func _exercise_runtime_failure_probes(smoke) -> void:
	# Scheduling failure retains ownership and is retried by later Godot frames.
	# It must not poison creation or require a test-only recovery.
	smoke.reset_release_stats()
	_require(smoke.force_schedule_failure_probe_from_worker(), "schedule-failure worker probe was not admitted")
	var stats: Dictionary = await _wait_for_probe_drain(smoke, "scheduling failure")
	_require(int(stats.get("scheduling_failure", 0)) == 1 and
		int(stats.get("drained_smoke_probes", 0)) == 1,
		"scheduling-failure probe did not fail once then drain")
	_require(int(stats.get("phase", 0)) == 1 and
		int(stats.get("runtime_failure_transitions", 0)) == 0,
		"scheduling failure permanently disabled creation")
	_require(int(stats.get("godot_thread_scheduling_attempts", 0)) >= 2 and
		int(stats.get("scheduling_wrong_thread", 0)) == 0,
		"drain scheduling did not remain on the installation/Godot thread")
	print("EVIDENCE: scheduling_retry=%s" % str(stats))

	_require(await _start_stream_scenario(), "runtime-full authority scenario failed to start")
	smoke.reset_release_stats()
	_require(smoke.force_runtime_full_probe_from_worker(), "runtime-full worker probe was not retained")
	stats = smoke.get_release_stats()
	_require(int(stats.get("runtime_full", 0)) == 1 and int(stats.get("emergency_pending", 0)) == 1,
		"runtime-full probe did not enter bounded emergency ownership")
	_require(int(stats.get("terminal_closed_rid_quarantine", 0)) == 0 and
		int(stats.get("terminal_closed_wrapper_quarantine", 0)) == 0,
		"runtime-full probe was terminal-quarantined")
	stats = await _wait_for_probe_drain(smoke, "runtime full")
	_require(int(stats.get("drained_smoke_probes", 0)) == 1,
		"runtime-full probe was not destroyed in the render drain")
	_require(CamBANGServer.get_state_snapshot() == null,
		"unrecoverable render-release failure did not stop public runtime truth")
	print("EVIDENCE: authoritative_full=%s" % str(stats))
	var rejected_start := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED)
	_require(rejected_start == ERR_UNAVAILABLE,
		"unrecoverable render-release failure did not reject restart")
	_require(smoke.recover_release_service(), "runtime-full recovery did not reopen test service")

	_require(await _start_stream_scenario(), "allocation-failure authority scenario failed to start")
	smoke.reset_release_stats()
	_require(smoke.force_allocation_failure_probe_from_worker(), "allocation-failure worker probe was not retained")
	stats = smoke.get_release_stats()
	_require(int(stats.get("runtime_allocation_failure", 0)) == 1 and
		int(stats.get("emergency_pending", 0)) == 1,
		"allocation-failure probe did not enter bounded emergency ownership")
	stats = await _wait_for_probe_drain(smoke, "allocation failure")
	_require(int(stats.get("drained_smoke_probes", 0)) == 1,
		"allocation-failure probe was not destroyed in the render drain")
	_require(CamBANGServer.get_state_snapshot() == null,
		"allocation failure did not reach the stopped runtime boundary")
	print("EVIDENCE: authoritative_allocation=%s" % str(stats))
	_require(smoke.recover_release_service(), "allocation-failure recovery did not reopen test service")
	_require(await _start_stream_scenario(), "post-failure test recovery could not restart")
	CamBANGServer.stop()
	await _wait_for_no_active_reservations(smoke)
	_require(int(stats.get("wrong_thread_release_attempt", 0)) == 0 and
		int(stats.get("release_while_lock_held", 0)) == 0 and
		int(stats.get("duplicate_release_attempts", 0)) == 0,
		"failure probes observed wrong-thread or under-lock release")


func _wait_for_probe_drain(smoke, label: String) -> Dictionary:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		await get_tree().process_frame
		var stats: Dictionary = smoke.get_release_stats()
		if int(stats.get("drained_smoke_probes", 0)) == 1 and \
			int(stats.get("active_reservations", 0)) == 0 and \
			int(stats.get("pending", 0)) == 0 and int(stats.get("emergency_pending", 0)) == 0:
			return stats
		if _timed_out():
			break
	_fail("timed out waiting for %s probe drain" % label)
	return {}


func _wait_for_stream_id() -> int:
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return 0
		await get_tree().process_frame
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot == null:
			continue
		for stream in snapshot.get("streams", []):
			if typeof(stream) == TYPE_DICTIONARY:
				var stream_id := int(stream.get("stream_id", 0))
				if stream_id != 0:
					return stream_id
	_fail("timed out waiting for stream identity")
	return 0


func _wait_for_display_view(stream_id: int):
	for _i in range(WAIT_TIMEOUT_FRAMES):
		if _timed_out():
			return null
		await get_tree().process_frame
		var result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if result == null:
			continue
		var display_view = result.get_display_view()
		if display_view is Texture2D:
			return display_view
	_fail("timed out waiting for stream display view")
	return null


func _timed_out() -> bool:
	if Time.get_ticks_msec() - _start_ms <= TOTAL_TIMEOUT_MS:
		return false
	_fail("scene exceeded %d ms" % TOTAL_TIMEOUT_MS)
	return true


func _require(condition: bool, detail: String) -> void:
	if not condition:
		_fail(detail)


func _fail(detail: String) -> void:
	if _done:
		return
	_done = true
	printerr("FAIL: %s" % detail)
	_emit_harness_verdict("fail", 1, detail)
	_cleanup_and_quit(1)


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
