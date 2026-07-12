# CamBANG Camera-Fact Model

This document defines the canonical source-neutral camera-fact architecture for
CamBANG.

It supplements and must remain consistent with:

- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`
- `docs/state_snapshot.md`
- `docs/naming.md`
- `docs/architecture/imaging_spec_seam.md`
- `docs/architecture/pixel_payload_and_result_contract.md`

It is architecture authority for:

- the separation of camera-fact authorities;
- the role of externally configured camera-description ingestion;
- the descriptive scope of calibration, pose, timing, and capture context;
- the relationship between per-camera facts, cross-camera capability truth, and
  still-capture result facts.

It does not by itself implement or approve additional public API beyond what is
explicitly recorded in `docs/naming.md` and the active tranche guidance.

---

## 1. Role

CamBANG obtains, validates, retains, resolves, and associates camera facts with
still-capture results.

CamBANG does not:

- perform lens calibration;
- rectify or distort images;
- project or unproject points;
- convert calibration between coordinate domains;
- rescale intrinsics for delivered images;
- fit or approximately translate one distortion model into another;
- infer missing calibration from sensor size, physical focal length, or similar
  approximations;
- use calibration to affect admission, arbitration, provider selection, backing
  plans, or capture execution.

Rich camera facts are principally a still-capture concern. `StreamResult`
remains deliberately metadata-light.

---

## 2. Source-Neutral Internal Model

Internal CamBANG types must remain source-neutral rather than ADC-, JSON-,
Godot-, or one-platform-shaped.

The intended fact authorities are distinct:

- `ProviderCameraFacts` for provider static/device facts;
- `ProviderCaptureImageFacts` for per-still-image or per-member facts;
- `ExternalCameraFacts` for persistent externally ingested static facts;
- capture-admission runtime context for geolocation and Capture Date-Time;
- realized payload truth for delivered dimensions, format, backing, and member
  identity.

At result assembly:

```text
provider static facts
+ provider capture-image facts
+ matching external camera facts
+ capture-admission context
+ realized payload truth
-> immutable CaptureResult facts
```

SyntheticProvider is a first-class virtual camera provider and supplies its
facts directly through provider-native C++ types. Its normal path does not
depend on ADC JSON.

---

## 3. Authority Split

Camera facts in CamBANG remain split by authority and operational role.

### 3.1 `CameraSpec`

`CameraSpec` is the per-camera stable specification/capability seam.

It is the home for stable per-camera description truth such as classification,
intrinsics, distortion, and pose.

### 3.2 `ImagingSpec`

`ImagingSpec` remains the cross-camera / imaging-subsystem operational
capability seam.

It carries capability truth that Core may need for current admission and
validation decisions. It must not become a general per-camera metadata,
calibration, or result-fact bucket.

The current implemented `ingest_camera_description(String json_text)` path
retains complete external camera-description facts and projects only optional
concurrency truth into `ImagingSpec`. That behavior does not make the broader
camera-description model an `ImagingSpec` responsibility.

### 3.3 Result facts

Still-capture results carry realized image-associated fact groups where
available.

Result facts remain distinct from:

- retained provider baseline facts;
- externally configured static facts;
- runtime capture-admission context;
- current runtime posture or admission truth.

---

## 4. Public Camera-Description API

The current server-level external camera-fact input is:

```gdscript
Error CamBANGServer.ingest_camera_description(String json_text)
```

It accepts a complete supported ADC v2 camera-description document while
stopped. Accepted documents replace prior external camera-description truth
transactionally and persist across stop/start. The optional concurrency section
is the only portion projected into `ImagingSpec`; legacy ADC v1
concurrency-only documents are not accepted by this API.

Capture geolocation remains separately gatekept and is not implemented as a
Godot API in this checkout.

CamBANG must not add:

- filesystem/path convenience ingestion methods;
- generic capture-context APIs;
- new Godot wrapper classes solely to carry camera facts.

Godot result and ingestion surfaces remain explicitly gatekept.

---

## 5. External Camera Description Lifecycle

External camera-description ingestion in CamBANG is:

- server-level;
- full replacement, not patch;
- transactional;
- persistent across stop/start;
- immutable during an active generation;
- matched by exact case-sensitive `camera_id == hardware_id`;
- schema-version gated to the exact compiled supported ADC v2 version.

Rejected input preserves prior accepted truth.

An accepted replacement containing `cameras: []` explicitly clears all retained
external per-camera facts.

Omitted facts or omitted camera entries in an accepted replacement remove the
corresponding previous external facts.

Unknown members inside a supported additive schema version may be ignored.
Recognized fields and recognized enum domains are validated strictly.

Unsupported schema versions are rejected.

---

## 6. Camera Identity

ADC `camera_id` matches CamBANG stable `hardware_id` by exact, case-sensitive
string equality.

Do not add:

- provider qualifiers;
- host qualifiers;
- prefixes;
- index heuristics;
- facing/model fallback;
- whitespace normalization;
- numeric interpretation.

An incorrectly applied document is caller error rather than something CamBANG
tries to repair heuristically.

Valid unmatched entries may still be retained by CamBANG as configured external
facts for later matching.

---

## 7. Document Provenance

ADC document-level provenance fields may be useful to ADC, Aide-De-Cam, humans,
or external tools, but they are non-authoritative to CamBANG.

Examples include:

- `generator`
- `generator_version`
- `timestamp_ms`
- `device_model`
- `device_manufacturer`
- `android_version`
- `godot_version`

CamBANG must not use, retain, compare, expose, derive from, or assign
precedence based on those fields.

In particular, ADC `timestamp_ms` has no effect on freshness, expiry, admission,
configured versioning, Capture Date-Time, result timing, matching, or warning
policy.

---

## 8. Fact Origin

The ADC fact-source vocabulary and sourced classification/atomic-record shapes
are defined by `docs/adc_camera_description_v2.md`.

CamBANG internally distinguishes fact origin from effective authority. The
first public implementation need not expose that full internal authority split.

---

## 9. Classification Facts

Static camera classification remains separate from realized per-image transform.

The normative ADC classification token vocabulary and sourced record shape are
defined by `docs/adc_camera_description_v2.md`.

CamBANG resolves classification values independently while retaining origin
separately from effective authority.

These are descriptive stable facts, not image-time transform results.

---

## 10. Atomic Records

Intrinsics, distortion, and pose are atomic descriptive records.

External configured records replace the whole applicable provider record. Do not
merge part of one record with part of another.

### 10.1 Intrinsics

Intrinsics are pixel-space calibration with required reference dimensions and a
required coordinate domain.

CamBANG carries them descriptively and performs no rescaling, crop adjustment,
rotation adjustment, or projection work.

### 10.2 Distortion

Distortion model vocabulary and record shape are defined by the ADC v2
contract.

Distortion records include their coordinate domain, reference dimensions where
applicable, and image-state interpretation.

CamBANG does not rectify, convert, interpolate, or fit distortion models.

### 10.3 Pose

Pose is an atomic per-camera descriptive record with:

- a reference kind;
- a coordinate convention;
- translation in metres;
- quaternion order `x, y, z, w`.

CamBANG carries pose descriptively and performs no spatial calculation.

---

## 11. Coordinate Domains and Conventions

The normative ADC v2 coordinate-domain and coordinate-convention token
vocabulary is defined by `docs/adc_camera_description_v2.md`.

CamBANG architectural consequences are:

- `platform_defined` requires a stable platform token;
- a record with unknown coordinate meaning must not become effective
  calibration truth;
- a record with unknown pose coordinate meaning must not become effective pose
  truth.

---

## 12. Timing and Capture Context

### 12.1 Capture Date-Time

Capture Date-Time is a runtime capture-context fact, not ADC document
provenance and not Image Acquisition Timing.

The initial rule is:

- sampled by Core at successful capture admission;
- stored as an absolute UTC instant;
- shared across one bracket and one rig capture;
- not used for ordering, duration, synchronization, or latency.

### 12.2 Image Acquisition Timing

The public concept is Image Acquisition Timing, not an overloaded generic
"capture timestamp."

Its central value is an acquisition mark with explicit clock-domain and
comparability meaning.

It is per-image-member descriptive timing rather than document metadata or
capture-admission wall-clock context.

### 12.3 Geolocation

Geolocation is capture-admission context, not provider execution input and not
camera-description static fact truth.

It is replaceable independently of camera-description ingestion and one
immutable snapshot is taken at successful capture admission.

---

## 13. Effective Precedence

Static overrideable records resolve by:

```text
matching external configured record
> applicable provider result-specific record
> applicable provider baseline record
> absent
```

This applies to:

- facing
- sensor orientation
- camera nature
- intrinsics
- distortion
- pose

Classification values resolve independently. Intrinsics, distortion, and pose
remain atomic.

Dynamic image-time facts remain provider-owned rather than ADC-overridable.

---

## 14. Relationship to ADC v2

ADC is an external camera-description interchange standard historically
associated with Aide-De-Cam. ADC v2+ is the target external
camera-description interchange contract for CamBANG.

ADC v1 human and machine schemas are historical source input only. CamBANG does
not require ADC v1 ingestion and does not carry ADC v1 permission-sentinel
conventions into ADC v2.

CamBANG is a consumer of ADC and is provisionally hosting the ADC v2 contract
in this repository to establish current compatibility. Long-term canonical
ownership of ADC is intended to return to Aide-De-Cam.

The repository-hosted ADC v2 contract CamBANG currently targets is defined in
`docs/adc_camera_description_v2.md`, with the matching machine-readable schema
under `schema/adc/camera_description/v2/`.
