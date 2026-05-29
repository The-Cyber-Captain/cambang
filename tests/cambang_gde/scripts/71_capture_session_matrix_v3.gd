extends Control

const DEVICE_A_KEY: String = "CameraA"
const DEVICE_B_KEY: String = "CameraB"
const SLOT_PREVIEW_A: String = "PREVIEW_A"
const SLOT_VIEWFINDER_A: String = "VIEWFINDER_A"
const SLOT_PREVIEW_B: String = "PREVIEW_B"
const SLOT_VIEWFINDER_B: String = "VIEWFINDER_B"

const HW_A: String = "synthetic:0"
const HW_B: String = "synthetic:1"
const SCENARIO_PATH: String = "res://scenarios/capture_session_matrix_v3.json"
const LOG_SECOND_EPS: float = 0.05
const EXERCISE_DISPLAY_ONESHOT := "display_oneshot"

const CHECKPOINTS: Array = [
	{"time_s": 1.0,  "kind": "capture",         "id": 1,  "device": DEVICE_A_KEY,    "desc": "Capture-only on CameraA"},
	{"time_s": 3.0,  "kind": "stream",          "id": 2,  "slot": SLOT_PREVIEW_A,    "desc": "Bind StreamResult Preview-A"},
	{"time_s": 5.0,  "kind": "capture",         "id": 3,  "device": DEVICE_A_KEY,    "desc": "Capture while Preview-A active"},
	{"time_s": 8.0,  "kind": "stream",          "id": 4,  "slot": SLOT_VIEWFINDER_A, "desc": "Bind StreamResult Viewfinder-A"},
	{"time_s": 10.0, "kind": "capture",         "id": 5,  "device": DEVICE_A_KEY,    "desc": "Capture while Viewfinder-A active"},
	{"time_s": 16.0, "kind": "capture",         "id": 6,  "device": DEVICE_B_KEY,    "desc": "Capture-only on CameraB"},
	{"time_s": 18.0, "kind": "stream",          "id": 7,  "slot": SLOT_PREVIEW_B,    "desc": "Bind StreamResult Preview-B"},
	{"time_s": 20.0, "kind": "capture",         "id": 8,  "device": DEVICE_B_KEY,    "desc": "Capture while Preview-B active"},
	{"time_s": 23.0, "kind": "stream",          "id": 9,  "slot": SLOT_VIEWFINDER_B, "desc": "Bind StreamResult Viewfinder-B"},
	{"time_s": 25.0, "kind": "capture",         "id": 10, "device": DEVICE_B_KEY,    "desc": "Capture while Viewfinder-B active"},
	{"time_s": 27.0, "kind": "dual_capture",    "id": 11,                              "desc": "Dual capture on CameraA and CameraB"},
	{"time_s": 32.0, "kind": "release_streams", "id": 12,                              "desc": "Release stream result bindings before teardown"}
]

@onready var _instructions: RichTextLabel = get_node("RootMargin/MainColumn/Instructions")
@onready var _log: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/LogPanel/LogVBox/Log")
@onready var _capture_a_rect: TextureRect = get_node("RootMargin/MainColumn/HBoxContainer/Grid/CaptureAPanel/VBox/Texture")
@onready var _capture_a_facts: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/Grid/CaptureAPanel/VBox/Facts")
@onready var _preview_a_rect: TextureRect = get_node("RootMargin/MainColumn/HBoxContainer/Grid/PreviewAPanel/VBox/Texture")
@onready var _preview_a_facts: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/Grid/PreviewAPanel/VBox/Facts")
@onready var _viewfinder_a_rect: TextureRect = get_node("RootMargin/MainColumn/HBoxContainer/Grid/ViewfinderAPanel/VBox/Texture")
@onready var _viewfinder_a_facts: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/Grid/ViewfinderAPanel/VBox/Facts")
@onready var _capture_b_rect: TextureRect = get_node("RootMargin/MainColumn/HBoxContainer/Grid/CaptureBPanel/VBox/Texture")
@onready var _capture_b_facts: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/Grid/CaptureBPanel/VBox/Facts")
@onready var _preview_b_rect: TextureRect = get_node("RootMargin/MainColumn/HBoxContainer/Grid/PreviewBPanel/VBox/Texture")
@onready var _preview_b_facts: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/Grid/PreviewBPanel/VBox/Facts")
@onready var _viewfinder_b_rect: TextureRect = get_node("RootMargin/MainColumn/HBoxContainer/Grid/ViewfinderBPanel/VBox/Texture")
@onready var _viewfinder_b_facts: RichTextLabel = get_node("RootMargin/MainColumn/HBoxContainer/Grid/ViewfinderBPanel/VBox/Facts")

