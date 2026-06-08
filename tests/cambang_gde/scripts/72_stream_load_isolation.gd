extends Control

const DEFAULT_SCENARIO_FILE := "stream_load_2x_1080p30_moving_bar.json"#"stream_load_2x_720p30.json"
const DEFAULT_DURATION_SEC := 20.0
const DEFAULT_POLL_RESULTS := true
const DEFAULT_BIND_DISPLAY := true
const EXERCISE_DISPLAY_ONESHOT := "display_oneshot"
const EXERCISE_DISPLAY_LATEST := "display_latest"
const EXERCISE_NO_DISPLAY_DEFAULT := "no_display_default"
const EXERCISE_NO_DISPLAY_EAGER := "no_display_eager"
const PRINT_DECIMALS := 2

@onready var _status: RichTextLabel = $RootMargin/MainColumn/Status
@onready var _stream_a: TextureRect = $RootMargin/MainColumn/Streams/StreamA
@onready var _stream_b: TextureRect = $RootMargin/MainColumn/Streams/StreamB

var _duration_sec := DEFAULT_DURATION_SEC
var _poll_results := DEFAULT_POLL_RESULTS
var _bind_display := DEFAULT_BIND_DISPLAY
var _exercise := EXERCISE_DISPLAY_ONESHOT
var _exercise_defaulted := true
var _bind_mode_latest := false
var _display_bound_by_slot: Dictionary = {}
var _display_bind_failed_logged_by_slot: Dictionary = {}
var _requested_gpu_policy := ""
var _effective_gpu_policy := ""

var _elapsed_sec := 0.0
var _accum_sec := 0.0
var _accum_fps := 0.0
var _sample_count := 0
var _min_fps := INF
var _worst_frame_sec := 0.0
var _done := false

var _frame_spike_trace_enabled := false
var _frame_spike_top_n := 10
var _frame_spikes: Array[Dictionary] = []

var _latched_stream_ids: Array[int] = []

var _process_total_sec := 0.0
var _process_max_sec := 0.0
var _process_calls := 0

var _advance_total_sec := 0.0
var _advance_max_sec := 0.0
var _advance_calls := 0

var _latch_total_sec := 0.0
var _latch_max_sec := 0.0
var _latch_calls := 0

var _poll_total_sec := 0.0
var _poll_max_sec := 0.0
var _poll_calls := 0

var _display_total_sec := 0.0
var _display_max_sec := 0.0
var _display_calls := 0

var _log_total_sec := 0.0
var _log_max_sec := 0.0
var _log_calls := 0

var _display_path_trace_enabled := false
var _display_path_retained_gpu_backing_count := 0
var _display_path_stream_live_cpu_display_view_count := 0
var _display_path_none_count := 0
var _display_path_transition_count := 0
var _display_path_longest_gpu_streak := 0
var _display_path_longest_cpu_streak := 0
var _display_path_current_gpu_streak := 0
var _display_path_current_cpu_streak := 0
var _display_path_last_kind := -1


func _ready() -> void:
	_status.clear()
	if not _config_from_env():
		return
	_bootstrap()
	set_process(true)


func _config_from_env() -> bool:
	_resolve_exercise_or_fail()
	if _done:
		return false
	_duration_sec = _env_float("CAMBANG_STREAM_LOAD_DURATION_SEC", DEFAULT_DURATION_SEC)
	_apply_exercise_defaults()
	_poll_results = _env_bool("CAMBANG_STREAM_LOAD_POLL_RESULTS", _poll_results)
	_bind_display = _env_bool("CAMBANG_STREAM_LOAD_BIND_DISPLAY", _bind_display)
	_frame_spike_trace_enabled = _env_bool("CAMBANG_STREAM_LOAD_FRAME_SPIKE_TRACE", false)
	_frame_spike_top_n = int(_env_float("CAMBANG_STREAM_LOAD_FRAME_SPIKE_TOP_N", 10.0))
	_display_path_trace_enabled = _env_bool("CAMBANG_STREAM_LOAD_DISPLAY_PATH_TRACE", false)
	if _frame_spike_top_n < 1:
		_frame_spike_top_n = 1
	_bind_mode_latest = (_exercise == EXERCISE_DISPLAY_LATEST)
	_resolve_effective_gpu_policy_or_fail()
	return not _done


