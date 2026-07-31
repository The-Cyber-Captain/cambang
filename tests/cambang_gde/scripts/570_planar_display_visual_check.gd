extends Control

# Attended visual check for the planar (CPU_PLANAR) display path.
#
# This scene exists because no automated verdict can establish that a colour
# conversion is CORRECT. A wrong matrix, a swapped chroma pair, or a
# full-vs-limited range mistake all produce a plausible image that passes every
# structural assertion. Only a human looking at it can tell.
#
# The format is named explicitly and is selectable:
#
#   --cambang-planar-format=nv12|nv21|i420|yv12   (default nv12)
#
# The request is a request. Camera2's YUV_420_888 is a family and the device
# picks the member -- an S20+ answers NV12 with NV21 -- so the panel may show
# a sibling of what was asked for. The run logs SUBSTITUTED when that happens
# and judges what was actually delivered. Only a PACKED payload fails.
#
# All four exist because chroma order is invisible to plane geometry: NV21 and
# YV12 carry V before U, and reading them as NV12/I420 swaps red and blue,
# which looks like a plausible image rather than a failure. The native suite
# checks that order against the descriptor table, but the table is its own
# ground truth -- an eye on NV21 is coverage nothing else provides.
#
# Three panels:
#   left   = packed RGBA display view, on the SECOND camera
#   middle = planar display view      (planar -> display conversion)
#   right  = planar to_image()        (planar -> explicit CPU materialization)
#
# WHAT IS COMPARABLE TO WHAT, and this differs by provider:
#
#   middle vs right -- always valid. Same stream, same retained frame, two
#   different access paths that share the colour maths. If display is right
#   and materialization is wrong, the fault is in the materialization plumbing
#   rather than the transform.
#
#   left vs the others -- valid ONLY on SyntheticProvider, where both endpoints
#   render the same deterministic pattern, so the packed panel is a true colour
#   reference for the planar ones. On a platform provider the left panel is a
#   physically different camera pointing somewhere else: it shows that packed
#   capture still works and that the second camera is alive, and it is NOT a
#   pixel reference. Do not read a hardware run as a colour comparison.
#
# One camera cannot serve both: at most one stream per device may be live
# (docs/state_snapshot.md, v1 invariant), so a packed reference and a planar
# stream must sit on different devices. That constraint is why the left panel
# means different things on different providers, and it cannot be designed
# away here.
#
# EXPECTED difference: chroma is subsampled 2x2 and SyntheticProvider point-
# samples it from the top-left of each block rather than averaging, so that
# the inverse stays exactly predictable for verification. On high-frequency
# content -- the default noise pattern especially -- that makes the planar
# panels look MORE saturated and blockier than the packed one, not softer.
# An averaging filter would dull it; point sampling does the opposite.
#
# REAL defects look different: a green or magenta cast, swapped red and blue,
# or washed out / crushed contrast. Those show up on FLAT colour, where
# subsampling has nothing to lose. Judge the run on flat regions; treat
# differences confined to noisy regions as the sampler, not a fault.
#
# Format SELECTION (Core choosing a format when the caller names none) is
# deliberately NOT what the panels depend on any more. That path has native
# coverage in the core_selects_stream_format_when_unspecified verification case
# and on WinRT hardware; making the panels ride on it meant that against a
# provider whose native form is packed, this scene silently showed three packed
# panels and verified nothing it claims to.
#
# Press Esc to quit.

const STREAM_WIDTH: int = 640
const STREAM_HEIGHT: int = 480

var _rgba_stream: CamBANGStream = null
var _planar_stream: CamBANGStream = null
var _rgba_rect: TextureRect = null
var _planar_rect: TextureRect = null
var _planar_image_rect: TextureRect = null
var _status: Label = null
var _bound_planar: bool = false
var _bound_planar_image: bool = false
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
var _last_planar_state: String = "no result yet"
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
# Freeze detection. A panel that stops updating is invisible to every
# structural check -- capability still reports READY, result_live stays
# true, nothing errors -- so it takes a human noticing, which is exactly how
# it was caught. Sampling the retained content turns that into log evidence.
# It reports; it never fails a run, because a legitimately static source
# (a synthetic pattern with a still base, a camera on a motionless scene)
# is indistinguishable from a frozen one by content alone.
const CONTENT_SAMPLE_SEC: float = 2.0
var _content_sample_elapsed: float = 0.0
var _last_rgba_hash: int = 0
var _last_planar_hash: int = 0
var _rgba_static_samples: int = 0
var _planar_static_samples: int = 0
# Resolved in _ready() from the command line; drives the middle/right pair.
var _planar_format: int = 0
var _planar_format_name: String = "nv12"
var _on_synthetic: bool = true


