extends Node

## Scene 80: CamBANGServer.capture_image_acquired -- early acquisition timing.
##
## Also the worked example of the signal. A consumer that needs an image's
## acquisition timing as early as possible (for example to look up a head pose
## for that instant before a pose history expires) does exactly this:
##
##   CamBANGServer.capture_image_acquired.connect(_on_image_acquired)
##   CamBANGServer.capture_finished.connect(_on_capture_finished)
##
##   func _on_image_acquired(capture_id: String, info: Dictionary) -> void:
##       # Record whatever is time-critical NOW, keyed by the image it is for.
##       _pending[[capture_id, info["image_member_index"]]] = my_observation(info)
##
##   func _on_capture_finished(capture_id: String, info: Dictionary) -> void:
##       # Settlement: join or discard everything recorded for capture_id.
##       var result = CamBANGServer.get_capture_result_by_id(capture_id)
##       ...result.get_image_member(i) is the image named by that notice...
##
## For every capture this scene triggers it proves:
## - every image of the final result was announced exactly once, before
##   capture_finished for its capture, under the result's own capture id and
##   image_member_index;
## - the announced acquisition_timing equals the final image's;
## - image_role and the capture identity (origin, device, rig membership) agree
##   with the result;
## - every key is present in every notice;
## - delivery latency (emitted_ticks_usec - observed_ticks_usec) stays inside
##   the 50 ms consumer window at p99.
##
## Phases: single image, three-member bracket, two-device rig. A phase the
## hardware cannot do (no bracket support, no declared concurrent pair) is
## reported as skipped, not failed; a scene whose every phase was skipped is
## expected_unsupported.
##
## Provider: --cambang-bench-provider=synthetic|platform (default synthetic),
## or the cambang/maintainer/bench_provider project setting on Android.
## Captures per phase: synthetic 20/5/5, platform 200/20/20; override the first
## with --cambang-early-timing-captures=N (host only).
##
## Self-terminating: emits [CamBANG][HarnessVerdict].

const SCENE_LABEL := "80_capture_image_acquired_verify"
const TIMEOUT_MS := 8000
const WINDOW_USEC := 50000

const INFO_KEYS := [
	"capture_origin", "rig_capture_id", "rig_member_hardware_id", "rig_member_index",
	"device_instance_id", "image_member_index", "image_role", "acquisition_timing",
	"has_acquisition_timing", "observed_ticks_usec", "emitted_ticks_usec",
]

const SYNTH_A := "synthetic:0"
const SYNTH_B := "synthetic:1"

# Android rig pairs by model (stable Camera2 ids), mirroring scene 870's table.
const ANDROID_RIG := {
	"Quest 3": {"cameras": ["1", "50", "51"], "combinations": [["1", "50", "51"]], "pair": ["50", "51"]},
	"SM-G986U1": {"cameras": ["0", "1", "2", "3", "4"], "combinations": [["0", "1"], ["0", "3"]], "pair": ["0", "1"]},
}

var _provider_arg := "synthetic"
var _captures_single := 0
var _captures_bracket := 0
var _captures_rig := 0
var _verdict_emitted := false

# Event log, in the order the main thread saw them.
var _seq := 0
var _notices: Dictionary = {}       # capture_id -> Array[{seq, info}]
var _finished_seq: Dictionary = {}  # capture_id -> seq of first capture_finished
var _finished_info: Dictionary = {}

var _dispatch_usec: Array[int] = []      # emitted - observed
var _trigger_to_notice_usec: Array[int] = []
var _notice_lead_usec: Array[int] = []   # capture_finished time - notice emitted
var _finished_ticks: Dictionary = {}
var _no_timing_count := 0
var _phases_run: Array[String] = []
var _phases_skipped: Array[String] = []
# Still geometry taken from the endpoint's own capture catalog (platform only);
# empty means the provider default, which a real camera may not offer.
var _still_geometry: Dictionary = {}


func _ready() -> void:
	_parse_args()
	print("RUN: %s provider=%s captures=%d/%d/%d" % [
		SCENE_LABEL, _provider_arg, _captures_single, _captures_bracket, _captures_rig])
	CamBANGServer.capture_image_acquired.connect(_on_image_acquired)
	CamBANGServer.capture_finished.connect(_on_capture_finished)
	await _run()


