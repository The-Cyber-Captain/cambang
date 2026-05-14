extends Node

const QUIT_FLUSH_FRAMES := 2
const TIMEOUT_MS := 7000
const FIRST_PUBLISH_TIMEOUT_MS := 2000
const OBSERVATION_WINDOW_MS := 1200

var _done := false
var _quit_requested := false

var _timer: Timer
var _first_publish_timer: Timer
var _observation_timer: Timer

var _signal_count_this_tick := 0
var _publish_count := 0
var _tick_count := 0

var _first_publish_seen := false
var _observation_started := false

var _last_gen := -1
var _last_version := -1
var _last_topology_version := -1

var _saw_nonbaseline_publish := false
var _saw_realized_device := false
var _saw_realized_stream := false

var _last_debug_summary := ""


func _ready() -> void:
	set_process(true)

	CamBANGServer.stop()

	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
		# CamBANGServer.TIMELINE_RECONCILIATION_STRICT
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

	print("RUN: godot tick-bounded coalescing abuse")

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

	_tick_count += 1

	# Any signals received since the previous _process belong to the same Godot tick bucket.
	if _signal_count_this_tick > 1:
		_fail_with_context("FAIL: more than one state_published emission observed in one Godot tick")
		return

	_signal_count_this_tick = 0


func _on_timeout() -> void:
	_fail_with_context("FAIL: tick-bounded coalescing abuse timed out before reaching deterministic completion")


func _on_first_publish_timeout() -> void:
	if _done:
		return
	if _publish_count > 0:
		return
	_fail_with_context("FAIL: no state_published callback observed during startup window")


func _on_observation_timeout() -> void:
	if _done:
		return

	print("INFO: observation-timeout fired (tick-bounded coalescing verifier)")

	if not _first_publish_seen:
		_fail_with_context("FAIL: observation window ended without first publish")
		return

	if not _saw_nonbaseline_publish:
		_fail_with_context("FAIL: no non-baseline publish observed during observation window")
		return

	if not _saw_realized_device:
		_fail_with_context("FAIL: no realized device observed during observation window")
		return

	if not _saw_realized_stream:
		_fail_with_context("FAIL: no realized stream observed during observation window")
		return

	_ok("OK: godot tick-bounded coalescing abuse PASS")


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	if _done:
		return

	_signal_count_this_tick += 1
	_publish_count += 1

	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		_fail_with_context("FAIL: NIL snapshot received in state_published handler")
		return

	var snap_gen := int(snapshot.get("gen", -1))
	var snap_version := int(snapshot.get("version", -1))
	var snap_topology_version := int(snapshot.get("topology_version", -1))

	var devices: Array = snapshot.get("devices", [])
	var streams: Array = snapshot.get("streams", [])
	var native_objects: Array = snapshot.get("native_objects", [])
	var detached_root_ids: Array = snapshot.get("detached_root_ids", [])

	_last_debug_summary = (
		"tick=%d publish_count=%d signal=(g=%d v=%d tv=%d) "
		+ "snapshot=(g=%d v=%d tv=%d) sizes=(devices=%d streams=%d native=%d detached=%d) "
		+ "prev=(g=%d v=%d tv=%d) flags=(first=%s nonbaseline=%s dev=%s stream=%s)"
	) % [
		_tick_count,
		_publish_count,
		gen,
		version,
		topology_version,
		snap_gen,
		snap_version,
		snap_topology_version,
		devices.size(),
		streams.size(),
		native_objects.size(),
		detached_root_ids.size(),
		_last_gen,
		_last_version,
		_last_topology_version,
		str(_first_publish_seen),
		str(_saw_nonbaseline_publish),
		str(_saw_realized_device),
		str(_saw_realized_stream),
	]

	if snap_gen != gen:
		_fail_with_context("FAIL: signal/snapshot gen mismatch")
		return
	if snap_version != version:
		_fail_with_context("FAIL: signal/snapshot version mismatch")
		return
	if snap_topology_version != topology_version:
		_fail_with_context("FAIL: signal/snapshot topology_version mismatch")
		return

	if not _first_publish_seen:
		_first_publish_seen = true
		print("INFO: first publish observed")
		_start_observation_window()

		if version != 0:
			_fail_with_context("FAIL: first observed publish was not baseline version=0")
			return
		if topology_version != 0:
			_fail_with_context("FAIL: first observed publish was not baseline topology_version=0")
			return

		_last_gen = gen
		_last_version = version
		_last_topology_version = topology_version
	else:
		if gen != _last_gen:
			_fail_with_context("FAIL: generation changed unexpectedly during coalescing abuse")
			return

		if version != _last_version + 1:
			_fail_with_context("FAIL: version is not contiguous within generation")
			return

		if topology_version < _last_topology_version:
			_fail_with_context("FAIL: topology_version regressed")
			return

		if topology_version > _last_topology_version and version <= _last_version:
			_fail_with_context("FAIL: topology_version changed without version advance")
			return

		_last_version = version
		_last_topology_version = topology_version

	if version > 0:
		_saw_nonbaseline_publish = true
	if devices.size() > 0:
		_saw_realized_device = true
	if streams.size() > 0:
		_saw_realized_stream = true


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


func _fail_with_context(msg: String) -> void:
	if _last_debug_summary != "":
		print("INFO: %s" % _last_debug_summary)
	_fail(msg)


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
