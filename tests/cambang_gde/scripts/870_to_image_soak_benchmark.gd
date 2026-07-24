extends Control

const SCENE_LABEL := "870_to_image_soak_benchmark"
const SCENE_RECORD_ID := "scene870_to_image_soak_summary"
const HW_A := "synthetic:0"
const HW_B := "synthetic:1"
const DEV_A := "device_a"
const DEV_B := "device_b"
const BENCH_RIG_ID := 870001
# Distinct synthetic per-device pictures (full PictureConfig) so the two sources
# render visibly distinct -- for BOTH the live streams and the captures, mirroring
# what the removed scenario authored (checker/noise/solid + distinct colours).
# Applied only for synthetic (platform providers reject a picture).
const _SYNTHETIC_STREAM_PICTURE := {
	DEV_A: {"preset": "checker", "seed": 87021, "solid_r": 255, "solid_g": 64, "solid_b": 64, "solid_a": 255, "checker_size_px": 24},
	DEV_B: {"preset": "noise", "seed": 87022, "solid_r": 64, "solid_g": 160, "solid_b": 255, "solid_a": 255, "checker_size_px": 20},
}
const _SYNTHETIC_CAPTURE_PICTURE := {
	DEV_A: {"preset": "checker", "seed": 87011, "solid_r": 16, "solid_g": 40, "solid_b": 96, "solid_a": 255, "checker_size_px": 18},
	DEV_B: {"preset": "solid", "seed": 87012, "solid_r": 92, "solid_g": 44, "solid_b": 152, "solid_a": 255, "checker_size_px": 16},
}

# Curated testable-equipment table (maintainer-owned), keyed by the
# --cambang-bench-provider selection. Each entry names the two devices the rig
# drives (by hardware_id) and whether their concurrent use is claimed to Core.
# The rig's concurrency truth is built from this and ingested via
# ingest_camera_description() before start(), because Core's rig admission gate
# fail-closes any multi-device rig unless a truth naming the exact combination
# was ingested (CoreRuntime::grouped_rig_imaging_spec_admission_failure_).
#
# Synthetic uses stable ids. Platform entries are added with the real equipment;
# note that Camera2 ids are stable strings but WinRT ids are machine-specific
# DeviceInformation symbolic links, and whether two cameras are truly concurrent
# is discovered at runtime (a claimed-but-unusable combination surfaces as a rig
# capture failure, not a silent wrong result).
# Curated testable-equipment table. Each entry declares the camera-concurrency
# the rig claims to Core plus which pair the rig drives. Two id regimes:
#  - KNOWN ids (synthetic constants; Android's stable Camera2 id strings): the
#    entry lists `cameras` + `concurrent_combinations` + `rig_pair`, and the
#    concurrency truth is ingested before start().
#  - DISCOVERED ids (WinRT DeviceInformation symbolic links -- machine-specific):
#    the entry lists no cameras, and the enumerate-first path discovers the pair
#    at runtime (start -> enumerate -> select -> stop -> ingest -> start).
# Android matches by OS.get_model_name(); single-host OSes match their sole entry.
# Adding a device is one table row.
const EQUIPMENT := {
	"synthetic": {
		"cameras": [HW_A, HW_B],
		"concurrent_combinations": [[HW_A, HW_B]],
		"rig_pair": [HW_A, HW_B],
		"permissions": [],
	},
	"platform_backed": {
		"Android": [
			{
				"label": "galaxy_s20_plus",
				"model_match": "SM-G986U1",
				"permissions": ["android.permission.CAMERA"],
				# Stable Camera2 ids (maintainer-supplied): five cameras, with the
				# device's real concurrent combinations. The rig drives one of them.
				"cameras": ["0", "1", "2", "3", "4"],
				"concurrent_combinations": [["0", "1"], ["0", "3"]],
				"rig_pair": ["0", "1"],
			},
			# hammer_construction_2_thermal, meta_quest_3: filled after the
			# galaxy_s20_plus proof of concept (each needs a physical plug/unplug).
		],
		"Windows": [
			{
				"label": "winrt_host",
				"model_match": "",  # single host: matched unconditionally
				"permissions": [],  # WinRT consent model is permissive here
				# No cameras/combinations: WinRT ids are machine-specific symbolic
				# links -> enumerate-first path discovers the pair at runtime.
				# (Populating this, if ever needed, requires on-host assistance.)
			},
		],
	},
}

const SETUP_TIMEOUT_MS := 10000
const PROFILE_APPLY_TIMEOUT_MS := 5000
const CAPTURE_TIMEOUT_US := 5000000
const RIG_CAPTURE_TIMEOUT_US := 7000000
const WARMUP_SEC_DEFAULT := 0.65
const HUMAN_PHASE_SEC_DEFAULT := 3.00 #0.70
const SUPERHUMAN_PHASE_SEC_DEFAULT := 0.35
const RIG_PHASE_SEC_DEFAULT := 0.70
const EXIT_VISUAL_HOLD_SEC_DEFAULT := 1.50
const PREFLIGHT_STREAMS_ONLY_SEC := 10.00
const STREAM_DISPLAY_REBIND_INTERVAL_US := 100000
const MAX_MAIN_VISUAL_TEXTURES_PER_FRAME := 3
const MAX_THUMBNAILS_PER_FRAME := 2
const MAX_STREAM_TO_IMAGE_DRAIN_PER_FRAME := 8
const MAX_CAPTURE_DRAIN_PER_FRAME := 8
const MAX_RIG_DRAIN_PER_FRAME := 4
const MAX_SLOW_SAMPLES := 12
const PRINT_DECIMALS := 3
const STILL_PROFILE_WIDTH := 1280
const STILL_PROFILE_HEIGHT := 720
#const STILL_PROFILE_FORMAT_RGBA := 1094862674
const PANEL_SEED_IMAGE_FORMAT := Image.FORMAT_RGBA8

const PHASE_SETUP := "setup"
const PHASE_PROFILE := "profile"
const PHASE_WARMUP := "warmup"
const PHASE_PREFLIGHT := "preflight"
const PHASE_RUNNING := "running"
const PHASE_SETTLEMENT_PROBE := "settlement_probe"
const PHASE_DONE := "done"

const SETTLEMENT_PROBE_SETTLE_TIMEOUT_US := 2500000
const LOAD_PROFILE_HUMAN := "human"
const LOAD_PROFILE_ELEVATED := "elevated"
const LOAD_PROFILE_SUPERHUMAN := "superhuman"
const LOAD_PROFILE_STRESS := "stress"
const LOAD_PROFILE_DEFAULT := LOAD_PROFILE_SUPERHUMAN
const LOAD_PROFILE_NAMES := [LOAD_PROFILE_HUMAN, LOAD_PROFILE_ELEVATED, LOAD_PROFILE_SUPERHUMAN, LOAD_PROFILE_STRESS]

var _state := PHASE_SETUP
var _done := false
var _failed := false
var _cleanup_started := false
var _finish_reason := "complete"
var _is_headless := false
var _provider_arg := "synthetic"
var _load_profile_arg := LOAD_PROFILE_DEFAULT
var _seed := 870001
var _rng := RandomNumberGenerator.new()
var _warmup_sec := WARMUP_SEC_DEFAULT
var _human_phase_sec := HUMAN_PHASE_SEC_DEFAULT
var _superhuman_phase_sec := SUPERHUMAN_PHASE_SEC_DEFAULT
var _rig_phase_sec := RIG_PHASE_SEC_DEFAULT
var _exit_visual_hold_sec := EXIT_VISUAL_HOLD_SEC_DEFAULT
var _phase_sec_override := -1.0
var _stream_requests_per_sec_per_stream := -1.0
var _capture_requests_per_sec_per_device := -1.0
var _rig_requests_per_sec := -1.0
var _max_pending_materializations := -1
var _max_pending_textures := -1
var _minimum_frames_per_phase := -1
var _minimum_attempts_per_phase := -1
var _minimum_completions_per_phase := -1
var _hard_timeout_multiplier := -1.0
var _superhuman_actions_per_tick := 3
var _max_inflight_captures_per_device := 1
var _materialize_textures_in_headless := false

var _started_us := 0
var _setup_started_ms := 0
var _profile_started_ms := 0
var _warmup_started_us := 0
var _phase_started_us := 0
var _last_stats_update_us := 0
var _last_stream_observation_us := 0
var _benchmark_metrics_frozen := false
var _latest_device_snapshot_by_id := {}
var _preflight_done := false
var _preflight_stage := ""
var _preflight_started_us := 0
var _preflight_observe_started_us := 0
var _preflight_devices := {}

var _devices := {
	DEV_A: {
		"label": "Device A",
		"hardware_id": HW_A,
		"device_id": 0,
		"device": null,
		"stream_id": 0,
		"stream_observed_changes": 0,
		"stream_last_observed_mark": 0,
		"stream_observation_first_us": 0,
		"stream_observation_last_us": 0,
		"live_display_bound": false,
		"inflight_captures": 0,
	},
	DEV_B: {
		"label": "Device B",
		"hardware_id": HW_B,
		"device_id": 0,
		"device": null,
		"stream_id": 0,
		"stream_observed_changes": 0,
		"stream_last_observed_mark": 0,
		"stream_observation_first_us": 0,
		"stream_observation_last_us": 0,
		"live_display_bound": false,
		"inflight_captures": 0,
	},
}
var _rig = null
var _rig_inflight := 0
# Unified public-API topology staging (no scenario): the setup phase drives
# enumerate -> engage -> stream -> create_rig itself, once each.
var _topology_engaged := false
var _topology_streamed := false

var _current_bundle_index := -1
var _current_bundle := {}
var _profile_before_by_device := {}
var _profile_requested := false
var _phases := []
var _phase_index := -1
var _active_phase := {}
var _current_phase_visual_sequence := 0

var _stream_jobs := []
var _capture_jobs := []
var _rig_jobs := []
var _thumbnail_jobs := []
var _completed_phase_records := []
var _capture_provenance_by_capture_id := {}
var _acq_probe_attempted := false
var _acq_probe_stage := ""
var _acq_probe_started_us := 0
var _acq_probe_settle_started_us := 0
var _acq_probe_settle_deadline_us := 0
var _acq_probe_bundle_label := ""
var _acq_probe_required_member_count := 0
var _acq_probe_devices := {}
var _run_frame_ms := []
var _frame_count := 0
var _last_frame_us := 0
var _benchmark_finished_us: int = 0
var _benchmark_frame_count: int = 0
var _exit_visual_hold_entered: bool = false
var _exit_visual_hold_started_us: int = 0
var _exit_visual_hold_ended_us: int = 0
var _exit_visual_hold_frame_count: int = 0
var _exit_visual_hold_frame_ms: Array = []

var _header_label: Label = null
var _stats_label: Label = null
var _log_label: RichTextLabel = null
var _stream_live_rects := {}
var _stream_live_labels := {}
var _stream_image_rects := {}
var _stream_image_labels := {}
var _stream_image_textures := {}
var _capture_rects := {}
var _capture_labels := {}
var _capture_textures := {}
var _capture_rows := {}
var _rig_rects := {}
var _rig_labels := {}
var _rig_textures := {}
var _rig_rows := {}

func _ready() -> void:
	_is_headless = DisplayServer.get_name() == "headless"
	_started_us = _now_us()
	_setup_started_ms = Time.get_ticks_msec()
	_last_stats_update_us = _started_us
	_last_stream_observation_us = _started_us
	_last_frame_us = _started_us
	_parse_args()
	_rng.seed = _seed
	_build_ui()
	_log("RUN: %s provider=%s seed=%d headless=%s" % [SCENE_LABEL, _provider_arg, _seed, str(_is_headless)])
	if _provider_arg == "synthetic" and _synthetic_producer_output_form_effective_selection() == "gpu_only" and _scene870_is_compatibility_renderer():
		_finish(0, true, "compatibility_gpu_only")
		return
	set_process(true)
	_bootstrap()


func _process(delta: float) -> void:
	if _done:
		return
	_record_frame(delta)
	_drain_visual_thumbnail_jobs(MAX_THUMBNAILS_PER_FRAME)
	_update_stream_display_views(false)
	_poll_capture_jobs()
	_poll_rig_jobs()
	_poll_acquisition_session_settlement_probe()
	_drain_stream_jobs(MAX_STREAM_TO_IMAGE_DRAIN_PER_FRAME)
	_update_stats_label(false)

	match _state:
		PHASE_SETUP:
			_poll_setup()
		PHASE_PROFILE:
			_poll_profile_application()
		PHASE_WARMUP:
			_poll_warmup()
		PHASE_PREFLIGHT:
			_poll_preflight()
		PHASE_RUNNING:
			_poll_active_phase()
		PHASE_SETTLEMENT_PROBE:
			pass


func _bootstrap() -> void:
	if _provider_arg == "synthetic":
		_bootstrap_from_known_ids(EQUIPMENT.get("synthetic", {}), true)
	else:
		_bootstrap_platform()


func _bootstrap_from_known_ids(equip: Dictionary, synthetic: bool) -> void:
	# Known camera ids (synthetic constants, or Android's stable Camera2 id
	# strings from the table): ingest the concurrency truth (cameras +
	# combinations) before start(); the rig drives rig_pair. No enumerate-first.
	var cameras: Array = equip.get("cameras", [])
	var combinations: Array = equip.get("concurrent_combinations", [])
	var rig_pair: Array = equip.get("rig_pair", [])
	if cameras.size() < 2 or rig_pair.size() < 2:
		_finish(0, true, "equipment_incomplete:%s" % str(equip.get("label", _provider_arg)))
		return
	_devices[DEV_A]["hardware_id"] = str(rig_pair[0])
	_devices[DEV_B]["hardware_id"] = str(rig_pair[1])
	CamBANGServer.stop()
	var truth := _build_concurrency_truth(cameras, combinations)
	if int(CamBANGServer.ingest_camera_description(truth)) != OK:
		_fail("bootstrap failed: ingest_camera_description")
		return
	var start_err := int(
		CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC) if synthetic
		else CamBANGServer.start()
	)
	if start_err != OK:
		_fail("bootstrap failed: start(%s) returned %d" % [_provider_arg, start_err])
		return
	_log("bootstrap: %s started (known ids, ingest-before-start); rig pair=%s" % [
		str(equip.get("label", _provider_arg)), str(rig_pair)
	])


func _bootstrap_platform() -> void:
	# Resolve the curated entry for the attached hardware, verify its permission
	# prerequisites (pre-granted for an automated soak).
	var equip := _resolve_platform_equipment()
	if equip.is_empty():
		_finish(0, true, "equipment_not_configured:%s" % _platform_identity_text())
		return
	if not _platform_permissions_granted(equip.get("permissions", [])):
		_finish(0, true, "permission_not_granted:%s" % str(equip.get("label", "?")))
		return
	# Android's Camera2 ids are stable and listed in the table -> known-id path.
	if not (equip.get("cameras", []) as Array).is_empty():
		_bootstrap_from_known_ids(equip, false)
		return
	# WinRT etc.: ids are machine-specific symbolic links -> enumerate-first
	# (discover the pair, then ingest): start -> enumerate -> select -> stop ->
	# ingest -> start.
	CamBANGServer.stop()
	if int(CamBANGServer.start()) != OK:
		_fail("platform bootstrap: initial start() failed")
		return
	var endpoints: Array = CamBANGServer.enumerate_devices()
	if endpoints.size() < 2:
		# Fewer than two cameras -> no rig here. Single-device degrade is a later
		# refinement; for now report it plainly rather than fail hard.
		CamBANGServer.stop()
		_finish(0, true, "insufficient_cameras:%s:%d" % [str(equip.get("label", "?")), endpoints.size()])
		return
	var pair := _select_platform_pair(endpoints)
	CamBANGServer.stop()
	var truth := _build_concurrency_truth(pair, [pair])
	if int(CamBANGServer.ingest_camera_description(truth)) != OK:
		_fail("platform bootstrap: ingest_camera_description failed")
		return
	if int(CamBANGServer.start()) != OK:
		_fail("platform bootstrap: working start() after ingest failed")
		return
	_devices[DEV_A]["hardware_id"] = str(pair[0])
	_devices[DEV_B]["hardware_id"] = str(pair[1])
	_log("bootstrap: %s started (enumerate-first); rig pair=%s" % [str(equip.get("label", "?")), str(pair)])


func _platform_identity_text() -> String:
	var model := (OS.get_model_name() if OS.has_method("get_model_name") else "?")
	return "%s/%s" % [OS.get_name(), model]


func _resolve_platform_equipment() -> Dictionary:
	var os_entries: Array = EQUIPMENT.get("platform_backed", {}).get(OS.get_name(), [])
	if os_entries.is_empty():
		return {}
	if OS.get_name() != "Android":
		# Single-host OSes (e.g. Windows/WinRT): the sole entry, no model gate.
		return os_entries[0]
	var model := (OS.get_model_name() if OS.has_method("get_model_name") else "")
	for e in os_entries:
		if str((e as Dictionary).get("model_match", "")) == model:
			return e
	return {}


func _platform_permissions_granted(perms: Array) -> bool:
	# Automated soak: permissions are pre-granted out of band (run_godot.ps1
	# -AndroidGrantPermissions). We only verify -- there is no in-scene dialog.
	if OS.get_name() != "Android" or perms.is_empty():
		return true
	var granted := OS.get_granted_permissions()
	for p in perms:
		if not (str(p) in granted):
			return false
	return true


func _select_platform_pair(endpoints: Array) -> Array:
	# Default selection policy: the first two enumerated cameras. Extensible per
	# equipment entry later (e.g. by facing/role) without hardcoding ids.
	var ids := []
	for ep in endpoints:
		if typeof(ep) == TYPE_DICTIONARY:
			var hw := str((ep as Dictionary).get("hardware_id", ""))
			if hw != "":
				ids.append(hw)
	return ids.slice(0, 2)


func _build_concurrency_truth(cameras: Array, combinations: Array) -> String:
	var camera_dicts := []
	for c in cameras:
		camera_dicts.append({"camera_id": str(c)})
	var combos := []
	for combo in combinations:
		var ids := []
		for c in combo:
			ids.append(str(c))
		combos.append(ids)
	return JSON.stringify({
		"schema_version": 2,
		"cameras": camera_dicts,
		"concurrent_camera_support": {
			"supported": combos.size() > 0,
			"camera_id_combinations": combos,
		},
	})

