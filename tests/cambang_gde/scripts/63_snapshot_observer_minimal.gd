extends Node

const TIMEOUT_MS := 3000

var _done := false
var _quit_requested := false
var _timer: Timer


func _ready() -> void:
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)
	CamBANGServer.start()


func _exit_tree() -> void:
	if _timer != null and is_instance_valid(_timer):
		if _timer.timeout.is_connected(_on_timeout):
			_timer.timeout.disconnect(_on_timeout)
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()


func _on_timeout() -> void:
	_fail("FAIL: snapshot observer timed out waiting for baseline publish")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot in observer publish callback")
		return

	var streams: Array = snapshot.get("streams", [])
	var frames_received := 0
	var frames_delivered := 0
	var frames_dropped := 0
	var stream_errors := 0
	for stream in streams:
		frames_received += int(stream.get("frames_received", 0))
		frames_delivered += int(stream.get("frames_delivered", 0))
		frames_dropped += int(stream.get("frames_dropped", 0))
		if int(stream.get("stop_reason", 0)) != 0:
			stream_errors += 1

	print(
		"OBS: gen=", gen,
		" version=", version,
		" topology_version=", topology_version,
		" devices=", int((snapshot.get("devices", []) as Array).size()),
		" streams=", streams.size(),
		" frames_received=", frames_received,
		" frames_delivered=", frames_delivered,
		" frames_dropped=", frames_dropped,
		" stream_errors=", stream_errors
	)

	_ok("OK: godot snapshot observer minimal PASS")


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
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		call_deferred("_quit_next_frame", code)


func _quit_next_frame(code: int) -> void:
	await get_tree().process_frame
	get_tree().quit(code)
