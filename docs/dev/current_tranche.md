# Current tranche

## Restore the retired capture-attributes quantities

Maintainer-activated (2026-07-21).

### Scope

Retiring the flattened `CaptureAttributes` group (commit 8ded61a) correctly
removed a writer-less duplicate, but the five quantities it covered were never
carried across into the camera-fact model: `exposure_time_ns`,
`aperture_f_number`, `focal_length_mm`, `focus_distance_m`, and
`sensor_sensitivity_iso_equivalent`. Only focus distance had a home
(`FocusAtDistance` inside `FocusState`), and even that lacked a static tier.
This tranche fixes the model; it is independent of any provider.

Governing rule, now stated in `camera_fact_model.md` §12.2.1 and §13: a fact is
externally overridable when an external describer can meaningfully assert it,
*not* when it happens to be static. All five are device-constant on some
hardware (fixed-focus lens, fixed iris, prime lens, locked exposure) and
per-capture on other hardware, and hardware that fixes one commonly exposes no
API to read it — exactly the case ADC ingestion exists to serve.

Each therefore gains both a `CameraStaticFacts` tier and a
`ProviderCaptureImageFacts` tier, resolving `external > provider-per-image >
provider-static`, the same chain as intrinsics/distortion/pose. `focus_state`'s
previous provider-only resolution was the anomaly and is corrected here.
`realized_image_transform` stays provider-only (it describes what the provider
did to the delivered pixels) and `acquisition_timing` stays frame-borne.

`focal_length_mm` is lens metadata and is **not** covered by
`Intrinsics::focal_length_x_px()`. They are different quantities related only
through sensor pixel pitch, which this model does not carry; a camera may
report either without the other.

The ADC schema is **modified, not re-versioned**: it stays at v2, whose
additive-tolerance rules (§2, §13) already cover new optional members.

Also in scope, as a small gap found while auditing the provider against the
reference: `WinrtCameraProvider` posts `camera_nature`, which Synthetic posts
and it did not.

### Explicitly out of scope

- Populating these five from any provider. No Windows-reachable API supplies
  realized values for them on the available hardware; external
  camera-description ingestion is the sole source there. Findings behind that
  conclusion belong in the commit message, not this file.
- Stream results carrying resolved camera facts. Streams deliberately carry
  only facts that ride free on the delivered frame; see
  `architecture/godot_boundary_contract.md`.

### Acceptance / validation

- MSVC and MinGW builds clean.
- `out/provider_compliance_verify.exe`, `out/core_spine_smoke.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe` all pass unmodified.
- Scene 569 (`569_capture_result_camera_facts_verify.gd`) still passes, and
  ADC round-trip coverage exercises at least one of the five new records
  overriding a provider-supplied value.
- Scenes 65/66/70/73/870 on both the MinGW (synthetic-only) and MSVC
  (windows_winrt) GDE artifacts.
- `out/windows_winrt_runtime_validate.exe` passes against real hardware.

**Not yet run.** Only the Windows runtime validator has been executed since
these changes; the native verifiers and Godot scenes are untouched.

### Known defects, not yet addressed

`open_device()` initialises `MediaCapture` with
`MediaCaptureSharingMode::ExclusiveControl` unconditionally, including for
plain streaming where no control is ever written. Exclusive control was
observed to freeze the camera's own autofocus for exactly as long as the
device was held, with autofocus resuming the moment it was released. If that
holds generally, captures are being taken with frozen focus and exposure —
an image-quality defect, not a fact-model question, and the more consequential
of the two here.

**Switching the sharing mode is not the fix — this was tried and reverted.**
Opening `SharedReadOnly` and escalating through
`VideoDeviceController.TryAcquireExclusiveControl` only on control writes
fails outright: `start_stream` returns an error and no frames arrive at all.
The cause is `MediaFrameSource::SetFormatAsync`, which is how Core's requested
geometry is honoured and which a read-only open cannot perform. The contract
forbids the obvious workaround — a provider must never invent or accept
whatever geometry the device happens to be in — so exclusive control is a
requirement of honouring the requested configuration, not a convenience.

The tension is therefore real and unresolved: deterministic geometry requires
exclusive control, and exclusive control appears to freeze the camera's own
auto algorithms. Directions worth evaluating, none yet tested:

- Escalate only when the requested geometry does not already match the
  source's current format, so the common case never takes exclusive control.
  Needs confirmation that `TryAcquireExclusiveControl` can enable a subsequent
  `SetFormatAsync` on a read-only open.
- Determine whether the freeze is caused by exclusive control itself or by the
  control state applied on the app's behalf at open, in which case explicitly
  re-asserting auto focus/exposure after opening might restore it. Neither
  available camera exposes those controls, so this cannot be tested here.
- Accept it as a platform constraint and document the image-quality cost.

Any fix must be verified by observing whether autofocus behaves while a stream
is running, and by a full runtime-validator pass — the read-only attempt
passed the build cleanly and still broke every streaming and capture check.


`validate_and_admit_submission_locked_` calls
`query_exposure_compensation_range_` on a device whose pipeline may never have
been realized — `ensure_reader_realized_` runs only in `start_stream` and in
the capture worker, both after admission. A cold read there cannot distinguish
"device cannot" from "nothing was running", so the current bracket refusal is
not trustworthy even where it happens to be correct. Fixing it needs capability
state cached at pipeline realization and consulted (never computed) at
admission, so admission stays prompt and I/O-free.
