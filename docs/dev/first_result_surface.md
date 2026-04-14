# First Result Surface (Implementation Slice 1)

Status: Dev note (implementation-shaping)  
Purpose: Freeze the first release-facing result-object surface for the initial implementation slice, while preserving the broader multi-representation release architecture.

This note narrows the next implementation step. It does **not** redefine the broader payload/result architecture established in:

- `docs/dev/pixel_result_architecture_direction.md`
- `docs/architecture/pixel_payload_and_result_contract.md`
- `docs/naming.md`
- `docs/architecture/frame_sinks.md`

---

## 1. Why this note exists

CamBANG now has a documented release-facing image/result architecture that is:

- multi-representation
- result-oriented
- capability/cost-aware
- distinct from the hot runtime snapshot path

However, implementation should begin with a deliberately small first slice.

This note freezes that first slice so implementation can proceed without:

- drifting back toward mailbox-shaped public semantics
- overcommitting to CPU-only architecture
- prematurely taking on every future payload kind
- leaving the first result-object seam underspecified

---

## 2. Scope of slice 1

### 2.1 First fully-supported payload representation

The first fully-supported retained payload representation is:

- `CPU_PACKED`

This is an implementation slice, **not** the canonical or ultimate release representation.

### 2.2 Architecturally in-scope payload kinds beyond the first fully-supported slice

The following remain first-class architectural targets:

- `CPU_PLANAR`
- `GPU_SURFACE`
- `ENCODED_IMAGE`
- `RAW_IMAGE`

`CPU_PLANAR`, `ENCODED_IMAGE`, and `RAW_IMAGE` are still beyond the first
fully-supported slice.

`GPU_SURFACE` is now concretely exercised for synthetic stream results as a
retained-primary path; this note still preserves the historical “slice 1 was
`CPU_PACKED` first” implementation framing.

---

## 3. Public result-object goals for slice 1

Slice 1 should prove that CamBANG can expose release-facing result objects that are:

- result-oriented rather than mailbox-oriented
- capability/cost-aware
- rich enough to carry image-associated fact groups
- explicit about materialization
- compatible with future non-CPU-only representations

Slice 1 should **not** attempt to prove every future transport or rendering path.

---

## 4. Canonical result nouns preserved

The public/runtime-visible result nouns remain:

- **Stream Result**
- **Capture Result**
- **Capture Result Set**

Slice 1 must preserve these nouns and must not reintroduce mailbox terminology into the public API.

---

## 5. Shared result-surface principles

Both `StreamResult` and `CaptureResult` should expose:

### 5.1 Cheap descriptive properties

These are intended to be directly readable and low-friction.

Initial set:

- `width`
- `height`
- `format`
- `payload_kind`
- `capture_timestamp`

Additional lineage/context fields may be type-specific.

### 5.2 Grouped fact surfaces

Rich image-associated facts remain grouped rather than exposed through a generic metadata bag.

Initial grouped fact surfaces:

- `ImageProperties`
- `CaptureAttributes`
- `LocationAttributes`
- `OpticalCalibration`

### 5.3 Capability/cost inspection

Result access paths must be inspectable before use.

Initial capability vocabulary:

- `READY`
- `CHEAP`
- `EXPENSIVE`
- `UNSUPPORTED`

### 5.4 Explicit operations

Expensive work must be explicit.

In particular:

- display-oriented access must remain distinct from CPU image materialization
- no method with a cheap-sounding name may secretly force expensive readback or conversion

---

## 6. `StreamResult` surface (slice 1)

`StreamResult` remains latest-result-oriented.

It represents the latest retained repeating-stream image-bearing output for a stream.

### 6.1 Core properties

Initial direct properties:

- `width`
- `height`
- `format`
- `payload_kind`
- `capture_timestamp`
- `stream_id`
- `device_instance_id`
- `intent`

### 6.2 Fact-group presence helpers

Initial helper surface:

- `has_image_properties()`
- `has_capture_attributes()`
- `has_location_attributes()`
- `has_optical_calibration()`

### 6.3 Fact-group accessors

Initial grouped accessors:

- `get_image_properties()`
- `get_capture_attributes()`
- `get_location_attributes()`
- `get_optical_calibration()`

### 6.4 Capability inspection

Initial stream capability checks:

- `can_get_display_view()`
- `can_to_image()`

Each returns the result capability/cost classification.

### 6.5 Explicit operations

Initial stream operations:

- `get_display_view()`
- `to_image()`

### 6.6 Intent of these operations

- `get_display_view()` is the preferred display-oriented access path.
- `to_image()` is the explicit CPU materialization path.

`get_display_view()` must not silently behave like “always convert to CPU image.”
For retained synthetic `GPU_SURFACE`, `get_display_view()` now has a direct GPU
display-view path.

### 6.7 Explicit non-goals for `StreamResult` in slice 1

Not included in slice 1:

- encoded-byte access
- filesystem save operations
- stream history / sequence access
- rectified/undistorted variants
- backend-native public handles
- arbitrary pixel-copy/export APIs

---

## 7. `CaptureResult` surface (slice 1)

`CaptureResult` is the discrete image-bearing result of a device still capture.

### 7.1 Core properties

Initial direct properties:

- `width`
- `height`
- `format`
- `payload_kind`
- `capture_timestamp`
- `device_instance_id`
- `capture_id`

### 7.2 Fact-group presence helpers

Initial helper surface:

