extends Node

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

	if not CamBANGServer.state_published.is_connected(_on_initial_publish):
		CamBANGServer.state_published.connect(_on_initial_publish)

	CamBANGServer.start()


func _on_timeout() -> void:
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
	await get_tree().process_frame
	get_tree().quit(code)
