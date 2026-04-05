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
	if CamBANGServer.has_method("set_platform_backed_provider") or CamBANGServer.has_method("set_synthetic_provider"):
		_fail("FAIL: removed stopped-time provider configuration setters should not be present")
		return
	if not CamBANGServer.has_method("start"):
		_fail("FAIL: CamBANGServer.start() missing")
		return
	if CamBANGServer.has_method("start_platform_backed") or CamBANGServer.has_method("start_synthetic") or CamBANGServer.has_method("start_synthetic_with_role") or CamBANGServer.has_method("start_synthetic_with_role_and_timing"):
		_fail("FAIL: compact start-family expected; helper proliferation should be absent")
		return
	if not CamBANGServer.has_method("is_running"):
		_fail("FAIL: CamBANGServer.is_running() missing")
		return
	if not CamBANGServer.has_method("stop"):
		_fail("FAIL: CamBANGServer.stop() missing")
		return
	if not CamBANGServer.has_method("get_state_snapshot"):
		_fail("FAIL: CamBANGServer.get_state_snapshot() missing")
		return
	if not CamBANGServer.has_method("get_active_provider_config"):
		_fail("FAIL: CamBANGServer.get_active_provider_config() missing")
		return
	if not CamBANGServer.has_method("select_builtin_scenario"):
		_fail("FAIL: CamBANGServer.select_builtin_scenario() missing")
		return
	if not CamBANGServer.has_method("load_external_scenario"):
		_fail("FAIL: CamBANGServer.load_external_scenario() missing")
		return
	if not CamBANGServer.has_method("start_scenario") or not CamBANGServer.has_method("stop_scenario"):
		_fail("FAIL: CamBANGServer scenario start/stop API missing")
		return
	if not CamBANGServer.has_method("set_timeline_paused") or not CamBANGServer.has_method("advance_timeline"):
		_fail("FAIL: CamBANGServer timeline control API missing")
		return

	if CamBANGServer.is_running():
		_fail("FAIL: stop() must leave server not running")
		return
	if CamBANGServer.get_active_provider_config() != null:
		_fail("FAIL: stopped server must report NIL active provider config")
		return

	var invalid_role_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, 9999, CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME)
	if invalid_role_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: invalid synthetic role must return ERR_INVALID_PARAMETER")
		return

	var invalid_timing_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE, 9999)
	if invalid_timing_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: invalid timing driver must return ERR_INVALID_PARAMETER")
		return

	var platform_start_err := CamBANGServer.start()
	if platform_start_err != OK:
		_fail("FAIL: start() default platform-backed failed")
		return
	if not CamBANGServer.is_running():
		_fail("FAIL: start() must set is_running() true")
		return
	var platform_cfg = CamBANGServer.get_active_provider_config()
	if typeof(platform_cfg) != TYPE_DICTIONARY:
		_fail("FAIL: running platform-backed start must expose Dictionary active config")
		return
	if int(platform_cfg.get("provider_kind", -1)) != CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED:
		_fail("FAIL: platform-backed active config must report provider_kind=PLATFORM_BACKED")
		return
	if platform_cfg.get("synthetic_role", "not_null") != null or platform_cfg.get("timing_driver", "not_null") != null:
		_fail("FAIL: platform-backed active config must report null synthetic fields")
		return
	CamBANGServer.stop()
	if CamBANGServer.is_running():
		_fail("FAIL: stop() must set is_running() false")
		return
	if CamBANGServer.get_active_provider_config() != null:
		_fail("FAIL: active provider config must be NIL after stop()")
		return

	var synthetic_default_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)
	if synthetic_default_err != OK:
		_fail("FAIL: start(SYNTHETIC) should default to NOMINAL + VIRTUAL_TIME")
		return
	var synth_default_cfg = CamBANGServer.get_active_provider_config()
	if int(synth_default_cfg.get("synthetic_role", -1)) != CamBANGServer.SYNTHETIC_ROLE_NOMINAL or int(synth_default_cfg.get("timing_driver", -1)) != CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME:
		_fail("FAIL: start(SYNTHETIC) default config mismatch")
		return
	CamBANGServer.stop()

	var synthetic_role_default_timing_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE
	)
	if synthetic_role_default_timing_err != OK:
		_fail("FAIL: start(SYNTHETIC, role) should default timing to VIRTUAL_TIME")
		return
	var synth_role_default_cfg = CamBANGServer.get_active_provider_config()
	if int(synth_role_default_cfg.get("synthetic_role", -1)) != CamBANGServer.SYNTHETIC_ROLE_TIMELINE or int(synth_role_default_cfg.get("timing_driver", -1)) != CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME:
		_fail("FAIL: start(SYNTHETIC, role) default timing mismatch")
		return
	CamBANGServer.stop()

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

	var set_synth_start_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE, CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME)
	if set_synth_start_err != OK:
		_fail("FAIL: start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, TIMELINE, VIRTUAL_TIME) rejected")
		return
	if not CamBANGServer.is_running():
		_fail("FAIL: synthetic start must set is_running() true")
		return
	if CamBANGServer.start() != ERR_ALREADY_IN_USE:
		_fail("FAIL: start() re-entry must return ERR_ALREADY_IN_USE while running")
		return
	if CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE, CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME) != ERR_ALREADY_IN_USE:
		_fail("FAIL: synthetic start re-entry must return ERR_ALREADY_IN_USE while running")
		return

	var synth_cfg = CamBANGServer.get_active_provider_config()
	if typeof(synth_cfg) != TYPE_DICTIONARY:
		_fail("FAIL: running synthetic start must expose Dictionary active config")
		return
	if int(synth_cfg.get("provider_kind", -1)) != CamBANGServer.PROVIDER_KIND_SYNTHETIC:
		_fail("FAIL: synthetic active config must report provider_kind=SYNTHETIC")
		return
	if int(synth_cfg.get("synthetic_role", -1)) != CamBANGServer.SYNTHETIC_ROLE_TIMELINE:
		_fail("FAIL: synthetic active config must report TIMELINE role")
		return
	if int(synth_cfg.get("timing_driver", -1)) != CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME:
		_fail("FAIL: synthetic active config must report VIRTUAL_TIME timing driver")
		return

	var timeline_stage_err := CamBANGServer.select_builtin_scenario("stream_lifecycle_versions")
	if timeline_stage_err != OK:
		_fail("FAIL: timeline builtin staging should be available after Timeline-role synthetic configuration")
		return
	var start_scenario_err := CamBANGServer.start_scenario()
	if start_scenario_err != OK:
		_fail("FAIL: start_scenario() should accept staged builtin scenario")
		return
	var stop_scenario_err := CamBANGServer.stop_scenario()
	if stop_scenario_err != OK:
		_fail("FAIL: stop_scenario() should stop staged scenario")
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
	if CamBANGServer.is_running():
		_fail("FAIL: is_running() must be false after completed stop()")
		return
	if CamBANGServer.get_active_provider_config() != null:
		_fail("FAIL: get_active_provider_config() must be NIL after completed stop()")
		return
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: get_state_snapshot() must be NIL after completed stop()")
		return

	var restart_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE, CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME)
	if restart_err != OK:
		_fail("FAIL: restart synthetic start rejected")
		return
	if not CamBANGServer.is_running():
		_fail("FAIL: is_running() must be true during restarted pre-baseline window")
		return
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
