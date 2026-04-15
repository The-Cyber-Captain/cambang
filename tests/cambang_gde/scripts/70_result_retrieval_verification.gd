extends Control

# Tranche-1 result verification scope:
# - proves CamBANGStreamResult from Godot boundary
# - proves CamBANGCaptureResult from Godot boundary
# - proves grouped facts/provenance accessor shape (Dictionary)
# - proves to_image() for both
# Non-goals intentionally preserved:
# - no CamBANGRig.trigger_sync_capture()
# - no CamBANGCaptureResultSet realization proof
# - no grouped-capture curation policy proof
# - no mailbox coupling

const QUIT_FLUSH_FRAMES := 2
const STREAM_TIMEOUT_MS := 4000
const CAPTURE_TIMEOUT_MS := 4000
const INSPECTION_CAPTURE_TIMEOUT_MS := 4000
const TOTAL_TIMEOUT_MS := 10000
const PAYLOAD_KIND_CPU_PACKED := 0
const PAYLOAD_KIND_GPU_SURFACE := 2

@onready var _status_label: RichTextLabel = $RootMargin/MainColumn/StatusLabel
@onready var _stream_facts_label: Label = $RootMargin/MainColumn/ResultsRow/StreamPanel/StreamFacts
@onready var _capture_facts_label: Label = $RootMargin/MainColumn/ResultsRow/CapturePanel/CaptureFacts
@onready var _stream_texture_rect: TextureRect = $RootMargin/MainColumn/ResultsRow/StreamPanel/StreamTexture
@onready var _capture_texture_rect: TextureRect = $RootMargin/MainColumn/ResultsRow/CapturePanel/CaptureTexture
@onready var _gui_controls: HBoxContainer = $RootMargin/MainColumn/GuiControls
@onready var _capture_again_button: Button = $RootMargin/MainColumn/GuiControls/CaptureAgainButton

var _step := 0
var _done := false
var _quit_requested := false
var _inspection_mode := false
var _is_headless := false
var _start_ms := 0
var _stream_poll_start_ms := 0
var _capture_poll_start_ms := 0
var _inspection_capture_poll_start_ms := 0

var _device_instance_id := 0
var _stream_id := 0
var _capture_id := 0
var _inspection_capture_id := 0

func _ready() -> void:
	_status_label.clear()
	_is_headless = DisplayServer.get_name() == "headless"
	_start_ms = Time.get_ticks_msec()
	_stream_poll_start_ms = _start_ms
	set_process(true)
	set_process_unhandled_input(true)

	if _is_headless:
		_gui_controls.visible = false
	else:
		_capture_again_button.pressed.connect(_on_capture_again_pressed)
		_capture_again_button.disabled = true

	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	_require(start_err == OK, "step %d FAIL: synthetic timeline start rejected (%d)" % [_step, start_err])
	_step_ok("bootstrap synthetic runtime started")

	var stage_err := CamBANGServer.select_builtin_scenario("stream_inspection_live")
	_require(stage_err == OK, "step %d FAIL: unable to stage stream_inspection_live" % _step)
	_step_ok("bootstrap scenario staged")

	var scenario_start_err := CamBANGServer.start_scenario()
	_require(scenario_start_err == OK, "step %d FAIL: unable to start staged scenario" % _step)
	_step_ok("bootstrap scenario started")

	_append_status("RUN: result_retrieval_verification")


func _process(_delta: float) -> void:
	if _done:
		return
	if _inspection_mode:
		if _stream_texture_rect.texture == null:
			_ensure_stream_panel_display_view_bound()
		_poll_inspection_capture_result()
		return

	if Time.get_ticks_msec() - _start_ms > TOTAL_TIMEOUT_MS:
		_fail("step %d FAIL: total verification timeout" % _step)
		return

	if _stream_id == 0 or _device_instance_id == 0:
		_try_latch_ids_from_snapshot()
		return

	if _capture_id == 0:
		if Time.get_ticks_msec() - _stream_poll_start_ms > STREAM_TIMEOUT_MS:
			_fail("step %d FAIL: latest stream result did not appear within timeout" % _step)
			return
		_try_verify_stream_result()
		return

	if Time.get_ticks_msec() - _capture_poll_start_ms > CAPTURE_TIMEOUT_MS:
		_fail("step %d FAIL: capture result did not appear within timeout" % _step)
		return
	_try_verify_capture_result()


func _unhandled_input(event: InputEvent) -> void:
	if _is_headless:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		var key_event := event as InputEventKey
		if key_event.keycode == KEY_ESCAPE:
			_append_status("INFO: Esc pressed; quitting scene")
			_cleanup_and_quit(0)
		elif key_event.keycode == KEY_C and _inspection_mode:
			_request_manual_capture()


