extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 7000
const FIRST_PUBLISH_TIMEOUT_MS := 2000
const OBSERVATION_WINDOW_MS := 1200
const MIN_PUBLISHES := 3

var _done := false
var _quit_requested := false
var _timer: Timer
var _first_publish_timer: Timer
var _observation_timer: Timer
var _dev_node: CamBANGDevNode
var _signal_count_this_tick := 0
var _publish_count := 0
var _last_gen := -1
var _last_version := -1
var _last_topology_version := -1
var _last_topology_sig := ""
var _observation_started := false
var _dev_node_state_logged_after_four := false
var _parent_for_child_exit_watch: Node
var _ready_ticks_msec := 0


func _ready() -> void:
	_ready_ticks_msec = Time.get_ticks_msec()
	set_process(true)
	print("INFO: verifier node name=%s path=%s" % [name, _self_path_desc()])
	print("INFO: verifier parent=%s owner=%s" % [_node_ref_desc(get_parent()), _node_ref_desc(owner)])
	_connect_parent_child_exit_watch()
	_log_current_scene_identity("ready")
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")
	print("RUN: godot tick-bounded coalescing abuse")
	_log_runtime_checkpoint("ready_enter")

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()
	_log_timer_state("ready:hard_timeout_started", _timer)

	_first_publish_timer = Timer.new()
	_first_publish_timer.one_shot = true
	_first_publish_timer.wait_time = float(FIRST_PUBLISH_TIMEOUT_MS) / 1000.0
	add_child(_first_publish_timer)
	_first_publish_timer.timeout.connect(_on_first_publish_timeout)
	_first_publish_timer.start()
	_log_timer_state("ready:first_publish_timeout_started", _first_publish_timer)

	_observation_timer = Timer.new()
	_observation_timer.one_shot = true
	_observation_timer.wait_time = float(OBSERVATION_WINDOW_MS) / 1000.0
	add_child(_observation_timer)
	_observation_timer.timeout.connect(_on_observation_timeout)
	_log_timer_state("ready:observation_timer_created", _observation_timer)

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)

	CamBANGServer.start()
	_dev_node = CamBANGDevNode.new()
	print("INFO: dev node allocated parent=%s owner=%s" % [_node_ref_desc(_dev_node.get_parent()), _node_ref_desc(_dev_node.owner)])
	if not _dev_node.tree_exiting.is_connected(_on_dev_node_tree_exiting):
		_dev_node.tree_exiting.connect(_on_dev_node_tree_exiting)
	if not _dev_node.tree_exited.is_connected(_on_dev_node_tree_exited):
		_dev_node.tree_exited.connect(_on_dev_node_tree_exited)
	if _dev_node.has_signal("scenario_completed") and not _dev_node.scenario_completed.is_connected(_on_dev_node_scenario_completed):
		_dev_node.scenario_completed.connect(_on_dev_node_scenario_completed)
	add_child(_dev_node)
	print("INFO: dev node after add_child parent=%s owner=%s path=%s" % [_node_ref_desc(_dev_node.get_parent()), _node_ref_desc(_dev_node.owner), _node_path_desc(_dev_node)])
	call_deferred("_start_scenario_after_ready")


func _process(_delta: float) -> void:
	if _done:
		return
	# Frame driver: keep headless main loop actively ticking until verifier completion.
	if _signal_count_this_tick > 1:
		_fail("FAIL: more than one state_published emission observed in one Godot tick")
		return
	_signal_count_this_tick = 0


func _start_scenario_after_ready() -> void:
	if _done:
		return
	if _dev_node == null or not is_instance_valid(_dev_node):
		_fail("FAIL: dev node unavailable before scenario start")
		return
	if not _dev_node.start_scenario("publication_coalescing"):
		_fail("FAIL: unable to start publication_coalescing scenario")


func _on_timeout() -> void:
	_fail("FAIL: tick-bounded coalescing abuse timed out before reaching deterministic completion")


