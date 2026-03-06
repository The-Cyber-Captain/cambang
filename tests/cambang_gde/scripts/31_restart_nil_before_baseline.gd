extends Node

var _phase := 0
var _saw_initial_publish := false
var _saw_restart_publish := false
var _done := false

func _finish_ok(msg: String) -> void:
	if _done:
		return
	_done = true
	print(msg)
	CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()
	call_deferred("_quit_with_code", 0)

func _finish_fail(msg: String) -> void:
	if _done:
		return
	_done = true
	print(msg)
	push_error(msg)
	CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()
	call_deferred("_quit_with_code", 1)

func _quit_with_code(code: int) -> void:
	get_tree().quit(code)

func _ready() -> void:
	CamBANGServer.state_published.connect(_on_state_published)
	CamBANGServer.start()

func _process(_dt: float) -> void:
	if _phase == 2 and not _saw_restart_publish:
		# After restart and before first new publish, snapshot must be NIL.
		var pre = CamBANGServer.get_state_snapshot()
		if pre != null:
			_finish_fail("FAIL: expected NIL snapshot before first post-restart publish")
			return
		_phase = 3

func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	if not _saw_initial_publish:
		_saw_initial_publish = true
		CamBANGServer.stop()
		_phase = 2
		CamBANGServer.start()
		return

	if not _saw_restart_publish:
		_saw_restart_publish = true
		# Once restarted publish arrives, snapshot must be non-NIL.
		var s = CamBANGServer.get_state_snapshot()
		if s == null:
			_finish_fail("FAIL: expected non-NIL snapshot after restart baseline publish")
			return
		_finish_ok("OK: restart NIL-before-baseline verified")
