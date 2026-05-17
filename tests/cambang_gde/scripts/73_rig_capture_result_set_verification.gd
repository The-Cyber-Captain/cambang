extends Control

const TOTAL_TIMEOUT_MS := 12000
const RESULT_SET_TIMEOUT_MS := 5000
const SCENARIO_PATH := "res://scenarios/rig_capture_result_basic.json"

const DEVICE_A := "DeviceA"
const DEVICE_B := "DeviceB"
const DEVICE_C := "DeviceC"
const DEVICE_D := "DeviceD"
const DEVICE_E := "DeviceE"
const DEVICE_F := "DeviceF"

@onready var _status_label: RichTextLabel = $RootMargin/VBoxContainer/MainColumn/StatusLabel

var _step := 0
var _done := false
var _start_ms := 0
var _result_set_poll_start_ms := 0

var _capture_id := 0
var _rig_a_id := 0
var _rig_a_members: Array[int] = []
var _excluded_device_ids: Array[int] = []

func _ready() -> void:
	_status_label.clear()
	_start_ms = Time.get_ticks_msec()
	set_process(true)

	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	_require(start_err == OK, "step %d FAIL: synthetic timeline start rejected (%d)" % [_step, start_err])
	_step_ok("bootstrap synthetic runtime started")

	var scenario_text: String = FileAccess.get_file_as_string(SCENARIO_PATH)
	_require(scenario_text != "", "step %d FAIL: scenario missing at %s" % [_step, SCENARIO_PATH])
	var stage_err := CamBANGServer.load_external_scenario(scenario_text)
	_require(stage_err == OK, "step %d FAIL: unable to load external scenario" % _step)
	_step_ok("bootstrap scenario staged (rig_capture_result_basic)")

	var scenario_start_err := CamBANGServer.start_scenario()
	_require(scenario_start_err == OK, "step %d FAIL: unable to start staged scenario" % _step)
	_step_ok("bootstrap scenario started")

	_append_status("RUN: rig_capture_result_set_verification")


func _process(_delta: float) -> void:
	if _done:
		return

	if Time.get_ticks_msec() - _start_ms > TOTAL_TIMEOUT_MS:
		_fail("step %d FAIL: total verification timeout" % _step)
		return

	if _rig_a_id == 0:
		_try_latch_and_validate_rig_topology()
		return

	if _capture_id == 0:
		_trigger_rig_a_capture()
		return

	if Time.get_ticks_msec() - _result_set_poll_start_ms > RESULT_SET_TIMEOUT_MS:
		_fail("step %d FAIL: capture result set did not materialize within timeout" % _step)
		return
	_try_verify_capture_result_set()


func _try_latch_and_validate_rig_topology() -> void:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return

	var devices: Array = snapshot.get("devices", [])
	var rigs: Array = snapshot.get("rigs", [])
	if devices.is_empty() or rigs.is_empty():
		return

	var device_id_by_hw: Dictionary = {}
	for dv in devices:
		var d: Dictionary = dv
		var id := int(d.get("instance_id", 0))
		var hw := str(d.get("hardware_id", ""))
		_require(id > 0, "step %d FAIL: invalid device instance id in snapshot" % _step)
		_require(hw != "", "step %d FAIL: hardware_id missing in snapshot device" % _step)
		device_id_by_hw[hw] = id

	var required_hw := [
		"synthetic:0", "synthetic:1", "synthetic:2", "synthetic:3", "synthetic:4", "synthetic:5"
	]
	for hw in required_hw:
		_require(device_id_by_hw.has(hw), "step %d FAIL: expected hardware id missing: %s" % [_step, hw])
	_step_ok("expected six deterministic devices present")

	var id_a := int(device_id_by_hw["synthetic:0"])
	var id_b := int(device_id_by_hw["synthetic:1"])
	var id_c := int(device_id_by_hw["synthetic:2"])
	var id_d := int(device_id_by_hw["synthetic:3"])
	var id_e := int(device_id_by_hw["synthetic:4"])
	var id_f := int(device_id_by_hw["synthetic:5"])

	var expected_a := _sorted_ids([id_a, id_e])
	var expected_b := _sorted_ids([id_b])
	var expected_c := _sorted_ids([id_c, id_f])

	var found_rig_a := false
	var found_rig_b := false
	var found_rig_c := false
	var all_rig_members: Array[int] = []

	for rv in rigs:
		var r: Dictionary = rv
		var rig_id := int(r.get("rig_id", 0))
		_require(rig_id > 0, "step %d FAIL: snapshot rig has invalid rig_id" % _step)
		var members_variant: Variant = r.get("member_device_instance_ids", null)
		if members_variant == null:
			var hw_members: Variant = r.get("member_hardware_ids", [])
			_require(typeof(hw_members) == TYPE_ARRAY, "step %d FAIL: rig member_hardware_ids must be Array" % _step)
			var derived: Array = []
			for hwv in hw_members:
				var hw := str(hwv)
				_require(device_id_by_hw.has(hw), "step %d FAIL: rig member hardware_id not found in devices: %s" % [_step, hw])
				derived.append(int(device_id_by_hw[hw]))
			members_variant = derived
		_require(typeof(members_variant) == TYPE_ARRAY, "step %d FAIL: rig members must be Array" % _step)
		var members: Array = _sorted_ids(members_variant)
		for m in members:
			if not all_rig_members.has(m):
				all_rig_members.append(m)

		if members == expected_a:
			found_rig_a = true
			_rig_a_id = rig_id
			_rig_a_members = members
		elif members == expected_b:
			found_rig_b = true
		elif members == expected_c:
			found_rig_c = true

	_require(found_rig_a, "step %d FAIL: Rig A signature [A,E] not found" % _step)
	_require(found_rig_b, "step %d FAIL: Rig B signature [B] not found" % _step)
	_require(found_rig_c, "step %d FAIL: Rig C signature [C,F] not found" % _step)
	_step_ok("expected rigs present (A=[A,E], B=[B], C=[C,F])")

	_require(not all_rig_members.has(id_d), "step %d FAIL: standalone DeviceD unexpectedly appears in rig membership" % _step)
	_step_ok("standalone DeviceD verified as non-member")

	_excluded_device_ids = [id_b, id_c, id_d, id_f]


