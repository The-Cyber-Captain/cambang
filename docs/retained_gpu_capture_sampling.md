# Sampling a retained capture on the GPU

Open the CamBANG test project and run [the retained capture scene](../tests/cambang_gde/scenes/83_retained_capture_sampling.tscn) on Quest. The scene selects Camera2 hardware ID `50` by default. It shows the ordinary still, marks two regions, and displays colours from the application's compute shader. It automatically samples both regions again after a later capture. Tap or click the image to move the regions in the flat view.

The scene submits the first sample from the GPU readiness handler. It checks
the early acquisition mark against the ordinary result for the same capture
and member, and saves that result with `to_image()` on a worker thread.
The panel shows the capture ID, acquisition mark, colour swatches and
`user://retained_capture.png`. Each run replaces the previous demo PNG.

## Claim the captured member

Use the Device Capture Id from the trigger receipt to associate the early
`capture_image_acquired` notice with the final result. For a rig trigger, use
each device capture id from the receipt's `members` map. The member index
identifies the image within that result.

```gdscript
var receipt := device.trigger_capture({"keep_gpu_backing": true})
# After capture_finished for receipt.id:
var result := CamBANGServer.get_capture_result_by_id(receipt.id)
var claim := result.claim_gpu_backing(0)
if claim.error == OK:
    backing = claim.backing
```

To start GPU work independently of CPU capture completion, connect
`CamBANGServer.capture_gpu_backing_ready(capture_id, info)` before triggering.
In the handler, call `CamBANGServer.claim_capture_gpu_backing(capture_id, info.image_member_index)`.
The signal runs on Godot's main thread when the retained GPU copy is available.
It carries the same Device Capture Id and member index as the ordinary result,
plus `info.colorimetry` for interpreting the GPU planes immediately. CPU
delivery may still be pending or may fail. Check `capture_finished` for the
ordinary capture outcome. A member with no GPU backing emits no readiness
signal.

Both claim methods consume the same offer. Claim through the signal when the
GPU work can start early, or through the result when handling both together.

Keep the backing while you need to use it. One claim grants a reference for
multiple compute requests. The allocation is released when the final
reference is dropped and accepted GPU work retires. On Camera2, an unclaimed
offer expires after approximately five seconds. An unavailable or expired
offer leaves the ordinary capture accessible through `to_image()`.

## Supply your own compute shader

Pass a precompiled `RDShaderSPIRV` compute program, raw push-constant bytes,
a dispatch size, an output size, and a completion callback. The
[demo shader source](../tests/cambang_gde/shaders/retained_capture_sample.comp)
uses an 8×8 sample grid to estimate the colour of a normalized UV rectangle.
The corresponding precompiled resource works with Compatibility because the
provider runs it on its own GPU device.

The first supported shader input layout uses descriptor set 0.

| Binding | Shader declaration | Contents |
| --- | --- | --- |
| 0 | `sampler2D` | Read-only R8 UNORM luma at capture resolution |
| 1 | `sampler2D` | Read-only RG8 UNORM interleaved Cb, Cr at half resolution |
| 2 | `std430` storage buffer | Output initialized to zero |

Use `textureSize()` for the plane dimensions. Coordinates start at the top
left of the captured member. A luma pixel coordinate divided by two identifies
its chroma texel. The ordinary result's member colourimetry describes the
encoding when available. The demo explicitly assumes limited-range BT.601
when the camera leaves it unspecified.

```gdscript
var admitted := backing.submit_compute(shader, parameters, Vector3i.ONE, 16,
    func(error: int, bytes: PackedByteArray):
        if error == OK:
            var rgb := Color(bytes.decode_float(0), bytes.decode_float(4), bytes.decode_float(8))
            apply_sample(rgb))
```

`OK` from submission means the provider has accepted the work. The callback
runs on Godot's main thread and reports shader validation, dispatch, or
cancellation errors separately. Submission can return `ERR_BUSY` while the
bounded queue is full. Supply trusted shaders with a finite execution time and
bounded accesses. The current Camera2 implementation accepts Vulkan 1.1
compute bytecode up to 64 KiB, dispatches of up to 4,096 workgroups in total,
up to 128 bytes of 32-bit scalar or vector push constants, and output sizes of
4 through 4,096 bytes in multiples of four.