func _parse_args() -> void:
	# Project-setting default for the provider: Android has no post-'--' user args,
	# so a manual export or run_godot.ps1 can select the provider via this setting.
	# The cmdline below still overrides it on the host.
	var setting_provider := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting_provider != "":
		_provider_arg = setting_provider
	var args := OS.get_cmdline_user_args()
	for raw_arg in args:
		var arg := str(raw_arg)
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
		elif arg.begins_with("--cambang-bench-load-profile="):
			_load_profile_arg = arg.substr("--cambang-bench-load-profile=".length()).strip_edges().to_lower()
		elif arg.begins_with("--cambang-bench-seed="):
			_seed = int(arg.substr("--cambang-bench-seed=".length()))
		elif arg.begins_with("--cambang-bench-phase-sec="):
			_phase_sec_override = maxf(0.05, float(arg.substr("--cambang-bench-phase-sec=".length())))
		elif arg.begins_with("--cambang-bench-human-phase-sec="):
			_human_phase_sec = maxf(0.05, float(arg.substr("--cambang-bench-human-phase-sec=".length())))
		elif arg.begins_with("--cambang-bench-superhuman-phase-sec="):
			_superhuman_phase_sec = maxf(0.05, float(arg.substr("--cambang-bench-superhuman-phase-sec=".length())))
		elif arg.begins_with("--cambang-bench-rig-phase-sec="):
			_rig_phase_sec = maxf(0.05, float(arg.substr("--cambang-bench-rig-phase-sec=".length())))
		elif arg.begins_with("--cambang-bench-exit-hold-sec="):
			_exit_visual_hold_sec = maxf(0.0, float(arg.substr("--cambang-bench-exit-hold-sec=".length())))
		elif arg.begins_with("--cambang-bench-warmup-sec="):
			_warmup_sec = maxf(0.0, float(arg.substr("--cambang-bench-warmup-sec=".length())))
		elif arg.begins_with("--cambang-bench-superhuman-actions-per-tick="):
			_superhuman_actions_per_tick = maxi(1, int(arg.substr("--cambang-bench-superhuman-actions-per-tick=".length())))
		elif arg.begins_with("--cambang-bench-max-inflight-captures="):
			_max_inflight_captures_per_device = maxi(1, int(arg.substr("--cambang-bench-max-inflight-captures=".length())))
		elif arg.begins_with("--cambang-bench-stream-rate="):
			_stream_requests_per_sec_per_stream = maxf(0.0, float(arg.substr("--cambang-bench-stream-rate=".length())))
		elif arg.begins_with("--cambang-bench-capture-rate="):
			_capture_requests_per_sec_per_device = maxf(0.0, float(arg.substr("--cambang-bench-capture-rate=".length())))
		elif arg.begins_with("--cambang-bench-rig-rate="):
			_rig_requests_per_sec = maxf(0.0, float(arg.substr("--cambang-bench-rig-rate=".length())))
		elif arg.begins_with("--cambang-bench-max-pending-materialisations="):
			_max_pending_materializations = maxi(1, int(arg.substr("--cambang-bench-max-pending-materialisations=".length())))
		elif arg.begins_with("--cambang-bench-max-pending-textures="):
			_max_pending_textures = maxi(1, int(arg.substr("--cambang-bench-max-pending-textures=".length())))
		elif arg.begins_with("--cambang-bench-minimum-frames-per-phase="):
			_minimum_frames_per_phase = maxi(1, int(arg.substr("--cambang-bench-minimum-frames-per-phase=".length())))
		elif arg.begins_with("--cambang-bench-minimum-attempts-per-phase="):
			_minimum_attempts_per_phase = maxi(1, int(arg.substr("--cambang-bench-minimum-attempts-per-phase=".length())))
		elif arg.begins_with("--cambang-bench-minimum-completions-per-phase="):
			_minimum_completions_per_phase = maxi(1, int(arg.substr("--cambang-bench-minimum-completions-per-phase=".length())))
		elif arg.begins_with("--cambang-bench-hard-timeout-multiplier="):
			_hard_timeout_multiplier = maxf(1.0, float(arg.substr("--cambang-bench-hard-timeout-multiplier=".length())))
		elif arg.begins_with("--cambang-bench-headless-texture="):
			_materialize_textures_in_headless = _parse_bool(arg.substr("--cambang-bench-headless-texture=".length()))
	_apply_load_profile_defaults()


func _parse_bool(value: String) -> bool:
	var normalized := value.strip_edges().to_lower()
	return normalized == "1" or normalized == "yes" or normalized == "true" or normalized == "on"


func _synthetic_producer_output_form_setting() -> String:
	return str(ProjectSettings.get_setting("cambang/maintainer/synthetic_producer_output_form", "runtime_default")).strip_edges().to_lower()


func _synthetic_producer_output_form_cmdline_selection() -> String:
	const PREFIX := "--cambang-synth-producer-output-form="
	return _single_namespaced_cmdline_selection(PREFIX).strip_edges().to_lower()


func _single_namespaced_cmdline_selection(prefix: String) -> String:
	var found := ""
	for raw_arg in OS.get_cmdline_user_args():
		var text := str(raw_arg)
		if not text.begins_with(prefix):
			continue
		var value := text.substr(prefix.length())
		if found != "":
			return "<duplicate>"
		found = value
	if found != "":
		return found
	for raw_arg in OS.get_cmdline_args():
		var text := str(raw_arg)
		if not text.begins_with(prefix):
			continue
		var value := text.substr(prefix.length())
		if found != "":
			return "<duplicate>"
		found = value
	return found


func _synthetic_producer_output_form_effective_selection() -> String:
	var cmdline_selection := _synthetic_producer_output_form_cmdline_selection()
	if cmdline_selection != "":
		return cmdline_selection
	return _synthetic_producer_output_form_setting()


func _harness_rendering_method_setting() -> String:
	return str(ProjectSettings.get_setting("cambang/maintainer/harness_rendering_method", "")).strip_edges().to_lower()


func _harness_rendering_method_cmdline_selection() -> String:
	const PREFIX := "--cambang-harness-rendering-method="
	return _single_namespaced_cmdline_selection(PREFIX).strip_edges().to_lower()


func _scene870_requested_rendering_method() -> String:
	var cmdline_selection := _harness_rendering_method_cmdline_selection()
	if cmdline_selection != "":
		return cmdline_selection
	var setting_selection := _harness_rendering_method_setting()
	if setting_selection != "":
		return setting_selection
	return _scene870_current_rendering_method()


func _scene870_current_rendering_method() -> String:
	var runtime_method := str(RenderingServer.get_current_rendering_method()).strip_edges().to_lower()
	if runtime_method != "":
		return runtime_method
	return str(ProjectSettings.get_setting("rendering/renderer/rendering_method", "")).strip_edges().to_lower()


func _scene870_cmdline_arg_matches_any_value(flag: String, accepted_values: Array[String]) -> bool:
	var accepted_normalized := []
	for value in accepted_values:
		accepted_normalized.append(str(value).strip_edges().to_lower())
	var args := OS.get_cmdline_args()
	var flag_with_equals := "%s=" % flag
	var index := 0
	while index < args.size():
		var text := str(args[index])
		if text == flag:
			if index + 1 >= args.size():
				return false
			var split_value := str(args[index + 1]).strip_edges().to_lower()
			if accepted_normalized.has(split_value):
				return true
			index += 2
			continue
		if text.begins_with(flag_with_equals):
			var equals_value := text.substr(flag_with_equals.length()).strip_edges().to_lower()
			if accepted_normalized.has(equals_value):
				return true
		index += 1
	return false


func _scene870_rendering_method_is_compatibility(rendering_method: String) -> bool:
	return rendering_method == "compatibility" or rendering_method == "gl_compatibility" or rendering_method == "opengl3"


func _scene870_is_compatibility_renderer() -> bool:
	var rendering_method := _scene870_requested_rendering_method()
	if _scene870_rendering_method_is_compatibility(rendering_method):
		return true
	if _scene870_cmdline_arg_matches_any_value("--rendering-method", ["compatibility", "gl_compatibility"]):
		return true
	return _scene870_cmdline_arg_matches_any_value("--rendering-driver", ["opengl3"])


func _apply_load_profile_defaults() -> void:
	if _load_profile_arg not in LOAD_PROFILE_NAMES:
		_load_profile_arg = LOAD_PROFILE_DEFAULT
	var defaults := _load_profile_defaults(_load_profile_arg)
	if _phase_sec_override >= 0.0:
		_human_phase_sec = _phase_sec_override
		_superhuman_phase_sec = _phase_sec_override
		_rig_phase_sec = _phase_sec_override
	if _stream_requests_per_sec_per_stream < 0.0:
		_stream_requests_per_sec_per_stream = float(defaults.get("stream_to_image_requests_per_sec_per_stream", 0.0))
	if _capture_requests_per_sec_per_device < 0.0:
		_capture_requests_per_sec_per_device = float(defaults.get("device_capture_requests_per_sec_per_device", 0.0))
	if _rig_requests_per_sec < 0.0:
		_rig_requests_per_sec = float(defaults.get("rig_capture_requests_per_sec", 0.0))
	if _max_pending_materializations < 0:
		_max_pending_materializations = int(defaults.get("max_pending_materializations", 1))
	if _max_pending_textures < 0:
		_max_pending_textures = int(defaults.get("max_pending_textures", 1))
	if _minimum_frames_per_phase < 0:
		_minimum_frames_per_phase = int(defaults.get("minimum_frames_per_phase", 1))
	if _minimum_attempts_per_phase < 0:
		_minimum_attempts_per_phase = int(defaults.get("minimum_attempts_per_phase", 1))
	if _minimum_completions_per_phase < 0:
		_minimum_completions_per_phase = int(defaults.get("minimum_completions_per_phase", 1))
	if _hard_timeout_multiplier < 0.0:
		_hard_timeout_multiplier = float(defaults.get("hard_timeout_multiplier", 2.0))


func _load_profile_defaults(profile: String) -> Dictionary:
	match profile:
		LOAD_PROFILE_HUMAN:
			return {
				"stream_to_image_requests_per_sec_per_stream": 0.1, #2.5,
				"device_capture_requests_per_sec_per_device": 0.9,
				"rig_capture_requests_per_sec": 0.35,
				"max_pending_materializations": 4,
				"max_pending_textures": 4,
				"minimum_frames_per_phase": 4,
				"minimum_attempts_per_phase": 1,
				"minimum_completions_per_phase": 1,
				"hard_timeout_multiplier": 2.25,
			}
		LOAD_PROFILE_ELEVATED:
			return {
				"stream_to_image_requests_per_sec_per_stream": 4.0,
				"device_capture_requests_per_sec_per_device": 1.6,
				"rig_capture_requests_per_sec": 0.55,
				"max_pending_materializations": 6,
				"max_pending_textures": 6,
				"minimum_frames_per_phase": 4,
				"minimum_attempts_per_phase": 1,
				"minimum_completions_per_phase": 1,
				"hard_timeout_multiplier": 2.5,
			}
		LOAD_PROFILE_STRESS:
			return {
				"stream_to_image_requests_per_sec_per_stream": 10.0,
				"device_capture_requests_per_sec_per_device": 4.0,
				"rig_capture_requests_per_sec": 1.5,
				"max_pending_materializations": 12,
				"max_pending_textures": 12,
				"minimum_frames_per_phase": 4,
				"minimum_attempts_per_phase": 1,
				"minimum_completions_per_phase": 1,
				"hard_timeout_multiplier": 2.75,
			}
		_:
			return {
				"stream_to_image_requests_per_sec_per_stream": 6.0,
				"device_capture_requests_per_sec_per_device": 2.2,
				"rig_capture_requests_per_sec": 0.75,
				"max_pending_materializations": 8,
				"max_pending_textures": 8,
				"minimum_frames_per_phase": 4,
				"minimum_attempts_per_phase": 1,
				"minimum_completions_per_phase": 1,
				"hard_timeout_multiplier": 2.5,
			}


func _build_ui() -> void:
	var root := MarginContainer.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.add_theme_constant_override("margin_left", 8)
	root.add_theme_constant_override("margin_top", 8)
	root.add_theme_constant_override("margin_right", 8)
	root.add_theme_constant_override("margin_bottom", 8)
	add_child(root)

	var main := VBoxContainer.new()
	main.add_theme_constant_override("separation", 6)
	root.add_child(main)

	_header_label = Label.new()
	_header_label.text = "Scene 870 — to_image soak benchmark"
	main.add_child(_header_label)

	_stats_label = Label.new()
	_stats_label.text = "starting..."
	_stats_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	main.add_child(_stats_label)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	main.add_child(scroll)

	var content := VBoxContainer.new()
	content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	content.add_theme_constant_override("separation", 8)
	scroll.add_child(content)

	var stream_band := HBoxContainer.new()
	stream_band.add_theme_constant_override("separation", 8)
	content.add_child(stream_band)
	_add_device_stream_column(stream_band, DEV_A)
	_add_device_stream_column(stream_band, DEV_B)

	var capture_band := HBoxContainer.new()
	capture_band.add_theme_constant_override("separation", 8)
	content.add_child(capture_band)
	_add_device_capture_column(capture_band, DEV_A)
	_add_device_capture_column(capture_band, DEV_B)

	var rig_band := HBoxContainer.new()
	rig_band.add_theme_constant_override("separation", 8)
	content.add_child(rig_band)
	_add_rig_capture_column(rig_band, DEV_A)
	_add_rig_capture_column(rig_band, DEV_B)

	_log_label = RichTextLabel.new()
	_log_label.custom_minimum_size = Vector2(0, 120)
	_log_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_log_label.fit_content = false
	main.add_child(_log_label)


func _add_device_stream_column(parent: Control, device_key: String) -> void:
	var device_label := str(_devices[device_key].get("label", device_key))
	var col := VBoxContainer.new()
	col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	col.add_theme_constant_override("separation", 4)
	parent.add_child(col)

	var live := _make_artifact_panel("%s live display_view" % device_label, Vector2(420, 236), 56)
	col.add_child(live["panel"])
	_stream_live_rects[device_key] = live["rect"]
	_stream_live_labels[device_key] = live["facts"]

	var img := _make_artifact_panel("%s latest Stream.to_image()" % device_label, Vector2(300, 169), 64)
	col.add_child(img["panel"])
	_stream_image_rects[device_key] = img["rect"]
	_stream_image_labels[device_key] = img["facts"]
	_prime_fixed_panel_texture(_stream_image_textures, device_key, img["rect"])


func _add_device_capture_column(parent: Control, device_key: String) -> void:
	var device_label := str(_devices[device_key].get("label", device_key))
	var col := VBoxContainer.new()
	col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	col.add_theme_constant_override("separation", 4)
	parent.add_child(col)

	var cap := _make_artifact_panel("%s latest device capture metered" % device_label, Vector2(360, 203), 72)
	col.add_child(cap["panel"])
	_capture_rects[device_key] = cap["rect"]
	_capture_labels[device_key] = cap["facts"]
	_prime_fixed_panel_texture(_capture_textures, device_key, cap["rect"])

	var strip_panel := PanelContainer.new()
	strip_panel.custom_minimum_size = Vector2(0, 105)
	col.add_child(strip_panel)
	var strip_scroll := ScrollContainer.new()
	strip_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	strip_panel.add_child(strip_scroll)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	strip_scroll.add_child(row)
	_capture_rows[device_key] = row


func _add_rig_capture_column(parent: Control, device_key: String) -> void:
	var device_label := str(_devices[device_key].get("label", device_key))
	var col := VBoxContainer.new()
	col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	col.add_theme_constant_override("separation", 4)
	parent.add_child(col)

	var rig := _make_artifact_panel("Rig capture — %s metered" % device_label, Vector2(360, 203), 72)
	col.add_child(rig["panel"])
	_rig_rects[device_key] = rig["rect"]
	_rig_labels[device_key] = rig["facts"]
	_prime_fixed_panel_texture(_rig_textures, device_key, rig["rect"])

	var strip_panel := PanelContainer.new()
	strip_panel.custom_minimum_size = Vector2(0, 105)
	col.add_child(strip_panel)
	var strip_scroll := ScrollContainer.new()
	strip_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	strip_panel.add_child(strip_scroll)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	strip_scroll.add_child(row)
	_rig_rows[device_key] = row


func _make_artifact_panel(title: String, texture_size: Vector2, facts_height: int) -> Dictionary:
	var panel := PanelContainer.new()
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 3)
	panel.add_child(box)
	var header := Label.new()
	header.text = title
	box.add_child(header)
	var rect := TextureRect.new()
	rect.custom_minimum_size = texture_size
	rect.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rect.expand_mode = TextureRect.EXPAND_FIT_WIDTH_PROPORTIONAL
	rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	box.add_child(rect)
	var facts := Label.new()
	facts.custom_minimum_size = Vector2(0, facts_height)
	facts.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	facts.text = "waiting..."
	box.add_child(facts)
	return {"panel": panel, "rect": rect, "facts": facts}

func _poll_setup() -> void:
	if Time.get_ticks_msec() - _setup_started_ms > SETUP_TIMEOUT_MS:
		_fail("setup timeout: devices/streams/rig did not become ready")
		return

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return
	_refresh_device_snapshot_cache(snapshot)

	# Stage 1: engage the two enumerated devices (once). Waits for the provider
	# baseline to advertise them first.
	if not _topology_engaged:
		var endpoints: Array = CamBANGServer.enumerate_devices()
		if endpoints.size() < 2:
			return
		if not _engage_setup_devices(endpoints):
			return  # _fail already reported the hard error
		_topology_engaged = true
		return

	# Stage 2: once both engaged devices are live, create + start a PREVIEW stream
	# on each (once). Device id/handle come from the engaged handles, not the
	# snapshot, so create_stream runs on the direct handle it needs.
	if not _topology_streamed:
		if not (_setup_device_live(DEV_A) and _setup_device_live(DEV_B)):
			return
		if not _create_setup_streams():
			return
		_topology_streamed = true
		return

	# Stage 3: form the rig from the two engaged devices via the public API (once).
	if _rig == null:
		_rig = CamBANGServer.create_rig(PackedStringArray([
			str(_devices[DEV_A].get("hardware_id", "")),
			str(_devices[DEV_B].get("hardware_id", "")),
		]))
		if _rig == null:
			_fail("setup: create_rig returned null (concurrency truth not authorizing the combination?)")
			return
		_log("setup: rig formed via create_rig id=%d" % int(_rig.get_id()))
		return

	# Stage 4: bind display views (streams must be producing), advance when ready.
	# Device/stream ids and the rig handle already come from the API calls above.
	_update_stream_display_views(true)
	if _setup_ready():
		_log("setup complete: devices, streams, rig and display views are ready")
		_begin_next_bundle()


func _setup_device_live(device_key: String) -> bool:
	var info: Dictionary = _devices[device_key]
	if int(info.get("device_id", 0)) <= 0:
		return false
	var dev = info.get("device", null)
	if dev == null:
		return false
	if dev.has_method("is_live"):
		return bool(dev.is_live())
	return true


func _engage_setup_devices(endpoints: Array) -> bool:
	# Map the enumerated endpoints onto Device A/B by hardware_id and engage each.
	var enumerated := {}
	for ep in endpoints:
		if typeof(ep) == TYPE_DICTIONARY:
			enumerated[str((ep as Dictionary).get("hardware_id", ""))] = true
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		var hw := str(info.get("hardware_id", ""))
		if not enumerated.has(hw):
			_fail("setup: expected device %s was not enumerated" % hw)
			return false
		var dev = CamBANGServer.get_device_for_hardware_id(hw)
		if dev == null or int(dev.engage()) != OK:
			_fail("setup: failed to engage %s" % hw)
			return false
		# Keep the direct engaged handle (create_stream needs it, not a
		# get_device() snapshot handle) and take the instance id from it directly.
		info["device"] = dev
		info["device_id"] = int(dev.get_instance_id())
		_devices[device_key] = info
	return true


func _create_setup_streams() -> bool:
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		var dev = info.get("device", null)
		if dev == null:
			_fail("setup: device handle missing for %s at stream creation" % device_key)
			return false
		# Intent PREVIEW at the still-capture geometry -- the live preview streams
		# the scenario used, now created directly through the public API.
		var stream_def := {
			"intent": CamBANGStream.INTENT_PREVIEW,
			"profile": {
				"width": STILL_PROFILE_WIDTH,
				"height": STILL_PROFILE_HEIGHT,
				"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
			},
		}
		# Synthetic-only: a distinct per-device stream picture so Device A and B are
		# visibly distinct sources (the scenario used to author this). Platform
		# providers reject a picture per supports_stream_picture_updates(), so it
		# is only sent for synthetic.
		if _provider_arg == "synthetic":
			stream_def["picture"] = _SYNTHETIC_STREAM_PICTURE.get(device_key, {})
		var stream = dev.create_stream(stream_def)
		if stream == null:
			_fail("setup: create_stream returned null for %s" % device_key)
			return false
		if int(stream.start()) != OK:
			_fail("setup: stream.start() failed for %s" % device_key)
			return false
		# Keep the handle so the direct stream is not torn down mid-run.
		info["stream"] = stream
		info["stream_id"] = int(stream.get_stream_id())
		_devices[device_key] = info

		# Synthetic-only: a distinct per-device CAPTURE picture too, so the captured
		# images (and the rig members) are visibly distinct, not just the live
		# streams. Device-scoped, gated on supports_capture_picture_updates().
		if _provider_arg == "synthetic":
			var cap_picture: Dictionary = _SYNTHETIC_CAPTURE_PICTURE.get(device_key, {})
			if not cap_picture.is_empty():
				var cap_err := int(dev.set_capture_picture(cap_picture))
				if cap_err != OK:
					_fail("setup: set_capture_picture failed for %s (%d)" % [device_key, cap_err])
					return false
	return true