func _parse_args() -> void:
	var setting := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting != "":
		_provider_arg = setting
	var override := -1
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	for arg in args:
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
		elif arg.begins_with("--cambang-early-timing-captures="):
			override = int(arg.substr("--cambang-early-timing-captures=".length()))
	var synthetic := _provider_arg == "synthetic"
	_captures_single = 20 if synthetic else 200
	_captures_bracket = 5 if synthetic else 20
	_captures_rig = 5 if synthetic else 20
	if override > 0:
		_captures_single = override


func _emit(status: String, reason: String) -> void:
	if _verdict_emitted:
		return
	_verdict_emitted = true
	# Printed on every verdict, so a failed run still carries its distributions.
	print("[CamBANG][EarlyTiming] provider=%s os=%s model=%s phases=%s skipped=%s no_timing=%d %s %s %s" % [
		_provider_arg, OS.get_name(), OS.get_model_name(), ",".join(_phases_run), ",".join(_phases_skipped),
		_no_timing_count,
		_dist("dispatch_usec", _dispatch_usec),
		_dist("trigger_to_notice_usec", _trigger_to_notice_usec),
		_dist("notice_lead_over_finished_usec", _notice_lead_usec)])
	var code := 0 if status == "ok" or status == "expected_unsupported" else 1
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [SCENE_LABEL, status, code, reason])
	CamBANGServer.stop()
	get_tree().quit(code)


func _fail(reason: String) -> void:
	_emit("fail", reason)


func _exit_tree() -> void:
	CamBANGServer.stop()


# ---- signal handlers ---------------------------------------------------------

func _on_image_acquired(capture_id: String, info: Dictionary) -> void:
	_seq += 1
	if not _notices.has(capture_id):
		_notices[capture_id] = []
	(_notices[capture_id] as Array).append({"seq": _seq, "info": info.duplicate(true)})


func _on_capture_finished(capture_id: String, info: Dictionary) -> void:
	_seq += 1
	if not _finished_seq.has(capture_id):
		_finished_seq[capture_id] = _seq
		_finished_info[capture_id] = info.duplicate(true)
		_finished_ticks[capture_id] = Time.get_ticks_usec()


# ---- session helpers ---------------------------------------------------------

func _start(truth: String) -> bool:
	CamBANGServer.stop()
	if truth != "" and int(CamBANGServer.ingest_camera_description(truth)) != OK:
		_fail("ingest_camera_description rejected")
		return false
	var err := int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC) if _provider_arg == "synthetic"
		else CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
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


func _first_hardware_id() -> String:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		var eps = CamBANGServer.enumerate_devices()
		if typeof(eps) == TYPE_ARRAY and not (eps as Array).is_empty():
			return str(((eps as Array)[0] as Dictionary).get("hardware_id", ""))
		await get_tree().process_frame
	return ""


func _engage(dev) -> bool:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	var err := int(dev.engage())
	while err == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		err = int(dev.engage())
	if err != OK:
		_fail("engage() failed (%d)" % err)
		return false
	while Time.get_ticks_msec() < deadline:
		if bool(dev.is_live()):
			return true
		await get_tree().process_frame
	_fail("device never became live")
	return false


# Triggers via `trigger_fn` (retrying ERR_BUSY briefly) and waits for its
# capture_finished. Returns the capture id or "" after failing the scene.
#
# `refused_ok` names a trigger error that means "this device does not do this"
# rather than a failure; it returns REFUSED instead of failing the scene.
const REFUSED := "<refused>"

func _trigger_and_wait(trigger_fn: Callable, what: String, refused_ok: int = OK) -> String:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	var trigger: Dictionary = trigger_fn.call()
	var trigger_ticks := Time.get_ticks_usec()
	while int(trigger.get("error", FAILED)) == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		trigger = trigger_fn.call()
		trigger_ticks = Time.get_ticks_usec()
	var err := int(trigger.get("error", FAILED))
	var id := str(trigger.get("id", ""))
	if refused_ok != OK and err == refused_ok:
		return REFUSED
	if err != OK or id.is_empty():
		_fail("%s: trigger_capture() err=%d" % [what, err])
		return ""
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if _finished_seq.has(id):
			# Two more frames so a late or duplicate notice would be seen.
			await get_tree().process_frame
			await get_tree().process_frame
			_record_trigger_latency(id, trigger_ticks)
			return id
	_fail("%s: capture_finished never arrived for %s" % [what, id])
	return ""


