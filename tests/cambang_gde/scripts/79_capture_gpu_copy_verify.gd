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
## - the GPU copy holds the CPU payload's bytes: decoding planes 0 and 1 with
##   Core's own integer conversion, in the member format's chroma order
##   (NV12 = Cb,Cr; NV21 = Cr,Cb), reproduces to_image() exactly;
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


func _clamp_u8(v: int) -> int:
	return clampi(v, 0, 255)


# Exactly Core's integer YUV -> RGB (src/pixels/format/yuv_convert.h,
# yuv_to_rgb_bt601_full / _limited), so a decode of the GPU planes can be held
# to to_image() byte for byte. GDScript's >> is arithmetic on negative ints,
# as the C++ is on every supported compiler.
func _decode(yv: int, u: int, v: int, full: bool) -> Vector3i:
	var d := u - 128
	var e := v - 128
	if full:
		return Vector3i(
			_clamp_u8(yv + ((359 * e + 128) >> 8)),
			_clamp_u8(yv + ((-88 * d - 183 * e + 128) >> 8)),
			_clamp_u8(yv + ((454 * d + 128) >> 8)))
	var c := yv - 16
	return Vector3i(
		_clamp_u8((298 * c + 409 * e + 128) >> 8),
		_clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8),
		_clamp_u8((298 * c + 516 * d + 128) >> 8))


# Proves the GPU copy holds the CPU payload's bytes. to_image() is Core's
# conversion of the CPU planes; decoding the GPU planes with the same integer
# conversion must reproduce it exactly at every sampled pixel. Any differing
# Y, Cb or Cr byte changes at least one channel unless clamping hides it, so
# an exact match on a lit, non-saturated scene is a byte-level proof in
# practice. The same decode with Cb/Cr swapped is reported alongside: on a
# scene with colour it must mismatch, which shows the check can see the order.
# Returns "" or the failure.
func _verify_content(result, what: String) -> String:
	var fourcc := int(result.get_format())
	if fourcc != CamBANGServer.PIXEL_FORMAT_NV12 and fourcc != CamBANGServer.PIXEL_FORMAT_NV21:
		return "%s: GPU_RETAINED member has format %d, expected NV12/NV21" % [what, fourcc]
	var member: Dictionary = result.get_image_member(0)
	var colorimetry: Dictionary = member.get("colorimetry", {})
	var full := str(colorimetry.get("range", "unspecified")) == "full"
	var image: Image = result.to_image()
	if image == null:
		return "%s: to_image() returned null" % what
	if image.get_format() != Image.FORMAT_RGBA8:
		return "%s: to_image() format %d, expected RGBA8" % [what, image.get_format()]
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
	# Native byte order within a chroma pair: NV12 is Cb,Cr; NV21 is Cr,Cb.
	var u_off := 0 if fourcc == CamBANGServer.PIXEL_FORMAT_NV12 else 1
	var v_off := 1 - u_off
	var samples := 0
	var exact := 0
	var swapped_exact := 0
	var max_diff := 0
	var first_miss := ""
	var y := 0
	while y < h:
		var x := 0
		while x < w:
			var ci := ((y >> 1) * cw + (x >> 1)) * 2
			var yv := int(y_bytes[y * w + x])
			var b0 := int(c_bytes[ci])
			var b1 := int(c_bytes[ci + 1])
			var u := b0 if u_off == 0 else b1
			var v := b1 if v_off == 1 else b0
			var got := _decode(yv, u, v, full)
			var swp := _decode(yv, v, u, full)
			var i := (y * w + x) * 4
			var want := Vector3i(int(rgba[i]), int(rgba[i + 1]), int(rgba[i + 2]))
			var diff := maxi(absi(got.x - want.x), maxi(absi(got.y - want.y), absi(got.z - want.z)))
			max_diff = maxi(max_diff, diff)
			if diff == 0:
				exact += 1
			elif first_miss == "":
				first_miss = "(%d,%d) gpu_yuv=%d,%d,%d gpu_rgb=%s cpu_rgb=%s" % [x, y, yv, u, v, str(got), str(want)]
			if swp == want:
				swapped_exact += 1
			samples += 1
			x += SAMPLE_STRIDE
		y += SAMPLE_STRIDE
	var swapped_share := float(swapped_exact) / float(maxi(samples, 1))
	var chroma := "native"
	if swapped_share >= 0.99:
		chroma = "inconclusive_grey_scene"
	_chroma_verdicts.append(chroma)
	_content_lines.append("%s %dx%d fmt=%d range=%s matrix=%s samples=%d exact=%d max_diff=%d swapped_exact_share=%.3f chroma=%s first_miss=%s" % [
		what, w, h, fourcc, str(colorimetry.get("range", "")), str(colorimetry.get("matrix", "")),
		samples, exact, max_diff, swapped_share, chroma, first_miss])
	if exact != samples:
		return "%s: GPU planes decode differently from to_image() at %d of %d samples (max_diff=%d first=%s)" % [
			what, samples - exact, samples, max_diff, first_miss]
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
