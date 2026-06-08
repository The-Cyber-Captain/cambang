extends Control

# Result retrieval verification scope:
# - proves CamBANGStreamResult from Godot boundary
# - proves CamBANGCaptureResult from Godot boundary
# - proves grouped facts/provenance accessor shape (Dictionary)
# - proves to_image() for both
# Non-goals intentionally preserved:
# - no CamBANGRig.trigger_capture()
# - no CamBANGCaptureResultSet realization proof
# - no grouped-capture curation policy proof
# - no mailbox coupling

const STREAM_TIMEOUT_MS := 4000
const CAPTURE_TIMEOUT_MS := 4000
const PROFILE_APPLICATION_TIMEOUT_MS := 4000
const INSPECTION_CAPTURE_TIMEOUT_MS := 4000
const TOTAL_TIMEOUT_MS := 10000
const PAYLOAD_KIND_CPU_PACKED := 0
const PAYLOAD_KIND_GPU_SURFACE := 2

@onready var _status_label: RichTextLabel = $RootMargin/MainColumn/StatusLabel
@onready var _stream_facts_label: Label = $RootMargin/MainColumn/ResultsRow/StreamPanel/StreamFacts
@onready var _requested_stream_facts_label: Label = $RootMargin/MainColumn/ResultsRow/RequestedStreamPanel/RequestedStreamFacts
@onready var _capture_facts_label: Label = $RootMargin/MainColumn/ResultsRow/CapturePanel/CaptureFacts
@onready var _stream_texture_rect: TextureRect = $RootMargin/MainColumn/ResultsRow/StreamPanel/StreamTexture
@onready var _requested_stream_texture_rect: TextureRect = $RootMargin/MainColumn/ResultsRow/RequestedStreamPanel/RequestedStreamTexture
@onready var _capture_texture_rect: TextureRect = $RootMargin/MainColumn/ResultsRow/CapturePanel/CaptureTexture
@onready var _member_strip_row: HBoxContainer = $RootMargin/MainColumn/MemberInspectionStrip/StripScroll/MembersRow
@onready var _gui_controls: HBoxContainer = $RootMargin/MainColumn/GuiControls
@onready var _request_stream_image_button: Button = $RootMargin/MainColumn/GuiControls/RequestStreamImageButton
@onready var _capture_again_button: Button = $RootMargin/MainColumn/GuiControls/CaptureAgainButton
@onready var _status_panel: CamBANGStatusPanel = $RootMargin/MainColumn/CamBANGStatusPanel

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
var _capture_device = null
var _inspection_capture_device = null
var _capture_profile_version_before_set := -1
var _capture_profile_version_after_set := -1
var _still_profile_set_requested := false
var _still_profile_request_start_ms := 0
var _applied_bracket_profile_snapshot_summary := ""
var _capture_triggered := false
var _capture_baseline_progress: Dictionary = {}
var _capture_completion_progress: Dictionary = {}
var _capture_completion_seen := false
var _expected_capture_id := 0
var _inspection_capture_baseline_progress: Dictionary = {}
var _inspection_capture_completion_progress: Dictionary = {}
var _inspection_capture_completion_seen := false
var _inspection_expected_capture_id := 0
var _stream_baseline_verified := false
var _device_seam_verified := false
var _status_panel_acquisition_session_detail_requested := false