func _record_trigger_latency(capture_id: String, trigger_ticks: int) -> void:
	for entry in _notices.get(capture_id, []):
		var info: Dictionary = (entry as Dictionary)["info"]
		var observed := int(info.get("observed_ticks_usec", -1))
		if observed >= 0:
			_trigger_to_notice_usec.append(observed - trigger_ticks)


# ---- verification ------------------------------------------------------------

# Verifies every notice of one DEVICE capture (dc_ id) against its result.
# Returns "" when it holds, else the failure text.
func _verify_device_capture(capture_id: String, expect_rig_id: String) -> String:
	var finished: Dictionary = _finished_info.get(capture_id, {})
	if int(finished.get("disposition", -1)) != CamBANGServer.DISPOSITION_DELIVERED:
		return "capture %s finished with disposition %d error_code=%d" % [
			capture_id, int(finished.get("disposition", -1)), int(finished.get("error_code", 0))]
	var result = CamBANGServer.get_capture_result_by_id(capture_id)
	if result == null:
		return "no result for delivered capture %s" % capture_id
	var identity: Dictionary = result.get_capture_identity()
	var entries: Array = _notices.get(capture_id, [])
	var count := int(result.get_image_count())
	if entries.size() != count:
		return "capture %s: %d notice(s) for %d image(s)" % [capture_id, entries.size(), count]
	var seen_members := {}
	for entry in entries:
		var e: Dictionary = entry
		var info: Dictionary = e["info"]
		for k in INFO_KEYS:
			if not info.has(k):
				return "capture %s: notice missing key %s" % [capture_id, k]
		if int(e["seq"]) > int(_finished_seq[capture_id]):
			return "capture %s: notice emitted after capture_finished" % capture_id
		var member := int(info["image_member_index"])
		if seen_members.has(member):
			return "capture %s: member %d announced twice" % [capture_id, member]
		seen_members[member] = true
		var image: Dictionary = result.get_image_member(member)
		if image.is_empty():
			return "capture %s: notice names member %d the result does not have" % [capture_id, member]
		if int(info["image_role"]) != int(image.get("role", -1)):
			return "capture %s member %d: image_role %d != result role %d" % [
				capture_id, member, int(info["image_role"]), int(image.get("role", -1))]
		var final_timing: Dictionary = (image.get("camera_facts", {}) as Dictionary).get("acquisition_timing", {})
		if bool(info["has_acquisition_timing"]) != (not final_timing.is_empty()):
			return "capture %s member %d: timing presence differs (notice=%s final=%s)" % [
				capture_id, member, str(info["has_acquisition_timing"]), str(not final_timing.is_empty())]
		if bool(info["has_acquisition_timing"]):
			if (info["acquisition_timing"] as Dictionary) != final_timing:
				return "capture %s member %d: timing %s != final %s" % [
					capture_id, member, JSON.stringify(info["acquisition_timing"]), JSON.stringify(final_timing)]
		else:
			if not (info["acquisition_timing"] as Dictionary).is_empty():
				return "capture %s member %d: acquisition_timing not {} when absent" % [capture_id, member]
			_no_timing_count += 1
		if int(info["capture_origin"]) != int(identity.get("capture_origin", -1)):
			return "capture %s: capture_origin differs from result identity" % capture_id
		if str(info["rig_capture_id"]) != str(identity.get("rig_capture_id", "")):
			return "capture %s: rig_capture_id differs from result identity" % capture_id
		if expect_rig_id != "" and str(info["rig_capture_id"]) != expect_rig_id:
			return "capture %s: rig_capture_id %s != triggered %s" % [capture_id, str(info["rig_capture_id"]), expect_rig_id]
		if str(info["rig_member_hardware_id"]) != str(identity.get("rig_member_hardware_id", "")) \
				or int(info["rig_member_index"]) != int(identity.get("rig_member_index", -1)):
			return "capture %s: rig membership differs from result identity" % capture_id
		if int(info["device_instance_id"]) != int(identity.get("device_instance_id", 0)):
			return "capture %s: device_instance_id differs from result identity" % capture_id
		var emitted := int(info["emitted_ticks_usec"])
		var observed := int(info["observed_ticks_usec"])
		if emitted < 0:
			return "capture %s: emitted_ticks_usec missing" % capture_id
		if observed >= 0:
			if observed > emitted:
				return "capture %s: observed_ticks_usec after emitted_ticks_usec" % capture_id
			_dispatch_usec.append(emitted - observed)
		_notice_lead_usec.append(int(_finished_ticks[capture_id]) - emitted)
	return ""


