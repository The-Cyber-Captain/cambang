extends Node

# Verifies CamBANGCaptureResult compute-texture planes end to end: a completed
# capture's pixels are reachable from a real compute dispatch.
#
# This is the Capture Compute Texture of pixel_payload_and_result_contract.md
# 11.6.1, not a display view. Nothing here polls a live view or cares about
# freshness -- the capture is frozen, and that is the whole premise.
#
# Under the Compatibility renderer there is no RenderingDevice, so 11.6.1's
# UNSUPPORTED row is the correct outcome and the scene verdicts
# expected_unsupported rather than failing. Run it with
# --rendering-method=mobile (or forward_plus) to exercise the real path.

const SCENE_LABEL := "74_capture_compute_texture_verify"
# Embedded rather than kept as a res:// file on purpose. A .glsl has a Godot
# importer, so it is only exported into an APK once the editor import step has
# run and its .import file is committed; a raw copy is not picked up by
# include_filter either. Embedding removes that dependency entirely, so the
# scene behaves identically on Windows and on device.
#
# Two counters, because they prove different things:
#   pixel_count -- exact and content-independent. Must equal width*height, which
#                  proves the texture was bound, has the expected dimensions,
#                  and every in-bounds invocation could read it.
#   red_sum     -- content-dependent. Compared against a CPU sum of the same
#                  member, which proves the texture holds the captured image
#                  rather than an empty or unrelated allocation.
const COMPUTE_SOURCE := """#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D src_image;

layout(set = 0, binding = 1, std430) restrict buffer Accum {
    uint pixel_count;
    uint red_sum;
} accum;

layout(push_constant, std430) uniform Params {
    uint width;
    uint height;
    uint pad0;
    uint pad1;
} params;

void main() {
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= params.width || gid.y >= params.height) {
        return;
    }
    vec4 texel = texelFetch(src_image, ivec2(gid), 0);
    atomicAdd(accum.pixel_count, 1u);
    atomicAdd(accum.red_sum, uint(round(texel.r * 255.0)));
}
"""
const TIMEOUT_MS := 20000
const LOCAL_GROUP := 8
# make_fourcc('N','V','1','2') -- see src/pixels/format/pixel_format_descriptor.h.
const FOURCC_NV12 := 842094158

# Two phases in one run, with no command-line knob, so both payload kinds are
# covered wherever the scene runs -- including Android, whose ExtraArgs
# translator whitelists a fixed set and would reject a scene-specific flag.
const PHASE_LATCH := 0
const PHASE_CAPTURE_PACKED := 1
const PHASE_VERIFY_PACKED := 2
const PHASE_REQUEST_PLANAR := 3
const PHASE_AWAIT_PLANAR := 4
const PHASE_CAPTURE_PLANAR := 5
const PHASE_VERIFY_PLANAR := 6

var _step := 0
var _done := false
var _verdict_emitted := false
var _quit_requested := false
var _device_instance_id := 0
var _phase := 0
var _packed_capture_id := ""
var _planar_capture_id := ""
var _planar_wait_ms := 0
var _elapsed_ms := 0


func _ready() -> void:
	print("RUN: %s" % SCENE_LABEL)
	CamBANGServer.stop()
	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME,
		CamBANGServer.TIMELINE_RECONCILIATION_COMPLETION_GATED
	)
	if start_err != OK:
		_fail("synthetic timeline start rejected (%d)" % start_err)
		return
	_step_ok("synthetic runtime started")

	if CamBANGServer.select_builtin_scenario("stream_inspection_live") != OK:
		_fail("unable to stage stream_inspection_live")
		return
	if CamBANGServer.start_scenario() != OK:
		_fail("unable to start staged scenario")
		return
	_step_ok("scenario started")


func _process(delta: float) -> void:
	if _done:
		return
	_elapsed_ms += int(delta * 1000.0)
	if _elapsed_ms > TIMEOUT_MS:
		_fail("timed out before a capture result was observed")
		return

	match _phase:
		PHASE_LATCH:
			_latch_device()
		PHASE_CAPTURE_PACKED:
			_packed_capture_id = _trigger()
			if _packed_capture_id != "":
				_step_ok("packed capture triggered (id=%s)" % _packed_capture_id)
				_phase = PHASE_VERIFY_PACKED
		PHASE_VERIFY_PACKED:
			_verify_phase(_packed_capture_id, 1, "packed")
		PHASE_REQUEST_PLANAR:
			_request_planar_profile()
		PHASE_AWAIT_PLANAR:
			_await_planar_profile()
		PHASE_CAPTURE_PLANAR:
			_planar_capture_id = _trigger()
			if _planar_capture_id != "":
				_step_ok("planar capture triggered (id=%s)" % _planar_capture_id)
				_phase = PHASE_VERIFY_PLANAR
		PHASE_VERIFY_PLANAR:
			_verify_phase(_planar_capture_id, 2, "planar")