func _ready() -> void:
	_build_ui()
	_log("RUN: planar_display_visual_check")
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
	# The launcher passes this after "--", which Godot routes to the user-arg
	# list rather than the engine one. Scan both: reading only engine args
	# silently ran this scene on Synthetic while the invocation named platform.
	for arg in (OS.get_cmdline_args() + OS.get_cmdline_user_args()):
		if str(arg) == "--cambang-bench-provider=platform":
			want_platform = true
	var provider_kind: int = (CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED
		if want_platform else CamBANGServer.PROVIDER_KIND_SYNTHETIC)
	_on_synthetic = not want_platform
	_log("provider=%s" % ("platform_backed" if want_platform else "synthetic"))
	_resolve_planar_format()
	if _failed:
		return
	_log("planar format=%s" % _planar_format_name)
	if not _on_synthetic:
		_log("NOTE: on a platform provider the left panel is a DIFFERENT camera --")
		_log("      it is a liveness panel, not a colour reference. Compare middle vs right.")
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

	# The planar stream goes on the FIRST camera and the packed one on the
	# second. RGBA was once proven working on the first camera while planar
	# on the second produced nothing, which left camera and format
	# confounded; swapping them separated the two. They must be different
	# devices either way: at most one stream per device may be live.
	var planar_device: CamBANGDevice = CamBANGServer.get_device_for_hardware_id(id_a)
	var rgba_device: CamBANGDevice = CamBANGServer.get_device_for_hardware_id(id_b)
	if rgba_device == null or planar_device == null:
		_fail("could not resolve devices %s / %s" % [id_a, id_b])
		return

	rgba_device.engage()
	planar_device.engage()
	while not rgba_device.live:
		await rgba_device.live_changed
	while not planar_device.live:
		await planar_device.live_changed

	_rgba_stream = rgba_device.create_stream(_profile(CamBANGServer.PIXEL_FORMAT_RGBA))
	if _rgba_stream == null:
		_fail("RGBA reference stream could not be created")
		return

	# The format is named explicitly. This scene judges colour, and a colour
	# judgement needs to know which format produced the pixels; leaving it to
	# Core's selection meant a provider whose native form is packed handed back
	# packed and the planar path went unexercised while everything still looked
	# fine. Selection keeps its own coverage elsewhere -- see the header.
	_planar_stream = planar_device.create_stream(_profile(_planar_format))
	if _planar_stream == null:
		# Creation is where format negotiation rejects an unadvertised format,
		# so this is the failure worth calling out specifically.
		_fail("planar stream (%s) could not be created -- format negotiation rejected it"
			% _planar_format_name)
		return

	# start() returns an Error. Ignoring it meant a refused stream looked
	# identical to one that started and produced nothing, which cost several
	# diagnostic cycles.
	_capture_device = planar_device
	# Name the capture format too. Still capture is a separate provider path,
	# Core route and access surface from streaming, and leaving it to
	# selection meant it quietly reported packed on a provider whose native
	# still form is packed -- the exact blind spot the stream panels just
	# stopped having.
	var still_err: int = int(_capture_device.set_still_capture_profile({
		"width": STREAM_WIDTH,
		"height": STREAM_HEIGHT,
		"format_fourcc": _planar_format,
	}))
	if still_err != OK:
		_log("NOTE: still profile (%s) refused (%d) -- capture step will use the selected format"
			% [_planar_format_name, still_err])
	var rgba_start: int = int(_rgba_stream.start())
	var planar_start: int = int(_planar_stream.start())
	_log("stream start: rgba=%d planar=%d" % [rgba_start, planar_start])
	if planar_start != OK:
		_fail("planar stream.start() refused: %d" % planar_start)
		return
	if rgba_start != OK:
		_fail("RGBA stream.start() refused: %d" % rgba_start)
		return
	_log("both streams started (%dx%d)" % [STREAM_WIDTH, STREAM_HEIGHT])