func _percentile(values: Array[int], p: float) -> int:
	if values.is_empty():
		return -1
	var sorted := values.duplicate()
	sorted.sort()
	var idx := clampi(int(ceil(p * sorted.size())) - 1, 0, sorted.size() - 1)
	return int(sorted[idx])


func _dist(label: String, values: Array[int]) -> String:
	return "%s(n=%d p50=%d p90=%d p99=%d max=%d)" % [
		label, values.size(), _percentile(values, 0.5), _percentile(values, 0.9),
		_percentile(values, 0.99), _percentile(values, 1.0)]


# ---- phases ------------------------------------------------------------------

# A platform camera only captures at a geometry it advertises, and the default
# still profile is not guaranteed to be one. Take the first catalog entry, as a
# consumer would, and wait for the profile to apply before capturing.
func _apply_catalog_still_geometry(dev, hw: String) -> bool:
	if _provider_arg == "synthetic":
		return true
	var caps: Dictionary = CamBANGServer.get_supported_capture_profiles(hw)
	if not caps.has("profiles") or (caps["profiles"] as Array).is_empty():
		print("INFO: %s advertises no capture catalog; using the default still profile" % hw)
		return true
	var profile: Dictionary = (((caps["profiles"] as Array)[0] as Dictionary)["profile"] as Dictionary).duplicate()
	_still_geometry = {
		"width": int(profile.get("width", 0)),
		"height": int(profile.get("height", 0)),
		"format_fourcc": int(profile.get("format_fourcc", 0)),
	}
	var err := int(dev.set_still_capture_profile(_still_geometry))
	if err != OK:
		_fail("set_still_capture_profile(%s) refused a catalog entry (%d)" % [JSON.stringify(_still_geometry), err])
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		var p = dev.get_still_capture_profile()
		if typeof(p) == TYPE_DICTIONARY and int((p as Dictionary).get("width", 0)) == int(_still_geometry["width"]) \
				and int((p as Dictionary).get("height", 0)) == int(_still_geometry["height"]):
			print("STEP OK: still geometry %s" % JSON.stringify(_still_geometry))
			return true
	_fail("still profile %s never applied" % JSON.stringify(_still_geometry))
	return false


func _phase_single(dev) -> bool:
	for i in range(_captures_single):
		var id := await _trigger_and_wait(func(): return dev.trigger_capture(), "single #%d" % i)
		if id.is_empty():
			return false
		var problem := _verify_device_capture(id, "")
		if problem != "":
			_fail("single: " + problem)
			return false
	_phases_run.append("single")
	return true


func _phase_bracket(dev) -> bool:
	var members := [
		{"image_member_index": 0, "role": CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED, "intended_exposure_compensation_milli_ev": 0},
		{"image_member_index": 1, "role": CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET, "intended_exposure_compensation_milli_ev": -1000},
		{"image_member_index": 2, "role": CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET, "intended_exposure_compensation_milli_ev": 1000},
	]
	var request: Dictionary = _still_geometry.duplicate()
	request["still_image_bundle"] = {"members": members}
	var err := int(dev.set_still_capture_profile(request))
	if err != OK:
		print("SKIP: bracket: set_still_capture_profile refused (%d)" % err)
		_phases_skipped.append("bracket")
		return true
	var done := 0
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while done < _captures_bracket:
		# A provider may accept the profile yet refuse a multi-image bundle at
		# admission on a device that cannot honour it (WinRT without an
		# exposure-compensation control refuses with ERR_UNAVAILABLE). That is
		# the hardware's answer, so the phase is skipped, not failed -- but only
		# before any bracket has been delivered.
		var id := await _trigger_and_wait(func(): return dev.trigger_capture(), "bracket #%d" % done,
			ERR_UNAVAILABLE if done == 0 else OK)
		if id == REFUSED:
			print("SKIP: bracket: trigger_capture() refused the bundle on this device (ERR_UNAVAILABLE)")
			_phases_skipped.append("bracket")
			return true
		if id.is_empty():
			return false
		var problem := _verify_device_capture(id, "")
		if problem != "":
			_fail("bracket: " + problem)
			return false
		var result = CamBANGServer.get_capture_result_by_id(id)
		if int(result.get_image_count()) == members.size():
			done += 1
		elif done == 0 and Time.get_ticks_msec() < deadline:
			continue  # profile not applied yet; a single-image capture still verified
		else:
			_fail("bracket: capture %s has %d image(s) after the profile applied" % [id, int(result.get_image_count())])
			return false
	_phases_run.append("bracket")
	return true