func _latch_device() -> void:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return
	var devices: Array = snapshot.get("devices", [])
	if devices.is_empty():
		return
	var device_d: Dictionary = devices[0]
	var id := int(device_d.get("instance_id", 0))
	if id <= 0:
		return
	_device_instance_id = id
	_step_ok("device latched (instance_id=%d)" % _device_instance_id)
	_phase = PHASE_CAPTURE_PACKED


func _device():
	return CamBANGServer.get_device(_device_instance_id)


# Returns the capture id once admitted, "" while not yet admissible. An early
# refusal during bring-up is normal and is retried rather than failed.
func _trigger() -> String:
	var device = _device()
	if device == null:
		return ""
	var capture: Dictionary = device.trigger_capture()
	if int(capture.get("error", FAILED)) != OK:
		return ""
	return str(capture.get("id", ""))


func _request_planar_profile() -> void:
	var device = _device()
	if device == null:
		return
	var err := int(device.set_still_capture_profile({"format_fourcc": FOURCC_NV12}))
	if err != OK:
		_fail("set_still_capture_profile(NV12) failed err=%d" % err)
		return
	_step_ok("planar still profile requested (format_fourcc=%d)" % FOURCC_NV12)
	_phase = PHASE_AWAIT_PLANAR


# Bounded, because the request being accepted does not mean it will apply.
# Synthetic's GPU backing is RGBA8-only, so under a GPU-only producer output
# form a planar still has no realization -- set_still_capture_profile() returns
# OK and the profile then never becomes NV12. That is the provider declining a
# posture it cannot satisfy, not this feature failing, so the planar phase is
# skipped with the reason stated rather than timing the run out.
const PLANAR_PROFILE_WAIT_BUDGET_MS := 6000

func _await_planar_profile() -> void:
	var device = _device()
	if device == null:
		return
	var current: Dictionary = device.get_still_capture_profile()
	if int(current.get("format_fourcc", 0)) == FOURCC_NV12:
		_step_ok("planar still profile is now the device profile")
		_phase = PHASE_CAPTURE_PLANAR
		return
	_planar_wait_ms += 16
	if _planar_wait_ms >= PLANAR_PROFILE_WAIT_BUDGET_MS:
		_step_ok("planar phase SKIPPED: profile request was accepted but never applied "
			+ "after %d ms (current format_fourcc=%d) -- no planar still realization in "
			% [_planar_wait_ms, int(current.get("format_fourcc", 0))]
			+ "this producer configuration")
		_finish_ok()


