extends Node

## Registers CamBANG wrappers from INSIDE CamBANG signal handlers, which is the
## one thing the boundary's tracked-wrapper bookkeeping was not safe against.
##
## THE HAZARD. CamBANGServer keeps tracked wrapper object ids in unordered
## containers and walks them once per publish tick, emitting a Godot signal from
## inside the loop body -- live_changed, result_live_changed, and the per-wrapper
## capture_finished. emit_signal() runs GDScript synchronously, so a handler that
## obtains any new wrapper inserts into the very container being walked. An
## unordered insert may rehash, a rehash invalidates every iterator, and the
## loop's next increment is then undefined behaviour. Walking a snapshot of the
## ids instead is what makes it safe.
##
## WHAT THIS SCENE DOES NOT PROVE -- READ BEFORE TRUSTING IT.
##
## It does NOT discriminate between the fixed and unfixed boundary. Measured on
## 2026-09-03, MSVC/Windows: reverting the snapshot fix and rebuilding, this
## scene still passed three runs out of three, and it passes on the fixed build
## too. So a PASS here is not evidence that the snapshot fix is present.
##
## The reason is implementation, not luck. MSVC's unordered containers are
## node-based: a rehash rebuilds the bucket array and relinks the intrusive
## list, but does not free the nodes, and an iterator is a node pointer. So the
## increment after an invalidating rehash still walks a valid list. The
## behaviour is undefined by the standard, and the fix is warranted on that
## ground alone, but the symptom this toolchain actually produces is a
## TRAVERSAL ANOMALY -- one tracked wrapper visited twice or skipped in that
## single pass, self-correcting on the next one -- not a fault. Anyone arriving
## here expecting a crash canary should stop expecting one.
##
## What it is worth keeping for: it is the only scene in the suite that
## registers a wrapper from inside a CamBANG signal handler at all, so it locks
## the reentrancy PATTERN in place. A future change that makes the pattern
## genuinely fault -- a different container, or an erase performed through an
## invalidated iterator rather than a bare increment -- would be caught here
## rather than in the field. Treat it as a pattern guard, not as this defect's
## regression test.
##
## WHY NO EXISTING SCENE COVERED THE PATTERN. Nothing else registers a wrapper
## from inside a CamBANG signal handler. 870 soaks volume but creates its streams
## once in setup and connects only the server-wide capture_finished, which is
## emitted before the wrapper loop rather than inside it. Volume was never the
## missing ingredient; reentrancy was.
##
## WHAT IS ASSERTED, POSITIVELY. Two things, and a burst that never fired is a
## FAIL rather than a quiet pass -- a scene that exercises nothing must not
## report ok:
##   - both reentrant bursts actually ran, from inside their respective signals;
##   - after both, the wrapper live-state flags are still correct
##     (CamBANGDevice.is_live, CamBANGStream.is_result_live). Those flags are
##     written ONLY by the two loops this scene abuses and are derived from the
##     published snapshot's LIVE-phase rows, so finding them still correct is
##     direct evidence that both walks are still running and still emitting.
##
## VECTORS. Two, both reachable through the public API with no abuse beyond the
## reentrancy itself:
##   - device container: get_device(id) mints and registers a canonical wrapper
##     for any non-zero id, so a burst of distinct ids forces repeated rehashes;
##   - stream container: create_stream() registers a wrapper per call and mints a
##     fresh id every time.
## The stream vector needs several created streams on one device. Core permits
## that (lifecycle_model.md 13 bounds ACTIVE streams, not stream records) and
## SyntheticProvider permits it; both platform providers currently refuse a
## second create_stream() per device with ERR_BUSY, which is a provider-local
## restriction. So under platform backing that sub-phase reports itself skipped
## rather than failing, and the device vector -- which is provider-independent --
## still runs.

const SCENE_LABEL := "69_signal_reentrancy_abuse"
const TOTAL_TIMEOUT_MS := 90000