func _on_first_publish_timeout() -> void:
	if _done:
		return
	if _publish_count > 0:
		return
	_fail("FAIL: no state_published callback observed during startup window")


func _on_observation_timeout() -> void:
	if _done:
		return
	print("INFO: observation-timeout fired (tick-bounded coalescing verifier)")
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
	print("INFO: publish_count=%d" % _publish_count)

	if _publish_count == 4 and not _dev_node_state_logged_after_four:
		_dev_node_state_logged_after_four = true
		var valid := _dev_node != null and is_instance_valid(_dev_node)
		var inside := valid and _dev_node.is_inside_tree()
		print("INFO: publish_count reached 4; dev_node valid=%s inside_tree=%s" % [str(valid), str(inside)])
		_log_runtime_checkpoint("publish_count_4")
		_log_timer_state("publish_count_4:hard_timeout", _timer)
		_log_timer_state("publish_count_4:first_publish_timeout", _first_publish_timer)
		_log_timer_state("publish_count_4:observation", _observation_timer)

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot received in state_published handler")
		return

	if _last_gen == -1:
		print("INFO: first publish observed")
		_log_timer_state("first_publish:hard_timeout", _timer)
		_log_timer_state("first_publish:first_publish_timeout", _first_publish_timer)
		_log_timer_state("first_publish:observation", _observation_timer)
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
	print("INFO: observation window started")
	_observation_timer.start()
	_log_timer_state("observation_started:hard_timeout", _timer)
	_log_timer_state("observation_started:first_publish_timeout", _first_publish_timer)
	_log_timer_state("observation_started:observation", _observation_timer)


func _on_dev_node_tree_exiting() -> void:
	_log_runtime_checkpoint("dev_node_tree_exiting")
	var reason := "unknown"
	if _dev_node != null and is_instance_valid(_dev_node) and _dev_node.has_method("get_exit_reason"):
		reason = str(_dev_node.get_exit_reason())
	print("INFO: dev node tree_exiting observed exit_reason=%s parent=%s owner=%s path=%s" % [
		reason,
		_node_ref_desc(_dev_node.get_parent() if _dev_node != null and is_instance_valid(_dev_node) else null),
		_node_ref_desc(_dev_node.owner if _dev_node != null and is_instance_valid(_dev_node) else null),
		_node_path_desc(_dev_node)
	])


func _on_dev_node_tree_exited() -> void:
	_log_runtime_checkpoint("dev_node_tree_exited")
	var reason := "unknown"
	if _dev_node != null and is_instance_valid(_dev_node) and _dev_node.has_method("get_exit_reason"):
		reason = str(_dev_node.get_exit_reason())
	print("INFO: dev node tree_exited observed exit_reason=%s parent=%s owner=%s path=%s" % [
		reason,
		_node_ref_desc(_dev_node.get_parent() if _dev_node != null and is_instance_valid(_dev_node) else null),
		_node_ref_desc(_dev_node.owner if _dev_node != null and is_instance_valid(_dev_node) else null),
		_node_path_desc(_dev_node)
	])


func _on_dev_node_scenario_completed(name: String) -> void:
	print("INFO: dev node scenario_completed observed name=%s" % name)


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
	set_process(false)
	_log_runtime_checkpoint("cleanup_and_quit")
	print("INFO: cleanup_and_quit pre-quit code=%d parent=%s current_scene=%s" % [
		code,
		_node_ref_desc(get_parent()),
		_current_scene_desc()
	])
	print("INFO: cleanup_and_quit code=%d verifier_parent=%s dev_node_valid=%s dev_node_inside_tree=%s server_snapshot_nil=%s" % [
		code,
		_node_ref_desc(get_parent()),
		str(_dev_node != null and is_instance_valid(_dev_node)),
		str(_dev_node != null and is_instance_valid(_dev_node) and _dev_node.is_inside_tree()),
		str(CamBANGServer.get_state_snapshot() == null)
	])
	print("INFO: server signal connected to verifier=%s" % str(CamBANGServer.state_published.is_connected(_on_state_published)))
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
	_log_runtime_checkpoint("quit_next_frame")
	print("INFO: quit requested code=%d" % code)
	get_tree().quit(code)


