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
- Current implementation status: `CaptureResult` availability is
  payload-retention-driven.
- Current implementation status: `capture_started` / `capture_completed` /
  `capture_failed` callbacks update acquisition-session counters/latency and do
  not create or gate `CaptureResult` availability.

## Release-direction targets (not implemented in this slice)

- `CaptureResult` should represent a completed image-bearing device-level
  still-capture result, not merely a retained payload observed before capture
  completion.
- Default-only capture is the one-image case of the same assembly model
  intended for future multi-image still capture.
- `CaptureResultSet` for rig-triggered capture should be curated only after
  participating device captures have assembled or failed according to explicit
  policy.
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
