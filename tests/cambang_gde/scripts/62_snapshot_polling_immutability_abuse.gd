extends Node

const TIMEOUT_MS := 7000
const MIN_UPDATES := 4

var _done := false
var _quit_requested := false
var _timer: Timer
var _dev_node: CamBANGDevNode
var _cached_snapshot: Dictionary
var _cached_version := -1
var _cached_stream_count := -1
var _publish_count := 0


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
	_dev_node = CamBANGDevNode.new()
	add_child(_dev_node)

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
	if _publish_count < MIN_UPDATES:
		_fail("FAIL: insufficient publishes for polling/immutability abuse")
		return
	_ok("OK: godot snapshot polling/immutability abuse PASS")


func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	if _done:
		return
	_publish_count += 1
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot inside state_published during polling abuse")
		return

	if _cached_version == -1:
		_cached_snapshot = snapshot
		_cached_version = int(snapshot.get("version", -1))
		var cached_streams: Array = snapshot.get("streams", [])
		_cached_stream_count = cached_streams.size()


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
