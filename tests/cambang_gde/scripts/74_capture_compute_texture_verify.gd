extends Node

# Verifies CamBANGCaptureResult.get_compute_texture() end to end: a completed
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
const SHADER_PATH := "res://shaders/capture_compute_probe.glsl"
const TIMEOUT_MS := 20000
const LOCAL_GROUP := 8

var _step := 0
var _done := false
var _verdict_emitted := false
var _quit_requested := false
var _device_instance_id := 0
var _capture_triggered := false
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

	if _device_instance_id == 0:
		_latch_device()
		return
	if not _capture_triggered:
		_trigger_capture()
		return
	_try_verify()


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


func _trigger_capture() -> void:
	var device = CamBANGServer.get_device(_device_instance_id)
	if device == null:
		return
	# trigger_capture() returns { id, error }; the id is the caller's handle for
	# this capture, and the error is the admission outcome.
	var capture: Dictionary = device.trigger_capture()
	var err := int(capture.get("error", FAILED))
	if err != OK:
		# Not yet admissible is normal early in bring-up; keep waiting rather
		# than failing on the first refusal.
		return
	_capture_triggered = true
	_step_ok("capture triggered (id=%s)" % str(capture.get("id", "")))


func _try_verify() -> void:
	var device = CamBANGServer.get_device(_device_instance_id)
	if device == null:
		return
	var result = device.get_result()
	if result == null:
		return

	var support: int = int(result.can_get_compute_texture())
	_step_ok("can_get_compute_texture() = %d" % support)

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		# 11.6.1: no GPU device, so there is nowhere to put a texture. The
		# capability must say so rather than hand back a degraded object.
		if support != result.CAPABILITY_UNSUPPORTED:
			_fail("no RenderingDevice but can_get_compute_texture()=%d (expected UNSUPPORTED=%d)"
				% [support, result.CAPABILITY_UNSUPPORTED])
			return
		if result.get_compute_texture() != null:
			_fail("no RenderingDevice but get_compute_texture() returned an object")
			return
		_step_ok("no RenderingDevice: UNSUPPORTED reported and nothing produced")
		_finish_unsupported("no RenderingDevice under this renderer")
		return

	if support == result.CAPABILITY_UNSUPPORTED:
		_fail("RenderingDevice present but can_get_compute_texture() = UNSUPPORTED")
		return
	if support == result.CAPABILITY_CHEAP:
		# 11.6.1: no path may be declared CHEAP without measured evidence.
		_fail("can_get_compute_texture() declared CHEAP; only READY/EXPENSIVE are derivable")
		return

	var texture = result.get_compute_texture()
	if texture == null or not (texture is Texture2D):
		_fail("get_compute_texture() returned no Texture2D despite support=%d" % support)
		return
	_step_ok("compute texture obtained (class=%s support=%d)" % [texture.get_class(), support])

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
	var again = result.get_compute_texture()
	var after: Dictionary = _compute_texture_metrics()
	if again == null:
		_fail("second get_compute_texture() returned null")
		return
	if int(after.get("uploads", -1)) != int(before.get("uploads", -2)):
		_fail("second get_compute_texture() uploaded again (uploads %d -> %d)"
			% [int(before.get("uploads", -1)), int(after.get("uploads", -1))])
		return
	_step_ok("repeat access served without re-upload (uploads=%d hits=%d)"
		% [int(after.get("uploads", -1)), int(after.get("hits", -1))])

	_run_compute(rd, rd_texture, result)


func _run_compute(rd: RenderingDevice, rd_texture: RID, result) -> void:
	var width: int = int(result.get_width())
	var height: int = int(result.get_height())
	if width <= 0 or height <= 0:
		_fail("capture reports non-positive dimensions %dx%d" % [width, height])
		return

	# Compiled from source rather than load()ed as an RDShaderFile: that resource
	# type only exists after Godot's editor import step has run, which a
	# --scene harness launch does not perform. Reading the text and compiling it
	# here makes the scene independent of the import pipeline.
	if not FileAccess.file_exists(SHADER_PATH):
		_fail("shader source missing at %s" % SHADER_PATH)
		return
	var source_text := FileAccess.get_file_as_string(SHADER_PATH)
	if source_text.is_empty():
		_fail("shader source at %s is empty" % SHADER_PATH)
		return
	# Strip the RDShaderFile stage marker; RDShaderSource takes the bare stage.
	var stripped_lines := PackedStringArray()
	for line in source_text.split("
"):
		if line.begins_with("#["):
			continue
		stripped_lines.append(line)
	var shader_source := RDShaderSource.new()
	shader_source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	shader_source.source_compute = "
".join(stripped_lines)
	var spirv: RDShaderSPIRV = rd.shader_compile_spirv_from_source(shader_source, false)
	if spirv == null:
		_fail("shader produced no SPIR-V")
		return
	var compile_error: String = spirv.compile_error_compute
	if not compile_error.is_empty():
		_fail("compute shader compile error: %s" % compile_error)
		return
	var shader: RID = rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		_fail("shader_create_from_spirv() failed")
		return
	var pipeline: RID = rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		_fail("compute_pipeline_create() failed")
		return

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
		return

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
		return
	var pixel_count := read.decode_u32(0)
	var red_sum := read.decode_u32(4)
	_free_all(rd, [pipeline, shader, accum_buffer, sampler])

	var expected_pixels := width * height
	if pixel_count != expected_pixels:
		_fail("compute saw %d pixels, expected %d" % [pixel_count, expected_pixels])
		return
	_step_ok("compute covered every pixel (%d)" % pixel_count)

	# Content proof: the same sum computed on the CPU from the same member.
	var image: Image = result.to_image()
	if image == null:
		_fail("to_image() returned null; cannot cross-check compute content")
		return
	var cpu_sum := 0
	var data: PackedByteArray = image.get_data()
	var stride := 4
	var i := 0
	while i < data.size():
		cpu_sum += data[i]
		i += stride
	print("compute red_sum=%d cpu red_sum=%d ratio=%.4f"
		% [red_sum, cpu_sum, (float(red_sum) / float(cpu_sum)) if cpu_sum > 0 else 0.0])
	if cpu_sum <= 0:
		_fail("CPU reference sum is zero; capture image appears empty")
		return
	if red_sum == 0:
		_fail("compute read all-zero pixels from a non-empty capture")
		return
	var ratio := float(red_sum) / float(cpu_sum)
	if ratio < 0.98 or ratio > 1.02:
		_fail("compute content does not match CPU reference (compute=%d cpu=%d ratio=%.4f)"
			% [red_sum, cpu_sum, ratio])
		return
	_step_ok("compute content matches CPU reference (ratio=%.4f)" % ratio)

	_finish_ok()


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