var _virtual_time_s: float = 0.0
var _last_logged_second: int = -1
var _checkpoint_index: int = 0
var _current_checkpoint: Dictionary = {}
var _waiting_for_user: bool = false
var _awaiting_capture_results: bool = false
var _scenario_complete_logged: bool = false

var _device_ids: Dictionary = {}
var _stream_ids: Dictionary = {}
var _known_stream_ids_by_device: Dictionary = {
	DEVICE_A_KEY: [],
	DEVICE_B_KEY: []
}
var _pending_captures: Array = []
var _timing_process_total_sec := 0.0
var _timing_process_max_sec := 0.0
var _timing_process_calls := 0
var _timing_checkpoint_total_sec := 0.0
var _timing_checkpoint_max_sec := 0.0
var _timing_checkpoint_calls := 0
var _timing_poll_total_sec := 0.0
var _timing_poll_max_sec := 0.0
var _timing_poll_calls := 0
var _timing_display_total_sec := 0.0
var _timing_display_max_sec := 0.0
var _timing_display_calls := 0
var _timing_status_total_sec := 0.0
var _timing_status_max_sec := 0.0
var _timing_status_calls := 0
var _timing_log_total_sec := 0.0
var _timing_log_max_sec := 0.0
var _timing_log_calls := 0
var _summary_timing_logged := false
var _display_demand_trace := false
var _resolved_exercise := EXERCISE_DISPLAY_ONESHOT
var _last_phase_metrics_snapshot: Dictionary = {}


func _ready() -> void:
	set_process_input(true)
	_display_demand_trace = OS.get_environment("CAMBANG_DEV_DISPLAY_DEMAND_TRACE") != ""
	if not _resolve_exercise_or_fail():
		return
	_bootstrap()


func _resolve_exercise_or_fail() -> bool:
	var exercise_raw := OS.get_environment("CAMBANG_EXERCISE").strip_edges()
	var defaulted := false
	var exercise := exercise_raw
	if exercise == "":
		exercise = EXERCISE_DISPLAY_ONESHOT
		defaulted = true
	_resolved_exercise = exercise
	if exercise != EXERCISE_DISPLAY_ONESHOT:
		push_error("[CamBANG][Scene71] exercise=%s supported=false supported_exercises=%s" % [exercise_raw, EXERCISE_DISPLAY_ONESHOT])
		get_tree().quit(2)
		return false
	var resolved_line := "[CamBANG][Scene71] exercise=%s default=%s supported=true behavior=current_one_shot_display_contract" % [
		exercise,
		str(defaulted)
	]
	print(resolved_line)
	_append_log(resolved_line)
	return true




func _phase_marker(phase: String, checkpoint_id: int = -1, detail: String = "") -> void:
	var line := "[CamBANG][Scene71Phase] phase=%s" % phase
	if checkpoint_id >= 0:
		line += " checkpoint=%d" % checkpoint_id
	if detail != "":
		line += " detail=%s" % detail
	print(line)
	_append_log(line)
	_emit_phase_metrics(phase, checkpoint_id)


func _metric_delta_u64(curr: Dictionary, key: String, reset: Array) -> int:
	var c := int(curr.get(key, 0))
	var p := int(_last_phase_metrics_snapshot.get(key, 0))
	if c < p:
		reset[0] = true
		return c
	return c - p


func _metric_delta_f(curr: Dictionary, key: String, reset: Array) -> float:
	var c := float(curr.get(key, 0.0))
	var p := float(_last_phase_metrics_snapshot.get(key, 0.0))
	if c < p:
		reset[0] = true
		return c
	return c - p


