extends Node

const TIMEOUT_MS := 5000
const STARTUP_REALIZATION_SCENARIO := "stream_inspection_live"

const PHASE_WAIT_FIRST_BASELINE := 0
const PHASE_WAIT_PENDING_EFFECTS := 1
const PHASE_RESTARTING := 2
const PHASE_WAIT_SECOND_BASELINE := 3
const PHASE_WAIT_RESTART_PENDING_EFFECTS := 4
const PHASE_DONE := 5

const STARTUP_INITIAL_STILL_WIDTH := 320
const STARTUP_FINAL_STILL_WIDTH := 352
const STARTUP_STILL_HEIGHT := 240
const STARTUP_STILL_FORMAT_RGBA := 1094862674
const STARTUP_INITIAL_WARM_HOLD_MS := 1
const STARTUP_FINAL_WARM_HOLD_MS := 50 # TODO: rework this test. Warm is now an intent
# returning OK very quickly, but not publising the new value until core says so.

var _done := false
var _quit_requested := false
var _phase := PHASE_WAIT_FIRST_BASELINE
var _timer: Timer
var _first_gen := -1
var _startup_hardware_id := ""
var _diag_last_phase_name := "init"
var _diag_waiting_for := "scene ready"
var _diag_last_observed_gen := -1
var _diag_last_observed_version := -1
var _diag_last_observed_topology_version := -1
var _diag_latest_snapshot_nil := true
var _diag_latest_snapshot_gen := -1
var _diag_latest_snapshot_version := -1
var _diag_latest_snapshot_topology_version := -1


func _ready() -> void:
	_diag_mark("invalid_start_argument_checks_begin", "public API shape and invalid-start rejection checks")
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

	_diag_mark("invalid_start_argument_checks_end", "main verification startup")

	print("RUN: godot public boundary verify")
	_diag_mark("main_verification_begin", "pre-start snapshot NIL check")

	_diag_snapshot("main_verification_pre_start_snapshot")
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

	_diag_mark("runtime_start_requested", "CamBANGServer.start(SYNTHETIC, TIMELINE, VIRTUAL_TIME, STRICT) return")
	var set_synth_start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_STRICT
	)
	_diag_mark("runtime_start_returned", "post-start snapshot NIL/non-NIL check", {"return_code": set_synth_start_err})
	_diag_snapshot("immediately_after_start")
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
	_diag_mark("baseline_wait_begin", "first baseline state_published version=0 topology_version=0", {"expected_gen": "unknown"})