func _setup_ready() -> bool:
	if _rig == null:
		return false
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		if int(info.get("device_id", 0)) <= 0:
			return false
		if info.get("device", null) == null:
			return false
		if int(info.get("stream_id", 0)) <= 0:
			return false
		if not bool(info.get("live_display_bound", false)):
			return false
	return true


func _update_stream_display_views(require_bind: bool) -> void:
	var now_us := _now_us()
	var observe_result_advancement := now_us - _last_stream_observation_us >= 10000
	if observe_result_advancement:
		_last_stream_observation_us = now_us
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		var label = _stream_live_labels.get(device_key, null)
		var bound := bool(info.get("live_display_bound", false))
		var prior_path_kind := int(info.get("live_display_path_kind", 0))
		var should_recheck := require_bind or not bound
		if not should_recheck and prior_path_kind != int(CamBANGStreamResult.DISPLAY_PATH_RETAINED_GPU_BACKING):
			var last_recheck_us := int(info.get("live_display_last_recheck_us", 0))
			should_recheck = (now_us - last_recheck_us) >= STREAM_DISPLAY_REBIND_INTERVAL_US
		var stream_id := int(info.get("stream_id", 0))
		if stream_id <= 0:
			continue
		var stream_result = null
		if observe_result_advancement or should_recheck:
			stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
			if stream_result != null and observe_result_advancement:
				# Provider-agnostic stream-advancement: count distinct successive
				# per-frame acquisition marks. The mark advances per delivered frame
				# and repeats when no new frame arrived between polls, so each change
				# is one observed delivery (native_reported on platform,
				# virtual-camera-authored on synthetic; both monotonic). This replaces
				# the synthetic-only synthetic_stream_result_revisions channel, which
				# was empty -- hence observed_updates=0 -- on platform-backed runs.
				var mark := _stream_acquisition_mark(stream_result)
				if mark != 0:
					var prior_mark := int(info.get("stream_last_observed_mark", 0))
					if prior_mark != 0 and mark != prior_mark:
						if int(info.get("stream_observation_first_us", 0)) == 0:
							info["stream_observation_first_us"] = now_us
						info["stream_observed_changes"] = int(info.get("stream_observed_changes", 0)) + 1
						info["stream_observation_last_us"] = now_us
					info["stream_last_observed_mark"] = mark
		if not should_recheck and bound:
			_set_label_text_if_changed(label, "%s\nstream_id=%d\nobserved_updates=%d\nobserved_update_fps=%.2f" % [
				str(info.get("label", device_key)),
				stream_id,
				int(info.get("stream_observed_changes", 0)),
				_stream_observed_fps(device_key),
			])
			_devices[device_key] = info
			continue
		if stream_result == null:
			stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if stream_result == null:
			_devices[device_key] = info
			continue
		info["live_display_last_recheck_us"] = now_us
		if stream_result.has_method("get_display_view_path_kind"):
			info["live_display_path_kind"] = int(stream_result.get_display_view_path_kind())
		var display_view: Variant = stream_result.get_display_view()
		if display_view == null:
			_devices[device_key] = info
			continue
		if display_view is Texture2D:
			var rect = _stream_live_rects.get(device_key, null)
			var current_texture = rect.texture if rect is TextureRect else null
			if require_bind or not bound or current_texture != display_view:
				_set_texture_if_changed(rect, display_view)
				info["live_display_bound"] = true
			_set_label_text_if_changed(label, "%s\nstream_id=%d\nobserved_updates=%d\nobserved_update_fps=%.2f" % [
				str(info.get("label", device_key)),
				stream_id,
				int(info.get("stream_observed_changes", 0)),
				_stream_observed_fps(device_key),
			])
		_devices[device_key] = info


func _stream_acquisition_mark(stream_result) -> int:
	# Per-frame acquisition mark from the stream result's camera facts, or 0 when
	# absent. add_acquisition_timing_camera_fact() omits the key entirely if the
	# provider did not report timing, so a missing key reads as 0 (no advancement).
	if stream_result == null or not stream_result.has_method("get_camera_facts"):
		return 0
	var facts: Dictionary = stream_result.get_camera_facts()
	var at_v: Variant = facts.get("acquisition_timing", null)
	if typeof(at_v) != TYPE_DICTIONARY:
		return 0
	return int((at_v as Dictionary).get("acquisition_mark", 0))


func _stream_observed_fps(device_key: String) -> float:
	var info: Dictionary = _devices[device_key]
	var first_us := int(info.get("stream_observation_first_us", 0))
	var last_us := int(info.get("stream_observation_last_us", 0))
	if first_us <= 0 or last_us <= first_us:
		return 0.0
	var elapsed_sec := float(last_us - first_us) / 1000000.0
	if elapsed_sec <= 0.0:
		return 0.0
	return float(int(info.get("stream_observed_changes", 0))) / elapsed_sec

func _begin_next_bundle() -> void:
	_current_bundle_index += 1
	if _current_bundle_index >= _bundle_definitions().size():
		if not _acq_probe_attempted:
			_begin_acquisition_session_settlement_probe()
			return
		_finish(0, false)
		return
	_current_bundle = _bundle_definitions()[_current_bundle_index]
	_profile_requested = false
	_profile_before_by_device.clear()
	_state = PHASE_PROFILE
	_profile_started_ms = Time.get_ticks_msec()
	_log("bundle start: %s members=%d ev=%s" % [
		str(_current_bundle.get("label", "")),
		int(_current_bundle.get("member_count", 0)),
		str(_current_bundle.get("ev_milli", [])),
	])


func _begin_acquisition_session_settlement_probe() -> void:
	_acq_probe_attempted = true
	_state = PHASE_SETTLEMENT_PROBE
	_acq_probe_stage = "capture"
	_acq_probe_started_us = _now_us()
	_acq_probe_settle_started_us = 0
	_acq_probe_settle_deadline_us = 0
	_acq_probe_bundle_label = str(_current_bundle.get("label", ""))
	_acq_probe_required_member_count = int(_current_bundle.get("member_count", 0))
	_acq_probe_devices = {}
	if _acq_probe_bundle_label != "ev5_-2_-1_0_1_2" or _acq_probe_required_member_count != 5:
		_fail("settlement probe failed: EV5 bundle was not active at probe start")
		return
	_freeze_benchmark_metrics()
	var reports := _backing_plan_acquisition_session_reports_summary(
		CamBANGServer.get_synthetic_metrics_snapshot())
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		_acq_probe_devices[device_key] = {
			"device_key": device_key,
			"device_id": int(info.get("device_id", 0)),
			"acquisition_session_id": 0,
			"capture_id": 0,
			"trigger_status": "probe_not_attempted",
			"returned_member_count": 0,
			"materialized_member_count": 0,
			"materialized_member_indices": [],
			"materialization_failed_indices": [],
			"capture_complete": false,
			"capture_failed": false,
			"materialization_complete": false,
		}
		for report_v in reports:
			if typeof(report_v) != TYPE_DICTIONARY:
				continue
			var report: Dictionary = report_v
			if int(report.get("device_instance_id", 0)) != int(info.get("device_id", 0)):
				continue
			_acq_probe_devices[device_key]["acquisition_session_id"] = int(report.get("acquisition_session_id", 0))
			break
	for device_key in [DEV_A, DEV_B]:
		_queue_settlement_probe_capture(device_key)
	_log("settlement probe start: bundle=%s required_members=%d" % [
		_acq_probe_bundle_label,
		_acq_probe_required_member_count,
	])


func _poll_acquisition_session_settlement_probe() -> void:
	if not _acq_probe_attempted or _done:
		return
	if _acq_probe_stage == "capture":
		if not _acq_probe_all_captures_finished():
			return
		_acq_probe_stage = "settle"
		_acq_probe_settle_started_us = _now_us()
		_acq_probe_settle_deadline_us = _acq_probe_settle_started_us + SETTLEMENT_PROBE_SETTLE_TIMEOUT_US
		return
	if _acq_probe_stage != "settle":
		return
	if _acq_probe_all_devices_settled() or _now_us() >= _acq_probe_settle_deadline_us:
		_finish(0, false)


func _acq_probe_all_captures_finished() -> bool:
	for device_key in [DEV_A, DEV_B]:
		var entry_v = _acq_probe_devices.get(device_key, {})
		if typeof(entry_v) != TYPE_DICTIONARY:
			return false
		var entry: Dictionary = entry_v
		if not bool(entry.get("capture_complete", false)) and not bool(entry.get("capture_failed", false)):
			return false
	return true


func _acq_probe_all_devices_settled() -> bool:
	for device_key in [DEV_A, DEV_B]:
		var entry_v = _acq_probe_devices.get(device_key, {})
		if typeof(entry_v) != TYPE_DICTIONARY:
			return false
		var entry: Dictionary = entry_v
		if not bool(entry.get("capture_complete", false)):
			return false
		if int(entry.get("returned_member_count", 0)) < _acq_probe_required_member_count:
			return false
		if int(entry.get("materialized_member_count", 0)) < _acq_probe_required_member_count:
			return false
		var report := _backing_plan_acquisition_session_report_for_device_id(_acq_probe_current_reports(), int(entry.get("device_id", 0)))
		if report.is_empty():
			return false
		if bool(report.get("evaluator_active", true)):
			return false
		var current_candidate_evidence: Dictionary = report.get("current_candidate_evidence", {})
		if str(current_candidate_evidence.get("capture_evidence_incomplete_reason", "")) != "none":
			return false
	return true


func _acq_probe_current_reports() -> Array:
	var synthetic_metrics = CamBANGServer.get_synthetic_metrics_snapshot()
	return _backing_plan_acquisition_session_reports_summary(synthetic_metrics)


func _backing_plan_acquisition_session_report_for_device_id(reports: Array, device_id: int) -> Dictionary:
	for report_v in reports:
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if int(report.get("device_instance_id", 0)) == device_id:
			return report
	return {}


func _queue_settlement_probe_capture(device_key: String) -> void:
	var info: Dictionary = _devices[device_key]
	var device = info.get("device", null)
	if device == null:
		_acq_probe_mark_capture_failure(device_key, "capture_failed")
		return
	var baseline_capture_id := _device_last_capture_id(device_key)
	var trigger_start := _now_us()
	var err := int(device.trigger_capture())
	var trigger_end := _now_us()
	if err != OK:
		_acq_probe_mark_capture_failure(device_key, "capture_failed")
		_acq_probe_devices[device_key]["capture_error"] = err
		return
	info["inflight_captures"] = int(info.get("inflight_captures", 0)) + 1
	_devices[device_key] = info
	_capture_jobs.append({
		"kind": "device_capture",
		"device_key": device_key,
		"device": device,
		"device_id": int(info.get("device_id", 0)),
		"request_us": _acq_probe_started_us,
		"trigger_start_us": trigger_start,
		"trigger_end_us": trigger_end,
		"trigger_call_us": trigger_end - trigger_start,
		"baseline_capture_id": baseline_capture_id,
		"bundle_label": _acq_probe_bundle_label,
		"expected_member_count": _acq_probe_required_member_count,
		"phase_index": -1,
		"visual_sequence": _current_phase_visual_sequence,
		"is_settlement_probe": true,
	})


func _acq_probe_mark_capture_failure(device_key: String, status: String) -> void:
	var entry_v = _acq_probe_devices.get(device_key, {})
	if typeof(entry_v) != TYPE_DICTIONARY:
		return
	var entry: Dictionary = entry_v
	entry["trigger_status"] = status
	entry["capture_failed"] = true
	_acq_probe_devices[device_key] = entry


func _bundle_definitions() -> Array:
	return [
		{
			"label": "metered_only",
			"member_count": 1,
			"ev_milli": [0],
			"additional_ev_milli": [],
		},
		{
			"label": "ev5_-2_-1_0_1_2",
			"member_count": 5,
			"ev_milli": [-2000, -1000, 0, 1000, 2000],
			"additional_ev_milli": [-2000, -1000, 1000, 2000],
		},
	]


func _poll_profile_application() -> void:
	if not _profile_requested:
		_apply_current_bundle_profile()
		return
	if _profiles_visible_for_current_bundle():
		_state = PHASE_WARMUP
		_warmup_started_us = _now_us()
		_log("profile visible: %s" % str(_current_bundle.get("label", "")))
		return
	if Time.get_ticks_msec() - _profile_started_ms > PROFILE_APPLY_TIMEOUT_MS:
		_fail("profile timeout: expected still profile did not become visible for %s" % str(_current_bundle.get("label", "")))


func _apply_current_bundle_profile() -> void:
	var members := _make_still_members_for_bundle(_current_bundle)
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		var device = info.get("device", null)
		if device == null:
			_fail("profile apply failed: missing device object for %s" % device_key)
			return
		var still_profile_before := _get_device_still_profile(int(info.get("device_id", 0)))
		_profile_before_by_device[device_key] = int(still_profile_before.get("version", -1))
		# Members alone are not enough on a platform-backed provider: a single-member
		# (metered_only) bundle matches the device default, which carries the wrong
		# geometry (Camera2's 640x480), so the apply must still run to pin STILL_PROFILE.
		if _still_profile_matches_members(still_profile_before, members) and _still_profile_geometry_ok(still_profile_before):
			continue
		var request := _make_still_profile_request(members, still_profile_before)
		var err := int(device.set_still_capture_profile(request))
		if err != OK:
			_fail("profile apply failed: %s set_still_capture_profile returned %d" % [device_key, err])
			return
	_profile_requested = true


func _make_still_profile_request(members: Array, visible_still_profile: Dictionary) -> Dictionary:
	# Camera2/WinRT couple the still to the live feed geometry: a session's output
	# set is fixed at creation and one device holds only one session, so a still
	# whose geometry differs from the producing stream is refused
	# (ERR_PLATFORM_CONSTRAINT) rather than rebuilding the session under a live feed
	# (see the Camera2 note in docs/dev/build_and_scaffolding.md). The soak's streams
	# are created at STILL_PROFILE, so on a platform-backed provider pin the capture
	# there too instead of inheriting the provider's default visible still geometry
	# (e.g. Camera2's 640x480 capture-template default, which drove the mismatch).
	# Synthetic has no such coupling; leave its geometry exactly as before so the
	# approved baseline is unchanged.
	var width: int = int(visible_still_profile.get("width", STILL_PROFILE_WIDTH))
	var height: int = int(visible_still_profile.get("height", STILL_PROFILE_HEIGHT))
	if _provider_arg != "synthetic":
		width = STILL_PROFILE_WIDTH
		height = STILL_PROFILE_HEIGHT
	var format_fourcc: int = int(visible_still_profile.get("format_fourcc", visible_still_profile.get("format", CamBANGServer.PIXEL_FORMAT_RGBA)))
	if width <= 0:
		width = STILL_PROFILE_WIDTH
	if height <= 0:
		height = STILL_PROFILE_HEIGHT
	if format_fourcc <= 0:
		format_fourcc = CamBANGServer.PIXEL_FORMAT_RGBA
	return {
		"width": width,
		"height": height,
		"format_fourcc": format_fourcc,
		"still_image_bundle": {
			"members": members,
		},
	}


func _make_still_members_for_bundle(bundle: Dictionary) -> Array:
	# The public still-image bundle contract requires member 0 to be the
	# DEFAULT_METERED image at 0 mEV. The five-image visual/benchmark bundle is
	# therefore represented as member 0 plus four ADDITIONAL_BRACKET members; the
	# UI can still sort/display them as [-2, -1, 0, +1, +2].
	var out: Array = [
		{
			"image_member_index": 0,
			"role": "DEFAULT_METERED",
			"intended_exposure_compensation_milli_ev": 0,
		},
	]
	var additional_v: Variant = bundle.get("additional_ev_milli", [])
	if typeof(additional_v) != TYPE_ARRAY:
		return out
	var additional_evs: Array = additional_v
	for i in range(additional_evs.size()):
		var ev: int = int(additional_evs[i])
		out.append({
			"image_member_index": i + 1,
			"role": "ADDITIONAL_BRACKET",
			"intended_exposure_compensation_milli_ev": ev,
		})
	return out


func _profiles_visible_for_current_bundle() -> bool:
	var members := _make_still_members_for_bundle(_current_bundle)
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		var device_id := int(info.get("device_id", 0))
		var still_profile := _get_device_still_profile(device_id)
		if still_profile.is_empty():
			return false
		if not _still_profile_matches_members(still_profile, members):
			return false
		if not _still_profile_geometry_ok(still_profile):
			return false
		var before := int(_profile_before_by_device.get(device_key, -1))
		if before >= 0 and int(still_profile.get("version", -1)) < before:
			return false
	return true