func _emit_phase_metrics(phase: String, checkpoint_id: int) -> void:
	if not CamBANGServer.has_method("get_synthetic_metrics_snapshot"):
		return
	var snap: Variant = CamBANGServer.get_synthetic_metrics_snapshot()
	if typeof(snap) != TYPE_DICTIONARY:
		return
	var curr: Dictionary = snap
	if _last_phase_metrics_snapshot.is_empty():
		_last_phase_metrics_snapshot = curr.duplicate(true)
		return
	var reset := [false]
	var line := "[CamBANG][Scene71PhaseMetrics] phase=%s" % phase
	if checkpoint_id >= 0:
		line += " checkpoint=%d" % checkpoint_id
	line += " reset_detected=%s" % str(bool(reset[0]))
	line += " frames_delta=%d" % _metric_delta_u64(curr, "total_emitted_frames", reset)
	line += " gpu_update_attempts_delta=%d" % _metric_delta_u64(curr, "gpu_update_attempts", reset)
	line += " gpu_update_demand_skipped_delta=%d" % _metric_delta_u64(curr, "gpu_update_demand_skipped", reset)
	line += " gpu_texture_update_calls_delta=%d" % _metric_delta_u64(curr, "gpu_texture_update_calls", reset)
	line += " frame_copy_calls_delta=%d" % _metric_delta_u64(curr, "frame_copy_calls", reset)
	line += " frame_render_total_ms_delta=%.3f" % _metric_delta_f(curr, "frame_render_total_ms", reset)
	line += " pattern_overlay_total_ms_delta=%.3f" % _metric_delta_f(curr, "pattern_overlay_total_ms", reset)
	line += " pattern_base_copy_total_ms_delta=%.3f" % _metric_delta_f(curr, "pattern_base_copy_total_ms", reset)
	line += " gpu_update_total_total_ms_delta=%.3f" % _metric_delta_f(curr, "gpu_update_total_total_ms", reset)
	line += " gpu_upload_copy_total_ms_delta=%.3f" % _metric_delta_f(curr, "gpu_upload_copy_total_ms", reset)
	line += " gpu_texture_update_total_ms_delta=%.3f" % _metric_delta_f(curr, "gpu_texture_update_total_ms", reset)
	line += " catchup_ticks_capped_delta=%d" % _metric_delta_u64(curr, "catchup_ticks_capped", reset)
	line += " catchup_frames_dropped_delta=%d" % _metric_delta_u64(curr, "catchup_frames_dropped", reset)
	if bool(reset[0]):
		line = line.replace("reset_detected=False", "reset_detected=True")
	print(line)
	_append_log(line)
	_last_phase_metrics_snapshot = curr.duplicate(true)


func _bootstrap() -> void:
	CamBANGServer.stop()

	var scenario_text: String = FileAccess.get_file_as_string(SCENARIO_PATH)
	_require(scenario_text != "", "Scenario JSON missing at %s" % SCENARIO_PATH)

	var start_err: int = int(CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	))
	_append_log("start -> %d" % start_err)
	_require(start_err == OK, "CamBANGServer.start failed: %d" % start_err)

	var load_err: int = int(CamBANGServer.load_external_scenario(scenario_text))
	_append_log("load_external_scenario -> %d" % load_err)
	_require(load_err == OK, "load_external_scenario failed: %d" % load_err)

	var start_scn_err: int = int(CamBANGServer.start_scenario())
	_append_log("start_scenario -> %d" % start_scn_err)
	_require(start_scn_err == OK, "start_scenario failed: %d" % start_scn_err)

	if CamBANGServer.has_method("set_timeline_paused"):
		var pause_err: int = int(CamBANGServer.set_timeline_paused(false))
		_append_log("set_timeline_paused(false) -> %d" % pause_err)
		_require(pause_err == OK, "set_timeline_paused(false) failed: %d" % pause_err)

	_update_instruction()
	_append_log("Started scenario; timeline running")
	_phase_marker("runtime_start", -1, "exercise=%s" % _resolved_exercise)


