extends Node

## Scene 79: Camera2 stills served from a frozen GPU copy.
##
## pixel_payload_and_result_contract.md 11.6.1: once a caller has asked for a
## Capture Compute Texture, a provider that can copy its native buffers on the
## GPU attaches a frozen GPU copy (the sidecar) to later stills, and
## get_compute_texture_plane() serves it without uploading. This scene proves
## that on a real camera:
##
## - before anything is asked for, captures are CPU_UPLOAD (demand arms the
##   sidecar; nothing is copied speculatively);
## - after the first plane request arms it, captures inside the importer's
##   scope (Quest 3 under OpenXR, RenderingDevice renderer) are GPU_RETAINED,
##   and the first plane request for one makes no upload;
## - outside that scope every capture stays CPU_UPLOAD (the gate is closed and
##   the scene is expected_unsupported, after checking exactly that);
## - the GPU copy holds the same image as the CPU payload: plane 0 matches luma
##   derived from to_image() with the member's own colorimetry, and plane 1's
##   chroma is in the member format's byte order (NV12 = Cb,Cr; NV21 = Cr,Cb);
## - capture_image_acquired still reaches the main thread inside the 50 ms
##   consumer window at p99 while copies are being made.
##
## Frame pacing (_process deltas while captures are in flight) and the cost of
## the first plane request are printed on every verdict, and asserted on
## nothing: the 90 Hz benchmark owns those numbers.
##
## Scope: a platform-backed provider. Synthetic makes its sidecars on the CPU
## and is covered by scene 74, so under synthetic this scene is
## expected_unsupported without capturing.
##
## Provider: --cambang-bench-provider=synthetic|platform (default platform), or
## the cambang/maintainer/bench_provider project setting on Android.
## Captures after arming: 20; override with --cambang-gpu-copy-captures=N.
## XR: this scene does not start OpenXR itself. Run it under an XR wrapper to
## exercise the open gate; without one it checks the closed gate.
##
## Self-terminating: emits [CamBANG][HarnessVerdict].

const SCENE_LABEL := "79_capture_gpu_copy_verify"
const TIMEOUT_MS := 8000
const WINDOW_USEC := 50000

const PRE_ARM_CAPTURES := 3
# Captures already admitted when the demand lands may still be CPU_UPLOAD.
const ARM_WARMUP_CAPTURES := 2
# At least this share of post-warmup captures must be GPU_RETAINED inside the
# gate. A copy that misses its finish bound falls back to CPU_UPLOAD by design;
# that fallback is allowed, a gate that never engages is not.
const MIN_RETAINED_SHARE := 0.9
# Content checks run on the first few GPU_RETAINED captures only: a readback
# stalls the GPU, and the remaining captures carry the pacing numbers.
const CONTENT_CHECKS := 3
const SAMPLE_STRIDE := 4
# Luma reconstructed from clamped RGB is not exact on saturated pixels.
const LUMA_MEAN_ABS_LIMIT := 4.0
const LUMA_WITHIN := 8
const LUMA_WITHIN_SHARE := 0.95
# Below this mean chroma excursion from 128 the scene is too grey for the
# byte-order comparison to say anything; it is reported as inconclusive.
const CHROMA_MIN_EXCURSION := 3.0

# Quest 3's forward passthrough sensors; 1 is the virtual avatar camera.
const QUEST_CAMERA := "50"

var _provider_arg := "platform"
var _captures := 20
var _verdict_emitted := false

var _finished: Dictionary = {}       # capture_id -> capture_finished info
var _dispatch_usec: Array[int] = []  # capture_image_acquired emitted - observed
var _measuring := false
var _last_frame_usec := 0
var _frame_delta_usec: Array[int] = []
var _first_plane_usec_retained: Array[int] = []
var _first_plane_usec_upload: Array[int] = []
var _backings_pre: Array[int] = []
var _backings_post: Array[int] = []
var _content_lines: Array[String] = []
var _chroma_verdicts: Array[String] = []
var _gate_expected := false
var _gate_reason := ""


func _ready() -> void:
	_parse_args()
	print("RUN: %s provider=%s captures=%d" % [SCENE_LABEL, _provider_arg, _captures])
	CamBANGServer.capture_image_acquired.connect(_on_image_acquired)
	CamBANGServer.capture_finished.connect(_on_capture_finished)
	await _run()