func _assert_pre_baseline_public_boundary(context: String, accept_endpoint_startup_intent: bool = true) -> bool:
	_diag_mark("pre_baseline_public_boundary_checks_begin", context)
	if not CamBANGServer.is_running():
		_fail("FAIL: " + context + " pre-baseline window should keep is_running() true after accepted start")
		return false
	_diag_snapshot(context + " pre_baseline_snapshot")
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
		var startup_profile_before_engage := {
			"width": STARTUP_INITIAL_STILL_WIDTH,
			"height": STARTUP_STILL_HEIGHT,
			"format_fourcc": STARTUP_STILL_FORMAT_RGBA,
		}
		var profile_before_err: int = int(endpoint_handle.set_still_capture_profile(startup_profile_before_engage))
		if profile_before_err != OK:
			_fail("FAIL: " + context + " endpoint handle set_still_capture_profile() before engage should accept startup intent before baseline (err=%d)" % profile_before_err)
			return false
		var engage_err: int = int(endpoint_handle.engage())
		if engage_err != OK:
			_fail("FAIL: " + context + " endpoint handle engage() should accept startup intent before baseline (err=%d)" % engage_err)
			return false
		var startup_profile_after_engage := {
			"width": STARTUP_FINAL_STILL_WIDTH,
			"height": STARTUP_STILL_HEIGHT,
			"format_fourcc": STARTUP_STILL_FORMAT_RGBA,
		}
		var profile_after_err: int = int(endpoint_handle.set_still_capture_profile(startup_profile_after_engage))
		if profile_after_err != OK:
			_fail("FAIL: " + context + " endpoint handle set_still_capture_profile() after engage should accept startup intent before baseline (err=%d)" % profile_after_err)
			return false
		var warm_initial_err: int = int(endpoint_handle.set_warm_policy({"warm_hold_ms": STARTUP_INITIAL_WARM_HOLD_MS}))
		if warm_initial_err != OK:
			_fail("FAIL: " + context + " endpoint handle initial set_warm_policy() should accept startup intent before baseline (err=%d)" % warm_initial_err)
			return false
		var warm_final_err: int = int(endpoint_handle.set_warm_policy({"warm_hold_ms": STARTUP_FINAL_WARM_HOLD_MS}))
		if warm_final_err != OK:
			_fail("FAIL: " + context + " endpoint handle final set_warm_policy() should accept startup intent before baseline (err=%d)" % warm_final_err)
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
	_diag_mark("start_scenario_unstaged_begin", context)
	var unstaged_start_err := CamBANGServer.start_scenario()
	_diag_mark("start_scenario_unstaged_end", context, {"return_code": unstaged_start_err})
	if unstaged_start_err == OK:
		_fail("FAIL: " + context + " start_scenario() must fail before baseline when no scenario is staged")
		return false
	_diag_mark("load_stage_scenario_begin", context)
	var scenario_stage_err := CamBANGServer.select_builtin_scenario(STARTUP_REALIZATION_SCENARIO)
	_diag_mark("load_stage_scenario_end", context, {"return_code": scenario_stage_err})
	if scenario_stage_err != OK:
		_fail("FAIL: " + context + " select_builtin_scenario() should stage provider-owned data before baseline (err=%d)" % scenario_stage_err)
		return false
	_diag_mark("start_scenario_during_startup_begin", context)
	var scenario_start_err := CamBANGServer.start_scenario()
	_diag_mark("start_scenario_during_startup_end", context, {"return_code": scenario_start_err})
	if scenario_start_err != OK:
		_fail("FAIL: " + context + " start_scenario() should accept pending playback before baseline (err=%d)" % scenario_start_err)
		return false
	var advance_err := CamBANGServer.advance_timeline(1)
	if advance_err == OK:
		_fail("FAIL: " + context + " advance_timeline() must remain rejected before baseline")
		return false
	_diag_mark("pre_baseline_public_boundary_checks_end", context)
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
		if int(device.get("warm_hold_ms", -1)) != STARTUP_FINAL_WARM_HOLD_MS:
			continue
		var capture_profile_v = device.get("capture_profile", null)
		if typeof(capture_profile_v) != TYPE_DICTIONARY:
			continue
		var capture_profile: Dictionary = capture_profile_v
		var still_v = capture_profile.get("still", null)
		if typeof(still_v) != TYPE_DICTIONARY:
			continue
		var still: Dictionary = still_v
		if int(still.get("width", -1)) == STARTUP_FINAL_STILL_WIDTH and int(still.get("height", -1)) == STARTUP_STILL_HEIGHT and int(still.get("format", -1)) == STARTUP_STILL_FORMAT_RGBA:
			return true
	return false


