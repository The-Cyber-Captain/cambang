extends Control

# Attended visual check for the NV12 (CPU_PLANAR) display path.
#
# This scene exists because no automated verdict can establish that a colour
# conversion is CORRECT. A wrong matrix, a swapped chroma pair, or a
# full-vs-limited range mistake all produce a plausible image that passes every
# structural assertion. Only a human looking at it can tell.
#
# It shows three views of the same synthetic pattern, side by side:
#   left   = RGBA display view (the long-standing packed path, the reference)
#   middle = NV12 display view (planar -> display conversion)
#   right  = NV12 to_image()   (planar -> explicit CPU materialization)
#
# The two NV12 panels exercise different code. They share the colour maths, so
# if display is right and materialization is wrong, the fault is in the
# materialization plumbing rather than the transform.
#
# They are rendered from identical source pixels, so they should look the same.
# Chroma is subsampled 2x2 in NV12, so the right image may be very slightly
# softer on sharp colour edges. Anything else -- a green or magenta cast,
# swapped red/blue, washed out or crushed contrast -- is a real defect.
#
# Press Esc to quit.

const STREAM_WIDTH: int = 640
const STREAM_HEIGHT: int = 480

var _rgba_stream: CamBANGStream = null
var _nv12_stream: CamBANGStream = null
var _rgba_rect: TextureRect = null
var _nv12_rect: TextureRect = null
var _nv12_image_rect: TextureRect = null
var _status: Label = null
var _bound_nv12: bool = false
var _bound_nv12_image: bool = false
var _failed: bool = false
# A stream that never produces a result is a silent stall: nothing errors, the
# panels simply stay blank. Without a deadline the scene looks busy while
# proving nothing, which is how the mobile-renderer defect first read as
# "no images" rather than "no results".
const RESULT_DEADLINE_SEC: float = 8.0
var _elapsed: float = 0.0
var _deadline_reported: bool = false
# Last observed state per path, so the deadline can say WHY nothing bound
# rather than only that nothing did.
var _last_rgba_state: String = "no result yet"
var _last_nv12_state: String = "no result yet"
var _last_image_state: String = "no result yet"
const IMAGE_REFRESH_SEC: float = 1.0
var _image_refresh_elapsed: float = 0.0
var _logged_image_once: bool = false
# Still capture is a separate path from streaming: a different provider code
# path, a different Core retention route, and a different access surface. It
# gets its own step rather than being assumed to follow from the stream result.
var _capture_device: CamBANGDevice = null
var _capture_requested: bool = false
var _capture_reported: bool = false
var _capture_settle: float = 0.0
var _bound_rgba: bool = false