func _make_scene70_still_image_bundle_members() -> Array:
	return [
		{
			"image_member_index": 0,
			"role": CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED,
			"intended_exposure_compensation_milli_ev": 0,
		},
		{
			"image_member_index": 1,
			"role": CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET,
			"intended_exposure_compensation_milli_ev": -1000,
		},
		{
			"image_member_index": 2,
			"role": CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET,
			"intended_exposure_compensation_milli_ev": 1000,
		},
	]

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
		_request_stream_image_button.pressed.connect(_on_request_stream_image_pressed)
		_capture_again_button.pressed.connect(_on_capture_again_pressed)
		_request_stream_image_button.disabled = true
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

	if not _capture_triggered:
		# Stream timeout only applies to initial stream-baseline acquisition.
		# After stream baseline passes, we may still be waiting for snapshot bundle
		# catch-up gates before triggering capture; that must not reuse stream timeout.
		if not _stream_baseline_verified and Time.get_ticks_msec() - _stream_poll_start_ms > STREAM_TIMEOUT_MS:
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
	# This scenario is bootstrapped from a synthetic timeline stream snapshot, not a
	# public CamBANGStream handle, so it uses the advanced server stream-id lookup.
	var stream_result = CamBANGServer.get_stream_result_by_stream_id(_stream_id)
	if stream_result == null:
		return

	if not _stream_baseline_verified:
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
		_materialize_requested_stream_image(stream_result, "mode=initial stream to_image request")
		_require(_requested_stream_texture_rect.texture != null, "step %d FAIL: requested stream image panel not populated" % _step)
		_step_ok("requested stream image panel populated from explicit request")

		var stream_display_view = stream_result.get_display_view()
		if stream_payload_kind == PAYLOAD_KIND_GPU_SURFACE:
			_require(stream_display_view is Texture2D, "step %d FAIL: GPU_SURFACE stream display_view must be Texture2D" % _step)
		else:
			_require(stream_display_view is Texture2D, "step %d FAIL: CPU_PACKED stream display_view must be Texture2D" % _step)
		_step_ok("stream display_view path verified")

		_ensure_stream_panel_display_view_bound(stream_result, true)
		_step_ok("stream image displayed")
		_stream_baseline_verified = true

	var device = CamBANGServer.get_device(_device_instance_id)
	_require(device != null, "step %d FAIL: CamBANGServer.get_device() returned null" % _step)
	_require(device.get_class() == "CamBANGDevice", "step %d FAIL: get_device() must return CamBANGDevice" % _step)
	if not _device_seam_verified:
		_step_ok("device seam verified")
		_device_seam_verified = true

	if not _still_profile_set_requested:
		var expected_members := _make_scene70_still_image_bundle_members()
		var device_snapshot_before_set := _get_device_snapshot_record(_device_instance_id)
		var profile_before_set: Dictionary = _extract_snapshot_still_profile(device_snapshot_before_set)
		_capture_profile_version_before_set = int(profile_before_set.get("version", -1))
		var bracket_profile := {
			"still_image_bundle": {
				"members": expected_members,
			},
		}
		var set_profile_err := int(device.set_still_capture_profile(bracket_profile))
		_require(
			set_profile_err == OK,
			"step %d FAIL: set_still_capture_profile failed err=%d" % [_step, set_profile_err]
		)
		_step_ok("device still capture profile set request accepted (three-member bracket)")
		_still_profile_set_requested = true
		_still_profile_request_start_ms = Time.get_ticks_msec()
		return

	if not _is_expected_bracket_profile_snapshot_visible():
		if Time.get_ticks_msec() - _still_profile_request_start_ms > PROFILE_APPLICATION_TIMEOUT_MS:
			var observed_snapshot := _get_device_snapshot_record(_device_instance_id)
			var observed_profile: Dictionary = _extract_snapshot_still_profile(observed_snapshot)
			_fail("step %d FAIL: bracket still profile did not become snapshot-visible before capture; observed %s" % [_step, _describe_still_profile(observed_profile)])
		return

	if not _capture_triggered:
		_capture_baseline_progress = _get_capture_progress_snapshot(_device_instance_id)
		_require(
			bool(_capture_baseline_progress.get("available", false)),
			"step %d FAIL: capture progress snapshot unavailable before trigger" % _step
		)
		var capture_err := int(device.trigger_capture())
		_require(capture_err == OK, "step %d FAIL: trigger_capture() returned err=%d" % [_step, capture_err])
		_capture_device = device
		_capture_completion_seen = false
		_capture_completion_progress = {}
		_expected_capture_id = 0
		_step_ok("capture trigger accepted after bracket profile became snapshot-visible")
		_capture_triggered = true
		_capture_poll_start_ms = Time.get_ticks_msec()
		return
	var device_snapshot_after_trigger := _get_device_snapshot_record(_device_instance_id)
	var profile_after_trigger: Dictionary = _extract_snapshot_still_profile(device_snapshot_after_trigger)
	var capture_profile_version_after_trigger := int(profile_after_trigger.get("version", -1))
	if capture_profile_version_after_trigger < 0:
		return
	_require(
		capture_profile_version_after_trigger == _capture_profile_version_after_set,
		"step %d FAIL: trigger_capture() must not change capture_profile_version (%d -> %d)" % [
			_step,
			_capture_profile_version_after_set,
			capture_profile_version_after_trigger
		]
	)
	_step_ok("capture_profile_version unchanged by trigger_capture")


