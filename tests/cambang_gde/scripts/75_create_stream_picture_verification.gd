extends Node

## Scene 75: create_stream() picture-extension public-API verification.
##
## The public create_stream definition gained an optional `picture` sub-dict
## (pattern seed + overlay toggles), forwarded to StreamRequest.picture and gated
## on the provider's supports_stream_picture_updates(). This verifies the parse
## contract on the synthetic provider (which supports it):
## - a valid picture (seed + overlays) is accepted (non-null stream),
## - a picture with an unknown key is rejected (null),
## - a picture with a wrong-typed field is rejected (null).
##
## The capability gate itself -- platform providers rejecting a picture because
## supports_stream_picture_updates() is false -- is exercised on the platform
## surfaces (Scene 870 platform-backed runs), since synthetic supports it here.
##
## Synthetic-only, headless, self-terminating: emits [CamBANG][HarnessVerdict].

const SCENE_LABEL := "75_create_stream_picture_verification"
const HW_A := "synthetic:0"

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


func _base_profile() -> Dictionary:
	return {
		"width": 640,
		"height": 360,
		"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
	}


func _run() -> void:
	CamBANGServer.stop()
	if int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)) != OK:
		_fail("start(synthetic) failed")
		return
	await _wait_snapshot(2000)
	var dev = await _engage_live(HW_A, 4000)
	if dev == null:
		_fail("device %s did not become live" % HW_A)
		return

	# A) valid picture (seed + both overlays) is accepted.
	var ok_stream = dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": _base_profile(),
		"picture": {"seed": 0x0751, "overlay_moving_bar": false, "overlay_frame_index_offsets": true},
	})
	if ok_stream == null:
		_fail("create_stream with a valid picture(seed+overlays) returned null on synthetic")
		return

	# B) unknown key in picture -> rejected (the parse whitelist is strict).
	if dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": _base_profile(),
		"picture": {"bogus_key": 1},
	}) != null:
		_fail("create_stream accepted a picture with an unknown key")
		return

	# C) wrong-typed field -> rejected.
	if dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": _base_profile(),
		"picture": {"seed": "not_an_integer"},
	}) != null:
		_fail("create_stream accepted a non-integer picture seed")
		return

	_emit("ok", "synthetic accepts picture(seed+overlays); rejects unknown-key and wrong-typed picture")
