# Current tranche

## WinRT-native bracketed capture

Maintainer-activated (2026-07-20). See `[[winrt-bracketing-expectation]]`
memory for prior context: the toolchain blocker (C++/WinRT needs MSVC) is
resolved as of the committed `windows_winrt` provider rewrite.

### Scope

`WinrtCameraProvider` gains real multi-image still-capture sequences
(`supports_multi_image_still_sequence()` → `true`), driven by the device's
`VideoDeviceController.ExposureCompensationControl` (real per-member
exposure-compensation values applied to the actual camera between
sequential member captures, with truthful realized-value readback) — not
`VariablePhotoSequenceCapture`/`AdvancedPhotoCapture`, which are niche
professional/HoloLens-class APIs unlikely to be supported (and therefore
unverifiable) on the available UVC webcam.

Per-device, not blanket: admission checks the specific opened device's
`ExposureCompensationControl.IsSupported` and rejects bracket bundles with
`ERR_NOT_SUPPORTED` when absent, and rejects bundles exceeding a provider
cap (`kMaxBracketMembers`) the same way. Member 0 (`DEFAULT_METERED`) is
captured without touching the control; each `ADDITIONAL_BRACKET` member
gets its requested EV applied (converted from milli-EV, clamped and
step-rounded to the device's actual range), then the truthful post-set
readback is reported as `realized_exposure_compensation_milli_ev` — never
the requested value re-stated as if verified. The control is restored to
its pre-bracket value after the bundle finishes (best-effort; a restore
failure does not fail an otherwise-successful capture, but is logged).

`capture_admission_watchdog_timeout_ns()` gains a derived override (not a
guessed constant) sized from the existing bounded per-step timeouts times
`kMaxBracketMembers`, since a cold multi-member bracket's bounded worst
case now exceeds Core's 30s default.

### Explicitly out of scope

- `VariablePhotoSequenceCapture`/`AdvancedPhotoCapture` — no hardware here
  can exercise it; adding an unverifiable code path is not worth it.
- Stream-side exposure-compensation control (`set_stream_picture_config`
  stays `ERR_NOT_SUPPORTED`).

### Acceptance / validation

- MSVC Windows GDE build and validator build clean.
- WinRT hardware runtime validator extended with a bracket-capture check
  (admission, per-member ordering, realized-EV distinctness, restore) and
  passing on the real Integrated Camera.
- Scene 01 (`01_basic_ux.tscn`, manual) bracket profile now admitted rather
  than refused, when run against the platform-backed build.
- Full regression: native verifiers, scenes 65/66/70/73/870 on both the
  MinGW (synthetic-only) and MSVC (windows_winrt) GDE artifacts.
