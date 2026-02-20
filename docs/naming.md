# CamBANG Naming and Terminology

This document defines the canonical names used by CamBANG in its public
Godot-facing API, configuration concepts, and diagnostics/introspection.

------------------------------------------------------------------------

## 1. Godot-facing API objects

### `CamBANGServer`

Singleton service/registry/factory for CamBANG. Responsibilities include
device enumeration, rig creation, global shutdown, and access to the
latest published state snapshot.

------------------------------------------------------------------------

### `CamBANGRig`

User-created multi-camera coordinator used to perform synchronised
capture across multiple devices.

Primary lifecycle controls:

-   `arm()` / `disarm()` --- place the rig into a capture-ready state or
    remove it from that state.
-   `trigger_sync_capture()` --- initiate a synchronised capture across
    rig members.

Rig-triggered sync capture has priority over standalone activity on
member devices when conflicts arise.

------------------------------------------------------------------------

### `CamBANGDevice`

Represents one camera hardware endpoint (physical or logical, depending
on platform).

Primary lifecycle controls:

-   `engage()` / `disengage()` --- begin or cease engagement with
    underlying camera resources (subject to warm policy).
-   `set_warm_policy(...)` --- define resource retention behaviour when
    not actively capturing.
-   `set_still_capture_profile(profile)` --- define configuration used
    for device-triggered still capture.
-   `trigger_capture()` --- perform a single-device still capture.

Devices may operate standalone or as members of a rig.

------------------------------------------------------------------------

### `CamBANGStream`

A repeating frame output channel associated with a specific device
instance.

Primary controls:

-   `start()` / `stop()` --- begin or cease the repeating flow of
    frames.

Each device supports at most **one active repeating stream at a time**
(design choice).

Streams are created with a `StreamIntent`:

-   `PREVIEW` --- low-latency, disposable, repeating feed intended for
    framing and UX.
-   `VIEWFINDER` --- higher-fidelity repeating feed with best-effort
    throughput, subject to arbitration policy.

------------------------------------------------------------------------

## 2. Capture Profiles

CamBANG uses the term **Capture Profile** to describe fidelity and
throughput configuration for image production. The valid fields of a
profile depend on intent (e.g., repeating stream vs still capture).

Two primary uses exist:

### Stream Capture Profile

A profile used for repeating streams (`PREVIEW` or `VIEWFINDER`).
Typical fields include:

-   Resolution
-   Raw pixel format
-   Target FPS or FPS range (best-effort)
-   Optional buffering hints

### Still Capture Profile

A profile used for triggered still capture (device-triggered or
rig-triggered). Typical fields include:

-   Resolution
-   Pixel format (may include formats such as `'JPEG'` and `'RAW '`
    where supported)
-   Optional processing hints

Profiles define capture fidelity and intent, not hardware truth.

------------------------------------------------------------------------

## 3. Specifications and configuration

CamBANG separates **hardware-reported specifications** (with optional
user corrections) from **user intent/configuration**.

### `CameraSpec`

Per-camera characteristics/capabilities (for example: sensor properties,
intrinsic parameters if available, supported formats, etc.).
`CameraSpec` is keyed by a camera's `hardware_id`.

### `ImagingSpec`

Global imaging subsystem constraints/capabilities that apply across
cameras (for example: concurrency/combination rules). `ImagingSpec` is
not per-camera.

### `RigConfig`

User intent for a particular rig (membership, sync policy, capture
profile). `RigConfig` is not a spec and is not treated as hardware
truth.

------------------------------------------------------------------------

## 4. Spec updates and `apply_mode`

Spec updates follow a single API pattern:

-   `update_camera_spec(camera_id, patch, apply_mode=...)`
-   `update_imaging_spec(patch, apply_mode=...)`

### `ApplyMode`

-   `APPLY_WHEN_SAFE` (default for release): accept and apply when it is
    safe to do so.