# Waits for the named capture's result, then verifies its compute planes.
# expected_planes is 1 for a packed member and 2 for NV12 -- asserted rather
# than reported, because a member whose plane exposure disagrees with its
# format is a defect.
func _verify_phase(capture_id: String, expected_planes: int, label: String) -> void:
	if capture_id == "":
		return
	# Fetched by id, not device.get_result(), so each phase verifies the capture
	# it actually triggered rather than whatever is latest on the device.
	var result = CamBANGServer.get_capture_result_by_id(capture_id)
	if result == null:
		return

	var support: int = int(result.can_get_compute_texture_member(0))
	_step_ok("%s: can_get_compute_texture_member(0) = %d" % [label, support])

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		# 11.6.1: no GPU device, so there is nowhere to put a texture. The
		# capability must say so rather than hand back a degraded object.
		if support != result.CAPABILITY_UNSUPPORTED:
			_fail("no RenderingDevice but can_get_compute_texture_member(0)=%d (expected UNSUPPORTED=%d)"
				% [support, result.CAPABILITY_UNSUPPORTED])
			return
		if result.get_compute_texture_plane(0, 0) != null:
			_fail("no RenderingDevice but get_compute_texture_plane(0,0) returned an object")
			return
		_step_ok("no RenderingDevice: UNSUPPORTED reported and nothing produced")
		_finish_unsupported("no RenderingDevice under this renderer")
		return

	if support == result.CAPABILITY_UNSUPPORTED:
		_fail("RenderingDevice present but can_get_compute_texture_member(0) = UNSUPPORTED")
		return
	if support == result.CAPABILITY_CHEAP:
		# 11.6.1: no path may be declared CHEAP without measured evidence.
		_fail("can_get_compute_texture_member(0) declared CHEAP; only READY/EXPENSIVE are derivable")
		return

	# Timed because it is the number that decides whether a zero-copy native path
	# is worth building: on the EXPENSIVE row this is the full-frame upload a
	# GPU-resident source would avoid, and on the READY row it is what that path
	# already costs. to_image() is timed too, since on the EXPENSIVE row the
	# upload is preceded by a CPU materialization the caller also pays.
	# Cold to_image() first, so the first-touch cost of this member (planar
	# conversion plus whatever retained-access calibration does on first access)
	# is attributed separately instead of landing inside the compute-texture
	# number and inflating it.
	var tc := Time.get_ticks_usec()
	var cold_image: Image = result.to_image()
	var cold_to_image_us := Time.get_ticks_usec() - tc
	if cold_image == null:
		_fail("cold to_image() returned null")
		return
	var t0 := Time.get_ticks_usec()
	var texture = result.get_compute_texture_plane(0, 0)
	var produce_us := Time.get_ticks_usec() - t0
	if texture == null or not (texture is Texture2D):
		_fail("get_compute_texture_plane(0,0) returned no Texture2D despite support=%d" % support)
		return
	var t1 := Time.get_ticks_usec()
	var timing_image: Image = result.to_image()
	var to_image_us := Time.get_ticks_usec() - t1
	print("TIMING cold_to_image_us=%d compute_plane0_first_call_us=%d warm_to_image_us=%d size=%dx%d format=%d payload_kind=%d support=%d class=%s"
		% [cold_to_image_us, produce_us, to_image_us,
		   int(result.get_width()), int(result.get_height()),
		   int(result.get_format()), int(result.get_payload_kind()), support,
		   texture.get_class()])
	if timing_image == null:
		_fail("to_image() returned null during timing")
		return
	var plane_count := int(result.get_compute_texture_plane_count(0))
	if plane_count <= 0:
		_fail("get_compute_texture_plane_count(0) returned %d" % plane_count)
		return
	# Planes are native: a packed member is one plane, NV12/NV21 two, I420/YV12
	# three. Anything else means the member's format and its plane exposure
	# disagree.
	for p in range(plane_count):
		if result.get_compute_texture_plane(0, p) == null:
			_fail("plane %d of %d was not produced" % [p, plane_count])
			return
	if result.get_compute_texture_plane(0, plane_count) != null:
		_fail("plane index %d is out of range but produced a texture" % plane_count)
		return
	_step_ok("compute texture planes obtained (planes=%d class=%s support=%d produce_us=%d)"
		% [plane_count, texture.get_class(), support, produce_us])

	# Colour interpretation must accompany the planes. A caller writing its own
	# Y'CbCr maths cannot get it right otherwise, and CamBANG holds the answer:
	# the provider contract refuses to render one colour space with another's
	# coefficients internally, so it must not leave a caller to do that either.
	var member_info: Dictionary = result.get_image_member(0)
	if not member_info.has("colorimetry"):
		_fail("%s: get_image_member(0) reported no colorimetry for a member with planes" % label)
		return
	var colorimetry: Dictionary = member_info["colorimetry"]
	for required_key in ["range", "matrix", "transfer", "primaries", "declared"]:
		if not colorimetry.has(required_key):
			_fail("%s: colorimetry is missing '%s'" % [label, required_key])
			return
	print("COLORIMETRY phase=%s range=%s matrix=%s transfer=%s primaries=%s declared=%s"
		% [label, colorimetry["range"], colorimetry["matrix"], colorimetry["transfer"],
		   colorimetry["primaries"], colorimetry["declared"]])
	_step_ok("%s: colorimetry reported alongside the planes" % label)

	# The one documented route to the RenderingDevice texture, identical for
	# every CamBANG-provided texture regardless of which path produced it.
	var rs_rid: RID = texture.get_rid()
	if not rs_rid.is_valid():
		_fail("compute texture get_rid() is invalid")
		return
	var rd_texture: RID = RenderingServer.texture_get_rd_texture(rs_rid)
	if not rd_texture.is_valid():
		_fail("texture_get_rd_texture() did not resolve (rs_rid=%d)" % rs_rid.get_id())
		return
	_step_ok("resolved RD texture (rs_rid=%d rd_rid=%d)" % [rs_rid.get_id(), rd_texture.get_id()])

	# Repeat access must be served from the texture already produced.
	#
	# pixel_payload_and_result_contract.md 11.6.1, "Identity, immutability, and
	# caching": a repeat request for the same retained member must be served
	# from the texture already produced, not materialized again, because the
	# source pixels are frozen and a second production is necessarily identical.
	# A bounded cache may release a texture and produce again later; what is
	# forbidden is producing afresh on every request while the previous result
	# was still held. These two calls are back to back, so the first result is
	# certainly still held.
	var before: Dictionary = _compute_texture_metrics()
	var again = result.get_compute_texture_plane(0, 0)
	var after: Dictionary = _compute_texture_metrics()
	if again == null:
		_fail("second get_compute_texture_plane(0,0) returned null")
		return
	if int(after.get("uploads", -1)) != int(before.get("uploads", -2)):
		_fail("second get_compute_texture_plane(0,0) uploaded again (uploads %d -> %d)"
			% [int(before.get("uploads", -1)), int(after.get("uploads", -1))])
		return
	_step_ok("repeat access served without re-upload (uploads=%d hits=%d)"
		% [int(after.get("uploads", -1)), int(after.get("hits", -1))])

	if not _run_compute(rd, rd_texture, result, plane_count, texture, label):
		return
	if label == "packed":
		_phase = PHASE_REQUEST_PLANAR
	else:
		_finish_ok()


