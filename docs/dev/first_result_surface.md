# First Result Surface

Status: Dev note (implementation-shaping)  
Purpose: Define the release-facing result-object surface while preserving the broader multi-representation release architecture.

This note records the result-object surface direction. It does **not** redefine the broader payload/result architecture established in:

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

This note keeps the result-object surface bounded to avoid:

- drifting back toward mailbox-shaped public semantics
- overcommitting to CPU-only architecture
- prematurely taking on every future payload kind
- leaving the first result-object seam underspecified

---

## 2. Scope

### 2.1 Supported payload representation baseline

The currently supported retained payload representation is:

- `CPU_PACKED`

This is a supported baseline, **not** the canonical or ultimate release representation.

### 2.2 Architecturally in-scope payload kinds beyond the currently supported baseline

The following remain first-class architectural targets:

- `CPU_PLANAR`
- `GPU_SURFACE`
- `ENCODED_IMAGE`
- `RAW_IMAGE`

`CPU_PLANAR`, `ENCODED_IMAGE`, and `RAW_IMAGE` remain first-class architectural targets beyond the currently supported baseline.

`GPU_SURFACE` is now concretely exercised for synthetic stream results as a retained-primary path. This does not redefine the release architecture around any single payload representation.

---

## 3. Public result-object goals

CamBANG result objects should remain:

- result-oriented rather than mailbox-oriented
- capability/cost-aware
- rich enough to carry image-associated fact groups
- explicit about materialization
- compatible with future non-CPU-only representations

This surface does not require every transport or rendering path to be available at once.

---

## 4. Canonical result nouns preserved

The public/runtime-visible result nouns remain:

- **Stream Result**
- **Capture Result**
- **Capture Result Set**

These nouns must be preserved, and mailbox terminology must not be reintroduced into the public API.

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

## 6. `StreamResult` surface

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
- `to_image()` is the explicit path for **materialization onto CPU-backed storage**.

For `StreamResult`, `get_display_view()` is a **display-oriented live view** of
the current retained stream state.

At the user-facing contract level, this is the consistent live-view path across
supported stream backing kinds (including CPU-backed and GPU-backed paths),
even where internal realization differs by backing/provider.

Where supported, this may be backed by **live GPU-backed stream display state**
owned by the stream and updated in place while the stream flows.

For synthetic GPU-backed repeating streams, this live display state is retained
for the active flowing lifetime of the stream (rather than recreated per
delivered frame) and is released deterministically on stream stop/destroy and
provider teardown.

This display-view path is intentionally **buffer-like**. It is not a promise of
frozen historical image identity for previously obtained stream-result objects.

`to_image()` performs explicit materialization onto CPU-backed storage from the
current retained stream state at the time of the call.

`get_display_view()` must not silently behave like “always convert to CPU-backed
storage.”

### 6.6.1 Freshness-policy clarification for synthetic stream live GPU backing

This note clarifies stream display-view freshness policy and does not declare a
new default runtime behavior by itself.

- Stream display-view freshness is tied to display-oriented access.
- Polling latest `StreamResult` alone does not imply display demand.
- For synthetic stream live GPU backing, no-display operation may legitimately
  avoid per-frame live GPU-backing updates. In current implementation this
  publishes those no-demand frames as CPU-primary with current CPU bytes.
- When display demand is active and GPU update succeeds, frames may publish as
  GPU-primary (`GPU_SURFACE`).
- Payload kind may therefore vary across successive stream frames under
  display-demanded policy, while remaining truthful per retained frame.
- `get_display_view()` is the demand-establishing path for stream live-view
  freshness.

This clarification is intentionally scoped to synthetic stream live GPU backing
and stream display-view freshness. It is not a global “GPU updates are optional”
statement, and it is not an AcquisitionSession/lifecycle change.

A `display_view` is a live view over stream-owned live backing, not a detached materialized
image artifact. Consumers that bind it into UI/display objects are responsible
for dropping those bindings before stopping/destroying the owning runtime or
stream state.

For synthetic `GPU_SURFACE`, `get_display_view()` has a direct GPU display path.
That direct display path must not be reframed as materialization onto CPU-backed
storage.

### 6.7 Explicit non-goals for `StreamResult`

Not included:

- encoded-byte access
- filesystem save operations
- stream history / sequence access
- rectified/undistorted variants
- backend-native public handles
- arbitrary pixel-copy/export APIs

---

## 7. `CaptureResult` surface

`CaptureResult` is the device-level still-capture result and is image-member based.

The canonical minimum valid image-member bundle is one image member:

- member `0` with role `DEFAULT_METERED`

Bracketed captures use the same model, adding members:

- member `1..N` with role `ADDITIONAL_BRACKET`

A one-image capture is therefore the minimum valid still image bundle (ordered
image-member bundle for one still event), not a separate legacy/default-only
path.

### 7.1 Core properties

Initial direct scalar properties describe the `CaptureResult` using member-0 convenience semantics:

- `width`
- `height`
- `format`
- `payload_kind`
- `capture_timestamp`
- `device_instance_id`
- `capture_id`

