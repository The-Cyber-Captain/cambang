extends Node

const TIMEOUT_MS := 7000
const MIN_PUBLISHES := 3

var _done := false
var _quit_requested := false
var _timer: Timer
var _dev_node: CamBANGDevNode
var _frame_index := 0
var _signal_count_this_tick := 0
var _publish_count := 0
var _last_gen := -1
var _last_version := -1
var _last_topology_version := -1
var _last_topology_sig := ""


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

	if not _dev_node.start_scenario("publication_coalescing"):
		_fail("FAIL: unable to start publication_coalescing scenario")


func _process(_delta: float) -> void:
	if _done:
		return
	if _signal_count_this_tick > 1:
		_fail("FAIL: more than one state_published emission observed in one Godot tick")
		return
	_signal_count_this_tick = 0
	_frame_index += 1


func _on_timeout() -> void:
	if _publish_count < MIN_PUBLISHES:
		_fail("FAIL: insufficient publishes observed for coalescing checks")
		return
	_ok("OK: godot tick-bounded coalescing abuse PASS")


func _topology_signature(snapshot: Dictionary) -> String:
	var devices: Array = snapshot.get("devices", [])
	var streams: Array = snapshot.get("streams", [])
	var device_keys: Array[String] = []
	for d in devices:
		device_keys.append("%s:%s" % [str(d.get("instance_id", 0)), str(d.get("phase", 0))])
	device_keys.sort()
	var stream_keys: Array[String] = []
	for s in streams:
		stream_keys.append("%s:%s:%s" % [str(s.get("stream_id", 0)), str(s.get("device_instance_id", 0)), str(s.get("phase", 0))])
	stream_keys.sort()
	return "D[%s]|S[%s]" % [";".join(device_keys), ";".join(stream_keys)]


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

	_signal_count_this_tick += 1
	_publish_count += 1

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot received in state_published handler")
		return

	if _last_gen == -1:
		_last_gen = gen
		_last_version = version
		_last_topology_version = topology_version
		_last_topology_sig = _topology_signature(snapshot)
		return

	if gen != _last_gen:
		_fail("FAIL: generation changed unexpectedly during coalescing abuse")
		return
	if version != _last_version + 1:
		_fail("FAIL: version is not contiguous within generation")
		return

	var topo_sig := _topology_signature(snapshot)
	var observed_change := topo_sig != _last_topology_sig
	if observed_change:
		if topology_version != _last_topology_version + 1:
			_fail("FAIL: topology_version did not increment on observed topology change")
			return
	else:
		if topology_version != _last_topology_version:
			_fail("FAIL: topology_version changed without observed topology change")
			return

	_last_version = version
	_last_topology_version = topology_version
	_last_topology_sig = topo_sig


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
