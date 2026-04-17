# Pixel / Result Architecture Direction

Status: Dev note (design-direction consolidation)  
Purpose: Records the agreed architectural direction for CamBANG’s release-facing image/result path and the split between State Snapshot, System Graph, and Result Objects.

This note consolidates design direction reached during architecture review and is intended to reduce drift before implementation.

It supplements, and does not redefine, the canonical documents including:

- `docs/state_snapshot.md`
- `docs/architecture/godot_boundary_contract.md`
- `docs/architecture/publication_model.md`
- `docs/provider_architecture.md`
- `docs/architecture/frame_sinks.md`
- `docs/naming.md`

---

## 1. Why this note exists

CamBANG’s current architecture already separates:

- immutable operational/runtime introspection (`State Snapshot`)
- internal frame routing/sinks
- release-facing image access in terms of:
    - **Stream Result**
    - **Capture Result**
    - **Capture Result Set**

However, the project has now identified a broader release-facing need:

1. a pixel path that is not artificially CPU-only
2. a result model that does not force all image-bearing outputs into one representation
3. a slower, query-oriented truth surface distinct from the fast operational snapshot
4. richer image-associated facts (capture state, location, calibration, provenance) without bloating the hot snapshot path

This note records that agreed direction.

---

## 2. Existing surfaces to preserve

## 2.1 State Snapshot remains narrow and operational

`State Snapshot` remains the immutable, tick-bounded, operational/runtime truth surface.

It remains the correct home for things such as:

- phase
- mode
- counters
- errors
- applied runtime capture-profile truth
- active runtime topology and registry-derived diagnostics

It should remain lean and suited to:

- lock-free polling
- status/debug UIs
- deterministic runtime introspection

This note does **not** recommend renaming or broadening `State Snapshot` into a catch-all truth container.

## 2.2 Result-oriented image access remains the release-facing direction

Release-facing/public image access remains result-oriented rather than mailbox-oriented.

The existing canonical image-access nouns remain:

- **Stream Result**
- **Capture Result**
- **Capture Result Set**

These are the correct public/runtime-visible concepts and should continue to anchor the release API.

---

## 3. Truth-layer split

CamBANG should be understood as exposing more than one kind of retained truth.

## 3.1 State Snapshot

**State Snapshot** is the immutable, tick-bounded operational truth surface.

It is for:

- runtime state
- counters
- lifecycle/mode truth
- active topology
- compact diagnostics

It is not the right primary home for rich per-result image-associated fact bundles.

## 3.2 System Graph

**System Graph** is the slower, query-oriented structural/configuration/calibration truth surface.

It is intended to support things such as:

- platform-known devices
- rig membership and grouping
- system/rig origin
- spatial relationships where known
- device placement relative to system/rig origin where known
- capability and combination constraints
- corrected device/lens/specification truth
- calibration and optics knowledge
- user defaults and overrides relevant to device understanding

System Graph is distinct from State Snapshot:

- not tick-bounded
- not primarily status/counter oriented
- not intended to churn with every frame-time camera-state update

## 3.3 Result Objects

**Result Objects** are the image-bearing truth surface.

They carry the realized image artifact and the image-associated facts needed to use, inspect, process, or display it.

They are the natural home for:

- image properties
- capture-time observed camera state
- attached location facts
- optical calibration carried with the image
- provenance of those facts
- materialization capabilities and cost classifications

---

## 4. Snapshot stays lean

A key direction from this design work is that **Core may retain more truth than Snapshot exposes**.

This is intentional.

The fact that Core retains some image-associated truth does **not** imply that all of that truth belongs in the frequently-published public snapshot.

In particular, rich result-associated facts should generally remain off the hot snapshot path unless there is a strong operational reason to project a compact summary.

### 4.1 Why

Putting rich per-result facts directly into the hot snapshot path would tend to:

- enlarge immutable snapshot payloads
- increase assembly/publish work on the core thread
- increase observable `version` churn
- burden status/polling consumers with result-rich detail they often do not need
- prematurely freeze a much larger public introspection contract

### 4.2 Preferred direction

Use Snapshot for:

- operational truth
- compact summary indicators

Use Result Objects for:

- rich image-associated detail

Use System Graph for:

- slower structural/configuration/calibration truth

---

## 5. Pixel path direction

CamBANG’s release path should be **multi-representation**, not CPU-only and not GPU-only.

The guiding principle is:

> providers surface the most efficient truthful payload available on that platform; Core routes, retains, and accounts for it; results expose truthful capability-aware access and explicit materialization.

This implies:

- no universal requirement that all frames become CPU `Image`s
- no universal requirement that all image-bearing outputs remain GPU-only
- explicit support for different retained representations depending on platform, provider, and use case

---

## 6. Result-object philosophy

Result Objects should be **capability-first**.

They should expose:

1. what the user has
2. what the user can do with it
3. what those actions are likely to cost
4. whether a given access path is already available, cheap, expensive, or unsupported

This is the main architectural justification for the additional complexity of multi-representation payloads.

A superficially simpler API that silently forces universal readback, conversion, or re-encoding would hide major performance cliffs.

---

## 7. Qualified fact groups, not generic “metadata”

The term **metadata** is too overloaded to be the primary architectural term for rich image-associated facts.

Instead, Result Objects should expose qualified fact groups.

Recommended first-pass group names:

- `ImageProperties`
- `CaptureAttributes`
- `LocationAttributes`
- `OpticalCalibration`

### 7.1 ImageProperties

Structural image facts, for example:

