# Still-capture result assembly status note

This note records current accepted internal decisions and release-direction work
for still-capture result assembly.

## Current accepted state

- `CaptureResult` is device-level and has an explicit required default image.
- Default-only still capture is the currently supported one-image case.
- `additional_images` exists only as internal storage preparation and remains
  unpopulated.
- `build_default_image_capture_result(...)` is the current default-only
  assembly seam.
- `supports_multi_image_still_sequence()` is an internal provider capability
  seam for future non-default images beyond the default image.
- Default-only capture remains on the existing materialization +
  `trigger_capture(...)` path and is intentionally not gated by
  `supports_multi_image_still_sequence()`.
- Current implementation status: device-level `CaptureResult` availability is
  assembly-success gated: retained default image + `capture_completed` are both
  required for successful retrieval.
- Current implementation status: `capture_failed` prevents successful result
  retrieval, including cases where a default image payload was retained.
- Current implementation status: `capture_completed` without retained default
  image and retained default image without terminal lifecycle do not produce
  successful retrievable `CaptureResult`.
- Current implementation status: `CaptureResultSet` retrieval filters to
  assembly-successful device candidates and then applies an explicit accept-all
  placeholder curation policy.
- Current implementation status: `capture_started` / `capture_completed` /
  `capture_failed` callbacks still update acquisition-session counters/latency
  in addition to device assembly tracking.

## Release-direction targets (not implemented in this slice)

- `CaptureResult` should represent a completed image-bearing device-level
  still-capture result, not merely a retained payload observed before capture
  completion.
- Default-only capture is the one-image case of the same assembly model
  intended for future multi-image still capture.
- `CaptureResultSet` for rig-triggered capture should be curated only after
  participating device captures have assembled or failed according to explicit
  policy.
- True rig-triggered `CaptureResultSet` curation still requires future
  admission-time cohort tracking of expected participants plus explicit
  selection/terminal policy; current accept-all placeholder curation is not
  rig-complete.
- Future work requires an internal capture assembly/group tracker joining
  lifecycle facts with retained image payloads.
- Do not implement a partial completion gate by simply checking
  `capture_completed` in the existing result store; coherent device and rig
  assembly state is required.

## Provider interpretation notes

- Current `StubProvider` and current Windows provider behaviors are
  development/transitional implementation facts, not constraints on
  release-facing result semantics.
- `SyntheticProvider` normal mode remains the reference/gold-standard provider,
  not a platform limitation simulator.
- Unsupported/restricted/negative capability cases should be tested via spec
  fixtures, `StubProvider`, or verifier-only fixtures, not normal
  `SyntheticProvider` personality modes.
