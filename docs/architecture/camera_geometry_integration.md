# Camera Geometry Integration

**Audience: integrators.** This document is written for someone building a
Godot application against CamBANG from outside this repository, who needs the
image CamBANG delivers to line up with a 3D scene — placing an overlay on a
real object, casting a ray through a pixel, or matching a virtual camera to the
physical one.

**Normative source: `camera_fact_model.md`**, with the ADC override contract in
`adc_camera_description_v2.md` §8A and the result surfaces in
`godot_boundary_contract.md`. This document defines nothing of its own; it
sequences those rules into the order a caller meets them and adds worked
arithmetic. Where it and the model disagree, the model wins and this document
is wrong.

---

## What you can reconstruct, and what you cannot

CamBANG gives you the **pinhole camera** that produced the image you are
holding: focal lengths, principal point, and the image dimensions they are
expressed in. From those four numbers and a near/far distance you can build an
exact view frustum and unproject any pixel to a ray.

CamBANG does **not** hand you a projection matrix, a `Camera3D`, or a pose in
your world. It reports what the camera was; turning that into your scene's
coordinate system is your application's business, because only you know where
the camera is standing.

---

## Step 1: get the facts

The two surfaces differ, and the difference is not cosmetic.

**A stream frame** carries one fact record for the frame:

```gdscript
var result = stream.get_result()
if result == null:
    return
var facts: Dictionary = result.get_camera_facts()
var w := int(result.get_width())
var h := int(result.get_height())
```

**A capture carries one record per image member.** A capture is not necessarily
one image — a bracket is several, from a single trigger, each exposed
differently — so the facts are attached to the member, not to the result:

```gdscript
var count := int(capture_result.get_image_count())
for i in range(count):
    var member: Dictionary = capture_result.get_image_member(i)
    if not member.has("camera_facts"):
        continue                     # this member reported none
    var facts: Dictionary = member["camera_facts"]
    # Members share the result's geometry.
    var w := int(capture_result.get_width())
    var h := int(capture_result.get_height())
```

`CamBANGCaptureResult` has no `get_camera_facts()`. Reaching for one is the
usual first mistake; the facts are one level down, on the member.

---

## Step 2: use the delivered-image calibration

Two calibrations may be present, and picking the wrong one is silently wrong
rather than loudly wrong.

| Key | Frame it describes | Use it for |
|---|---|---|
| `intrinsics_delivered` | the pixels you were handed | **this** — frustums, rays, overlays |
| `intrinsics` | whatever frame the camera measured in, usually a sensor array | relating the image back to the sensor |

Use `intrinsics_delivered`. Its `coordinate_domain` is always
`delivered_image`, and its `reference_width_px` / `reference_height_px` are the
dimensions of the image in your hands.

`intrinsics` is typically expressed in a sensor array much larger than your
stream — a 4032×3024 array behind a 640×480 preview is ordinary. Feeding those
focal lengths into a frustum for a 640×480 image overstates the focal length by
the scale factor and yields a field of view several times too narrow. The
numbers look plausible throughout; nothing errors.

### Check that it belongs to your image

```gdscript
if not facts.has("intrinsics_delivered"):
    return                          # see "When a fact is absent"
var k: Dictionary = facts["intrinsics_delivered"]
if int(k["reference_width_px"]) != w or int(k["reference_height_px"]) != h:
    return                          # describes a different geometry; do not use
```

This check is cheap and worth keeping. Reference dimensions exist precisely so
a calibration can be tested against a picture rather than assumed to match one.

---

## Step 3: the coordinate conventions

Getting these backwards produces a mirrored or upside-down alignment that looks
almost right, which is worse than looking wrong.

**Image space** — `x` increases to the **right** from the left edge, `y`
increases **downward** from the top edge, in pixels of the delivered image.

**Godot camera space** — `x` right, `y` **up**, and the camera looks down
**−Z**.

So the vertical axis flips between them, and every formula below carries that
flip explicitly.

---

## Step 4: build the frustum

```gdscript
var fx := float(k["focal_length_x_px"])
var fy := float(k["focal_length_y_px"])
var cx := float(k["principal_point_x_px"])
var cy := float(k["principal_point_y_px"])

var near := 0.05
var far  := 1000.0

var left   := -near * cx / fx
var right  :=  near * (float(w) - cx) / fx
var top    :=  near * cy / fy
var bottom := -near * (float(h) - cy) / fy

var proj := Projection.create_frustum(left, right, bottom, top, near, far)
```

