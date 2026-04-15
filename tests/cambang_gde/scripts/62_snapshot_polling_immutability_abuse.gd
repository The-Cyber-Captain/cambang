extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 7000
const FIRST_PUBLISH_TIMEOUT_MS := 2000
const OBSERVATION_WINDOW_MS := 1800

var _done := false
var _quit_requested := false
var _timer: Timer
var _first_publish_timer: Timer
var _observation_timer: Timer
var _cached_snapshot: Dictionary
var _cached_version := -1
var _cached_stream_count := -1
var _observation_started := false
var _first_publish_observed := false
var _later_distinct_publish_observed := false
var _latest_signal_version := -1
var _signal_poll_aligned := false


func _ready() -> void:
	set_process(true)
	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	if start_err != OK:
		_fail("FAIL: synthetic timeline start rejected with error %d" % start_err)
		return
	var stage_err := CamBANGServer.select_builtin_scenario("publication_coalescing")
	if stage_err != OK:
		_fail("FAIL: unable to stage publication_coalescing scenario")
		return
	var scenario_start_err := CamBANGServer.start_scenario()
	if scenario_start_err != OK:
		_fail("FAIL: unable to start publication_coalescing scenario")
		return
	print("RUN: godot snapshot polling/immutability abuse")

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = float(TIMEOUT_MS) / 1000.0
	add_child(_timer)
	_timer.timeout.connect(_on_timeout)
	_timer.start()

	_first_publish_timer = Timer.new()
	_first_publish_timer.one_shot = true
	_first_publish_timer.wait_time = float(FIRST_PUBLISH_TIMEOUT_MS) / 1000.0
	add_child(_first_publish_timer)
	_first_publish_timer.timeout.connect(_on_first_publish_timeout)
	_first_publish_timer.start()

	_observation_timer = Timer.new()
	_observation_timer.one_shot = true
	_observation_timer.wait_time = float(OBSERVATION_WINDOW_MS) / 1000.0
	add_child(_observation_timer)
	_observation_timer.timeout.connect(_on_observation_timeout)

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)


func _process(_delta: float) -> void:
	if _done:
		return
	# Frame driver: keep headless main loop actively ticking until verifier completion.
	var polled = CamBANGServer.get_state_snapshot()
	if polled == null:
		return

	if _cached_version >= 0:
		if int(_cached_snapshot.get("version", -1)) != _cached_version:
			_fail("FAIL: cached snapshot mutated after newer publishes")
			return
		var cached_streams: Array = _cached_snapshot.get("streams", [])
		if cached_streams.size() != _cached_stream_count:
			_fail("FAIL: cached snapshot stream array mutated")
			return

	if _latest_signal_version >= 0:
		var polled_version := int(polled.get("version", -1))
		if polled_version == _latest_signal_version:
			_signal_poll_aligned = true


func _on_timeout() -> void:
	_fail("FAIL: snapshot polling/immutability abuse timed out before reaching deterministic completion")


func _on_first_publish_timeout() -> void:
	if _done:
		return
	if _first_publish_observed:
		return
	_fail("FAIL: no state_published callback observed during startup window")


func _on_observation_timeout() -> void:
	if _done:
		return
	print("INFO: observation-timeout fired (snapshot polling/immutability verifier)")
	if not _first_publish_observed:
		_fail("FAIL: first publish was never observed")
		return
	if not _later_distinct_publish_observed:
		_fail("FAIL: no later distinct publish observed in polling/immutability window")
		return
	if not _signal_poll_aligned:
		_fail("FAIL: polling view never aligned with signal-observed publish version")
		return
	_ok("OK: godot snapshot polling/immutability abuse PASS")


func _on_state_published(_gen: int, version: int, _topology_version: int) -> void:
	if _done:
		return

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail("FAIL: NIL snapshot inside state_published during polling abuse")
		return

	var snapshot_version := int(snapshot.get("version", -1))
	if snapshot_version != version:
		_fail("FAIL: state_published version does not match immediate snapshot version")
		return
	_latest_signal_version = version

	if _cached_version == -1:
		print("INFO: first publish observed")
		_start_observation_window()
		_cached_snapshot = snapshot
		_cached_version = snapshot_version
		var cached_streams: Array = snapshot.get("streams", [])
		_cached_stream_count = cached_streams.size()
		_first_publish_observed = true
		return

	if snapshot_version != _cached_version:
		_later_distinct_publish_observed = true


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
	print("INFO: quit requested code=%d" % code)
	get_tree().quit(code)
