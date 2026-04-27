extends Control

const DEFAULT_SCENARIO_FILE := "stream_load_2x_720p30.json"
const DEFAULT_DURATION_SEC := 20.0
const DEFAULT_POLL_RESULTS := true
const DEFAULT_BIND_DISPLAY := true
const PRINT_DECIMALS := 2

@onready var _status: RichTextLabel = $RootMargin/MainColumn/Status
@onready var _stream_a: TextureRect = $RootMargin/MainColumn/Streams/StreamA
@onready var _stream_b: TextureRect = $RootMargin/MainColumn/Streams/StreamB

var _duration_sec := DEFAULT_DURATION_SEC
var _poll_results := DEFAULT_POLL_RESULTS
var _bind_display := DEFAULT_BIND_DISPLAY

var _elapsed_sec := 0.0
var _accum_sec := 0.0
var _accum_fps := 0.0
var _sample_count := 0
var _min_fps := INF
var _worst_frame_sec := 0.0
var _done := false

var _latched_stream_ids: Array[int] = []


func _ready() -> void:
	_status.clear()
	_config_from_env()
	_bootstrap()
	set_process(true)


func _config_from_env() -> void:
	_duration_sec = _env_float("CAMBANG_STREAM_LOAD_DURATION_SEC", DEFAULT_DURATION_SEC)
	_poll_results = _env_bool("CAMBANG_STREAM_LOAD_POLL_RESULTS", DEFAULT_POLL_RESULTS)
	_bind_display = _env_bool("CAMBANG_STREAM_LOAD_BIND_DISPLAY", DEFAULT_BIND_DISPLAY)


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
	_log("scenario=%s duration_sec=%.1f poll_results=%s bind_display=%s" % [
		scenario_file,
		_duration_sec,
		str(_poll_results),
		str(_bind_display)
	])


func _process(delta: float) -> void:
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

	if _latched_stream_ids.is_empty():
		_latch_stream_ids_from_snapshot()

	if _poll_results:
		_poll_stream_results()

	if _accum_sec >= 1.0:
		var stream_count := _latched_stream_ids.size()
		_log("t=%ss fps=%.1f streams=%d" % [
			String.num(_elapsed_sec, PRINT_DECIMALS),
			fps,
			stream_count
		])
		_accum_sec = 0.0

	if _elapsed_sec >= _duration_sec:
		_print_summary_and_quit()


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


func _poll_stream_results() -> void:
	for i: int in range(_latched_stream_ids.size()):
		var stream_id := _latched_stream_ids[i]
		var stream_result: Variant = CamBANGServer.get_latest_stream_result(stream_id)
		if stream_result == null:
			continue

		if not _bind_display:
			continue

		var display_view: Variant = stream_result.get_display_view()
		if display_view is Texture2D:
			if i == 0:
				_stream_a.texture = display_view
			elif i == 1:
				_stream_b.texture = display_view


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
	CamBANGServer.stop()
	get_tree().quit(0)


func _repo_external_scenario_path(filename: String) -> String:
	var repo_root := ProjectSettings.globalize_path("res://../..")
	return repo_root.path_join("external_scenarios").path_join(filename)


func _env_bool(name: String, default_value: bool) -> bool:
	var raw := OS.get_environment(name).strip_edges().to_lower()
	if raw == "":
		return default_value
	if raw == "1" or raw == "true" or raw == "yes" or raw == "on":
		return true
	if raw == "0" or raw == "false" or raw == "no" or raw == "off":
		return false
	return default_value


func _env_float(name: String, default_value: float) -> float:
	var raw := OS.get_environment(name).strip_edges()
	if raw == "":
		return default_value
	if not raw.is_valid_float():
		return default_value
	return max(0.1, raw.to_float())


func _log(message: String) -> void:
	print(message)
	if is_instance_valid(_status):
		_status.append_text("%s\n" % message)


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	push_error(message)
	_log("FAIL: %s" % message)
	CamBANGServer.stop()
	_done = true
	get_tree().quit(1)