func _exit_tree() -> void:
	if CamBANGServer.capture_image_acquired.is_connected(_on_image_acquired):
		CamBANGServer.capture_image_acquired.disconnect(_on_image_acquired)
	if CamBANGServer.capture_finished.is_connected(_on_capture_finished):
		CamBANGServer.capture_finished.disconnect(_on_capture_finished)
	CamBANGServer.stop()


func _parse_args() -> void:
	var setting := str(ProjectSettings.get_setting("cambang/maintainer/bench_provider", "")).strip_edges().to_lower()
	if setting != "":
		_provider_arg = setting
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	for arg in args:
		if arg.begins_with("--cambang-bench-provider="):
			_provider_arg = arg.substr("--cambang-bench-provider=".length()).strip_edges().to_lower()
		elif arg.begins_with("--cambang-gpu-copy-captures="):
			_captures = maxi(1, int(arg.substr("--cambang-gpu-copy-captures=".length())))


func _emit(status: String, reason: String) -> void:
	if _verdict_emitted:
		return
	_verdict_emitted = true
	var target_hz := float(Engine.physics_ticks_per_second)
	var xr = _openxr()
	if xr != null and xr.is_initialized():
		target_hz = float(xr.get_display_refresh_rate())
	var over_budget := 0
	if target_hz > 0.0:
		var budget_usec := int(1.5e6 / target_hz)
		for d in _frame_delta_usec:
			if d > budget_usec:
				over_budget += 1
	print("[CamBANG][GpuCopy] provider=%s os=%s model=%s gate_expected=%s gate=%s target_hz=%.1f pre=%s post=%s frames_over_1.5x=%d %s %s %s %s" % [
		_provider_arg, OS.get_name(), OS.get_model_name(), str(_gate_expected), _gate_reason, target_hz,
		_backing_counts(_backings_pre), _backing_counts(_backings_post), over_budget,
		_dist("frame_delta_usec", _frame_delta_usec),
		_dist("dispatch_usec", _dispatch_usec),
		_dist("first_plane_retained_usec", _first_plane_usec_retained),
		_dist("first_plane_upload_usec", _first_plane_usec_upload)])
	for line in _content_lines:
		print("[CamBANG][GpuCopyContent] " + line)
	var code := 0 if status == "ok" or status == "expected_unsupported" else 1
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [SCENE_LABEL, status, code, reason])
	CamBANGServer.stop_and_quit(code)


func _fail(reason: String) -> void:
	push_error(reason)
	print("FAIL: " + reason)
	_emit("fail", reason.replace(" ", "_"))


# ---- signal handlers ---------------------------------------------------------

func _process(_delta: float) -> void:
	var now := Time.get_ticks_usec()
	if _measuring and _last_frame_usec > 0:
		_frame_delta_usec.append(now - _last_frame_usec)
	_last_frame_usec = now


func _on_image_acquired(_capture_id: String, info: Dictionary) -> void:
	var observed := int(info.get("observed_ticks_usec", -1))
	var emitted := int(info.get("emitted_ticks_usec", -1))
	if observed >= 0 and emitted >= observed:
		_dispatch_usec.append(emitted - observed)


func _on_capture_finished(capture_id: String, info: Dictionary) -> void:
	if not _finished.has(capture_id):
		_finished[capture_id] = info.duplicate(true)


# ---- helpers -----------------------------------------------------------------

# Untyped: get_display_refresh_rate() is OpenXRInterface's, not XRInterface's.
func _openxr():
	return XRServer.find_interface("OpenXR")


# The importer's documented scope: Quest 3, OpenXR running, a RenderingDevice.
# Anything else must leave every capture on CPU_UPLOAD.
func _decide_gate() -> void:
	if RenderingServer.get_rendering_device() == null:
		_gate_reason = "no_rendering_device"
		return
	if OS.get_name() != "Android" or OS.get_model_name() != "Quest 3":
		_gate_reason = "not_quest3"
		return
	var xr = _openxr()
	if xr == null or not xr.is_initialized():
		_gate_reason = "openxr_not_running"
		return
	_gate_expected = true
	_gate_reason = "quest3_openxr"


