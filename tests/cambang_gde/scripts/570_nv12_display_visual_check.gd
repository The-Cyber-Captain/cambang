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
var _bound_rgba: bool = false


func _ready() -> void:
	_build_ui()
	_log("RUN: nv12_display_visual_check")
	_log("ATTENDED SCENE: compare the two images, then press Esc to quit.")

	CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC)

	var endpoints: Array = CamBANGServer.enumerate_devices()
	if endpoints.size() < 2:
		_fail("need two synthetic endpoints for a side-by-side comparison, found %d" % endpoints.size())
		return

	var rgba_device: CamBANGDevice = CamBANGServer.get_device_for_hardware_id("synthetic:0")
	var nv12_device: CamBANGDevice = CamBANGServer.get_device_for_hardware_id("synthetic:1")
	if rgba_device == null or nv12_device == null:
		_fail("could not resolve synthetic:0 / synthetic:1")
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

	_nv12_stream = nv12_device.create_stream(_profile(CamBANGServer.PIXEL_FORMAT_NV12))
	if _nv12_stream == null:
		# Creation is where format negotiation rejects an unadvertised format,
		# so this is the failure worth calling out specifically.
		_fail("NV12 stream could not be created -- format negotiation rejected it")
		return

	_rgba_stream.start()
	_nv12_stream.start()
	_log("both streams started (%dx%d)" % [STREAM_WIDTH, STREAM_HEIGHT])


func _profile(format_fourcc: int) -> Dictionary:
	return {
		"intent": CamBANGStream.INTENT_VIEWFINDER,
		"profile": {
			"width": STREAM_WIDTH,
			"height": STREAM_HEIGHT,
			"format_fourcc": format_fourcc,
			"target_fps_min": 30,
			"target_fps_max": 30,
		},
		# Colour bars, deliberately, not the default noise pattern.
		#
		# NV12 subsamples chroma 2x2, so on per-pixel random noise each block's
		# chroma is taken from one pixel and applied to four. That pairs a
		# pixel's luma with a neighbour's colour and reads as extra saturation
		# -- a large artifact that appears even when the conversion is exactly
		# right, and which swamps the matrix and range errors this scene exists
		# to detect.
		#
		# Colour bars are the correct instrument: large flat areas of saturated
		# primaries and secondaries, where subsampling changes almost nothing
		# except at the vertical edges, and where a wrong matrix, swapped
		# chroma pair, or range mistake is immediately obvious.
		"picture": {
			"preset": "color_bars",
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
