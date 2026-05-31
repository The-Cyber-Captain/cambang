extends Node

const TIMEOUT_MS := 5000

const PHASE_WAIT_FIRST_BASELINE := 0
const PHASE_WAIT_PENDING_EFFECTS := 1
const PHASE_RESTARTING := 2
const PHASE_WAIT_SECOND_BASELINE := 3
const PHASE_WAIT_RESTART_PENDING_EFFECTS := 4
const PHASE_DONE := 5

const STARTUP_STILL_WIDTH := 320
const STARTUP_STILL_HEIGHT := 240
const STARTUP_STILL_FORMAT_RGBA := 1094862674
const STARTUP_WARM_HOLD_MS := 1

var _done := false
var _quit_requested := false
var _phase := PHASE_WAIT_FIRST_BASELINE
var _timer: Timer
var _first_gen := -1
var _startup_hardware_id := ""


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
	if CamBANGServer.has_method("set_completion_gated_destructive_sequencing_enabled"):
		_fail("FAIL: legacy live timeline reconciliation setter should be absent")
		return
	if CamBANGServer.has_method("is_ready") or CamBANGServer.has_method("is_command_ready"):
		_fail("FAIL: public readiness getter must not be added on CamBANGServer")
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

	var invalid_reconciliation_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		9999
	)
	if invalid_reconciliation_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: invalid timeline reconciliation must return ERR_INVALID_PARAMETER")
		return

	var invalid_platform_with_reconciliation_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED,
		CamBANGServer.SYNTHETIC_ROLE_NOMINAL,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_STRICT
	)
	if invalid_platform_with_reconciliation_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: start(PLATFORM_BACKED, role, timing_driver, reconciliation) must return ERR_INVALID_PARAMETER")
		return

	var invalid_nominal_with_reconciliation_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_NOMINAL,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_STRICT
	)
	if invalid_nominal_with_reconciliation_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: start(SYNTHETIC, NOMINAL, VIRTUAL_TIME, reconciliation) must return ERR_INVALID_PARAMETER")
		return

	var invalid_realtime_with_reconciliation_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_REAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_STRICT
	)
	if invalid_realtime_with_reconciliation_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: start(SYNTHETIC, TIMELINE, REAL_TIME, reconciliation) must return ERR_INVALID_PARAMETER")
		return

	var invalid_platform_with_role_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED,
		CamBANGServer.SYNTHETIC_ROLE_NOMINAL
	)
	if invalid_platform_with_role_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: start(PLATFORM_BACKED, role) must return ERR_INVALID_PARAMETER")
		return

	var invalid_platform_with_role_and_timing_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED,
		CamBANGServer.SYNTHETIC_ROLE_NOMINAL,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME
	)
	if invalid_platform_with_role_and_timing_err != ERR_INVALID_PARAMETER:
		_fail("FAIL: start(PLATFORM_BACKED, role, timing_driver) must return ERR_INVALID_PARAMETER")
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
	if platform_cfg.get("synthetic_role", "not_null") != null or platform_cfg.get("timing_driver", "not_null") != null or platform_cfg.get("timeline_reconciliation", "not_null") != null:
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
	if synth_default_cfg.get("timeline_reconciliation", "not_null") != null:
		_fail("FAIL: start(SYNTHETIC) nominal config must report null timeline_reconciliation")
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
	if str(synth_role_default_cfg.get("timeline_reconciliation", "")) != "completion_gated":
		_fail("FAIL: start(SYNTHETIC, TIMELINE) must default timeline_reconciliation to completion_gated")
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

	var set_synth_start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_STRICT
	)
	if set_synth_start_err != OK:
		_fail("FAIL: start(SYNTHETIC, TIMELINE, VIRTUAL_TIME, STRICT) rejected")
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
	if CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_STRICT
	) != ERR_ALREADY_IN_USE:
		_fail("FAIL: synthetic strict start re-entry must return ERR_ALREADY_IN_USE while running")
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
	if str(synth_cfg.get("timeline_reconciliation", "")) != "strict":
		_fail("FAIL: synthetic active config must report strict timeline_reconciliation")
		return

	if not _assert_pre_baseline_public_boundary("initial start"):
		return