func _resolve_exercise_or_fail() -> void:
	var exercise_raw := OS.get_environment("CAMBANG_EXERCISE").strip_edges()
	if exercise_raw == "":
		_exercise = EXERCISE_DISPLAY_ONESHOT
		_exercise_defaulted = true
		return
	_exercise_defaulted = false
	match exercise_raw:
		EXERCISE_DISPLAY_ONESHOT, EXERCISE_DISPLAY_LATEST, EXERCISE_NO_DISPLAY_DEFAULT, EXERCISE_NO_DISPLAY_EAGER:
			_exercise = exercise_raw
		_:
			push_error("[CamBANG][Scene72] exercise=%s supported=false supported_exercises=%s,%s,%s,%s" % [
				exercise_raw,
				EXERCISE_DISPLAY_ONESHOT,
				EXERCISE_DISPLAY_LATEST,
				EXERCISE_NO_DISPLAY_DEFAULT,
				EXERCISE_NO_DISPLAY_EAGER
			])
			_done = true
			get_tree().quit(2)


func _apply_exercise_defaults() -> void:
	match _exercise:
		EXERCISE_DISPLAY_ONESHOT:
			_bind_display = true
			_poll_results = true
		EXERCISE_DISPLAY_LATEST:
			_bind_display = true
			_poll_results = true
		EXERCISE_NO_DISPLAY_DEFAULT:
			_bind_display = false
			_poll_results = false
		EXERCISE_NO_DISPLAY_EAGER:
			_bind_display = false
			_poll_results = false


func _resolve_effective_gpu_policy_or_fail() -> void:
	_requested_gpu_policy = ""
	if _exercise == EXERCISE_NO_DISPLAY_EAGER:
		_requested_gpu_policy = "always"
	var explicit_policy := OS.get_environment("CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY").strip_edges()
	if explicit_policy != "":
		_effective_gpu_policy = explicit_policy
		if _exercise == EXERCISE_NO_DISPLAY_EAGER and explicit_policy != "always":
			print("[CamBANG][Scene72][ERROR] configuration conflict: exercise=no_display_eager requested_policy=always explicit_policy=%s." % explicit_policy)
			print("[CamBANG][Scene72][ERROR] no_display_eager requires effective CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=always.")
			push_error("[CamBANG][Scene72] configuration conflict: exercise=no_display_eager with CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=%s" % explicit_policy)
			_done = true
			get_tree().quit(2)
		return
	if _exercise == EXERCISE_NO_DISPLAY_EAGER:
		_effective_gpu_policy = "always"
	else:
		_effective_gpu_policy = "display_demanded"


func _bootstrap() -> void:
	CamBANGServer.stop()

	var start_err: int = int(CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	))
	_require(start_err == OK, "CamBANGServer.start failed: %d" % start_err)

	var scenario_file: String = OS.get_environment("CAMBANG_STREAM_LOAD_SCENARIO")
	if scenario_file == "":
		scenario_file = DEFAULT_SCENARIO_FILE
	var scenario_path := _repo_external_scenario_path(scenario_file)
	var scenario_text := FileAccess.get_file_as_string(scenario_path)
	_require(scenario_text != "", "Scenario JSON missing at %s" % scenario_path)

	var load_err: int = int(CamBANGServer.load_external_scenario(scenario_text))
	_require(load_err == OK, "load_external_scenario failed: %d" % load_err)

	var scenario_start_err: int = int(CamBANGServer.start_scenario())
	_require(scenario_start_err == OK, "start_scenario failed: %d" % scenario_start_err)

	if CamBANGServer.has_method("set_timeline_paused"):
		var pause_err: int = int(CamBANGServer.set_timeline_paused(false))
		_require(pause_err == OK, "set_timeline_paused(false) failed: %d" % pause_err)

	_log("RUN: stream_load_isolation")
	_log("[CamBANG][Scene72] exercise=%s default=%s supported=true scenario=%s poll=%s display=%s bind=%s requested_policy=%s effective_policy=%s cap=%s" % [
		_exercise,
		str(_exercise_defaulted),
		scenario_file,
		str(_poll_results),
		str(_bind_display),
		("latest" if _bind_mode_latest else "oneshot"),
		_requested_gpu_policy,
		_effective_gpu_policy,
		OS.get_environment("CAMBANG_DEV_SYNTH_CATCHUP_CAP")
	])
	_log("scenario=%s duration_sec=%.1f poll_results=%s bind_display=%s" % [
		scenario_file,
		_duration_sec,
		str(_poll_results),
		str(_bind_display)
	])