func _process(delta: float) -> void:
	var process_start_usec := Time.get_ticks_usec()
	var status_start_usec := Time.get_ticks_usec()
	_latch_snapshot_state()
	var status_sec := _elapsed_sec_from_ticks(status_start_usec)
	_timing_status_total_sec += status_sec
	_timing_status_max_sec = max(_timing_status_max_sec, status_sec)
	_timing_status_calls += 1

	var poll_start_usec := Time.get_ticks_usec()
	var display_sec := _poll_pending_captures()
	var poll_sec := _elapsed_sec_from_ticks(poll_start_usec)
	_timing_poll_total_sec += poll_sec
	_timing_poll_max_sec = max(_timing_poll_max_sec, poll_sec)
	_timing_poll_calls += 1
	_timing_display_total_sec += display_sec
	_timing_display_max_sec = max(_timing_display_max_sec, display_sec)
	_timing_display_calls += 1

	if _awaiting_capture_results:
		if _all_pending_captures_bound():
			_awaiting_capture_results = false
			_waiting_for_user = false
			_append_log("Capture checkpoint complete")
			_phase_marker("capture_checkpoint_complete", int(_current_checkpoint.get("id", -1)))
			_advance_checkpoint()
		_record_process_timing(process_start_usec)
		return

	if _waiting_for_user:
		_record_process_timing(process_start_usec)
		return

	if _checkpoint_index >= CHECKPOINTS.size():
		if not _scenario_complete_logged:
			_append_log("All checkpoints complete; awaiting authored teardown")
			_phase_marker("checkpoint_sequence_complete")
			_scenario_complete_logged = true
			_log_summary_timing_once()
		_record_process_timing(process_start_usec)
		return

	_virtual_time_s += delta
	_maybe_log_virtual_time()

	var cp: Dictionary = CHECKPOINTS[_checkpoint_index]
	var target_s: float = float(cp.get("time_s", 0.0))
	if _virtual_time_s + LOG_SECOND_EPS < target_s:
		_record_process_timing(process_start_usec)
		return

	var checkpoint_start_usec := Time.get_ticks_usec()
	if not _checkpoint_prereq_satisfied(cp):
		_record_process_timing(process_start_usec)
		return
	var checkpoint_sec := _elapsed_sec_from_ticks(checkpoint_start_usec)
	_timing_checkpoint_total_sec += checkpoint_sec
	_timing_checkpoint_max_sec = max(_timing_checkpoint_max_sec, checkpoint_sec)
	_timing_checkpoint_calls += 1

	_current_checkpoint = cp
	_waiting_for_user = true
	_pause_timeline(true)
	_update_instruction()
	_phase_marker("checkpoint_ready", int(cp.get("id", -1)), "index=%d kind=%s t=%.2f" % [_checkpoint_index, str(cp.get("kind", "")), _virtual_time_s])
	_append_log("Checkpoint %d ready at t=%.2fs: %s" % [int(cp.get("id", 0)), _virtual_time_s, str(cp.get("desc", ""))])
	_maybe_log_timing_per_second()
	_record_process_timing(process_start_usec)


func _input(event: InputEvent) -> void:
	if not _waiting_for_user:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		var key_event: InputEventKey = event
		if key_event.keycode == KEY_SPACE or key_event.keycode == KEY_ENTER or key_event.keycode == KEY_KP_ENTER:
			_append_log("Input accepted at checkpoint %d" % int(_current_checkpoint.get("id", 0)))
			_phase_marker("checkpoint_action_begin", int(_current_checkpoint.get("id", -1)), "index=%d action=%s" % [_checkpoint_index, str(_current_checkpoint.get("kind", ""))])
			_perform_checkpoint_action()
			get_viewport().set_input_as_handled()


func _perform_checkpoint_action() -> void:
	var kind: String = str(_current_checkpoint.get("kind", ""))
	match kind:
		"capture":
			var device_key: String = str(_current_checkpoint.get("device", ""))
			if _trigger_capture_for_device(device_key):
				_awaiting_capture_results = true
			else:
				_waiting_for_user = false
				_pause_timeline(false)
		"dual_capture":
			var ok_a: bool = _trigger_capture_for_device(DEVICE_A_KEY)
			var ok_b: bool = _trigger_capture_for_device(DEVICE_B_KEY)
			if ok_a and ok_b:
				_awaiting_capture_results = true
			else:
				_waiting_for_user = false
				_pause_timeline(false)
		"stream":
			var slot: String = str(_current_checkpoint.get("slot", ""))
			if _bind_stream_slot(slot):
				_waiting_for_user = false
				_advance_checkpoint()
			else:
				_waiting_for_user = false
				_pause_timeline(false)
		"release_streams":
			_release_stream_bindings()
			_waiting_for_user = false
			_advance_checkpoint()
		_:
			_fail("Unknown checkpoint kind %s" % kind)