func _try_verify_capture_result() -> void:
	if _capture_device == null:
		return
	if not _capture_completion_seen:
		var progress := _get_capture_progress_snapshot(_device_instance_id)
		if not bool(progress.get("available", false)):
			return
		if int(progress.get("captures_failed", 0)) > int(_capture_baseline_progress.get("captures_failed", 0)):
			_fail("step %d FAIL: capture failed after trigger (baseline=%s progress=%s)" % [
				_step,
				_describe_capture_progress(_capture_baseline_progress),
				_describe_capture_progress(progress),
			])
			return
		if int(progress.get("captures_completed", 0)) <= int(_capture_baseline_progress.get("captures_completed", 0)):
			return
		_capture_completion_progress = progress
		_expected_capture_id = int(progress.get("last_capture_id", 0))
		_capture_completion_seen = true
		_step_ok("new capture completion observed (%s)" % _describe_capture_progress(progress))

	var capture_result = _capture_device.get_result()
	if capture_result == null:
		return

	var result_capture_id := _capture_result_id(capture_result)
	if _expected_capture_id > 0:
		_require(
			result_capture_id == _expected_capture_id,
			"step %d FAIL: capture result id mismatch (expected=%d observed=%d baseline=%s completion=%s)" % [
				_step,
				_expected_capture_id,
				result_capture_id,
				_describe_capture_progress(_capture_baseline_progress),
				_describe_capture_progress(_capture_completion_progress),
			]
		)

	_require(capture_result.get_class() == "CamBANGCaptureResult", "step %d FAIL: capture result must be CamBANGCaptureResult" % _step)
	_step_ok("capture result object branding verified")

	_require(capture_result.get_width() > 0, "step %d FAIL: capture width invalid" % _step)
	_require(capture_result.get_height() > 0, "step %d FAIL: capture height invalid" % _step)
	_require(capture_result.get_format() != 0, "step %d FAIL: capture format invalid" % _step)
	_require(capture_result.get_payload_kind() == PAYLOAD_KIND_CPU_PACKED, "step %d FAIL: capture payload_kind must be CPU_PACKED" % _step)
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

	var expected_members := _make_scene70_still_image_bundle_members()
	var expected_member_count := expected_members.size()
	var observed_member_count := int(capture_result.get_image_count())
	_require(
		observed_member_count == expected_member_count,
		"step %d FAIL: capture get_image_count() mismatch for bracket profile (expected=%d observed=%d expected_capture_id=%d result_capture_id=%d applied_snapshot=%s baseline=%s completion=%s)" % [
			_step,
			expected_member_count,
			observed_member_count,
			_expected_capture_id,
			result_capture_id,
			_applied_bracket_profile_snapshot_summary,
			_describe_capture_progress(_capture_baseline_progress),
			_describe_capture_progress(_capture_completion_progress),
		]
	)
	_require(bool(capture_result.has_additional_images()) == (expected_member_count > 1), "step %d FAIL: capture has_additional_images() mismatch for bracket profile" % _step)
	var materialized_member_0: Image = null
	for i in range(expected_member_count):
		var expected_member: Dictionary = expected_members[i]
		var image_member: Dictionary = capture_result.get_image_member(i)
		_require(not image_member.is_empty(), "step %d FAIL: capture get_image_member(%d) must return non-empty Dictionary" % [_step, i])
		_require(int(image_member.get("image_member_index", -1)) == int(expected_member.get("image_member_index", -1)), "step %d FAIL: capture image_member(%d).image_member_index mismatch" % [_step, i])
		_require(int(image_member.get("role", -1)) == int(expected_member.get("role", -1)), "step %d FAIL: capture image_member(%d).role mismatch" % [_step, i])
		var expected_intended_ev := int(expected_member.get("intended_exposure_compensation_milli_ev", 0))
		_require(int(image_member.get("applied_exposure_compensation_milli_ev", 0)) == expected_intended_ev, "step %d FAIL: capture image_member(%d) applied EV mismatch" % [_step, i])
		_require(bool(image_member.get("has_realized_exposure_compensation_milli_ev", false)), "step %d FAIL: capture image_member(%d) realized EV must be present in synthetic normal mode" % [_step, i])
		_require(int(image_member.get("realized_exposure_compensation_milli_ev", 0)) == int(image_member.get("applied_exposure_compensation_milli_ev", 0)), "step %d FAIL: capture image_member(%d) realized EV must equal applied EV in synthetic normal mode" % [_step, i])
		var image_member_materialized: Image = capture_result.to_image_member(i)
		_require(image_member_materialized != null, "step %d FAIL: capture to_image_member(%d) returned null" % [_step, i])
		if i == 0:
			materialized_member_0 = image_member_materialized
	var out_of_range_member: Dictionary = capture_result.get_image_member(expected_member_count)
	_require(out_of_range_member.is_empty(), "step %d FAIL: capture get_image_member(expected_count) must return empty Dictionary for out-of-range" % _step)
	var out_of_range_image: Image = capture_result.to_image_member(expected_member_count)
	_require(out_of_range_image == null, "step %d FAIL: capture to_image_member(expected_count) must be null for out-of-range member" % _step)
	_step_ok("capture indexed image-member metadata verified (three-member profile)")

	var capture_can_to_image := int(capture_result.can_to_image())
	_require(capture_can_to_image != int(capture_result.CAPABILITY_UNSUPPORTED), "step %d FAIL: capture can_to_image() unexpectedly unsupported" % _step)
	_step_ok("capture can_to_image capability verified")
	var capture_can_to_image_member_0 := int(capture_result.can_to_image_member(0))
	_require(capture_can_to_image_member_0 != int(capture_result.CAPABILITY_UNSUPPORTED), "step %d FAIL: capture can_to_image_member(0) unexpectedly unsupported" % _step)
	_step_ok("capture can_to_image_member(0) capability verified")

	# Honesty: encoded-byte access can remain unsupported.
	var encoded_capability := int(capture_result.can_get_encoded_bytes())
	if encoded_capability == int(capture_result.CAPABILITY_UNSUPPORTED):
		_append_status("INFO: capture can_get_encoded_bytes() is UNSUPPORTED (acceptable)")
	else:
		_append_status("INFO: capture can_get_encoded_bytes() capability=%d" % encoded_capability)

	var capture_image: Image = capture_result.to_image()
	_require(capture_image != null, "step %d FAIL: capture to_image() returned null" % _step)
	_require(capture_image.get_width() > 0 and capture_image.get_height() > 0, "step %d FAIL: capture image dimensions invalid" % _step)
	_require(materialized_member_0 != null, "step %d FAIL: capture to_image_member(0) returned null" % _step)
	_require(materialized_member_0.get_width() == capture_image.get_width(), "step %d FAIL: capture to_image_member(0) width must match to_image()" % _step)
	_require(materialized_member_0.get_height() == capture_image.get_height(), "step %d FAIL: capture to_image_member(0) height must match to_image()" % _step)
	_require(materialized_member_0.get_format() == capture_image.get_format(), "step %d FAIL: capture to_image_member(0) format must match to_image()" % _step)
	_step_ok("capture to_image materialization verified")

	_capture_texture_rect.texture = ImageTexture.create_from_image(capture_image)
	_refresh_member_inspection_strip(capture_result, expected_members)
	_capture_facts_label.text = "payload_kind=%d\nsize=%dx%d\nimages=%d/%d additional=%s\nmode=initial verification" % [
		capture_result.get_payload_kind(),
		capture_result.get_width(),
		capture_result.get_height(),
		observed_member_count,
		expected_member_count,
		str(bool(capture_result.has_additional_images())),
	]
	_exercise_status_panel_acquisition_session_fixture_detail_visibility()
	if _status_panel != null:
		_status_panel.apply_fixture_detail_visible_rows([])
		_status_panel.force_refresh()
	_step_ok("capture image displayed")
	_ok("OK: result_retrieval_verification passed")