func _start() -> bool:
	CamBANGServer.stop()
	var err := int(CamBANGServer.start(CamBANGServer.PROVIDER_KIND_PLATFORM_BACKED))
	if err != OK:
		_fail("start(platform) returned %d" % err)
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		if CamBANGServer.get_state_snapshot() != null:
			return true
	_fail("no baseline snapshot")
	return false


func _pick_hardware_id() -> String:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		var eps = CamBANGServer.enumerate_devices()
		if typeof(eps) == TYPE_ARRAY and not (eps as Array).is_empty():
			var first := ""
			for ep in eps:
				var hw := str((ep as Dictionary).get("hardware_id", ""))
				if first == "":
					first = hw
				if OS.get_model_name() == "Quest 3" and hw == QUEST_CAMERA:
					return hw
			return first
		await get_tree().process_frame
	return ""


func _engage(dev) -> bool:
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	var err := int(dev.engage())
	while err == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		err = int(dev.engage())
	if err != OK:
		_fail("engage() failed (%d)" % err)
		return false
	while Time.get_ticks_msec() < deadline:
		if bool(dev.is_live()):
			return true
		await get_tree().process_frame
	_fail("device never became live")
	return false


# The first NV12 entry of the endpoint's own catalog: a real camera captures
# only at a geometry it advertises, and the GPU copy covers the YUV family only.
func _apply_planar_still_geometry(dev, hw: String) -> bool:
	var caps: Dictionary = CamBANGServer.get_supported_capture_profiles(hw)
	var chosen: Dictionary = {}
	for entry in caps.get("profiles", []):
		var profile: Dictionary = (entry as Dictionary).get("profile", {})
		if int(profile.get("format_fourcc", 0)) == CamBANGServer.PIXEL_FORMAT_NV12:
			chosen = {
				"width": int(profile.get("width", 0)),
				"height": int(profile.get("height", 0)),
				"format_fourcc": CamBANGServer.PIXEL_FORMAT_NV12,
			}
			break
	if chosen.is_empty():
		_fail("%s advertises no NV12 still geometry" % hw)
		return false
	var err := int(dev.set_still_capture_profile(chosen))
	if err != OK:
		_fail("set_still_capture_profile(%s) refused a catalog entry (%d)" % [JSON.stringify(chosen), err])
		return false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		var p = dev.get_still_capture_profile()
		if typeof(p) == TYPE_DICTIONARY and int((p as Dictionary).get("width", 0)) == int(chosen["width"]) \
				and int((p as Dictionary).get("height", 0)) == int(chosen["height"]) \
				and int((p as Dictionary).get("format_fourcc", 0)) == CamBANGServer.PIXEL_FORMAT_NV12:
			print("STEP OK: still geometry %s" % JSON.stringify(chosen))
			return true
	_fail("still profile %s never applied" % JSON.stringify(chosen))
	return false


# Triggers one capture and waits for its delivered result. Returns null after
# failing the scene.
func _capture(dev, what: String):
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	var trigger: Dictionary = dev.trigger_capture()
	while int(trigger.get("error", FAILED)) == ERR_BUSY and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
		trigger = dev.trigger_capture()
	var err := int(trigger.get("error", FAILED))
	var id := str(trigger.get("id", ""))
	if err != OK or id.is_empty():
		_fail("%s: trigger_capture() err=%d" % [what, err])
		return null
	_last_frame_usec = 0
	_measuring = true
	while Time.get_ticks_msec() < deadline and not _finished.has(id):
		await get_tree().process_frame
	_measuring = false
	if not _finished.has(id):
		_fail("%s: capture_finished never arrived for %s" % [what, id])
		return null
	var info: Dictionary = _finished[id]
	if int(info.get("disposition", -1)) != CamBANGServer.DISPOSITION_DELIVERED:
		_fail("%s: %s finished with disposition %d error_code=%d" % [
			what, id, int(info.get("disposition", -1)), int(info.get("error_code", 0))])
		return null
	var result = CamBANGServer.get_capture_result_by_id(id)
	if result == null:
		_fail("%s: no result for delivered capture %s" % [what, id])
		return null
	return result


func _compute_texture_metrics() -> Dictionary:
	var evidence: Dictionary = CamBANGServer.get_result_access_timing_evidence()
	var m = evidence.get("capture_compute_textures", {})
	return m if m is Dictionary else {}