# Which 4:2:0 member the planar panels use. Unknown values are refused rather
# than silently defaulted: a typo that quietly ran NV12 while the operator
# believed they were inspecting NV21 would defeat the point of the run.
func _resolve_planar_format() -> void:
	var by_name := {
		"nv12": CamBANGServer.PIXEL_FORMAT_NV12,
		"nv21": CamBANGServer.PIXEL_FORMAT_NV21,
		"i420": CamBANGServer.PIXEL_FORMAT_I420,
		"yv12": CamBANGServer.PIXEL_FORMAT_YV12,
	}
	var chosen := "nv12"
	var setting := str(
		ProjectSettings.get_setting("cambang/maintainer/planar_format", "")
	).strip_edges().to_lower()
	if setting != "":
		chosen = setting
	const PREFIX := "--cambang-planar-format="
	for arg in (OS.get_cmdline_args() + OS.get_cmdline_user_args()):
		var a := str(arg)
		if a.begins_with(PREFIX):
			chosen = a.substr(PREFIX.length()).strip_edges().to_lower()
	if not by_name.has(chosen):
		_fail("unknown planar format '%s' -- expected one of nv12, nv21, i420, yv12" % chosen)
		return
	_planar_format_name = chosen
	_planar_format = int(by_name[chosen])


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

	if _planar_stream != null and not _bound_planar and _planar_stream.result_live:
		var n: Variant = _planar_stream.get_result()
		if n != null:
			var kind: int = int(n.get_payload_kind())
			var can_display: int = int(n.can_get_display_view())
			var sel_fmt: int = int(n.get_format())
			_log("planar result: payload_kind=%d format=%d (%s) can_get_display_view=%d"
				% [kind, sel_fmt, _fourcc_name(sel_fmt), can_display])
			# A packed payload against an explicit planar request means the
			# request was not honoured, and that must fail: three packed panels
			# look exactly like a passing planar run.
			#
			# A DIFFERENT 4:2:0 member must not fail, though. Camera2's
			# YUV_420_888 is a family, and the device decides which member it
			# hands over: a Galaxy S20+ answers an NV12 request with NV21. The
			# provider resolves that from the observed strides and the payload
			# carries what was actually delivered, so the substitution is
			# correct behaviour, not a broken promise. It is reported loudly
			# because it changes what this run is evidence FOR.
			if kind != 1:
				_fail("asked for %s but the retained payload is not planar (kind=%d, format=%s)"
					% [_planar_format_name, kind, _fourcc_name(sel_fmt)])
				return
			if sel_fmt != _planar_format:
				_log("SUBSTITUTED: asked for %s, device delivered %s -- this run judges %s"
					% [_planar_format_name.to_upper(), _fourcc_name(sel_fmt), _fourcc_name(sel_fmt)])
			if can_display == 0:
				_fail("planar stream result reports no display path")
				return
			_last_planar_state = "can_get_display_view=%d payload_kind=%d format=%s" % [
				can_display, kind, _fourcc_name(sel_fmt)]
			var view: Variant = n.get_display_view()
			# can_get_display_view() reporting a capability does not prove
			# get_display_view() returned one. An earlier run logged a
			# successful bind while showing a blank panel, because the
			# capability and the implementing path disagreed.
			if view == null or not (view is Texture2D):
				_fail("planar reported display capability %d but get_display_view() returned no texture" % can_display)
				return
			_planar_rect.texture = view
			_bound_planar = true
			_log("planar display bound (%dx%d)" % [view.get_width(), view.get_height()])

	# Explicit CPU materialization, a separate access path from display.
	#
	# Re-materialized periodically rather than once. to_image() is a snapshot of
	# the state at the time of the call, and a camera's first retained frame is
	# typically black before exposure settles, so a single early call shows a
	# black panel forever and looks like a conversion failure.
	_image_refresh_elapsed += delta
	if _bound_planar_image and _image_refresh_elapsed >= IMAGE_REFRESH_SEC:
		_image_refresh_elapsed = 0.0
		_bound_planar_image = false
	if _planar_stream != null and not _bound_planar_image and _planar_stream.result_live:
		var m: Variant = _planar_stream.get_result()
		if m != null:
			var can_image: int = int(m.can_to_image())
			_last_image_state = "can_to_image=%d payload_kind=%d" % [
				can_image, int(m.get_payload_kind())]
			if can_image == 0:
				_fail("planar stream result reports no to_image path")
				return
			var img: Variant = m.to_image()
			if img == null or not (img is Image):
				_fail("planar reported to_image capability %d but to_image() returned nothing" % can_image)
				return
			var tex := ImageTexture.create_from_image(img)
			if tex == null:
				_fail("planar to_image() produced an Image that could not become a texture")
				return
			_planar_image_rect.texture = tex
			_bound_planar_image = true
			_log("planar to_image materialized (%dx%d can_to_image=%d)"
				% [img.get_width(), img.get_height(), can_image])

	# Still capture is a separate provider path, Core route and access surface
	# from streaming, so it is checked explicitly. A provider that silently fell
	# back to packed here would look identical from the stream panels.
	if _bound_planar and not _capture_requested:
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
			if int(cap.get_payload_kind()) != 1:
				_log("NOTICE: capture came back %s (packed) -- the planar CAPTURE path is NOT exercised in this run"
					% _fourcc_name(cfmt))
			elif cfmt != _planar_format:
				_log("CAPTURE SUBSTITUTED: asked for %s, device delivered %s"
					% [_planar_format_name.to_upper(), _fourcc_name(cfmt)])

	_sample_content_change(delta)

	if _bound_rgba and _bound_planar:
		_status.text = ("Compare %s (middle) against its to_image() (right) -- these always match.\n"
			+ ("Left is the same pattern in packed RGBA; all three should agree.\n" if _on_synthetic
			else "Left is a DIFFERENT camera: liveness only, not a colour reference.\n")
			+ "Planar looks more saturated on noise (point-sampled chroma); judge flat colour. Press Esc to quit.") % _planar_format_name.to_upper()