func _rig_plan() -> Dictionary:
	if _provider_arg == "synthetic":
		return {"cameras": [SYNTH_A, SYNTH_B], "combinations": [[SYNTH_A, SYNTH_B]], "pair": [SYNTH_A, SYNTH_B]}
	if OS.get_name() == "Android":
		var model := OS.get_model_name()
		if ANDROID_RIG.has(model):
			return ANDROID_RIG[model]
	return {}


func _truth(plan: Dictionary) -> String:
	var cams := []
	for c in plan["cameras"]:
		cams.append({"camera_id": str(c)})
	return JSON.stringify({
		"schema_version": 2,
		"cameras": cams,
		"concurrent_camera_support": {"supported": true, "camera_id_combinations": plan["combinations"]},
	})


func _phase_rig() -> bool:
	var plan := _rig_plan()
	if plan.is_empty():
		print("SKIP: rig: no declared concurrent pair for %s/%s" % [OS.get_name(), OS.get_model_name()])
		_phases_skipped.append("rig")
		return true
	if not await _start(_truth(plan)):
		return false
	var devs: Array[CamBANGDevice] = []
	for hw in plan["pair"]:
		var d = CamBANGServer.get_device_for_hardware_id(str(hw))
		if d == null:
			_fail("rig: no handle for %s" % str(hw))
			return false
		if not await _engage(d):
			return false
		if not await _apply_catalog_still_geometry(d, str(hw)):
			return false
		devs.append(d)
	var rig = CamBANGServer.create_rig(devs)
	if rig == null:
		_fail("rig: create_rig refused the declared pair %s" % str(plan["pair"]))
		return false
	for i in range(_captures_rig):
		var before := {}
		for k in _finished_seq.keys():
			before[k] = true
		var rig_id := await _trigger_and_wait(func(): return rig.trigger_capture(), "rig #%d" % i)
		if rig_id.is_empty():
			return false
		# The rig's device captures are the dc_ ids that finished during it.
		var members := 0
		for k in _finished_seq.keys():
			var key := str(k)
			if before.has(key) or not key.begins_with("dc_"):
				continue
			var problem := _verify_device_capture(key, rig_id)
			if problem != "":
				_fail("rig: " + problem)
				return false
			members += 1
		if members != 2:
			_fail("rig: capture %s settled %d device capture(s), expected 2" % [rig_id, members])
			return false
	_phases_run.append("rig")
	return true


func _run() -> void:
	if not await _start(""):
		return
	var hw := await _first_hardware_id()
	if hw.is_empty():
		_emit("expected_unsupported", "no_camera_endpoints")
		return
	var dev = CamBANGServer.get_device_for_hardware_id(hw)
	if dev == null:
		_fail("no handle for %s" % hw)
		return
	if not await _engage(dev):
		return
	if not await _apply_catalog_still_geometry(dev, hw):
		return
	if not await _phase_single(dev):
		return
	if not await _phase_bracket(dev):
		return
	if not await _phase_rig():
		return

	var p99 := _percentile(_dispatch_usec, 0.99)
	if p99 > WINDOW_USEC:
		_fail("dispatch latency p99=%dus exceeds the %dus window" % [p99, WINDOW_USEC])
		return
	if _phases_run.is_empty():
		_emit("expected_unsupported", "no_phase_runnable")
		return
	_emit("ok", "phases=%s;skipped=%s;dispatch_p99_usec=%d" % [",".join(_phases_run), ",".join(_phases_skipped), p99])
