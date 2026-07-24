extends Node

## Scene 75: picture-extension public-API verification (stream + capture).
##
## Two public entry points gained the full PictureConfig (preset by canonical
## token, seed, generator cadence, overlays, solid colour, checker size):
## - create_stream()'s optional `picture` sub-dict -> StreamRequest.picture, gated
##   on supports_stream_picture_updates(),
## - CamBANGDevice.set_capture_picture() -> device capture picture, gated on
##   supports_capture_picture_updates().
##
## Verified on the synthetic provider (which supports both): a valid full picture
## is accepted; an unknown preset token, an unknown key, and an out-of-range solid
## colour are each rejected; set_capture_picture accepts a valid picture and
## rejects an unknown preset. The capability gate itself (platform providers
## rejecting a picture) is exercised on the platform surfaces (Scene 870).
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

	# --- create_stream picture (full PictureConfig) ---------------------------
	# A) valid full stream picture (preset by name + seed + colour + checker).
	var ok_stream = dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW,
		"profile": _base_profile(),
		"picture": {"preset": "checker", "seed": 0x0751, "solid_r": 255, "solid_g": 64,
			"solid_b": 64, "solid_a": 255, "checker_size_px": 24, "overlay_moving_bar": false},
	})
	if ok_stream == null:
		_fail("create_stream with a valid full picture returned null on synthetic")
		return

	# B) unknown preset token -> rejected.
	if dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW, "profile": _base_profile(),
		"picture": {"preset": "no_such_preset"},
	}) != null:
		_fail("create_stream accepted an unknown preset token")
		return

	# C) unknown key -> rejected (the parse whitelist is strict).
	if dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW, "profile": _base_profile(),
		"picture": {"bogus_key": 1},
	}) != null:
		_fail("create_stream accepted a picture with an unknown key")
		return

	# D) out-of-range solid colour (>255) -> rejected.
	if dev.create_stream({
		"intent": CamBANGStream.INTENT_PREVIEW, "profile": _base_profile(),
		"picture": {"solid_r": 999},
	}) != null:
		_fail("create_stream accepted an out-of-range solid colour")
		return

	# --- set_capture_picture (device-scoped) ----------------------------------
	# E) a valid full capture picture is accepted.
	if int(dev.set_capture_picture({"preset": "solid", "seed": 0x0752,
			"solid_r": 92, "solid_g": 44, "solid_b": 152, "solid_a": 255})) != OK:
		_fail("set_capture_picture rejected a valid picture on synthetic")
		return

	# F) an unknown preset token is rejected.
	if int(dev.set_capture_picture({"preset": "no_such_preset"})) == OK:
		_fail("set_capture_picture accepted an unknown preset token")
		return

	_emit("ok", "create_stream + set_capture_picture accept full pictures; reject unknown preset/key/out-of-range")
