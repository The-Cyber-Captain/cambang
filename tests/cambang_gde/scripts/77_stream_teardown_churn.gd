extends Node

## Scene 77: stream teardown churn -- create, start, deliver, tear down, repeat.
##
## NAMED FOR WHAT IT COVERS. An earlier name for this scene carried the word
## "race", which asserted exactly the coverage the paragraph below disclaims. A
## scene called after a defect it cannot detect will eventually be cited as
## covering it. It is named for the sequences it drives instead.
##
## READ THIS FIRST. This scene was written to reproduce a reported Camera2
## abort, and IT DOES NOT REPRODUCE IT. It was run against provider binaries
## with and without the ownership change in "Stop a Camera2 metadata callback
## using a production it holds no share of", on a Galaxy S20+ and a Quest 3,
## platform-backed. Every run completed all 40 cycles with no abort, INCLUDING
## the runs on the unchanged binary. The scene therefore does NOT discriminate a
## provider carrying that defect from one that does not, and a pass here is not
## evidence that any teardown race is fixed or absent. Do not cite it as such.
##
## WHY IT DOES NOT, as far as the evidence goes. The abort needs a provider
## callback in flight at the instant teardown releases the StreamProduction it
## reads. CoreRuntime's shutdown ordering appears to prevent that from any
## sequence a caller can drive: stop_stream, then destroy_stream, then provider
## shutdown, so ACameraCaptureSession_stopRepeating is always issued and
## returned through Core before a production is released, and the device looper
## drains in the gap. The reported abort came from a path this scene does not
## reach, and that path has NOT been identified -- two hypotheses about it were
## checked and both were wrong. The log that would have named it was not
## retained.
##
## WHAT IT IS WORTH KEEPING FOR. It is a stream lifecycle churn exerciser, and
## the only one in the suite that tears a stream down WHILE IT IS DELIVERING
## rather than after it settles. It repeatedly drives:
##   - create_stream / start / deliver / stop / destroy, with the teardown on
##     the FIRST delivered frame and the next create on the following frame;
##   - runtime stop with a stream still started and still delivering;
##   - geometry change every second cycle, so half the cycles are a plain
##     release and half force a session rebuild, where the production is
##     REPLACED rather than released.
## Gross breakage in any of that -- a refused create, a failed start, a stream
## that starts and never delivers, a teardown that returns an error -- fails the
## scene with a named reason. That is real coverage. It is just not coverage of
## the race the scene was named after.
##
## HOW A NATIVE ABORT WOULD SURFACE, if one ever did. It kills the process;
## there is no assertion that can run afterwards. The evidence would be the
## ABSENCE of the terminal verdict line, which the harness classifies as
## error/timeout. That remains true, and is why the scene is still worth running
## on device even though it has never caught anything.
##
## WHAT IS NOT ASSERTED. Frame rate, image content, or timing. Delivery is used
## only as proof that the stream is genuinely producing at the moment it is torn
## down; a stream torn down before it ever delivered would exercise nothing, and
## is a failed cycle rather than a passed one.

const SCENE_LABEL := "77_stream_teardown_churn"
# Enough attempts at a small window to be worth running, bounded so the scene
# stays inside a normal harness timeout on slow hardware. At roughly a tenth of
# a second per cycle this is seconds of work, not minutes.
const CYCLES := 40
# A cycle must see delivery before it tears down, or it proves nothing. This is
# the budget for that; exceeding it means the stream started and never produced,
# which is a real failure and not a race.
const CYCLE_DELIVERY_TIMEOUT_MS := 4000
const TOTAL_TIMEOUT_MS := 180000

var _provider_arg := "synthetic"
var _done := false
var _terminal_verdict_emitted := false
var _quit_requested := false
var _started_ms := 0
var _phase := "start"
var _device = null
var _stream = null
var _hardware_id := ""
var _profiles: Array = []
var _cycle := 0
var _cycle_started_ms := 0
var _cycles_completed := 0
var _same_geometry_cycles := 0
var _changed_geometry_cycles := 0
var _prev_profile_index := -1
var _stream_teardowns := 0
var _runtime_teardowns := 0
var _needs_restart := false


func _ready() -> void:
	_started_ms = Time.get_ticks_msec()
	var setting_provider := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting_provider != "":
		_provider_arg = setting_provider
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	for arg in args:
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
	print("RUN: %s provider=%s cycles=%d" % [SCENE_LABEL, _provider_arg, CYCLES])

	var err := 0
	if _provider_arg == "synthetic":
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC))
	else:
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
	if err != OK:
		_error("runtime start failed (%d)" % err, "runtime_start_failed")
		return
	print("STEP OK: runtime started (provider=%s)" % _provider_arg)