func _get_device_still_profile(device_instance_id: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return {}
	_refresh_device_snapshot_cache(snapshot)
	var rec_v: Variant = _latest_device_snapshot_by_id.get(device_instance_id, {})
	if typeof(rec_v) != TYPE_DICTIONARY:
		return {}
	var rec: Dictionary = rec_v
	var capture_profile_v: Variant = rec.get("capture_profile", null)
	if typeof(capture_profile_v) != TYPE_DICTIONARY:
		return {}
	var capture_profile: Dictionary = capture_profile_v
	var still_v: Variant = capture_profile.get("still", null)
	if typeof(still_v) != TYPE_DICTIONARY:
		return {}
	return still_v


func _refresh_device_snapshot_cache(snapshot: Dictionary) -> void:
	_latest_device_snapshot_by_id.clear()
	for dv in snapshot.get("devices", []):
		if typeof(dv) != TYPE_DICTIONARY:
			continue
		var rec: Dictionary = dv
		var device_id := int(rec.get("instance_id", 0))
		if device_id <= 0:
			continue
		_latest_device_snapshot_by_id[device_id] = rec
		var device_key := _device_key_for_id(device_id)
		if device_key == "":
			continue
		var info: Dictionary = _devices[device_key]
		info["last_capture_id"] = int(rec.get("last_capture_id", int(info.get("last_capture_id", 0))))
		_devices[device_key] = info


func _still_profile_matches_members(still_profile: Dictionary, expected_members: Array) -> bool:
	var bundle_v: Variant = still_profile.get("still_image_bundle", null)
	if typeof(bundle_v) != TYPE_DICTIONARY:
		return false
	var bundle: Dictionary = bundle_v
	var observed_v: Variant = bundle.get("members", null)
	if typeof(observed_v) != TYPE_ARRAY:
		return false
	var observed: Array = observed_v
	if observed.size() != expected_members.size():
		return false
	for i in range(expected_members.size()):
		if typeof(observed[i]) != TYPE_DICTIONARY or typeof(expected_members[i]) != TYPE_DICTIONARY:
			return false
		var o: Dictionary = observed[i]
		var e: Dictionary = expected_members[i]
		if int(o.get("image_member_index", -1)) != int(e.get("image_member_index", -1)):
			return false
		if _role_to_code(o.get("role", null)) != _role_to_code(e.get("role", null)):
			return false
		if int(o.get("intended_exposure_compensation_milli_ev", 0)) != int(e.get("intended_exposure_compensation_milli_ev", 0)):
			return false
	return true


func _still_profile_geometry_ok(still_profile: Dictionary) -> bool:
	# Synthetic keeps its historical still geometry (no feed coupling), so it never
	# blocks the members-match skip. Platform-backed providers must capture at
	# STILL_PROFILE to match the producing feed (see _make_still_profile_request).
	if _provider_arg == "synthetic":
		return true
	return int(still_profile.get("width", -1)) == STILL_PROFILE_WIDTH and int(still_profile.get("height", -1)) == STILL_PROFILE_HEIGHT


func _role_to_code(role_v: Variant) -> int:
	if typeof(role_v) == TYPE_INT:
		return int(role_v)
	if typeof(role_v) == TYPE_STRING:
		var role_s := String(role_v)
		if role_s == "DEFAULT_METERED":
			return CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED
		if role_s == "ADDITIONAL_BRACKET":
			return CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET
	return -1


func _poll_warmup() -> void:
	if _elapsed_sec_since(_warmup_started_us) < _warmup_sec:
		return
	if not _preflight_done:
		_begin_preflight()
		return
	_phases = _make_phase_matrix()
	_phase_index = -1
	_begin_next_phase()


func _begin_preflight() -> void:
	_state = PHASE_PREFLIGHT
	_preflight_stage = "capture"
	_preflight_started_us = _now_us()
	_preflight_observe_started_us = 0
	_preflight_devices = {}
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		_preflight_devices[device_key] = {
			"device_key": device_key,
			"device_id": int(info.get("device_id", 0)),
			"capture_complete": false,
			"capture_failed": false,
			"capture_id": 0,
			"returned_member_count": 0,
		}
		_queue_preflight_capture(device_key)
	_log("preflight start: one initial capture per device, then streams-only observation")


func _queue_preflight_capture(device_key: String) -> void:
	var info: Dictionary = _devices[device_key]
	var device = info.get("device", null)
	if device == null:
		_mark_preflight_capture_failure(device_key)
		return
	var baseline_capture_id := _device_last_capture_id(device_key)
	var trigger_start := _now_us()
	var err := int(device.trigger_capture())
	var trigger_end := _now_us()
	if err != OK:
		_mark_preflight_capture_failure(device_key)
		return
	info["inflight_captures"] = int(info.get("inflight_captures", 0)) + 1
	_devices[device_key] = info
	_capture_jobs.append({
		"kind": "device_capture",
		"device_key": device_key,
		"device": device,
		"device_id": int(info.get("device_id", 0)),
		"request_us": _preflight_started_us,
		"trigger_start_us": trigger_start,
		"trigger_end_us": trigger_end,
		"trigger_call_us": trigger_end - trigger_start,
		"baseline_capture_id": baseline_capture_id,
		"bundle_label": str(_current_bundle.get("label", "")),
		"expected_member_count": int(_current_bundle.get("member_count", 0)),
		"phase_index": -1,
		"visual_sequence": _current_phase_visual_sequence,
		"is_preflight_capture": true,
	})


func _mark_preflight_capture_failure(device_key: String) -> void:
	var entry_v = _preflight_devices.get(device_key, {})
	if typeof(entry_v) != TYPE_DICTIONARY:
		return
	var entry: Dictionary = entry_v
	entry["capture_failed"] = true
	_preflight_devices[device_key] = entry


func _poll_preflight() -> void:
	if _preflight_stage == "capture":
		if not _preflight_all_captures_finished():
			return
		_preflight_stage = "observe"
		_preflight_observe_started_us = _now_us()
		_log("preflight capture population complete: observing live streams before benchmark")
		return
	if _preflight_stage != "observe":
		return
	if _elapsed_sec_since(_preflight_observe_started_us) < PREFLIGHT_STREAMS_ONLY_SEC:
		return
	_preflight_done = true
	_state = PHASE_WARMUP
	_warmup_started_us = _now_us() - int(_warmup_sec * 1000000.0)
	_log("preflight complete: entering benchmark phase matrix")


func _preflight_all_captures_finished() -> bool:
	for device_key in [DEV_A, DEV_B]:
		var entry_v = _preflight_devices.get(device_key, {})
		if typeof(entry_v) != TYPE_DICTIONARY:
			return false
		var entry: Dictionary = entry_v
		if not bool(entry.get("capture_complete", false)) and not bool(entry.get("capture_failed", false)):
			return false
	return true


func _make_phase_matrix() -> Array:
	var phases := []
	var scopes := [
		{"scope": "device_a", "devices": [DEV_A]},
		{"scope": "device_b", "devices": [DEV_B]},
		{"scope": "both_devices", "devices": [DEV_A, DEV_B]},
	]
	for mode in ["human", "superhuman"]:
		for scope in scopes:
			phases.append(_phase_def(mode, scope, true, false, false, "Capture"))
			phases.append(_phase_def(mode, scope, false, true, false, "Stream"))
			phases.append(_phase_def(mode, scope, true, true, false, "Capture & Stream"))
	phases.append(_phase_def("rig", {"scope": "rig", "devices": [DEV_A, DEV_B]}, false, false, true, "Rig-capture"))
	phases.append(_phase_def("rig", {"scope": "rig_plus_capture", "devices": [DEV_A, DEV_B]}, true, false, true, "Rig-capture & Capture"))
	phases.append(_phase_def("rig", {"scope": "rig_plus_capture_stream", "devices": [DEV_A, DEV_B]}, true, true, true, "Rig-capture & Capture & Stream"))
	return phases


func _phase_def(mode: String, scope: Dictionary, capture: bool, stream: bool, rig: bool, action_label: String) -> Dictionary:
	var duration := _human_phase_sec
	if mode == "superhuman":
		duration = _superhuman_phase_sec
	elif mode == "rig":
		duration = _rig_phase_sec
	return {
		"mode": mode,
		"scope": str(scope.get("scope", "")),
		"devices": scope.get("devices", []),
		"capture": capture,
		"stream": stream,
		"rig": rig,
		"action_label": action_label,
		"duration_sec": duration,
	}


func _begin_next_phase() -> void:
	_phase_index += 1
	if _phase_index >= _phases.size():
		_begin_next_bundle()
		return
	var phase: Dictionary = _phases[_phase_index]
	_phase_started_us = _now_us()
	_current_phase_visual_sequence += 1
	_active_phase = {
		"bundle_label": str(_current_bundle.get("label", "")),
		"capture_member_count": int(_current_bundle.get("member_count", 0)),
		"capture_ev_series_milli": _current_bundle.get("ev_milli", []),
		"phase_index": _phase_index,
		"phase_count": _phases.size(),
		"mode": str(phase.get("mode", "")),
		"scope": str(phase.get("scope", "")),
		"action_label": str(phase.get("action_label", "")),
		"capture": bool(phase.get("capture", false)),
		"stream": bool(phase.get("stream", false)),
		"rig": bool(phase.get("rig", false)),
		"devices": phase.get("devices", []),
		"duration_sec": float(phase.get("duration_sec", 0.0)),
		"started_us": _phase_started_us,
		"ended_us": 0,
		"frame_ms": [],
		"load_state": _make_phase_load_state(),
		"samples": {
			"stream_to_image": [],
			"device_capture": [],
			"device_capture_member": [],
			"rig_capture": [],
			"rig_capture_member": [],
		},
		"admissions": {
			"stream_to_image": {"scheduled": 0, "admitted": 0, "completed": 0, "blocked_by_inflight": 0, "blocked_by_materialization_backlog": 0, "blocked_by_texture_backlog": 0, "accepted": 0, "rejected": 0, "skipped_inflight": 0, "errors": {}},
			"device_capture": {"scheduled": 0, "admitted": 0, "completed": 0, "blocked_by_inflight": 0, "blocked_by_materialization_backlog": 0, "blocked_by_texture_backlog": 0, "accepted": 0, "rejected": 0, "skipped_inflight": 0, "errors": {}},
			"rig_capture": {"scheduled": 0, "admitted": 0, "completed": 0, "blocked_by_inflight": 0, "blocked_by_materialization_backlog": 0, "blocked_by_texture_backlog": 0, "accepted": 0, "rejected": 0, "skipped_inflight": 0, "errors": {}},
		},
		"completion_status": "running",
	}
	_state = PHASE_RUNNING
	_log("phase start: bundle=%s %d/%d %s %s %s" % [
		str(_current_bundle.get("label", "")),
		_phase_index + 1,
		_phases.size(),
		str(phase.get("mode", "")),
		str(phase.get("scope", "")),
		str(phase.get("action_label", "")),
	])
	_update_stats_label(true)


func _poll_active_phase() -> void:
	if _phase_index < 0 or _phase_index >= _phases.size():
		return
	var now := _now_us()
	var phase: Dictionary = _phases[_phase_index]
	var duration_us := int(float(phase.get("duration_sec", 0.0)) * 1000000.0)
	var hard_timeout_us := maxi(duration_us + 1, int(float(duration_us) * _hard_timeout_multiplier))
	_schedule_phase_actions(phase, now)
	var nominal_done := now - _phase_started_us >= duration_us
	var hard_timeout := now - _phase_started_us >= hard_timeout_us
	var phase_load_status := _phase_load_status()
	var phase_has_pending_work := _has_pending_work_for_phase(_phase_index)
	var minimums_met := bool(phase_load_status.get("minimums_met", false))
	if nominal_done and minimums_met and not phase_has_pending_work:
		_active_phase["ended_us"] = now
		_active_phase["completion_status"] = "complete"
		_completed_phase_records.append(_summarize_phase_record(_active_phase))
		_active_phase = {}
		_begin_next_phase()
		return
	if hard_timeout:
		_active_phase["ended_us"] = now
		_active_phase["completion_status"] = "under_sampled" if not minimums_met else "timeout"
		_active_phase["completion_reason"] = phase_load_status.get("completion_reason", "")
		_completed_phase_records.append(_summarize_phase_record(_active_phase))
		_active_phase = {}
		_begin_next_phase()


func _has_pending_work_for_phase(phase_index: int) -> bool:
	for job_v in _stream_jobs:
		if typeof(job_v) == TYPE_DICTIONARY:
			var job: Dictionary = job_v
			if int(job.get("phase_index", -999)) == phase_index:
				return true
	for job_v in _capture_jobs:
		if typeof(job_v) == TYPE_DICTIONARY:
			var job: Dictionary = job_v
			if int(job.get("phase_index", -999)) == phase_index:
				return true
	for job_v in _rig_jobs:
		if typeof(job_v) == TYPE_DICTIONARY:
			var job: Dictionary = job_v
			if int(job.get("phase_index", -999)) == phase_index:
				return true
	for job_v in _thumbnail_jobs:
		if typeof(job_v) == TYPE_DICTIONARY:
			var job: Dictionary = job_v
			if int(job.get("phase_index", -999)) == phase_index:
				return true
	return false


func _schedule_phase_actions(phase: Dictionary, now_us: int) -> void:
	var load_state: Dictionary = _active_phase.get("load_state", {})
	if load_state.is_empty():
		return
	var elapsed_sec := float(maxi(0, now_us - _phase_started_us)) / 1000000.0
	_drive_stream_load(phase, load_state, now_us, elapsed_sec)
	_drive_capture_load(phase, load_state, now_us, elapsed_sec)
	_drive_rig_load(phase, load_state, now_us, elapsed_sec)
	_active_phase["load_state"] = load_state


func _make_phase_load_state() -> Dictionary:
	return {
		"stream_scheduled_by_device": {},
		"capture_scheduled_by_device": {},
		"rig_scheduled": 0,
	}


func _drive_stream_load(phase: Dictionary, load_state: Dictionary, now_us: int, elapsed_sec: float) -> void:
	if not bool(phase.get("stream", false)):
		return
	var scheduled_by_device: Dictionary = load_state.get("stream_scheduled_by_device", {})
	for device_key_v in phase.get("devices", []):
		var device_key := str(device_key_v)
		var target_due := int(floor(_stream_requests_per_sec_per_stream * elapsed_sec))
		var scheduled := int(scheduled_by_device.get(device_key, 0))
		while scheduled < target_due:
			_request_stream_to_image(device_key, now_us)
			scheduled += 1
		scheduled_by_device[device_key] = scheduled
	load_state["stream_scheduled_by_device"] = scheduled_by_device


func _drive_capture_load(phase: Dictionary, load_state: Dictionary, now_us: int, elapsed_sec: float) -> void:
	if not bool(phase.get("capture", false)):
		return
	var scheduled_by_device: Dictionary = load_state.get("capture_scheduled_by_device", {})
	for device_key_v in phase.get("devices", []):
		var device_key := str(device_key_v)
		var target_due := int(floor(_capture_requests_per_sec_per_device * elapsed_sec))
		var scheduled := int(scheduled_by_device.get(device_key, 0))
		while scheduled < target_due:
			_request_device_capture(device_key, now_us)
			scheduled += 1
		scheduled_by_device[device_key] = scheduled
	load_state["capture_scheduled_by_device"] = scheduled_by_device


func _drive_rig_load(phase: Dictionary, load_state: Dictionary, now_us: int, elapsed_sec: float) -> void:
	if not bool(phase.get("rig", false)):
		return
	var target_due := int(floor(_rig_requests_per_sec * elapsed_sec))
	var scheduled := int(load_state.get("rig_scheduled", 0))
	while scheduled < target_due:
		_request_rig_capture(now_us)
		scheduled += 1
	load_state["rig_scheduled"] = scheduled


func _phase_load_status() -> Dictionary:
	var phase: Dictionary = _phases[_phase_index] if _phase_index >= 0 and _phase_index < _phases.size() else {}
	var admissions: Dictionary = _active_phase.get("admissions", {})
	var samples: Dictionary = _active_phase.get("samples", {})
	var categories := _phase_load_categories(phase)
	var scheduled := 0
	var admitted := 0
	var completed := 0
	var expected_attempts := _phase_expected_attempts(phase)
	for category in categories:
		var adm_v: Variant = admissions.get(category, {})
		var adm: Dictionary = adm_v if typeof(adm_v) == TYPE_DICTIONARY else {}
		var sample_v: Variant = samples.get(category, [])
		scheduled += int(adm.get("scheduled", 0))
		admitted += int(adm.get("admitted", adm.get("accepted", 0)))
		if typeof(sample_v) == TYPE_ARRAY:
			completed += (sample_v as Array).size()
	var minimum_attempts := maxi(_minimum_attempts_per_phase, maxi(1, int(ceil(float(expected_attempts) * 0.5))))
	var minimum_completions := maxi(_minimum_completions_per_phase, maxi(1, int(ceil(float(expected_attempts) * 0.5))))
	var minimum_frames := _minimum_frames_per_phase
	var phase_frame_count := 0
	var frame_ms_v: Variant = _active_phase.get("frame_ms", [])
	if typeof(frame_ms_v) == TYPE_ARRAY:
		phase_frame_count = (frame_ms_v as Array).size()
	var minimums_met := scheduled >= minimum_attempts and completed >= minimum_completions and phase_frame_count >= minimum_frames
	var completion_reason := ""
	if not minimums_met:
		if scheduled < minimum_attempts:
			completion_reason = "awaiting_minimum_attempts"
		elif completed < minimum_completions:
			completion_reason = "awaiting_minimum_completions"
		elif phase_frame_count < minimum_frames:
			completion_reason = "awaiting_minimum_frames"
	return {
		"scheduled_attempts": scheduled,
		"admitted": admitted,
		"completed": completed,
		"expected_attempts": expected_attempts,
		"minimum_attempts": minimum_attempts,
		"minimum_completions": minimum_completions,
		"minimum_frames": minimum_frames,
		"minimums_met": minimums_met,
		"completion_reason": completion_reason,
	}


func _phase_load_categories(phase: Dictionary) -> Array:
	var categories := []
	if bool(phase.get("stream", false)):
		categories.append("stream_to_image")
	if bool(phase.get("capture", false)):
		categories.append("device_capture")
	if bool(phase.get("rig", false)):
		categories.append("rig_capture")
	return categories


func _phase_expected_attempts(phase: Dictionary) -> int:
	var duration_sec := float(phase.get("duration_sec", 0.0))
	var target_count := 1.0
	var devices_v: Variant = phase.get("devices", [])
	if typeof(devices_v) == TYPE_ARRAY:
		target_count = float(maxi(1, (devices_v as Array).size()))
	var expected := 0.0
	if bool(phase.get("stream", false)):
		expected += _stream_requests_per_sec_per_stream * duration_sec * target_count
	if bool(phase.get("capture", false)):
		expected += _capture_requests_per_sec_per_device * duration_sec * target_count
	if bool(phase.get("rig", false)):
		expected += _rig_requests_per_sec * duration_sec
	return maxi(1, int(ceil(expected)))


func _phase_backlog_counts() -> Dictionary:
	return {
		"materialization": _capture_jobs.size() + _rig_jobs.size() + _thumbnail_jobs.size(),
		"texture": _stream_jobs.size() + _thumbnail_jobs.size(),
	}


func _schedule_one_action_set(phase: Dictionary, request_us: int) -> void:
	var devices = phase.get("devices", [])
	if bool(phase.get("rig", false)):
		_request_rig_capture(request_us)
	if bool(phase.get("capture", false)):
		for device_key in devices:
			_request_device_capture(str(device_key), request_us)
	if bool(phase.get("stream", false)):
		for device_key in devices:
			_request_stream_to_image(str(device_key), request_us)


func _random_human_delay_us() -> int:
	return int(_rng.randi_range(120000, 520000))

func _request_stream_to_image(device_key: String, request_us: int) -> void:
	var info: Dictionary = _devices[device_key]
	var stream_id := int(info.get("stream_id", 0))
	if stream_id <= 0:
		return
	_increment_admission("stream_to_image", "scheduled", 0)
	var pending := _phase_backlog_counts()
	if int(pending.get("texture", 0)) >= _max_pending_textures:
		_increment_admission("stream_to_image", "blocked_by_texture_backlog", 0)
		return
	_increment_admission("stream_to_image", "admitted", 0)
	_increment_admission("stream_to_image", "accepted", 0)
	_stream_jobs.append({
		"kind": "stream_to_image",
		"device_key": device_key,
		"stream_id": stream_id,
		"request_us": request_us,
		"bundle_label": str(_current_bundle.get("label", "")),
		"phase_index": _phase_index,
		"visual_sequence": _current_phase_visual_sequence,
	})


func _drain_stream_jobs(limit: int) -> void:
	var drained := 0
	var keep := []
	for job_v in _stream_jobs:
		var job: Dictionary = job_v
		if drained >= limit:
			keep.append(job)
			continue
		_complete_stream_to_image_job(job)
		drained += 1
	_stream_jobs = keep


func _complete_stream_to_image_job(job: Dictionary) -> void:
	var now := _now_us()
	var stream_id := int(job.get("stream_id", 0))
	var stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
	var sample := {
		"device_key": str(job.get("device_key", "")),
		"stream_id": stream_id,
		"request_us": int(job.get("request_us", 0)),
		"dequeue_us": now,
		"queue_wait_us": maxi(0, now - int(job.get("request_us", 0))),
		"result_lookup_us": 0,
		"to_image_us": 0,
		"texture_us": 0,
		"click_to_image_us": 0,
		"status": "unknown",
		"image_summary": "",
	}
	var lookup_start := _now_us()
	stream_result = CamBANGServer.get_stream_result_by_stream_id(stream_id)
	var lookup_end := _now_us()
	sample["result_lookup_us"] = lookup_end - lookup_start
	if stream_result == null:
		sample["status"] = "no_stream_result"
		_record_sample("stream_to_image", sample)
		return
	var to_image_start := _now_us()
	var image = stream_result.to_image()
	var to_image_end := _now_us()
	sample["to_image_us"] = to_image_end - to_image_start
	if image == null or image.is_empty():
		sample["status"] = "no_image"
		_record_sample("stream_to_image", sample)
		return
	var texture_start := _now_us()
	var texture: Texture2D = null
	if _visual_textures_enabled():
		texture = _assign_or_update_panel_image_texture(_stream_image_textures, str(job.get("device_key", "")), image)
	var texture_end := _now_us()
	sample["texture_us"] = texture_end - texture_start
	sample["click_to_image_us"] = texture_end - int(job.get("request_us", 0))
	sample["status"] = "complete"
	sample["image_summary"] = _image_summary(image)
	_record_sample("stream_to_image", sample)
	if texture != null:
		var device_key := str(job.get("device_key", ""))
		var rect = _stream_image_rects.get(device_key, null)
		var label = _stream_image_labels.get(device_key, null)
		_set_texture_if_changed(rect, texture)
		_set_label_text_if_changed(label, "%s\nstream_id=%d\nto_image_us=%d\nqueue_wait_us=%d\n%s" % [
			str(_devices[device_key].get("label", device_key)),
			stream_id,
			int(sample.get("to_image_us", 0)),
			int(sample.get("queue_wait_us", 0)),
			str(sample.get("image_summary", "")),
		])


func _request_device_capture(device_key: String, request_us: int) -> void:
	var info: Dictionary = _devices[device_key]
	_increment_admission("device_capture", "scheduled", 0)
	var inflight := int(info.get("inflight_captures", 0))
	if inflight >= _max_inflight_captures_per_device:
		_increment_admission("device_capture", "blocked_by_inflight", 0)
		_increment_admission("device_capture", "skipped_inflight", 0)
		return
	var pending := _phase_backlog_counts()
	if int(pending.get("materialization", 0)) >= _max_pending_materializations:
		_increment_admission("device_capture", "blocked_by_materialization_backlog", 0)
		return
	var device = info.get("device", null)
	if device == null:
		_increment_admission("device_capture", "rejected", ERR_UNAVAILABLE)
		return
	var baseline_capture_id := _device_last_capture_id(device_key)
	var trigger_start := _now_us()
	var err := int(device.trigger_capture())
	var trigger_end := _now_us()
	if err != OK:
		_increment_admission("device_capture", "rejected", err)
		_record_sample("device_capture", {
			"device_key": device_key,
			"status": "trigger_rejected",
			"error": err,
			"request_us": request_us,
			"trigger_call_us": trigger_end - trigger_start,
		})
		return
	_increment_admission("device_capture", "admitted", 0)
	_increment_admission("device_capture", "accepted", 0)
	info["inflight_captures"] = inflight + 1
	_devices[device_key] = info
	_capture_jobs.append({
		"kind": "device_capture",
		"device_key": device_key,
		"device": device,
		"device_id": int(info.get("device_id", 0)),
		"action_label": str(_active_phase.get("action_label", "")),
		"scope": str(_active_phase.get("scope", "")),
		"request_us": request_us,
		"trigger_start_us": trigger_start,
		"trigger_end_us": trigger_end,
		"trigger_call_us": trigger_end - trigger_start,
		"baseline_capture_id": baseline_capture_id,
		"bundle_label": str(_current_bundle.get("label", "")),
		"expected_member_count": int(_current_bundle.get("member_count", 0)),
		"phase_index": _phase_index,
		"visual_sequence": _current_phase_visual_sequence,
	})


func _poll_capture_jobs() -> void:
	var keep := []
	var drained := 0
	for job_v in _capture_jobs:
		var job: Dictionary = job_v
		if drained >= MAX_CAPTURE_DRAIN_PER_FRAME:
			keep.append(job)
			continue
		if _poll_one_capture_job(job):
			drained += 1
		else:
			keep.append(job)
	_capture_jobs = keep


func _poll_one_capture_job(job: Dictionary) -> bool:
	var now := _now_us()
	if now - int(job.get("request_us", 0)) > CAPTURE_TIMEOUT_US:
		if bool(job.get("is_settlement_probe", false)):
			_acq_probe_mark_capture_failure(str(job.get("device_key", "")), "capture_failed")
			_acq_probe_devices[str(job.get("device_key", ""))]["capture_timeout_us"] = now - int(job.get("request_us", 0))
		_release_device_capture_inflight(str(job.get("device_key", "")))
		_record_sample("device_capture", {
			"status": "timeout",
			"device_key": str(job.get("device_key", "")),
			"device_id": int(job.get("device_id", 0)),
			"request_us": int(job.get("request_us", 0)),
			"trigger_call_us": int(job.get("trigger_call_us", 0)),
			"click_to_result_ready_us": now - int(job.get("request_us", 0)),
		})
		return true
	var device = job.get("device", null)
	if device == null:
		return false
	var result = device.get_result()
	if result == null:
		return false
	var capture_id := int(result.get_capture_id()) if result.has_method("get_capture_id") else 0
	if capture_id != 0 and capture_id <= int(job.get("baseline_capture_id", 0)):
		return false
	_complete_capture_result(job, result, false)
	return true


func _complete_capture_result(job: Dictionary, capture_result, is_rig_member: bool) -> void:
	var result_ready_us := _now_us()
	var device_key := str(job.get("device_key", ""))
	var capture_id := int(capture_result.get_capture_id()) if capture_result.has_method("get_capture_id") else 0
	var get_count_start := _now_us()
	var returned_count := int(capture_result.get_image_count()) if capture_result.has_method("get_image_count") else 1
	var get_count_end := _now_us()
	var to_image_start := _now_us()
	var image = capture_result.to_image()
	var to_image_end := _now_us()
	var texture: Texture2D = null
	var texture_start := _now_us()
	if image != null and not image.is_empty() and _visual_textures_enabled():
		var texture_map := _rig_textures if is_rig_member else _capture_textures
		texture = _assign_or_update_panel_image_texture(texture_map, device_key, image)
	var texture_end := _now_us()
	var sample := {
		"device_key": device_key,
		"device_id": int(job.get("device_id", 0)),
		"capture_id": capture_id,
		"bundle_label": str(job.get("bundle_label", "")),
		"action_label": str(job.get("action_label", "")),
		"scope": str(job.get("scope", "")),
		"request_us": int(job.get("request_us", 0)),
		"trigger_call_us": int(job.get("trigger_call_us", 0)),
		"click_to_result_ready_us": result_ready_us - int(job.get("request_us", 0)),
		"get_image_count_us": get_count_end - get_count_start,
		"to_image_us": to_image_end - to_image_start,
		"texture_us": texture_end - texture_start,
		"click_to_default_image_us": texture_end - int(job.get("request_us", 0)),
		"returned_member_count": returned_count,
		"expected_member_count": int(job.get("expected_member_count", 0)),
		"status": "complete" if image != null and not image.is_empty() else "no_default_image",
		"phase_index": int(job.get("phase_index", -1)),
		"image_summary": _image_summary(image),
	}
	_record_capture_sample_provenance(sample, job, is_rig_member)
	if is_rig_member:
		_record_sample("rig_capture_member", sample)
	else:
		_record_sample("device_capture", sample)
		_release_device_capture_inflight(device_key)
	if device_key != "" and capture_id > 0 and _devices.has(device_key):
		var info: Dictionary = _devices[device_key]
		info["last_capture_id"] = maxi(int(info.get("last_capture_id", 0)), capture_id)
		_devices[device_key] = info
	_update_capture_visuals(device_key, capture_result, image, texture, sample, is_rig_member)
	if bool(job.get("is_preflight_capture", false)) and not is_rig_member:
		_note_preflight_capture_result(device_key, capture_result, returned_count)
	if bool(job.get("is_settlement_probe", false)) and not is_rig_member:
		_settlement_probe_note_capture_result(str(job.get("device_key", "")), capture_result, returned_count)


func _note_preflight_capture_result(device_key: String, capture_result, returned_count: int) -> void:
	var entry_v = _preflight_devices.get(device_key, {})
	if typeof(entry_v) != TYPE_DICTIONARY:
		return
	var entry: Dictionary = entry_v
	entry["capture_complete"] = true
	entry["capture_failed"] = false
	entry["capture_id"] = int(capture_result.get_capture_id()) if capture_result.has_method("get_capture_id") else 0
	entry["returned_member_count"] = returned_count
	_preflight_devices[device_key] = entry


func _settlement_probe_note_capture_result(device_key: String, capture_result, returned_count: int) -> void:
	var entry_v = _acq_probe_devices.get(device_key, {})
	if typeof(entry_v) != TYPE_DICTIONARY:
		return
	var entry: Dictionary = entry_v
	entry["capture_complete"] = true
	entry["trigger_status"] = "complete"
	entry["capture_id"] = int(capture_result.get_capture_id()) if capture_result.has_method("get_capture_id") else 0
	entry["returned_member_count"] = returned_count
	entry["materialized_member_indices"] = []
	entry["materialization_failed_indices"] = []
	entry["materialized_member_count"] = 0
	var materialize_count := mini(returned_count, _acq_probe_required_member_count)
	if returned_count > 0:
		entry["materialized_member_indices"].append(0)
		entry["materialized_member_count"] = 1
	for member_index in range(1, materialize_count):
		var image = capture_result.to_image_member(member_index)
		if image == null or image.is_empty():
			entry["materialization_failed_indices"].append(member_index)
			continue
		entry["materialized_member_indices"].append(member_index)
		entry["materialized_member_count"] = int(entry.get("materialized_member_count", 0)) + 1
	entry["materialization_complete"] = int(entry.get("materialized_member_count", 0)) >= _acq_probe_required_member_count
	entry["capture_failed"] = false
	_acq_probe_devices[device_key] = entry


func _update_capture_visuals(device_key: String, capture_result, image, texture: Texture2D, sample: Dictionary, is_rig_member: bool) -> void:
	var rect_map := _rig_rects if is_rig_member else _capture_rects
	var label_map := _rig_labels if is_rig_member else _capture_labels
	var row_map := _rig_rows if is_rig_member else _capture_rows
	var rect = rect_map.get(device_key, null)
	var label = label_map.get(device_key, null)
	_set_texture_if_changed(rect, texture)
	_set_label_text_if_changed(label, "%s\ncapture_id=%d members=%d/%d\ndefault_to_image_us=%d\nclick_to_default_us=%d\n%s" % [
		str(_devices[device_key].get("label", device_key)),
		int(sample.get("capture_id", 0)),
		int(sample.get("returned_member_count", 0)),
		int(sample.get("expected_member_count", 0)),
		int(sample.get("to_image_us", 0)),
		int(sample.get("click_to_default_image_us", 0)),
		str(sample.get("image_summary", "")),
	])
	var row = row_map.get(device_key, null)
	if row != null:
		_rebuild_member_strip(row, capture_result, image, texture, sample, is_rig_member, device_key)

func _request_rig_capture(request_us: int) -> void:
	_increment_admission("rig_capture", "scheduled", 0)
	if _rig == null:
		_increment_admission("rig_capture", "rejected", ERR_UNAVAILABLE)
		return
	if _rig_inflight > 0:
		_increment_admission("rig_capture", "blocked_by_inflight", 0)
		_increment_admission("rig_capture", "skipped_inflight", 0)
		return
	var pending := _phase_backlog_counts()
	if int(pending.get("materialization", 0)) >= _max_pending_materializations:
		_increment_admission("rig_capture", "blocked_by_materialization_backlog", 0)
		return
	var before_by_device := {}
	for device_key in [DEV_A, DEV_B]:
		before_by_device[device_key] = _device_last_capture_id(device_key)
	var trigger_start := _now_us()
	var err := int(_rig.trigger_capture())
	var trigger_end := _now_us()
	if err != OK:
		_increment_admission("rig_capture", "rejected", err)
		_record_sample("rig_capture", {
			"status": "trigger_rejected",
			"error": err,
			"request_us": request_us,
			"trigger_call_us": trigger_end - trigger_start,
		})
		return
	_increment_admission("rig_capture", "admitted", 0)
	_increment_admission("rig_capture", "accepted", 0)
	_rig_inflight += 1
	_rig_jobs.append({
		"kind": "rig_capture",
		"request_us": request_us,
		"trigger_start_us": trigger_start,
		"trigger_end_us": trigger_end,
		"trigger_call_us": trigger_end - trigger_start,
		"before_by_device": before_by_device,
		"bundle_label": str(_current_bundle.get("label", "")),
		"action_label": str(_active_phase.get("action_label", "")),
		"scope": str(_active_phase.get("scope", "")),
		"expected_member_count": int(_current_bundle.get("member_count", 0)),
		"phase_index": _phase_index,
	})


func _poll_rig_jobs() -> void:
	var keep := []
	var drained := 0
	for job_v in _rig_jobs:
		var job: Dictionary = job_v
		if drained >= MAX_RIG_DRAIN_PER_FRAME:
			keep.append(job)
			continue
		if _poll_one_rig_job(job):
			drained += 1
		else:
			keep.append(job)
	_rig_jobs = keep


func _poll_one_rig_job(job: Dictionary) -> bool:
	var now := _now_us()
	if now - int(job.get("request_us", 0)) > RIG_CAPTURE_TIMEOUT_US:
		_rig_inflight = maxi(0, _rig_inflight - 1)
		_record_sample("rig_capture", {
			"status": "timeout",
			"request_us": int(job.get("request_us", 0)),
			"trigger_call_us": int(job.get("trigger_call_us", 0)),
			"click_to_result_set_ready_us": now - int(job.get("request_us", 0)),
		})
		return true
	if _rig == null:
		return false
	# CamBANGRig.get_result() returns a plain Array[CamBANGCaptureResult]
	# (no dedicated CaptureResultSet wrapper class -- see
	# docs/architecture/pixel_payload_and_result_contract.md 10.6.3). This
	# script predates that removal (commit 3a082d8) and was missed by it;
	# 73_rig_capture_result_set_verification.gd was migrated correctly at
	# the same time and is the reference pattern this mirrors.
	var results: Array = _rig.get_result()
	if results.is_empty():
		return false
	var result_by_key := {}
	for result in results:
		if result == null:
			continue
		var device_id := int(result.get_device_instance_id())
		var device_key := _device_key_for_id(device_id)
		if device_key != "":
			result_by_key[device_key] = result
	for device_key in [DEV_A, DEV_B]:
		if not result_by_key.has(device_key):
			return false
		var result = result_by_key[device_key]
		var capture_id := int(result.get_capture_id()) if result.has_method("get_capture_id") else 0
		var before_by_device = job.get("before_by_device", {})
		if capture_id != 0 and capture_id <= int(before_by_device.get(device_key, 0)):
			return false
	var ready_us := _now_us()
	var sample := {
		"status": "complete",
		"request_us": int(job.get("request_us", 0)),
		"trigger_call_us": int(job.get("trigger_call_us", 0)),
		"click_to_result_set_ready_us": ready_us - int(job.get("request_us", 0)),
		"result_count": results.size(),
	}
	_record_sample("rig_capture", sample)
	for device_key in [DEV_A, DEV_B]:
		var result = result_by_key[device_key]
		var member_job := job.duplicate(true)
		member_job["device_key"] = device_key
		member_job["device_id"] = int(result.get_device_instance_id())
		_complete_capture_result(member_job, result, true)
	_rig_inflight = maxi(0, _rig_inflight - 1)
	return true


func _device_key_for_id(device_id: int) -> String:
	for device_key in [DEV_A, DEV_B]:
		if int(_devices[device_key].get("device_id", 0)) == device_id:
			return device_key
	return ""


func _device_last_capture_id(device_key: String) -> int:
	var info: Dictionary = _devices[device_key]
	return int(info.get("last_capture_id", 0))


func _release_device_capture_inflight(device_key: String) -> void:
	if not _devices.has(device_key):
		return
	var info: Dictionary = _devices[device_key]
	info["inflight_captures"] = maxi(0, int(info.get("inflight_captures", 0)) - 1)
	_devices[device_key] = info


func _increment_admission(kind: String, field: String, err: int) -> void:
	if _active_phase.is_empty():
		return
	var admissions = _active_phase.get("admissions", {})
	var rec = admissions.get(kind, {})
	rec[field] = int(rec.get(field, 0)) + 1
	if field == "rejected":
		var errors = rec.get("errors", {})
		var key := str(err)
		errors[key] = int(errors.get(key, 0)) + 1
		rec["errors"] = errors
	admissions[kind] = rec
	_active_phase["admissions"] = admissions


func _rebuild_member_strip(row: HBoxContainer, capture_result, pre_image, pre_texture, parent_sample: Dictionary, is_rig_member: bool, device_key: String) -> void:
	var returned_count := int(parent_sample.get("returned_member_count", 0))
	var expected_count := int(parent_sample.get("expected_member_count", 0))
	var count: int = returned_count if returned_count > 0 else 0
	_drop_thumbnail_jobs_for_row(row)
	var row_serial := int(row.get_meta("capture_serial")) + 1 if row.has_meta("capture_serial") else 1
	row.set_meta("capture_serial", row_serial)
	if count <= 0:
		_clear_member_strip_row(row)
		var label := Label.new()
		label.text = "no members returned"
		row.add_child(label)
		row.set_meta("member_cells", [])
		row.set_meta("member_note", null)
		return
	var members := _make_still_members_for_bundle(_current_bundle)
	var cell_records := _ensure_member_strip_cells(row, count, members)
	var note: Label = _ensure_member_strip_note(row)
	note.visible = returned_count < expected_count
	if note.visible:
		note.text = "returned %d / expected %d" % [returned_count, expected_count]
	for i in range(cell_records.size()):
		var cell_record: Dictionary = cell_records[i]
		var rect: TextureRect = cell_record.get("rect", null)
		var facts: Label = cell_record.get("facts", null)
		var title: Label = cell_record.get("title", null)
		var texture_key := _member_strip_texture_key(row, i)
		if title != null:
			title.text = _member_label(i, members)
		if rect != null and i != 0:
			_assign_or_clear_member_strip_texture(row, texture_key, rect, null)
		if facts != null:
			facts.text = "pending"
		if i == 0 and pre_image != null and not pre_image.is_empty():
			if pre_texture != null and rect != null:
				rect.texture = pre_texture
			if facts != null:
				facts.text = "idx=0\nmain"
		else:
			_thumbnail_jobs.append({
				"capture_result": capture_result,
				"member_index": i,
				"requested_us": _now_us(),
				"rect": rect,
				"facts": facts,
				"row": row,
				"row_serial": row_serial,
				"texture_key": texture_key,
				"device_key": device_key,
				"is_rig_member": is_rig_member,
				"expected_count": expected_count,
				"phase_index": int(parent_sample.get("phase_index", -1)),
			})


func _ensure_member_strip_cells(row: HBoxContainer, count: int, members: Array) -> Array:
	var cells_v: Variant = row.get_meta("member_cells") if row.has_meta("member_cells") else []
	var cells: Array = cells_v if typeof(cells_v) == TYPE_ARRAY else []
	while cells.size() < count:
		var index := cells.size()
		var cell := VBoxContainer.new()
		cell.custom_minimum_size = Vector2(96, 94)
		row.add_child(cell)
		var title := Label.new()
		title.text = _member_label(index, members)
		cell.add_child(title)
		var rect := TextureRect.new()
		rect.custom_minimum_size = Vector2(92, 52)
		rect.expand_mode = TextureRect.EXPAND_FIT_WIDTH_PROPORTIONAL
		rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		cell.add_child(rect)
		var facts := Label.new()
		facts.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		facts.text = "pending"
		cell.add_child(facts)
		cells.append({
			"cell": cell,
			"title": title,
			"rect": rect,
			"facts": facts,
		})
	while cells.size() > count:
		var removed: Dictionary = cells.pop_back()
		_member_strip_release_texture(row, cells.size())
		var removed_cell = removed.get("cell", null)
		if removed_cell != null:
			row.remove_child(removed_cell)
			removed_cell.queue_free()
	row.set_meta("member_cells", cells)
	return cells


func _ensure_member_strip_note(row: HBoxContainer) -> Label:
	var note = row.get_meta("member_note") if row.has_meta("member_note") else null
	if note != null:
		return note
	note = Label.new()
	note.visible = false
	row.add_child(note)
	row.set_meta("member_note", note)
	return note


func _clear_member_strip_row(row: HBoxContainer) -> void:
	row.set_meta("member_strip_textures", {})
	for child in row.get_children():
		row.remove_child(child)
		child.queue_free()


func _drop_thumbnail_jobs_for_row(row: HBoxContainer) -> void:
	if _thumbnail_jobs.is_empty():
		return
	var keep := []
	for job_v in _thumbnail_jobs:
		var job: Dictionary = job_v
		if job.get("row", null) == row:
			continue
		keep.append(job)
	_thumbnail_jobs = keep


func _drain_visual_thumbnail_jobs(limit: int) -> void:
	var drained := 0
	var keep := []
	for job_v in _thumbnail_jobs:
		var job: Dictionary = job_v
		if drained >= limit:
			keep.append(job)
			continue
		_complete_thumbnail_job(job)
		drained += 1
	_thumbnail_jobs = keep


func _complete_thumbnail_job(job: Dictionary) -> void:
	var capture_result = job.get("capture_result", null)
	if capture_result == null:
		return
	var row = job.get("row", null)
	if row != null:
		if not row.has_meta("capture_serial"):
			return
		if int(row.get_meta("capture_serial")) != int(job.get("row_serial", 0)):
			return
	var member_index := int(job.get("member_index", 0))
	var to_image_start := _now_us()
	var image = capture_result.to_image_member(member_index)
	var to_image_end := _now_us()
	var texture_start := _now_us()
	var texture: Texture2D = null
	if image != null and not image.is_empty() and _visual_textures_enabled():
		texture = _assign_or_update_member_strip_texture(job.get("row", null), str(job.get("texture_key", "")), image)
	var texture_end := _now_us()
	var category := "rig_capture_member" if bool(job.get("is_rig_member", false)) else "device_capture_member"
	var sample := {
		"device_key": str(job.get("device_key", "")),
		"member_index": member_index,
		"phase_index": int(job.get("phase_index", -1)),
		"to_image_member_us": to_image_end - to_image_start,
		"texture_us": texture_end - texture_start,
		"scheduled_to_available_us": texture_end - int(job.get("requested_us", 0)),
		"status": "complete" if image != null and not image.is_empty() else "no_image",
		"image_summary": _image_summary(image),
	}
	_record_sample(category, sample)
	var rect = job.get("rect", null)
	_set_texture_if_changed(rect, texture)
	var facts = job.get("facts", null)
	_set_label_text_if_changed(facts, "idx=%d\nto_img=%d" % [member_index, int(sample.get("to_image_member_us", 0))])

func _record_sample(category: String, sample: Dictionary) -> void:
	if _active_phase.is_empty():
		return
	var samples = _active_phase.get("samples", {})
	var arr = samples.get(category, [])
	arr.append(sample)
	samples[category] = arr
	_active_phase["samples"] = samples


func _record_frame(delta: float) -> void:
	_frame_count += 1
	var frame_ms := delta * 1000.0
	if not _benchmark_metrics_frozen:
		_run_frame_ms.append(frame_ms)
	if not _active_phase.is_empty():
		var frame_samples = _active_phase.get("frame_ms", [])
		frame_samples.append(frame_ms)
		_active_phase["frame_ms"] = frame_samples


func _freeze_benchmark_metrics() -> void:
	if _benchmark_metrics_frozen:
		return
	_benchmark_metrics_frozen = true
	_benchmark_finished_us = _now_us()
	_benchmark_frame_count = _frame_count


func _summarize_phase_record(record: Dictionary) -> Dictionary:
	var out := record.duplicate(true)
	var frame_ms_v: Variant = out.get("frame_ms", [])
	var frame_count := 0
	if typeof(frame_ms_v) == TYPE_ARRAY:
		frame_count = (frame_ms_v as Array).size()
	out["frame_count"] = frame_count
	out["duration_observed_sec"] = float(int(out.get("ended_us", 0)) - int(out.get("started_us", 0))) / 1000000.0
	out["frame_stats"] = _numeric_stats(frame_ms_v if typeof(frame_ms_v) == TYPE_ARRAY else [])
	out.erase("frame_ms")
	var summarized_samples := {}
	var samples = out.get("samples", {})
	for category in samples.keys():
		var arr = samples.get(category, [])
		summarized_samples[category] = _summarize_sample_array(category, arr)
	out["samples"] = summarized_samples
	return out


func _summarize_sample_array(category: String, samples: Array) -> Dictionary:
	var numeric_keys := _numeric_keys_for_category(category)
	var result := {
		"count": samples.size(),
		"status_counts": _status_counts(samples),
		"timing": {},
		"slowest": _slowest_samples(samples),
	}
	var timing := {}
	for key in numeric_keys:
		var values := []
		for sample_v in samples:
			if typeof(sample_v) != TYPE_DICTIONARY:
				continue
			var sample: Dictionary = sample_v
			if not sample.has(key):
				continue
			var value := float(sample.get(key, 0))
			if value >= 0.0:
				values.append(value)
		timing[key] = _numeric_stats(values)
	result["timing"] = timing
	return result


func _numeric_keys_for_category(category: String) -> Array:
	match category:
		"stream_to_image":
			return ["queue_wait_us", "result_lookup_us", "to_image_us", "texture_us", "click_to_image_us"]
		"device_capture":
			return ["trigger_call_us", "click_to_result_ready_us", "get_image_count_us", "to_image_us", "texture_us", "click_to_default_image_us"]
		"device_capture_member":
			return ["to_image_member_us", "texture_us", "scheduled_to_available_us"]
		"rig_capture":
			return ["trigger_call_us", "click_to_result_set_ready_us"]
		"rig_capture_member":
			return ["trigger_call_us", "click_to_result_ready_us", "get_image_count_us", "to_image_us", "texture_us", "click_to_default_image_us", "to_image_member_us", "scheduled_to_available_us"]
		_:
			return []


func _status_counts(samples: Array) -> Dictionary:
	var counts := {}
	for sample_v in samples:
		if typeof(sample_v) != TYPE_DICTIONARY:
			continue
		var sample: Dictionary = sample_v
		var status := str(sample.get("status", "unspecified"))
		counts[status] = int(counts.get(status, 0)) + 1
	return counts


func _slowest_samples(samples: Array) -> Array:
	var keyed := []
	for sample_v in samples:
		if typeof(sample_v) != TYPE_DICTIONARY:
			continue
		var sample: Dictionary = sample_v
		var score := 0
		for key in ["click_to_default_image_us", "click_to_image_us", "scheduled_to_available_us", "to_image_us", "to_image_member_us", "click_to_result_set_ready_us"]:
			if sample.has(key):
				score = maxi(score, int(sample.get(key, 0)))
		keyed.append({"score": score, "sample": sample})
	keyed.sort_custom(func(a, b): return int(a.get("score", 0)) > int(b.get("score", 0)))
	var out := []
	for i in range(mini(MAX_SLOW_SAMPLES, keyed.size())):
		out.append(keyed[i].get("sample", {}))
	return out


func _numeric_stats(values: Array) -> Dictionary:
	if values.is_empty():
		return {
			"count": 0,
			"min": 0,
			"mean": 0,
			"stdev": 0,
			"p50": 0,
			"p90": 0,
			"p95": 0,
			"p99": 0,
			"max": 0,
		}
	var sorted := []
	var total := 0.0
	for v in values:
		var f := float(v)
		sorted.append(f)
		total += f
	sorted.sort()
	var mean := total / float(sorted.size())
	var variance := 0.0
	for v in sorted:
		variance += pow(float(v) - mean, 2.0)
	variance /= float(sorted.size())
	return {
		"count": sorted.size(),
		"min": sorted[0],
		"mean": mean,
		"stdev": sqrt(variance),
		"p50": _percentile(sorted, 0.50),
		"p90": _percentile(sorted, 0.90),
		"p95": _percentile(sorted, 0.95),
		"p99": _percentile(sorted, 0.99),
		"max": sorted[sorted.size() - 1],
	}


func _percentile(sorted_values: Array, q: float) -> float:
	if sorted_values.is_empty():
		return 0.0
	var index := int(round((float(sorted_values.size() - 1)) * clamp(q, 0.0, 1.0)))
	index = clamp(index, 0, sorted_values.size() - 1)
	return float(sorted_values[index])


func _build_summary(exit_code: int, expected_unsupported: bool) -> Dictionary:
	var synthetic_metrics := {}
	if CamBANGServer.has_method("get_synthetic_metrics_snapshot"):
		var synthetic_metrics_value = CamBANGServer.get_synthetic_metrics_snapshot()
		if typeof(synthetic_metrics_value) == TYPE_DICTIONARY:
			synthetic_metrics = synthetic_metrics_value
	var acquisition_session_settlement_probe := _acquisition_session_settlement_probe_summary(synthetic_metrics)
	var load_model := _load_model_summary()
	var load_delivery := _load_delivery_summary(_completed_phase_records)
	return {
		"schema_version": 1,
		"scene": SCENE_LABEL,
		"exit_code": exit_code,
		"expected_unsupported": expected_unsupported,
		"harness": {
			"status": _harness_status_for_finish(exit_code, expected_unsupported),
			"reason": _finish_reason,
		},
		"run_context": _run_context(),
		"config": {
			"load_profile": _load_profile_arg,
			"provider": _provider_arg,
			"seed": _seed,
			"warmup_sec": _warmup_sec,
			"human_phase_sec": _human_phase_sec,
			"superhuman_phase_sec": _superhuman_phase_sec,
			"rig_phase_sec": _rig_phase_sec,
			"exit_visual_hold_sec": _exit_visual_hold_sec,
			"phase_sec_override": _phase_sec_override,
			"stream_requests_per_sec_per_stream": _stream_requests_per_sec_per_stream,
			"capture_requests_per_sec_per_device": _capture_requests_per_sec_per_device,
			"rig_requests_per_sec": _rig_requests_per_sec,
			"max_pending_materializations": _max_pending_materializations,
			"max_pending_textures": _max_pending_textures,
			"minimum_frames_per_phase": _minimum_frames_per_phase,
			"minimum_attempts_per_phase": _minimum_attempts_per_phase,
			"minimum_completions_per_phase": _minimum_completions_per_phase,
			"hard_timeout_multiplier": _hard_timeout_multiplier,
			"superhuman_actions_per_tick": _superhuman_actions_per_tick,
			"max_inflight_captures_per_device": _max_inflight_captures_per_device,
			"materialize_textures_in_headless": _materialize_textures_in_headless,
		},
		"load_model": load_model,
		"load_delivery": load_delivery,
		"baseline_validity": _baseline_validity_summary(exit_code, load_delivery),
		"scenario": "none (public-api topology, no scenario)",
		"capture_bundle_runs": _group_phase_records_by_bundle(_completed_phase_records),
		"benchmark_frame_stats": _numeric_stats(_run_frame_ms),
		"run_frame_stats": _numeric_stats(_run_frame_ms),
		"exit_visual_hold": _exit_visual_hold_summary(),
		"stream_display_observation": _stream_display_observation_summary(),
		"cpu_display_refresh_observation": _cpu_display_refresh_observation_summary(
			synthetic_metrics
		),
		"acquisition_session_settlement_probe": acquisition_session_settlement_probe,
		"run_quality_warnings": _run_quality_warnings_summary(
			acquisition_session_settlement_probe,
			synthetic_metrics
		),
		"backing_plan_acquisition_session_reports": _backing_plan_acquisition_session_reports_summary(
			synthetic_metrics
		),
		"synthetic_metrics": synthetic_metrics,
		"elapsed_us": _benchmark_elapsed_us(),
		"whole_scene_elapsed_us": _now_us() - _started_us,
	}


func _load_model_summary() -> Dictionary:
	return {
		"profile": _load_profile_arg,
		"resolved_config": {
			"phase_sec_override": _phase_sec_override,
			"human_phase_sec": _human_phase_sec,
			"superhuman_phase_sec": _superhuman_phase_sec,
			"rig_phase_sec": _rig_phase_sec,
			"stream_to_image_requests_per_sec_per_stream": _stream_requests_per_sec_per_stream,
			"device_capture_requests_per_sec_per_device": _capture_requests_per_sec_per_device,
			"rig_capture_requests_per_sec": _rig_requests_per_sec,
			"max_pending_materializations": _max_pending_materializations,
			"max_pending_textures": _max_pending_textures,
			"minimum_frames_per_phase": _minimum_frames_per_phase,
			"minimum_attempts_per_phase": _minimum_attempts_per_phase,
			"minimum_completions_per_phase": _minimum_completions_per_phase,
			"hard_timeout_multiplier": _hard_timeout_multiplier,
			"superhuman_actions_per_tick": _superhuman_actions_per_tick,
		},
		"platform_agnostic": true,
	}


func _load_delivery_summary(records: Array) -> Dictionary:
	var by_phase: Array = []
	var aggregate := {
		"scheduled_attempts": 0,
		"admitted": 0,
		"completed": 0,
		"blocked_by_backpressure": 0,
		"blocked_by_inflight": 0,
		"blocked_by_materialization_backlog": 0,
		"blocked_by_texture_backlog": 0,
	}
	for record_v in records:
		if typeof(record_v) != TYPE_DICTIONARY:
			continue
		var phase_delivery := _phase_load_delivery_summary(record_v)
		by_phase.append(phase_delivery)
		aggregate["scheduled_attempts"] = int(aggregate.get("scheduled_attempts", 0)) + int(phase_delivery.get("scheduled_attempts", 0))
		aggregate["admitted"] = int(aggregate.get("admitted", 0)) + int(phase_delivery.get("admitted", 0))
		aggregate["completed"] = int(aggregate.get("completed", 0)) + int(phase_delivery.get("completed", 0))
		aggregate["blocked_by_backpressure"] = int(aggregate.get("blocked_by_backpressure", 0)) + int(phase_delivery.get("blocked_by_backpressure", 0))
		aggregate["blocked_by_inflight"] = int(aggregate.get("blocked_by_inflight", 0)) + int(phase_delivery.get("blocked_by_inflight", 0))
		aggregate["blocked_by_materialization_backlog"] = int(aggregate.get("blocked_by_materialization_backlog", 0)) + int(phase_delivery.get("blocked_by_materialization_backlog", 0))
		aggregate["blocked_by_texture_backlog"] = int(aggregate.get("blocked_by_texture_backlog", 0)) + int(phase_delivery.get("blocked_by_texture_backlog", 0))
	var scheduled_attempts := int(aggregate.get("scheduled_attempts", 0))
	var admitted := int(aggregate.get("admitted", 0))
	var completed := int(aggregate.get("completed", 0))
	return {
		"aggregate": {
			"scheduled_attempts": scheduled_attempts,
			"admitted": admitted,
			"completed": completed,
			"blocked_by_backpressure": int(aggregate.get("blocked_by_backpressure", 0)),
			"blocked_by_inflight": int(aggregate.get("blocked_by_inflight", 0)),
			"blocked_by_materialization_backlog": int(aggregate.get("blocked_by_materialization_backlog", 0)),
			"blocked_by_texture_backlog": int(aggregate.get("blocked_by_texture_backlog", 0)),
			"admission_ratio": float(admitted) / float(scheduled_attempts) if scheduled_attempts > 0 else 0.0,
			"completion_ratio": float(completed) / float(scheduled_attempts) if scheduled_attempts > 0 else 0.0,
		},
		"by_phase": by_phase,
	}


func _phase_load_delivery_summary(record: Dictionary) -> Dictionary:
	var admissions: Dictionary = record.get("admissions", {})
	var samples: Dictionary = record.get("samples", {})
	var phase: Dictionary = record
	var categories := _phase_load_categories(phase)
	var scheduled_attempts := 0
	var admitted := 0
	var completed := 0
	var blocked_by_inflight := 0
	var blocked_by_materialization_backlog := 0
	var blocked_by_texture_backlog := 0
	var category_delivery := []
	for category in categories:
		var adm_v: Variant = admissions.get(category, {})
		var adm: Dictionary = adm_v if typeof(adm_v) == TYPE_DICTIONARY else {}
		var sample_v: Variant = samples.get(category, {})
		var sample: Dictionary = sample_v if typeof(sample_v) == TYPE_DICTIONARY else {}
		var cat_scheduled := int(adm.get("scheduled", 0))
		var cat_admitted := int(adm.get("admitted", adm.get("accepted", 0)))
		# "count" is every recorded sample regardless of outcome (includes
		# "timeout"/"trigger_rejected"/"no_image" -- see _record_sample call
		# sites); a job that silently times out without ever completing must
		# not read as "completed" here, or a regression that stalls delivery
		# can hide behind a misleadingly healthy completion_ratio.
		var status_counts_v: Variant = sample.get("status_counts", {})
		var status_counts: Dictionary = status_counts_v if typeof(status_counts_v) == TYPE_DICTIONARY else {}
		var cat_completed := int(status_counts.get("complete", 0))
		var cat_blocked_by_inflight := int(adm.get("blocked_by_inflight", adm.get("skipped_inflight", 0)))
		var cat_blocked_by_materialization_backlog := int(adm.get("blocked_by_materialization_backlog", 0))
		var cat_blocked_by_texture_backlog := int(adm.get("blocked_by_texture_backlog", 0))
		category_delivery.append({
			"category": category,
			"scheduled_attempts": cat_scheduled,
			"admitted": cat_admitted,
			"completed": cat_completed,
			"blocked_by_inflight": cat_blocked_by_inflight,
			"blocked_by_materialization_backlog": cat_blocked_by_materialization_backlog,
			"blocked_by_texture_backlog": cat_blocked_by_texture_backlog,
			"admission_ratio": float(cat_admitted) / float(cat_scheduled) if cat_scheduled > 0 else 0.0,
			"completion_ratio": float(cat_completed) / float(cat_scheduled) if cat_scheduled > 0 else 0.0,
		})
		scheduled_attempts += cat_scheduled
		admitted += cat_admitted
		completed += cat_completed
		blocked_by_inflight += cat_blocked_by_inflight
		blocked_by_materialization_backlog += cat_blocked_by_materialization_backlog
		blocked_by_texture_backlog += cat_blocked_by_texture_backlog
	var blocked_by_backpressure := blocked_by_inflight + blocked_by_materialization_backlog + blocked_by_texture_backlog
	var phase_count := int(record.get("phase_index", -1))
	var frame_count := int(record.get("frame_count", 0))
	var completion_status := str(record.get("completion_status", ""))
	var completion_reason := str(record.get("completion_reason", ""))
	var duration_sec := float(record.get("duration_sec", 0.0))
	return {
		"bundle_label": str(record.get("bundle_label", "")),
		"phase_index": phase_count,
		"mode": str(record.get("mode", "")),
		"scope": str(record.get("scope", "")),
		"action_label": str(record.get("action_label", "")),
		"duration_sec": duration_sec,
		"duration_observed_sec": float(int(record.get("ended_us", 0)) - int(record.get("started_us", 0))) / 1000000.0,
		"frame_count": frame_count,
		"scheduled_attempts": scheduled_attempts,
		"admitted": admitted,
		"completed": completed,
		"blocked_by_backpressure": blocked_by_backpressure,
		"blocked_by_inflight": blocked_by_inflight,
		"blocked_by_materialization_backlog": blocked_by_materialization_backlog,
		"blocked_by_texture_backlog": blocked_by_texture_backlog,
		"admission_ratio": float(admitted) / float(scheduled_attempts) if scheduled_attempts > 0 else 0.0,
		"completion_ratio": float(completed) / float(scheduled_attempts) if scheduled_attempts > 0 else 0.0,
		"completion_status": completion_status,
		"completion_reason": completion_reason,
		"minimums_met": completion_status == "complete",
		"category_delivery": category_delivery,
	}


func _baseline_validity_summary(exit_code: int, load_delivery: Dictionary) -> Dictionary:
	var reasons: Array = []
	var classification := "healthy_baseline"
	if exit_code != 0 or _failed:
		classification = "failed"
		reasons.append("scene_exit_failed")
		return {
			"classification": classification,
			"reasons": reasons,
		}
	var saw_under_sampled := false
	var saw_overload := false
	for phase_v in load_delivery.get("by_phase", []):
		if typeof(phase_v) != TYPE_DICTIONARY:
			continue
		var phase: Dictionary = phase_v
		var status := str(phase.get("completion_status", ""))
		if status == "under_sampled":
			saw_under_sampled = true
			reasons.append("phase_under_sampled:%s_%d" % [str(phase.get("bundle_label", "")), int(phase.get("phase_index", -1))])
		elif status == "timeout":
			saw_overload = true
			reasons.append("phase_timeout:%s_%d" % [str(phase.get("bundle_label", "")), int(phase.get("phase_index", -1))])
		var scheduled_attempts := int(phase.get("scheduled_attempts", 0))
		var admitted := int(phase.get("admitted", 0))
		var completed := int(phase.get("completed", 0))
		var admission_ratio := float(admitted) / float(scheduled_attempts) if scheduled_attempts > 0 else 0.0
		var completion_ratio := float(completed) / float(scheduled_attempts) if scheduled_attempts > 0 else 0.0
		if scheduled_attempts > 0 and admission_ratio < 0.75:
			saw_overload = true
			reasons.append("admission_ratio_below_target:%s_%d" % [str(phase.get("bundle_label", "")), int(phase.get("phase_index", -1))])
		if scheduled_attempts > 0 and completion_ratio < 0.75:
			saw_overload = true
			reasons.append("completion_ratio_below_target:%s_%d" % [str(phase.get("bundle_label", "")), int(phase.get("phase_index", -1))])
		if int(phase.get("frame_count", 0)) < _minimum_frames_per_phase:
			saw_under_sampled = true
			reasons.append("phase_frame_cadence_below_target:%s_%d" % [str(phase.get("bundle_label", "")), int(phase.get("phase_index", -1))])
	if saw_under_sampled:
		classification = "under_sampled"
	elif saw_overload:
		classification = "overload_diagnostic"
	return {
		"classification": classification,
		"reasons": reasons,
	}


func _exit_visual_hold_summary() -> Dictionary:
	var observed_sec := 0.0
	if _exit_visual_hold_entered and _exit_visual_hold_ended_us >= _exit_visual_hold_started_us:
		observed_sec = float(_exit_visual_hold_ended_us - _exit_visual_hold_started_us) / 1000000.0
	return {
		"configured_sec": _exit_visual_hold_sec,
		"eligible": not _is_headless and _exit_visual_hold_sec > 0.0,
		"entered": _exit_visual_hold_entered,
		"started_us": _exit_visual_hold_started_us,
		"ended_us": _exit_visual_hold_ended_us,
		"observed_sec": observed_sec,
		"frame_count": _exit_visual_hold_frame_count,
		"frame_stats": _numeric_stats(_exit_visual_hold_frame_ms),
	}


func _benchmark_elapsed_us() -> int:
	if _benchmark_finished_us > 0:
		return _benchmark_finished_us - _started_us
	return _now_us() - _started_us


func _run_context() -> Dictionary:
	return {
		"os_name": OS.get_name(),
		"display_server": DisplayServer.get_name(),
		"headless": _is_headless,
		"cmdline_user_args": OS.get_cmdline_user_args(),
		"frame_count": _frame_count,
		"benchmark_frame_count": _benchmark_frame_count,
	}


func _group_phase_records_by_bundle(records: Array) -> Array:
	var grouped := []
	for bundle in _bundle_definitions():
		var label := str(bundle.get("label", ""))
		var phases := []
		for rec_v in records:
			if typeof(rec_v) != TYPE_DICTIONARY:
				continue
			var rec: Dictionary = rec_v
			if str(rec.get("bundle_label", "")) == label:
				phases.append(rec)
		grouped.append({
			"label": label,
			"capture_member_count": int(bundle.get("member_count", 0)),
			"capture_ev_series_milli": bundle.get("ev_milli", []),
			"phase_results": phases,
		})
	return grouped


func _backing_plan_acquisition_session_reports_summary(synthetic_metrics: Variant) -> Array:
	var summarized: Array = []
	if typeof(synthetic_metrics) != TYPE_DICTIONARY:
		return summarized
	var reports_v = (synthetic_metrics as Dictionary).get("backing_plan_evaluation_reports", [])
	if typeof(reports_v) != TYPE_ARRAY:
		return summarized
	for report_v in reports_v:
		if typeof(report_v) != TYPE_DICTIONARY:
			continue
		var report: Dictionary = report_v
		if str(report.get("parent_kind", "")) != "acquisition_session":
			continue
		summarized.append(_backing_plan_acquisition_session_report_summary(report, synthetic_metrics))
	return summarized


func _backing_plan_acquisition_session_report_summary(report: Dictionary, synthetic_metrics: Variant) -> Dictionary:
	var summary := {
		"parent_id": int(report.get("parent_id", 0)),
		"acquisition_session_id": int(report.get("acquisition_session_id", 0)),
		"device_instance_id": int(report.get("device_instance_id", 0)),
		"stream_id": int(report.get("stream_id", 0)),
		"target_kind": str(report.get("target_kind", "")),
		"target_id": int(report.get("target_id", 0)),
		"parent_kind": str(report.get("parent_kind", "")),
		"primary_function": str(report.get("primary_function", "")),
		"evaluator_active": bool(report.get("evaluator_active", false)),
		"requested": report.get("requested", {}),
		"steady": report.get("steady", {}),
		"completion_reason": str(report.get("completion_reason", "")),
		"current_candidate_index": int(report.get("current_candidate_index", -1)),
		"candidate_sequence": report.get("candidate_sequence", []),
	}
	var candidate_evidence := _backing_plan_candidate_evidence_summary_entries(report, synthetic_metrics)
	summary["candidate_evidence"] = candidate_evidence
	summary["current_candidate_evidence"] = _backing_plan_current_candidate_evidence_summary(report, candidate_evidence)
	return summary


func _backing_plan_current_candidate_evidence_summary(report: Dictionary, candidate_evidence: Array) -> Dictionary:
	var current_candidate_index := int(report.get("current_candidate_index", -1))
	if current_candidate_index < 0 or current_candidate_index >= candidate_evidence.size():
		return {}
	var entry_v = candidate_evidence[current_candidate_index]
	if typeof(entry_v) != TYPE_DICTIONARY:
		return {}
	return entry_v


func _acquisition_session_settlement_probe_summary(synthetic_metrics: Variant) -> Dictionary:
	var summary := {
		"enabled": true,
		"attempted": _acq_probe_attempted,
		"bundle_label": _acq_probe_bundle_label,
		"required_member_count": _acq_probe_required_member_count,
		"settle_timeout_us": SETTLEMENT_PROBE_SETTLE_TIMEOUT_US,
		"settle_started_us": _acq_probe_settle_started_us,
		"settle_deadline_us": _acq_probe_settle_deadline_us,
		"devices": [],
	}
	if not _acq_probe_attempted:
		return summary
	var reports := _backing_plan_acquisition_session_reports_summary(synthetic_metrics)
	for device_key in [DEV_A, DEV_B]:
		var entry_v = _acq_probe_devices.get(device_key, {})
		if typeof(entry_v) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = entry_v
		var report := _backing_plan_acquisition_session_report_for_device_id(reports, int(entry.get("device_id", 0)))
		var current_candidate_evidence: Dictionary = {}
		if not report.is_empty():
			var current_candidate_evidence_v: Variant = report.get("current_candidate_evidence", {})
			if typeof(current_candidate_evidence_v) == TYPE_DICTIONARY:
				current_candidate_evidence = current_candidate_evidence_v
		var probe_capture_id := int(entry.get("capture_id", 0))
		var live_observed_capture_id := int(current_candidate_evidence.get("observed_capture_id", 0))
		var live_observed_source: Dictionary = {}
		var live_observed_source_v: Variant = current_candidate_evidence.get("observed_source", {})
		if typeof(live_observed_source_v) == TYPE_DICTIONARY:
			live_observed_source = live_observed_source_v
		var live_observed_source_bundle_label := str(live_observed_source.get("bundle_label", ""))
		var settlement_status := _acq_probe_settlement_status(entry, report)
		var attribution_status := _acq_probe_attribution_status(
			entry,
			report,
			current_candidate_evidence,
			live_observed_source_bundle_label,
			settlement_status)
		summary["devices"].append({
			"device_key": device_key,
			"device_id": int(entry.get("device_id", 0)),
			"acquisition_session_id": int(entry.get("acquisition_session_id", 0)),
			"capture_id": probe_capture_id,
			"live_observed_capture_id": live_observed_capture_id,
			"live_observed_source_bundle_label": live_observed_source_bundle_label,
			"trigger_status": str(entry.get("trigger_status", "")),
			"capture_complete": bool(entry.get("capture_complete", false)),
			"capture_failed": bool(entry.get("capture_failed", false)),
			"returned_member_count": int(entry.get("returned_member_count", 0)),
			"materialized_member_count": int(entry.get("materialized_member_count", 0)),
			"materialized_member_indices": entry.get("materialized_member_indices", []),
			"materialization_failed_indices": entry.get("materialization_failed_indices", []),
			"materialization_complete": bool(entry.get("materialization_complete", false)),
			"settlement_status": settlement_status,
			"attribution_status": attribution_status,
			"attribution_matches_probe_capture": attribution_status == "matches_probe_capture",
			"attribution_mismatch_reason": _acq_probe_attribution_mismatch_reason(
				probe_capture_id, live_observed_capture_id, live_observed_source, settlement_status
			),
			"report": report,
			"current_candidate_evidence": current_candidate_evidence,
		})
	return summary


func _acq_probe_settlement_status(entry: Dictionary, report: Dictionary) -> String:
	if not _acq_probe_attempted:
		return "probe_not_attempted"
	if not bool(entry.get("capture_complete", false)):
		return "capture_failed"
	if bool(entry.get("capture_failed", false)):
		return "capture_failed"
	var required_count := _acq_probe_required_member_count
	var returned_count := int(entry.get("returned_member_count", 0))
	if returned_count < required_count:
		return "capture_returned_fewer_than_5_members"
	var materialized_count := int(entry.get("materialized_member_count", 0))
	if materialized_count < required_count:
		return "not_all_members_materialised"
	if report.is_empty():
		return "members_materialised_but_evaluator_still_active"
	if bool(report.get("evaluator_active", false)):
		return "members_materialised_but_evaluator_still_active"
	var current_candidate_evidence_v: Variant = report.get("current_candidate_evidence", {})
	if typeof(current_candidate_evidence_v) == TYPE_DICTIONARY:
		var current_candidate_evidence: Dictionary = current_candidate_evidence_v
		if str(current_candidate_evidence.get("capture_evidence_incomplete_reason", "")) != "none":
			return "members_materialised_but_evaluator_still_active"
	return "evaluator_settled"


func _acq_probe_attribution_status(
		entry: Dictionary,
		report: Dictionary,
		current_candidate_evidence: Dictionary,
		live_observed_source_bundle_label: String,
		settlement_status: String) -> String:
	if not _acq_probe_attempted:
		return "probe_not_attempted"
	if report.is_empty():
		return "no_live_report"
	var probe_capture_id := int(entry.get("capture_id", 0))
	var live_capture_id := int(current_candidate_evidence.get("observed_capture_id", 0))
	if probe_capture_id <= 0:
		return "no_probe_capture"
	if live_capture_id <= 0:
		return "no_live_evidence"
	if live_capture_id == probe_capture_id:
		return "matches_probe_capture"
	if live_observed_source_bundle_label != _acq_probe_bundle_label:
		return "prior_capture_family_attribution"
	if settlement_status == "evaluator_settled":
		return "settled_before_probe_capture"
	return "stale_previous_capture_attribution"


func _acq_probe_attribution_mismatch_reason(
		probe_capture_id: int,
		live_observed_capture_id: int,
		live_observed_source: Dictionary,
		settlement_status: String) -> String:
	if probe_capture_id <= 0:
		return "no_probe_capture"
	if live_observed_capture_id <= 0:
		return "no_live_evidence"
	if live_observed_capture_id != probe_capture_id:
		if str(live_observed_source.get("bundle_label", "")) != _acq_probe_bundle_label:
			return "live_report_references_prior_capture_family"
		if settlement_status == "evaluator_settled":
			return "settled_before_probe_capture"
		return "live_report_references_different_capture"
	if live_observed_source.is_empty():
		return "no_live_source"
	if str(live_observed_source.get("bundle_label", "")) != _acq_probe_bundle_label:
		return "live_report_bundle_label_mismatch"
	if int(live_observed_source.get("expected_member_count", 0)) != _acq_probe_required_member_count:
		return "live_report_expected_member_count_mismatch"
	return ""


func _run_quality_warnings_summary(
	acquisition_session_settlement_probe: Dictionary,
	synthetic_metrics: Variant
) -> Array:
	var warnings: Array = []
	var cpu_display_refresh := _cpu_display_refresh_observation_summary(synthetic_metrics)
	if int(cpu_display_refresh.get("live_attempts", 0)) > 0:
		warnings.append({
			"code": "live_cpu_display_refresh_observed",
			"live_attempts": int(cpu_display_refresh.get("live_attempts", 0)),
			"live_updated": int(cpu_display_refresh.get("live_updated", 0)),
			"live_total_ms": float(cpu_display_refresh.get("live_total_ms", 0.0)),
			"live_update_ms": float(cpu_display_refresh.get("live_update_ms", 0.0)),
			"aggregate_attempts": int(cpu_display_refresh.get("aggregate_attempts", 0)),
			"aggregate_updated": int(cpu_display_refresh.get("aggregate_updated", 0)),
		})
	if not bool(acquisition_session_settlement_probe.get("attempted", false)):
		return warnings
	var required_count := int(acquisition_session_settlement_probe.get("required_member_count", 0))
	for device_v in acquisition_session_settlement_probe.get("devices", []):
		if typeof(device_v) != TYPE_DICTIONARY:
			continue
		var device: Dictionary = device_v
		var settlement_status := str(device.get("settlement_status", ""))
		if settlement_status == "evaluator_settled":
			continue
		var report_v: Variant = device.get("report", {})
		var report: Dictionary = report_v if typeof(report_v) == TYPE_DICTIONARY else {}
		var current_candidate_evidence_v: Variant = device.get("current_candidate_evidence", {})
		var current_candidate_evidence: Dictionary = current_candidate_evidence_v if typeof(current_candidate_evidence_v) == TYPE_DICTIONARY else {}
		var steady_valid := false
		var steady_v: Variant = report.get("steady", {})
		if typeof(steady_v) == TYPE_DICTIONARY:
			steady_valid = bool((steady_v as Dictionary).get("valid", false))
		var attribution_status := str(device.get("attribution_status", ""))
		if settlement_status != "evaluator_settled":
			warnings.append({
				"code": "acquisition_session_evaluator_unsettled_after_probe",
				"device_key": str(device.get("device_key", "")),
				"device_id": int(device.get("device_id", 0)),
				"acquisition_session_id": int(device.get("acquisition_session_id", 0)),
				"status": settlement_status if settlement_status != "" else "probe_not_attempted",
				"bundle_label": str(acquisition_session_settlement_probe.get("bundle_label", "")),
				"required_capture_member_count": required_count,
				"returned_member_count": int(device.get("returned_member_count", 0)),
				"materialized_capture_member_count": int(device.get("materialized_member_count", 0)),
				"capture_evidence_incomplete_reason": str(current_candidate_evidence.get("capture_evidence_incomplete_reason", "")),
				"has_first_missing_required_capture_member_index": bool(current_candidate_evidence.get("has_first_missing_required_capture_member_index", false)),
				"first_missing_required_capture_member_index": int(current_candidate_evidence.get("first_missing_required_capture_member_index", 0)),
				"evaluator_active": bool(report.get("evaluator_active", false)),
				"steady_valid": steady_valid,
				"current_candidate_index": int(report.get("current_candidate_index", -1)),
				"observed_source": current_candidate_evidence.get("observed_source", {}),
			})
		if attribution_status == "prior_capture_family_attribution":
			warnings.append({
				"code": "acquisition_session_evaluator_prior_capture_family_after_probe",
				"device_key": str(device.get("device_key", "")),
				"device_id": int(device.get("device_id", 0)),
				"acquisition_session_id": int(device.get("acquisition_session_id", 0)),
				"status": attribution_status,
				"probe_capture_id": int(device.get("capture_id", 0)),
				"live_observed_capture_id": int(device.get("live_observed_capture_id", 0)),
				"bundle_label": str(acquisition_session_settlement_probe.get("bundle_label", "")),
				"live_observed_source_bundle_label": str(device.get("live_observed_source_bundle_label", "")),
				"live_observed_source": current_candidate_evidence.get("observed_source", {}),
				"current_candidate_index": int(report.get("current_candidate_index", -1)),
			})
		elif attribution_status == "stale_previous_capture_attribution":
			warnings.append({
				"code": "acquisition_session_evaluator_stale_attribution_after_probe",
				"device_key": str(device.get("device_key", "")),
				"device_id": int(device.get("device_id", 0)),
				"acquisition_session_id": int(device.get("acquisition_session_id", 0)),
				"status": attribution_status,
				"probe_capture_id": int(device.get("capture_id", 0)),
				"live_observed_capture_id": int(device.get("live_observed_capture_id", 0)),
				"bundle_label": str(acquisition_session_settlement_probe.get("bundle_label", "")),
				"live_observed_source_bundle_label": str(device.get("live_observed_source_bundle_label", "")),
				"live_observed_source": current_candidate_evidence.get("observed_source", {}),
				"current_candidate_index": int(report.get("current_candidate_index", -1)),
			})
	return warnings


func _cpu_display_refresh_observation_summary(synthetic_metrics: Variant) -> Dictionary:
	if typeof(synthetic_metrics) != TYPE_DICTIONARY:
		return {
			"available": false,
		}
	var metrics: Dictionary = synthetic_metrics
	return {
		"available": true,
		"aggregate_attempts": int(metrics.get("cpu_display_refresh_attempts", 0)),
		"aggregate_updated": int(metrics.get("cpu_display_refresh_updated", 0)),
		"aggregate_total_ms": float(metrics.get("cpu_display_refresh_total_ms", 0.0)),
		"aggregate_update_ms": float(metrics.get("cpu_display_refresh_update_ms", 0.0)),
		"live_attempts": int(metrics.get("cpu_display_refresh_live_attempts", 0)),
		"live_updated": int(metrics.get("cpu_display_refresh_live_updated", 0)),
		"live_total_ms": float(metrics.get("cpu_display_refresh_live_total_ms", 0.0)),
		"live_update_ms": float(metrics.get("cpu_display_refresh_live_update_ms", 0.0)),
		"ephemeral_attempts": int(metrics.get("cpu_display_refresh_ephemeral_attempts", 0)),
		"ephemeral_updated": int(metrics.get("cpu_display_refresh_ephemeral_updated", 0)),
		"ephemeral_total_ms": float(metrics.get("cpu_display_refresh_ephemeral_total_ms", 0.0)),
		"ephemeral_update_ms": float(metrics.get("cpu_display_refresh_ephemeral_update_ms", 0.0)),
	}


func _backing_plan_candidate_evidence_summary_entries(report: Dictionary, synthetic_metrics: Variant) -> Array:
	var entries: Array = []
	var entries_v = report.get("candidate_evidence", [])
	if typeof(entries_v) != TYPE_ARRAY:
		return entries
	for entry_v in entries_v:
		if typeof(entry_v) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = entry_v
		entries.append(_backing_plan_candidate_evidence_summary(entry, synthetic_metrics))
	return entries


func _backing_plan_candidate_evidence_summary(entry: Dictionary, synthetic_metrics: Variant) -> Dictionary:
	var observed_capture_id := int(entry.get("observed_capture_id", 0))
	var candidate_v: Variant = entry.get("candidate", {})
	var candidate: Dictionary = candidate_v if typeof(candidate_v) == TYPE_DICTIONARY else {}
	return {
		"candidate": candidate,
		"observed_capture_id": observed_capture_id,
		"observed_acquisition_session_id": int(entry.get("observed_acquisition_session_id", 0)),
		"required_capture_member_count": int(entry.get("required_capture_member_count", 0)),
		"observed_capture_member_count": int(entry.get("observed_capture_member_count", 0)),
		"materialized_capture_member_count": int(entry.get("materialized_capture_member_count", 0)),
		"has_first_missing_required_capture_member_index": bool(entry.get("has_first_missing_required_capture_member_index", false)),
		"first_missing_required_capture_member_index": int(entry.get("first_missing_required_capture_member_index", 0)),
		"capture_evidence_incomplete_reason": str(entry.get("capture_evidence_incomplete_reason", "")),
		"has_capture_ready_elapsed_ns": bool(entry.get("has_capture_ready_elapsed_ns", false)),
		"capture_ready_elapsed_ns": int(entry.get("capture_ready_elapsed_ns", 0)),
		"has_materialization_elapsed_ns": bool(entry.get("has_materialization_elapsed_ns", false)),
		"materialization_elapsed_ns": int(entry.get("materialization_elapsed_ns", 0)),
		"has_total_elapsed_ns": bool(entry.get("has_total_elapsed_ns", false)),
		"total_elapsed_ns": int(entry.get("total_elapsed_ns", 0)),
		"evidence_complete": bool(entry.get("evidence_complete", false)),
		"evidence_accepted": bool(entry.get("evidence_accepted", false)),
		"observed_posture": str(entry.get("observed_posture", "")),
		"observed_payload_kind": str(entry.get("observed_payload_kind", "")),
		"provisional_to_image": str(entry.get("provisional_to_image", "")),
		"capture_ready_timing_attribution": entry.get("capture_ready_timing_attribution", {}),
		"observed_source": _capture_source_provenance_summary(observed_capture_id),
	}


func _record_capture_sample_provenance(sample: Dictionary, job: Dictionary, is_rig_member: bool) -> void:
	var capture_id := int(sample.get("capture_id", 0))
	if capture_id == 0:
		return
	_capture_provenance_by_capture_id[capture_id] = {
		"bundle_label": str(job.get("bundle_label", "")),
		"phase_index": int(job.get("phase_index", -1)),
		"action_label": str(job.get("action_label", "")),
		"scope": str(job.get("scope", "")),
		"sample_category": "rig_capture_member" if is_rig_member else "device_capture",
		"device_id": int(sample.get("device_id", 0)),
		"device_key": str(sample.get("device_key", "")),
		"expected_member_count": int(sample.get("expected_member_count", 0)),
		"returned_member_count": int(sample.get("returned_member_count", 0)),
		"status": str(sample.get("status", "")),
	}


func _capture_source_provenance_summary(capture_id: int) -> Dictionary:
	if capture_id == 0:
		return {}
	var provenance_v = _capture_provenance_by_capture_id.get(capture_id, {})
	if typeof(provenance_v) != TYPE_DICTIONARY:
		return {}
	var provenance: Dictionary = provenance_v
	return {
		"capture_id": capture_id,
		"bundle_label": str(provenance.get("bundle_label", "")),
		"phase_index": int(provenance.get("phase_index", -1)),
		"action_label": str(provenance.get("action_label", "")),
		"scope": str(provenance.get("scope", "")),
		"sample_category": str(provenance.get("sample_category", "")),
		"device_id": int(provenance.get("device_id", 0)),
		"device_key": str(provenance.get("device_key", "")),
		"expected_member_count": int(provenance.get("expected_member_count", 0)),
		"returned_member_count": int(provenance.get("returned_member_count", 0)),
		"status": str(provenance.get("status", "")),
	}


func _stream_display_observation_summary() -> Dictionary:
	var out := {}
	for device_key in [DEV_A, DEV_B]:
		var info: Dictionary = _devices[device_key]
		out[device_key] = {
			"label": str(info.get("label", device_key)),
			"stream_id": int(info.get("stream_id", 0)),
			"device_id": int(info.get("device_id", 0)),
			"observed_result_advancements": int(info.get("stream_observed_changes", 0)),
			"observed_result_update_fps": _stream_observed_fps(device_key),
			"observed_update_count": int(info.get("stream_observed_changes", 0)),
			"display_view_bound": bool(info.get("live_display_bound", false)),
		}
	return out


func _emit_framed_record(record_name: String, kind: String, payload_text: String) -> void:
	var payload_base64 := Marshalls.utf8_to_base64(payload_text)
	const CHUNK_SIZE := 768
	var chunk_count := int(ceil(float(payload_base64.length()) / float(CHUNK_SIZE))) if payload_base64.length() > 0 else 0
	print("[CamBANG][RecordStart] id=%s name=%s kind=%s chunks=%d encoding=base64" % [SCENE_RECORD_ID, record_name, kind, chunk_count])
	var chunk_index := 0
	var offset := 0
	while offset < payload_base64.length():
		var chunk := payload_base64.substr(offset, CHUNK_SIZE)
		print("[CamBANG][RecordChunk] id=%s index=%d data=%s" % [SCENE_RECORD_ID, chunk_index, chunk])
		offset += CHUNK_SIZE
		chunk_index += 1
	print("[CamBANG][RecordEnd] id=%s" % SCENE_RECORD_ID)


func _finish(exit_code: int, expected_unsupported: bool, reason: String = "complete") -> void:
	if _done:
		return
	_done = true
	_finish_reason = reason if reason != "" else ("expected_unsupported" if expected_unsupported else ("complete" if exit_code == 0 else "failure"))
	_state = PHASE_DONE
	if not _active_phase.is_empty() and int(_active_phase.get("ended_us", 0)) == 0:
		_active_phase["ended_us"] = _now_us()
		_completed_phase_records.append(_summarize_phase_record(_active_phase))
		_active_phase = {}
	if not _benchmark_metrics_frozen:
		_freeze_benchmark_metrics()
	if not expected_unsupported and not _is_headless and _exit_visual_hold_sec > 0.0:
		_log("complete: holding populated dashboard for %.2fs before teardown" % _exit_visual_hold_sec)
	_cleanup_and_quit(exit_code, not expected_unsupported, expected_unsupported)


func _fail(message: String) -> void:
	if _done:
		return
	_failed = true
	push_error("%s: %s" % [SCENE_LABEL, message])
	_log("FAIL: %s" % message)
	_finish(1, false, "failure")


func _cleanup_and_quit(exit_code: int, preserve_visuals_for_hold: bool, expected_unsupported: bool) -> void:
	if _cleanup_started:
		return
	_cleanup_started = true
	set_process(false)
	call_deferred("_hold_then_emit_stop_and_quit", exit_code, preserve_visuals_for_hold, expected_unsupported)


func _hold_then_emit_stop_and_quit(exit_code: int, preserve_visuals_for_hold: bool, expected_unsupported: bool) -> void:
	# Keep the populated dashboard visible briefly after benchmark timing has been
	# frozen. The hold is measured separately and is skipped for headless or
	# expected-unsupported runs.
	if preserve_visuals_for_hold and not _is_headless and _exit_visual_hold_sec > 0.0:
		await _run_exit_visual_hold()
	_emit_final_summary_and_marker(exit_code, expected_unsupported)
	_release_pending_runtime_references()
	# TextureRect.texture assignments release their Ref<> immediately, but the
	# display-view wrapper lifetime guard is intentionally conservative. Yield a
	# couple of frames so queued-free thumbnail cells and TextureRect releases are
	# observed before CamBANGServer.stop() tears down retained GPU display views.
	await get_tree().process_frame
	await get_tree().process_frame
	CamBANGServer.stop()
	get_tree().quit(exit_code)


func _run_exit_visual_hold() -> void:
	_exit_visual_hold_entered = true
	_exit_visual_hold_started_us = _now_us()
	_exit_visual_hold_ended_us = _exit_visual_hold_started_us
	_exit_visual_hold_frame_count = 0
	_exit_visual_hold_frame_ms.clear()
	var target_us := _exit_visual_hold_started_us + int(round(_exit_visual_hold_sec * 1000000.0))
	var previous_us := _exit_visual_hold_started_us
	while _now_us() < target_us:
		await get_tree().process_frame
		var current_us := _now_us()
		_exit_visual_hold_frame_ms.append(float(current_us - previous_us) / 1000.0)
		previous_us = current_us
	_exit_visual_hold_ended_us = _now_us()
	_exit_visual_hold_frame_count = _exit_visual_hold_frame_ms.size()


func _emit_final_summary_and_marker(exit_code: int, expected_unsupported: bool) -> void:
	_emit_harness_verdict(_harness_status_for_finish(exit_code, expected_unsupported), exit_code, _finish_reason)
	var summary := _build_summary(exit_code, expected_unsupported)
	var payload_text := JSON.stringify(summary)
	_write_summary_file(payload_text)
	_emit_framed_record("scene870_to_image_soak_summary", "json", payload_text)


func _harness_status_for_finish(exit_code: int, expected_unsupported: bool) -> String:
	if expected_unsupported:
		return "expected_unsupported"
	if exit_code == 0:
		return "ok"
	return "fail"


func _emit_harness_verdict(status: String, exit_code: int, reason: String) -> void:
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL,
		status,
		exit_code,
		reason,
	])