func _on_timeout() -> void:
	_diag_snapshot("timeout_handler_latest_snapshot")
	print("DIAG65 timeout last_phase=%s last_observed_gen=%d last_observed_version=%d last_observed_topology_version=%d latest_snapshot_nil=%s latest_snapshot_gen=%d latest_snapshot_version=%d latest_snapshot_topology_version=%d waiting_for=%s" % [
		_diag_last_phase_name,
		_diag_last_observed_gen,
		_diag_last_observed_version,
		_diag_last_observed_topology_version,
		str(_diag_latest_snapshot_nil),
		_diag_latest_snapshot_gen,
		_diag_latest_snapshot_version,
		_diag_latest_snapshot_topology_version,
		_diag_waiting_for
	])
	_fail("FAIL: public boundary verify timed out")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

	_diag_last_observed_gen = gen
	_diag_last_observed_version = version
	_diag_last_observed_topology_version = topology_version
	_diag_mark("state_published_observed", "snapshot validation and phase dispatch", {"gen": gen, "version": version, "topology_version": topology_version})
	var snapshot = CamBANGServer.get_state_snapshot()
	_diag_record_snapshot(snapshot)
	_diag_print_snapshot("state_published_snapshot_read")
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
				_diag_mark("baseline_rejected", "first publish must be baseline", {"reason": "nonzero version/topology"})
				_fail("FAIL: first Godot-visible publish of generation must be baseline (version=0, topology_version=0)")
				return
			if _snapshot_has_scenario_effects(d):
				_diag_mark("baseline_rejected", "first baseline must not show pending startup effects", {"reason": "scenario effects visible"})
				_fail("FAIL: pending startup effects must not be visible in the baseline snapshot")
				return

			if not _assert_post_baseline_public_boundary():
				return

			_diag_mark("baseline_accepted", "scenario playback start effects", {"gen": gen, "version": version, "topology_version": topology_version})
			_first_gen = gen
			_phase = PHASE_WAIT_PENDING_EFFECTS
			_diag_mark("scenario_playback_start_observed", "pending startup effects publish", {"observed": false, "reason": "baseline only"})

		PHASE_WAIT_PENDING_EFFECTS:
			if version == 0:
				_diag_mark("scenario_playback_start_observed", "nonzero version with scenario effects", {"observed": false, "reason": "version still zero"})
				return
			if not _snapshot_has_scenario_effects(d):
				_diag_mark("scenario_playback_start_observed", "nonzero version with scenario effects", {"observed": false, "reason": "scenario effects absent"})
				return
			if not _snapshot_has_startup_endpoint_effects(d):
				_diag_mark("scenario_playback_start_observed", "startup endpoint effects", {"observed": false, "reason": "startup endpoint effects absent"})
				return
			_diag_mark("scenario_playback_start_observed", "restart boundary", {"observed": true, "gen": gen, "version": version, "topology_version": topology_version})
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
				_diag_mark("baseline_rejected", "restart generation must advance exactly by one", {"reason": "unexpected generation", "expected": _first_gen + 1, "actual": gen})
				_fail("FAIL: restart must advance generation exactly by one")
				return
			if version != 0 or topology_version != 0:
				_diag_mark("baseline_rejected", "restarted first publish must be baseline", {"reason": "nonzero version/topology"})
				_fail("FAIL: first publish of restarted generation must be baseline")
				return
			if _snapshot_has_scenario_effects(d):
				_diag_mark("baseline_rejected", "restarted baseline must not show pending startup effects", {"reason": "scenario effects visible"})
				_fail("FAIL: pending startup effects must not be visible in restarted baseline snapshot")
				return

			_diag_mark("baseline_accepted", "restart pending effects", {"gen": gen, "version": version, "topology_version": topology_version})
			_phase = PHASE_WAIT_RESTART_PENDING_EFFECTS

		PHASE_WAIT_RESTART_PENDING_EFFECTS:
			if version == 0:
				_diag_mark("scenario_playback_start_observed", "restart scenario effects", {"observed": false, "reason": "version still zero"})
				return
			if _snapshot_has_hardware_id(d, _startup_hardware_id):
				_fail("FAIL: endpoint startup intent from previous session leaked into restarted generation")
				return
			if not _snapshot_has_scenario_effects(d):
				_diag_mark("scenario_playback_start_observed", "restart scenario effects", {"observed": false, "reason": "scenario effects absent"})
				return
			_diag_mark("scenario_playback_start_observed", "final pass", {"observed": true, "gen": gen, "version": version, "topology_version": topology_version})
			_ok("OK: godot public boundary verify PASS")

		PHASE_DONE:
			return