-   `APPLY_NOW` (advanced/testing): attempt to apply immediately; may
    fail if policy forbids or if a safe condition cannot be met.

"Safe" is defined by CamBANG policy and generally means that affected
devices are not actively engaged with hardware resources and there is no
in-flight synchronised capture that depends on them.

------------------------------------------------------------------------

## 5. Published snapshot and introspection

CamBANG exposes a read-only, immutable snapshot that reflects the
current truth of the core and its owned native objects.

### `CamBANGStateSnapshot`

Immutable, point-in-time snapshot published by core to `CamBANGServer`,
and exposed for polling/inspection.

### Snapshot record types

Records inside the snapshot that correspond to user-facing objects:

-   `CamBANGRigState`
-   `CamBANGDeviceState`
-   `CamBANGStreamState`

Native/core objects created by CamBANG are tracked as registry records:

-   `NativeObjectRecord`

### Publication counters

-   `gen`: increments on every published snapshot.
-   `topology_gen`: increments when the structural hierarchy changes
    (e.g., new instances, membership changes, detached branches
    appearing/disappearing).

### Signals

-   `state_published(gen, topology_gen)` --- emitted by `CamBANGServer`
    when a new snapshot is published.

------------------------------------------------------------------------

## 6. Lifecycle `phase` vs operational `mode`

To avoid ambiguity, CamBANG uses separate fields for lifecycle and
operational posture.

### `phase` (lifecycle)

Lifecycle phases refer only to existence and teardown:

-   `CREATED`
-   `LIVE`
-   `TEARING_DOWN`
-   `DESTROYED`

### `mode` (operational posture)

Operational mode describes what an entity is currently doing.

Rig mode examples: - `OFF`, `ARMED`, `TRIGGERING`, `COLLECTING`, `ERROR`

Device mode examples: - `IDLE`, `STREAMING`, `CAPTURING`, `ERROR` -
`engaged: bool` indicates whether underlying camera resources are
currently held.

Stream mode examples: - `STOPPED`, `FLOWING`, `STARVED`, `ERROR`

`StreamIntent` defines purpose (`PREVIEW` or `VIEWFINDER`) separately
from operational mode.

`NativeObjectRecord` always has a `phase`. A `mode` may be included when
useful but is not required.

------------------------------------------------------------------------

## 7. Identity and lineage

To support reliable diagnostics (including "old teardown + new live"
scenarios), CamBANG separates stable hardware identity from runtime
instance identity.

-   `hardware_id`: stable platform camera identifier (as reported by the
    platform backend).
-   `instance_id`: monotonic runtime identifier for a specific opened
    lineage of that hardware.
-   `root_id`: lineage root identifier used for grouping branches.

Detached branches are those no longer attached to the active hierarchy
but still present due to teardown or retention policies.

------------------------------------------------------------------------

## 8. Internal naming (for contributors)

These names appear in the native/core implementation, build system, and
debugging logs.

-   `ICameraProvider`: platform backend interface (e.g., Android camera2
    provider, stub provider, synthetic provider).
-   `CBLifecycleRegistry`: tracks CamBANG-owned native/core object
    lifecycles and retains recently destroyed records for inspection.
-   `CBStatePublisher`: assembles and publishes `CamBANGStateSnapshot`
    and performs retention sweeps.

------------------------------------------------------------------------

## 9. Glossary (quick reference)

-   **Spec**: hardware-reported truth (with optional user corrections).
-   **Config**: user intent (choices made by the developer/app).
-   **Capture Profile**: fidelity definition for image production
    (streams and stills).
-   **Snapshot**: immutable published record of current truth.
-   **Phase**: lifecycle stage of an entity/native object.
-   **Mode**: operational posture of an entity.
-   **Detached**: a branch no longer attached to the active tree but
    still present due to teardown/retention.
-   **StreamIntent**: purpose of a repeating stream (`PREVIEW` or
    `VIEWFINDER`).