func _role_name(role: int) -> String:
	if role == CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED:
		return "default"
	if role == CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET:
		return "bracket"
	return str(role)


func _clear_member_inspection_strip() -> void:
	if _member_strip_row == null:
		return
	for child in _member_strip_row.get_children():
		child.queue_free()


func _refresh_member_inspection_strip(capture_result, expected_members: Array) -> void:
	_clear_member_inspection_strip()
	if _member_strip_row == null or capture_result == null:
		return

	var returned_count := int(capture_result.get_image_count())
	var expected_count := expected_members.size()

	if returned_count <= 0:
		var empty_label := Label.new()
		empty_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty_label.text = "no returned capture image members (applied bundle=%d)" % expected_count
		_member_strip_row.add_child(empty_label)
		return

	for i in range(returned_count):
		var expected_member: Dictionary = {}
		if i < expected_members.size() and typeof(expected_members[i]) == TYPE_DICTIONARY:
			expected_member = expected_members[i]

		var image_member: Dictionary = capture_result.get_image_member(i)
		var item_col := VBoxContainer.new()
		item_col.custom_minimum_size = Vector2(150, 0)
		item_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL

		var image_member_materialized: Image = capture_result.to_image_member(i)
		if image_member_materialized != null:
			var preview := TextureRect.new()
			preview.custom_minimum_size = Vector2(140, 78)
			preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			preview.expand_mode = TextureRect.EXPAND_FIT_WIDTH_PROPORTIONAL
			preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
			preview.texture = ImageTexture.create_from_image(image_member_materialized)
			item_col.add_child(preview)
		else:
			var missing_preview := Label.new()
			missing_preview.custom_minimum_size = Vector2(140, 78)
			missing_preview.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			missing_preview.text = "member %d\nimage unavailable" % i
			item_col.add_child(missing_preview)

		var role_value := int(image_member.get("role", expected_member.get("role", -1)))
		var role_label := _role_name(role_value)
		var realized_ev_label := "unknown"
		if bool(image_member.get("has_realized_exposure_compensation_milli_ev", false)):
			realized_ev_label = str(int(image_member.get("realized_exposure_compensation_milli_ev", 0)))

		var meta := Label.new()
		meta.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		if image_member.is_empty():
			meta.text = "idx=%d returned metadata unavailable\nreturned=%d applied=%d" % [
				i,
				returned_count,
				expected_count,
			]
		else:
			meta.text = "idx=%d role=%s ev=%s\nreturned=%d applied=%d" % [
				int(image_member.get("image_member_index", i)),
				role_label,
				realized_ev_label,
				returned_count,
				expected_count,
			]
		item_col.add_child(meta)

		_member_strip_row.add_child(item_col)

	if returned_count < expected_count:
		var note := Label.new()
		note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		note.text = "CaptureResult returned %d of applied bundle %d" % [
			returned_count,
			expected_count,
		]
		_member_strip_row.add_child(note)


