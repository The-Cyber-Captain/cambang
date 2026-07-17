extends SceneTree

# Empirical stress test for the "extension-teardown race" question raised
# against enqueue_live_cpu_texture_create()/request_pending_live_cpu_texture_create_drain()
# in src/godot/cambang_stream_result_internal.cpp.
#
# What this is racing:
#   Thread A (this script's background Thread) repeatedly calls
#   CamBANGStreamResult.get_display_view(), which -- on the very first call for
#   a given stream/dimensions -- reaches enqueue_live_cpu_texture_create() and
#   schedules a RenderingServer::call_on_render_thread() callback targeting a
#   RefCounted helper (LiveCpuTextureCreateDrainHelper). The Callable(Object*,
#   StringName) constructor godot-cpp uses there does NOT hold a strong
#   reference to that helper (verified against
#   thirdparty/godot-cpp/gen/src/variant/callable.cpp) -- only the local
#   godot::Ref taken during scheduling keeps it alive, and only for the
#   duration of that synchronous call.
#
#   Thread B is Godot's own engine shutdown sequence: once this script calls
#   quit(), the engine proceeds toward calling the GDExtension terminator
#   (cambang_gde_uninitialize -> uninstall_live_cpu_display_bridge()), which
#   drops the bridge's own (global) reference to the same helper.
#
# If Thread A's enqueue+schedule is still in flight -- specifically, if the
# render thread has not yet executed the deferred callback -- when Thread B's
# uninstall drops the last reference, the callback's target may be destroyed
# before it runs. This script does not prove that is impossible; it drives
# many repeated attempts at hitting that exact window and reports whether any
# of them produced a crash (as opposed to Godot's engine-level Callable
# dispatch safely no-op'ing against a stale ObjectID, which is the expected
# outcome if the auditor's narrower "stale reference" observation does not
# also imply a crash).
#
# The background thread is bounded (time and iteration count) so a single run
# cannot hang forever even if it never manages to race the shutdown sequence.
#
# Synchronization note: the main thread waits for a signal that the
# background thread has completed at least one get_display_view() call
# before calling quit(). This is NOT synchronizing away the race -- by the
# time get_display_view() returns, enqueue_live_cpu_texture_create() has
# already run synchronously (it only enqueues + calls
# RenderingServer::call_on_render_thread(), which schedules and returns
# immediately; it does not wait for the render thread to actually execute the
# callback). So quitting right after that first return is the adversarial
# condition: the deferred render-thread callback is very likely still
# pending when shutdown begins. An earlier version of this script called
# quit() with zero synchronization at all and reliably lost the race before
# the background thread even reached its first engine call (observed as
# "Cannot call method 'get_ticks_usec' on a null value" on essentially every
# run) -- too early to exercise the mechanism under test at all.

const HAMMER_MAX_SECONDS := 3.0
const HAMMER_MAX_ITERATIONS := 200000
const BOOTSTRAP_MAX_FRAMES := 300
const FIRST_CALL_WAIT_MAX_MSEC := 2000

var _stress_thread: Thread = null
var _first_call_mutex := Mutex.new()
var _first_call_done := false


func _initialize() -> void:
	# Force the CPU-backed live display path (not GPU_SURFACE) so
	# get_display_view() reaches enqueue_live_cpu_texture_create().
	ProjectSettings.set_setting("cambang/maintainer/synthetic_producer_output_form", "cpu_only")

	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	if start_err != OK:
		print("FAIL: cpu_display_teardown_race_stress could not start server (err=%d)" % start_err)
		quit(1)
		return

	var stage_err := CamBANGServer.select_builtin_scenario("stream_inspection_live")
	if stage_err != OK:
		print("FAIL: cpu_display_teardown_race_stress could not stage scenario (err=%d)" % stage_err)
		quit(1)
		return

	var scenario_start_err := CamBANGServer.start_scenario()
	if scenario_start_err != OK:
		print("FAIL: cpu_display_teardown_race_stress could not start scenario (err=%d)" % scenario_start_err)
		quit(1)
		return

	var stream_id := await _latch_stream_id()
	if stream_id == 0:
		print("FAIL: cpu_display_teardown_race_stress did not observe a stream_id within timeout")
		quit(1)
		return

	print("[CamBANG][RaceStress] stream_id=%d latched; starting background hammer thread" % stream_id)

	_stress_thread = Thread.new()
	_stress_thread.start(_hammer_display_view.bind(stream_id))

	var waited_msec := 0
	while waited_msec < FIRST_CALL_WAIT_MAX_MSEC:
		_first_call_mutex.lock()
		var done := _first_call_done
		_first_call_mutex.unlock()
		if done:
			break
		await process_frame
		waited_msec += 16

	if waited_msec >= FIRST_CALL_WAIT_MAX_MSEC:
		print("[CamBANG][RaceStress] WARNING: background thread never completed a get_display_view() call within timeout; quitting anyway")
	else:
		print("[CamBANG][RaceStress] background thread completed its first get_display_view() call; quitting now while its scheduled render-thread callback is very likely still pending")
	quit(0)


func _latch_stream_id() -> int:
	for _frame_index in range(BOOTSTRAP_MAX_FRAMES):
		var snapshot = CamBANGServer.get_state_snapshot()
		if snapshot != null:
			var streams: Array = snapshot.get("streams", [])
			if not streams.is_empty():
				var stream_d: Dictionary = streams[0]
				var candidate := int(stream_d.get("stream_id", 0))
				if candidate > 0:
					return candidate
		await process_frame
	return 0


func _hammer_display_view(stream_id: int) -> void:
	var deadline_usec := Time.get_ticks_usec() + int(HAMMER_MAX_SECONDS * 1000000.0)
	var iterations := 0
	while iterations < HAMMER_MAX_ITERATIONS and Time.get_ticks_usec() < deadline_usec:
		var stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if stream_result != null:
			# Discard immediately: destroying the returned wrapper right away
			# also stresses SharedLiveCpuTextureRidState's destructor-triggered
			# release path (Claim 1) concurrently with the enqueue/schedule
			# path (Claim 3), from this same non-render, non-main thread.
			var _display_view = stream_result.get_display_view()
		iterations += 1
		if iterations == 1:
			# Signal the main thread as soon as the first call returns, so it
			# can quit() while this call's scheduled render-thread callback
			# (if any) is very likely still pending -- see the synchronization
			# note above _initialize().
			_first_call_mutex.lock()
			_first_call_done = true
			_first_call_mutex.unlock()