func _process(_delta: float) -> void:
	if _done:
		return
	if Time.get_ticks_msec() - _started_ms > TOTAL_TIMEOUT_MS:
		_error("timed out in phase %s at cycle %d/%d" % [_phase, _cycle, CYCLES], "timeout")
		return
	match _phase:
		"start":
			_phase = "discover"
		"discover":
			_phase_discover()
		"engage":
			_phase_engage()
		"open":
			_phase_open()
		"await_delivery":
			_phase_await_delivery()


func _phase_discover() -> void:
	var eps = CamBANGServer.enumerate_devices()
	if typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty():
		return
	_hardware_id = str(((eps as Array)[0] as Dictionary).get("hardware_id", ""))
	if _hardware_id.is_empty():
		_fail("endpoint hardware_id must be non-empty", "no_hardware_id")
		return
	_device = CamBANGServer.get_device_for_hardware_id(_hardware_id)
	if _device == null:
		_fail("get_device_for_hardware_id() returned null", "no_device_handle")
		return
	_phase = "engage"


func _phase_engage() -> void:
	var err := int(_device.engage())
	if err == ERR_BUSY:
		return
	if err != OK:
		_fail("engage() failed (%d)" % err, "engage_failed")
		return

	# Geometry comes from the catalog, never invented: an unadvertised size is
	# refused by the provider and would fail this scene for a reason that has
	# nothing to do with teardown.
	var caps: Dictionary = CamBANGServer.get_supported_stream_profiles(_hardware_id)
	if not caps.has("profiles"):
		_expected_unsupported("provider cannot enumerate stream profiles for %s" % _hardware_id,
			"catalog_not_enumerable")
		return
	var profiles: Array = caps["profiles"]
	if profiles.is_empty():
		_expected_unsupported("endpoint advertises no stream profiles", "catalog_empty")
		return

	# Up to two DISTINCT geometries. The second is what turns half the cycles
	# into reconfigurations; a device advertising only one size still exercises
	# the release path, just not the replace path, and says so in its summary.
	var first: Dictionary = ((profiles[0] as Dictionary)["profile"] as Dictionary).duplicate()
	_profiles = [first]
	for entry in profiles:
		var p: Dictionary = ((entry as Dictionary)["profile"] as Dictionary).duplicate()
		if int(p.get("width", 0)) != int(first.get("width", 0)) \
				or int(p.get("height", 0)) != int(first.get("height", 0)):
			_profiles.append(p)
			break
	if _cycle == 0:
		print("STEP OK: engaged %s with %d distinct geometr%s" % [
			_hardware_id, _profiles.size(), "y" if _profiles.size() == 1 else "ies",
		])
	_phase = "open"


func _phase_open() -> void:
	# TWO cycles per geometry, not one. Alternating every cycle would make every
	# single teardown a reconfiguration and leave the same-geometry release path
	# -- the plain destroy, with no session rebuild -- completely unexercised.
	var index := int(_cycle / 2) % _profiles.size()
	var profile: Dictionary = _profiles[index]
	if _prev_profile_index == index:
		_same_geometry_cycles += 1
	else:
		_changed_geometry_cycles += 1
	_prev_profile_index = index
	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": profile,
	})
	if _stream == null:
		_fail("cycle %d: create_stream refused a catalog profile: %s"
			% [_cycle, JSON.stringify(profile)], "create_stream_null")
		return
	var err := int(_stream.start())
	if err != OK:
		_fail("cycle %d: stream.start() failed (%d) on a catalog profile: %s"
			% [_cycle, err, JSON.stringify(profile)], "start_failed")
		return
	_cycle_started_ms = Time.get_ticks_msec()
	_phase = "await_delivery"