func _write_summary_file(payload_text: String) -> void:
	const SUMMARY_DIR := "user://cambang_records"
	const SUMMARY_USER_PATH := "cambang_records/scene870_to_image_soak_summary.json"
	const SUMMARY_FULL_PATH := "user://cambang_records/scene870_to_image_soak_summary.json"
	var dir_err := DirAccess.make_dir_recursive_absolute(SUMMARY_DIR)
	if dir_err != OK:
		print("[CamBANG][RecordFileError] id=%s path=%s error=%d" % [
			SCENE_RECORD_ID,
			SUMMARY_FULL_PATH,
			dir_err,
		])
		return
	var file := FileAccess.open(SUMMARY_FULL_PATH, FileAccess.WRITE)
	if file == null:
		print("[CamBANG][RecordFileError] id=%s path=%s error=%d" % [
			SCENE_RECORD_ID,
			SUMMARY_FULL_PATH,
			FileAccess.get_open_error(),
		])
		return
	file.store_string(payload_text)
	file.close()
	print("[CamBANG][RecordFile] id=%s name=scene870_to_image_soak_summary kind=json user_path=%s bytes=%d" % [
		SCENE_RECORD_ID,
		SUMMARY_USER_PATH,
		payload_text.to_utf8_buffer().size(),
	])


