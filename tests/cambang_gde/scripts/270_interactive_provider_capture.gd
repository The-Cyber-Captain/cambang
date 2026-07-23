extends Control

## Scene 270: interactive provider + capture (first 2## human-interactive harness)
##
## Role:
## - the first 2## scene. 2## = human-interactive, no scenario, topology built
##   through the public GDScript Device/Stream API. Where 70 auto-runs a fixed
##   sequence and emits a harness verdict, 270 is driven by the maintainer: pick
##   a provider, watch the live stream, press Capture, read the realized camera
##   facts. There is no authoritative pass/fail verdict -- the on-screen state is
##   the truth, and acceptance is a maintainer interactive run.
##
## What it demonstrates:
## - switching between the synthetic and platform-backed providers at runtime
##   (stop -> start(kind) -> enumerate -> engage -> create_stream -> start), the
##   same public-API topology 768 verifies, but interactive and visual
## - the interactive Android camera-permission flow, which needs a human at the
##   OS dialog and so belongs here rather than in a windowless verifier
## - a single still capture and its realized per-image camera facts (exposure,
##   sensitivity, aperture, focal length, focus, acquisition timing) -- the facts
##   the platform providers were built to report truthfully
##
## Deliberately thin (MVP): single default capture only. Bracket-bundle display
## (multiple members, per-member realized EV) is a fast follow, not here yet.
##
## Cross-platform: the only platform-specific code is the small
## `if OS.get_name() == "Android"` permission branch (the design-rule-permitted
## harness check). On Windows/WinRT the consent model is permissive and the
## branch no-ops.

const SCENE_LABEL := "270_interactive_provider_capture"

## Camera prerequisites for the platform-backed provider. Listed here so the
## permission flow is data-driven; a Horizon headset would add
## "horizonos.permission.HEADSET_CAMERA" (out of scope until the device is here).
const CAMERA_PERMISSION_PREREQUISITES := ["android.permission.CAMERA"]

const STREAM_WIDTH := 640
const STREAM_HEIGHT := 360
const CAPTURE_POLL_TIMEOUT_MS := 8000

var _provider_kind := ""            # "" | "synthetic" | "platform_backed"
var _device = null                  # CamBANGDevice handle while engaged
var _stream = null                  # CamBANGStream handle while started
var _stream_id := 0
var _device_instance_id := 0
var _busy := false                  # a provider switch / capture is in flight
var _pending_after_permissions: Callable = Callable()
var _perm_signal_connected := false

# --- UI nodes (built programmatically to keep the scene self-contained) ---
var _synthetic_button: Button
var _platform_button: Button
var _stop_button: Button
var _capture_button: Button
var _status_label: Label
var _stream_view: TextureRect
var _capture_view: TextureRect
var _facts_label: Label


func _ready() -> void:
	_build_ui()
	_set_status("Idle. Choose a provider to begin.")
	print("RUN: %s (interactive; no harness verdict -- on-screen state is authoritative)" % SCENE_LABEL)


func _exit_tree() -> void:
	# Leave the runtime clean if the scene is torn down mid-session.
	if _perm_signal_connected and get_tree() and get_tree().on_request_permissions_result.is_connected(_on_request_permissions_result):
		get_tree().on_request_permissions_result.disconnect(_on_request_permissions_result)
	CamBANGServer.stop()


# ---------------------------------------------------------------------------
# UI
# ---------------------------------------------------------------------------

# Generous sizing: this runs full-screen on a handset, so controls must be
# thumb-sized and text legible, with margin off the screen edges (and clear of
# any display cutout/status bar).
const UI_MARGIN := 40
const BUTTON_MIN_SIZE := Vector2(300, 96)
const BUTTON_FONT_SIZE := 34
const STATUS_FONT_SIZE := 32
const FACTS_FONT_SIZE := 28
const TITLE_FONT_SIZE := 28