func _phase_await_delivery() -> void:
	var waited := Time.get_ticks_msec() - _cycle_started_ms
	var result = _stream.get_result()
	if result == null:
		if waited > CYCLE_DELIVERY_TIMEOUT_MS:
			# Not a race: a stream that started and never produced is its own
			# defect, and tearing it down here would prove nothing anyway.
			_fail("cycle %d: no frame delivered within %d ms of start()" % [_cycle, waited],
				"cycle_no_delivery")
		return

	# Delivering. Tear down NOW, in this frame, with a callback plausibly in
	# flight -- that is the entire point of the scene.
	#
	# TWO teardown shapes, alternating. Neither reproduced the reported abort on
	# an unchanged provider binary; see the header. They are kept because they
	# are different lifecycle sequences worth exercising, not because either is
	# known to be dangerous.
	if (_cycle % 2) == 0:
		# Stream stop then destroy, which the contract requires in that order.
		# stop_stream issues ACameraCaptureSession_stopRepeating and returns
		# through Core, so destroy_stream's release happens a whole round trip
		# later, by which time the device looper has drained.
		var stop_err := int(_stream.stop())
		if stop_err != OK:
			_fail("cycle %d: stream.stop() failed (%d)" % [_cycle, stop_err], "stop_failed")
			return
		var destroy_err := int(_stream.destroy())
		if destroy_err != OK:
			_fail("cycle %d: stream.destroy() failed (%d)" % [_cycle, destroy_err], "destroy_failed")
			return
		_stream = null
		_stream_teardowns += 1
	else:
		# Runtime stop with the stream STILL STARTED and still delivering, so
		# the caller never stops or destroys it itself.
		#
		# This was added on the theory that Camera2CameraProvider::shutdown()
		# releases the production while the repeating request is still live --
		# it issues no stopRepeating of its own, and closes the device only
		# afterwards. That theory was WRONG about what reaches it: CoreRuntime
		# calls stop_stream and destroy_stream before provider shutdown, so the
		# stream is already stopped and gone by then. The sequence is still a
		# distinct lifecycle path worth driving; it is not the wide window it
		# was written to be.
		_stream = null
		CamBANGServer.stop()
		_runtime_teardowns += 1
		_needs_restart = true

	_cycles_completed += 1
	_cycle += 1
	if _cycle % 10 == 0:
		print("STEP OK: %d/%d churn cycles survived" % [_cycle, CYCLES])
	if _cycle >= CYCLES:
		_pass("churn_survived")
		return
	# Straight back round. After a wide teardown the runtime is down, so the
	# cycle restarts it and rediscovers; after a narrow one the device handle is
	# still good and the next create_stream lands on the very next frame, which
	# is the caller pattern the crash report described.
	if _needs_restart:
		_needs_restart = false
		_device = null
		var rerr := 0
		if _provider_arg == "synthetic":
			rerr = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC))
		else:
			rerr = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
		if rerr != OK:
			_fail("cycle %d: runtime restart after server stop failed (%d)" % [_cycle, rerr],
				"restart_failed")
			return
		_phase = "discover"
		return
	_phase = "open"


func _pass(reason: String) -> void:
	if _done:
		return
	_done = true
	print("STEP OK: %d cycles completed (%d same-geometry, %d reconfiguring; %d by stream stop+destroy, %d by runtime stop)" % [
		_cycles_completed, _same_geometry_cycles, _changed_geometry_cycles,
		_stream_teardowns, _runtime_teardowns,
	])
	if _changed_geometry_cycles == 0:
		print("NOTE: one advertised geometry only; the session-rebuild path was not exercised")
	print("NOTE: this scene does not discriminate a provider carrying the reported")
	print("NOTE: teardown defect from one that does not -- an unchanged binary passes")
	print("NOTE: it too. A pass is lifecycle coverage, not race evidence.")
	_emit_harness_verdict("ok", 0, reason)
	_cleanup_and_quit(0)


func _fail(message: String, reason: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("fail", 1, reason)
	push_error("FAIL: %s" % message)
	print("FAIL: %s" % message)
	_cleanup_and_quit(1)


func _error(message: String, reason: String) -> void:
	if _done:
		return
	_done = true
	_emit_harness_verdict("error", 1, reason)
	push_error(message)
	print(message)
	_cleanup_and_quit(1)


func _expected_unsupported(message: String, reason: String) -> void:
	if _done:
		return
	_done = true
	print("EXPECTED_UNSUPPORTED: %s" % message)
	_emit_harness_verdict("expected_unsupported", 0, reason)
	_cleanup_and_quit(0)


func _emit_harness_verdict(status: String, exit_code: int, reason: String) -> void:
	if _terminal_verdict_emitted:
		return
	_terminal_verdict_emitted = true
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, exit_code, reason,
	])


func _cleanup_and_quit(code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	CamBANGServer.stop()
	get_tree().quit(code)