func _release_pending_runtime_references() -> void:
	# Drop pending jobs first; thumbnail jobs retain CaptureResult objects and UI
	# cell references, and the live stream panels retain GPU display-view wrappers.
	_stream_jobs.clear()
	_capture_jobs.clear()
	_rig_jobs.clear()
	_thumbnail_jobs.clear()
	for device_key in [DEV_A, DEV_B]:
		_clear_texture_rect(_stream_live_rects.get(device_key, null))
		_clear_texture_rect(_stream_image_rects.get(device_key, null))
		_clear_texture_rect(_capture_rects.get(device_key, null))
		_clear_texture_rect(_rig_rects.get(device_key, null))
		_clear_descendant_texture_rects(_capture_rows.get(device_key, null))
		_clear_descendant_texture_rects(_rig_rows.get(device_key, null))
		var info: Dictionary = _devices[device_key]
		info["live_display_bound"] = false
		_devices[device_key] = info


func _clear_texture_rect(value: Variant) -> void:
	if value is TextureRect:
		var rect: TextureRect = value
		rect.texture = null


func _clear_descendant_texture_rects(value: Variant) -> void:
	if not (value is Node):
		return
	var root: Node = value as Node
	for child_node in root.get_children():
		if child_node is TextureRect:
			var rect: TextureRect = child_node as TextureRect
			rect.texture = null
		_clear_descendant_texture_rects(child_node)