# Sized to force several rehashes, not to soak. An unordered container grows
# when load factor passes 1.0, so a burst well past the tracked-wrapper count at
# rest is what makes the invalidation deterministic rather than occasional.
const DEVICE_BURST := 64
const STREAM_BURST := 32
# Instance ids that name nothing. get_device() consults no snapshot at all -- it
# mints a canonical wrapper per id by design -- so these register wrappers
# without touching hardware. The wrappers are not pretending to be anything:
# is_live() on each correctly reports false, because no device row in the
# published snapshot carries LIVE phase for these ids.
const FAKE_DEVICE_ID_BASE := 0x7000_0000
# Frames to keep running after a burst before believing the walk survived it.
const SETTLE_FRAMES := 30
# Liveness after a burst is asserted from the live-state flags THEMSELVES rather
# than from a publish count. Publication is change-driven, so an idle runtime
# stops publishing and a count-based probe hangs instead of testing anything.
# is_live() and is_result_live() are maintained only by the two loops this scene
# abuses, so finding them still correct after a burst is direct evidence that
# those loops are still walking and still emitting -- which is the property under
# test, and a stronger one than "a signal arrived".

var _done := false
var _terminal_verdict_emitted := false
var _quit_requested := false
var _started_ms := 0
var _provider_arg := "synthetic"
var _phase := "start"

var _hardware_id := ""
var _device = null
var _stream = null
var _burst_streams: Array = []
var _held_device_wrappers: Array = []

var _device_burst_fired := false
var _stream_burst_fired := false
var _stream_burst_skipped := false
var _in_burst := false

var _publishes := 0
var _publishes_at_last_burst := -1
var _settle_frames := 0


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
	print("RUN: %s provider=%s" % [SCENE_LABEL, _provider_arg])

	CamBANGServer.state_published.connect(_on_state_published)

	var err := 0
	if _provider_arg == "synthetic":
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC))
	else:
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
	if err != OK:
		_error("runtime start failed (%d)" % err, "runtime_start_failed")
		return
	print("STEP OK: runtime started (provider=%s)" % _provider_arg)


func _on_state_published(_gen: int, _version: int, _topology_version: int) -> void:
	_publishes += 1


func _process(_delta: float) -> void:
	if _done:
		return
	if Time.get_ticks_msec() - _started_ms > TOTAL_TIMEOUT_MS:
		_error("timed out in phase %s" % _phase, "timeout")
		return
	if _phase == "discover":
		_phase_discover()
	elif _phase == "await_device_burst":
		_phase_await_device_burst()
	elif _phase == "settle_device_burst":
		_settle_frames += 1
		if _settle_frames >= SETTLE_FRAMES:
			_phase_begin_stream()
	elif _phase == "await_stream_burst":
		_phase_await_stream_burst()
	elif _phase == "settle_stream_burst":
		_settle_frames += 1
		if _settle_frames >= SETTLE_FRAMES:
			_phase_conclude()
	elif _phase == "start":
		_phase = "discover"


func _phase_discover() -> void:
	var eps = CamBANGServer.enumerate_devices()
	if typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty():
		return
	var endpoint0: Dictionary = (eps as Array)[0]
	_hardware_id = str(endpoint0.get("hardware_id", ""))
	if _hardware_id.is_empty():
		_fail("enumerate_devices() endpoint hardware_id must be non-empty", "no_hardware_id")
		return
	_device = CamBANGServer.get_device_for_hardware_id(_hardware_id)
	if _device == null:
		_fail("get_device_for_hardware_id() returned null for an enumerated endpoint", "no_device_handle")
		return

	# Connect BEFORE engage: the false -> true live flip is emitted from inside
	# the device loop, which is exactly where the burst has to land.
	_device.connect("live_changed", Callable(self, "_on_device_live_changed"))
	var err := int(_device.engage())
	if err != OK and err != ERR_BUSY:
		_fail("engage() failed (%d)" % err, "engage_failed")
		return
	_phase = "await_device_burst"