func _ensure_stream_panel_display_view_bound(stream_result = null, force_rebind: bool = false) -> void:
	var latest_stream_result = stream_result
	if latest_stream_result == null:
		if _stream_id == 0:
			return
		# This scene only has a timeline-authored stream id here, not a public
		# CamBANGStream object, so use the advanced server stream-id lookup.
		latest_stream_result = CamBANGServer.get_stream_result_by_stream_id(_stream_id)
	if latest_stream_result == null:
		return
	if int(latest_stream_result.can_get_display_view()) == int(latest_stream_result.CAPABILITY_UNSUPPORTED):
		return

	# get_display_view() is a display-oriented live view of current retained
	# stream state for flowing streams. Bind once, leave it bound while flowing,
	# and only rebind when the view object genuinely changes.
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


func _on_request_stream_image_pressed() -> void:
	_request_manual_stream_image()


func _request_manual_stream_image() -> void:
	if _is_headless or not _inspection_mode:
		return
	# This scene only has a timeline-authored stream id here, not a public
	# CamBANGStream object, so use the advanced server stream-id lookup.
	var stream_result = CamBANGServer.get_stream_result_by_stream_id(_stream_id)
	if stream_result == null:
		_append_status("WARN: cannot request stream to_image(); stream result unavailable")
		return
	_materialize_requested_stream_image(stream_result, "mode=manual stream to_image request")