func _process(delta: float) -> void:
	var process_start_usec := Time.get_ticks_usec()
	var frame_process_call := _process_calls + 1
	var frame_latch_sec := 0.0
	var frame_poll_sec := 0.0
	var frame_display_sec := 0.0
	var frame_log_sec := 0.0
	if _done:
		return

	_elapsed_sec += delta
	_accum_sec += delta
	_sample_count += 1

	var fps: float = 0.0
	if delta > 0.0:
		fps = 1.0 / delta
	_accum_fps += fps
	_min_fps = min(_min_fps, fps)
	_worst_frame_sec = max(_worst_frame_sec, delta)

	_advance_calls += 1
	_advance_total_sec += 0.0
	_advance_max_sec = max(_advance_max_sec, 0.0)

	if _latched_stream_ids.is_empty():
		var latch_start_usec := Time.get_ticks_usec()
		_latch_stream_ids_from_snapshot()
		var latch_sec := _elapsed_sec_from_ticks(latch_start_usec)
		frame_latch_sec = latch_sec
		_latch_total_sec += latch_sec
		_latch_max_sec = max(_latch_max_sec, latch_sec)
		_latch_calls += 1

	if _poll_results:
		var poll_start_usec := Time.get_ticks_usec()
		var display_sec := _poll_stream_results()
		frame_display_sec = display_sec
		var poll_sec := _elapsed_sec_from_ticks(poll_start_usec)
		frame_poll_sec = poll_sec
		_poll_total_sec += poll_sec
		_poll_max_sec = max(_poll_max_sec, poll_sec)
		_poll_calls += 1
		_display_total_sec += display_sec
		_display_max_sec = max(_display_max_sec, display_sec)
		_display_calls += 1

	if _accum_sec >= 1.0:
		var stream_count := _latched_stream_ids.size()
		var per_sec_message := "t=%ss fps=%.1f streams=%d process_max_ms=%.3f advance_max_ms=%.3f latch_max_ms=%.3f poll_max_ms=%.3f display_max_ms=%.3f log_max_ms=%.3f" % [
			String.num(_elapsed_sec, PRINT_DECIMALS),
			fps,
			stream_count,
			_process_max_sec * 1000.0,
			_advance_max_sec * 1000.0,
			_latch_max_sec * 1000.0,
			_poll_max_sec * 1000.0,
			_display_max_sec * 1000.0,
			_log_max_sec * 1000.0
		]
		var log_start_usec := Time.get_ticks_usec()
		_log(per_sec_message)
		var log_sec := _elapsed_sec_from_ticks(log_start_usec)
		frame_log_sec = log_sec
		_log_total_sec += log_sec
		_log_max_sec = max(_log_max_sec, log_sec)
		_log_calls += 1
		_accum_sec = 0.0

	if _elapsed_sec >= _duration_sec:
		_print_summary_and_quit()

	var process_sec := _elapsed_sec_from_ticks(process_start_usec)
	_record_frame_spike(frame_process_call, delta, process_sec, frame_latch_sec, frame_poll_sec, frame_display_sec, frame_log_sec)
	_process_total_sec += process_sec
	_process_max_sec = max(_process_max_sec, process_sec)
	_process_calls += 1


func _latch_stream_ids_from_snapshot() -> void:
	var snapshot: Variant = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return

	var streams: Array = snapshot.get("streams", [])
	if streams.is_empty():
		return

	var ids: Array[int] = []
	for entry: Variant in streams:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var stream_id := int(entry.get("stream_id", 0))
		if stream_id > 0:
			ids.append(stream_id)

	if not ids.is_empty():
		_latched_stream_ids = ids
		_log("latched stream_ids=%s" % str(_latched_stream_ids))