# Requests plane 0 and checks the upload count against the declared backing.
# Returns the texture, or null after failing the scene.
func _first_plane(result, backing: int, what: String):
	var before := _compute_texture_metrics()
	var t0 := Time.get_ticks_usec()
	var texture = result.get_compute_texture_plane(0, 0)
	var spent := Time.get_ticks_usec() - t0
	var after := _compute_texture_metrics()
	if texture == null or not (texture is Texture2D):
		_fail("%s: get_compute_texture_plane(0,0) returned no Texture2D (backing=%d)" % [what, backing])
		return null
	var uploads := int(after.get("uploads", 0)) - int(before.get("uploads", 0))
	if backing == CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_RETAINED:
		_first_plane_usec_retained.append(spent)
		if uploads != 0:
			_fail("%s: backing GPU_RETAINED but the first plane request made %d upload(s)" % [what, uploads])
			return null
	elif backing == CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_CPU_UPLOAD:
		_first_plane_usec_upload.append(spent)
		if uploads != 1:
			_fail("%s: backing CPU_UPLOAD but the first plane request made %d upload(s)" % [what, uploads])
			return null
	return texture


func _rd_bytes(texture: Texture2D) -> PackedByteArray:
	var rd := RenderingServer.get_rendering_device()
	var rd_texture: RID = RenderingServer.texture_get_rd_texture(texture.get_rid())
	if rd == null or not rd_texture.is_valid():
		return PackedByteArray()
	return rd.texture_get_data(rd_texture, 0)


# Kr/Kb for the member's matrix; "unspecified" resolves to BT.601 as CamBANG's
# own CPU conversion does.
func _matrix_coefficients(matrix: String) -> Vector2:
	match matrix:
		"bt709":
			return Vector2(0.2126, 0.0722)
		"bt2020_ncl":
			return Vector2(0.2627, 0.0593)
	return Vector2(0.299, 0.114)