func _checkpoint_prereq_satisfied(cp: Dictionary) -> bool:
	var kind: String = str(cp.get("kind", ""))
	match kind:
		"capture":
			var device_key: String = str(cp.get("device", ""))
			return int(_device_ids.get(device_key, 0)) != 0
		"stream":
			var slot: String = str(cp.get("slot", ""))
			return int(_stream_ids.get(slot, 0)) != 0
		"dual_capture":
			return int(_device_ids.get(DEVICE_A_KEY, 0)) != 0 and int(_device_ids.get(DEVICE_B_KEY, 0)) != 0
		"release_streams":
			return true
		_:
			return true


func _trigger_capture_for_device(device_key: String) -> bool:
	var device_instance_id: int = int(_device_ids.get(device_key, 0))
	if device_instance_id == 0:
		_append_log("WARN: no device id yet for %s" % device_key)
		return false

	var device: Variant = CamBANGServer.get_device(device_instance_id)
	if device == null:
		_append_log("WARN: get_device returned null for %s" % device_key)
		return false

	var capture_err: int = int(device.trigger_capture())
	if capture_err != OK:
		_append_log("WARN: trigger_capture returned err=%d for %s" % [capture_err, device_key])
		return false

	_pending_captures.append({
		"device": device_key,
		"device_object": device,
		"device_instance_id": device_instance_id,
		"bound": false
	})
	_append_log("Capture requested %s" % device_key)
	_phase_marker("capture_requested", int(_current_checkpoint.get("id", -1)), "device=%s" % device_key)
	return true


func _poll_pending_captures() -> float:
	var display_sec := 0.0
	for i: int in range(_pending_captures.size()):
		var item: Dictionary = _pending_captures[i]
		if bool(item.get("bound", false)):
			continue

		var device_instance_id: int = int(item.get("device_instance_id", 0))
		var device: Variant = item.get("device_object", null)
		if device == null:
			continue
		var result: Variant = device.get_result()
		if result == null:
			continue

		var image: Image = result.to_image()
		if image == null or image.is_empty():
			continue

		var tex: ImageTexture = ImageTexture.create_from_image(image)
		var device_key: String = str(item.get("device", ""))

		var display_start_usec := Time.get_ticks_usec()
		if device_key == DEVICE_A_KEY:
			_capture_a_rect.texture = tex
			_capture_a_facts.text = "Capture A\ndevice_instance_id=%d\nsize=%dx%d" % [
				device_instance_id, image.get_width(), image.get_height()
			]
		else:
			_capture_b_rect.texture = tex
			_capture_b_facts.text = "Capture B\ndevice_instance_id=%d\nsize=%dx%d" % [
				device_instance_id, image.get_width(), image.get_height()
			]

		item["bound"] = true
		_pending_captures[i] = item
		_append_log("Capture bound %s" % device_key)
		display_sec += _elapsed_sec_from_ticks(display_start_usec)
	return display_sec


func _all_pending_captures_bound() -> bool:
	if _pending_captures.is_empty():
		return false
	for item_variant: Variant in _pending_captures:
		var item: Dictionary = item_variant
		if not bool(item.get("bound", false)):
			return false
	_pending_captures.clear()
	return true


func _bind_stream_slot(slot: String) -> bool:
	var stream_id: int = int(_stream_ids.get(slot, 0))
	if stream_id == 0:
		_append_log("WARN: stream id not latched for %s" % slot)
		return false

	# This matrix latches scenario-authored stream ids rather than public
	# CamBANGStream handles, so it uses the advanced server stream-id lookup.
	var result: Variant = CamBANGServer.get_latest_stream_result(stream_id)
	if result == null:
		_append_log("WARN: latest stream result not ready yet for %s" % slot)
		return false

	var view: Variant = result.get_display_view()
	if view == null:
		_append_log("WARN: display view unavailable for %s" % slot)
		return false

	if _display_demand_trace:
		var path_kind: int = -1
		if result.has_method("get_display_view_path_kind"):
			path_kind = int(result.get_display_view_path_kind())
		var tex_id: int = -1
		if view is Object:
			tex_id = int((view as Object).get_instance_id())
		_append_log("DemandTrace bind slot=%s stream_id=%d path_kind=%d texture_id=%d" % [slot, stream_id, path_kind, tex_id])

	match slot:
		SLOT_PREVIEW_A:
			_preview_a_rect.texture = view
			_preview_a_facts.text = "Preview A\nstream_id=%d\nlive display_view bound" % stream_id
		SLOT_VIEWFINDER_A:
			_viewfinder_a_rect.texture = view
			_viewfinder_a_facts.text = "Viewfinder A\nstream_id=%d\nlive display_view bound" % stream_id
		SLOT_PREVIEW_B:
			_preview_b_rect.texture = view
			_preview_b_facts.text = "Preview B\nstream_id=%d\nlive display_view bound" % stream_id
		SLOT_VIEWFINDER_B:
			_viewfinder_b_rect.texture = view
			_viewfinder_b_facts.text = "Viewfinder B\nstream_id=%d\nlive display_view bound" % stream_id
		_:
			return false

	_append_log("Stream bound %s id=%d" % [slot, stream_id])
	_phase_marker("stream_display_bound", int(_current_checkpoint.get("id", -1)), "slot=%s stream_id=%d" % [slot, stream_id])
	return true