- width
- height
- pixel/encoding format
- orientation
- bit depth
- crop region where meaningful

### 7.2 CaptureAttributes

Camera/device state observed or effective at image time, for example:

- exposure time
- aperture / f-number
- focal length
- focus distance
- sensor sensitivity / ISO-equivalent
- flash state
- white balance state
- zoom/crop state

### 7.3 LocationAttributes

Capture-associated location facts, for example:

- latitude / longitude / altitude
- location accuracy where known
- location timestamp where known

### 7.4 OpticalCalibration

Calibration and optics facts, for example:

- intrinsics
- principal point
- focal lengths in calibration space
- distortion model
- distortion coefficients

---

## 8. Provenance direction

Provenance is **per fact**, not per result.

A single result may legitimately contain mixed truth such as:

- exposure time from hardware/API
- location from runtime injection
- distortion coefficients from user override
- some unsupported fields still unknown

Therefore provenance must attach to individual exposed facts (or to small fact groups), not to the result as one blanket classification.

Recommended first-pass provenance vocabulary:

- `HARDWARE_REPORTED`
- `PROVIDER_DERIVED`
- `RUNTIME_INJECTED`
- `USER_DEFAULT`
- `USER_OVERRIDE`
- `UNKNOWN`

### 8.1 Intended meaning

- `HARDWARE_REPORTED`  
  The same fact came from backend/API/hardware truth.

- `PROVIDER_DERIVED`  
  The provider computed or materially transformed this fact from other authoritative inputs.

- `RUNTIME_INJECTED`  
  The embedding runtime/application supplied the fact at runtime (for example current location).

- `USER_DEFAULT`  
  The fact comes from a user-supplied fallback/default.

- `USER_OVERRIDE`  
  The fact comes from a user-supplied corrective override.

- `UNKNOWN`  
  No authoritative value is currently retained.

### 8.2 UX direction

Provenance should remain available without overwhelming the default API experience.

The intended direction is:

- direct values remain easy to read
- provenance is grouped and inspectable
- compact summary helpers may exist for “mixed / any overrides / all hardware-reported” style queries

---

## 9. Distortion/calibration policy

Default policy should be:

> retain the original image artifact and carry calibration/distortion with it.

CamBANG should not silently rectify or undistort by default.

### 9.1 Why

This preserves:

- original image truth
- explicit cost boundaries
- flexibility for vision/photogrammetry/science use cases
- the ability to display or save the original and derived forms separately

### 9.2 Future-oriented design requirement

Even though rectification may be future work, the architecture should be designed to allow efficient derived rectified materializations later.

That means:

- original image remains authoritative
- rectified image is a derived materialization
- capability/cost reporting should eventually be able to distinguish:
    - original view/materialization paths
    - rectified view/materialization paths

---

## 10. Device-facing control direction

`CamBANGDevice` is the natural Godot-facing access point for camera/device control requests such as:

- exposure mode
- exposure time
- aperture
- sensitivity / ISO-equivalent
- focus mode / distance
- white balance mode
- zoom / crop
- related control families

These should not be treated as naive immediate setters.

They are requests whose realization depends on:

- current runtime state
- rig ownership/authority
- provider/device capability
- whether a safe point exists
- whether live reconfiguration is allowed
- whether backend rebuild/restart is required

### 10.1 Core’s role

Core should be **active**, not passive.

Core should own:

- validation
- policy/adjudication
- sequencing
- requested/effective state retention
- safety/defer logic
- lifecycle/resource accounting consequences

### 10.2 Provider’s role

Provider remains the execution-plane adapter and owns:

- backend API specifics
- live vs rebuild realization details
- truthful reporting of outcomes
- truthful reporting of any lifecycle/native-object consequences

### 10.3 Three-way truth split

For controllable attributes, the design should preserve:

- **requested** truth
- **effective** truth
- **observed** truth

Those three may legitimately differ, especially under auto modes and constrained backends.

---

## 11. First-pass control taxonomy

The current direction is to classify control requests by **realization class**, not by superficial setter shape.

Recommended first-pass classes:

- **live-safe**
- **quiescence-required**
- **restart/rebuild**
- **annotation-only**

This classification is intended to support stable Core semantics while allowing providers to advertise what each device/backend can actually realize.

---

## 12. Device-facing surface direction

`CamBANGDevice` should be an ergonomic access point, but should not collapse all truth layers into one undifferentiated bag of mutable-looking properties.

The intended direction is that device-facing access will likely need grouped sub-surfaces for things such as:

- stable/corrected specification/calibration truth
- latest observed summaries
- result access
- control requests

This note does not freeze exact API shapes, but records the principle that:

- stable specification/calibration truth
- runtime operational truth
- latest image-associated observed truth

should not be conflated.

---

## 13. Relationship between State Snapshot, System Graph, and Result Objects

A useful mental split is:

### State Snapshot
Operational runtime truth.

### System Graph
Structural/configuration/calibration truth.

### Result Objects
Image-bearing realized truth.

Together, these form three complementary public/runtime-visible layers rather than one overloaded “state” surface.

---

## 14. Explicit non-goals of this note

This note does **not** define:

- final public method signatures
- final C++ type layout
- final wire/schema additions
- full System Graph schema
- full result-object API surface
- provider-specific backend mappings
- recording/broadcast/sequence APIs
- full rectification pipeline design

Those belong in narrower contract/design documents.

---

## 15. Immediate documentation consequence

This note is intended to accompany a more focused contract document:

- `docs/architecture/pixel_payload_and_result_contract.md`

That companion document should define the payload/result/materialization contract in more detail.

---