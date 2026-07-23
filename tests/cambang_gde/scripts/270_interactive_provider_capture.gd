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
## - a still capture (single or an N-member exposure bracket) and its realized
##   per-member camera facts: requested vs realized EV, realised exposure/ISO,
##   burst-timing marks, plus a member-coherence check that the fields a bracket
##   must hold constant (aperture, focal length, focus distance) actually match
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

# Fixed outer bracket spread. A bracket's most-compensated members sit at
# +/-SPREAD; intermediate members (5- and 7-shot) are distributed evenly inside
# it. The spread is fixed on purpose -- only the member COUNT is user-selectable
# (see tranche scope); a tunable spread is a later follow.
const BRACKET_SPREAD_MILLI_EV := 2000

# Selectable member counts. 1 == single capture (no bracket); 3/5/7 == brackets.
const MEMBER_COUNT_CHOICES := [1, 3, 5, 7]

# Float equality tolerance for the coherence check (aperture / focal / distance).
const COHERENCE_EPSILON := 1.0e-4

var _provider_kind := ""            # "" | "synthetic" | "platform_backed"
var _device = null                  # CamBANGDevice handle while engaged
var _stream = null                  # CamBANGStream handle while started
var _stream_id := 0
var _device_instance_id := 0
var _busy := false                  # a provider switch / capture is in flight
var _pending_after_permissions: Callable = Callable()
var _perm_signal_connected := false

var _bracket_enabled := false
var _member_count := 3              # count used when bracket is enabled
# Signature of the still bundle last pushed via set_still_capture_profile(). The
# still profile is retained by the provider until changed, and re-setting it can
# force a capture-session reconfigure on the platform providers, so we only push
# it when the bundle actually changes -- repeat captures at the same member count
# then skip a costly reconfigure. Reset on teardown so the next session re-pushes.
var _last_bundle_signature := ""

# --- UI nodes (built programmatically to keep the scene self-contained) ---
var _synthetic_button: Button
var _platform_button: Button
var _stop_button: Button
var _capture_button: Button
var _bracket_toggle: CheckButton
var _members_option: OptionButton
var _status_label: Label
var _stream_view: TextureRect
var _prime_view: TextureRect              # member 0 shown large ("prime")
var _coherence_label: Label
var _timing_label: Label
var _member_strip_row: HBoxContainer      # one compact card per member (thumbnail + facts)
var _prime_facts_label: Label             # full facts for member 0, in a scroll box


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

# This runs full-screen on a handset, typically LANDSCAPE and short (~900px
# tall): horizontal space is abundant, vertical is scarce. So the images are the
# vertical-flex hero and everything else is compact/fixed. The earlier layout
# stacked two ~240px scroll boxes plus titles and overflowed the viewport, which
# starved the image row to nothing -- hence these tightened, budgeted sizes.
const UI_MARGIN := 24
const BUTTON_MIN_SIZE := Vector2(280, 88)
const BUTTON_FONT_SIZE := 32
const STATUS_FONT_SIZE := 30
const TITLE_FONT_SIZE := 24
const COHERENCE_FONT_SIZE := 28
const TIMING_FONT_SIZE := 26
# Member cards and the prime-member facts are denser: smaller fonts, fixed
# heights, and scrollable so a 7-shot bracket + long fact list stay usable.
const CARD_FONT_SIZE := 20
const PRIME_FACTS_FONT_SIZE := 23
const MEMBER_THUMB_MIN := Vector2(170, 104)
const MEMBER_STRIP_HEIGHT := 196
const PRIME_FACTS_HEIGHT := 116


