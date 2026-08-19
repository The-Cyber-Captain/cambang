extends Node

## Scene 74: CamBANGServer.create_rig() public-API verification.
##
## Proves the new rig-formation factory (added so platform-backed providers, and
## synthetic without a scenario, can form rigs -- previously rigs only existed via
## synthetic scenario staging). Contract:
## - with an ingested concurrency truth that authorizes the exact combination,
##   create_rig(devices) returns a live CamBANGRig whose rig appears
##   in the published snapshot,
## - a single-member request returns null (a rig needs >= 2 members),
## - with no authorizing truth (Unsupported), create_rig returns null (fail
##   closed) -- the same gate rig-capture admission uses at trigger time.
##
## Synthetic-only, headless, self-terminating: emits [CamBANG][HarnessVerdict].

const SCENE_LABEL := "74_rig_create_api_verification"
const HW_A := "synthetic:0"
const HW_B := "synthetic:1"

# Concurrency truth that authorizes the [synthetic:0, synthetic:1] combination.
const TRUTH_SUPPORTED := "{\"schema_version\":2,\"cameras\":[{\"camera_id\":\"synthetic:0\"},{\"camera_id\":\"synthetic:1\"}],\"concurrent_camera_support\":{\"supported\":true,\"camera_id_combinations\":[[\"synthetic:0\",\"synthetic:1\"]]}}"
# A valid truth that declares concurrency unsupported -> no rig may form.
const TRUTH_UNSUPPORTED := "{\"schema_version\":2,\"cameras\":[{\"camera_id\":\"synthetic:0\"},{\"camera_id\":\"synthetic:1\"}],\"concurrent_camera_support\":{\"supported\":false}}"

var _verdict_emitted := false


func _ready() -> void:
	print("RUN: %s" % SCENE_LABEL)
	await _run()


func _emit(status: String, reason: String) -> void:
	if _verdict_emitted:
		return
	_verdict_emitted = true
	var code := 0 if status == "ok" else 1
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [SCENE_LABEL, status, code, reason])
	CamBANGServer.stop()
	get_tree().quit(code)


func _fail(reason: String) -> void:
	_emit("fail", reason)


func _exit_tree() -> void:
	CamBANGServer.stop()


func _wait_snapshot(timeout_ms: int) -> void:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() != null:
			return


func _engage_live(hw: String, timeout_ms: int):
	var dev = CamBANGServer.get_device_for_hardware_id(hw)
	if dev == null or int(dev.engage()) != OK:
		return null
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if dev.has_method("is_live") and bool(dev.is_live()):
			return dev
	return dev


func _rig_in_snapshot(rig_id: int) -> bool:
	var snap = CamBANGServer.get_state_snapshot()
	if snap == null:
		return false
	for rv in snap.get("rigs", []):
		if int((rv as Dictionary).get("rig_id", 0)) == rig_id:
			return true
	return false


func _run() -> void:
	# --- A) positive: authorized combination forms a live, published rig -------
	CamBANGServer.stop()
	if int(CamBANGServer.ingest_camera_description(TRUTH_SUPPORTED)) != OK:
		_fail("ingest_camera_description(supported) returned non-OK")
		return
	if int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)) != OK:
		_fail("start(synthetic) failed")
		return
	await _wait_snapshot(2000)
	var dev_a = await _engage_live(HW_A, 4000)
	if dev_a == null:
		_fail("device %s did not become live" % HW_A)
		return
	var dev_b = await _engage_live(HW_B, 4000)
	if dev_b == null:
		_fail("device %s did not become live" % HW_B)
		return

	# Wrapper objects are canonical per id: asking again returns the SAME
	# object, which is what makes a handle worth connecting a signal to and
	# what makes identity comparison usable for keeping a set of devices.
	if CamBANGServer.get_device_for_hardware_id(HW_A) != dev_a:
		_fail("get_device_for_hardware_id(%s) returned a different wrapper instance" % HW_A)
		return
	# get_device(instance_id) is canonical for ITS key, not unified with the
	# endpoint form: the two handle kinds resolve their instance id differently
	# and merging them breaks a scenario-created device (scene 70).
	var by_instance = CamBANGServer.get_device(dev_a.get_instance_id())
	if by_instance == null or CamBANGServer.get_device(dev_a.get_instance_id()) != by_instance:
		_fail("get_device(instance_id) is not canonical for its own id")
		return

	var rig = CamBANGServer.create_rig([dev_a, dev_b] as Array[CamBANGDevice])
	if rig == null:
		_fail("create_rig returned null for an authorized combination")
		return
	if int(rig.get_id()) <= 0:
		_fail("create_rig handle has invalid rig_id")
		return
	await get_tree().process_frame
	if not _rig_in_snapshot(int(rig.get_id())):
		_fail("created rig %d absent from published snapshot" % int(rig.get_id()))
		return

	if CamBANGServer.get_rig(int(rig.get_id())) != rig:
		_fail("get_rig(%d) returned a different wrapper than create_rig" % int(rig.get_id()))
		return

	# --- C) arity: a single-member request forms no rig ------------------------
	if CamBANGServer.create_rig([dev_a] as Array[CamBANGDevice]) != null:
		_fail("create_rig accepted a single-member request")
		return
	# A handle that names no device is a caller error, not an empty slot.
	if CamBANGServer.create_rig([dev_a, null] as Array[CamBANGDevice]) != null:
		_fail("create_rig accepted a null device handle")
		return

	# --- B) gate: no authorizing truth -> fail closed --------------------------
	CamBANGServer.stop()
	if int(CamBANGServer.ingest_camera_description(TRUTH_UNSUPPORTED)) != OK:
		_fail("ingest_camera_description(unsupported) returned non-OK")
		return
	if int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)) != OK:
		_fail("restart(synthetic) failed")
		return
	await _wait_snapshot(2000)
	var dev_a2 = CamBANGServer.get_device_for_hardware_id(HW_A)
	var dev_b2 = CamBANGServer.get_device_for_hardware_id(HW_B)
	if dev_a2 == null or dev_b2 == null:
		_fail("device handles unavailable after restart under the Unsupported truth")
		return
	if CamBANGServer.create_rig([dev_a2, dev_b2] as Array[CamBANGDevice]) != null:
		_fail("create_rig formed a rig under an Unsupported concurrency truth")
		return

	_emit("ok", "authorized-create+snapshot, arity-reject, unsupported-gate-reject all verified")