func _trigger_rig_a_capture() -> void:
	var rig = CamBANGServer.get_rig(_rig_a_id)
	_require(rig != null, "step %d FAIL: CamBANGServer.get_rig() returned null" % _step)
	_require(rig.get_class() == "CamBANGRig", "step %d FAIL: get_rig() must return CamBANGRig" % _step)
	_require(int(rig.get_id()) == _rig_a_id, "step %d FAIL: rig.get_id() mismatch" % _step)
	_step_ok("selected Rig A object verified")

	_capture_id = int(rig.trigger_capture())
	_require(_capture_id != 0, "step %d FAIL: rig.trigger_capture() returned zero capture id" % _step)
	_step_ok("rig capture trigger accepted (capture_id=%d)" % _capture_id)
	_result_set_poll_start_ms = Time.get_ticks_msec()


func _try_verify_capture_result_set() -> void:
	var result_set = CamBANGServer.get_capture_result_set(_capture_id)
	if result_set == null or result_set.is_empty():
		return

	_require(result_set.get_class() == "CamBANGCaptureResultSet", "step %d FAIL: result set must be CamBANGCaptureResultSet" % _step)
	_require(int(result_set.get_capture_id()) == _capture_id, "step %d FAIL: result set capture_id mismatch" % _step)
	_require(int(result_set.size()) == _rig_a_members.size(), "step %d FAIL: result set size mismatch" % _step)
	_step_ok("capture result set materialized for selected rig")

	var actual_ids: Array[int] = []
	for result in result_set.get_results():
		_require(result != null, "step %d FAIL: null capture result in result set" % _step)
		_require(int(result.get_capture_id()) == _capture_id, "step %d FAIL: capture result capture_id mismatch" % _step)
		actual_ids.append(int(result.get_device_instance_id()))

	actual_ids.sort()
	_require(actual_ids == _rig_a_members, "step %d FAIL: result-set members mismatch expected=%s actual=%s" % [_step, str(_rig_a_members), str(actual_ids)])

	for excluded_id in _excluded_device_ids:
		_require(not actual_ids.has(int(excluded_id)), "step %d FAIL: excluded device id present in rig result set: %d" % [_step, int(excluded_id)])
	_step_ok("result-set membership matches Rig A only; RigB/RigC/standalone excluded")

	_append_status("PASS: rig capture result set verification complete")
	_cleanup_and_quit(0)


func _sorted_ids(input_ids: Array) -> Array[int]:
	var out: Array[int] = []
	for v in input_ids:
		var id := int(v)
		_require(id > 0, "step %d FAIL: expected positive device id in member set" % _step)
		out.append(id)
	out.sort()
	return out


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	_fail(message)


func _step_ok(message: String) -> void:
	_step += 1
	_append_status("step %d OK: %s" % [_step, message])


func _append_status(message: String) -> void:
	print(message)
	_status_label.append_text("%s\n" % message)


func _fail(message: String) -> void:
	push_error(message)
	_append_status(message)
	_cleanup_and_quit(1)


func _cleanup_and_quit(code: int) -> void:
	if _done:
		return
	_done = true
	CamBANGServer.stop()
	get_tree().quit(code)
