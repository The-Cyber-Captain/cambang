# Current Tranche

## Camera Facts Tranche 9 - Closeout

### Objective

Close the camera-fact work by auditing the checked-out repository for
consistency between implemented source and Godot bindings, focused and
Core-level verification, architecture and user-facing documentation, canonical
naming and public dictionary vocabulary, and active development guidance.

### Required Work

The implementation task must audit completed Tranches 0 through 8 as
represented by the current repository, and directly correct any contradiction,
obsolete proposed surface, stale future wording, or superseded terminology.
Prefer retirement or direct correction over compatibility notes or historical
commentary.

Confirm the final public Godot surface:

- `CamBANGServer.ingest_camera_description(String json_text) -> Error`;
- `CamBANGServer.set_capture_geolocation(Dictionary geolocation) -> Error`;
- `CamBANGCaptureResult.get_capture_datetime_unix_nanoseconds() -> int`;
- `CamBANGCaptureResult.has_geolocation() -> bool`;
- `CamBANGCaptureResult.get_geolocation() -> Dictionary`;
- optional per-member `get_image_member(index).camera_facts`.

Confirm the canonical classification keys: `facing`, `camera_nature`, and
`sensor_orientation_degrees`. Confirm that intrinsics, distortion, and pose
remain complete atomic dictionaries containing `origin`; public fact origin is
distinct from internal resolution authority; absent facts are omitted; and an
entirely absent `camera_facts` dictionary is omitted.

Confirm existing acquisition timestamp behaviour remains unchanged; no
`get_camera_facts()` method, camera-fact wrapper class, or StreamResult
camera-fact/geolocation exposure exists; and geolocation uses WGS 84 geodetic
latitude/longitude with optional WGS 84 ellipsoidal altitude.

Confirm the camera-fact work has not acquired calibration, rectification,
projection, scaling, conversion, or approximation behaviour.

### Verification Posture

Require focused review of:

- camera-fact Core resolution and omission coverage;
- Scene 569's sanctioned headless Godot-boundary verification;
- source-to-document public-key consistency;
- bindings and rejected API absence;
- repository-wide stale terminology and contradictory design claims.

Any genuine contradiction found may be corrected narrowly. No new feature,
API, wrapper, provider behaviour, result shape, or architectural seam may be
added.

### Required Validation

After the final correction, run every validation mandated by `AGENTS.md`,
including at minimum:

- maintainer-tools build;
- focused Core camera-fact resolution check;
- full `core_spine_smoke.exe --verbose`;
- Windows GDE build;
- Android arm64 GDE build;
- Scene 569 through the sanctioned headless runner.

### Completion

The tranche may close only when all required validation passes; source, tests,
naming, architecture, and public documentation agree; no obsolete alternative
public surface remains; and the active tranche can be retired without leaving
deferred camera-fact implementation work.

Keep settled architecture referenced rather than reproducing its full
specification.
