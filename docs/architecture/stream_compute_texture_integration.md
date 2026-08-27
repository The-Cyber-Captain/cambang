# Stream Compute Texture Integration

**Audience: integrators.** This document is written for someone building a
Godot application against CamBANG from outside this repository, who cannot read
the source when a description is ambiguous. Everything here is therefore stated
as something to read at runtime rather than something to assume.

**Normative source: `pixel_payload_and_result_contract.md` §11.6.2**, with the
colorimetry obligation in §6.2 and the pixel-format pin semantics in §6.3.0.
This document defines nothing of its own; it reorders those rules into the
sequence a caller meets them and adds worked examples. Where it and the
contract disagree, the contract wins and this document is wrong.

Public result vocabulary is fixed by `naming.md`.

---

## What this surface is

Native, per-frame image planes from a live camera stream, as GPU textures you
can sample in your own shader.

It is **additional to** `get_display_view()`, not a replacement. Use the display
view to put a viewfinder on screen. Use this when you need the frame's own
pixels — to run your own conversion, analysis or compute — and when you need
those pixels to correspond to a specific frame.

## Prerequisite: a RenderingDevice

The surface requires a `RenderingDevice`, so your project must use **Forward+ or
Mobile**. Under the Compatibility renderer the surface reports UNSUPPORTED.

That is a correct answer rather than a fault, and it is not a configuration
mistake on CamBANG's part — Compatibility is a reasonable default for a project
to declare. But it does mean the whole surface is unavailable, so check for it
rather than assuming it.

## Obtaining a result

```gdscript
CamBANGServer.start()

var endpoints = CamBANGServer.enumerate_devices()   # Array[Dictionary]: hardware_id, name, ...
var hw := str((endpoints[0] as Dictionary)["hardware_id"])
var device = CamBANGServer.get_device_for_hardware_id(hw)

# engage() can return ERR_BUSY. Retry across frames rather than treating it as failure.
while int(device.engage()) != OK:
    await get_tree().process_frame

var stream = device.create_stream({
    "intent": CamBANGStream.INTENT_PREVIEW,
    "profile": {"width": 1280, "height": 720},   # your choice; the provider may adjust it
})
if stream == null:
    return    # see "When something is refused"
stream.start()
```

`create_stream()` returns **null** on refusal, not an `Error`. Every refusal
prints a reason to the Godot error log identifying which one it was.

## Reading frames

There is no per-frame signal. Poll once per frame and keep the result for that
frame:

```gdscript
func _process(_delta: float) -> void:
    var result = stream.get_result()
    if result == null:
        return                       # nothing retained yet
```

`get_result()` returns a **new result object on every call**, wrapping whichever
frame is currently retained. Call it once per frame and hold that object for the
frame; calling it repeatedly produces independent objects that each do their own
work.

To detect whether the frame actually advanced, compare the acquisition mark:

```gdscript
var timing: Dictionary = result.get_camera_facts()["acquisition_timing"]
# { origin, acquisition_mark, tick_period_numerator_ns, tick_period_denominator,
#   clock_domain, reference_event, comparability }
```

## Capability

```gdscript
if int(result.can_get_compute_texture()) == CamBANGStreamResult.CAPABILITY_UNSUPPORTED:
    return
```

**`CAPABILITY_READY` is `0` — zero is the best answer, not the absent one.**
Always compare against the named constants (`CAPABILITY_READY`,
`CAPABILITY_CHEAP`, `CAPABILITY_EXPENSIVE`, `CAPABILITY_UNSUPPORTED`), never
against a bare number. Comparing against a literal `0` has already inverted the
meaning once inside this project.

This surface reports either `CAPABILITY_EXPENSIVE` or `CAPABILITY_UNSUPPORTED`.
Producing a plane is a full-frame copy, so it is never claimed to be cheap, and
it never wraps existing GPU memory, so it is never READY.

## The planes

```gdscript
var count := int(result.get_compute_texture_plane_count())
var plane_0: Texture2D = result.get_compute_texture_plane(0)
```

**Plane layout follows the delivered pixel format, which you must read.** It is
not something you choose and not something to assume:

```gdscript
var fmt := int(result.get_format())
```