func _build_ui() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)

	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_%s" % side, UI_MARGIN)
	add_child(margin)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 20)
	margin.add_child(root)

	var controls := HBoxContainer.new()
	controls.add_theme_constant_override("separation", 20)
	root.add_child(controls)

	_synthetic_button = _make_button("Synthetic", controls, _on_synthetic_pressed)
	_platform_button = _make_button("Platform-backed", controls, _on_platform_pressed)
	_stop_button = _make_button("Stop", controls, _on_stop_pressed)
	_capture_button = _make_button("Capture", controls, _on_capture_pressed)
	_capture_button.disabled = true
	_stop_button.disabled = true

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.add_theme_font_size_override("font_size", STATUS_FONT_SIZE)
	root.add_child(_status_label)

	var views := HBoxContainer.new()
	views.add_theme_constant_override("separation", 20)
	views.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(views)

	_stream_view = _make_image_view("Live stream", views)
	_capture_view = _make_image_view("Last capture", views)

	_facts_label = Label.new()
	_facts_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_facts_label.add_theme_font_size_override("font_size", FACTS_FONT_SIZE)
	_facts_label.text = "Capture facts will appear here."
	root.add_child(_facts_label)


func _make_button(text: String, parent: Node, handler: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = BUTTON_MIN_SIZE
	b.add_theme_font_size_override("font_size", BUTTON_FONT_SIZE)
	b.pressed.connect(handler)
	parent.add_child(b)
	return b


func _make_image_view(title: String, parent: Node) -> TextureRect:
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var label := Label.new()
	label.text = title
	label.add_theme_font_size_override("font_size", TITLE_FONT_SIZE)
	box.add_child(label)
	var view := TextureRect.new()
	view.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	view.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	view.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	view.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(view)
	parent.add_child(box)
	return view


func _set_status(text: String) -> void:
	if _status_label:
		_status_label.text = "[%s] %s" % [_provider_kind if _provider_kind != "" else "none", text]
	print("270: %s" % text)


func _set_controls_enabled(enabled: bool) -> void:
	_synthetic_button.disabled = not enabled
	_platform_button.disabled = not enabled
	_stop_button.disabled = not enabled or _provider_kind == ""
	_capture_button.disabled = not enabled or _stream == null


# ---------------------------------------------------------------------------
# Button handlers
# ---------------------------------------------------------------------------

func _on_synthetic_pressed() -> void:
	if _busy:
		return
	_start_provider("synthetic")


func _on_platform_pressed() -> void:
	if _busy:
		return
	# Camera permission is required for the platform-backed provider on Android.
	# Ensure it before starting; the callback runs only once prerequisites hold.
	_ensure_camera_permissions_then(func() -> void: _start_provider("platform_backed"))


func _on_stop_pressed() -> void:
	if _busy:
		return
	_teardown_topology()
	CamBANGServer.stop()
	_provider_kind = ""
	_set_status("Stopped.")
	_set_controls_enabled(true)


func _on_capture_pressed() -> void:
	if _busy or _stream == null or _device == null:
		return
	_capture_once()


# ---------------------------------------------------------------------------
# Permission flow (Android only; no-op elsewhere)
# ---------------------------------------------------------------------------

func _ensure_camera_permissions_then(callback: Callable) -> void:
	if OS.get_name() != "Android":
		# Windows/WinRT and desktop consent models are permissive here.
		callback.call()
		return
	if _camera_prerequisites_granted():
		callback.call()
		return

	# Not yet granted: request, and resume via the result signal. The granted
	# list re-check is the source of truth; request_permissions()'s return value
	# is not trusted (see docs/dev/upstream_discrepancies.md).
	_pending_after_permissions = callback
	_connect_permission_signal()
	_set_status("Requesting camera permission...")
	OS.request_permissions()


func _connect_permission_signal() -> void:
	if _perm_signal_connected:
		return
	var tree := get_tree()
	if tree and tree.has_signal("on_request_permissions_result"):
		tree.on_request_permissions_result.connect(_on_request_permissions_result)
		_perm_signal_connected = true
	else:
		# No signal to await (unexpected on Android). Fail closed rather than hang.
		_set_status("Permission request unavailable on this platform build; cannot start platform-backed provider.")
		_pending_after_permissions = Callable()


func _on_request_permissions_result(permission: String, granted: bool) -> void:
	# Only prerequisites concern us; ignore unrelated results and keep waiting.
	if not (permission in CAMERA_PERMISSION_PREREQUISITES):
		return
	if not granted:
		_pending_after_permissions = Callable()
		# Android will not prompt again for an explicitly denied permission, so
		# do not imply an in-app retry works. The only in-app route left is to
		# grant it in system Settings and then reselect (the granted-list
		# re-check passes without a fresh prompt). Synthetic needs no permission.
		_set_status("Camera permission denied. Android will not ask again -- grant it in system Settings, then reselect Platform-backed. (Synthetic needs no permission.)")
		return
	# Granted -- re-check the full prerequisite set (there may be more than one).
	if _camera_prerequisites_granted():
		var callback := _pending_after_permissions
		_pending_after_permissions = Callable()
		if callback.is_valid():
			callback.call()
	# else: other prerequisites still outstanding; keep waiting for their results.


func _camera_prerequisites_granted() -> bool:
	var granted := OS.get_granted_permissions()
	for prerequisite in CAMERA_PERMISSION_PREREQUISITES:
		if not (prerequisite in granted):
			return false
	return true


# ---------------------------------------------------------------------------
# Provider lifecycle + topology (public GDScript API, no scenario)
# ---------------------------------------------------------------------------

func _start_provider(kind: String) -> void:
	_busy = true
	_set_controls_enabled(false)
	_teardown_topology()
	CamBANGServer.stop()
	_provider_kind = kind
	_set_status("Starting %s provider..." % kind)

	var start_err := (
		CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC) if kind == "synthetic"
		else CamBANGServer.start()
	)
	if start_err != OK:
		_provider_kind = ""
		_set_status("start() failed (%d)." % start_err)
		_busy = false
		_set_controls_enabled(true)
		return

	# Let the runtime publish its baseline. Provider/topology operations are
	# marshalled to the core thread and processed between frames, so this whole
	# sequence must yield frames rather than run synchronously in one.
	await _wait_for_snapshot(2000)

	var endpoints: Array = CamBANGServer.enumerate_devices()
	if endpoints.is_empty():
		_set_status("Provider started but no camera enumerated. Nothing to display.")
		_busy = false
		_set_controls_enabled(true)
		return
	var endpoint: Dictionary = endpoints[0]
	var hardware_id := str(endpoint.get("hardware_id", ""))
	var display_name := str(endpoint.get("name", ""))

	_device = CamBANGServer.get_device_for_hardware_id(hardware_id)
	if _device == null or int(_device.engage()) != OK:
		_device = null
		_set_status("Failed to engage camera '%s'." % display_name)
		_busy = false
		_set_controls_enabled(true)
		return
	_device_instance_id = int(_device.get_instance_id())

	# engage() is an admission; the device open completes on the core thread.
	# Wait for the handle to report live before creating a stream on it.
	if not await _wait_device_live(_device, 4000):
		_set_status("Camera '%s' engaged but did not become live." % display_name)
		_busy = false
		_set_controls_enabled(true)
		return

	_stream = _device.create_stream({
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": STREAM_WIDTH,
			"height": STREAM_HEIGHT,
			"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
		},
	})
	if _stream == null:
		_set_status("create_stream() returned null on '%s' (width=%d height=%d)." % [
			display_name, STREAM_WIDTH, STREAM_HEIGHT
		])
		_busy = false
		_set_controls_enabled(true)
		return
	_stream_id = int(_stream.get_stream_id())

	# Yield a frame so the core thread realizes the created stream before start.
	await get_tree().process_frame
	var stream_start_err := int(_stream.start())
	if stream_start_err != OK:
		_set_status("stream.start() failed (err=%d) on '%s' (stream_id=%d)." % [
			stream_start_err, display_name, _stream_id
		])
		_stream.destroy()
		_stream = null
		_stream_id = 0
		_busy = false
		_set_controls_enabled(true)
		return

	# Align the still-capture geometry to the stream. Camera2 and WinRT refuse a
	# still whose geometry differs from a live stream (they share one capture
	# session; synthetic has no such coupling), so without this a capture WHILE
	# streaming is rejected with ERR_UNAVAILABLE on the platform providers. This
	# is the real product pattern: a viewfinder and its stills at one geometry.
	_device.set_still_capture_profile({
		"width": STREAM_WIDTH,
		"height": STREAM_HEIGHT,
		"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
	})
	await _wait_still_profile(STREAM_WIDTH, STREAM_HEIGHT, 2000)

	_set_status("Live on '%s'. Press Capture." % display_name)
	_busy = false
	_set_controls_enabled(true)


