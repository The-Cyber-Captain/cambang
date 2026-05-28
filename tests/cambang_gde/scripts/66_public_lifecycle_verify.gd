extends Node

const MAX_FRAMES := 180

var _done := false


func _ready() -> void:
	CamBANGServer.stop()

	if not CamBANGServer.has_method("enumerate_devices"):
		_fail("FAIL: CamBANGServer.enumerate_devices() missing")
		return
	if not CamBANGServer.has_method("get_device_for_hardware_id"):
		_fail("FAIL: CamBANGServer.get_device_for_hardware_id() missing")
		return

	var start_err := CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)
	if start_err != OK:
		_fail("FAIL: start(SYNTHETIC) rejected")
		return

	var endpoints = CamBANGServer.enumerate_devices()
	if typeof(endpoints) != TYPE_ARRAY or endpoints.size() < 1:
		_fail("FAIL: synthetic enumerate_devices() must return at least one endpoint")
		return
	var endpoint0 = endpoints[0]
	if typeof(endpoint0) != TYPE_DICTIONARY:
		_fail("FAIL: enumerate_devices() entries must be Dictionary")
		return
	var hardware_id := str(endpoint0.get("hardware_id", ""))
	if hardware_id.is_empty():
		_fail("FAIL: enumerate_devices() endpoint hardware_id must be non-empty")
		return

	var handle_a = CamBANGServer.get_device_for_hardware_id(hardware_id)
	var handle_b = CamBANGServer.get_device_for_hardware_id(hardware_id)
	if handle_a == null or handle_b == null:
		_fail("FAIL: get_device_for_hardware_id() must return non-null handles for known hardware_id")
		return
	if not handle_a.has_method("engage") or not handle_a.has_method("disengage") or not handle_a.has_method("create_stream") or not handle_a.has_method("set_warm_policy"):
		_fail("FAIL: endpoint CamBANGDevice lifecycle methods missing")
		return
	if handle_a.set_warm_policy({"warm_hold_ms": 1500}) == OK:
		_fail("FAIL: pre-engage set_warm_policy() must not return OK")
		return
	if str(handle_a.get_hardware_id()) != hardware_id or str(handle_b.get_hardware_id()) != hardware_id:
		_fail("FAIL: endpoint handles must expose matching hardware_id")
		return
	if int(handle_a.get_instance_id()) != 0 or int(handle_b.get_instance_id()) != 0:
		_fail("FAIL: endpoint handles must have instance_id == 0")
		return

	var engage_a_err := ERR_BUSY
	for _engage_i in range(MAX_FRAMES):
		engage_a_err = handle_a.engage()
		if engage_a_err == OK:
			break
		if engage_a_err != ERR_BUSY and engage_a_err != ERR_UNAVAILABLE:
			break
		await get_tree().process_frame
	if engage_a_err != OK:
		_fail("FAIL: endpoint handle engage() must return OK for known synthetic endpoint")
		return

	var engaged_instance_id := int(handle_a.get_instance_id())
	if engaged_instance_id == 0:
		_fail("FAIL: endpoint handle get_instance_id() must become nonzero after engage()")
		return
	if int(handle_b.get_instance_id()) != engaged_instance_id:
		_fail("FAIL: endpoint handles for same hardware_id must resolve the same instance id after engage()")
		return
	if handle_b.engage() != OK:
		_fail("FAIL: second endpoint handle engage() must return OK for already engaged endpoint")
		return
	if int(handle_b.get_instance_id()) != engaged_instance_id:
		_fail("FAIL: second endpoint handle engage() must not change resolved instance id")
		return

	if handle_a.set_warm_policy({"warm_hold_ms": 1500}) != OK:
		_fail("FAIL: set_warm_policy({\"warm_hold_ms\":1500}) must return OK after engage")
		return

	var warm_visible := false
	for _warm_i in range(MAX_FRAMES):
		var warm_snap = CamBANGServer.get_state_snapshot()
		if typeof(warm_snap) == TYPE_DICTIONARY:
			var warm_devices = warm_snap.get("devices", [])
			if typeof(warm_devices) == TYPE_ARRAY:
				for warm_dev in warm_devices:
					if typeof(warm_dev) == TYPE_DICTIONARY and int(warm_dev.get("instance_id", -1)) == engaged_instance_id:
						if int(warm_dev.get("warm_hold_ms", -1)) == 1500 and warm_dev.has("warm_remaining_ms"):
							warm_visible = true
							break
		if warm_visible:
			break
		await get_tree().process_frame
	if not warm_visible:
		_fail("FAIL: snapshot must eventually report warm_hold_ms=1500 for engaged device")
		return

	var stream = handle_a.create_stream()
	if stream == null:
		_fail("FAIL: create_stream() must return a non-null CamBANGStream handle for engaged endpoint")
		return
	if not stream.has_method("get_stream_id") or not stream.has_method("get_device_instance_id") or not stream.has_method("get_hardware_id") or not stream.has_method("is_valid_stream_handle"):
		_fail("FAIL: CamBANGStream handle missing required identity methods")
		return
	if int(stream.get_stream_id()) == 0:
		_fail("FAIL: CamBANGStream.get_stream_id() must be nonzero")
		return
	if int(stream.get_device_instance_id()) != engaged_instance_id:
		_fail("FAIL: CamBANGStream.get_device_instance_id() must match engaged device instance id")
		return
	if str(stream.get_hardware_id()) != hardware_id:
		_fail("FAIL: CamBANGStream.get_hardware_id() must match endpoint hardware_id")
		return
	if not bool(stream.is_valid_stream_handle()):
		_fail("FAIL: CamBANGStream.is_valid_stream_handle() must return true")
		return
	if stream.start() != OK:
		_fail("FAIL: CamBANGStream.start() must return OK")
		return
	if stream.start() != OK:
		_fail("FAIL: CamBANGStream.start() second call must be idempotent and return OK")
		return
	if stream.stop() != OK:
		_fail("FAIL: CamBANGStream.stop() must return OK")
		return
	if stream.stop() != OK:
		_fail("FAIL: CamBANGStream.stop() second call must be idempotent and return OK")
		return
	if stream.destroy() != OK:
		_fail("FAIL: CamBANGStream.destroy() must return OK")
		return
	if stream.destroy() != OK:
		_fail("FAIL: CamBANGStream.destroy() second call must be idempotent and return OK")
		return
	if bool(stream.is_valid_stream_handle()):
		_fail("FAIL: CamBANGStream.is_valid_stream_handle() must return false after destroy")
		return

	stream = null
	if handle_a.disengage() != OK:
		_fail("FAIL: endpoint handle disengage() must return OK")
		return
	if handle_b.disengage() != OK:
		_fail("FAIL: second endpoint handle disengage() must return OK")
		return

	var disengaged_to_zero := false
	for _close_i in range(MAX_FRAMES):
		if int(handle_a.get_instance_id()) == 0 and int(handle_b.get_instance_id()) == 0:
			disengaged_to_zero = true
			break
		await get_tree().process_frame
	if not disengaged_to_zero:
		_fail("FAIL: endpoint handles must eventually resolve get_instance_id() == 0 after disengage close truth")
		return

	handle_a = null
	handle_b = null
	endpoints = []
	CamBANGServer.stop()
	_ok("OK: godot public lifecycle verify PASS")


func _ok(msg: String) -> void:
	if _done:
		return
	_done = true
	print(msg)
	CamBANGServer.stop()
	get_tree().quit(0)


func _fail(msg: String) -> void:
	if _done:
		return
	_done = true
	push_error(msg)
	print(msg)
	CamBANGServer.stop()
	get_tree().quit(1)
