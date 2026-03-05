# CamBANG Naming and Terminology

This document defines the canonical names used by CamBANG in its public
Godot-facing API, configuration concepts, and diagnostics/introspection.

------------------------------------------------------------------------

## 1. Godot-facing API objects

### `CamBANGServer`

Singleton service/registry/factory for CamBANG. Responsibilities include
device enumeration, rig creation, global shutdown, and access to the
latest published state snapshot.

CamBANGServer is Engine singleton; explicit start()/stop(); get_state_snapshot() returns NIL before first publish;
state_published(gen, version, topology_version) begins at version/topology_version 0/0 for each new gen.

------------------------------------------------------------------------

### `CamBANGRig`

User-created multi-camera coordinator used to perform synchronised
capture across multiple devices.

Primary lifecycle controls:

- `arm()` / `disarm()` --- place the rig into a capture-ready state or remove it from that state.
- `trigger_sync_capture()` --- initiate a synchronised capture across rig members.

Rig-triggered sync capture has priority over standalone activity on
member devices when conflicts arise.

------------------------------------------------------------------------

### `CamBANGDevice`

Represents one camera hardware endpoint (physical or logical, depending
on platform).

Primary lifecycle controls:

- `engage()` / `disengage()` --- begin or cease engagement with underlying camera resources (subject to warm policy).
- `set_warm_policy(...)` --- define resource retention behaviour when not actively capturing.
- `set_still_capture_profile(profile)` --- define configuration used for device-triggered still capture.
- `trigger_capture()` --- perform a single-device still capture.

Devices may operate standalone or as members of a rig.

------------------------------------------------------------------------

### `CamBANGStream`

A repeating frame output channel associated with a specific device
instance.

Primary controls:

- `start()` / `stop()` --- begin or cease the repeating flow of frames.

Each device supports at most one active repeating stream at a time
(i.e., a stream with `phase=LIVE` and `mode != STOPPED`).

Streams are created with a `StreamIntent`:

- `PREVIEW` --- low-latency, disposable, repeating feed intended for framing and UX.
- `VIEWFINDER` --- higher-fidelity repeating feed with best-effort throughput, subject to arbitration policy.

------------------------------------------------------------------------

## 2. Capture Profiles

CamBANG uses the term **Capture Profile** to describe fidelity and
throughput configuration for image production. The valid fields of a
profile depend on intent (e.g., repeating stream vs still capture).

Two primary uses exist:

### Stream Capture Profile

A profile used for repeating streams (`PREVIEW` or `VIEWFINDER`).
Typical fields include:

- Resolution
- Raw pixel format
- Target FPS or FPS range (best-effort)
- Optional buffering hints

### Still Capture Profile

A profile used for triggered still capture (device-triggered or
rig-triggered). Typical fields include:

- Resolution
- Pixel format (may include formats such as `'JPEG'` and `'RAW '` where supported)
- Optional processing hints

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

### `PictureConfig`

Per-stream picture appearance configuration.

`PictureConfig` represents user intent regarding how frames should
appear, independent of structural capture properties.

It is distinct from **Capture Profile**:

- Capture Profile defines structural capture properties such as resolution, pixel format, and frame rate range.
- PictureConfig defines picture appearance parameters.

For synthetic and stub providers, `PictureConfig` describes
pattern-generation parameters. Platform-backed providers may interpret
`PictureConfig` as picture adjustment parameters subject to capability.

### Pattern Presets (Synthetic Pixel Generation)

CamBANG uses the term **Pattern Preset** to describe a selectable synthetic pixel pattern.

Vocabulary rules:

- Programmable surfaces use a canonical enum: `PatternPreset` (zero-indexed, contiguous).
- Edge surfaces (CLI/UI) use stable string tokens (`name`) defined by the Pattern Module’s preset registry.
- The enum and token vocabulary must remain in **1:1 correspondence** via the registry.

Invalid selection behaviour:

- Invalid preset names (CLI/UI) are input errors; tools should fail clearly and UI should refuse selection.
- Invalid preset enum values at core (corruption/mismatch) must fall back deterministically to a default preset.

------------------------------------------------------------------------

## 4. Spec updates and `apply_mode`

Spec updates follow a single API pattern:

