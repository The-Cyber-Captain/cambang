extends Control

const TOTAL_TIMEOUT_MS := 24000
const RESULT_SET_TIMEOUT_MS := 5000
const SCENARIO_PATH := "res://scenarios/rig_capture_result_basic.json"

const DEVICE_A := "DeviceA"
const DEVICE_B := "DeviceB"
const DEVICE_C := "DeviceC"
const DEVICE_D := "DeviceD"
const DEVICE_E := "DeviceE"
const DEVICE_F := "DeviceF"

const RIG_A_CAMERA_DESCRIPTION_JSON := "{\"schema_version\":2,\"cameras\":[{\"camera_id\":\"synthetic:0\"},{\"camera_id\":\"synthetic:4\"}],\"concurrent_camera_support\":{\"supported\":true,\"camera_id_combinations\":[[\"synthetic:0\",\"synthetic:4\"]]}}"

@onready var _status_label: RichTextLabel = $RootMargin/VBoxContainer/MainColumn/StatusLabel

var _step := 0
var _done := false
var _start_ms := 0
var _result_set_poll_start_ms := 0

var _rig_a = null
var _rig_a_capture_requested := false
var _rig_a_id := 0
var _rig_a_members: Array[int] = []
var _excluded_device_ids: Array[int] = []
var _rig_a_capture_ready := false
# Completion signals (capture_identity_and_lifecycle.md 4.2). Recorded from the
# signal handlers, which run on the Godot thread. Before this, the emit path had
# never been received by anything: it compiled and queued correctly, but no
# consumer had ever seen one fire.
var _rig_a_capture_id := ""
var _rig_a_trigger_members: Dictionary = {}
var _rig_settled_capture_id := ""
var _rig_settled_reason := -1
var _rig_settled_count := 0
var _server_settled_capture_id := ""
var _server_settled_rig_id := 0
var _server_rig_closed_reason := -1
var _server_device_finished_count := 0
# Per-device completion, one entry per member: capture_id -> disposition.
# Section 3 requires a capture to report its terminal disposition to its own
# subscriber; nothing had ever subscribed to prove that path carries.
var _device_finished: Dictionary = {}
var _result_set_verified := false
# Negative phase: the first session deliberately starts WITHOUT ingesting the
# camera-concurrency truth, so the multi-device rig admission gate must reject
# the trigger with ERR_UNCONFIGURED (a permanent configuration gap), not a
# transient-looking code. The positive phase then restarts with the truth
# ingested and runs the original verification unchanged.
var _negative_phase_complete := false

func _ready() -> void:
	_status_label.clear()
	_start_ms = Time.get_ticks_msec()
	set_process(true)

	CamBANGServer.stop()

	var start_err := _start_runtime_and_scenario()
	_require(start_err == OK, "step %d FAIL: negative-phase start rejected (%d)" % [_step, start_err])
	_step_ok("negative-phase synthetic runtime started (no camera description ingested)")

	_append_status("RUN: rig_capture_result_set_verification")


func _start_runtime_and_scenario() -> Error:
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	if start_err != OK:
		return start_err
	var scenario_text: String = FileAccess.get_file_as_string(SCENARIO_PATH)
	_require(scenario_text != "", "step %d FAIL: scenario missing at %s" % [_step, SCENARIO_PATH])
	var stage_err := CamBANGServer.load_external_scenario(scenario_text)
	_require(stage_err == OK, "step %d FAIL: unable to load external scenario" % _step)
	var scenario_start_err := CamBANGServer.start_scenario()
	_require(scenario_start_err == OK, "step %d FAIL: unable to start staged scenario" % _step)
	return OK


func _process(_delta: float) -> void:
	if _done:
		return

	if Time.get_ticks_msec() - _start_ms > TOTAL_TIMEOUT_MS:
		if _rig_a_id != 0 and not _rig_a_capture_ready:
			var snapshot = CamBANGServer.get_state_snapshot()
			var diag_lines: Array[String] = []
			if snapshot != null:
				var by_id: Dictionary = {}
				for dv in snapshot.get("devices", []):
					var d: Dictionary = dv
					by_id[int(d.get("instance_id", 0))] = d
				for member_id in _rig_a_members:
					if by_id.has(member_id):
						var d: Dictionary = by_id[member_id]
						var still: Dictionary = d.get("capture_profile", {}).get("still", {})
						diag_lines.append("id=%d hw=%s w=%d h=%d fmt=%d cpv=%d" % [
							int(member_id), str(d.get("hardware_id", "")),
							int(still.get("width", 0)), int(still.get("height", 0)),
							int(still.get("format", 0)), int(still.get("version", 0))
						])
			_fail("step %d FAIL: capture readiness timeout for Rig A members: %s" % [_step, "; ".join(diag_lines)])
			return
		_fail("step %d FAIL: total verification timeout" % _step)
		return

	if _rig_a_id == 0:
		_try_latch_and_validate_rig_topology()
		return

	if not _rig_a_capture_ready:
		_try_latch_rig_a_capture_readiness()
		return

	if not _negative_phase_complete:
		_run_negative_phase_and_restart()
		return

	if not _rig_a_capture_requested:
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
	if devices.size() != 6 or rigs.size() != 3:
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