func _assign_or_update_panel_image_texture(texture_map: Dictionary, key: String, image: Image) -> Texture2D:
	if image == null or image.is_empty():
		return null
	var existing = texture_map.get(key, null)
	if existing is ImageTexture:
		var existing_texture: ImageTexture = existing
		if _can_update_image_texture(existing_texture, image):
			existing_texture.update(image)
			return existing_texture
	var created := ImageTexture.create_from_image(image)
	texture_map[key] = created
	return created


func _prime_fixed_panel_texture(texture_map: Dictionary, key: String, rect_value: Variant) -> void:
	if texture_map.has(key):
		_set_texture_if_changed(rect_value, texture_map.get(key, null))
		return
	var image := Image.create_empty(STILL_PROFILE_WIDTH, STILL_PROFILE_HEIGHT, false, PANEL_SEED_IMAGE_FORMAT)
	if image == null:
		return
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	var texture := ImageTexture.create_from_image(image)
	texture_map[key] = texture
	_set_texture_if_changed(rect_value, texture)


func _can_update_image_texture(texture: ImageTexture, image: Image) -> bool:
	if texture == null or image == null or image.is_empty():
		return false
	return texture.get_width() == image.get_width() \
		and texture.get_height() == image.get_height()


func _member_strip_texture_key(_row: HBoxContainer, member_index: int) -> String:
	return "%d" % member_index


