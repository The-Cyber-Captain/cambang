extends Node

const TIMEOUT_MS := 3000

const PHASE_WAIT_INITIAL_PUBLISH := 0
const PHASE_RESTART_DEFERRED_PENDING := 1
const PHASE_WAIT_POST_RESTART_PUBLISH := 2
const PHASE_DONE := 3

var _phase := PHASE_WAIT_INITIAL_PUBLISH
var _done := false
var _quit_requested := false
var _timeout_timer: Timer
var _initial_gen := -1


func _ready() -> void:
	CamBANGServer.stop()

	_timeout_timer = Timer.new()
	_timeout_timer.one_shot = true
	_timeout_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timeout_timer)
	_timeout_timer.timeout.connect(_on_timeout)
	_timeout_timer.start()

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)

	CamBANGServer.start()


func _exit_tree() -> void:
	if _timeout_timer != null and is_instance_valid(_timeout_timer):
		if _timeout_timer.timeout.is_connected(_on_timeout):
			_timeout_timer.timeout.disconnect(_on_timeout)

	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)

	if not _done:
		print("FAIL: scene exited before reaching terminal verification state")


func _on_timeout() -> void:
	_finish_fail("FAIL: timed out waiting for restart NIL-before-baseline verification")


func _on_state_published(gen: int, _version: int, _topology_version: int) -> void:
	if _done:
		return

	match _phase:
		PHASE_WAIT_INITIAL_PUBLISH:
			var initial_snapshot = CamBANGServer.get_state_snapshot()
			if initial_snapshot == null:
				_finish_fail("FAIL: expected non-NIL snapshot after initial baseline publish")
				return

			_initial_gen = gen
			_phase = PHASE_RESTART_DEFERRED_PENDING
			call_deferred("_perform_restart")

		PHASE_RESTART_DEFERRED_PENDING:
			_finish_fail("FAIL: received state_published before deferred restart completed")

		PHASE_WAIT_POST_RESTART_PUBLISH:
			var restart_snapshot = CamBANGServer.get_state_snapshot()
			if restart_snapshot == null:
				_finish_fail("FAIL: expected non-NIL snapshot after restart baseline publish")
				return

			if gen != _initial_gen + 1:
				_finish_fail(
					"FAIL: expected post-restart generation %d, got %d" % [_initial_gen + 1, gen]
				)
				return

			_finish_ok("OK: restart NIL-before-baseline verified")

		PHASE_DONE:
			return


func _perform_restart() -> void:
	if _done:
		return

	if _phase != PHASE_RESTART_DEFERRED_PENDING:
		_finish_fail("FAIL: deferred restart called in unexpected phase")
		return

	CamBANGServer.stop()

	var after_stop = CamBANGServer.get_state_snapshot()
	if after_stop != null:
		_finish_fail("FAIL: expected NIL snapshot after stop during restart sequence")
		return

	CamBANGServer.start()

	var pre_publish = CamBANGServer.get_state_snapshot()
	if pre_publish != null:
		_finish_fail("FAIL: expected NIL snapshot before first post-restart publish")
		return

	_phase = PHASE_WAIT_POST_RESTART_PUBLISH


func _finish_ok(msg: String) -> void:
	if _done:
		return

	_done = true
	_phase = PHASE_DONE
	print(msg)

	if _timeout_timer != null and is_instance_valid(_timeout_timer):
		_timeout_timer.stop()

	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)

	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		call_deferred("_quit_next_frame", 0)


func _finish_fail(msg: String) -> void:
	if _done:
		return

	_done = true
	_phase = PHASE_DONE
	push_error(msg)
	print(msg)

	if _timeout_timer != null and is_instance_valid(_timeout_timer):
		_timeout_timer.stop()

	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)

	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		call_deferred("_quit_next_frame", 1)


func _quit_next_frame(code: int) -> void:
	await get_tree().process_frame
	get_tree().quit(code)
