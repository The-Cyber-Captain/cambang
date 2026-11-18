extends Node

## Scene 81: to_image() from worker threads.
##
## The result contract allows CaptureResult.to_image(), CaptureResult.to_image_member()
## and StreamResult.to_image() on any thread, for every member and payload kind,
## behaving the same on every thread. This scene proves it on the configuration it
## runs on:
##
## 1. Identity: every capture result (and, under Synthetic, the stream result) is
##    converted by WORKERS concurrent WorkerThreadPool tasks; each image must be
##    byte-identical to a main-thread to_image() of the same result.
## 2. stop() underneath workers: workers keep converting while CamBANGServer.stop()
##    runs. A conversion that started after stop() returned must give the same
##    outcome (image bytes, or null) as a main-thread call after stop(). A call that
##    overlapped stop() may land on either side and is only counted. Before stop(), a
##    capture image must equal its reference; a stream image may be a later whole
##    frame (its live backing is rewritten in place by the producer).
## 3. Release on a worker: the last reference to each result is dropped on a pool
##    thread.
##
## A worker consumer must wait for its tasks before quitting: Godot 4.5.2 crashes at
## exit on unwaited GDScript-lambda pool tasks whatever they hold. This scene waits.
##
## Provider: --cambang-bench-provider=synthetic|platform (default synthetic), or the
## cambang/maintainer/bench_provider project setting on Android. Under Synthetic the
## producer form follows --cambang-synth-producer-output-form; a GPU form needs a
## RenderingDevice (mobile renderer, not headless/compatibility) and is
## expected_unsupported without one.
##
## Self-terminating: emits [CamBANG][HarnessVerdict].

const SCENE_LABEL := "81_to_image_worker_thread_verify"
const TIMEOUT_MS := 10000
const WORKERS := 4
const STOP_PHASE_MS := 3000

# Android rig pairs by model (stable Camera2 ids), as scene 80.
const ANDROID_RIG := {
	"Quest 3": {"cameras": ["1", "50", "51"], "combinations": [["1", "50", "51"]], "pair": ["50", "51"]},
	"SM-G986U1": {"cameras": ["0", "1", "2", "3", "4"], "combinations": [["0", "1"], ["0", "3"]], "pair": ["0", "1"]},
}
# The qualified stereo geometry on a Quest 3; elsewhere the first catalog entry.
const QUEST_STILL := {"width": 1280, "height": 1280}

var _provider_arg := "synthetic"
var _synth_form := "runtime_default"
var _verdict_emitted := false
var _finished := {}


func _ready() -> void:
	_parse_args()
	print("RUN: %s provider=%s synth_form=%s os=%s model=%s" % [
		SCENE_LABEL, _provider_arg, _synth_form, OS.get_name(), OS.get_model_name()])
	CamBANGServer.capture_finished.connect(_on_capture_finished)
	await _run()


func _parse_args() -> void:
	var setting := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting != "":
		_provider_arg = setting
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	for arg in args:
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
		elif arg.begins_with("--cambang-synth-producer-output-form="):
			_synth_form = arg.substr("--cambang-synth-producer-output-form=".length()).strip_edges().to_lower()


func _on_capture_finished(capture_id: String, _info: Dictionary) -> void:
	_finished[capture_id] = true


func _exit_tree() -> void:
	CamBANGServer.stop()


func _emit(status: String, reason: String) -> void:
	if _verdict_emitted:
		return
	_verdict_emitted = true
	var code := 0 if status == "ok" or status == "expected_unsupported" else 1
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, code, reason.replace(" ", "_")])
	CamBANGServer.stop()
	get_tree().quit(code)


func _fail(reason: String) -> void:
	_emit("fail", reason)


# ---- session -----------------------------------------------------------------

func _rig_plan() -> Dictionary:
	if _provider_arg != "synthetic" and OS.get_name() == "Android" and ANDROID_RIG.has(OS.get_model_name()):
		return ANDROID_RIG[OS.get_model_name()]
	return {}


func _truth(plan: Dictionary) -> String:
	var cams := []
	for c in plan["cameras"]:
		cams.append({"camera_id": str(c)})
	return JSON.stringify({"schema_version": 2, "cameras": cams,
		"concurrent_camera_support": {"supported": true, "camera_id_combinations": plan["combinations"]}})


func _start(truth: String) -> bool:
	CamBANGServer.stop()
	if truth != "" and int(CamBANGServer.ingest_camera_description(truth)) != OK:
		_fail("ingest_camera_description rejected")
		return false
	var err := 0
	if _provider_arg == "synthetic":
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
			CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME, CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED))
	else:
		err = int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
	if err != OK:
		_fail("start(%s) returned %d" % [_provider_arg, err])
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() != null:
			return true
	_fail("no baseline snapshot")
	return false