func _ready() -> void:
	_build_ui()
	_log("RUN: nv12_display_visual_check")
	_log("ATTENDED SCENE: compare the two images, then press Esc to quit.")

	# Provider is selectable so the same comparison can be run against real
	# hardware. Hardware ids are resolved from enumeration rather than hardcoded,
	# because a platform provider's ids are device-specific.
	# Provider selection reuses the established --cambang-bench-provider
	# mechanism rather than inventing another. Android has no post-"--" user
	# args, so the launcher translates that argument into the
	# cambang/maintainer/bench_provider project setting; reading the setting
	# means the same invocation works on Windows and on a handset.
	var want_platform := false
	var setting_provider := str(
		ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")
	).strip_edges().to_lower()
	if setting_provider == "platform":
		want_platform = true
	for arg in OS.get_cmdline_args():
		if str(arg) == "--cambang-bench-provider=platform":
			want_platform = true
	var provider_kind: int = (CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED
		if want_platform else CamBANGServer.PROVIDER_KIND_SYNTHETIC)
	_log("provider=%s" % ("platform_backed" if want_platform else "synthetic"))
	# Driving two cameras at once is gated on an ingested camera-concurrency
	# truth naming the exact combination, and that must be ingested BEFORE
	# start(). Hardware ids are only known after enumeration, so this starts
	# once to enumerate, stops, ingests the truth for the pair it will use, and
	# starts again. Without this the second camera's stream produces no frames
	# and nothing reports an error -- the gate is deliberately fail-closed.
	var start_err: int = int(CamBANGServer.start(provider_kind))
	if start_err != OK:
		_fail("CamBANGServer.start failed: %d" % start_err)
		return

	var endpoints: Array = CamBANGServer.enumerate_devices()
	_log("endpoints=%d" % endpoints.size())
	if endpoints.size() < 2:
		_fail("need two endpoints for a side-by-side comparison, found %d" % endpoints.size())
		return

	var id_a: String = str((endpoints[0] as Dictionary).get("hardware_id", ""))
	var id_b: String = str((endpoints[1] as Dictionary).get("hardware_id", ""))
	_log("endpoint_a=%s endpoint_b=%s" % [id_a, id_b])

	CamBANGServer.stop()
	var description := JSON.stringify({
		"schema_version": 2,
		"cameras": [{"camera_id": id_a}, {"camera_id": id_b}],
		"concurrent_camera_support": {
			"supported": true,
			"camera_id_combinations": [[id_a, id_b]],
		},
	})
	var ingest_err: int = int(CamBANGServer.ingest_camera_description(description))
	if ingest_err != OK:
		_fail("ingest_camera_description rejected: %d" % ingest_err)
		return
	_log("ingested concurrency truth for [%s, %s]" % [id_a, id_b])
	start_err = int(CamBANGServer.start(provider_kind))
	if start_err != OK:
		_fail("restart after ingest failed: %d" % start_err)
		return

	# NV12 goes on the FIRST camera and the reference on the second. RGBA was
	# previously proven working on the first camera while NV12 on the second
	# produced nothing, which leaves camera and format confounded. Swapping
	# them separates the two: if NV12 now works, the earlier failure was the
	# camera; if it still fails here, it is the format.
	var nv12_device: CamBANGDevice = CamBANGServer.get_device_for_hardware_id(id_a)
	var rgba_device: CamBANGDevice = CamBANGServer.get_device_for_hardware_id(id_b)
	if rgba_device == null or nv12_device == null:
		_fail("could not resolve devices %s / %s" % [id_a, id_b])
		return

	rgba_device.engage()
	nv12_device.engage()
	while not rgba_device.live:
		await rgba_device.live_changed
	while not nv12_device.live:
		await nv12_device.live_changed

	_rgba_stream = rgba_device.create_stream(_profile(CamBANGServer.PIXEL_FORMAT_RGBA))
	if _rgba_stream == null:
		_fail("RGBA reference stream could not be created")
		return

	# Deliberately NO format named. This is the path an ordinary caller takes,
	# and the whole point of the format-selection work: Core should choose the
	# device's native format from its advertised capability, so the application
	# never handles a FourCC. Naming NV12 here would exercise negotiation but
	# not selection, which is the distinction this run exists to close.
	_nv12_stream = nv12_device.create_stream(_default_profile())
	if _nv12_stream == null:
		# Creation is where format negotiation rejects an unadvertised format,
		# so this is the failure worth calling out specifically.
		_fail("NV12 stream could not be created -- format negotiation rejected it")
		return

	# start() returns an Error. Ignoring it meant a refused stream looked
	# identical to one that started and produced nothing, which cost several
	# diagnostic cycles.
	_capture_device = nv12_device
	var rgba_start: int = int(_rgba_stream.start())
	var nv12_start: int = int(_nv12_stream.start())
	_log("stream start: rgba=%d nv12=%d" % [rgba_start, nv12_start])
	if nv12_start != OK:
		_fail("NV12 stream.start() refused: %d" % nv12_start)
		return
	if rgba_start != OK:
		_fail("RGBA stream.start() refused: %d" % rgba_start)
		return
	_log("both streams started (%dx%d)" % [STREAM_WIDTH, STREAM_HEIGHT])


# Geometry only, no format. Core fills the format from device capability.
func _default_profile() -> Dictionary:
	return {
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": STREAM_WIDTH,
			"height": STREAM_HEIGHT,
			"target_fps_min": 30,
			"target_fps_max": 30,
		},
	}