func _assert_pre_baseline_public_boundary(context: String, accept_endpoint_startup_intent: bool = true) -> bool:
	if not CamBANGServer.is_running():
		_fail("FAIL: " + context + " pre-baseline window should keep is_running() true after accepted start")
		return false
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: " + context + " snapshot must remain NIL until first Godot-visible baseline publish")
		return false
	var endpoints = CamBANGServer.enumerate_devices()
	if typeof(endpoints) != TYPE_ARRAY:
		_fail("FAIL: " + context + " enumerate_devices() must return an Array before baseline")
		return false
	if endpoints.size() < 1:
		_fail("FAIL: " + context + " enumerate_devices() must expose synthetic endpoints before baseline")
		return false
	var endpoint_index := 1 if endpoints.size() > 1 else 0
	var endpoint0 = endpoints[endpoint_index]
	if typeof(endpoint0) != TYPE_DICTIONARY:
		_fail("FAIL: " + context + " enumerate_devices() entries must be Dictionary before baseline")
		return false
	var hardware_id := str(endpoint0.get("hardware_id", ""))
	if hardware_id.is_empty():
		_fail("FAIL: " + context + " pre-baseline endpoint hardware_id must be non-empty")
		return false
	var endpoint_handle = CamBANGServer.get_device_for_hardware_id(hardware_id)
	if endpoint_handle == null:
		_fail("FAIL: " + context + " get_device_for_hardware_id() must return a handle before baseline")
		return false
	if not endpoint_handle.has_method("engage") or not endpoint_handle.has_method("create_stream") or not endpoint_handle.has_method("set_warm_policy") or not endpoint_handle.has_method("set_still_capture_profile"):
		_fail("FAIL: " + context + " pre-baseline endpoint handle must expose mutating lifecycle methods")
		return false
	if int(endpoint_handle.get_instance_id()) != 0:
		_fail("FAIL: " + context + " pre-baseline endpoint handle must not resolve a runtime instance id")
		return false
	if endpoint_handle.create_stream() != null:
		_fail("FAIL: " + context + " endpoint handle create_stream() must return null before baseline")
		return false
	if endpoint_handle.trigger_capture() == OK:
		_fail("FAIL: " + context + " endpoint handle trigger_capture() must remain rejected before baseline")
		return false
	if accept_endpoint_startup_intent:
		_startup_hardware_id = hardware_id
		var startup_profile := {
			"width": STARTUP_STILL_WIDTH,
			"height": STARTUP_STILL_HEIGHT,
			"format_fourcc": STARTUP_STILL_FORMAT_RGBA,
		}
		var profile_err: int = int(endpoint_handle.set_still_capture_profile(startup_profile))
		if profile_err != OK:
			_fail("FAIL: " + context + " endpoint handle set_still_capture_profile() should accept startup intent before baseline (err=%d)" % profile_err)
			return false
		var engage_err: int = int(endpoint_handle.engage())
		if engage_err != OK:
			_fail("FAIL: " + context + " endpoint handle engage() should accept startup intent before baseline (err=%d)" % engage_err)
			return false
		var warm_err: int = int(endpoint_handle.set_warm_policy({"warm_hold_ms": STARTUP_WARM_HOLD_MS}))
		if warm_err != OK:
			_fail("FAIL: " + context + " endpoint handle set_warm_policy() should accept startup intent before baseline (err=%d)" % warm_err)
			return false
	if CamBANGServer.get_device(1) != null:
		_fail("FAIL: " + context + " get_device() must return null before baseline")
		return false
	if CamBANGServer.get_rig(1) != null:
		_fail("FAIL: " + context + " get_rig() must return null before baseline")
		return false
	if CamBANGServer.get_stream_result_by_stream_id(1) != null:
		_fail("FAIL: " + context + " stream result lookup must return null before baseline")
		return false
	if CamBANGServer.get_capture_result_by_id(1, 1) != null:
		_fail("FAIL: " + context + " capture result lookup must return null before baseline")
		return false
	var result_set = CamBANGServer.get_capture_result_set_by_id(1)
	if result_set != null:
		_fail("FAIL: " + context + " capture result-set lookup must return null before baseline")
		return false
	var unstaged_start_err := CamBANGServer.start_scenario()
	if unstaged_start_err == OK:
		_fail("FAIL: " + context + " start_scenario() must fail before baseline when no scenario is staged")
		return false
	var scenario_stage_err := CamBANGServer.select_builtin_scenario("stream_lifecycle_versions")
	if scenario_stage_err != OK:
		_fail("FAIL: " + context + " select_builtin_scenario() should stage provider-owned data before baseline (err=%d)" % scenario_stage_err)
		return false
	var scenario_start_err := CamBANGServer.start_scenario()
	if scenario_start_err != OK:
		_fail("FAIL: " + context + " start_scenario() should accept pending playback before baseline (err=%d)" % scenario_start_err)
		return false
	var advance_err := CamBANGServer.advance_timeline(1)
	if advance_err == OK:
		_fail("FAIL: " + context + " advance_timeline() must remain rejected before baseline")
		return false
	return true


