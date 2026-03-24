extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 7000
const FIRST_PUBLISH_TIMEOUT_MS := 2000
const OBSERVATION_WINDOW_MS := 1200
const MIN_PUBLISHES := 3
const HEARTBEAT_INTERVAL_SEC := 1.0
const TRACE_PATH := "user://61_tick_bounded_coalescing_trace.log"

var _done := false
var _quit_requested := false
var _timer: Timer
var _first_publish_timer: Timer
var _observation_timer: Timer
var _dev_node: CamBANGDevNode
var _frame_index := 0
var _signal_count_this_tick := 0
var _publish_count := 0
var _finished := false
var _last_gen := -1
var _last_version := -1
var _last_topology_version := -1
var _last_topology_sig := ""
var _observation_started := false
var _heartbeat_elapsed := 0.0


func _ready() -> void:
	await _run_verifier()


func _run_verifier() -> void:
	set_process(true)
	_init_trace_file()
	_trace("INFO: _ready reached (tick-bounded coalescing verifier)")
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")
	print("RUN: godot tick-bounded coalescing abuse")

	_timer = Timer.new()
	_timer.process_mode = Node.PROCESS_MODE_ALWAYS
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()

	_first_publish_timer = Timer.new()
	_first_publish_timer.process_mode = Node.PROCESS_MODE_ALWAYS
	_first_publish_timer.one_shot = true
	_first_publish_timer.wait_time = float(FIRST_PUBLISH_TIMEOUT_MS) / 1000.0
	add_child(_first_publish_timer)
	_first_publish_timer.timeout.connect(_on_first_publish_timeout)
	_first_publish_timer.start()

	_observation_timer = Timer.new()
	_observation_timer.process_mode = Node.PROCESS_MODE_ALWAYS
	_observation_timer.one_shot = true
	_observation_timer.wait_time = float(OBSERVATION_WINDOW_MS) / 1000.0
	add_child(_observation_timer)
	_observation_timer.timeout.connect(_on_observation_timeout)

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)

	CamBANGServer.start()
	_dev_node = CamBANGDevNode.new()
	add_child(_dev_node)
	call_deferred("_start_scenario_after_ready")
	_trace("INFO: frame-lifetime loop entered (tick-bounded coalescing verifier)")
	while not _finished:
		await get_tree().process_frame
	_trace("INFO: frame-lifetime loop exited (tick-bounded coalescing verifier)")


func _start_scenario_after_ready() -> void:
	_trace("INFO: deferred scenario start invoked (tick-bounded coalescing verifier)")
	if _done:
		return
	if _dev_node == null or not is_instance_valid(_dev_node):
		_fail("FAIL: dev node unavailable before scenario start")
		return
	if not _dev_node.start_scenario("publication_coalescing"):
		_fail("FAIL: unable to start publication_coalescing scenario")


func _process(_delta: float) -> void:
	if _done:
		return
	_heartbeat_elapsed += _delta
	if _heartbeat_elapsed >= HEARTBEAT_INTERVAL_SEC:
		_heartbeat_elapsed = 0.0
		_trace("INFO: process heartbeat (tick-bounded coalescing verifier)")
	if _frame_index == 0:
		print("INFO: process loop active (tick-bounded coalescing verifier)")
	if _signal_count_this_tick > 1:
		_fail("FAIL: more than one state_published emission observed in one Godot tick")
		return
	_signal_count_this_tick = 0
	_frame_index += 1


func _on_timeout() -> void:
	print("INFO: overall timeout fired (tick-bounded coalescing verifier)")
	_fail("FAIL: tick-bounded coalescing abuse timed out before reaching deterministic completion")


func _on_first_publish_timeout() -> void:
	if _done:
		return
	print("INFO: first-publish timeout fired with publish_count=%d" % _publish_count)
	if _publish_count > 0:
		return
	print("INFO: first-publish timeout decision=FAIL (no publishes observed)")
	_fail("FAIL: no state_published callback observed during startup window")


func _on_observation_timeout() -> void:
	if _done:
		return
	_trace("INFO: observation-timeout fired (tick-bounded coalescing verifier)")
	print("INFO: observation timeout fired with publish_count=%d (min=%d)" % [_publish_count, MIN_PUBLISHES])
	if _publish_count < MIN_PUBLISHES:
		print("INFO: observation timeout decision=FAIL (insufficient publishes)")
		_fail("FAIL: insufficient publishes observed for coalescing checks")
		return
	print("INFO: observation timeout decision=PASS")
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
	_trace("INFO: publish_count=%d (tick-bounded coalescing verifier)" % _publish_count)
	print("INFO: publish_count=%d (gen=%d version=%d topology_version=%d)" % [_publish_count, gen, version, topology_version])

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot received in state_published handler")
		return

	if _last_gen == -1:
		print("INFO: first publish observed in tick-bounded coalescing verifier")
		_trace("INFO: first publish observed (tick-bounded coalescing verifier)")
		_start_observation_window()
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


func _start_observation_window() -> void:
	if _done:
		return
	if _observation_started:
		return
	if _observation_timer == null or not is_instance_valid(_observation_timer):
		return
	_observation_started = true
	_trace("INFO: observation window started (tick-bounded coalescing verifier)")
	print("INFO: observation window started for tick-bounded coalescing verifier")
	_observation_timer.start()


func _ok(msg: String) -> void:
	if _done:
		return
	_done = true
	_finished = true
	_trace("INFO: OK path reached (tick-bounded coalescing verifier)")
	print(msg)
	_cleanup_and_quit(0)


func _fail(msg: String) -> void:
	if _done:
		return
	_done = true
	_finished = true
	_trace("INFO: FAIL path reached (tick-bounded coalescing verifier)")
	push_error(msg)
	print(msg)
	_cleanup_and_quit(1)


func _cleanup_and_quit(code: int) -> void:
	print("INFO: cleanup_and_quit called with code=%d" % code)
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
	if not _finished:
		print("INFO: _quit_next_frame ignored because verifier is not finished")
		return
	for _i in range(QUIT_FLUSH_FRAMES):
		await get_tree().process_frame
	_trace("INFO: quit requested code=%d (tick-bounded coalescing verifier)" % code)
	print("INFO: quit requested code=%d (tick-bounded coalescing verifier)" % code)
	get_tree().quit(code)


func _exit_tree() -> void:
	_trace("INFO: exit_tree reached (tick-bounded coalescing verifier)")
	print("INFO: exit_tree reached (tick-bounded coalescing verifier)")


func _init_trace_file() -> void:
	var file := FileAccess.open(TRACE_PATH, FileAccess.WRITE)
	if file == null:
		return
	file.store_line("=== tick-bounded coalescing trace start ===")
	file.flush()


func _trace(msg: String) -> void:
	print(msg)
	var file := FileAccess.open(TRACE_PATH, FileAccess.READ_WRITE)
	if file == null:
		return
	file.seek_end()
	file.store_line(msg)
	file.flush()
