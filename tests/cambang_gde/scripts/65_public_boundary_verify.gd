extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 5000

const PHASE_WAIT_FIRST_BASELINE := 0
const PHASE_RESTARTING := 1
const PHASE_WAIT_SECOND_BASELINE := 2
const PHASE_DONE := 3

var _done := false
var _quit_requested := false
var _phase := PHASE_WAIT_FIRST_BASELINE
var _timer: Timer
var _first_gen := -1


func _ready() -> void:
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")
	print("RUN: godot public boundary verify")
	_snapshot_diag("ready_before_first_baseline")

	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: snapshot must be NIL before start/baseline")
		return

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)

	CamBANGServer.start()
	_snapshot_diag("ready_after_start_before_first_publish")

	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: snapshot must remain NIL after start() until first Godot-visible baseline publish")


func _on_timeout() -> void:
	_fail("FAIL: public boundary verify timed out")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

	_snapshot_diag("on_state_published", gen, version, topology_version)
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: get_state_snapshot() returned NIL inside state_published handler")
		return
	if typeof(snapshot) != TYPE_DICTIONARY:
		_fail("FAIL: snapshot in handler is not a Dictionary")
		return

	var d: Dictionary = snapshot
	if int(d.get("gen", -1)) != gen:
		_fail("FAIL: handler gen does not match synchronous snapshot gen")
		return
	if int(d.get("version", -1)) != version:
		_fail("FAIL: handler version does not match synchronous snapshot version")
		return
	if int(d.get("topology_version", -1)) != topology_version:
		_fail("FAIL: handler topology_version does not match synchronous snapshot topology_version")
		return

	match _phase:
		PHASE_WAIT_FIRST_BASELINE:
			if version != 0 or topology_version != 0:
				_fail("FAIL: first Godot-visible publish of generation must be baseline (version=0, topology_version=0)")
				return

			_first_gen = gen
			_phase = PHASE_RESTARTING
			call_deferred("_restart_and_assert_nil")

		PHASE_RESTARTING:
			# Ignore any in-flight publishes until restart assertions have completed.
			_snapshot_diag("ignoring_publish_during_restart", gen, version, topology_version)
			return

		PHASE_WAIT_SECOND_BASELINE:
			print("INFO: restart_generation_check phase=%s expected=%d actual=%d condition=(actual == expected + 1)" % [
				_phase_name(_phase),
				_first_gen,
				gen
			])
			if gen != _first_gen + 1:
				_fail("FAIL: restart must advance generation exactly by one")
				return
			if version != 0 or topology_version != 0:
				_fail("FAIL: first publish of restarted generation must be baseline")
				return

			_ok("OK: godot public boundary verify PASS")

		PHASE_DONE:
			return


func _restart_and_assert_nil() -> void:
	if _done:
		return

	_snapshot_diag("restart_before_stop")
	CamBANGServer.stop()
	_snapshot_diag("restart_after_stop")
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: get_state_snapshot() must be NIL after completed stop()")
		return

	_snapshot_diag("restart_before_start")
	CamBANGServer.start()
	_snapshot_diag("restart_after_start_before_first_publish")
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: stale prior-generation snapshot leaked before next generation baseline")
		return
	_phase = PHASE_WAIT_SECOND_BASELINE


func _ok(msg: String) -> void:
	if _done:
		return
	_done = true
	_phase = PHASE_DONE
	print(msg)
	_cleanup_and_quit(0)


func _fail(msg: String) -> void:
	if _done:
		return
	_done = true
	_phase = PHASE_DONE
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
	for _i in range(QUIT_FLUSH_FRAMES):
		await get_tree().process_frame
	get_tree().quit(code)


func _phase_name(p: int) -> String:
	match p:
		PHASE_WAIT_FIRST_BASELINE:
			return "wait_first_baseline"
		PHASE_RESTARTING:
			return "restarting"
		PHASE_WAIT_SECOND_BASELINE:
			return "wait_second_baseline"
		PHASE_DONE:
			return "done"
		_:
			return "unknown"


func _snapshot_diag(label: String, sig_gen: int = -999, sig_version: int = -999, sig_topology_version: int = -999) -> void:
	var snap = CamBANGServer.get_state_snapshot()
	var nil_snap := snap == null
	var snap_gen := "nil"
	var snap_version := "nil"
	var snap_topology := "nil"
	if not nil_snap and typeof(snap) == TYPE_DICTIONARY:
		snap_gen = str(int(snap.get("gen", -1)))
		snap_version = str(int(snap.get("version", -1)))
		snap_topology = str(int(snap.get("topology_version", -1)))
	print("INFO: boundary_diag step=%s phase=%s signal=(gen=%d version=%d topology_version=%d) snapshot_nil=%s snapshot=(gen=%s version=%s topology_version=%s)" % [
		label,
		_phase_name(_phase),
		sig_gen,
		sig_version,
		sig_topology_version,
		str(nil_snap),
		snap_gen,
		snap_version,
		snap_topology
	])