func _poll_stream_results() -> float:
	var display_sec := 0.0
	for i: int in range(_latched_stream_ids.size()):
		var stream_id := _latched_stream_ids[i]
		# This load-isolation harness latches stream ids from snapshots rather than
		# public CamBANGStream handles, so it uses the advanced server stream-id lookup.
		var stream_result: Variant = CamBANGServer.get_stream_result_by_stream_id(stream_id)
		if stream_result == null:
			continue

		if not _bind_display:
			continue

		var display_start_usec := Time.get_ticks_usec()
		if _display_path_trace_enabled and stream_result.has_method("get_display_view_path_kind"):
			_record_display_path_kind(int(stream_result.get_display_view_path_kind()))

		var slot_bound := bool(_display_bound_by_slot.get(i, false))
		if _bind_mode_latest or not slot_bound:
			var display_view: Variant = stream_result.get_display_view()
			var texture_valid := (display_view is Texture2D)
			if texture_valid:
				if i == 0:
					_stream_a.texture = display_view
				elif i == 1:
					_stream_b.texture = display_view
				_display_bound_by_slot[i] = true
				if bool(_display_bind_failed_logged_by_slot.get(i, false)):
					_display_bind_failed_logged_by_slot[i] = false
				if not slot_bound:
					_log("[CamBANG][Scene72] display_bind slot=%d stream_id=%d bind=%s result_valid=true texture_valid=true path=%d" % [
						i,
						stream_id,
						("latest" if _bind_mode_latest else "oneshot"),
						(int(stream_result.get_display_view_path_kind()) if stream_result.has_method("get_display_view_path_kind") else -1)
					])
			else:
				if not bool(_display_bind_failed_logged_by_slot.get(i, false)):
					_display_bind_failed_logged_by_slot[i] = true
					_log("[CamBANG][Scene72] display_bind slot=%d stream_id=%d bind=%s result_valid=true texture_valid=false path=%d" % [
						i,
						stream_id,
						("latest" if _bind_mode_latest else "oneshot"),
						(int(stream_result.get_display_view_path_kind()) if stream_result.has_method("get_display_view_path_kind") else -1)
					])
		display_sec += _elapsed_sec_from_ticks(display_start_usec)
	return display_sec


func _record_display_path_kind(path_kind: int) -> void:
	if path_kind == CamBANGStreamResult.DISPLAY_PATH_RETAINED_GPU_BACKING:
		_display_path_retained_gpu_backing_count += 1
		_display_path_current_gpu_streak += 1
		_display_path_current_cpu_streak = 0
		_display_path_longest_gpu_streak = max(_display_path_longest_gpu_streak, _display_path_current_gpu_streak)
	elif path_kind == CamBANGStreamResult.DISPLAY_PATH_STREAM_LIVE_CPU_DISPLAY_VIEW:
		_display_path_stream_live_cpu_display_view_count += 1
		_display_path_current_cpu_streak += 1
		_display_path_current_gpu_streak = 0
		_display_path_longest_cpu_streak = max(_display_path_longest_cpu_streak, _display_path_current_cpu_streak)
	else:
		_display_path_none_count += 1
		_display_path_current_gpu_streak = 0
		_display_path_current_cpu_streak = 0
	if _display_path_last_kind != -1 and _display_path_last_kind != path_kind:
		_display_path_transition_count += 1
	_display_path_last_kind = path_kind


func _print_summary_and_quit() -> void:
	_done = true
	var avg_fps := 0.0
	if _sample_count > 0:
		avg_fps = _accum_fps / float(_sample_count)
	var min_fps := 0.0 if _min_fps == INF else _min_fps
	_log("SUMMARY avg_fps=%.2f min_fps=%.2f worst_frame_ms=%.2f elapsed_sec=%.2f" % [
		avg_fps,
		min_fps,
		_worst_frame_sec * 1000.0,
		_elapsed_sec
	])
	if _display_path_trace_enabled and _bind_display:
		_log("SUMMARY_DISPLAY_PATH display_path_retained_gpu_backing_count=%d display_path_stream_live_cpu_display_view_count=%d display_path_none_count=%d display_path_transition_count=%d display_path_longest_gpu_streak=%d display_path_longest_cpu_streak=%d" % [
			_display_path_retained_gpu_backing_count,
			_display_path_stream_live_cpu_display_view_count,
			_display_path_none_count,
			_display_path_transition_count,
			_display_path_longest_gpu_streak,
			_display_path_longest_cpu_streak
		])
	_log("SUMMARY_TIMING process_max_ms=%.3f process_total_ms=%.3f process_calls=%d advance_max_ms=%.3f advance_total_ms=%.3f advance_calls=%d latch_max_ms=%.3f latch_total_ms=%.3f latch_calls=%d poll_max_ms=%.3f poll_total_ms=%.3f poll_calls=%d display_max_ms=%.3f display_total_ms=%.3f display_calls=%d log_max_ms=%.3f log_total_ms=%.3f log_calls=%d" % [
		_process_max_sec * 1000.0,
		_process_total_sec * 1000.0,
		_process_calls,
		_advance_max_sec * 1000.0,
		_advance_total_sec * 1000.0,
		_advance_calls,
		_latch_max_sec * 1000.0,
		_latch_total_sec * 1000.0,
		_latch_calls,
		_poll_max_sec * 1000.0,
		_poll_total_sec * 1000.0,
		_poll_calls,
		_display_max_sec * 1000.0,
		_display_total_sec * 1000.0,
		_display_calls,
		_log_max_sec * 1000.0,
		_log_total_sec * 1000.0,
		_log_calls
	])
	_print_frame_spike_trace()
	
	_stream_a.texture = null
	_stream_b.texture = null

	print("INFO: before CamBANGServer.stop_and_quit")
	CamBANGServer.stop_and_quit()
	print("INFO: after CamBANGServer.stop_and_quit")


