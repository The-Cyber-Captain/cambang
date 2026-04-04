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

	if CamBANGServer.has_method("set_provider_mode") or CamBANGServer.has_method("get_provider_mode"):
		_fail("FAIL: legacy string provider-mode API should not be present on CamBANGServer")
		return
	if not CamBANGServer.has_method("set_platform_backed_provider"):
		_fail("FAIL: CamBANGServer.set_platform_backed_provider() missing")
		return
	if not CamBANGServer.has_method("set_synthetic_provider"):
		_fail("FAIL: CamBANGServer.set_synthetic_provider() missing")
		return

	var invalid_role_err := CamBANGServer.set_synthetic_provider(
		9999,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME
	)
	if invalid_role_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: invalid synthetic role must return ERR_INVALID_PARAMETER")
		return

	var invalid_timing_err := CamBANGServer.set_synthetic_provider(
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		9999
	)
	if invalid_timing_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: invalid timing driver must return ERR_INVALID_PARAMETER")
		return

	var set_synth_err := CamBANGServer.set_synthetic_provider(
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME
	)
	if set_synth_err != OK:
		_fail("FAIL: set_synthetic_provider(TIMELINE, VIRTUAL_TIME) rejected while stopped")
		return

	print("RUN: godot public boundary verify")

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

	var busy_reconfig_err := CamBANGServer.set_platform_backed_provider()
	if busy_reconfig_err != ERR_BUSY:
		_fail("FAIL: running reconfiguration must return ERR_BUSY")
		return

	var timeline_stage_err := CamBANGServer.select_builtin_scenario("stream_lifecycle_versions")
	if timeline_stage_err != OK:
		_fail("FAIL: timeline builtin staging should be available after Timeline-role synthetic configuration")
		return

	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: snapshot must remain NIL after start() until first Godot-visible baseline publish")


func _on_timeout() -> void:
	_fail("FAIL: public boundary verify timed out")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

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

	CamBANGServer.stop()
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: get_state_snapshot() must be NIL after completed stop()")
		return

	CamBANGServer.start()
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