func _profile(format_fourcc: int) -> Dictionary:
	# No picture block: pattern presets are a synthetic-generator concept and a
	# real camera has no such control. Against synthetic this means the default
	# pattern rather than colour bars.
	return {
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": STREAM_WIDTH,
			"height": STREAM_HEIGHT,
			"format_fourcc": format_fourcc,
			"target_fps_min": 30,
			"target_fps_max": 30,
		},
	}


func _process(delta: float) -> void:
	# _process runs every frame; without this a single fault would repeat its
	# report indefinitely and bury the first occurrence.
	if _failed:
		return

	_elapsed += delta
	if not _deadline_reported and _elapsed > RESULT_DEADLINE_SEC:
		_deadline_reported = true
		_report_stall()
	if _rgba_stream != null and not _bound_rgba and _rgba_stream.result_live:
		var r: Variant = _rgba_stream.get_result()
		if r != null:
			# Not fatal if the first retained result is not yet usable. Core runs
			# a bounded backing-plan evaluation at stream start, during which a
			# GPU-primary result can legitimately exist before its backing does.
			# Only the deadline treats this as a failure.
			var rgba_can: int = int(r.can_get_display_view())
			var rgba_view: Variant = (r.get_display_view() if rgba_can != 0 else null)
			if rgba_view == null or not (rgba_view is Texture2D):
				_last_rgba_state = "can_get_display_view=%d payload_kind=%d" % [rgba_can, int(r.get_payload_kind())]
				return
			_rgba_rect.texture = rgba_view
			_bound_rgba = true
			_log("RGBA reference bound (payload_kind=%d)" % int(r.get_payload_kind()))

	if _nv12_stream != null and not _bound_nv12 and _nv12_stream.result_live:
		var n: Variant = _nv12_stream.get_result()
		if n != null:
			var kind: int = int(n.get_payload_kind())
			var can_display: int = int(n.can_get_display_view())
			_log("NV12 result: payload_kind=%d format=%d can_get_display_view=%d"
				% [kind, int(n.get_format()), can_display])
			if can_display == 0:
				_fail("NV12 stream result reports no display path")
				return
			var view: Variant = n.get_display_view()
			# can_get_display_view() reporting a capability does not prove
			# get_display_view() returned one. An earlier run logged a
			# successful bind while showing a blank panel, because the
			# capability and the implementing path disagreed.
			if view == null or not (view is Texture2D):
				_fail("NV12 reported display capability %d but get_display_view() returned no texture" % can_display)
				return
			_nv12_rect.texture = view
			_bound_nv12 = true
			_log("NV12 display bound (%dx%d)" % [view.get_width(), view.get_height()])

	# Explicit CPU materialization, a separate access path from display.
	#
	# Re-materialized periodically rather than once. to_image() is a snapshot of
	# the state at the time of the call, and a camera's first retained frame is
	# typically black before exposure settles, so a single early call shows a
	# black panel forever and looks like a conversion failure.
	_image_refresh_elapsed += delta
	if _bound_nv12_image and _image_refresh_elapsed >= IMAGE_REFRESH_SEC:
		_image_refresh_elapsed = 0.0
		_bound_nv12_image = false
	if _nv12_stream != null and not _bound_nv12_image and _nv12_stream.result_live:
		var m: Variant = _nv12_stream.get_result()
		if m != null:
			var can_image: int = int(m.can_to_image())
			if can_image == 0:
				_fail("NV12 stream result reports no to_image path")
				return
			var img: Variant = m.to_image()
			if img == null or not (img is Image):
				_fail("NV12 reported to_image capability %d but to_image() returned nothing" % can_image)
				return
			var tex := ImageTexture.create_from_image(img)
			if tex == null:
				_fail("NV12 to_image() produced an Image that could not become a texture")
				return
			_nv12_image_rect.texture = tex
			_bound_nv12_image = true
			_log("NV12 to_image materialized (%dx%d can_to_image=%d)"
				% [img.get_width(), img.get_height(), can_image])

	# Still capture is a separate provider path, Core route and access surface
	# from streaming, so it is checked explicitly. A provider that silently fell
	# back to packed here would look identical from the stream panels.
	if _bound_nv12 and not _capture_requested:
		_capture_settle += delta
		if _capture_settle >= 2.0:
			_capture_requested = true
			var trig: int = int(_capture_device.trigger_capture())
			if trig != OK:
				_capture_reported = true
				_log("CAPTURE: trigger refused (%d)" % trig)

	if _capture_requested and not _capture_reported:
		var cap: Variant = _capture_device.get_result()
		if cap != null:
			_capture_reported = true
			var cfmt: int = int(cap.get_format())
			var can_img: int = int(cap.can_to_image_member(0))
			var member_img: Variant = (cap.to_image_member(0) if can_img != 0 else null)
			var got := "no"
			if member_img != null and member_img is Image:
				got = "yes %dx%d" % [member_img.get_width(), member_img.get_height()]
			_log("CAPTURE: format=%d (%s) payload_kind=%d can_to_image_member=%d materialized=%s"
				% [cfmt, _fourcc_name(cfmt), int(cap.get_payload_kind()), can_img, got])

	if _bound_rgba and _bound_nv12:
		_status.text = "Compare the two images. They should match (NV12 may be slightly softer).\nPress Esc to quit."


