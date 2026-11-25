extends Node

const SCENE := "83_retained_capture_sampling"
const SHADER: RDShaderSPIRV = preload("res://shaders/retained_capture_sample.tres")
const WAIT_MS := 10000
const SAVE_PATH := "user://retained_capture.png"
const PREVIEW_SIZE := Vector2(512, 512)
const REGIONS := [Rect2(0.18, 0.33, 0.10, 0.10), Rect2(0.67, 0.54, 0.10, 0.10)]

@export var hardware_id := "50"
var _device: CamBANGDevice
var _backing: CamBANGGpuBacking
var _colorimetry := {}
var _early := {}
var _finished := {}
var _claim_on_ready := false
var _ready_capture_id := ""
var _first_sample := {}
var _save_task := -1
var _regions: Array[Rect2] = []
var _last_output := PackedByteArray()
var _compute_us: Array[int] = []
var _capture_us: Array[int] = []
var _frame_ms: Array[float] = []
var _performance_window := false
var _xr_active := false
var _busy := true
var _verdict := false
var _panel: Control
var _preview: TextureRect
var _markers: Array[ReferenceRect] = []
var _swatches: Array[ColorRect] = []
var _status: Label
var _refresh: Button
var _next_region := 0


func _ready() -> void:
	_make_panel()
	var want_xr := bool(ProjectSettings.get_setting("xr/openxr/enabled", false))
	if want_xr and not _enable_xr():
		_fail("OpenXR was requested but did not initialize")
		return
	if OS.get_name() != "Android":
		_fail("Select an Android Camera2 device with retained backing support")
		return
	CamBANGServer.capture_image_acquired.connect(_on_acquired)
	CamBANGServer.capture_finished.connect(_on_finished)
	CamBANGServer.capture_gpu_backing_ready.connect(_on_gpu_backing_ready)
	await _run()


func _make_panel() -> void:
	_panel = Control.new()
	_panel.size = Vector2(920, 620)
	add_child(_panel)
	var background := ColorRect.new()
	background.size = _panel.size
	background.color = Color("#111922")
	_panel.add_child(background)
	var title := Label.new()
	title.text = "Retained capture sampling"
	title.position = Vector2(20, 14)
	title.add_theme_font_size_override("font_size", 28)
	_panel.add_child(title)
	_preview = TextureRect.new()
	_preview.position = Vector2(20, 65)
	_preview.size = PREVIEW_SIZE
	_preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_preview.gui_input.connect(_pick_region)
	_panel.add_child(_preview)
	_status = Label.new()
	_status.position = Vector2(555, 72)
	_status.text = "Opening camera…"
	_panel.add_child(_status)
	_refresh = Button.new()
	_refresh.position = Vector2(555, 458)
	_refresh.text = "Sample both regions again"
	_refresh.disabled = true
	_refresh.pressed.connect(_sample_regions_again)
	_panel.add_child(_refresh)
	for i in range(REGIONS.size()):
		_regions.append(REGIONS[i])
		var marker := ReferenceRect.new()
		marker.editor_only = false
		marker.border_color = Color("#49dbd4") if i == 0 else Color("#ffbd6d")
		marker.border_width = 3
		marker.mouse_filter = Control.MOUSE_FILTER_IGNORE
		_preview.add_child(marker)
		_markers.append(marker)
		var swatch := ColorRect.new()
		swatch.position = Vector2(555 + 152 * i, 272)
		swatch.size = Vector2(130, 120)
		_panel.add_child(swatch)
		_swatches.append(swatch)
		var label := Label.new()
		label.position = swatch.position + Vector2(0, 125)
		label.text = "Region %s" % ["A" if i == 0 else "B"]
		label.modulate = marker.border_color
		_panel.add_child(label)
	_update_markers()


func _enable_xr() -> bool:
	var interface := XRServer.find_interface("OpenXR")
	if interface == null or (not interface.is_initialized() and not interface.initialize()):
		return false
	_xr_active = true
	get_viewport().use_xr = true
	var origin := XROrigin3D.new()
	add_child(origin)
	var camera := XRCamera3D.new()
	origin.add_child(camera)
	var surface := SubViewport.new()
	surface.size = Vector2i(920, 620)
	surface.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(surface)
	_panel.reparent(surface, false)
	var image_panel := Sprite3D.new()
	image_panel.texture = surface.get_texture()
	image_panel.pixel_size = 0.0015
	image_panel.position = Vector3(0, 1.40, -1.55)
	add_child(image_panel)
	return true