func _engage(dev) -> bool:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	var err := int(dev.engage())
	while err == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		err = int(dev.engage())
	if err != OK:
		_fail("engage() failed (%d)" % err)
		return false
	return true


# Platform: the qualified stereo geometry on a Quest, else the first catalog entry.
func _apply_platform_still(dev, hw: String) -> bool:
	var want := {}
	if OS.get_model_name() == "Quest 3":
		want = {"width": QUEST_STILL["width"], "height": QUEST_STILL["height"], "format_fourcc": CamBANGServer.PIXEL_FORMAT_NV12}
	else:
		var caps: Dictionary = CamBANGServer.get_supported_capture_profiles(hw)
		if not caps.has("profiles") or (caps["profiles"] as Array).is_empty():
			print("INFO: %s advertises no capture catalog; using the default still profile" % hw)
			return true
		var p: Dictionary = ((caps["profiles"] as Array)[0] as Dictionary)["profile"]
		want = {"width": int(p.get("width", 0)), "height": int(p.get("height", 0)), "format_fourcc": int(p.get("format_fourcc", 0))}
	var err := int(dev.set_still_capture_profile(want))
	if err != OK:
		_fail("set_still_capture_profile(%s) refused (%d)" % [JSON.stringify(want), err])
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if int(dev.get_still_capture_profile().get("width", 0)) == int(want["width"]):
			print("STEP OK: still geometry %s on %s" % [JSON.stringify(want), hw])
			return true
	_fail("still profile %s never applied on %s" % [JSON.stringify(want), hw])
	return false


# Synthetic: planar NV12 under a CPU form (the costly conversion); under gpu_only the
# producer's own RGBA geometry, set as scene 70 does (bundle only).
func _apply_synthetic_still(dev) -> bool:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	if _synth_form == "gpu_only":
		var err := ERR_BUSY
		while err == ERR_BUSY and Time.get_ticks_msec() < deadline:
			err = int(dev.set_still_capture_profile({"still_image_bundle": {"members": [
				{"image_member_index": 0, "role": CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED,
				 "intended_exposure_compensation_milli_ev": 0}]}}))
			if err == ERR_BUSY:
				await get_tree().process_frame
		if err != OK:
			_fail("synthetic gpu_only still profile refused (%d)" % err)
			return false
		while Time.get_ticks_msec() < deadline and (dev.get_still_capture_profile() as Dictionary).is_empty():
			await get_tree().process_frame
		return true
	while Time.get_ticks_msec() < deadline and int(dev.get_still_capture_profile().get("format_fourcc", 0)) != CamBANGServer.PIXEL_FORMAT_NV12:
		dev.set_still_capture_profile({"width": 1280, "height": 720, "format_fourcc": CamBANGServer.PIXEL_FORMAT_NV12})
		await get_tree().process_frame
	if int(dev.get_still_capture_profile().get("format_fourcc", 0)) != CamBANGServer.PIXEL_FORMAT_NV12:
		_fail("synthetic NV12 still profile never applied")
		return false
	return true


# Triggers until accepted (ERR_BUSY is retried) and returns the dc_ ids settled since.
func _trigger_wait(trigger: Callable, members: int) -> Array:
	var before := _finished.duplicate()
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	var cap: Dictionary = {}
	while Time.get_ticks_msec() < deadline:
		cap = trigger.call()
		if int(cap.get("error", FAILED)) != ERR_BUSY:
			break
		await get_tree().process_frame
	if int(cap.get("error", FAILED)) != OK:
		_fail("trigger_capture() err=%d" % int(cap.get("error", FAILED)))
		return []
	var ids := []
	while ids.size() < members and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		ids.clear()
		for k in _finished.keys():
			if not before.has(k) and str(k).begins_with("dc_"):
				ids.append(str(k))
	if ids.size() < members:
		_fail("capture settled %d of %d members" % [ids.size(), members])
		return []
	return ids


# ---- phase 1: identity -------------------------------------------------------

# Returns "" if WORKERS concurrent worker to_image() calls of `res` are all
# byte-identical to a main-thread to_image(), else the failure text.
func _identity(res, label: String) -> String:
	if res == null:
		return "%s: no result" % label
	var main_img: Image = res.to_image()
	if main_img == null:
		return "%s: main-thread to_image() returned null (payload_kind=%d)" % [label, int(res.get_payload_kind())]
	var ref := main_img.get_data()
	var out := {}
	var out_mutex := Mutex.new()
	var tasks := []
	for w in WORKERS:
		var slot := w
		tasks.append(WorkerThreadPool.add_task(func():
			var img: Image = res.to_image()
			out_mutex.lock()
			out[slot] = img.get_data() if img != null else null
			out_mutex.unlock()))
	await _wait_tasks(tasks)
	for w in WORKERS:
		if out.get(w) == null:
			return "%s: worker %d to_image() returned null" % [label, w]
		if out[w] != ref:
			return "%s: worker %d image differs from the main thread's" % [label, w]
	return ""