For multi-member captures, image-facing scalar values that can vary between members (for example `capture_timestamp`) refer to member `0` unless an image-member access path explicitly selects another member. Structural properties such as `width`, `height`, `format`, and `payload_kind` are result-level homogeneous truth.

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

`get_image_properties()`, `get_location_attributes()`, and `get_optical_calibration()` expose result-level shared truth. `get_capture_attributes()` describes member `0` where capture attributes may vary between image members.


### 7.3.x Image-member model and Godot wrapper access

`FrameView` capture metadata now carries image-member routing facts (`routing`,
`image_member_index`, `applied_exposure_compensation_milli_ev`,
`has_realized_exposure_compensation_milli_ev`,
`realized_exposure_compensation_milli_ev`) and retained still data is
modeled as a default member plus optional additional members in
`CoreCaptureResultData` (`default_image`, `additional_images`).

Godot-facing `CamBANGCaptureResult` now exposes indexed image-member access:

- `IMAGE_ROLE_DEFAULT_METERED`
- `IMAGE_ROLE_ADDITIONAL_BRACKET`
- `get_image_count()`
- `has_additional_images()`
- `get_image_member(index)`
- `can_to_image_member(index)`
- `to_image_member(index)`

`get_image_member(index)` returns Dictionary metadata for the selected member, including `applied_exposure_compensation_milli_ev` and provider-realized exposure truth (`has_realized_exposure_compensation_milli_ev`, `realized_exposure_compensation_milli_ev`).
Invalid/out-of-range access returns an empty Dictionary.

`to_image_member(index)` explicitly materializes the selected member where
supported.

Existing scalar/default `CaptureResult` methods (`to_image()`,
`can_to_image()`, and related default-image descriptive accessors) remain member-0
conveniences.

Still-capture profile authoring at the Godot boundary uses the Dictionary key
`still_image_bundle` to describe this ordered member bundle and intentionally
avoids `image_sequence` wording to prevent time-sequence ambiguity.

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

Accordingly, this surface does **not** include:

- `save_to_file(...)`
- path handling
- file-permission or platform-storage concerns

If the user wants to persist a result to disk, the intended portable boundary artifact is:

- `Image` returned by `to_image()`

or, where supported and appropriate:

- encoded bytes returned by `get_encoded_bytes()`

Actual file saving belongs to Godot/app code.

### 7.7 Capture-result guardrail

Still-capture public result semantics remain distinct from repeating-stream
display-view semantics.

A `CaptureResult` is the device-level still-capture result at the public result seam and should not inherit the stream-side notion of a live GPU-backed display buffer.

The existence of a live GPU-backed display path for repeating streams must not be
used to justify retained or exposed per-capture GPU artifacts in the public
capture-result model.

### 7.8 Explicit non-goals for `CaptureResult`

Not included:

- filesystem save APIs
- RAW processing/export APIs
- rectified/undistorted variants
- alternate export negotiation
- backend-native public handles

---

## 8. `CaptureResultSet` surface

`CaptureResultSet` is the rig/Core-curated grouping of selected device `CaptureResult` objects for a rig-triggered synchronised capture.

CaptureResultSet curation remains distinct from the definition of `CaptureResult` and is not the container for bracket image members inside a single device capture result.

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

## 9. Fact-group object shapes

These grouped objects should be read-only and typed.

They should remain intentionally modest.

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

## 10. Provenance surface

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

Conceptually, the surface should support patterns like:

- `result.get_capture_attributes().exposure_time_ns`
- `result.get_capture_attributes().get_provenance().exposure_time_ns`

without forcing provenance verbosity into the primary value-access path.

---

## 11. Explicit exclusions

This surface does **not** imply or require:

- full GPU-native presentation architecture
- full multi-plane YUV result support
- RAW-domain result processing
- rectified materialization
- recording/broadcast/sequence APIs
- backend-native handle exposure
- filesystem persistence inside CamBANG

---

## 12. Guardrail against accidental narrowing

The existence of a working `CPU_PACKED` path must **not** be used to redefine the release architecture as CPU-only.

Implementation and follow-up design must preserve:

- the broader payload taxonomy
- the distinction between display-view access and CPU image materialization
- the ability to add `GPU_SURFACE`, `CPU_PLANAR`, `ENCODED_IMAGE`, and `RAW_IMAGE` without redefining public result nouns

If code or docs begin to treat `CPU_PACKED` as the canonical representation rather than one supported payload path, that is architectural drift and should be corrected.

## 12.x Non-CPU exemplar discipline (now exercised)

The non-CPU exemplar now exists in synthetic stream paths and still must not
teach the wrong architecture lesson about access semantics.

In particular:

- a non-CPU primary payload such as `GPU_SURFACE` remains compatible with an
  available `to_image()` fallback path
- the current synthetic `GPU_SURFACE` exemplar may legitimately use one primary
  backing plus an
  optional auxiliary CPU backing
- direct display-view access can use stream-owned live backing (including a
  GPU-native path where supported) while `to_image()` remains the explicit API
  for materialization onto CPU-backed storage and chooses the least expensive
  supported CPU route from current retained state

This note does not freeze exact retention heuristics or auxiliary-backing policy.
It exists to prevent future implementation from teaching the wrong lesson that
a non-CPU primary payload removes the need for an available CPU-facing fallback.

---