func _assert_post_baseline_public_boundary() -> bool:
	var endpoints = CamBANGServer.enumerate_devices()
	if typeof(endpoints) != TYPE_ARRAY or endpoints.size() < 1:
		_fail("FAIL: enumerate_devices() must work after baseline")
		return false
	var endpoint0 = endpoints[0]
	if typeof(endpoint0) != TYPE_DICTIONARY:
		_fail("FAIL: enumerate_devices() entries must be Dictionary after baseline")
		return false
	var hardware_id := str(endpoint0.get("hardware_id", ""))
	if hardware_id.is_empty():
		_fail("FAIL: post-baseline endpoint hardware_id must be non-empty")
		return false
	if CamBANGServer.get_device_for_hardware_id(hardware_id) == null:
		_fail("FAIL: get_device_for_hardware_id() must produce a handle after baseline")
		return false
	return true


func _snapshot_has_scenario_effects(snapshot: Dictionary) -> bool:
	var devices = snapshot.get("devices", [])
	if typeof(devices) == TYPE_ARRAY and devices.size() > 0:
		return true
	var streams = snapshot.get("streams", [])
	if typeof(streams) == TYPE_ARRAY and streams.size() > 0:
		return true
	var rigs = snapshot.get("rigs", [])
	if typeof(rigs) == TYPE_ARRAY and rigs.size() > 0:
		return true
	return false


func _snapshot_has_hardware_id(snapshot: Dictionary, hardware_id: String) -> bool:
	if hardware_id.is_empty():
		return false
	var devices = snapshot.get("devices", [])
	if typeof(devices) != TYPE_ARRAY:
		return false
	for device_v in devices:
		if typeof(device_v) == TYPE_DICTIONARY and str(device_v.get("hardware_id", "")) == hardware_id:
			return true
	return false


func _snapshot_has_startup_endpoint_effects(snapshot: Dictionary) -> bool:
	if _startup_hardware_id.is_empty():
		return false
	var devices = snapshot.get("devices", [])
	if typeof(devices) != TYPE_ARRAY:
		return false
	for device_v in devices:
		if typeof(device_v) != TYPE_DICTIONARY:
			continue
		var device: Dictionary = device_v
		if str(device.get("hardware_id", "")) != _startup_hardware_id:
			continue
		if not bool(device.get("engaged", false)):
			continue
		if int(device.get("warm_hold_ms", -1)) != STARTUP_WARM_HOLD_MS:
			continue
		var capture_profile_v = device.get("capture_profile", null)
		if typeof(capture_profile_v) != TYPE_DICTIONARY:
			continue
		var capture_profile: Dictionary = capture_profile_v
		var still_v = capture_profile.get("still", null)
		if typeof(still_v) != TYPE_DICTIONARY:
			continue
		var still: Dictionary = still_v
		if int(still.get("width", -1)) == STARTUP_STILL_WIDTH and int(still.get("height", -1)) == STARTUP_STILL_HEIGHT and int(still.get("format", -1)) == STARTUP_STILL_FORMAT_RGBA:
			return true
	return false


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
			if _snapshot_has_scenario_effects(d):
				_fail("FAIL: pending startup effects must not be visible in the baseline snapshot")
				return

			if not _assert_post_baseline_public_boundary():
				return

			_first_gen = gen
			_phase = PHASE_WAIT_PENDING_EFFECTS

		PHASE_WAIT_PENDING_EFFECTS:
			if version == 0:
				return
			if not _snapshot_has_scenario_effects(d):
				return
			if not _snapshot_has_startup_endpoint_effects(d):
				return
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
			if _snapshot_has_scenario_effects(d):
				_fail("FAIL: pending startup effects must not be visible in restarted baseline snapshot")
				return

			_phase = PHASE_WAIT_RESTART_PENDING_EFFECTS

		PHASE_WAIT_RESTART_PENDING_EFFECTS:
			if version == 0:
				return
			if _snapshot_has_hardware_id(d, _startup_hardware_id):
				_fail("FAIL: endpoint startup intent from previous session leaked into restarted generation")
				return
			if not _snapshot_has_scenario_effects(d):
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
	if not _assert_pre_baseline_public_boundary("restart", false):
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
		remove_child(_timer)
		_timer.queue_free()
		_timer = null
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		get_tree().quit(code)


func _phase_name(p: int) -> String:
	match p:
		PHASE_WAIT_FIRST_BASELINE:
			return "wait_first_baseline"
		PHASE_WAIT_PENDING_EFFECTS:
			return "wait_pending_effects"
		PHASE_RESTARTING:
			return "restarting"
		PHASE_WAIT_SECOND_BASELINE:
			return "wait_second_baseline"
		PHASE_WAIT_RESTART_PENDING_EFFECTS:
			return "wait_restart_pending_effects"
		PHASE_DONE:
			return "done"
		_:
			return "unknown"