func _try_latch_ids_from_snapshot() -> void:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return
	var devices: Array = snapshot.get("devices", [])
	var streams: Array = snapshot.get("streams", [])
	if devices.is_empty() or streams.is_empty():
		return

	var device_d: Dictionary = devices[0]
	var stream_d: Dictionary = streams[0]
	var latched_device_id := int(device_d.get("instance_id", 0))
	var latched_stream_id := int(stream_d.get("stream_id", 0))
	_require(latched_device_id > 0, "step %d FAIL: latched device instance id is invalid" % _step)
	_require(latched_stream_id > 0, "step %d FAIL: latched stream id is invalid" % _step)

	_device_instance_id = latched_device_id
	_stream_id = latched_stream_id
	_step_ok("bootstrap identifiers latched (device_instance_id=%d stream_id=%d)" % [_device_instance_id, _stream_id])


func _try_verify_stream_result() -> void:
	var stream_result = CamBANGServer.get_latest_stream_result(_stream_id)
	if stream_result == null:
		return

	_require(stream_result.get_class() == "CamBANGStreamResult", "step %d FAIL: stream result must be CamBANGStreamResult" % _step)
	_step_ok("stream result object branding verified")

	_require(stream_result.get_width() > 0, "step %d FAIL: stream width invalid" % _step)
	_require(stream_result.get_height() > 0, "step %d FAIL: stream height invalid" % _step)
	_require(stream_result.get_format() != 0, "step %d FAIL: stream format invalid" % _step)
	var stream_payload_kind := int(stream_result.get_payload_kind())
	_require(
		stream_payload_kind == PAYLOAD_KIND_CPU_PACKED or stream_payload_kind == PAYLOAD_KIND_GPU_SURFACE,
		"step %d FAIL: stream payload_kind must be CPU_PACKED or GPU_SURFACE" % _step
	)
	_require(stream_result.get_stream_id() == _stream_id, "step %d FAIL: stream_id mismatch" % _step)
	_require(stream_result.get_device_instance_id() == _device_instance_id, "step %d FAIL: stream device_instance_id mismatch" % _step)
	_step_ok("stream direct properties verified")

	_require(typeof(stream_result.get_image_properties()) == TYPE_DICTIONARY, "step %d FAIL: stream image_properties accessor must return Dictionary" % _step)
	_require(typeof(stream_result.get_capture_attributes()) == TYPE_DICTIONARY, "step %d FAIL: stream capture_attributes accessor must return Dictionary" % _step)
	_require(typeof(stream_result.get_location_attributes()) == TYPE_DICTIONARY, "step %d FAIL: stream location_attributes accessor must return Dictionary" % _step)
	_require(typeof(stream_result.get_optical_calibration()) == TYPE_DICTIONARY, "step %d FAIL: stream optical_calibration accessor must return Dictionary" % _step)
	_step_ok("stream grouped fact accessors verified (Dictionary)")

	_require(typeof(stream_result.get_image_properties_provenance()) == TYPE_DICTIONARY, "step %d FAIL: stream image_properties_provenance must return Dictionary" % _step)
	_require(typeof(stream_result.get_capture_attributes_provenance()) == TYPE_DICTIONARY, "step %d FAIL: stream capture_attributes_provenance must return Dictionary" % _step)
	_require(typeof(stream_result.get_location_attributes_provenance()) == TYPE_DICTIONARY, "step %d FAIL: stream location_attributes_provenance must return Dictionary" % _step)
	_require(typeof(stream_result.get_optical_calibration_provenance()) == TYPE_DICTIONARY, "step %d FAIL: stream optical_calibration_provenance must return Dictionary" % _step)
	_step_ok("stream provenance grouped accessors verified (Dictionary)")

	var stream_can_to_image := int(stream_result.can_to_image())
	_require(stream_can_to_image != int(stream_result.CAPABILITY_UNSUPPORTED), "step %d FAIL: stream can_to_image() unexpectedly unsupported" % _step)
	_step_ok("stream can_to_image capability verified")

	# to_image() is explicit materialization onto CPU-backed storage; it is not
	# the normal live display path.
	var stream_image: Image = stream_result.to_image()
	_require(stream_image != null, "step %d FAIL: stream to_image() returned null" % _step)
	_require(stream_image.get_width() > 0 and stream_image.get_height() > 0, "step %d FAIL: stream image dimensions invalid" % _step)
	_step_ok("stream to_image materialization verified")

	var stream_display_view = stream_result.get_display_view()
	if stream_payload_kind == PAYLOAD_KIND_GPU_SURFACE:
		_require(stream_display_view is Texture2D, "step %d FAIL: GPU_SURFACE stream display_view must be Texture2D" % _step)
	else:
		_require(stream_display_view is Texture2D, "step %d FAIL: CPU_PACKED stream display_view must be Texture2D" % _step)
	_step_ok("stream display_view path verified")

	_ensure_stream_panel_display_view_bound(stream_result, true)
	_step_ok("stream image displayed")

	var device = CamBANGServer.get_device(_device_instance_id)
	_require(device != null, "step %d FAIL: CamBANGServer.get_device() returned null" % _step)
	_require(device.get_class() == "CamBANGDevice", "step %d FAIL: get_device() must return CamBANGDevice" % _step)
	_step_ok("device seam verified")

	_capture_id = int(device.trigger_capture())
	_require(_capture_id != 0, "step %d FAIL: trigger_capture() returned zero capture id" % _step)
	_step_ok("capture trigger accepted (capture_id=%d)" % _capture_id)
	_capture_poll_start_ms = Time.get_ticks_msec()


