extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 7000
const FIRST_PUBLISH_TIMEOUT_MS := 2000
const OBSERVATION_WINDOW_MS := 1800
const MIN_UPDATES := 4

var _done := false
var _quit_requested := false
var _timer: Timer
var _first_publish_timer: Timer
var _observation_timer: Timer
var _dev_node: CamBANGDevNode
var _cached_snapshot: Dictionary
var _cached_version := -1
var _cached_stream_count := -1
var _publish_count := 0
var _observation_started := false
var _dev_node_state_logged_after_four := false


func _ready() -> void:
	set_process(true)
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")
	print("RUN: godot snapshot polling/immutability abuse")

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()

	_first_publish_timer = Timer.new()
	_first_publish_timer.one_shot = true
	_first_publish_timer.wait_time = float(FIRST_PUBLISH_TIMEOUT_MS) / 1000.0
	add_child(_first_publish_timer)
	_first_publish_timer.timeout.connect(_on_first_publish_timeout)
	_first_publish_timer.start()

	_observation_timer = Timer.new()
	_observation_timer.one_shot = true
	_observation_timer.wait_time = float(OBSERVATION_WINDOW_MS) / 1000.0
	add_child(_observation_timer)
	_observation_timer.timeout.connect(_on_observation_timeout)

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)

	CamBANGServer.start()
	_dev_node = CamBANGDevNode.new()
	if not _dev_node.tree_exiting.is_connected(_on_dev_node_tree_exiting):
		_dev_node.tree_exiting.connect(_on_dev_node_tree_exiting)
	if not _dev_node.tree_exited.is_connected(_on_dev_node_tree_exited):
		_dev_node.tree_exited.connect(_on_dev_node_tree_exited)
	add_child(_dev_node)
	call_deferred("_start_scenario_after_ready")


func _start_scenario_after_ready() -> void:
	if _done:
		return
	if _dev_node == null or not is_instance_valid(_dev_node):
		_fail("FAIL: dev node unavailable before scenario start")
		return
	if not _dev_node.start_scenario("stream_lifecycle_versions"):
		_fail("FAIL: unable to start stream_lifecycle_versions scenario")


func _process(_delta: float) -> void:
	if _done:
		return
	var polled = CamBANGServer.get_state_snapshot()
	if polled == null:
		return

	if _cached_version >= 0:
		if int(_cached_snapshot.get("version", -1)) != _cached_version:
			_fail("FAIL: cached snapshot mutated after newer publishes")
			return
		var cached_streams: Array = _cached_snapshot.get("streams", [])
		if cached_streams.size() != _cached_stream_count:
			_fail("FAIL: cached snapshot stream array mutated")
			return


func _on_timeout() -> void:
	_fail("FAIL: snapshot polling/immutability abuse timed out before reaching deterministic completion")


func _on_first_publish_timeout() -> void:
	if _done:
		return
	if _publish_count > 0:
		return
	_fail("FAIL: no state_published callback observed during startup window")


func _on_observation_timeout() -> void:
	if _done:
		return
	print("INFO: observation-timeout fired (snapshot polling/immutability verifier)")
	if _publish_count < MIN_UPDATES:
		_fail("FAIL: insufficient publishes for polling/immutability abuse")
		return
	_ok("OK: godot snapshot polling/immutability abuse PASS")


func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	if _done:
		return
	_publish_count += 1
	print("INFO: publish_count=%d" % _publish_count)

	if _publish_count == 4 and not _dev_node_state_logged_after_four:
		_dev_node_state_logged_after_four = true
		var valid := _dev_node != null and is_instance_valid(_dev_node)
		var inside := valid and _dev_node.is_inside_tree()
		print("INFO: publish_count reached 4; dev_node valid=%s inside_tree=%s" % [str(valid), str(inside)])

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot inside state_published during polling abuse")
		return

	if _cached_version == -1:
		print("INFO: first publish observed")
		_start_observation_window()
		_cached_snapshot = snapshot
		_cached_version = int(snapshot.get("version", -1))
		var cached_streams: Array = snapshot.get("streams", [])
		_cached_stream_count = cached_streams.size()


func _start_observation_window() -> void:
	if _done:
		return
	if _observation_started:
		return
	if _observation_timer == null or not is_instance_valid(_observation_timer):
		return
	_observation_started = true
	print("INFO: observation window started")
	_observation_timer.start()


func _on_dev_node_tree_exiting() -> void:
	print("INFO: dev node tree_exiting observed")


func _on_dev_node_tree_exited() -> void:
	print("INFO: dev node tree_exited observed")


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
	if _first_publish_timer != null and is_instance_valid(_first_publish_timer):
		_first_publish_timer.stop()
	if _observation_timer != null and is_instance_valid(_observation_timer):
		_observation_timer.stop()
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		call_deferred("_quit_next_frame", code)


func _quit_next_frame(code: int) -> void:
	for _i in range(QUIT_FLUSH_FRAMES):
		await get_tree().process_frame
	print("INFO: quit requested code=%d" % code)
	get_tree().quit(code)


func _exit_tree() -> void:
	print("INFO: exit_tree reached")