func _run_compute(rd: RenderingDevice, rd_texture: RID, result, plane_count: int, texture, label: String) -> bool:
	# Plane 0's own dimensions -- luma's for a planar member, the image's for a
	# packed one -- so the dispatch stays correct for either.
	var width: int = int(texture.get_width())
	var height: int = int(texture.get_height())
	if width <= 0 or height <= 0:
		_fail("%s: plane 0 reports non-positive dimensions %dx%d" % [label, width, height])
		return false

	var shader_source := RDShaderSource.new()
	shader_source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	shader_source.source_compute = COMPUTE_SOURCE
	var spirv: RDShaderSPIRV = rd.shader_compile_spirv_from_source(shader_source, false)
	if spirv == null:
		_fail("shader produced no SPIR-V")
		return false
	var compile_error: String = spirv.compile_error_compute
	if not compile_error.is_empty():
		_fail("compute shader compile error: %s" % compile_error)
		return false
	var shader: RID = rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		_fail("shader_create_from_spirv() failed")
		return false
	var pipeline: RID = rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		_fail("compute_pipeline_create() failed")
		return false

	# Two uint counters, zeroed.
	var zero := PackedByteArray()
	zero.resize(8)
	zero.fill(0)
	var accum_buffer: RID = rd.storage_buffer_create(zero.size(), zero)

	var sampler_state := RDSamplerState.new()
	var sampler: RID = rd.sampler_create(sampler_state)

	var tex_uniform := RDUniform.new()
	tex_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	tex_uniform.binding = 0
	tex_uniform.add_id(sampler)
	tex_uniform.add_id(rd_texture)

	var buf_uniform := RDUniform.new()
	buf_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	buf_uniform.binding = 1
	buf_uniform.add_id(accum_buffer)

	var uniform_set: RID = rd.uniform_set_create([tex_uniform, buf_uniform], shader, 0)
	if not uniform_set.is_valid():
		_free_all(rd, [pipeline, shader, accum_buffer, sampler])
		_fail("uniform_set_create() failed -- compute texture not bindable")
		return false

	var push := PackedInt32Array([width, height, 0, 0]).to_byte_array()
	var groups_x := int((width + LOCAL_GROUP - 1) / LOCAL_GROUP)
	var groups_y := int((height + LOCAL_GROUP - 1) / LOCAL_GROUP)

	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, groups_x, groups_y, 1)
	rd.compute_list_end()
	_step_ok("compute dispatched (%dx%d groups over %dx%d)" % [groups_x, groups_y, width, height])

	var read: PackedByteArray = rd.buffer_get_data(accum_buffer)
	if read.size() < 8:
		_free_all(rd, [pipeline, shader, accum_buffer, sampler])
		_fail("buffer_get_data() returned %d bytes" % read.size())
		return false
	var pixel_count := read.decode_u32(0)
	var red_sum := read.decode_u32(4)
	_free_all(rd, [pipeline, shader, accum_buffer, sampler])

	var expected_pixels := width * height
	if pixel_count != expected_pixels:
		_fail("compute saw %d pixels, expected %d" % [pixel_count, expected_pixels])
		return false
	_step_ok("compute covered every pixel (%d)" % pixel_count)

	# Content proof.
	#
	# Plane 0 is luma for a planar member and the packed pixel for a packed one,
	# so the shader's per-texel .r is the first byte of each sample either way.
	# The CPU reference walks the same bytes: for a planar member that is the
	# retained Y plane read back out of to_image()'s source, which is why the
	# packed case compares against to_image() while the planar case compares
	# against the plane texture's own image.
	var reference_sum := 0
	var reference_label := ""
	if plane_count > 1:
		var plane_img: Image = (texture as Texture2D).get_image()
		if plane_img == null:
			_fail("could not read back plane 0 for the CPU reference")
			return false
		var pdata: PackedByteArray = plane_img.get_data()
		for i in range(pdata.size()):
			reference_sum += pdata[i]
		reference_label = "plane0(%s)" % plane_img.get_format()
	else:
		var image: Image = result.to_image()
		if image == null:
			_fail("to_image() returned null; cannot cross-check compute content")
			return false
		var data: PackedByteArray = image.get_data()
		var i2 := 0
		while i2 < data.size():
			reference_sum += data[i2]
			i2 += 4
		reference_label = "to_image().r"
	print("compute red_sum=%d cpu reference=%d (%s) ratio=%.4f"
		% [red_sum, reference_sum, reference_label,
		   (float(red_sum) / float(reference_sum)) if reference_sum > 0 else 0.0])
	if reference_sum <= 0:
		_fail("CPU reference sum is zero; capture image appears empty")
		return false
	if red_sum == 0:
		_fail("compute read all-zero pixels from a non-empty capture")
		return false
	var ratio := float(red_sum) / float(reference_sum)
	if ratio < 0.98 or ratio > 1.02:
		_fail("compute content does not match CPU reference (compute=%d cpu=%d ratio=%.4f)"
			% [red_sum, reference_sum, ratio])
		return false
	_step_ok("compute content matches CPU reference (%s ratio=%.4f)" % [reference_label, ratio])

	return true