func _try_verify_capture_result() -> void:
	var capture_result = CamBANGServer.get_capture_result(_capture_id, _device_instance_id)
	if capture_result == null:
		return

	_require(capture_result.get_class() == "CamBANGCaptureResult", "step %d FAIL: capture result must be CamBANGCaptureResult" % _step)
	_step_ok("capture result object branding verified")

	_require(capture_result.get_width() > 0, "step %d FAIL: capture width invalid" % _step)
	_require(capture_result.get_height() > 0, "step %d FAIL: capture height invalid" % _step)
	_require(capture_result.get_format() != 0, "step %d FAIL: capture format invalid" % _step)
	_require(capture_result.get_payload_kind() == PAYLOAD_KIND_CPU_PACKED, "step %d FAIL: tranche-1 capture payload_kind must be CPU_PACKED" % _step)
	_require(capture_result.get_capture_id() == _capture_id, "step %d FAIL: capture_id mismatch" % _step)
	_require(capture_result.get_device_instance_id() == _device_instance_id, "step %d FAIL: capture device_instance_id mismatch" % _step)
	_step_ok("capture direct properties verified")

	_require(typeof(capture_result.get_image_properties()) == TYPE_DICTIONARY, "step %d FAIL: capture image_properties accessor must return Dictionary" % _step)
	_require(typeof(capture_result.get_capture_attributes()) == TYPE_DICTIONARY, "step %d FAIL: capture capture_attributes accessor must return Dictionary" % _step)
	_require(typeof(capture_result.get_location_attributes()) == TYPE_DICTIONARY, "step %d FAIL: capture location_attributes accessor must return Dictionary" % _step)
	_require(typeof(capture_result.get_optical_calibration()) == TYPE_DICTIONARY, "step %d FAIL: capture optical_calibration accessor must return Dictionary" % _step)
	_step_ok("capture grouped fact accessors verified (Dictionary)")

	_require(typeof(capture_result.get_image_properties_provenance()) == TYPE_DICTIONARY, "step %d FAIL: capture image_properties_provenance must return Dictionary" % _step)
	_require(typeof(capture_result.get_capture_attributes_provenance()) == TYPE_DICTIONARY, "step %d FAIL: capture capture_attributes_provenance must return Dictionary" % _step)
	_require(typeof(capture_result.get_location_attributes_provenance()) == TYPE_DICTIONARY, "step %d FAIL: capture location_attributes_provenance must return Dictionary" % _step)
	_require(typeof(capture_result.get_optical_calibration_provenance()) == TYPE_DICTIONARY, "step %d FAIL: capture optical_calibration_provenance must return Dictionary" % _step)
	_step_ok("capture provenance grouped accessors verified (Dictionary)")

	var capture_can_to_image := int(capture_result.can_to_image())
	_require(capture_can_to_image != int(capture_result.CAPABILITY_UNSUPPORTED), "step %d FAIL: capture can_to_image() unexpectedly unsupported" % _step)
	_step_ok("capture can_to_image capability verified")

	# Tranche-1 honesty: encoded-byte access can remain unsupported.
	var encoded_capability := int(capture_result.can_get_encoded_bytes())
	if encoded_capability == int(capture_result.CAPABILITY_UNSUPPORTED):
		_append_status("INFO: capture can_get_encoded_bytes() is UNSUPPORTED in tranche 1 (acceptable)")
	else:
		_append_status("INFO: capture can_get_encoded_bytes() capability=%d" % encoded_capability)

	var capture_image: Image = capture_result.to_image()
	_require(capture_image != null, "step %d FAIL: capture to_image() returned null" % _step)
	_require(capture_image.get_width() > 0 and capture_image.get_height() > 0, "step %d FAIL: capture image dimensions invalid" % _step)
	_step_ok("capture to_image materialization verified")

	_capture_texture_rect.texture = ImageTexture.create_from_image(capture_image)
	_capture_facts_label.text = "payload_kind=%d\nsize=%dx%d\ncapture_id=%d\nmode=initial verification" % [
		capture_result.get_payload_kind(),
		capture_result.get_width(),
		capture_result.get_height(),
		capture_result.get_capture_id()
	]
	_step_ok("capture image displayed")
	_ok("OK: result_retrieval_verification passed")


