extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 5000

var _done := false
var _quit_requested := false
var _timer: Timer
var _initial_gen := -1
var _parent_for_child_exit_watch: Node


func _ready() -> void:
	print("INFO: verifier node name=%s path=%s" % [name, _self_path_desc()])
	print("INFO: verifier parent=%s owner=%s" % [_node_ref_desc(get_parent()), _node_ref_desc(owner)])
	_connect_parent_child_exit_watch()
	_log_current_scene_identity("ready")

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
	print("INFO: cleanup_and_quit pre-quit code=%d parent=%s current_scene=%s" % [
		code,
		_node_ref_desc(get_parent()),
		_current_scene_desc()
	])
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


func _exit_tree() -> void:
	_log_current_scene_identity("exit_tree")


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


func _node_ref_desc(n: Node) -> String:
	if n == null:
		return "null"
	if not is_instance_valid(n):
		return "invalid"
	if n.is_inside_tree():
		return "%s@%s" % [str(n.name), str(n.get_path())]
	return "%s@<not_in_tree>" % str(n.name)


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