func _exit_tree() -> void:
	_log_runtime_checkpoint("exit_tree")
	_log_current_scene_identity("exit_tree")
	print("INFO: exit_tree reached parent=%s owner=%s path=%s" % [_node_ref_desc(get_parent()), _node_ref_desc(owner), _self_path_desc()])
	print("INFO: exit_tree server snapshot nil=%s signal connected=%s" % [
		str(CamBANGServer.get_state_snapshot() == null),
		str(CamBANGServer.state_published.is_connected(_on_state_published))
	])


func _notification(what: int) -> void:
	if what == NOTIFICATION_EXIT_TREE:
		print("INFO: _notification NOTIFICATION_EXIT_TREE")
	elif what == NOTIFICATION_PREDELETE:
		print("INFO: _notification NOTIFICATION_PREDELETE")


func _connect_parent_child_exit_watch() -> void:
	var p := get_parent()
	if p == null or not is_instance_valid(p):
		print("INFO: parent child_exiting_tree watch not connected (no parent)")
		return
	_parent_for_child_exit_watch = p
	if not p.child_exiting_tree.is_connected(_on_parent_child_exiting_tree):
		p.child_exiting_tree.connect(_on_parent_child_exiting_tree)
	print("INFO: parent child_exiting_tree watch connected parent=%s" % _node_ref_desc(p))


func _on_parent_child_exiting_tree(child: Node) -> void:
	_log_runtime_checkpoint("parent_child_exiting_tree")
	print("INFO: parent child_exiting_tree parent=%s child=%s is_self=%s current_scene=%s" % [
		_node_ref_desc(_parent_for_child_exit_watch),
		_node_ref_desc(child),
		str(child == self),
		_current_scene_desc()
	])


func _log_current_scene_identity(label: String) -> void:
	var tree := get_tree()
	if tree == null:
		print("INFO: current_scene %s tree=null" % label)
		return
	var cs := tree.current_scene
	print("INFO: current_scene %s self_is_current=%s current=%s" % [
		label,
		str(cs == self),
		_node_ref_desc(cs)
	])


func _current_scene_desc() -> String:
	var tree := get_tree()
	if tree == null:
		return "tree=null"
	return _node_ref_desc(tree.current_scene)


func _self_path_desc() -> String:
	if is_inside_tree():
		return str(get_path())
	return "<not_in_tree>"


func _elapsed_ms() -> int:
	if _ready_ticks_msec <= 0:
		return -1
	return Time.get_ticks_msec() - _ready_ticks_msec


func _log_runtime_checkpoint(label: String) -> void:
	var tree := get_tree()
	var frame := Engine.get_process_frames()
	var paused := "tree=null"
	if tree != null:
		paused = str(tree.paused)
	print("INFO: runtime_checkpoint %s elapsed_ms=%d frame=%d tree_paused=%s current_scene=%s" % [
		label,
		_elapsed_ms(),
		frame,
		paused,
		_current_scene_desc()
	])


func _node_ref_desc(n: Node) -> String:
	if n == null:
		return "null"
	if not is_instance_valid(n):
		return "invalid"
	if not n.is_inside_tree():
		return "%s@<not_in_tree>" % str(n.name)
	return "%s@%s" % [n.name, str(n.get_path())]


func _node_path_desc(n: Node) -> String:
	if n == null:
		return "null"
	if not is_instance_valid(n):
		return "invalid"
	if not n.is_inside_tree():
		return "<not_in_tree>"
	return str(n.get_path())


func _log_timer_state(label: String, t: Timer) -> void:
	var valid := t != null and is_instance_valid(t)
	var inside := valid and t.is_inside_tree()
	var parent_desc := _node_ref_desc(t.get_parent() if valid else null)
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