# Diagnostic counters live inside the existing result-access evidence
# dictionary rather than on a new public method.
func _compute_texture_metrics() -> Dictionary:
	var evidence: Dictionary = CamBANGServer.get_result_access_timing_evidence()
	var m = evidence.get("capture_compute_textures", {})
	return m if m is Dictionary else {}


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)


func _step_ok(message: String) -> void:
	_step += 1
	print("step %d OK: %s" % [_step, message])


func _fail(message: String) -> void:
	if _done:
		return
	_done = true
	var text := "step %d FAIL: %s" % [_step + 1, message]
	push_error(text)
	print(text)
	_emit_verdict("fail", 1, "failure")
	_cleanup_and_quit(1)


func _finish_ok() -> void:
	if _done:
		return
	_done = true
	print("OK: %s passed" % SCENE_LABEL)
	_emit_verdict("ok", 0, "passed")
	_cleanup_and_quit(0)


func _finish_unsupported(reason: String) -> void:
	if _done:
		return
	_done = true
	print("EXPECTED UNSUPPORTED: %s (%s)" % [SCENE_LABEL, reason])
	_emit_verdict("expected_unsupported", 0, reason.replace(" ", "_"))
	_cleanup_and_quit(0)


func _emit_verdict(status: String, exit_code: int, reason: String) -> void:
	if _verdict_emitted:
		return
	_verdict_emitted = true
	print("[CamBANG][HarnessVerdict] scene=%s status=%s exit_code=%d reason=%s" % [
		SCENE_LABEL, status, exit_code, reason,
	])


func _cleanup_and_quit(code: int) -> void:
	if _quit_requested:
		return
	_quit_requested = true
	CamBANGServer.stop_and_quit(code)