# Proves the GPU copy holds the CPU payload's image. Returns "" or the failure.
func _verify_content(result, what: String) -> String:
	var member: Dictionary = result.get_image_member(0)
	var fourcc := int(member.get("format", result.get_format()))
	if fourcc != CamBANGServer.PIXEL_FORMAT_NV12 and fourcc != CamBANGServer.PIXEL_FORMAT_NV21:
		return "%s: GPU_RETAINED member has format %d, expected NV12/NV21" % [what, fourcc]
	var colorimetry: Dictionary = member.get("colorimetry", {})
	var full := str(colorimetry.get("range", "unspecified")) == "full"
	var k := _matrix_coefficients(str(colorimetry.get("matrix", "unspecified")))
	var kr := k.x
	var kb := k.y
	var kg := 1.0 - kr - kb
	var image: Image = result.to_image()
	if image == null:
		return "%s: to_image() returned null" % what
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	var w := image.get_width()
	var h := image.get_height()
	var rgba := image.get_data()
	if int(result.get_compute_texture_plane_count(0)) != 2:
		return "%s: plane count %d for a semi-planar member" % [what, int(result.get_compute_texture_plane_count(0))]
	var y_bytes := _rd_bytes(result.get_compute_texture_plane(0, 0))
	var c_bytes := _rd_bytes(result.get_compute_texture_plane(0, 1))
	if y_bytes.size() != w * h:
		return "%s: plane 0 read back %d bytes, expected %dx%d=%d" % [what, y_bytes.size(), w, h, w * h]
	var cw := int((w + 1) / 2.0)
	var ch := int((h + 1) / 2.0)
	if c_bytes.size() != cw * ch * 2:
		return "%s: plane 1 read back %d bytes, expected %dx%dx2=%d" % [what, c_bytes.size(), cw, ch, cw * ch * 2]

	var luma_scale := 219.0 / 255.0 if not full else 1.0
	var luma_offset := 16.0 if not full else 0.0
	var chroma_scale := 224.0 / 255.0 if not full else 1.0
	var samples := 0
	var abs_sum := 0.0
	var within := 0
	var err_native := 0.0
	var err_swapped := 0.0
	var excursion := 0.0
	var csamples := 0
	var y := 0
	while y < h:
		var x := 0
		while x < w:
			var i := (y * w + x) * 4
			var r := float(rgba[i])
			var g := float(rgba[i + 1])
			var b := float(rgba[i + 2])
			var lum := kr * r + kg * g + kb * b
			var ref_y := luma_offset + luma_scale * lum
			var d := absf(float(y_bytes[y * w + x]) - ref_y)
			abs_sum += d
			if d <= LUMA_WITHIN:
				within += 1
			samples += 1
			if (x % 2) == 0 and (y % 2) == 0:
				var cb := 128.0 + chroma_scale * (b - lum) / (2.0 * (1.0 - kb))
				var cr := 128.0 + chroma_scale * (r - lum) / (2.0 * (1.0 - kr))
				var ci := ((y >> 1) * cw + (x >> 1)) * 2
				var b0 := float(c_bytes[ci])
				var b1 := float(c_bytes[ci + 1])
				# Native order: NV12 interleaves Cb,Cr; NV21 Cr,Cb.
				var first := cb if fourcc == CamBANGServer.PIXEL_FORMAT_NV12 else cr
				var second := cr if fourcc == CamBANGServer.PIXEL_FORMAT_NV12 else cb
				err_native += absf(b0 - first) + absf(b1 - second)
				err_swapped += absf(b0 - second) + absf(b1 - first)
				excursion += absf(cb - 128.0) + absf(cr - 128.0)
				csamples += 1
			x += SAMPLE_STRIDE
		y += SAMPLE_STRIDE
	var mean_abs := abs_sum / float(maxi(samples, 1))
	var within_share := float(within) / float(maxi(samples, 1))
	var mean_excursion := excursion / float(maxi(csamples * 2, 1))
	var chroma := "inconclusive"
	if mean_excursion >= CHROMA_MIN_EXCURSION:
		chroma = "native" if err_native < err_swapped else "swapped"
	_chroma_verdicts.append(chroma)
	_content_lines.append("%s %dx%d fmt=%d range=%s matrix=%s luma_mean_abs=%.2f luma_within_%d=%.3f chroma_err_native=%.1f chroma_err_swapped=%.1f chroma_excursion=%.2f chroma=%s" % [
		what, w, h, fourcc, str(colorimetry.get("range", "")), str(colorimetry.get("matrix", "")),
		mean_abs, LUMA_WITHIN, within_share,
		err_native / float(maxi(csamples, 1)), err_swapped / float(maxi(csamples, 1)), mean_excursion, chroma])
	if mean_abs > LUMA_MEAN_ABS_LIMIT or within_share < LUMA_WITHIN_SHARE:
		return "%s: GPU plane 0 does not match luma from to_image() (mean_abs=%.2f within_%d=%.3f)" % [
			what, mean_abs, LUMA_WITHIN, within_share]
	if chroma == "swapped":
		return "%s: GPU plane 1 chroma is in the swapped order for format %d" % [what, fourcc]
	return ""


func _percentile(values: Array[int], p: float) -> int:
	if values.is_empty():
		return -1
	var sorted := values.duplicate()
	sorted.sort()
	var idx := clampi(int(ceil(p * sorted.size())) - 1, 0, sorted.size() - 1)
	return int(sorted[idx])


func _dist(label: String, values: Array[int]) -> String:
	return "%s(n=%d p50=%d p90=%d p99=%d max=%d)" % [
		label, values.size(), _percentile(values, 0.5), _percentile(values, 0.9),
		_percentile(values, 0.99), _percentile(values, 1.0)]


func _backing_counts(values: Array[int]) -> String:
	var counts := {}
	for v in values:
		counts[v] = int(counts.get(v, 0)) + 1
	var names := {
		CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_NONE: "none",
		CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_CPU_UPLOAD: "cpu_upload",
		CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_COPY: "gpu_copy",
		CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_RETAINED: "gpu_retained",
	}
	var parts: Array[String] = []
	for key in counts.keys():
		parts.append("%s:%d" % [str(names.get(key, key)), int(counts[key])])
	return "{" + ",".join(parts) + "}"


# ---- run ---------------------------------------------------------------------

