extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 5000

var _done := false
var _quit_requested := false
var _timer: Timer
var _initial_gen := -1


func _ready() -> void:
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()
	_log_timer_state("ready:hard_timeout_started", _timer)

	if not CamBANGServer.state_published.is_connected(_on_initial_publish):
		CamBANGServer.state_published.connect(_on_initial_publish)

	CamBANGServer.start()


func _on_timeout() -> void:
	_log_timer_state("timeout_fired:hard_timeout", _timer)
	_fail("FAIL: restart_boundary_abuse timed out")


func _on_initial_publish(gen: int, _version: int, _topology_version: int) -> void:
	if _done:
		return
	CamBANGServer.state_published.disconnect(_on_initial_publish)

	if CamBANGServer.get_state_snapshot() == null:
		_fail("FAIL: expected non-NIL snapshot at initial publish")
		return

	_initial_gen = gen
	call_deferred("_do_restart_assertions")


func _do_restart_assertions() -> void:
	if _done:
		return

	CamBANGServer.stop()
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: stale snapshot visible after stop")
		return

	if not CamBANGServer.state_published.is_connected(_on_post_restart_publish):
		CamBANGServer.state_published.connect(_on_post_restart_publish)

	CamBANGServer.start()
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: pre-baseline snapshot visible after restart")
		return


func _on_post_restart_publish(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

	if CamBANGServer.get_state_snapshot() == null:
		_fail("FAIL: NIL snapshot at post-restart first publish")
		return

	if gen != _initial_gen + 1:
		_fail("FAIL: generation did not advance exactly once across restart")
		return
	if version != 0:
		_fail("FAIL: first publish after restart must have version=0")
		return
	if topology_version != 0:
		_fail("FAIL: first publish after restart must have topology_version=0")
		return

	_ok("OK: godot restart boundary abuse PASS")


func _ok(msg: String) -> void:
	if _done:
		return
	_done = true
	print(msg)
	_cleanup_and_quit(0)


func _fail(msg: String) -> void:
	if _done:
		return
	_done = true
	push_error(msg)
	print(msg)
	_cleanup_and_quit(1)


func _cleanup_and_quit(code: int) -> void:
	if _timer != null and is_instance_valid(_timer):
		_timer.stop()
	if CamBANGServer.state_published.is_connected(_on_initial_publish):
		CamBANGServer.state_published.disconnect(_on_initial_publish)
	if CamBANGServer.state_published.is_connected(_on_post_restart_publish):
		CamBANGServer.state_published.disconnect(_on_post_restart_publish)
	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		call_deferred("_quit_next_frame", code)


func _quit_next_frame(code: int) -> void:
	for _i in range(QUIT_FLUSH_FRAMES):
		await get_tree().process_frame
	get_tree().quit(code)


func _log_timer_state(label: String, t: Timer) -> void:
	var valid := t != null and is_instance_valid(t)
	var inside := valid and t.is_inside_tree()
	var parent_desc := "null"
	if valid:
		var p := t.get_parent()
		if p != null and is_instance_valid(p):
			if p.is_inside_tree():
				parent_desc = "%s@%s" % [p.name, str(p.get_path())]
			else:
				parent_desc = "%s@<not_in_tree>" % str(p.name)
		else:
			parent_desc = "invalid_parent"
	var stopped := "n/a"
	var time_left := "n/a"
	var process_mode := "n/a"
	if valid:
		stopped = str(t.is_stopped())
		time_left = str(t.time_left)
		process_mode = str(t.process_mode)
	print("INFO: timer_state %s valid=%s inside_tree=%s parent=%s stopped=%s time_left=%s process_mode=%s" % [
		label,
		str(valid),
		str(inside),
		parent_desc,
		stopped,
		time_left,
		process_mode
	])