func _clear_display_bindings_for_teardown() -> void:
	_preview_a_rect.texture = null
	_viewfinder_a_rect.texture = null
	_preview_b_rect.texture = null
	_viewfinder_b_rect.texture = null
	_capture_a_rect.texture = null
	_capture_b_rect.texture = null


func _release_stream_bindings() -> void:
	_clear_display_bindings_for_teardown()
	_preview_a_facts.text = "Preview A released"
	_viewfinder_a_facts.text = "Viewfinder A released"
	_preview_b_facts.text = "Preview B released"
	_viewfinder_b_facts.text = "Viewfinder B released"
	_append_log("Released stream display_view bindings")
	_phase_marker("stream_bindings_released", int(_current_checkpoint.get("id", -1)), "reason=checkpoint_action" )


func _latch_snapshot_state() -> void:
	var snap: Variant = CamBANGServer.get_state_snapshot()
	if snap == null:
		return

	var devices: Array = snap.get("devices", [])
	for d_variant: Variant in devices:
		var d: Dictionary = d_variant
		var hardware_id: String = str(d.get("hardware_id", ""))
		var instance_id: int = int(d.get("instance_id", 0))

		if hardware_id == HW_A and instance_id != 0:
			if int(_device_ids.get(DEVICE_A_KEY, 0)) == 0:
				_append_log("Latched CameraA instance_id=%d" % instance_id)
			_device_ids[DEVICE_A_KEY] = instance_id
		elif hardware_id == HW_B and instance_id != 0:
			if int(_device_ids.get(DEVICE_B_KEY, 0)) == 0:
				_append_log("Latched CameraB instance_id=%d" % instance_id)
			_device_ids[DEVICE_B_KEY] = instance_id

	var streams: Array = snap.get("streams", [])
	for s_variant: Variant in streams:
		var s: Dictionary = s_variant
		var stream_id: int = int(s.get("stream_id", 0))
		var device_instance_id: int = int(s.get("device_instance_id", 0))
		if stream_id == 0 or device_instance_id == 0:
			continue

		var device_key: String = ""
		if device_instance_id == int(_device_ids.get(DEVICE_A_KEY, 0)):
			device_key = DEVICE_A_KEY
		elif device_instance_id == int(_device_ids.get(DEVICE_B_KEY, 0)):
			device_key = DEVICE_B_KEY
		if device_key == "":
			continue

		var known: Array = _known_stream_ids_by_device.get(device_key, [])
		if not known.has(stream_id):
			known.append(stream_id)
			_known_stream_ids_by_device[device_key] = known
			_append_log("Latched %s stream_id=%d (ordinal=%d)" % [device_key, stream_id, known.size()])

		if device_key == DEVICE_A_KEY:
			if known.size() >= 1:
				_stream_ids[SLOT_PREVIEW_A] = int(known[0])
			if known.size() >= 2:
				_stream_ids[SLOT_VIEWFINDER_A] = int(known[1])
		else:
			if known.size() >= 1:
				_stream_ids[SLOT_PREVIEW_B] = int(known[0])
			if known.size() >= 2:
				_stream_ids[SLOT_VIEWFINDER_B] = int(known[1])


func _advance_checkpoint() -> void:
	_checkpoint_index += 1
	_current_checkpoint = {}
	_pause_timeline(false)
	_update_instruction()


func _pause_timeline(paused: bool) -> void:
	if CamBANGServer.has_method("set_timeline_paused"):
		var err: int = int(CamBANGServer.set_timeline_paused(paused))
		_append_log("set_timeline_paused(%s) -> %d" % [str(paused), err])
		_phase_marker("timeline_pause_toggle", int(_current_checkpoint.get("id", -1)), "paused=%s err=%d" % [str(paused), err])
		_require(err == OK, "set_timeline_paused(%s) failed: %d" % [str(paused), err])