- `has_image_properties()`
- `has_capture_attributes()`
- `has_location_attributes()`
- `has_optical_calibration()`

### 7.3 Fact-group accessors

Initial grouped accessors:

- `get_image_properties()`
- `get_capture_attributes()`
- `get_location_attributes()`
- `get_optical_calibration()`

### 7.4 Capability inspection

Initial capture capability checks:

- `can_get_display_view()`
- `can_to_image()`
- `can_get_encoded_bytes()`

Each returns the result capability/cost classification.

### 7.5 Explicit operations

Initial capture operations:

- `get_display_view()`
- `to_image()`
- `get_encoded_bytes()`

### 7.6 Boundary rule

CamBANG does **not** own filesystem persistence.

Accordingly, slice 1 does **not** include:

- `save_to_file(...)`
- path handling
- file-permission or platform-storage concerns

If the user wants to persist a result to disk, the intended portable boundary artifact is:

- `Image` returned by `to_image()`

or, where supported and appropriate:

- encoded bytes returned by `get_encoded_bytes()`

Actual file saving belongs to Godot/app code.

### 7.7 Explicit non-goals for `CaptureResult` in slice 1

Not included in slice 1:

- filesystem save APIs
- RAW processing/export APIs
- rectified/undistorted variants
- alternate export negotiation
- backend-native public handles

---

## 8. `CaptureResultSet` surface (slice 1)

`CaptureResultSet` is a grouped container for the subset of device-associated capture results realized by a rig-triggered capture.

### 8.1 Initial surface

- `capture_id`
- `size()`
- `is_empty()`
- `get_results()`
- `get_result_for_device(device_instance_id)`

### 8.2 Intent

`CaptureResultSet` is a grouping/container surface.

It is not itself an image-bearing result object.

---

## 9. Fact-group object shapes (slice 1)

These grouped objects should be read-only and typed.

They should remain intentionally modest in slice 1.

## 9.1 `ImageProperties`

Initial fields:

- `width`
- `height`
- `format`
- `orientation`
- `bit_depth`

## 9.2 `CaptureAttributes`

Initial fields:

- `exposure_time_ns`
- `aperture_f_number`
- `focal_length_mm`
- `focus_distance_m`
- `sensor_sensitivity_iso_equivalent`

## 9.3 `LocationAttributes`

Initial fields:

- `latitude`
- `longitude`
- `altitude_m`

## 9.4 `OpticalCalibration`

Initial fields:

- `principal_point_x`
- `principal_point_y`
- `focal_length_x`
- `focal_length_y`
- `distortion_model`
- `distortion_coefficients`

---

## 10. Provenance surface (slice 1)

Provenance remains per fact, not per result.

To avoid cluttering the default happy path, provenance is exposed as a grouped companion surface.

### 10.1 Provenance access

Each fact-group object that supports provenance exposes:

- `get_provenance()`

### 10.2 Provenance vocabulary

Initial provenance values:

- `HARDWARE_REPORTED`
- `PROVIDER_DERIVED`
- `RUNTIME_INJECTED`
- `USER_DEFAULT`
- `USER_OVERRIDE`
- `UNKNOWN`

### 10.3 Example shape intent

Conceptually, slice 1 should support patterns like:

- `result.get_capture_attributes().exposure_time_ns`
- `result.get_capture_attributes().get_provenance().exposure_time_ns`

without forcing provenance verbosity into the primary value-access path.

---

## 11. Explicit exclusions from slice 1

Slice 1 does **not** imply or require:

- full GPU-native presentation architecture
- full multi-plane YUV result support
- RAW-domain result processing
- rectified materialization
- recording/broadcast/sequence APIs
- backend-native handle exposure
- filesystem persistence inside CamBANG

---

## 12. Guardrail against accidental narrowing

The existence of a working `CPU_PACKED` slice must **not** be used to redefine the release architecture as CPU-only.

Implementation and follow-up design must preserve:

- the broader payload taxonomy
- the distinction between display-view access and CPU image materialization
- the ability to add `GPU_SURFACE`, `CPU_PLANAR`, `ENCODED_IMAGE`, and `RAW_IMAGE` without redefining public result nouns

If future code or docs begin to treat `CPU_PACKED` as the canonical representation rather than the first fully-supported slice, that is architectural drift and should be corrected.

## 12.x Non-CPU exemplar discipline (now exercised)

The non-CPU exemplar now exists in synthetic stream paths and still must not
teach the wrong architecture lesson about access semantics.

In particular:

- a non-CPU primary payload such as `GPU_SURFACE` remains compatible with an
  available `to_image()` fallback path
- the current synthetic `GPU_SURFACE` exemplar may legitimately use one primary
  backing plus an
  optional auxiliary CPU backing
- direct display-view access can be GPU-native while `to_image()` remains the
  explicit CPU materialization API and chooses the least expensive supported CPU
  route from current retained state

This note does not freeze exact retention heuristics or auxiliary-backing policy.
It exists to prevent future implementation from teaching the wrong lesson that
a non-CPU primary payload removes the need for an available CPU-facing fallback.

---

## 13. Immediate implementation consequence

Implementation should now focus on:

1. defining the result-object scaffolding
2. defining grouped fact objects and grouped provenance access
3. adding internal retained-result ownership paths for stream and capture
4. wiring the first fully-supported `CPU_PACKED` result path
5. preserving deterministic ownership/release and Core accounting

This is the intended bridge from architecture/design into implementation.

---