- `update_camera_spec(camera_id, patch, apply_mode=...)`
- `update_imaging_spec(patch, apply_mode=...)`

### `ApplyMode`

- `APPLY_WHEN_SAFE` (default for release): accept and apply when it is safe to do so.
- `APPLY_NOW` (advanced/testing): attempt to apply immediately; may fail if policy forbids or if a safe condition cannot be met.

"Safe" is defined by CamBANG policy and generally means that affected
devices are not actively engaged with hardware resources and there is no
in-flight synchronised capture that depends on them.

The precise definition of "safe" is governed by core arbitration and
warm scheduling policy as defined in core_runtime_model.md.

------------------------------------------------------------------------

## 5. Published snapshot and introspection

CamBANG exposes a read-only, immutable snapshot that reflects the
current truth of the core and its owned native objects.

### `CamBANGStateSnapshot`

Immutable, point-in-time snapshot published by core to `CamBANGServer`,
and exposed for polling/inspection.

### Snapshot record types

Records inside the snapshot that correspond to user-facing objects:

- `CamBANGRigState`
- `CamBANGDeviceState`
- `CamBANGStreamState`

Native/core objects created by the provider on behalf of CamBANG are tracked as registry records:

- `NativeObjectRecord`

### Publication counters

These counter names are **preserved**, but their definitions are from the
**Godot-facing tick-bounded** perspective (observable truth):

- `gen`: zero-indexed and advances by +1 on each successful
  `CamBANGServer.start()` that transitions from stopped → running.
  For each new `gen`, `version` and `topology_version` reset to 0.

- `version`: increments by +1 for each emitted `state_published(...)` within the
  current `gen`. It is contiguous (no gaps).

- `topology_version`: increments by +1 only on ticks where the observed topology
  differs from the topology at the previous emission. It never changes without
  `version` also changing.

### Timestamp fields and time domains

CamBANG uses multiple timestamp *domains* for different purposes. Field names must
make the domain and units unambiguous.

**Snapshot publish time (schema v1)**

- `CamBANGStateSnapshot.timestamp_ns` is a **monotonic publish timestamp** produced by core at snapshot assembly time.
- It is **generation-relative** (monotonic since a core-defined epoch, e.g. core loop start for the current `gen`).
- It is **not wall-clock** and must not be interpreted as UNIX epoch time.

**Capture time (provider → core frame metadata)**

Providers must tag frames with a provider-agnostic capture timestamp representation:

- `CaptureTimestamp.value` (integer ticks)
- `CaptureTimestamp.tick_ns` (tick period in nanoseconds; e.g. `1` for ns, `100` for 100ns)
- `CaptureTimestamp.domain` (declared semantics / comparability)

The `domain` is semantic and provider-agnostic. v1 domains:

- `PROVIDER_MONOTONIC` — monotonic and comparable across streams produced by this provider instance.
- `CORE_MONOTONIC` — already mapped into core's monotonic timebase (generation-relative).
- `DOMAIN_OPAQUE` — ordering-only; provider cannot guarantee meaningful cross-stream comparability.

**Provider boundary rule**

Platform/provider-specific timestamp concepts (e.g. Media Foundation sample time,
Android camera timestamp source enums) must remain **provider-internal** and must not
appear in core/shared types, schema fields, or generic logs. Prefer generic names
like `capture_timestamp`, `tick_ns`, and `domain`.

### Signals

- `state_published(gen, version, topology_version)` --- emitted by `CamBANGServer` when a new snapshot is published.

## Core structural nouns

Within the CamBANG core architecture the following nouns have
canonical meanings.

| Term | Meaning |
|-----|--------|
| **Provider** | Backend implementation controlling a camera API |
| **Device** | Opened camera hardware instance |
| **Stream** | Configured capture pipeline producing frames |

These terms are provider-agnostic abstractions.

Platform terminology such as:

- session
- reader
- pipeline
- track

remains provider-internal.

### Godot-facing naming

Godot-facing classes are prefixed with `CamBANG` to avoid collisions
with generic engine terminology.

Examples:

| Core concept | Godot class |
|-------------|-------------|
| Device | `CamBANGDevice` |
| Stream | `CamBANGStream` |
| Rig | `CamBANGRig` |