func _materialize_requested_stream_image(stream_result, mode_text: String) -> void:
	if stream_result == null:
		return
	# Explicit to_image() materialization from stream result. This yields a
	# manually requested CPU-backed artifact, not the normal live display path.
	var requested_image: Image = stream_result.to_image()
	if requested_image == null:
		_append_status("WARN: requested stream to_image() returned null")
		return
	if requested_image.get_width() <= 0 or requested_image.get_height() <= 0:
		_append_status("WARN: requested stream to_image() dimensions invalid")
		return
	_requested_stream_texture_rect.texture = ImageTexture.create_from_image(requested_image)
	_requested_stream_facts_label.text = "payload_kind=%d\nsize=%dx%d\nstream_id=%d\n%s" % [
		stream_result.get_payload_kind(),
		stream_result.get_width(),
		stream_result.get_height(),
		stream_result.get_stream_id(),
		mode_text
	]
	if mode_text.begins_with("mode=manual"):
		_append_status("INFO: requested stream image displayed (%s)" % mode_text)


func _request_manual_capture() -> void:
	if _is_headless or not _inspection_mode:
		return
	if _inspection_capture_device != null:
		_append_status("INFO: capture already pending; wait for completion")
		return
	var device = CamBANGServer.get_device(_device_instance_id)
	if device == null:
		_append_status("WARN: cannot capture again; device unavailable")
		return

	_inspection_capture_baseline_progress = _get_capture_progress_snapshot(_device_instance_id)
	if not bool(_inspection_capture_baseline_progress.get("available", false)):
		_append_status("WARN: cannot capture again; capture progress snapshot unavailable")
		return

	_clear_member_inspection_strip()
	var capture_err := int(device.trigger_capture())
	if capture_err != OK:
		_append_status("WARN: manual capture request rejected err=%d" % capture_err)
		return
	_inspection_capture_device = device
	_inspection_capture_completion_seen = false
	_inspection_capture_completion_progress = {}
	_inspection_expected_capture_id = 0
	_inspection_capture_poll_start_ms = Time.get_ticks_msec()
	_append_status("INFO: manual capture requested (baseline=%s)" % _describe_capture_progress(_inspection_capture_baseline_progress))