# Names exactly which stage each stream reached, so a stall is attributable
# rather than merely visible.
func _report_stall() -> void:
	var problems: Array[String] = []
	for entry in [["RGBA", _rgba_stream, _bound_rgba], ["NV12", _nv12_stream, _bound_nv12]]:
		var label: String = entry[0]
		var stream: CamBANGStream = entry[1]
		var bound: bool = entry[2]
		if bound:
			continue
		if stream == null:
			problems.append("%s: stream was never created" % label)
		elif not stream.result_live:
			problems.append("%s: result_live never became true (no frame retained)" % label)
		else:
			var st: String = (_last_rgba_state if label == "RGBA" else _last_nv12_state)
			problems.append("%s: result live but never usable (%s)" % [label, st])
	if not _bound_nv12_image and _bound_nv12:
		problems.append("NV12 to_image: never materialized (%s)" % _last_image_state)
	if problems.is_empty():
		return
	_fail("no result after %.1fs -- %s" % [RESULT_DEADLINE_SEC, "; ".join(problems)])


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		_log("Esc pressed; shutting down")
		CamBANGServer.stop_and_quit()


func _build_ui() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var root := VBoxContainer.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(root)

	_status = Label.new()
	_status.text = "Starting NV12 display check..."
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_status)

	var row := HBoxContainer.new()
	row.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(row)

	row.add_child(_labelled_view("RGBA display (reference)", 0))
	row.add_child(_labelled_view("NV12 display view", 1))
	row.add_child(_labelled_view("NV12 to_image()", 2))


func _labelled_view(caption: String, slot: int) -> Control:
	var col := VBoxContainer.new()
	col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	col.size_flags_vertical = Control.SIZE_EXPAND_FILL

	var label := Label.new()
	label.text = caption
	col.add_child(label)

	var rect := TextureRect.new()
	rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	rect.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rect.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(rect)

	match slot:
		0: _rgba_rect = rect
		1: _nv12_rect = rect
		2: _nv12_image_rect = rect
	return col


# Renders a FourCC as its four characters so the log states which family
# member Core actually selected rather than a bare integer.
func _fourcc_name(fourcc: int) -> String:
	if fourcc == 0:
		return "none"
	var out := ""
	for i in range(4):
		out += char((fourcc >> (i * 8)) & 0xFF)
	return out


func _log(message: String) -> void:
	print("[Scene570] ", message)


func _fail(reason: String) -> void:
	if _failed:
		return
	_failed = true
	push_error("KERR " + reason)
	_log("FAIL: " + reason)
	if _status != null:
		_status.text = "FAILED: " + reason + "\nPress Esc to quit."