func _phase_await_device_burst() -> void:
	# engage() may need retrying across frames; the handler fires when it takes.
	if _device_burst_fired:
		_publishes_at_last_burst = _publishes
		_settle_frames = 0
		_phase = "settle_device_burst"
		return
	var err := int(_device.engage())
	if err != OK and err != ERR_BUSY:
		_fail("engage() failed on retry (%d)" % err, "engage_failed")


func _on_device_live_changed(live: bool) -> void:
	# Reentrant: this runs inside CamBANGServer's walk of the tracked DEVICE
	# wrapper container, and every get_device() below inserts into it.
	if _in_burst or _device_burst_fired or not live:
		return
	_in_burst = true
	for i in range(DEVICE_BURST):
		var wrapper = CamBANGServer.get_device(FAKE_DEVICE_ID_BASE + i)
		if wrapper == null:
			_in_burst = false
			_fail("get_device() returned null during reentrant burst at i=%d" % i, "device_burst_null")
			return
		_held_device_wrappers.append(wrapper)
	_device_burst_fired = true
	_in_burst = false
	print("STEP OK: device wrapper burst of %d registered from inside live_changed" % DEVICE_BURST)


func _phase_begin_stream() -> void:
	_stream = _device.create_stream({"intent": CamBANGStream.INTENT_PREVIEW})
	if _stream == null:
		_fail("create_stream() returned null for the engaged device", "create_stream_null")
		return
	_stream.connect("result_live_changed", Callable(self, "_on_result_live_changed"))
	var err := int(_stream.start())
	if err != OK:
		_fail("stream.start() failed (%d)" % err, "stream_start_failed")
		return
	_phase = "await_stream_burst"


func _phase_await_stream_burst() -> void:
	if _stream_burst_fired or _stream_burst_skipped:
		_publishes_at_last_burst = _publishes
		_settle_frames = 0
		_phase = "settle_stream_burst"


func _on_result_live_changed(live: bool) -> void:
	# Reentrant: this runs inside CamBANGServer's walk of the tracked STREAM
	# wrapper container, and every create_stream() below inserts into it.
	if _in_burst or _stream_burst_fired or _stream_burst_skipped or not live:
		return
	_in_burst = true
	for i in range(STREAM_BURST):
		var extra = _device.create_stream({"intent": CamBANGStream.INTENT_PREVIEW})
		if extra == null:
			# Provider refuses a second created stream per device. Provider-local,
			# not a boundary defect; the device vector already exercised the same
			# hazard on the other container.
			_stream_burst_skipped = true
			_in_burst = false
			print("SKIP: provider refused a second created stream at i=%d; stream vector not exercised" % i)
			return
		_burst_streams.append(extra)
	_stream_burst_fired = true
	_in_burst = false
	print("STEP OK: stream wrapper burst of %d created from inside result_live_changed" % STREAM_BURST)


func _phase_conclude() -> void:
	if not _device_burst_fired:
		_fail("device live_changed never fired; no reentrancy was exercised", "device_burst_never_fired")
		return
	if not _stream_burst_fired and not _stream_burst_skipped:
		_fail("stream result_live_changed never fired; no reentrancy was exercised", "stream_burst_never_fired")
		return
	# The loops under test are the only writers of these flags. Still correct
	# after both bursts means both walks survived and kept working.
	if not bool(_device.is_live()):
		_fail("device wrapper live state went stale after the reentrant bursts", "device_live_state_stale")
		return
	if not _stream_burst_skipped and not bool(_stream.is_result_live()):
		_fail("stream wrapper result-live state went stale after the reentrant bursts", "stream_live_state_stale")
		return
	var vectors := "device+stream"
	if _stream_burst_skipped:
		vectors = "device_only"
	print("STEP OK: wrapper live states still maintained after both bursts (publishes=%d)" % _publishes)
	_pass("pass_vectors_%s" % vectors)


func _pass(reason: String) -> void:
	if _done:
		return
	_done = true
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
	for extra in _burst_streams:
		if extra != null:
			extra.destroy()
	_burst_streams.clear()
	_held_device_wrappers.clear()
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	CamBANGServer.stop()
	get_tree().quit(code)