func _get_device_snapshot_record(device_instance_id: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return {}
	var devices: Array = snapshot.get("devices", [])
	for d in devices:
		if typeof(d) != TYPE_DICTIONARY:
			continue
		var rec: Dictionary = d
		if int(rec.get("instance_id", 0)) == device_instance_id:
			return rec
	return {}


func _extract_snapshot_still_profile(device_snapshot: Dictionary) -> Dictionary:
	var capture_profile_v: Variant = device_snapshot.get("capture_profile", null)
	if typeof(capture_profile_v) != TYPE_DICTIONARY:
		return {}
	var capture_profile: Dictionary = capture_profile_v
	var still_v: Variant = capture_profile.get("still", null)
	if typeof(still_v) != TYPE_DICTIONARY:
		return {}
	return still_v


func _extract_still_image_bundle_members(still_profile: Dictionary) -> Array:
	var bundle_v: Variant = still_profile.get("still_image_bundle", null)
	if typeof(bundle_v) != TYPE_DICTIONARY:
		return []
	var bundle: Dictionary = bundle_v
	var members_v: Variant = bundle.get("members", null)
	if typeof(members_v) != TYPE_ARRAY:
		return []
	var members: Array = members_v
	return members


func _is_expected_bracket_profile_snapshot_visible() -> bool:
	var device_snapshot := _get_device_snapshot_record(_device_instance_id)
	var still_profile: Dictionary = _extract_snapshot_still_profile(device_snapshot)
	var version := int(still_profile.get("version", -1))
	if version < 0:
		return false
	if _capture_profile_version_before_set >= 0 and version <= _capture_profile_version_before_set:
		return false

	var expected_members := _make_scene70_still_image_bundle_members()
	var observed_members := _extract_still_image_bundle_members(still_profile)
	if observed_members.size() != expected_members.size():
		return false
	for i in range(expected_members.size()):
		if typeof(observed_members[i]) != TYPE_DICTIONARY:
			return false
		var observed_member: Dictionary = observed_members[i]
		var expected_member: Dictionary = expected_members[i]
		if int(observed_member.get("image_member_index", -1)) != int(expected_member.get("image_member_index", -1)):
			return false
		if int(observed_member.get("role", -1)) != int(expected_member.get("role", -1)):
			return false
		if int(observed_member.get("intended_exposure_compensation_milli_ev", 0)) != int(expected_member.get("intended_exposure_compensation_milli_ev", 0)):
			return false

	_capture_profile_version_after_set = version
	_applied_bracket_profile_snapshot_summary = _describe_still_profile(still_profile)
	_step_ok("bracket still profile snapshot-visible (%s)" % _applied_bracket_profile_snapshot_summary)
	return true


func _describe_still_profile(still_profile: Dictionary) -> String:
	var version := int(still_profile.get("version", -1))
	var members := _extract_still_image_bundle_members(still_profile)
	var member_descriptions: Array[String] = []
	for m in members:
		if typeof(m) != TYPE_DICTIONARY:
			member_descriptions.append("<non-dictionary>")
			continue
		var member: Dictionary = m
		member_descriptions.append("{index=%d role=%d intended_ev=%d}" % [
			int(member.get("image_member_index", -1)),
			int(member.get("role", -1)),
			int(member.get("intended_exposure_compensation_milli_ev", 0))
		])
	return "version=%d members=%d [%s]" % [version, members.size(), ", ".join(member_descriptions)]


func _get_acquisition_session_snapshot_record(device_instance_id: int) -> Dictionary:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return {}
	var acquisition_sessions: Array = snapshot.get("acquisition_sessions", [])
	for s in acquisition_sessions:
		if typeof(s) != TYPE_DICTIONARY:
			continue
		var rec: Dictionary = s
		if int(rec.get("device_instance_id", 0)) == device_instance_id:
			return rec
	return {}


func _capture_progress_from_record(record: Dictionary, source: String) -> Dictionary:
	if record.is_empty():
		return {"available": false, "source": source}
	var has_completed := record.has("captures_completed")
	var has_failed := record.has("captures_failed")
	var has_last_capture := record.has("last_capture_id")
	if not has_completed and not has_failed and not has_last_capture:
		return {"available": false, "source": source}
	return {
		"available": true,
		"source": source,
		"captures_triggered": int(record.get("captures_triggered", 0)),
		"captures_completed": int(record.get("captures_completed", 0)),
		"captures_failed": int(record.get("captures_failed", 0)),
		"last_capture_id": int(record.get("last_capture_id", 0)),
		"active_capture_id": int(record.get("active_capture_id", 0)),
	}


func _get_capture_progress_snapshot(device_instance_id: int) -> Dictionary:
	var device_progress := _capture_progress_from_record(_get_device_snapshot_record(device_instance_id), "device")
	if bool(device_progress.get("available", false)):
		return device_progress
	var acquisition_session_progress := _capture_progress_from_record(_get_acquisition_session_snapshot_record(device_instance_id), "acquisition_session")
	if bool(acquisition_session_progress.get("available", false)):
		return acquisition_session_progress
	return {"available": false, "source": "none"}


func _describe_capture_progress(progress: Dictionary) -> String:
	if not bool(progress.get("available", false)):
		return "capture_progress unavailable source=%s" % str(progress.get("source", "none"))
	return "source=%s triggered=%d completed=%d failed=%d last_capture_id=%d active_capture_id=%d" % [
		str(progress.get("source", "unknown")),
		int(progress.get("captures_triggered", 0)),
		int(progress.get("captures_completed", 0)),
		int(progress.get("captures_failed", 0)),
		int(progress.get("last_capture_id", 0)),
		int(progress.get("active_capture_id", 0)),
	]


func _capture_result_id(capture_result) -> int:
	if capture_result == null:
		return 0
	if capture_result.has_method("get_capture_id"):
		return int(capture_result.get_capture_id())
	return 0


func _exercise_status_panel_acquisition_session_fixture_detail_visibility() -> void:
	if _status_panel_acquisition_session_detail_requested:
		return
	if _status_panel == null:
		return
	var acquisition_session_snapshot := _get_acquisition_session_snapshot_record(_device_instance_id)
	if acquisition_session_snapshot.is_empty():
		return
	var acquisition_session_id := int(acquisition_session_snapshot.get("acquisition_session_id", 0))
	if acquisition_session_id <= 0:
		return
	var row_id := "acquisition_session/%d" % acquisition_session_id
	_status_panel.apply_fixture_expanded_rows([row_id])
	_status_panel.apply_fixture_detail_visible_rows([row_id])
	_status_panel.force_refresh()
	_status_panel_acquisition_session_detail_requested = true
	_append_status("INFO: status panel fixture detail visibility exercised (%s)" % row_id)


func _poll_inspection_capture_result() -> void:
	if _inspection_capture_device == null:
		return
	if Time.get_ticks_msec() - _inspection_capture_poll_start_ms > INSPECTION_CAPTURE_TIMEOUT_MS:
		_append_status("WARN: manual capture timeout (baseline=%s completion=%s)" % [
			_describe_capture_progress(_inspection_capture_baseline_progress),
			_describe_capture_progress(_inspection_capture_completion_progress),
		])
		_inspection_capture_device = null
		return
	if not _inspection_capture_completion_seen:
		var progress := _get_capture_progress_snapshot(_device_instance_id)
		if not bool(progress.get("available", false)):
			return
		if int(progress.get("captures_failed", 0)) > int(_inspection_capture_baseline_progress.get("captures_failed", 0)):
			_append_status("WARN: manual capture failed after trigger (baseline=%s progress=%s)" % [
				_describe_capture_progress(_inspection_capture_baseline_progress),
				_describe_capture_progress(progress),
			])
			_inspection_capture_device = null
			return
		if int(progress.get("captures_completed", 0)) <= int(_inspection_capture_baseline_progress.get("captures_completed", 0)):
			return
		_inspection_capture_completion_progress = progress
		_inspection_expected_capture_id = int(progress.get("last_capture_id", 0))
		_inspection_capture_completion_seen = true
		_append_status("INFO: manual capture completion observed (%s)" % _describe_capture_progress(progress))

	var capture_result = _inspection_capture_device.get_result()
	if capture_result == null:
		return
	var result_capture_id := _capture_result_id(capture_result)
	if _inspection_expected_capture_id > 0 and result_capture_id != _inspection_expected_capture_id:
		return
	var capture_image: Image = capture_result.to_image()
	if capture_image == null:
		_append_status("WARN: manual capture image materialization failed")
		_inspection_capture_device = null
		return
	_capture_texture_rect.texture = ImageTexture.create_from_image(capture_image)
	var expected_members := _make_scene70_still_image_bundle_members()
	var returned_count := int(capture_result.get_image_count())
	var expected_count := expected_members.size()
	_refresh_member_inspection_strip(capture_result, expected_members)
	_capture_facts_label.text = "payload_kind=%d\nsize=%dx%d\nimages=%d/%d additional=%s\ncapture_id=%d expected=%d\nmode=manual capture" % [
		capture_result.get_payload_kind(),
		capture_result.get_width(),
		capture_result.get_height(),
		returned_count,
		expected_count,
		str(bool(capture_result.has_additional_images())),
		result_capture_id,
		_inspection_expected_capture_id,
	]
	_append_status("INFO: manual capture displayed (image_count=%d/%d capture_id=%d expected=%d)" % [
		returned_count,
		expected_count,
		result_capture_id,
		_inspection_expected_capture_id,
	])
	_inspection_capture_device = null


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
	_request_stream_image_button.disabled = false
	_capture_again_button.disabled = false
	_append_status("GUI mode: staying open for inspection")
	_append_status("Press Esc to quit (optional: click 'Request Stream Image' / 'Capture Again')")


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
	_requested_stream_texture_rect.texture = null
	_capture_texture_rect.texture = null
	_clear_member_inspection_strip()
	if not _quit_requested:
		_quit_requested = true
		print("INFO: quit requested code=%d" % code)
		CamBANGServer.stop_and_quit(code)
