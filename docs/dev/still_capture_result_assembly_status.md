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
- Current implementation status: for `capture_id` values with no cohort,
  `CaptureResultSet` retrieval keeps current non-rig behavior: assembly-success
  filtering over result-store candidates, then explicit accept-all placeholder
  curation.
- Current implementation status: for cohort-backed `capture_id` values,
  `CaptureResultSet` retrieval is cohort-aware:
  - cohort `FAILED` => empty result set;
  - cohort `OPEN` with any expected participant incomplete/missing => empty
    result set;
  - cohort `OPEN` with all expected participants assembly-successful and
    retained => exactly expected participant results in cohort participant
    order, then explicit accept-all placeholder curation.
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
- Future “cohort” terminology is internal maintainer/Core terminology only:
  one admitted rig-triggered capture group = one `capture_id`, one `rig_id`,
  and one fixed expected set of participant `device_instance_id` values.
- A future cohort should not own image payloads and should not duplicate
  per-device `DeviceCaptureAssembly` logic.
- Current implementation status: internal rig preflight/admission/submission
  helpers and cohort-aware `CaptureResultSet` curation support now exist, but
  public rig-triggered capture API/surface remains future work.
- Current implementation status: an internal end-to-end rig orchestration
  helper now exists and composes:
  preflight → caller-supplied `capture_id` validation → cohort admission →
  participant submission.
- Current implementation status: orchestration intentionally requires
  caller-supplied `capture_id`; no second `capture_id` allocator was added in
  this layer.
- Current implementation status: CamBANGServer now has a private server-internal
  rig-trigger helper that allocates `capture_id` from its existing
  `next_capture_id_` monotonic allocator and invokes runtime orchestration with
  caller-supplied `rig_id` + allocated `capture_id` (no second allocator in
  runtime).
- Current accept-all placeholder curation is still not final rig
  selection/terminal policy.
- Release-direction admission model: resolve rig membership to participant
  `device_instance_id` values at admission time (with useful hardware-id
  traceability), then preflight/materialize all participant `CaptureRequest`
  values before provider submission.
- Preferred initial admission policy is strict all-or-nothing: if any
  participant cannot be admitted, deny the rig trigger before execution and do
  not create an admitted cohort.
- Once all participants pass admission, create the cohort before submitting
  provider requests; later curation should observe/reference per-device
  assembly terminal states and then apply explicit `CaptureResultSet` policy.
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
