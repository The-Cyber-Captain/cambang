# Current tranche

## Still capture via LowLagPhotoCapture

Maintainer-activated (2026-07-21).

### Why

`WinrtCameraProvider` currently serves still captures by taking the next
matching frame off the shared video `MediaFrameReader` — the same reader that
feeds streams. That is not a chosen design; it is what the code does. A still
capture should use the still-capture API.

`LowLagPhotoCapture` is also required for the settled bracket fallback tier
(repeated `CaptureAsync()` with exposure changed and settled between members),
so building it once serves both single and bracketed capture rather than
bolting a second mechanism on later.

### Scope

Still captures — single and every bracket member — are served by
`MediaCapture.PrepareLowLagPhotoCaptureAsync` / `LowLagPhotoCapture
.CaptureAsync()`. `MediaFrameReader` remains the stream path, so the split is
stills-versus-streams and no third mechanism is introduced.

The prepared `LowLagPhotoCapture` is a per-device resource: prepared lazily,
finished on device close, and correctly abandoned under the existing
generation-based cancellation on shutdown/restart. It is a lifetime-significant
support resource, so consider whether it warrants native-object facts.

Bracketing keeps its current shape — `ExposureCompensationControl` written and
settled between members, truthful realized readback, best-effort restore — but
the members become real still captures. Where the device has no
exposure-compensation control, admission continues to refuse the bundle
deterministically with `ERR_NOT_SUPPORTED` rather than degrading silently.

### Measured constraints — do not re-derive these

Established on the Integrated Camera and eMeet C970 before this tranche opened:

- `LowLagPhotoCapture` is supported on both, and captures succeed while a
  frame reader is streaming; the stream keeps delivering during and after.
- **Photo geometry is not independent of the stream.** Setting
  `MediaStreamType::Photo` properties silently reconfigured the running preview
  from 640x480 to 1920x1080. Photo geometry must therefore be kept aligned with
  stream geometry, and the existing geometry-conflict refusal
  (`ERR_PLATFORM_CONSTRAINT` when a started stream pins a different size) stays
  as truthful behaviour. It is a hardware constraint, not an artefact of the
  current mechanism.
- **No camera-fact benefit.** `ICapturedFrame2.ControlValues` is absent or null
  on both devices, and KS `CameraControl` values do not refresh across the
  photo lifecycle. The case for this tranche is capture correctness and the
  bracket fallback, nothing more.

### Explicitly out of scope

- Realized optical camera facts. Parked; external camera-description ingestion
  remains the sole Windows source.
- `VariablePhotoSequenceCapture`. Measured unsupported on both cameras.
- Removing or relaxing the geometry-conflict refusal.

### Open design points to settle within the tranche

1. **Acquisition timing — a measured regression, to be accepted knowingly.**
   `ICapturedFrame`/`ICapturedFrame2` expose no timestamp, whereas
   `MediaFrameReference` supplies `SystemRelativeTime` (monotonic, 100ns),
   which is what the current mark is built from.

   `BitmapProperties` was checked and does carry a time: EXIF
   `DateTimeOriginal` (tag 36867) plus `SubsecTimeOriginal` (37521), about
   millisecond resolution. It is not a substitute. That is a wall-clock value,
   and Image Acquisition Timing is a monotonic mark in the provider's clock
   domain — Capture Date-Time already covers absolute UTC and comes from Core
   at admission. A wall clock can also jump.

   So the photo path loses the frame-borne monotonic mark. The remaining
   truthful option is a provider-observed mark taken when `CaptureAsync()`
   returns, reported with `PROVIDER_OBSERVED` and the provider's own clock
   domain, with comparability set no higher than the ordering that actually
   holds. Weaker than today. Decide it deliberately; do not let it happen by
   omission, and do not substitute the EXIF wall clock.

   Note also that the only other `BitmapProperties` entries are EXIF Make,
   Model and Software, and Make/Model describe the *host machine* (Razer /
   Blade 15), not the camera. They are not camera facts and must not be
   surfaced as any.
2. **Payload conversion — settled, no new code needed.** A
   `LowLagPhotoCapture` `CapturedFrame` exposes
   `ICapturedFrameWithSoftwareBitmap`, and on both available cameras the
   bitmap comes back as `Bgra8` (format 87) with `StartIndex` 0, stride equal
   to width*4, capacity exactly the packed size, and real image content. That
   is precisely what the existing `convert_software_bitmap()` consumes, so the
   photo path feeds it unchanged and the packed `cpu_payload_owner` is
   produced the same way stream frames already are. Per-member allocation
   remains accepted for stills.

   The dimension check inside that function requires the bitmap to match the
   requested geometry. Photos follow the frame source's current format (shown
   by a photo returning 640x480 while the source was pinned there), and the
   provider already sets source geometry from the capture request, so they
   agree -- but the implementation must not assume it, since a mismatch is a
   conversion failure rather than a resize.
3. **Capture without a stream.** Captures currently realize the reader on
   demand. Decide whether a capture on a device with no started stream still
   needs the reader realized at all once photos no longer come from it.
4. **Bounded-execution budget.** `capture_admission_watchdog_timeout_ns()` is
   derived from the current per-step timeouts; re-derive it if the photo path
   changes the worst-case chain.

### Acceptance / validation

- MSVC and MinGW builds clean, no new warnings.
- All five native verifiers pass unmodified.
- Godot scenes 65, 66, 70, 569, 73, 870 pass on both GDE artifacts.
- `out/windows_winrt_runtime_validate.exe` passes against real hardware, with
  its still-capture checks exercising the photo path — including that a
  capture succeeds while a stream is running and the stream survives it.
- The bracket-EV half remains unexercisable on available hardware; its
  deterministic admission refusal must still be verified.
- A clean build is not evidence on this MSVC-only path: an earlier sharing-mode
  change compiled without warnings and broke every streaming and capture check.
  Run the validator.