func _run() -> void:
	if _provider_arg == "synthetic":
		_emit("expected_unsupported", "platform_provider_scene;synthetic_covered_by_scene_74")
		return
	_decide_gate()
	print("INFO: gate_expected=%s (%s)" % [str(_gate_expected), _gate_reason])
	if not await _start():
		return
	var hw := await _pick_hardware_id()
	if hw.is_empty():
		_emit("expected_unsupported", "no_camera_endpoints")
		return
	var dev = CamBANGServer.get_device_for_hardware_id(hw)
	if dev == null:
		_fail("no handle for %s" % hw)
		return
	print("INFO: camera %s" % hw)
	if not await _engage(dev):
		return
	if not await _apply_planar_still_geometry(dev, hw):
		return

	var no_rd := RenderingServer.get_rendering_device() == null

	# Before anything is asked for: nothing is copied speculatively.
	for i in range(PRE_ARM_CAPTURES):
		var what := "pre-arm #%d" % i
		var result = await _capture(dev, what)
		if result == null:
			return
		var backing := int(result.get_compute_texture_backing(0))
		_backings_pre.append(backing)
		var expected := CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_NONE if no_rd \
			else CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_CPU_UPLOAD
		if backing != expected:
			_fail("%s: backing %d before any plane request (expected %d)" % [what, backing, expected])
			return
	if no_rd:
		_emit("expected_unsupported", "no_rendering_device;backing_none_checked")
		return

	# Arming: the first plane request of this session.
	var arm_result = await _capture(dev, "arm")
	if arm_result == null:
		return
	var arm_backing := int(arm_result.get_compute_texture_backing(0))
	_backings_pre.append(arm_backing)
	if arm_backing != CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_CPU_UPLOAD:
		_fail("arm: backing %d before any plane request (expected CPU_UPLOAD)" % arm_backing)
		return
	if await _first_plane(arm_result, arm_backing, "arm") == null:
		return
	print("STEP OK: sidecar demand armed by the first plane request")

	var retained := 0
	var counted := 0
	var content_checked := 0
	for i in range(_captures):
		var what := "post-arm #%d" % i
		var result = await _capture(dev, what)
		if result == null:
			return
		var backing := int(result.get_compute_texture_backing(0))
		_backings_post.append(backing)
		var support := int(result.can_get_compute_texture_member(0))
		if (support == CamBANGCaptureResult.CAPABILITY_READY) != (backing == CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_RETAINED):
			_fail("%s: can_get_compute_texture_member=%d disagrees with backing=%d" % [what, support, backing])
			return
		if backing != CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_RETAINED \
				and backing != CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_CPU_UPLOAD:
			_fail("%s: unexpected backing %d" % [what, backing])
			return
		if not _gate_expected and backing != CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_CPU_UPLOAD:
			_fail("%s: backing %d outside the importer's scope (%s)" % [what, backing, _gate_reason])
			return
		if await _first_plane(result, backing, what) == null:
			return
		if i >= ARM_WARMUP_CAPTURES:
			counted += 1
			if backing == CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_RETAINED:
				retained += 1
			else:
				print("INFO: %s fell back to CPU_UPLOAD" % what)
		if backing == CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_GPU_RETAINED and content_checked < CONTENT_CHECKS:
			var problem := _verify_content(result, what)
			if problem != "":
				_fail(problem)
				return
			content_checked += 1

	var p99 := _percentile(_dispatch_usec, 0.99)
	if p99 > WINDOW_USEC:
		_fail("capture_image_acquired dispatch p99=%dus exceeds the %dus window" % [p99, WINDOW_USEC])
		return
	if not _gate_expected:
		_emit("expected_unsupported", "gate_closed_%s;all_cpu_upload_checked" % _gate_reason)
		return
	var share := float(retained) / float(maxi(counted, 1))
	if counted > 0 and share < MIN_RETAINED_SHARE:
		_fail("only %d of %d post-warmup captures were GPU_RETAINED (share %.2f < %.2f)" % [
			retained, counted, share, MIN_RETAINED_SHARE])
		return
	if content_checked == 0:
		_fail("no GPU_RETAINED capture was available for the content check")
		return
	_emit("ok", "retained=%d/%d;content_checked=%d;chroma=%s;dispatch_p99_usec=%d" % [
		retained, counted, content_checked, ",".join(_chroma_verdicts), p99])