func _wait_for_snapshot(timeout_ms: int) -> void:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() != null:
			return


func _wait_still_profile(width: int, height: int, timeout_ms: int) -> void:
	# Wait for the still-capture profile change to be reflected before the user
	# can capture, so the first Capture is admitted rather than racing the set.
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if _device == null:
			return
		var p = _device.get_still_capture_profile()
		if typeof(p) == TYPE_DICTIONARY and int((p as Dictionary).get("width", 0)) == width and int((p as Dictionary).get("height", 0)) == height:
			return


func _wait_device_live(device, timeout_ms: int) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if device == null:
			return false
		if device.has_method("is_live") and bool(device.is_live()):
			return true
	return false


func _teardown_topology() -> void:
	if _stream != null:
		_stream.stop()
		_stream.destroy()
		_stream = null
	if _device != null:
		_device.disengage()
		_device = null
	_stream_id = 0
	_device_instance_id = 0
	_stream_view.texture = null
	_capture_button.disabled = true


func _capture_once() -> void:
	_busy = true
	_set_controls_enabled(false)
	_set_status("Capturing...")

	var capture_err := int(_device.trigger_capture())
	if capture_err != OK:
		_set_status("trigger_capture() rejected (%d)." % capture_err)
		_busy = false
		_set_controls_enabled(true)
		return

	var deadline := Time.get_ticks_msec() + CAPTURE_POLL_TIMEOUT_MS
	var capture_result = null
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		capture_result = _device.get_result()
		if capture_result != null:
			break
	if capture_result == null:
		_set_status("Capture returned no result within %d ms." % CAPTURE_POLL_TIMEOUT_MS)
		_busy = false
		_set_controls_enabled(true)
		return

	_show_capture_result(capture_result)
	_busy = false
	_set_controls_enabled(true)


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