func _ensure_stream_panel_display_view_bound(stream_result = null, force_rebind: bool = false) -> void:
	var latest_stream_result = stream_result
	if latest_stream_result == null:
		if _stream_id == 0:
			return
		latest_stream_result = CamBANGServer.get_latest_stream_result(_stream_id)
	if latest_stream_result == null:
		return
	if int(latest_stream_result.can_get_display_view()) == int(latest_stream_result.CAPABILITY_UNSUPPORTED):
		return

	# get_display_view() is a display-oriented live view of current retained
	# stream state. For GPU-backed paths this is buffer-like/live, so user code
	# should bind it rather than rebinding every frame to force updates.
	var stream_display_view = latest_stream_result.get_display_view()
	if not (stream_display_view is Texture2D):
		return
	var current_texture = _stream_texture_rect.texture
	if force_rebind or current_texture == null or current_texture != stream_display_view:
		_stream_texture_rect.texture = stream_display_view
	_stream_facts_label.text = "payload_kind=%d\nsize=%dx%d\nstream_id=%d\n(live display view bound)" % [
		latest_stream_result.get_payload_kind(),
		latest_stream_result.get_width(),
		latest_stream_result.get_height(),
		latest_stream_result.get_stream_id()
	]


func _on_capture_again_pressed() -> void:
	_request_manual_capture()


func _request_manual_capture() -> void:
	if _is_headless or not _inspection_mode:
		return
	if _inspection_capture_id != 0:
		_append_status("INFO: capture already pending; wait for completion")
		return
	var device = CamBANGServer.get_device(_device_instance_id)
	if device == null:
		_append_status("WARN: cannot capture again; device unavailable")
		return
	_inspection_capture_id = int(device.trigger_capture())
	if _inspection_capture_id == 0:
		_append_status("WARN: manual capture request rejected (capture_id=0)")
		return
	_inspection_capture_poll_start_ms = Time.get_ticks_msec()
	_append_status("INFO: manual capture requested (capture_id=%d)" % _inspection_capture_id)


func _poll_inspection_capture_result() -> void:
	if _inspection_capture_id == 0:
		return
	if Time.get_ticks_msec() - _inspection_capture_poll_start_ms > INSPECTION_CAPTURE_TIMEOUT_MS:
		_append_status("WARN: manual capture timeout for capture_id=%d" % _inspection_capture_id)
		_inspection_capture_id = 0
		return
	var capture_result = CamBANGServer.get_capture_result(_inspection_capture_id, _device_instance_id)
	if capture_result == null:
		return
	var capture_image: Image = capture_result.to_image()
	if capture_image == null:
		_append_status("WARN: manual capture image materialization failed")
		_inspection_capture_id = 0
		return
	_capture_texture_rect.texture = ImageTexture.create_from_image(capture_image)
	_capture_facts_label.text = "payload_kind=%d\nsize=%dx%d\ncapture_id=%d\nmode=manual capture" % [
		capture_result.get_payload_kind(),
		capture_result.get_width(),
		capture_result.get_height(),
		capture_result.get_capture_id()
	]
	_append_status("INFO: manual capture displayed (capture_id=%d)" % _inspection_capture_id)
	_inspection_capture_id = 0


func _step_ok(detail: String) -> void:
	_append_status("step %d OK: %s" % [_step, detail])
	_step += 1


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	_fail(message)


func _append_status(line: String) -> void:
	print(line)
	_status_label.append_text(line + "\n")


func _ok(message: String) -> void:
	if _done:
		return
	_append_status(message)
	if _is_headless:
		_done = true
		_cleanup_and_quit(0)
		return
	_inspection_mode = true
	_ensure_stream_panel_display_view_bound()
	_capture_again_button.disabled = false
	_append_status("GUI mode: staying open for inspection")
	_append_status("Press Esc to quit (optional: press C or click 'Capture Again')")


func _fail(message: String) -> void:
	if _done:
		return
	_done = true
	push_error(message)
	_append_status(message)
	_cleanup_and_quit(1)


func _cleanup_and_quit(code: int) -> void:
	set_process(false)
	# Drop UI-held display/capture texture refs before runtime teardown. The
	# stream display view may be backed by runtime-owned GPU state that becomes
	# invalid after CamBANGServer.stop().
	_stream_texture_rect.texture = null
	_capture_texture_rect.texture = null
	CamBANGServer.stop()
	if not _quit_requested:
		_quit_requested = true
		call_deferred("_quit_next_frame", code)


func _quit_next_frame(code: int) -> void:
	for _i in range(QUIT_FLUSH_FRAMES):
		await get_tree().process_frame
	print("INFO: quit requested code=%d" % code)
	get_tree().quit(code)
