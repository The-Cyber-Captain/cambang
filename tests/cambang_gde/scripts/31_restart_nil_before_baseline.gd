extends Node

const TIMEOUT_MS := 3000

var _done := false
var _saw_initial_publish := false
var _saw_restart_publish := false
var _timeout_timer: Timer


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


func _on_timeout() -> void:
	_finish_fail("FAIL: timed out waiting for restart NIL-before-baseline verification")


func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	if _done:
		return

	if not _saw_initial_publish:
		_saw_initial_publish = true
		call_deferred("_perform_restart")
		return

	if not _saw_restart_publish:
		_saw_restart_publish = true

		var s = CamBANGServer.get_state_snapshot()
		if s == null:
			_finish_fail("FAIL: expected non-NIL snapshot after restart baseline publish")
			return

		_finish_ok("OK: restart NIL-before-baseline verified")


func _perform_restart() -> void:
	if _done:
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


func _finish_ok(msg: String) -> void:
	if _done:
		return

	_done = true
	print(msg)

	if _timeout_timer != null and is_instance_valid(_timeout_timer):
		_timeout_timer.stop()

	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)

	CamBANGServer.stop()
	call_deferred("_quit_with_code", 0)


func _finish_fail(msg: String) -> void:
	if _done:
		return

	_done = true
	push_error(msg)
	print(msg)

	if _timeout_timer != null and is_instance_valid(_timeout_timer):
		_timeout_timer.stop()

	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)

	CamBANGServer.stop()
	call_deferred("_quit_with_code", 1)


func _quit_with_code(code: int) -> void:
	get_tree().quit(code)