### Native object registry naming

Native object registry types use the same canonical nouns:

- `Provider`
- `Device`
- `Stream`
- `FrameProducer`

These names represent ownership structure rather than specific
platform objects.

------------------------------------------------------------------------

## 6. Lifecycle `phase` vs operational `mode`

To avoid ambiguity, CamBANG uses separate fields for lifecycle and
operational posture.

### `phase` (lifecycle)

Lifecycle phases refer only to existence and teardown:

- `CREATED`
- `LIVE`
- `TEARING_DOWN`
- `DESTROYED`

### `mode` (operational posture)

Operational mode describes what an entity is currently doing.

Rig mode examples:

- `OFF`, `ARMED`, `TRIGGERING`, `COLLECTING`, `ERROR`

Device mode examples:

- `IDLE`, `STREAMING`, `CAPTURING`, `ERROR`
- `engaged: bool` indicates whether underlying camera resources are currently held.

Stream mode examples:

- `STOPPED`, `FLOWING`, `STARVED`, `ERROR`

`StreamIntent` defines purpose (`PREVIEW` or `VIEWFINDER`) separately
from operational mode.

`NativeObjectRecord` always has a `phase`. Native objects do not expose an operational `mode` in schema v1.

### Provider mode (capture origin)

The core capture origin is selected via:

`provider_mode`

Allowed values:

- `platform_backed`
- `synthetic`

Exactly one provider instance is bound to Core at core.

The term `hardware` must not be used as a core mode alias in code, documentation, or configuration.  
(Use `hardware_*` only for identity/spec concepts such as `hardware_id` and hardware-reported specifications.)

Behavioural semantics and determinism guarantees are defined in `provider_architecture.md`.

### Synthetic configuration axes

When `provider_mode = synthetic`, two additional configuration axes apply:

`synthetic_role`

Allowed values:

- `nominal`
- `timeline`

`timing_driver`

Allowed values:

- `virtual_time`
- `real_time`

These axes are orthogonal and define behavioural intent and time-advancement semantics for SyntheticProvider.

Behavioural semantics and determinism guarantees are defined in `provider_architecture.md`.

------------------------------------------------------------------------

## 7. Identity and lineage

To support reliable diagnostics (including "old teardown + new live"
scenarios), CamBANG separates stable hardware identity from core
instance identity.

- `hardware_id`: stable platform camera identifier (as reported by the platform backend).
- `instance_id`: monotonic core identifier for a specific opened lineage of that hardware.
- `root_id`: lineage root identifier used for grouping branches.

Detached branches are those no longer attached to the active hierarchy
but still present due to teardown or retention policies.

------------------------------------------------------------------------

## 8. Internal naming (for contributors)

These names appear in the native/core implementation, build system, and
debugging logs.

- `ICameraProvider`: platform backend interface (e.g., Android camera2 provider, stub provider, synthetic provider).
- `CBLifecycleRegistry`: tracks CamBANG-owned native/core object lifecycles and retains recently destroyed records for inspection.
- `CBStatePublisher`: assembles and publishes `CamBANGStateSnapshot` and performs retention sweeps.

- Timestamp conventions:
  - Use suffixes to encode units: `_ns`, `_ms`, `_us`, `_100ns`, etc.
  - Use `capture_` prefix for per-frame capture time and keep it distinct from snapshot publish time.
  - Do not use provider/platform prefixes (e.g. `mf_`, `camera2_`) outside provider code; translate to provider-agnostic `CaptureTimestamp` at the provider boundary.

------------------------------------------------------------------------

## 9. Glossary (quick reference)

- **Spec**: hardware-reported truth (with optional user corrections).
- **Config**: user intent (choices made by the developer/app).
- **Capture Profile**: fidelity definition for image production (streams and stills).
- **PictureConfig**: per-stream picture appearance configuration, distinct from Capture Profile (structural capture properties).
- **Snapshot**: immutable published record of current truth.
- **Phase**: lifecycle stage of an entity/native object.
- **Mode**: operational posture of an entity.
- **Detached**: a branch no longer attached to the active tree but still present due to teardown/retention.
- **StreamIntent**: purpose of a repeating stream (`PREVIEW` or `VIEWFINDER`).