# Reports whether each panel's retained content is still changing.
#
# Uses to_image() rather than reading the displayed texture, because that is the
# retained truth the panel is drawn from; if this says the content changed and
# the panel did not, the fault is in the display wrapper rather than upstream,
# which narrows the search considerably.
func _sample_content_change(delta: float) -> void:
	if not (_bound_rgba and _bound_planar):
		return
	_content_sample_elapsed += delta
	if _content_sample_elapsed < CONTENT_SAMPLE_SEC:
		return
	_content_sample_elapsed = 0.0
	var rgba_state := _sample_one(_rgba_stream, _last_rgba_hash)
	if rgba_state[0] != 0:
		if rgba_state[1]:
			_rgba_static_samples += 1
		else:
			_rgba_static_samples = 0
		_last_rgba_hash = rgba_state[0]
	var planar_state := _sample_one(_planar_stream, _last_planar_hash)
	if planar_state[0] != 0:
		if planar_state[1]:
			_planar_static_samples += 1
		else:
			_planar_static_samples = 0
		_last_planar_hash = planar_state[0]
	# Two consecutive unchanged samples is ~4s of identical content, which is
	# worth saying out loud even though it can be legitimate.
	if _rgba_static_samples == 2 or _planar_static_samples == 2:
		_log("CONTENT: rgba_static=%d planar_static=%d (consecutive unchanged samples; a frozen panel and a still scene look alike here)"
			% [_rgba_static_samples, _planar_static_samples])


# Returns [hash, unchanged]. A zero hash means no sample was taken.
func _sample_one(stream: CamBANGStream, previous: int) -> Array:
	if stream == null or not stream.result_live:
		return [0, false]
	var r: Variant = stream.get_result()
	if r == null or int(r.can_to_image()) == 0:
		return [0, false]
	var img: Variant = r.to_image()
	if img == null or not (img is Image):
		return [0, false]
	# PackedByteArray has no hash() method in Godot 4.5; the global hash() does.
	var h: int = hash(img.get_data())
	if h == 0:
		# Distinguishable from "no sample" only by accident; treat as no sample
		# rather than silently comparing against the sentinel.
		return [0, false]
	return [h, previous != 0 and h == previous]


# Names exactly which stage each stream reached, so a stall is attributable
# rather than merely visible.
func _report_stall() -> void:
	var problems: Array[String] = []
	for entry in [["RGBA", _rgba_stream, _bound_rgba], ["planar", _planar_stream, _bound_planar]]:
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
			var st: String = (_last_rgba_state if label == "RGBA" else _last_planar_state)
			problems.append("%s: result live but never usable (%s)" % [label, st])
	if not _bound_planar_image and _bound_planar:
		problems.append("planar to_image: never materialized (%s)" % _last_image_state)
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
	_status.text = "Starting planar display check..."
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_status)

	var row := HBoxContainer.new()
	row.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(row)

	row.add_child(_labelled_view(
		"RGBA packed, 2nd camera%s" % (" (colour reference)" if _on_synthetic
			else " (LIVENESS ONLY -- different camera, not a reference)"), 0))
	row.add_child(_labelled_view("%s display view" % _planar_format_name.to_upper(), 1))
	row.add_child(_labelled_view("%s to_image()" % _planar_format_name.to_upper(), 2))


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
		1: _planar_rect = rect
		2: _planar_image_rect = rect
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
