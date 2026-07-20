# Current tranche

## Remove dead flattened result fact groups; tighten image_properties

Maintainer-approved (2026-07-20, in-session direction, expanded from the
original optical-calibration/location scope to also cover capture_attributes
and an image_properties structural tightening).

### Scope

1. **image_properties structural tightening.** `CoreImageFactBundle.
   image_properties.{width,height,format}` is now derived from the
   already-assigned top-level scalar fields (`image_width`/`image_height`/
   `image_format_fourcc`) at both `retain_frame` call sites in
   `src/core/core_result_store.cpp`, instead of being independently
   re-derived from `frame.*`. This makes the flattened copy structurally
   incapable of drifting from `get_width()`/`get_height()`/`get_format()`;
   behavior is unchanged (same values, same call sites).

2. **Remove the flattened `optical_calibration` and `location_attributes`
   result fact groups** (writer-less; superseded by the resolved per-member
   `camera_facts` surface and by `get_geolocation()` respectively) —
   completed in this tranche's first pass.

3. **Remove the flattened `capture_attributes` result fact group.**
   Confirmed writer-less (`facts.has_capture_attributes` / `default_image.
   has_capture_attributes` are never set true anywhere in Core); no provider
   or Core source currently supplies exposure/aperture/focal-length/ISO
   facts at all, flat or per-member. Removing it also removes
   `focus_distance_m`, closing the only real duplication risk in this
   group: `camera_facts.focus_state` (a live, provider-populated variant)
   is the sole remaining focus surface.

   Removed: `has_/get_capture_attributes()`, `get_capture_attributes_
   provenance()` (both `CamBANGCaptureResult` and `CamBANGStreamResult`) and
   bindings; `ResultCaptureAttributesFacts`/`ResultCaptureAttributesProvenance`
   (`src/core/result_fact_types.h`); their fields on `CoreImageFactBundle`
   and `CoreCaptureResultData::ImageMemberData`
   (`src/core/core_result_store.h`); the dead reset in `core_result_store.cpp`;
   the `to_dict` converters. Scene 70's four `capture_attributes` assertions
   became absence assertions alongside the earlier eight for the other two
   groups (12 absence assertions total).

### Explicitly out of scope / deferred

- **WinRT-native bracketed capture** (real per-member exposure via
  `ExposureCompensationControl`, or `VariablePhotoSequenceCapture`) —
  deferred to its own tranche. Rationale: this removal tranche is pure
  deletion/tightening with no new capability surface and is verified by
  proving absence; bracket capture is new provider capability work that
  changes `supports_multi_image_still_sequence()` (a contract-facing
  capability advertisement Core's admission path depends on), needs new
  hardware-backed verification (no existing WinRT bracket proof), and needs
  its own capture-worker timing analysis (sequential per-member exposure
  changes must still fit Core's 30s capture-admission watchdog). Mixing that
  into a deletion tranche would blur this tranche's acceptance criteria and
  its low-risk character. See `[[winrt-bracketing-expectation]]` memory.
- `image_properties` itself is NOT removed (has a real writer; the
  provenance it carries is not available from the direct scalar accessors).

### Acceptance / validation

- Full MinGW build (maintainer tools + GDE) and MSVC Windows GDE build stay
  clean; no remaining references to any removed identifier in src/docs
  (grep-verified).
- Native verifiers: core_spine_smoke, provider_compliance_verify,
  restart_boundary_verify, verify_case_runner --run-all,
  core_thread_liveness_watchdog_verify.
- Godot scenes 65, 66, 70 (12 absence assertions), 73, 870 verdict ok on both
  the MinGW (synthetic) and MSVC (windows_winrt) GDE artifacts.
- WinRT hardware runtime validator stays green (image_properties path is
  exercised by real capture/stream results).