func _on_acquired(id: String, info: Dictionary) -> void:
	if int(info.get("image_member_index", -1)) == 0 and not _finished.has(id):
		_early[id] = info.get("acquisition_timing", {})


func _on_finished(id: String, _info: Dictionary) -> void:
	_finished[id] = true


func _on_gpu_backing_ready(id: String, info: Dictionary) -> void:
	if not _claim_on_ready:
		return
	var claim := CamBANGServer.claim_capture_gpu_backing(id, int(info["image_member_index"]))
	if int(claim["error"]) != OK:
		_fail("GPU backing claim from readiness signal failed: %s" % [claim])
		return
	_ready_capture_id = id
	_colorimetry = info["colorimetry"]
	_backing = claim["backing"]
	# The ready handler can start GPU work before the CPU result is delivered.
	_first_sample = _submit_sample(0)
	if not _first_sample.is_empty():
		print("Submitted region A from GPU readiness for %s member 0" % id)


func _process(delta: float) -> void:
	if _performance_window:
		_frame_ms.append(delta * 1000.0)


func _capture(keep: bool) -> CamBANGCaptureResult:
	var start := Time.get_ticks_usec()
	var deadline := Time.get_ticks_msec() + WAIT_MS
	var request: Dictionary = {}
	while Time.get_ticks_msec() < deadline:
		request = _device.trigger_capture({"keep_gpu_backing": true}) if keep else _device.trigger_capture()
		if int(request.get("error", FAILED)) != ERR_BUSY:
			break
		await get_tree().process_frame
	if int(request.get("error", FAILED)) != OK:
		_fail("Capture trigger failed: %s" % [request])
		return null
	var id := str(request["id"])
	while not _finished.has(id) and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	var result := CamBANGServer.get_capture_result_by_id(id)
	if result == null or not _finished.has(id):
		_fail("No completed still for %s" % id)
		return null
	var timing: Dictionary = result.get_image_member(0).get("camera_facts", {}).get("acquisition_timing", {})
	if timing.is_empty() or _early.get(id, {}) != timing:
		_fail("Early timing did not match the captured member: %s" % id)
		return null
	_capture_us.append(Time.get_ticks_usec() - start)
	return result


func _run() -> void:
	if CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED) != OK:
		_fail("Could not start Camera2")
		return
	var deadline := Time.get_ticks_msec() + WAIT_MS
	while CamBANGServer.get_state_snapshot() == null and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	_device = CamBANGServer.get_device_for_hardware_id(hardware_id)
	if _device == null:
		_fail("Camera endpoint %s was not available" % hardware_id)
		return
	var error := _device.engage()
	while error == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		error = _device.engage()
	if error != OK:
		_fail("Camera engage failed: %d" % error)
		return
	error = _device.set_still_capture_profile(
		{"width": 1280, "height": 1280, "format_fourcc": CamBANGServer.PIXEL_FORMAT_NV12})
	if error != OK:
		_fail("Still profile refused: %d" % error)
		return
	while int(_device.get_still_capture_profile().get("width", 0)) != 1280 and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	_performance_window = true
	var baseline := await _capture(false)
	if baseline == null:
		return
	if baseline.to_image() == null or int(baseline.claim_gpu_backing()["error"]) != ERR_UNAVAILABLE:
		_fail("Ordinary capture behavior changed")
		return
	_claim_on_ready = true
	var retained := await _capture(true)
	_claim_on_ready = false
	if retained == null:
		return
	if _backing == null or _ready_capture_id != str(retained.get_capture_identity()["device_capture_id"]):
		_fail("GPU readiness did not identify the completed capture")
		return
	if int(retained.claim_gpu_backing()["error"]) != ERR_ALREADY_IN_USE:
		_fail("The ordinary result granted a second claim on the same backing")
		return
	var id := str(retained.get_capture_identity()["device_capture_id"])
	var timing: Dictionary = _early[id]
	var saved := {}
	_save_task = WorkerThreadPool.add_task(func():
		var image := retained.to_image()
		saved["image"] = image
		saved["error"] = ERR_UNAVAILABLE if image == null else image.save_png(SAVE_PATH))
	_status.text = "Capture %s\nEarly acquisition matched member 0\nSampling and saving…" % id
	if _first_sample.is_empty():
		return
	var first: Array[PackedByteArray] = await _sample_both(_first_sample)
	_first_sample.clear()
	if first.size() != REGIONS.size():
		return
	while not WorkerThreadPool.is_task_completed(_save_task):
		await get_tree().process_frame
	WorkerThreadPool.wait_for_task_completion(_save_task)
	_save_task = -1
	if int(saved.get("error", FAILED)) != OK:
		_fail("Saving %s failed (%s)" % [SAVE_PATH, saved.get("error", FAILED)])
		return
	_preview.texture = ImageTexture.create_from_image(saved["image"])
	print("Saved %s member 0 (acquisition mark %s) to %s" %
		[id, timing["acquisition_mark"], ProjectSettings.globalize_path(SAVE_PATH)])
	var next := await _capture(false)
	if next == null:
		return
	if next.to_image() == null:
		_fail("Later ordinary capture lost its pixels")
		return
	var repeated: Array[PackedByteArray] = await _sample_both()
	if repeated != first:
		_fail("The first backing changed after another capture")
		return
	_performance_window = false
	_busy = false
	_refresh.disabled = false
	_status.text = ("Capture %s\nMember 0 acquisition mark: %s\n"
		+ "Saved %s\nBoth colours matched after a later still\n\nClick the image to move a region.") % [
			id, timing["acquisition_mark"], SAVE_PATH]
	var renderer := RenderingServer.get_current_rendering_method()
	print("Retained capture metrics renderer=%s openxr=%s capture_us=%s compute_us=%s frame_ms=%s" %
		[renderer, _xr_active, _capture_us, _compute_us, _frame_summary()])
	if OS.has_feature("cambang_verify_backing") and not await _verify_lifecycle(first[0]):
		return
	_verdict = true
	print("[CamBANG][HarnessVerdict] scene=%s status=ok exit_code=0 reason=renderer_%s_openxr_%s" %
		[SCENE, renderer, _xr_active])