func _try_latch_rig_a_capture_readiness() -> void:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return
	var devices: Array = snapshot.get("devices", [])
	if devices.is_empty():
		return

	var by_id: Dictionary = {}
	for dv in devices:
		var d: Dictionary = dv
		by_id[int(d.get("instance_id", 0))] = d

	var pending: Array[String] = []
	for member_id in _rig_a_members:
		if not by_id.has(member_id):
			pending.append("id=%d missing-device-row" % int(member_id))
			continue
		var d: Dictionary = by_id[member_id]
		var phase := str(d.get("phase", ""))
		if phase != "LIVE":
			var still: Dictionary = d.get("capture_profile", {}).get("still", {})
			pending.append("id=%d hw=%s phase=%s mode=%s w=%d h=%d fmt=%d cpv=%d" % [
				int(member_id), str(d.get("hardware_id", "")), phase, str(d.get("mode", "")),
				int(still.get("width", 0)), int(still.get("height", 0)),
				int(still.get("format", 0)), int(still.get("version", 0))
			])

	if not pending.is_empty():
		return

	_rig_a_capture_ready = true
	_step_ok("Rig A member devices are LIVE and capture-admissible")


func _run_negative_phase_and_restart() -> void:
	var rig = CamBANGServer.get_rig(_rig_a_id)
	_require(rig != null, "step %d FAIL: negative-phase get_rig() returned null" % _step)
	if _done:
		return
	var capture_err := int(rig.trigger_capture().get("error", FAILED))
	_require(capture_err == ERR_UNCONFIGURED,
		"step %d FAIL: rig.trigger_capture() without ingested camera-concurrency truth must return ERR_UNCONFIGURED (%d), got %d" % [
			_step, ERR_UNCONFIGURED, capture_err])
	if _done:
		return
	_step_ok("trigger without camera-concurrency truth rejected with ERR_UNCONFIGURED")

	# Restart into the positive phase with the truth ingested. Rig/device
	# instance ids belong to the finished session's generation, so all latched
	# state is reset and re-derived from the new session's snapshots.
	CamBANGServer.stop()
	var ingest_err := CamBANGServer.ingest_camera_description(RIG_A_CAMERA_DESCRIPTION_JSON)
	_require(ingest_err == OK, "step %d FAIL: ingest_camera_description rejected (%d)" % [_step, ingest_err])
	_step_ok("Rig A camera concurrency description ingested")

	var start_err := _start_runtime_and_scenario()
	_require(start_err == OK, "step %d FAIL: positive-phase start rejected (%d)" % [_step, start_err])
	if _done:
		return
	_rig_a_id = 0
	_rig_a_members = []
	_rig_a_capture_ready = false
	_excluded_device_ids = []
	_negative_phase_complete = true
	_step_ok("positive-phase synthetic runtime restarted with ingested truth")