func _restart_and_assert_nil() -> void:
	if _done:
		return

	_diag_mark("restart_boundary_begin", "stop before restart")
	_diag_mark("stop_boundary_begin", "CamBANGServer.stop during restart")
	CamBANGServer.stop()
	_diag_mark("stop_boundary_end", "post-stop NIL checks during restart")
	if CamBANGServer.is_running():
		_fail("FAIL: is_running() must be false after completed stop()")
		return
	if CamBANGServer.get_active_provider_config() != null:
		_fail("FAIL: get_active_provider_config() must be NIL after completed stop()")
		return
	if CamBANGServer.get_state_snapshot() != null:
		_fail("FAIL: get_state_snapshot() must be NIL after completed stop()")
		return

	_diag_mark("runtime_start_requested", "restart synthetic start return")
	var restart_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE, CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME)
	_diag_mark("runtime_start_returned", "restart pre-baseline checks", {"return_code": restart_err})
	_diag_snapshot("immediately_after_restart_start")
	if restart_err != OK:
		_fail("FAIL: restart synthetic start rejected")
		return
	if not CamBANGServer.is_running():
		_fail("FAIL: is_running() must be true during restarted pre-baseline window")
		return
	if not _assert_pre_baseline_public_boundary("restart", false):
		return
	_phase = PHASE_WAIT_SECOND_BASELINE
	_diag_mark("baseline_wait_begin", "second baseline state_published version=0 topology_version=0", {"expected_gen": _first_gen + 1})
	_diag_mark("restart_boundary_end", "waiting for second baseline", {"expected_gen": _first_gen + 1})


func _ok(msg: String) -> void:
	if _done:
		return
	_diag_mark("final_pass_quit_begin", "cleanup and quit success")
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
	_diag_mark("final_pass_quit_end" if code == 0 else "final_fail_quit_begin", "cleanup stop/disconnect")
	if _timer != null and is_instance_valid(_timer):
		_timer.stop()
		remove_child(_timer)
		_timer.queue_free()
		_timer = null
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	_diag_mark("stop_boundary_begin", "cleanup stop")
	if not _quit_requested:
		_quit_requested = true
		CamBANGServer.stop_and_quit(code)
	_diag_mark("stop_boundary_end", "cleanup quit scheduled")


func _diag_mark(name: String, waiting_for: String = "", details: Dictionary = {}) -> void:
	_diag_last_phase_name = name
	if not waiting_for.is_empty():
		_diag_waiting_for = waiting_for
	var detail_text := ""
	if not details.is_empty():
		detail_text = " details=" + str(details)
	print("DIAG65 phase=%s logical_phase=%s waiting_for=%s%s" % [
		name,
		_phase_name(_phase),
		_diag_waiting_for,
		detail_text
	])


func _diag_record_snapshot(snapshot) -> void:
	_diag_latest_snapshot_nil = snapshot == null
	_diag_latest_snapshot_gen = -1
	_diag_latest_snapshot_version = -1
	_diag_latest_snapshot_topology_version = -1
	if typeof(snapshot) == TYPE_DICTIONARY:
		var d: Dictionary = snapshot
		_diag_latest_snapshot_gen = int(d.get("gen", -1))
		_diag_latest_snapshot_version = int(d.get("version", -1))
		_diag_latest_snapshot_topology_version = int(d.get("topology_version", -1))


func _diag_snapshot(label: String) -> void:
	_diag_record_snapshot(CamBANGServer.get_state_snapshot())
	_diag_print_snapshot(label)


func _diag_print_snapshot(label: String) -> void:
	print("DIAG65 snapshot label=%s nil=%s gen=%d version=%d topology_version=%d" % [
		label,
		str(_diag_latest_snapshot_nil),
		_diag_latest_snapshot_gen,
		_diag_latest_snapshot_version,
		_diag_latest_snapshot_topology_version
	])


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