func _wait_tasks(tasks: Array) -> void:
	while true:
		var done := true
		for t in tasks:
			if not WorkerThreadPool.is_task_completed(t):
				done = false
		if done:
			break
		await get_tree().process_frame
	for t in tasks:
		WorkerThreadPool.wait_for_task_completion(t)


# ---- phase 2: stop() underneath workers ---------------------------------------

func _stop_under_workers(live: Array) -> String:
	var refs := []
	for r in live:
		var img: Image = r.to_image()
		refs.append(img.get_data() if img != null else null)
	var n := live.size()
	var t := {"before_ok": 0, "stream_newer_frame": 0, "overlap": 0, "bad": [], "after_null": [], "after_img": [], "after_bytes": []}
	for i in n:
		t["after_null"].append(0)
		t["after_img"].append(0)
		t["after_bytes"].append(null)
	var m := Mutex.new()
	var stop := {"begin": 0, "end": 0}
	var tasks := []
	var until := Time.get_ticks_msec() + STOP_PHASE_MS
	for w in WORKERS:
		tasks.append(WorkerThreadPool.add_task(func():
			while Time.get_ticks_msec() < until:
				for i in n:
					m.lock()
					var started_before: bool = stop["begin"] == 0
					var started_after: bool = stop["end"] != 0
					m.unlock()
					var img: Image = live[i].to_image()
					m.lock()
					if (started_before and stop["begin"] != 0) or (not started_before and not started_after):
						t["overlap"] += 1
					elif started_before:
						if img != null and refs[i] != null and img.get_data() == refs[i]:
							t["before_ok"] += 1
						elif img != null and live[i] is CamBANGStreamResult and refs[i] != null \
								and img.get_width() == live[i].get_width() and img.get_height() == live[i].get_height() \
								and img.get_data().size() == (refs[i] as PackedByteArray).size():
							t["stream_newer_frame"] += 1
						elif (t["bad"] as Array).size() < 8:
							t["bad"].append("r%d_before_%s" % [i, "null" if img == null else "differ"])
					elif img == null:
						t["after_null"][i] += 1
					else:
						t["after_img"][i] += 1
						if t["after_bytes"][i] == null:
							t["after_bytes"][i] = img.get_data()
						elif t["after_bytes"][i] != img.get_data() and (t["bad"] as Array).size() < 8:
							t["bad"].append("r%d_after_unstable" % i)
					m.unlock()))
	await get_tree().process_frame
	await get_tree().process_frame
	m.lock()
	stop["begin"] = Time.get_ticks_msec()
	m.unlock()
	CamBANGServer.stop()
	m.lock()
	stop["end"] = Time.get_ticks_msec()
	m.unlock()
	var main_after := []
	for r in live:
		var img: Image = r.to_image()
		main_after.append(img.get_data() if img != null else null)
	await _wait_tasks(tasks)
	var per := []
	var problems: Array = (t["bad"] as Array).duplicate()
	for i in n:
		var wn: int = t["after_null"][i]
		var wi: int = t["after_img"][i]
		var same: bool
		if main_after[i] == null:
			same = wi == 0 and wn > 0
		else:
			same = wn == 0 and wi > 0 and t["after_bytes"][i] == main_after[i]
		if not same:
			problems.append("r%d_after_differs_from_main(main_null=%s,worker_null=%d,worker_img=%d)" % [i, main_after[i] == null, wn, wi])
		per.append("r%d:main_null=%s,worker_null=%d,worker_img=%d" % [i, main_after[i] == null, wn, wi])
	print("[ToImageWorker] stop_under_workers before_ok=%d stream_newer_frame=%d overlap=%d %s" % [
		t["before_ok"], t["stream_newer_frame"], t["overlap"], " ".join(per)])
	return "" if problems.is_empty() else "stop_under_workers: " + ",".join(problems)


# ---- phase 3: last reference released on a worker -----------------------------

func _release_on_workers(live: Array) -> void:
	var holders := []
	for r in live:
		holders.append([r])
	live.clear()
	var tasks := []
	for h in holders:
		tasks.append(WorkerThreadPool.add_task(func():
			var _img: Image = h[0].to_image()
			h.clear()))
	holders.clear()
	await _wait_tasks(tasks)
	for i in 4:
		await get_tree().process_frame


# ---- run -----------------------------------------------------------------------