func _trigger_rig_a_capture() -> void:
	var rig = CamBANGServer.get_rig(_rig_a_id)
	_require(rig != null, "step %d FAIL: CamBANGServer.get_rig() returned null" % _step)
	_require(rig.get_class() == "CamBANGRig", "step %d FAIL: get_rig() must return CamBANGRig" % _step)
	_require(int(rig.get_id()) == _rig_a_id, "step %d FAIL: rig.get_id() mismatch" % _step)
	if _done:
		return
	_step_ok("selected Rig A object verified")

	# Subscribe BEFORE triggering. A completion can settle inside the same tick
	# the trigger is accepted, and a subscription made afterwards would miss it
	# and look exactly like a signal that never fires.
	if not rig.capture_finished.is_connected(_on_rig_capture_finished):
		rig.capture_finished.connect(_on_rig_capture_finished)
	# One server-wide signal covers both capture kinds; branch on capture_origin.
	if not CamBANGServer.capture_finished.is_connected(_on_server_capture_finished):
		CamBANGServer.capture_finished.connect(_on_server_capture_finished)

	# Each member device individually. A rig member is an ordinary Device
	# Capture, so it must report on its own device handle too.
	for member_id in _rig_a_members:
		var member_device = CamBANGServer.get_device(member_id)
		if member_device != null and not member_device.capture_finished.is_connected(_on_device_capture_finished):
			member_device.capture_finished.connect(_on_device_capture_finished)

	var trigger: Dictionary = rig.trigger_capture()
	var capture_err := int(trigger.get("error", FAILED))
	if capture_err != OK:
		var snapshot = CamBANGServer.get_state_snapshot()
		var diag_lines: Array[String] = []
		if snapshot != null:
			var by_id: Dictionary = {}
			for dv in snapshot.get("devices", []):
				var d: Dictionary = dv
				by_id[int(d.get("instance_id", 0))] = d
			for member_id in _rig_a_members:
				if by_id.has(member_id):
					var d: Dictionary = by_id[member_id]
					var still: Dictionary = d.get("capture_profile", {}).get("still", {})
					diag_lines.append("id=%d hw=%s w=%d h=%d fmt=%d cpv=%d" % [
						int(member_id), str(d.get("hardware_id", "")),
						int(still.get("width", 0)), int(still.get("height", 0)),
						int(still.get("format", 0)), int(still.get("version", 0))
					])
				else:
					diag_lines.append("id=%d missing-device-row" % int(member_id))
		_fail("step %d FAIL: rig.trigger_capture() returned err=%d; RigA member diagnostics: %s" % [_step, capture_err, "; ".join(diag_lines)])
		return
	_rig_a = rig
	_rig_a_capture_requested = true
	_rig_a_capture_id = str(trigger.get("id", ""))
	_rig_a_trigger_members = trigger.get("members", {})

	# An accepted trigger must name the capture it started (4.1).
	_require(_rig_a_capture_id != "", "step %d FAIL: accepted rig trigger returned no id" % _step)
	# And it must name it in the RIG id space. Two counters both starting at 1
	# would hand back a Rig Capture Id numerically equal to a member's Device
	# Capture Id -- different spaces, identical number (2.1).
	_require(_rig_a_capture_id.begins_with("rc_"),
		"step %d FAIL: rig capture id %s is not in the Rig Capture space" % [
			_step, _rig_a_capture_id])
	# One member entry per rig member, each carrying that member's own Device
	# Capture Id, so an arriving result can be correlated immediately.
	_require(_rig_a_trigger_members.size() == _rig_a_members.size(),
		"step %d FAIL: rig trigger member map has %d entries, expected %d" % [
			_step, _rig_a_trigger_members.size(), _rig_a_members.size()])
	for hw in _rig_a_trigger_members.keys():
		var member_capture_id := str(_rig_a_trigger_members[hw])
		_require(member_capture_id != "",
			"step %d FAIL: rig trigger member %s carries no Device Capture Id" % [_step, str(hw)])
		_require(member_capture_id.begins_with("dc_"),
			"step %d FAIL: member %s id %s is not in the Device Capture space" % [
				_step, str(hw), member_capture_id])
	if _done:
		return
	_step_ok("rig trigger named its capture: id=%s members=%s" % [
		_rig_a_capture_id, str(_rig_a_trigger_members)])

	# Outstanding work (4.5): immediately after an accepted trigger the rig and
	# every member must be listed. Before this existed, a caller could only
	# discover a device was busy by being refused.
	var outstanding: Dictionary = CamBANGServer.get_unfinished_captures()
	var rig_ids: Array = outstanding.get("rig_captures", [])
	_require(rig_ids.has(_rig_a_capture_id),
		"step %d FAIL: rig capture %s absent from get_unfinished_captures() right after trigger" % [
			_step, _rig_a_capture_id])
	var by_device: Dictionary = outstanding.get("by_device", {})
	for member_id in _rig_a_members:
		var device_ids: Array = by_device.get(member_id, [])
		_require(not device_ids.is_empty(),
			"step %d FAIL: member device %d has no unfinished capture after rig trigger" % [
				_step, int(member_id)])
	_require(int(outstanding.get("total", 0)) >= 1 + _rig_a_members.size(),
		"step %d FAIL: unfinished total %d is less than the rig plus its %d members" % [
			_step, int(outstanding.get("total", 0)), _rig_a_members.size()])
	if _done:
		return
	_step_ok("outstanding set lists the rig and its members while in flight")
	_result_set_poll_start_ms = Time.get_ticks_msec()