func _member_strip_textures(row: HBoxContainer) -> Dictionary:
	var textures_v: Variant = row.get_meta("member_strip_textures") if row.has_meta("member_strip_textures") else {}
	return textures_v if typeof(textures_v) == TYPE_DICTIONARY else {}


func _assign_or_update_member_strip_texture(row_value: Variant, key: String, image: Image) -> Texture2D:
	if not (row_value is HBoxContainer):
		return null
	var row: HBoxContainer = row_value
	var textures := _member_strip_textures(row)
	var existing = textures.get(key, null)
	if existing is ImageTexture:
		var existing_texture: ImageTexture = existing
		if _can_update_image_texture(existing_texture, image):
			existing_texture.update(image)
			row.set_meta("member_strip_textures", textures)
			return existing_texture
	var created := ImageTexture.create_from_image(image)
	textures[key] = created
	row.set_meta("member_strip_textures", textures)
	return created


func _assign_or_clear_member_strip_texture(row: HBoxContainer, key: String, rect: TextureRect, texture: Texture2D) -> void:
	if rect != null:
		rect.texture = texture
	if texture == null:
		var textures := _member_strip_textures(row)
		if textures.has(key):
			textures.erase(key)
			row.set_meta("member_strip_textures", textures)


func _member_strip_release_texture(row: HBoxContainer, member_index: int) -> void:
	var textures := _member_strip_textures(row)
	var key := _member_strip_texture_key(row, member_index)
	if textures.has(key):
		textures.erase(key)
		row.set_meta("member_strip_textures", textures)


func _set_texture_if_changed(rect_value: Variant, texture: Texture2D) -> void:
	if not (rect_value is TextureRect):
		return
	var rect: TextureRect = rect_value
	if rect.texture == texture:
		return
	rect.texture = texture


func _set_label_text_if_changed(label_value: Variant, text: String) -> void:
	if label_value == null:
		return
	if not (label_value is Label):
		return
	var label: Label = label_value
	if label.text == text:
		return
	label.text = text

func _record_log_to_ui(message: String) -> void:
	if _log_label != null:
		var should_follow := true
		var v_scroll = _log_label.get_v_scroll_bar()
		if v_scroll != null:
			should_follow = float(v_scroll.value) >= float(v_scroll.max_value - v_scroll.page) - 4.0
		_log_label.append_text("%s\n" % message)
		if should_follow:
			_log_label.scroll_to_line(maxi(0, _log_label.get_line_count() - 1))


func _log(message: String) -> void:
	var elapsed := float(_now_us() - _started_us) / 1000000.0 if _started_us > 0 else 0.0
	var line := "[CamBANG][Scene870] t=%s %s" % [String.num(elapsed, PRINT_DECIMALS), message]
	print(line)
	_record_log_to_ui(line)


func _update_stats_label(force: bool) -> void:
	var now := _now_us()
	if not force and now - _last_stats_update_us < 250000:
		return
	_last_stats_update_us = now
	if _header_label != null:
		var phase_text := "setup"
		if not _active_phase.is_empty():
			phase_text = "%s %s %s %d/%d" % [
				str(_active_phase.get("bundle_label", "")),
				str(_active_phase.get("mode", "")),
				str(_active_phase.get("action_label", "")),
				int(_active_phase.get("phase_index", -1)) + 1,
				int(_active_phase.get("phase_count", 0)),
			]
		_header_label.text = "Scene 870 · provider=%s · seed=%d · %s" % [_provider_arg, _seed, phase_text]
	if _stats_label != null:
		var elapsed_sec: float = float(now - _started_us) / 1000000.0
		if elapsed_sec < 0.001:
			elapsed_sec = 0.001
		var run_fps: float = float(_frame_count) / elapsed_sec
		_stats_label.text = "fps=%.1f active stream_jobs=%d capture_jobs=%d rig_jobs=%d thumbnails=%d inflight D1=%d D2=%d rig=%d stream_update_fps D1=%.2f D2=%.2f" % [
			run_fps,
			_stream_jobs.size(),
			_capture_jobs.size(),
			_rig_jobs.size(),
			_thumbnail_jobs.size(),
			int(_devices[DEV_A].get("inflight_captures", 0)),
			int(_devices[DEV_B].get("inflight_captures", 0)),
			_rig_inflight,
			_stream_observed_fps(DEV_A),
			_stream_observed_fps(DEV_B),
		]


func _member_label(index: int, members: Array) -> String:
	if index >= 0 and index < members.size() and typeof(members[index]) == TYPE_DICTIONARY:
		var member: Dictionary = members[index]
		var ev := int(member.get("intended_exposure_compensation_milli_ev", 0))
		if ev == 0:
			return "0 metered\nidx=%d" % index
		var ev_sign := "+" if ev > 0 else ""
		return "%s%.1f EV\nidx=%d" % [ev_sign, float(ev) / 1000.0, index]
	return "idx=%d" % index


func _image_summary(image) -> String:
	if image == null:
		return "null"
	if image.is_empty():
		return "empty"
	return "%dx%d fmt=%d" % [image.get_width(), image.get_height(), image.get_format()]


func _visual_textures_enabled() -> bool:
	return not _is_headless or _materialize_textures_in_headless


func _now_us() -> int:
	return Time.get_ticks_usec()


func _elapsed_sec_since(start_us: int) -> float:
	if start_us <= 0:
		return 0.0
	return float(_now_us() - start_us) / 1000000.0