func _submit_check(backing: CamBANGGpuBacking, shader: RDShaderSPIRV, answer: Dictionary) -> int:
	return backing.submit_compute(shader, _parameters(_regions[0]), Vector3i.ONE, 16,
		func(error: int, bytes: PackedByteArray):
			answer["error"] = error
			answer["bytes"] = bytes)


func _wait_check(answer: Dictionary) -> int:
	var end := Time.get_ticks_msec() + WAIT_MS
	while not answer.has("error") and Time.get_ticks_msec() < end:
		await get_tree().process_frame
	return int(answer.get("error", ERR_TIMEOUT))


func _verify_lifecycle(expected: PackedByteArray) -> bool:
	_busy = true
	_refresh.disabled = true
	var invalid := RDShaderSPIRV.new()
	invalid.set_stage_bytecode(RenderingDevice.SHADER_STAGE_COMPUTE,
		PackedInt32Array([0x07230203, 0x00010300, 0, 1, 0]).to_byte_array())
	var rejected := {}
	if _submit_check(_backing, invalid, rejected) != OK or await _wait_check(rejected) != ERR_INVALID_PARAMETER:
		_fail("Malformed shader did not fail validation before dispatch")
		return false

	var after_release := {}
	if _submit_check(_backing, SHADER, after_release) != OK:
		_fail("Could not submit the backing release check")
		return false
	_backing = null
	if await _wait_check(after_release) != OK or after_release.get("bytes") != expected:
		_fail("Accepted work lost the backing when the final application reference was dropped")
		return false

	var unclaimed := await _capture(true)
	if unclaimed == null:
		return false
	await get_tree().create_timer(5.5).timeout
	if int(unclaimed.claim_gpu_backing()["error"]) != ERR_TIMEOUT or unclaimed.to_image() == null:
		_fail("Unclaimed backing expiry affected the ordinary capture")
		return false

	var last := await _capture(true)
	if last == null:
		return false
	var receipt := last.claim_gpu_backing()
	if int(receipt["error"]) != OK:
		_fail("No backing for shutdown check")
		return false
	var at_stop: CamBANGGpuBacking = receipt["backing"]
	var queued := {}
	if _submit_check(at_stop, SHADER, queued) != OK:
		_fail("Could not submit the shutdown check")
		return false
	var start := Time.get_ticks_usec()
	CamBANGServer.stop()
	var stop_us := Time.get_ticks_usec() - start
	var outcome := await _wait_check(queued)
	if outcome != OK and outcome != ERR_SKIP:
		_fail("Accepted compute did not settle when CamBANG stopped: %d" % outcome)
		return false
	if _submit_check(at_stop, SHADER, {}) != ERR_UNAVAILABLE:
		_fail("A stopped provider accepted more compute")
		return false
	print("STEP OK: shader rejection, pinned dispatch, expiry, and orderly stop (stop_us=%d)" % stop_us)
	return true