func _record_frame_spike(process_call_index: int, delta_sec: float, process_sec: float, latch_sec: float, poll_sec: float, display_sec: float, log_sec: float) -> void:
	if not _frame_spike_trace_enabled:
		return
	var phase := "steady_run"
	if _latched_stream_ids.is_empty():
		phase = "before_start_or_latch"
	elif _elapsed_sec >= _duration_sec:
		phase = "teardown_or_summary"
	var entry := {
		"process_call_index": process_call_index,
		"elapsed_sec": _elapsed_sec,
		"delta_ms": delta_sec * 1000.0,
		"phase": phase,
		"process_ms": process_sec * 1000.0,
		"latch_ms": latch_sec * 1000.0,
		"poll_ms": poll_sec * 1000.0,
		"display_ms": display_sec * 1000.0,
		"log_ms": log_sec * 1000.0
	}
	_frame_spikes.append(entry)
	_frame_spikes.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("delta_ms", 0.0)) > float(b.get("delta_ms", 0.0))
	)
	while _frame_spikes.size() > _frame_spike_top_n:
		_frame_spikes.pop_back()


func _print_frame_spike_trace() -> void:
	if not _frame_spike_trace_enabled:
		return
	_log("FRAME_SPIKE_TRACE enabled=1 top_n=%d captured=%d" % [_frame_spike_top_n, _frame_spikes.size()])
	for i: int in range(_frame_spikes.size()):
		var s: Dictionary = _frame_spikes[i]
		_log("FRAME_SPIKE rank=%d process_call=%d elapsed_sec=%.3f delta_ms=%.3f phase=%s process_ms=%.3f latch_ms=%.3f poll_ms=%.3f display_ms=%.3f log_ms=%.3f" % [
			i + 1,
			int(s.get("process_call_index", 0)),
			float(s.get("elapsed_sec", 0.0)),
			float(s.get("delta_ms", 0.0)),
			str(s.get("phase", "")),
			float(s.get("process_ms", 0.0)),
			float(s.get("latch_ms", 0.0)),
			float(s.get("poll_ms", 0.0)),
			float(s.get("display_ms", 0.0)),
			float(s.get("log_ms", 0.0))
		])


func _repo_external_scenario_path(filename: String) -> String:
	#var repo_root := ProjectSettings.globalize_path("res://../..")
	#var repo_root := ProjectSettings.globalize_path("res://scenarios/")
	#return repo_root.path_join("external_scenarios").path_join(filename)
	return "res://scenarios/external_scenarios/%s" % filename

func _env_bool(env_key: String, default_value: bool) -> bool:
	var raw := OS.get_environment(env_key).strip_edges().to_lower()
	if raw == "":
		return default_value
	if raw == "1" or raw == "true" or raw == "yes" or raw == "on":
		return true
	if raw == "0" or raw == "false" or raw == "no" or raw == "off":
		return false
	return default_value


func _env_float(env_key: String, default_value: float) -> float:
	var raw := OS.get_environment(env_key).strip_edges()
	if raw == "":
		return default_value
	if not raw.is_valid_float():
		return default_value
	return max(0.1, raw.to_float())


func _elapsed_sec_from_ticks(start_usec: int) -> float:
	return float(Time.get_ticks_usec() - start_usec) / 1000000.0


func _log(message: String) -> void:
	print(message)
	if is_instance_valid(_status):
		_status.append_text("%s\n" % message)


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	push_error(message)
	_log("FAIL: %s" % message)
	_stream_a.texture = null
	_stream_b.texture = null
	_done = true
	CamBANGServer.stop_and_quit(1)