func _update_instruction() -> void:
	if _checkpoint_index >= CHECKPOINTS.size():
		_instructions.text = "[b]All checkpoints complete.[/b]\nAuthored teardown is now running to close both devices."
		return
	var cp: Dictionary = CHECKPOINTS[_checkpoint_index]
	_instructions.text = "[b]Checkpoint %d[/b]\n%s\nPress Space / Enter to perform the scene-owned action." % [
		int(cp.get("id", 0)),
		str(cp.get("desc", ""))
	]


func _maybe_log_virtual_time() -> void:
	var whole_second: int = int(floor(_virtual_time_s + LOG_SECOND_EPS))
	if whole_second > _last_logged_second:
		_last_logged_second = whole_second
		_append_log("virtual_time_s=%.1f" % _virtual_time_s)
		_maybe_log_timing_per_second()


func _append_log(msg: String) -> void:
	var log_start_usec := Time.get_ticks_usec()
	_log.text += msg + "\n"
	_log.scroll_to_line(_log.get_line_count())
	var log_sec := _elapsed_sec_from_ticks(log_start_usec)
	_timing_log_total_sec += log_sec
	_timing_log_max_sec = max(_timing_log_max_sec, log_sec)
	_timing_log_calls += 1


func _maybe_log_timing_per_second() -> void:
	_append_log("TIMING scene_process_max_ms=%.3f checkpoint_max_ms=%.3f poll_max_ms=%.3f display_max_ms=%.3f status_max_ms=%.3f log_max_ms=%.3f" % [
		_timing_process_max_sec * 1000.0,
		_timing_checkpoint_max_sec * 1000.0,
		_timing_poll_max_sec * 1000.0,
		_timing_display_max_sec * 1000.0,
		_timing_status_max_sec * 1000.0,
		_timing_log_max_sec * 1000.0
	])


func _elapsed_sec_from_ticks(start_usec: int) -> float:
	return float(Time.get_ticks_usec() - start_usec) / 1000000.0


func _record_process_timing(process_start_usec: int) -> void:
	var process_sec := _elapsed_sec_from_ticks(process_start_usec)
	_timing_process_total_sec += process_sec
	_timing_process_max_sec = max(_timing_process_max_sec, process_sec)
	_timing_process_calls += 1


func _log_summary_timing_once() -> void:
	if _summary_timing_logged:
		return
	_summary_timing_logged = true
	_phase_marker("teardown_summary")
	_append_log("SUMMARY_TIMING scene_process_max_ms=%.3f scene_process_total_ms=%.3f scene_process_calls=%d checkpoint_max_ms=%.3f checkpoint_total_ms=%.3f checkpoint_calls=%d poll_max_ms=%.3f poll_total_ms=%.3f poll_calls=%d display_max_ms=%.3f display_total_ms=%.3f display_calls=%d status_max_ms=%.3f status_total_ms=%.3f status_calls=%d log_max_ms=%.3f log_total_ms=%.3f log_calls=%d" % [
		_timing_process_max_sec * 1000.0, _timing_process_total_sec * 1000.0, _timing_process_calls,
		_timing_checkpoint_max_sec * 1000.0, _timing_checkpoint_total_sec * 1000.0, _timing_checkpoint_calls,
		_timing_poll_max_sec * 1000.0, _timing_poll_total_sec * 1000.0, _timing_poll_calls,
		_timing_display_max_sec * 1000.0, _timing_display_total_sec * 1000.0, _timing_display_calls,
		_timing_status_max_sec * 1000.0, _timing_status_total_sec * 1000.0, _timing_status_calls,
		_timing_log_max_sec * 1000.0, _timing_log_total_sec * 1000.0, _timing_log_calls
	])
	print("[CamBANG][Scene71] completed=true quitting=true")
	_append_log("[CamBANG][Scene71] completed=true quitting=true")
	_clear_display_bindings_for_teardown()
	_pending_captures.clear()
	CamBANGServer.stop()
	await get_tree().create_timer(10.0).timeout
	get_tree().quit(0)


func _require(cond: bool, msg: String) -> void:
	if not cond:
		_fail(msg)

func _fail(msg: String) -> void:
	push_error(msg)
	_append_log("FAIL: " + msg)
	_log_summary_timing_once()