`top` uses `cy` and `bottom` uses `h - cy` — that is the vertical flip, not a
transposition error.

The frustum is **asymmetric** whenever the principal point is not exactly at
the image centre, which on real hardware is essentially always. Godot's
ordinary perspective projection (`Camera3D` in `PROJECTION_PERSPECTIVE`, or
`Projection.create_perspective()`) assumes a centred principal point and cannot
express this. Either use the `Projection` above for your own maths, or put the
camera in `PROJECTION_FRUSTUM` and drive `Camera3D.set_frustum()` with an
offset — the latter matches only when your viewport's aspect ratio equals
`(right - left) / (top - bottom)`.

### Unprojecting a pixel to a ray

```gdscript
# Direction in camera space for pixel (u, v).
func pixel_to_ray(u: float, v: float, fx: float, fy: float, cx: float, cy: float) -> Vector3:
    return Vector3((u - cx) / fx, -(v - cy) / fy, -1.0).normalized()
```

### Projecting a camera-space point to a pixel

```gdscript
# p is in camera space and must be in front of the camera (p.z < 0).
func point_to_pixel(p: Vector3, fx: float, fy: float, cx: float, cy: float) -> Vector2:
    var d := -p.z
    return Vector2(cx + fx * p.x / d, cy - fy * p.y / d)
```

Transform world points into camera space with your camera's inverse transform
before calling this; CamBANG has no opinion about where your camera is.

---

## Why the off-axis frustum is worth the trouble

Measured from a Galaxy S20+ main camera delivering 640×480:

```text
fx = 483.139  fy = 482.821
cx = 318.031  cy = 235.801       image centre would be (320.0, 240.0)
```

The principal point sits about 2 px left and 4 px above the centre. The total
field of view barely notices — 52.859° vertical against 52.862° for a centred
model. But the frustum is not centred: 26.030° above the axis and 26.829°
below. A symmetric frustum of the correct total FOV therefore misplaces
everything vertically by roughly 0.4°, which is about 7 cm at 10 m. Overlays
drift consistently in one direction, and the error does not shrink as you tune
the field of view, because the field of view was never the problem.

---

## Distortion: when a frustum is not enough

A frustum is a **rectilinear** model. If the delivered pixels are distorted,
straight lines in the world are not straight in the image, and no projection
matrix will fix that.

```gdscript
if facts.has("distortion"):
    var d: Dictionary = facts["distortion"]
    match str(d["model"]):
        "none":
            pass                       # rectilinear; the frustum is complete
        "brown_conrady_5":
            if str(d["image_state"]) == "distorted":
                pass                   # you must undistort before projecting
```

`image_state` is the deciding field, not the presence of coefficients:
`rectified` means the pixels have already had the distortion removed and the
coefficients are describing what was corrected; `distorted` means they have
not. Check `reference_width_px` / `reference_height_px` and `coordinate_domain`
on the distortion record too — a distortion model expressed in a sensor array
must be scaled to your image before it means anything.

CamBANG carries distortion descriptively and does not undistort for you.

### Skew

If `skew_px` is present and non-zero, the camera model is not axis-aligned and
an ordinary frustum is an approximation. Most cameras report no skew at all, in
which case the key is absent and there is nothing to think about.

---

## When a fact is absent

**Every key is optional, and absence is truthful.** It means no source supplied
that value — never that the value is zero, and never that it is a default.

There is no fallback that is safe to invent. A guessed field of view produces
an overlay that is confidently wrong, which is harder to diagnose than one that
never appears. If `intrinsics_delivered` is missing, the honest responses are
to disable the geometry feature, or to ask the user to supply a calibration —
which is what the ADC override exists for (`adc_camera_description_v2.md` §8A).

Some cameras on a device report calibration while others on the same device do
not. Do not conclude from one endpoint that the provider publishes nothing.

## Where a value came from

Every fact is a Dictionary carrying `origin`, never a bare number:

```gdscript
var origin := str(k["origin"])
```

| `origin` | Meaning for geometry |
|---|---|
| `native_reported` | the camera asserted this exact value |
| `core_derived` | CamBANG computed it from values the camera did assert |
| `user_supplied` | it came from an ingested camera description, overriding the camera |
| `virtual_camera_authored` | a synthetic camera's authored truth |

`origin` describes **who made this assertion**, not who supplied the inputs. So
on a platform-backed provider you should expect:

- `intrinsics` — `native_reported`. This is the camera's own calibration.
- `intrinsics_delivered` — `core_derived`, on every provider CamBANG currently
  ships. No Camera2 or WinRT camera states a calibration in the delivered
  image's own frame; CamBANG computes it. This holds even when the arithmetic
  turns out to be an identity, because the claim that these numbers apply to
  the delivered image is still CamBANG's. A future provider on a platform that
  *does* report a delivered-frame calibration natively could publish it as
  `native_reported`.
- `delivered_image_region` — either. `native_reported` means the camera named
  that exact rectangle. `core_derived` means CamBANG did: because the platform
  reports no region at all (WinRT infers the whole reference frame), or because
  the camera's readout region had to be narrowed to your output's aspect ratio,
  producing a rectangle the camera never named.

`core_derived` is not second-class — it is exact arithmetic on reported values,
not an estimate. But if your application needs to distinguish a measurement
from a computation, `origin` is how, and the distinction is real.

---

## Advanced: relating the image back to the sensor

`delivered_image_region` states which rectangle of the sensor-domain reference
frame your image covers, in the domain it names:

```gdscript
var g: Dictionary = facts["delivered_image_region"]
# g.left, g.top, g.width, g.height, g.coordinate_domain, g.origin
```

Most applications never need this; the delivered calibration already accounts
for it. It matters when you are relating the image to something else expressed
in the sensor frame, or comparing two streams of different geometry from one
sensor — two output aspects take different crops of the same readout, so they
genuinely see different parts of the scene.

The relationship it encodes is exactly:

```text
scale_x = delivered_width  / region.width
scale_y = delivered_height / region.height

fx_delivered = fx_native * scale_x
cx_delivered = (cx_native - region.left) * scale_x
```

with the vertical the same. When the region is the whole reference frame and
the delivered size equals it, the two calibrations are identical — moving
between the surfaces never silently rescales anything.

---

## Worked example

Measured on a Galaxy S20+ (`android_camera2`), preview at 640×480:

```text
intrinsics              (origin native_reported)
  f    = (3043.777832, 3041.773438)
  c    = (2003.595215, 1485.548584)
  ref  = 4032 x 3024
  domain = android_sensor_pre_correction_active_array

delivered_image_region  (origin native_reported)
  rect   = (0, 0, 4032, 3024)
  domain = android_sensor_pre_correction_active_array

intrinsics_delivered    (origin core_derived)
  f    = (483.139338, 482.821181)
  c    = (318.030986, 235.801363)
  ref  = 640 x 480
  domain = delivered_image
```

The scale is `640 / 4032 = 480 / 3024 = 0.158730`, and
`3043.777832 x 0.158730 = 483.139`. The region starts at the origin here, so
the principal point is only scaled.

Frustum at `near = 0.1`:

```text
left   = -0.065826      right = 0.066641
bottom = -0.050577      top   = 0.048838
```

### A camera whose region is offset

Measured on a Meta Quest 3 (`android_camera2`), preview at 640×480. This camera
has a **square** 1280×1280 sensor array, and a 4:3 output cannot cover it, so
the readout region is inset vertically:

```text
intrinsics              f = (867.137573, 867.137573)
                        c = (643.795837, 636.774231)
                        ref = 1280 x 1280

delivered_image_region  rect = (0, 160, 1280, 960)

intrinsics_delivered    f = (433.568787, 433.568787)
                        c = (321.897919, 238.387115)
                        ref = 640 x 480
```

The scale is `0.5` on both axes, and the vertical shows why the region matters:

```text
cy_delivered = (636.774231 - 160) * 0.5 = 238.387
```

Drop the `- 160` — that is, scale the sensor calibration without accounting for
where the image was taken from — and you get `318.387`: **80 pixels adrift on a
480-pixel-tall image**, a sixth of the frame. This is the failure mode that
makes `intrinsics` unusable directly, and it is invisible on a camera whose
region happens to start at the origin.

Frustum at `near = 0.1`: `left = -0.074244`, `right = 0.073368`,
`bottom = -0.055727`, `top = 0.054983` — 72.86° horizontal, 57.93° vertical.

---

## What this surface does not do

- It does not undistort, rectify, or reproject pixels.
- It does not rotate the image or the calibration to match device orientation.
  Check `realized_image_transform` for what a provider already applied to the
  pixels; if your application rotates an image for display, the calibration you
  built the frustum from describes the image **before** your rotation.
- It does not place the camera in your world. Pose facts, where a camera
  reports them, are relative to the device, not to your scene.