func _parameters(region: Rect2) -> PackedByteArray:
	var kr := 0.299
	var kb := 0.114
	match str(_colorimetry.get("matrix", "")).to_lower():
		"bt709":
			kr = 0.2126
			kb = 0.0722
		"bt2020_ncl":
			kr = 0.2627
			kb = 0.0593
	# The demo explicitly assumes limited-range BT.601 if the camera omits colour metadata.
	var full := str(_colorimetry.get("range", "")).to_lower() == "full"
	return PackedFloat32Array([region.position.x, region.position.y, region.size.x, region.size.y,
		kr, kb, 0.0 if full else 16.0 / 255.0, 1.0 if full else 219.0 / 255.0,
		1.0 if full else 224.0 / 255.0]).to_byte_array()


func _submit_sample(index: int) -> Dictionary:
	var result := {"done": false, "error": FAILED, "bytes": PackedByteArray(),
		"start": Time.get_ticks_usec()}
	var admitted := _backing.submit_compute(SHADER, _parameters(_regions[index]), Vector3i.ONE, 16,
		func(error: int, bytes: PackedByteArray):
			result["error"] = error
			result["bytes"] = bytes
			result["elapsed_us"] = Time.get_ticks_usec() - int(result["start"])
			result["done"] = true)
	if admitted != OK:
		_fail("Compute request refused: %d" % admitted)
		return {}
	return result


func _sample_both(first_sample: Dictionary = {}) -> Array[PackedByteArray]:
	var outputs: Array[PackedByteArray] = []
	for i in range(_regions.size()):
		var result := first_sample if i == 0 and not first_sample.is_empty() else _submit_sample(i)
		if result.is_empty():
			return []
		var deadline := Time.get_ticks_msec() + WAIT_MS
		while not result["done"] and Time.get_ticks_msec() < deadline:
			await get_tree().process_frame
		_compute_us.append(int(result["elapsed_us"]))
		var bytes: PackedByteArray = result["bytes"]
		if int(result["error"]) != OK or bytes.size() != 16 or bytes.decode_float(12) != 1.0:
			_fail("Compute result invalid: %d (%d bytes)" % [int(result["error"]), bytes.size()])
			return []
		var channels := [bytes.decode_float(0), bytes.decode_float(4), bytes.decode_float(8)]
		for channel in channels:
			if not is_finite(channel) or channel < 0.0 or channel > 1.0:
				_fail("The caller shader returned a colour outside its declared range")
				return []
		_swatches[i].color = Color(channels[0], channels[1], channels[2])
		print("Retained capture region %d RGB %s" % [i, channels])
		outputs.append(bytes)
	return outputs


func _update_markers() -> void:
	for i in range(_regions.size()):
		_markers[i].position = _regions[i].position * PREVIEW_SIZE
		_markers[i].size = _regions[i].size * PREVIEW_SIZE


func _pick_region(event: InputEvent) -> void:
	if _busy or not (event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT):
		return
	var old := _regions[_next_region]
	var corner: Vector2 = (event.position / PREVIEW_SIZE - old.size * 0.5).clamp(Vector2.ZERO, Vector2.ONE - old.size)
	_regions[_next_region] = Rect2(corner, old.size)
	_next_region = (_next_region + 1) % _regions.size()
	_update_markers()
	await _sample_regions_again()


func _sample_regions_again() -> void:
	if _busy:
		return
	_busy = true
	_refresh.disabled = true
	await _sample_both()
	_refresh.disabled = false
	_busy = false


func _frame_summary() -> Dictionary:
	_frame_ms.sort()
	if _frame_ms.is_empty():
		return {}
	return {"count": _frame_ms.size(), "p50": _frame_ms[int(_frame_ms.size() * 0.5)],
		"p95": _frame_ms[int((_frame_ms.size() - 1) * 0.95)], "max": _frame_ms.back()}


func _fail(message: String) -> void:
	_status.text = message
	if not _verdict:
		_verdict = true
		print("[CamBANG][HarnessVerdict] scene=%s status=fail exit_code=1 reason=%s" %
			[SCENE, message.replace(" ", "_")])
		CamBANGServer.stop()
		get_tree().quit(1)


func _exit_tree() -> void:
	if _save_task != -1:
		WorkerThreadPool.wait_for_task_completion(_save_task)
	_backing = null
	CamBANGServer.stop()