| Delivered format | Planes | Plane 0 | Remaining planes |
|---|---|---|---|
| Packed (e.g. `PIXEL_FORMAT_RGBA`, `PIXEL_FORMAT_BGRA`) | 1 | the image in its packed form | — |
| Semi-planar 4:2:0 (`PIXEL_FORMAT_NV12`, `PIXEL_FORMAT_NV21`) | 2 | luma, `FORMAT_R8`, full payload width x height | plane 1: both chroma components **interleaved**, `FORMAT_RG8`, subsampled per the format |
| Fully planar 4:2:0 (`PIXEL_FORMAT_I420`, `PIXEL_FORMAT_YV12`) | 3 | luma, `FORMAT_R8`, full payload width x height | planes 1 and 2: one chroma component each, `FORMAT_R8`, subsampled per the format |

Planes are **tightly packed**. Camera buffers are frequently padded, and that
padding is removed on the way out, so each texture's dimensions are exactly the
plane's dimensions with no stride slack.

### Which chroma component is which

Chroma ordering is part of the format's identity, so derive it from the format:

- `PIXEL_FORMAT_NV12` — chroma is **U (Cb) first**: `.r` = Cb, `.g` = Cr
- `PIXEL_FORMAT_NV21` — chroma is **V (Cr) first**: `.r` = Cr, `.g` = Cb
- `PIXEL_FORMAT_I420` — plane 1 = U (Cb), plane 2 = V (Cr)
- `PIXEL_FORMAT_YV12` — plane 1 = V (Cr), plane 2 = U (Cb)

Getting this backwards swaps the colour-difference channels and produces a
plausible, wrong image rather than a visible failure.

### The format you ask for is not necessarily the format you get

If you pin `format_fourcc` in the stream profile, **a 4:2:0 pin selects the
family, not the member.** A camera provider cannot guarantee a specific member —
the device decides the memory layout at runtime — so a request for one member
may be delivered as its sibling. Packed formats, which a provider produces by
converting, *are* honoured exactly.

Compare what you asked for against what arrived, at setup:

```gdscript
var requested := int(stream.get_requested_format())   # 0 when you pinned nothing
var delivered := int(result.get_format())             # authoritative
```

The simplest correct approach is to pin nothing, let Core select, and branch on
`get_format()`.

## Colour

```gdscript
var c: Dictionary = result.get_colorimetry()
# { range, matrix, transfer, primaries, declared }
```

Values are tokens: `range` is `"limited"`, `"full"` or `"unspecified"`; `matrix`,
`transfer` and `primaries` likewise carry named values or `"unspecified"`.

**You must apply this yourself.** `to_image()` and the display view convert for
you; this surface hands over unconverted planes precisely so that you control
the conversion, which means the colour decision is yours too. At minimum, branch
on `range` — full and limited require different maths, and using the wrong one
crushes or blows the image without erroring.

`declared: false` means the source did not state its colour interpretation. That
is truthful absence, not a default: choose your fallback explicitly, and know
that you have chosen it.

**Do not calibrate against one source and ship to another.** Different providers
and different hardware legitimately declare different colour interpretations,
and each is correct for itself. Read the values at runtime; never bake them in.

## Lifetime

A result's planes belong to **that result's frame** and never change. Hold the
result and its planes stay valid and stable; a newer result yields different
texture objects carrying that frame's pixels.

This is the opposite of `get_display_view()`, which is deliberately a live
texture refreshed in place. The freeze is what lets you pair pixels with that
frame's acquisition mark — a live texture would give you a mark from one frame
and pixels from another, with nothing in the API to reveal it.

Practical consequences:

- Texture objects change each frame, so **re-set your shader parameters each
  frame**.
- Do not hold a result across an `await` and expect it to have advanced. It will
  not; fetch a new one.
- Memory is held for exactly as long as you hold results. Release them when done.

## Teardown

```gdscript
stream.stop()
stream.destroy()
```

In that order — `destroy()` is refused while the stream is started. `destroy()`
is safe to call more than once.

If you release a stream handle without calling `destroy()`, CamBANG reaps the
stream and logs a warning. Rely on that as a safety net, not as the contract.

## When something is refused

`create_stream()` returning null always prints a reason. The distinct cases are:
the runtime is not started; the endpoint is not engaged; the definition could not
be parsed (an unknown key, a wrong value type, an unrecognised token); the
provider does not accept a picture config; or Core refused it — including a pixel
format the provider does not advertise.

`stream.start()` returns an `Error`, and logs busy and provider-rejection cases
distinctly.

## Diagnostics

```gdscript
var ev: Dictionary = CamBANGServer.get_result_access_timing_evidence()
var m: Dictionary = ev["stream_compute_textures"]   # { uploads, hits, uploaded_bytes }
```

`uploads` counts planes produced; `hits` counts re-requests served from a result
you already hold. A high upload count relative to frames processed usually means
results are being fetched more than once per frame.