func _run() -> void:
	var plan := _rig_plan()
	if not await _start(_truth(plan) if not plan.is_empty() else ""):
		return
	var trigger: Callable
	var members := 1
	var stream_id := 0
	var captures := 6
	if _provider_arg == "synthetic":
		if CamBANGServer.select_builtin_scenario("stream_inspection_live") != OK or CamBANGServer.start_scenario() != OK:
			_fail("stream_inspection_live did not start")
			return
		var dev = null
		var deadline := Time.get_ticks_msec() + TIMEOUT_MS
		while (dev == null or stream_id == 0) and Time.get_ticks_msec() < deadline:
			await get_tree().process_frame
			var s = CamBANGServer.get_state_snapshot()
			if s == null:
				continue
			for d in s.get("devices", []):
				if dev == null:
					dev = CamBANGServer.get_device(int(d.get("instance_id", 0)))
			for st in s.get("streams", []):
				if stream_id == 0:
					stream_id = int(st.get("stream_id", 0))
		if dev == null:
			_fail("no synthetic device")
			return
		if _synth_form in ["gpu_only", "cpu_gpu"] and CamBANGServer.get_stream_result_by_stream_id(stream_id) == null:
			# Wait for the first stream result; a GPU form without a RenderingDevice never publishes one.
			var sdl := Time.get_ticks_msec() + 3000
			while Time.get_ticks_msec() < sdl and CamBANGServer.get_stream_result_by_stream_id(stream_id) == null:
				await get_tree().process_frame
			if CamBANGServer.get_stream_result_by_stream_id(stream_id) == null:
				_emit("expected_unsupported", "synthetic_%s_published_no_stream_result_(no_RenderingDevice?)" % _synth_form)
				return
		if not await _apply_synthetic_still(dev):
			return
		trigger = func(): return dev.trigger_capture()
		captures = 10
	elif not plan.is_empty():
		var devs: Array[CamBANGDevice] = []
		for hw in plan["pair"]:
			var d = CamBANGServer.get_device_for_hardware_id(str(hw))
			if d == null:
				_fail("no device for hardware id %s" % hw)
				return
			if not await _engage(d) or not await _apply_platform_still(d, str(hw)):
				return
			devs.append(d)
		var rig = CamBANGServer.create_rig(devs)
		if rig == null:
			_fail("create_rig refused")
			return
		trigger = func(): return rig.trigger_capture()
		members = devs.size()
		captures = 3
	else:
		var eps = CamBANGServer.enumerate_devices()
		var deadline := Time.get_ticks_msec() + TIMEOUT_MS
		while (typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty()) and Time.get_ticks_msec() < deadline:
			await get_tree().process_frame
			eps = CamBANGServer.enumerate_devices()
		if typeof(eps) != TYPE_ARRAY or (eps as Array).is_empty():
			_emit("expected_unsupported", "no_camera_endpoints")
			return
		var hw := str(((eps as Array)[0] as Dictionary).get("hardware_id", ""))
		var d = CamBANGServer.get_device_for_hardware_id(hw)
		if d == null or not await _engage(d) or not await _apply_platform_still(d, hw):
			if not _verdict_emitted:
				_fail("no device for %s" % hw)
			return
		trigger = func(): return d.trigger_capture()

	# Phase 1: identity.
	var kinds := {}
	var checked := 0
	var last_results := []
	for i in captures:
		var ids := await _trigger_wait(trigger, members)
		if ids.is_empty():
			return
		last_results.clear()
		for id in ids:
			var res = CamBANGServer.get_capture_result_by_id(id)
			var problem := await _identity(res, "capture %d %s" % [i, id])
			if problem != "":
				_fail(problem)
				return
			kinds[int(res.get_payload_kind())] = int(kinds.get(int(res.get_payload_kind()), 0)) + 1
			checked += 1
			last_results.append(res)
	var stream_checked := 0
	if stream_id != 0:
		for i in 5:
			var sres = CamBANGServer.get_stream_result_by_stream_id(stream_id)
			var problem := await _identity(sres, "stream %d" % i)
			if problem != "":
				_fail(problem)
				return
			stream_checked += 1
			await get_tree().process_frame
		last_results.append(CamBANGServer.get_stream_result_by_stream_id(stream_id))
	print("STEP OK: identity captures=%d stream=%d payload_kinds=%s" % [checked, stream_checked, JSON.stringify(kinds)])

	# Phase 2: stop() underneath converting workers.
	var problem2 := await _stop_under_workers(last_results)
	if problem2 != "":
		_fail(problem2)
		return
	print("STEP OK: stop() under converting workers")

	# Phase 3: last references released on workers.
	var released := last_results.size()
	await _release_on_workers(last_results)
	print("STEP OK: %d results released on workers" % released)

	_emit("ok", "provider=%s form=%s captures_checked=%d stream_checked=%d payload_kinds=%s workers=%d" % [
		_provider_arg, _synth_form if _provider_arg == "synthetic" else "-", checked, stream_checked, JSON.stringify(kinds), WORKERS])