func _process(_delta: float) -> void:
	# Keep the live stream view fresh from whatever the provider is delivering.
	if _stream_id == 0:
		return
	var stream_result = CamBANGServer.get_stream_result_by_stream_id(_stream_id)
	if stream_result == null:
		return
	var display_view = stream_result.get_display_view()
	if display_view is Texture2D:
		_stream_view.texture = display_view


func _show_capture_result(capture_result) -> void:
	var member_count := int(capture_result.get_image_count())
	var image = null
	if int(capture_result.can_to_image_member(0)) != int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED):
		image = capture_result.to_image_member(0)
	if image is Image:
		_capture_view.texture = ImageTexture.create_from_image(image)

	var member: Dictionary = capture_result.get_image_member(0)
	_facts_label.text = _format_member_facts(member, member_count)
	_set_status("Captured (%d member%s)." % [member_count, "" if member_count == 1 else "s"])


func _format_member_facts(member: Dictionary, member_count: int) -> String:
	var lines: Array[String] = []
	lines.append("members: %d   role: %s" % [member_count, str(member.get("role_name", "?"))])
	lines.append("requested EV (milli): %d   realized EV (milli): %s" % [
		int(member.get("applied_exposure_compensation_milli_ev", 0)),
		str(int(member.get("realized_exposure_compensation_milli_ev", 0))) if bool(member.get("has_realized_exposure_compensation_milli_ev", false)) else "absent",
	])
	var facts_v = member.get("camera_facts", {})
	if typeof(facts_v) == TYPE_DICTIONARY:
		var facts: Dictionary = facts_v
		lines.append(_fact_line(facts, "exposure_time", "nanoseconds", "exposure_ns"))
		lines.append(_fact_line(facts, "sensor_sensitivity_iso", "iso_equivalent", "iso"))
		lines.append(_fact_line(facts, "aperture_f_number", "f_number", "f"))
		lines.append(_fact_line(facts, "focal_length_mm", "millimetres", "focal_mm"))
		if facts.has("focus_state"):
			lines.append("focus_state: %s" % JSON.stringify(facts["focus_state"]))
		if facts.has("acquisition_timing"):
			lines.append("acquisition_timing: %s" % JSON.stringify(facts["acquisition_timing"]))
		if facts.has("intrinsics"):
			lines.append("intrinsics: present (%s domain)" % str((facts["intrinsics"] as Dictionary).get("coordinate_domain", "?")))
	else:
		lines.append("(no camera_facts reported for this member)")
	return "\n".join(lines)


func _fact_line(facts: Dictionary, key: String, value_field: String, label: String) -> String:
	if not facts.has(key):
		return "%s: absent" % label
	var entry_v = facts[key]
	if typeof(entry_v) != TYPE_DICTIONARY:
		return "%s: (malformed)" % label
	var entry: Dictionary = entry_v
	return "%s: %s (origin=%s)" % [label, str(entry.get(value_field, "?")), str(entry.get("origin", "?"))]