func _on_rig_capture_finished(rig_capture_id: String, closed_reason: int) -> void:
	_rig_settled_capture_id = rig_capture_id
	_rig_settled_reason = int(closed_reason)
	_rig_settled_count += 1


func _on_device_capture_finished(capture_id: String, disposition: int, error_code: int) -> void:
	_device_finished[str(capture_id)] = {
		"disposition": int(disposition),
		"error_code": int(error_code),
	}


func _on_server_capture_finished(capture_id: String, info: Dictionary) -> void:
	for key in ["capture_origin", "device_instance_id", "rig_id",
			"disposition", "closed_reason", "error_code"]:
		if not info.has(key):
			_fail("server capture_finished info is missing key %s" % key)
			return
	if int(info["capture_origin"]) == CamBANGCaptureResult.CAPTURE_ORIGIN_RIG:
		_server_settled_capture_id = str(capture_id)
		_server_settled_rig_id = int(info["rig_id"])
		_server_rig_closed_reason = int(info["closed_reason"])
	else:
		_server_device_finished_count += 1


func _try_verify_capture_result_set() -> void:
	if _rig_a == null:
		return
	# CamBANGRig.get_result() returns a plain Array[CamBANGCaptureResult]
	# (no dedicated CaptureResultSet wrapper class -- see
	# docs/architecture/pixel_payload_and_result_contract.md 10.6.3).
	var results: Array = _rig_a.get_result()
	if results.is_empty():
		return

	_require(int(results.size()) == _rig_a_members.size(), "step %d FAIL: result set size mismatch" % _step)
	_step_ok("capture result set materialized for selected rig")

	var actual_ids: Array[int] = []
	for result in results:
		_require(result != null, "step %d FAIL: null capture result in result set" % _step)
		actual_ids.append(int(result.get_device_instance_id()))

	actual_ids.sort()
	_require(actual_ids == _rig_a_members, "step %d FAIL: result-set members mismatch expected=%s actual=%s" % [_step, str(_rig_a_members), str(actual_ids)])

	for excluded_id in _excluded_device_ids:
		_require(not actual_ids.has(int(excluded_id)), "step %d FAIL: excluded device id present in rig result set: %d" % [_step, int(excluded_id)])
	if not _result_set_verified:
		_result_set_verified = true
		_step_ok("result-set membership matches Rig A only; RigB/RigC/standalone excluded")

	# A materialized result set is not the same event as a closed cohort. Keep
	# polling until the completion signal arrives; the caller-facing promise of
	# 4.2 is that it does, and the existing RESULT_SET_TIMEOUT_MS bounds it.
	if _rig_settled_count == 0:
		return

	_require(_rig_settled_capture_id == _rig_a_capture_id,
		"step %d FAIL: rig capture_finished reported id %s, trigger returned %s" % [
			_step, _rig_settled_capture_id, _rig_a_capture_id])
	# Every member delivered, so the cohort must close because they all reached
	# a terminal disposition -- not because the simultaneity window ran out.
	_require(_rig_settled_reason == CamBANGServer.COHORT_CLOSED_ALL_MEMBERS_TERMINAL,
		"step %d FAIL: rig capture closed with reason %d, expected ALL_MEMBERS_TERMINAL (%d)" % [
			_step, _rig_settled_reason, CamBANGServer.COHORT_CLOSED_ALL_MEMBERS_TERMINAL])
	_require(_rig_settled_count == 1,
		"step %d FAIL: rig capture_finished fired %d times for one capture" % [_step, _rig_settled_count])
	# The server-wide fan-in must report the same settlement, attributed to this
	# rig -- that is what a consumer holding no wrapper has to rely on.
	_require(_server_settled_capture_id == _rig_a_capture_id,
		"step %d FAIL: server capture_finished reported rig capture %s, expected %s" % [
			_step, _server_settled_capture_id, _rig_a_capture_id])
	_require(_server_settled_rig_id == _rig_a_id,
		"step %d FAIL: server capture_finished reported rig %d, expected %d" % [
			_step, _server_settled_rig_id, _rig_a_id])
	if _done:
		return
	_require(_server_rig_closed_reason == CamBANGServer.COHORT_CLOSED_ALL_MEMBERS_TERMINAL,
		"step %d FAIL: server capture_finished reported closed_reason %d for the rig" % [
			_step, _server_rig_closed_reason])
	# The single server signal must carry BOTH kinds: each member's device
	# capture came through it too, not just the rig closure.
	_require(_server_device_finished_count >= _rig_a_members.size(),
		"step %d FAIL: server capture_finished carried %d device captures, expected at least %d" % [
			_step, _server_device_finished_count, _rig_a_members.size()])
	if _done:
		return
	_step_ok("completion signals received: rig, per-device and one server-wide signal carrying both kinds")

	# ...and the outstanding set must have emptied for what finished. A set that
	# only ever grows is worse than none: a caller would refuse to trigger
	# forever.
	var settled_outstanding: Dictionary = CamBANGServer.get_unfinished_captures()
	var still_open: Array = settled_outstanding.get("rig_captures", [])
	_require(not still_open.has(_rig_a_capture_id),
		"step %d FAIL: rig capture %s still listed unfinished after its completion signal" % [
			_step, _rig_a_capture_id])
	var settled_by_device: Dictionary = settled_outstanding.get("by_device", {})
	for member_id in _rig_a_members:
		var remaining: Array = settled_by_device.get(member_id, [])
		for hw in _rig_a_trigger_members.keys():
			var member_capture_id := str(_rig_a_trigger_members[hw])
			_require(not remaining.has(member_capture_id),
				"step %d FAIL: member capture %s still listed unfinished for device %d" % [
					_step, member_capture_id, int(member_id)])
	if _done:
		return
	_step_ok("outstanding set cleared once the captures finished")

	# Every member is accounted for, delivered or not (4.3). The result set can
	# be short; this must not be. Without it, a two-camera rig that lost one
	# camera is indistinguishable from a rig that only ever had one.
	var outcomes: Array = _rig_a.get_member_outcomes()
	_require(outcomes.size() == _rig_a_members.size(),
		"step %d FAIL: %d member outcomes for a %d-member rig" % [
			_step, outcomes.size(), _rig_a_members.size()])
	var outcome_device_ids: Array[int] = []
	for entry_v in outcomes:
		var entry: Dictionary = entry_v
		for key in ["hardware_id", "device_instance_id", "device_capture_id", "disposition", "error_code"]:
			_require(entry.has(key),
				"step %d FAIL: member outcome missing key %s" % [_step, key])
		outcome_device_ids.append(int(entry["device_instance_id"]))
		# This scene's members all deliver, so anything else means the outcome
		# is being reported from something other than what actually happened.
		_require(int(entry["disposition"]) == CamBANGServer.DISPOSITION_DELIVERED,
			"step %d FAIL: member %s reported disposition %d, expected DELIVERED (%d)" % [
				_step, str(entry["hardware_id"]), int(entry["disposition"]),
				CamBANGServer.DISPOSITION_DELIVERED])
		_require(int(entry["error_code"]) == 0,
			"step %d FAIL: delivered member %s carries error_code %d" % [
				_step, str(entry["hardware_id"]), int(entry["error_code"])])
		# The outcome names the member's own Device Capture Id -- the same one
		# the trigger handed back, so the two surfaces can be correlated.
		var hw := str(entry["hardware_id"])
		_require(_rig_a_trigger_members.has(hw),
			"step %d FAIL: outcome names %s, which the trigger did not" % [_step, hw])
		_require(str(entry["device_capture_id"]) == str(_rig_a_trigger_members[hw]),
			"step %d FAIL: outcome for %s says capture %s, trigger said %s" % [
				_step, hw, str(entry["device_capture_id"]), str(_rig_a_trigger_members[hw])])
	outcome_device_ids.sort()
	_require(outcome_device_ids == _rig_a_members,
		"step %d FAIL: member outcomes cover %s, expected %s" % [
			_step, str(outcome_device_ids), str(_rig_a_members)])
	if _done:
		return
	_step_ok("every rig member is accounted for by disposition, correlated to its trigger id")

	# Section 3: the device-level signal must actually reach a subscriber. Each
	# member's own device handle reports its own capture, by its own id.
	for hw in _rig_a_trigger_members.keys():
		var member_capture_id := str(_rig_a_trigger_members[hw])
		_require(_device_finished.has(member_capture_id),
			"step %d FAIL: device capture_finished never fired for member %s (capture %s)" % [
				_step, str(hw), member_capture_id])
		var report: Dictionary = _device_finished[member_capture_id]
		_require(int(report["disposition"]) == CamBANGServer.DISPOSITION_DELIVERED,
			"step %d FAIL: device capture_finished for %s reported disposition %d" % [
				_step, str(hw), int(report["disposition"])])
	if _done:
		return
	_step_ok("device-level capture_finished reached a subscriber for every member")

	# A result must describe itself (2.3). Before this, a result from a rig was
	# indistinguishable from a device-triggered one, and correlating it back to
	# its rig meant keeping the trigger's member map by hand.
	var results_again: Array = _rig_a.get_result()
	for result in results_again:
		var ident: Dictionary = result.get_capture_identity()
		for key in ["capture_origin", "device_capture_id", "rig_capture_id",
				"rig_member_hardware_id", "rig_member_index", "device_instance_id"]:
			_require(ident.has(key),
				"step %d FAIL: capture identity missing key %s" % [_step, key])
		_require(int(ident["capture_origin"]) == CamBANGCaptureResult.CAPTURE_ORIGIN_RIG,
			"step %d FAIL: rig member result reports origin %d, expected RIG (%d)" % [
				_step, int(ident["capture_origin"]), CamBANGCaptureResult.CAPTURE_ORIGIN_RIG])
		_require(str(ident["rig_capture_id"]) == _rig_a_capture_id,
			"step %d FAIL: result names rig capture %s, trigger returned %s" % [
				_step, str(ident["rig_capture_id"]), _rig_a_capture_id])
		var hw := str(ident["rig_member_hardware_id"])
		_require(_rig_a_trigger_members.has(hw),
			"step %d FAIL: result names member %s, which the trigger did not" % [_step, hw])
		_require(str(ident["device_capture_id"]) == str(_rig_a_trigger_members[hw]),
			"step %d FAIL: result for %s says capture %s, trigger said %s" % [
				_step, hw, str(ident["device_capture_id"]), str(_rig_a_trigger_members[hw])])
		_require(int(ident["rig_member_index"]) >= 0,
			"step %d FAIL: rig member result has index %d" % [_step, int(ident["rig_member_index"])])
	if _done:
		return
	_step_ok("every rig result describes its own origin, rig and membership")

	# Section 8: a device-level accessor must not be blind to rig-originated
	# captures. These member devices were never triggered directly, so before
	# this the server had no latest-capture entry for them and get_result()
	# returned null while the result plainly existed.
	for member_id in _rig_a_members:
		var member_device = CamBANGServer.get_device(member_id)
		_require(member_device != null,
			"step %d FAIL: get_device(%d) returned null" % [_step, int(member_id)])
		var member_result = member_device.get_result()
		_require(member_result != null,
			"step %d FAIL: device %d get_result() is null after a rig capture" % [
				_step, int(member_id)])
		if _done:
			return
		# ...and it must be THIS rig capture's result, not an older one.
		var member_ident: Dictionary = member_result.get_capture_identity()
		_require(int(member_ident["capture_origin"]) == CamBANGCaptureResult.CAPTURE_ORIGIN_RIG,
			"step %d FAIL: device %d get_result() returned a device-origin result" % [
				_step, int(member_id)])
		_require(str(member_ident["rig_capture_id"]) == _rig_a_capture_id,
			"step %d FAIL: device %d get_result() returned rig capture %s, expected %s" % [
				_step, int(member_id), str(member_ident["rig_capture_id"]), _rig_a_capture_id])
		_require(int(member_ident["device_instance_id"]) == int(member_id),
			"step %d FAIL: device %d get_result() returned another device's result" % [
				_step, int(member_id)])
	if _done:
		return
	_step_ok("each member device get_result() returns its own result from this rig capture")

	_append_status("PASS: rig capture result set verification complete")
	_cleanup_and_quit(0, "rig_capture_result_set_verified")


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
	_cleanup_and_quit(1, "verification_failed")


func _cleanup_and_quit(code: int, reason: String) -> void:
	if _done:
		return
	_done = true
	print("[CamBANG][HarnessVerdict] scene=rig_capture_result_set_verification status=%s exit_code=%d reason=%s" % [
		"ok" if code == 0 else "fail",
		code,
		reason,
	])
	await get_tree().create_timer(10.0).timeout

	CamBANGServer.stop()
	await get_tree().create_timer(5.0).timeout
	get_tree().quit(code)