func _build_ui() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)

	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_%s" % side, UI_MARGIN)
	add_child(margin)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 8)
	margin.add_child(root)

	var controls := HBoxContainer.new()
	controls.add_theme_constant_override("separation", 16)
	root.add_child(controls)

	_synthetic_button = _make_button("Synthetic", controls, _on_synthetic_pressed)
	_platform_button = _make_button("Platform-backed", controls, _on_platform_pressed)
	_stop_button = _make_button("Stop", controls, _on_stop_pressed)
	_capture_button = _make_button("Capture", controls, _on_capture_pressed)
	_capture_button.disabled = true
	_stop_button.disabled = true

	_bracket_toggle = CheckButton.new()
	_bracket_toggle.text = "Bracket (±%d mEV)" % BRACKET_SPREAD_MILLI_EV
	_bracket_toggle.add_theme_font_size_override("font_size", BUTTON_FONT_SIZE)
	_bracket_toggle.toggled.connect(_on_bracket_toggled)
	controls.add_child(_bracket_toggle)

	_members_option = OptionButton.new()
	_members_option.add_theme_font_size_override("font_size", BUTTON_FONT_SIZE)
	_members_option.custom_minimum_size = Vector2(220, 88)
	for count in MEMBER_COUNT_CHOICES:
		_members_option.add_item("%d members" % count if count > 1 else "1 (single)", count)
	_members_option.select(MEMBER_COUNT_CHOICES.find(_member_count))
	_members_option.item_selected.connect(_on_members_selected)
	controls.add_child(_members_option)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.add_theme_font_size_override("font_size", STATUS_FONT_SIZE)
	root.add_child(_status_label)

	# Main image row is the vertical-flex hero: live stream and the prime
	# captured member side by side, prime given the larger share of the width.
	var views := HBoxContainer.new()
	views.add_theme_constant_override("separation", 20)
	views.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(views)

	_stream_view = _make_image_view("Live stream", views, 2.0)
	_prime_view = _make_image_view("Prime capture (member 0)", views, 3.0)

	# Coherence banner and timing line: one prominent line each.
	_coherence_label = Label.new()
	_coherence_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_coherence_label.add_theme_font_size_override("font_size", COHERENCE_FONT_SIZE)
	root.add_child(_coherence_label)

	_timing_label = Label.new()
	_timing_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_timing_label.add_theme_font_size_override("font_size", TIMING_FONT_SIZE)
	root.add_child(_timing_label)

	# Member thumbnail strip: a fixed-height, horizontally scrollable row of
	# per-member cards (thumbnail + compact realised facts). The cards self-label
	# ("m0 metered" etc.), so no separate title line is spent on it here.
	var strip_scroll := ScrollContainer.new()
	strip_scroll.custom_minimum_size = Vector2(0, MEMBER_STRIP_HEIGHT)
	strip_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	strip_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	root.add_child(strip_scroll)
	_member_strip_row = HBoxContainer.new()
	_member_strip_row.add_theme_constant_override("separation", 14)
	strip_scroll.add_child(_member_strip_row)

	# Member-0 detailed facts: smaller font, fixed-height scroll box.
	var facts_title := Label.new()
	facts_title.text = "Member 0 detailed facts"
	facts_title.add_theme_font_size_override("font_size", TITLE_FONT_SIZE)
	root.add_child(facts_title)

	var facts_scroll := ScrollContainer.new()
	facts_scroll.custom_minimum_size = Vector2(0, PRIME_FACTS_HEIGHT)
	facts_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	facts_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	root.add_child(facts_scroll)
	_prime_facts_label = Label.new()
	_prime_facts_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_prime_facts_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_prime_facts_label.add_theme_font_size_override("font_size", PRIME_FACTS_FONT_SIZE)
	_prime_facts_label.text = "Capture facts will appear here."
	facts_scroll.add_child(_prime_facts_label)


