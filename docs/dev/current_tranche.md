# Current Tranche

## Camera Facts Tranche 8 - Godot CaptureResult Exposure

### Objective

Expose the settled, immutable still-capture camera facts and capture-admission
context through the existing `CamBANGCaptureResult` surface, consistent with
`docs/camera_fact_model.md`.

### Approved Implementation Delta

- Extend `CamBANGCaptureResult.get_image_member(index)` with optional
  `camera_facts`.
- Add `get_capture_datetime_unix_nanoseconds() -> int`.
- Add `has_geolocation() -> bool`.
- Add `get_geolocation() -> Dictionary`.
- Add `CamBANGServer.set_capture_geolocation(Dictionary geolocation) -> Error`.
- Classification camera facts use `{ "value": ..., "origin": ... }`.
- Intrinsics, distortion, and pose are complete atomic dictionaries containing
  `origin`.
- Omit absent facts, and omit `camera_facts` when every resolved camera fact
  is absent.
- Expose provenance origin, not internal resolution authority.
- Leave the existing acquisition-timestamp surface unchanged.

### Capture Geolocation Ingress

`CamBANGServer.set_capture_geolocation(Dictionary geolocation) -> Error`
updates the Core-owned geolocation sampled by later successful capture
admissions. It is callable while the server is stopped or running. An empty
dictionary clears the current geolocation. A non-empty dictionary must contain
`latitude_degrees` and `longitude_degrees`, with optional `altitude_meters`.

Replacement is transactional: invalid input returns an error and preserves the
previous geolocation. Latitude and longitude must be finite WGS 84 geodetic
decimal degrees, within `[-90, 90]` and `[-180, 180]` respectively. Optional
altitude must be finite WGS 84 ellipsoidal height in metres.

The setter changes only the value sampled by future successful capture
admissions. Existing admitted captures and completed results remain unchanged.
Capture admission continues to sample geolocation once, and bracket and rig
participants continue to share their admitted context. Reuse the existing Core
geolocation state and replacement/clear behaviour; do not add a test-only
ingress or a second geolocation store.

### Documentation Scope

Reconcile the affected internal, architectural, binding, and user-facing
documentation. Keep settled architecture referenced rather than duplicated.

Geolocation documentation must define latitude and longitude as WGS 84
geodetic decimal degrees, and optional altitude as WGS 84 ellipsoidal height in
metres, not orthometric height or elevation above mean sea level.

### Critical Invariants

- Preserve exact capture, device, and image-member identity.
- Preserve atomic intrinsics, distortion, and pose records and their origins.
- Do not resample or reinterpret capture-admission context.
- Completed result facts remain immutable.

### Exclusions

- No `get_camera_facts()` method or new wrapper classes.
- No StreamResult exposure.
- No ADC, provider-ingress, SyntheticProvider-generation, result-resolution,
  admission-context redesign, or state-snapshot changes.
- No calibration, scaling, coordinate conversion, or distortion fitting.

### Acceptance Criteria

Focused checks cover classification and atomic fact dictionaries, provenance,
absence/omission semantics, bracket and rig identity isolation, shared capture
context, unchanged acquisition timestamp behaviour, and no StreamResult or
additional wrapper exposure. They also cover valid geolocation replacement,
clear through `{}`, malformed, missing, out-of-range, and non-finite rejection,
transactional preservation after rejected input, stopped-time and running-time
use, present and absent geolocation on completed capture results, and existing
admitted/completed result immutability. Documentation accurately describes the
implemented Godot surface and geolocation convention.

### Required Validation

- maintainer-tools build;
- focused Tranche 8 checks;
- full `core_spine_smoke.exe --verbose`;
- Windows GDE build;
- Android arm64 GDE build.

All required validation is governed by the hard completion contract in
`AGENTS.md`.