func _make_button(text: String, parent: Node, handler: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = BUTTON_MIN_SIZE
	b.add_theme_font_size_override("font_size", BUTTON_FONT_SIZE)
	b.pressed.connect(handler)
	parent.add_child(b)
	return b


func _make_image_view(title: String, parent: Node, stretch_ratio: float) -> TextureRect:
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.size_flags_stretch_ratio = stretch_ratio
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
	# Member count only matters while bracket is on and we are idle.
	_members_option.disabled = not enabled or not _bracket_enabled


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


func _on_bracket_toggled(pressed: bool) -> void:
	_bracket_enabled = pressed
	_members_option.disabled = not pressed or _busy


func _on_members_selected(index: int) -> void:
	_member_count = int(_members_option.get_item_id(index))


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
	_last_bundle_signature = ""
	_stream_view.texture = null
	_prime_view.texture = null
	_clear_member_strip()
	_coherence_label.text = ""
	_timing_label.text = ""
	_capture_button.disabled = true


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------

func _capture_once() -> void:
	_busy = true
	_set_controls_enabled(false)
	var bracket := _bracket_enabled
	var count := _member_count if bracket else 1
	# Wall-clock stopwatch (Godot main-thread clock). This measures the whole
	# button-press -> result round-trip, which is a DIFFERENT clock domain from
	# the provider's acquisition marks -- the two are never cross-subtracted.
	var press_us := Time.get_ticks_usec()
	_set_status("Capturing %s (%d member%s)..." % [
		"bracket" if bracket else "single", count, "" if count == 1 else "s"
	])

	# Set the still profile to the requested bundle only when it has changed since
	# the last capture. Geometry stays aligned to the stream (capture-while-
	# streaming); the bundle is single or an N-member bracket. Re-pushing an
	# unchanged bundle can force a session reconfigure on the platform providers,
	# so a repeat capture at the same member count skips it. A set/trigger refusal
	# is a capability signal (this member count is refused), not a hard failure.
	var members := _build_bundle_members(count)
	var signature := _bundle_signature(count, members)
	if signature != _last_bundle_signature:
		var profile := {
			"width": STREAM_WIDTH,
			"height": STREAM_HEIGHT,
			"format_fourcc": CamBANGServer.PIXEL_FORMAT_RGBA,
			"still_image_bundle": {"members": members},
		}
		var profile_err := int(_device.set_still_capture_profile(profile))
		if profile_err != OK:
			_set_status("set_still_capture_profile() rejected (%d)%s." % [
				profile_err, " -- %d-member bundle refused; try fewer members" % count if count > 1 else ""
			])
			_busy = false
			_set_controls_enabled(true)
			return
		_last_bundle_signature = signature
		await get_tree().process_frame

	var capture_err := int(_device.trigger_capture())
	if capture_err != OK:
		# Refused is a member-count signal, not "no bracket support": smaller
		# counts work on this same camera. The provider caps the bundle size
		# (a policy cap, identical on every device -- not a per-device limit),
		# so this is "too many members for the provider", not "no bracket".
		_set_status("trigger_capture() refused (%d)%s. (Previous result below is stale.)" % [
			capture_err,
			" -- %d-member bracket exceeds the provider's bracket cap; try fewer members (brackets ARE supported here)" % count if count > 1 else ""
		])
		# The refused bundle never took effect for a subsequent single capture;
		# force a re-push next time so a later capture is not skipped in error.
		_last_bundle_signature = ""
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
	var result_us := Time.get_ticks_usec()

	_show_capture_result(capture_result, press_us, result_us)
	_busy = false
	_set_controls_enabled(true)


func _build_bundle_members(count: int) -> Array:
	# Member 0 is always the default-metered reference at 0 milli-EV (the bundle
	# contract requires index 0 to be DEFAULT_METERED). Any additional members
	# are ADDITIONAL_BRACKET at the EV stops from _bracket_ev_stops().
	var stops := _bracket_ev_stops(count)
	var members := []
	for i in range(stops.size()):
		members.append({
			"image_member_index": i,
			"role": CamBANGCaptureResult.IMAGE_ROLE_DEFAULT_METERED if i == 0 else CamBANGCaptureResult.IMAGE_ROLE_ADDITIONAL_BRACKET,
			"intended_exposure_compensation_milli_ev": int(stops[i]),
		})
	return members


func _bundle_signature(count: int, members: Array) -> String:
	# Stable identity for a bundle at the current geometry, so an unchanged bundle
	# is not re-pushed to the provider between captures.
	return "%dx%d:%d:%s" % [STREAM_WIDTH, STREAM_HEIGHT, count, JSON.stringify(members)]


func _bracket_ev_stops(count: int) -> Array:
	# EV stops for an N-member bundle: member 0 at 0, the rest distributed
	# symmetrically across +/-SPREAD. For 3 -> [0, -S, +S]; 5 -> [0, -S/2, +S/2,
	# -S, +S]; 7 -> [0, -S/3, +S/3, -2S/3, +2S/3, -S, +S]. Even counts (not in
	# the UI choices, but supported) get one extra member on the low side.
	var stops := [0]
	if count <= 1:
		return stops
	var pairs := (count - 1) / 2
	for i in range(1, pairs + 1):
		var ev := int(round(float(BRACKET_SPREAD_MILLI_EV) * float(i) / float(pairs)))
		stops.append(-ev)
		stops.append(ev)
	if (count - 1) % 2 == 1:
		stops.append(-BRACKET_SPREAD_MILLI_EV)
	return stops


# ---------------------------------------------------------------------------
# Live stream display
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


# ---------------------------------------------------------------------------
# Capture result display
# ---------------------------------------------------------------------------

func _clear_member_strip() -> void:
	if _member_strip_row == null:
		return
	for child in _member_strip_row.get_children():
		child.queue_free()


func _show_capture_result(capture_result, press_us: int, result_us: int) -> void:
	var member_count := int(capture_result.get_image_count())
	_clear_member_strip()

	# Gather every member up front: metadata, camera facts, thumbnail, mark.
	var members: Array = []
	for i in range(member_count):
		members.append(_gather_member(capture_result, i))

	# Prime view: member 0 large, with its full facts in the scroll box.
	if member_count > 0:
		var prime: Dictionary = members[0]
		_prime_view.texture = prime.get("texture", null)
		_prime_facts_label.text = _format_member_facts(prime.get("member", {}), member_count)

	# Thumbnail row: all members, ordered by requested EV so it reads dark->bright.
	# Member 0 (0 EV) sits where its compensation places it in the ramp.
	var ordered: Array = members.duplicate()
	ordered.sort_custom(func(a, b): return int(a.get("applied_ev", 0)) < int(b.get("applied_ev", 0)))
	var mark0_ns: int = int(members[0].get("mark_ns", 0)) if member_count > 0 else 0
	var mark0_has: bool = bool(members[0].get("mark_has", false)) if member_count > 0 else false
	for entry_v in ordered:
		var entry: Dictionary = entry_v
		_member_strip_row.add_child(_make_member_card(entry, mark0_ns, mark0_has))

	# Coherence check + timing diagnostics.
	var coherence := _evaluate_member_coherence(members)
	_apply_coherence_banner(coherence, member_count)
	_apply_timing_line(members, press_us, result_us)

	_set_status("Captured %d member%s%s." % [
		member_count, "" if member_count == 1 else "s",
		" (bracket)" if member_count > 1 else "",
	])


func _gather_member(capture_result, index: int) -> Dictionary:
	var member: Dictionary = capture_result.get_image_member(index)
	var facts_v = member.get("camera_facts", {})
	var facts: Dictionary = facts_v if typeof(facts_v) == TYPE_DICTIONARY else {}
	var texture: Texture2D = null
	if int(capture_result.can_to_image_member(index)) != int(CamBANGCaptureResult.CAPABILITY_UNSUPPORTED):
		var image = capture_result.to_image_member(index)
		if image is Image:
			texture = ImageTexture.create_from_image(image)
	var mark := _member_mark_ns(facts)
	return {
		"index": index,
		"member": member,
		"facts": facts,
		"texture": texture,
		"applied_ev": int(member.get("applied_exposure_compensation_milli_ev", 0)),
		"has_realized_ev": bool(member.get("has_realized_exposure_compensation_milli_ev", false)),
		"realized_ev": int(member.get("realized_exposure_compensation_milli_ev", 0)),
		"role_name": str(member.get("role_name", "?")),
		"mark_has": bool(mark.get("has", false)),
		"mark_ns": int(mark.get("ns", 0)),
	}


func _member_mark_ns(facts: Dictionary) -> Dictionary:
	# acquisition_timing.acquisition_mark is the provider-monotonic tick with
	# tick_period 1/1 ns, so the mark is already a nanosecond count.
	var timing_v = facts.get("acquisition_timing", null)
	if typeof(timing_v) != TYPE_DICTIONARY:
		return {"has": false, "ns": 0}
	var timing: Dictionary = timing_v
	if not timing.has("acquisition_mark"):
		return {"has": false, "ns": 0}
	return {"has": true, "ns": int(timing.get("acquisition_mark", 0))}


func _make_member_card(entry: Dictionary, mark0_ns: int, mark0_has: bool) -> VBoxContainer:
	var card := VBoxContainer.new()
	card.add_theme_constant_override("separation", 6)

	var view := TextureRect.new()
	view.custom_minimum_size = MEMBER_THUMB_MIN
	view.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	view.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	view.texture = entry.get("texture", null)
	card.add_child(view)

	var facts: Dictionary = entry.get("facts", {})
	var exp_ms := _exposure_ms_text(facts)
	var iso_text := _iso_text(facts)
	var realized_ev := str(int(entry.get("realized_ev", 0))) if bool(entry.get("has_realized_ev", false)) else "absent"
	var mark_text := "ref"
	if int(entry.get("index", 0)) == 0:
		mark_text = "ref (0.0)"
	elif bool(entry.get("mark_has", false)) and mark0_has:
		mark_text = "%+0.2f ms" % ((int(entry.get("mark_ns", 0)) - mark0_ns) / 1.0e6)
	else:
		mark_text = "n/a"

	var label := Label.new()
	label.add_theme_font_size_override("font_size", CARD_FONT_SIZE)
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	# Three compact lines to fit the short strip: identity+EV, exposure+ISO, mark.
	label.text = "m%d %s  EV %d→%s\nexp %s · ISO %s\nΔmark %s" % [
		int(entry.get("index", 0)),
		"metered" if int(entry.get("index", 0)) == 0 else "bracket",
		int(entry.get("applied_ev", 0)), realized_ev,
		exp_ms, iso_text, mark_text,
	]
	card.add_child(label)
	return card


func _exposure_ms_text(facts: Dictionary) -> String:
	var entry = _fact_value(facts, "exposure_time", "nanoseconds")
	if entry == null:
		return "-"
	return "%0.2f ms" % (float(entry) / 1.0e6)


func _iso_text(facts: Dictionary) -> String:
	var entry = _fact_value(facts, "sensor_sensitivity_iso", "iso_equivalent")
	return "-" if entry == null else str(int(entry))


func _fact_value(facts: Dictionary, key: String, value_field: String):
	# Returns the raw value of a camera fact, or null if the fact is absent.
	if not facts.has(key):
		return null
	var entry_v = facts[key]
	if typeof(entry_v) != TYPE_DICTIONARY:
		return null
	var entry: Dictionary = entry_v
	if not entry.has(value_field):
		return null
	return entry[value_field]


# ---------------------------------------------------------------------------
# Member coherence check
# ---------------------------------------------------------------------------

func _evaluate_member_coherence(members: Array) -> Dictionary:
	# A bracket varies only exposure/ISO; the geometry-and-focus fields must hold
	# constant across members. Compare aperture_f_number, focal_length_mm and
	# focus distance_m. All-equal (within epsilon) OR all-absent -> OK; differing
	# values, or present-on-some, -> WARNING. Absent==absent is fine (e.g.
	# synthetic, which today authors neither aperture nor focal).
	var checks := [
		{"name": "aperture", "getter": Callable(self, "_member_aperture")},
		{"name": "focal_mm", "getter": Callable(self, "_member_focal")},
		{"name": "focus_distance_m", "getter": Callable(self, "_member_focus_distance")},
	]
	var results: Array = []
	var any_warning := false
	for check_v in checks:
		var check: Dictionary = check_v
		var getter: Callable = check.get("getter")
		var present_values: Array = []
		var present_count := 0
		for m_v in members:
			var value = getter.call(m_v as Dictionary)
			if value != null:
				present_count += 1
				present_values.append(float(value))
		var status := "ok"
		var detail := "absent"
		if present_count == 0:
			status = "ok"
			detail = "absent (all members)"
		elif present_count < members.size():
			status = "warning"
			detail = "present on %d/%d members" % [present_count, members.size()]
			any_warning = true
		else:
			var mn: float = present_values[0]
			var mx: float = present_values[0]
			for v in present_values:
				mn = min(mn, float(v))
				mx = max(mx, float(v))
			if mx - mn > COHERENCE_EPSILON:
				status = "warning"
				detail = "differs [%0.4f..%0.4f]" % [mn, mx]
				any_warning = true
			else:
				status = "ok"
				detail = "= %0.4f" % mn
		results.append({"name": str(check.get("name")), "status": status, "detail": detail})
	return {"ok": not any_warning, "checks": results}


func _member_aperture(m: Dictionary):
	return _fact_value(m.get("facts", {}), "aperture_f_number", "f_number")


func _member_focal(m: Dictionary):
	return _fact_value(m.get("facts", {}), "focal_length_mm", "millimetres")


func _member_focus_distance(m: Dictionary):
	# distance_m lives directly in focus_state (only when a distance is known).
	var facts: Dictionary = m.get("facts", {})
	var focus_v = facts.get("focus_state", null)
	if typeof(focus_v) != TYPE_DICTIONARY:
		return null
	var focus: Dictionary = focus_v
	return focus.get("distance_m", null) if focus.has("distance_m") else null


func _apply_coherence_banner(coherence: Dictionary, member_count: int) -> void:
	var parts: Array[String] = []
	for c_v in coherence.get("checks", []):
		var c: Dictionary = c_v
		var mark := "OK" if str(c.get("status")) == "ok" else "WARN"
		parts.append("%s %s (%s)" % [str(c.get("name")), mark, str(c.get("detail"))])
	var headline := "OK" if bool(coherence.get("ok", true)) else "⚠ WARNING"
	_coherence_label.text = "Coherence across %d member%s: %s — %s" % [
		member_count, "" if member_count == 1 else "s", headline, "  ·  ".join(parts)
	]
	# Greppable log line so the check is an assured "test", not just a glance.
	print("[CamBANG][270][coherence] status=%s members=%d %s" % [
		"ok" if bool(coherence.get("ok", true)) else "warning", member_count, "  ".join(parts)
	])


func _apply_timing_line(members: Array, press_us: int, result_us: int) -> void:
	# Wall clock (Godot): button press -> result object available. This whole
	# figure is round-trip, NOT capture latency alone.
	var wall_ms := (result_us - press_us) / 1000.0
	# Provider clock: burst span from the earliest to latest member acquisition
	# mark. Different epoch from the wall clock, so it is reported alongside, not
	# subtracted from it. If the burst span is tiny but the wall time is large,
	# the round-trip is dominated by set-profile / poll / display overhead, not
	# by the capture itself.
	var span_text := "n/a"
	var mn := 0
	var mx := 0
	var have := false
	for m_v in members:
		var m: Dictionary = m_v
		if not bool(m.get("mark_has", false)):
			continue
		var ns := int(m.get("mark_ns", 0))
		if not have:
			mn = ns
			mx = ns
			have = true
		else:
			mn = min(mn, ns)
			mx = max(mx, ns)
	var attribution := ""
	if have:
		var span_ms := (mx - mn) / 1.0e6
		span_text = "%0.2f ms" % span_ms
		if wall_ms > span_ms * 3.0 + 50.0:
			attribution = "  → round-trip is overhead-bound, not capture"
	_timing_label.text = "Timing: press→result %0.0f ms (wall) · burst span %s (provider clock)%s" % [
		wall_ms, span_text, attribution
	]


# ---------------------------------------------------------------------------
# Member-0 detailed facts (full, for the scroll box)
# ---------------------------------------------------------------------------

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
