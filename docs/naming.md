# CamBANG Naming and Terminology

This document defines the canonical names used by CamBANG in its public
Godot-facing API, configuration concepts, lifecycle/native truth surfaces,
and diagnostics/introspection.

It is intentionally broader than a simple glossary. In addition to naming,
it preserves compact semantic guardrails where terminology drift would
otherwise create architecture or implementation confusion.

------------------------------------------------------------------------

## 0. Terminology guardrail: scenario vs verification case

To prevent cross-layer ambiguity:

- **Scenario** is reserved for the user-facing / Godot / SyntheticProvider
  timeline concept (including timeline recording, storage, and execution
  discussions).
- **Scenario library** is the umbrella term for scenario collections.
  - **Built-in scenario library**: current C++-authored scenario set.
  - **External scenario library**: future file-backed/user-provided scenario collections.
  - **Scenario loader**: future serialized ingestion path for external scenario libraries.
- **Verification case** is the canonical prose term for maintainer-authored
  smoke/CLI validation inputs.
- Code identifiers may use compact `verify_case` forms for smoke tooling, but
  contributor-facing prose should prefer the full phrase **verification case**.
- **Verification case catalog** is the maintainer tooling term for verification-case
  selection/listing surfaces (for example `verify_case_catalog`).
- **Fixture** remains a separate concept: a prepared input artifact/dataset used
  by tests or tooling. A fixture may be consumed by a verification case, but the
  terms are not interchangeable.

This distinction is important because scenario semantics belong to
SyntheticProvider timeline execution, while verification cases are
maintainer-authored validation procedures.

------------------------------------------------------------------------

## 1. Godot-facing API objects

### CamBANG prefix rule (normative)

`CamBANG*` naming is reserved for **Godot-facing API classes**.

Internal/core/schema/runtime record types must use internal/shared names
without adding a `CamBANG` prefix. For AcquisitionSession-related truth this
includes canonical terms such as:

- `AcquisitionSession`
- `AcquisitionSessionState`
- `acquisition_sessions`
- `acquisition_session_id`
- `owner_acquisition_session_id`

This naming rule does **not** imply approval of a new Godot-facing
`CamBANGAcquisitionSession*` class. Variant-compatible snapshot data remains the
default Godot-facing representation unless explicitly documented otherwise.

### `CamBANGServer`

Singleton service/registry/factory for CamBANG.

Responsibilities include:

- runtime start / stop
- access to the latest published snapshot
- provider selection/startup surface
- global lifecycle management
- singleton-hosted Godot-facing signal emission

Observable boundary contract:

- `CamBANGServer.start()`
- `CamBANGServer.stop()`
- `CamBANGServer.get_state_snapshot()`
- `signal state_published(gen, version, topology_version)`

`get_state_snapshot()` returns `NIL` before the first baseline publish of
a generation, and again after a completed stop.

The full behavioural contract for the Godot-facing runtime boundary
(start semantics, restart generation rules, snapshot visibility, and
tick-bounded publication) is documented in:
`docs/architecture/godot_boundary_contract.md`.

### `CamBANGRig`

User-created multi-camera coordinator used to perform synchronised
capture across multiple devices.

Primary lifecycle controls:

- `arm()` / `disarm()`
- `trigger_sync_capture()`

Rig-triggered sync capture has priority over standalone activity on
member devices when conflicts arise.

### `CamBANGDevice`

Represents one camera hardware endpoint (physical or logical, depending
on platform).

Primary lifecycle controls:

- `engage()` / `disengage()`
- `set_warm_policy(...)`
- `set_still_capture_profile(profile)`
- `trigger_capture()`

`CamBANGDevice` is the public Godot-facing control point for device-level
still capture. That public surface does not imply any required 1:1 parity
with provider-internal or native-object execution details.

### `CamBANGStream`

A repeating frame output channel associated with a specific device
instance.

Primary controls:

- `start()` / `stop()`

Each device supports at most one active repeating stream at a time
(i.e. a stream with `phase=LIVE` and `mode != STOPPED`).

Streams are created with a `StreamIntent`:

- `PREVIEW`
- `VIEWFINDER`

`CamBANGStream` is a public/runtime-visible user-semantic object.
It must not be conflated with every native/provider stream-like resource
that may appear in native-object truth.

------------------------------------------------------------------------

## 2. Capture Profiles

CamBANG uses the term **Capture Profile** to describe fidelity and
throughput configuration for image production. The valid fields of a
profile depend on intent (e.g. repeating stream vs still capture).

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

Picture/source appearance configuration.

`PictureConfig` represents user intent regarding how frames should
appear, independent of structural capture properties.

It is distinct from **Capture Profile**:

- Capture Profile defines structural capture properties such as resolution, pixel format, and frame rate range.
- PictureConfig defines picture/source appearance parameters.

For synthetic and stub providers, `PictureConfig` describes
pattern-generation parameters. Platform-backed providers may interpret
`PictureConfig` as picture adjustment parameters subject to capability.

`PictureConfig` may also include picture/source generation controls such as
generator cadence (for example `generator_fps_num` / `generator_fps_den`)
used to determine the synthetic source-frame ordinal supplied to the renderer.

Picture/source appearance intent is distinct from storage/backing capability.
For example, choosing a pattern preset or generator cadence does not by itself
assert that CPU-backed or GPU-backed realization is available.

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
warm scheduling policy as defined in `core_runtime_model.md`.

------------------------------------------------------------------------

## 5. Published snapshot and introspection

CamBANG exposes a read-only, immutable snapshot that reflects the
current truth of the core and its owned native objects.

### `CamBANGStateSnapshot`

Immutable, point-in-time snapshot published by core to `CamBANGServer`,
and exposed for polling/inspection.

### Snapshot record types

Records inside the snapshot that correspond to user-facing/runtime-facing state:

- `CamBANGRigState`
- `CamBANGDeviceState`
- `CamBANGStreamState`
- `AcquisitionSessionState`

Native/core objects created by the provider on behalf of CamBANG are tracked as registry records:

- `NativeObjectRecord`

### Publication counters

These counter names are preserved, but their definitions are from the
Godot-facing tick-bounded perspective (observable truth):

- `gen`
- `version`
- `topology_version`

#### `gen`

Identifies the current runtime generation and advances by +1 on each successful
`CamBANGServer.start()` that transitions from stopped → running.

#### `version`

Counts observable state changes within a generation and increments by +1 on
each emitted `state_published(...)` within that generation.

#### `topology_version`

Counts observable topology changes within a generation and increments only
when the observable structure changes.

When `gen` changes, both `version` and `topology_version` reset to zero for
the new baseline.

### Timestamp fields and time domains

CamBANG uses multiple timestamp domains for different purposes. Field names must
make the domain and units unambiguous.

#### Snapshot publish time (schema v1)

- `CamBANGStateSnapshot.timestamp_ns` is a **monotonic publish timestamp** produced by core at snapshot assembly time.
- It is **generation-relative**.
- It is **not wall-clock** and must not be interpreted as UNIX epoch time.

#### Capture time (provider → core frame metadata)

Providers must tag frames with a provider-agnostic capture timestamp representation:

- `CaptureTimestamp.value`
- `CaptureTimestamp.tick_ns`
- `CaptureTimestamp.domain`

Capture timestamps describe image-capture time, not snapshot publication time.

------------------------------------------------------------------------

## 6. Native object registry naming

Native object registry types use the same canonical structural nouns:

- `Provider`
- `Device`
- `AcquisitionSession`
- `Stream`
- `FrameProducer`

These names represent the preferred CamBANG viewing structure for
lifecycle-significant native truth. They do **not** imply that native truth
is limited only to those categories, and they do **not** promise 1:1 parity
with public Godot-facing objects.

In particular:

- `Stream` in native-object truth means a truthfully realized
  provider/native stream-like resource
- `FrameProducer` means a truthfully realized provider/native
  frame-production seam
- a `FrameProducer` may be owned by a `Stream` or directly by an
  `AcquisitionSession`
- native-object truth may therefore contain `Stream` / `FrameProducer`
  records that do not correspond to user-created `CamBANGStream` objects

This distinction is intentional:

- public Godot-facing API expresses user/runtime semantics
- native-object truth expresses provider-owned lifecycle and resource truth

Additional provider-owned native resources may also surface through
native-object reporting when their lifetime matters for:

- ownership diagnostics
- leak prevention
- queue health
- teardown correctness
- retained-resource truth

The canonical structural nouns are therefore a preferred cross-provider
organizing model, not a limit on what native truth may contain.

------------------------------------------------------------------------

## 7. Structural hierarchy vs native truth breadth

CamBANG imposes a small cross-provider viewing structure for intelligibility.
That structure is useful, but it must not be mistaken for a claim that:

- underlying platform APIs are hierarchical in the same way, or
- only those canonical categories are permitted to surface in native truth

This distinction matters especially for:

- provider-owned resource-bearing native objects
- retained samples / acquired images
- mapped or attached buffers
- short-lived stream-like resources used to service still capture
- other leak-relevant provider-owned native handles or leases

A provider may therefore report native `Stream` or `FrameProducer` truth
without creating a matching public `CamBANGStream`, and may also report
additional provider-owned native resources when their lifetime matters.

------------------------------------------------------------------------

## 8. Synthetic configuration axes

When `provider_mode = synthetic`, two additional configuration axes apply:

### `synthetic_role`

Allowed values:

- `nominal`
- `timeline`

### `timing_driver`

Allowed values:

- `virtual_time`
- `real_time`

These axes are orthogonal and define behavioural intent and
time-advancement semantics for SyntheticProvider.

Behavioural semantics and determinism guarantees are defined in
`provider_architecture.md`.

------------------------------------------------------------------------

## 9. Identity and lineage

To support reliable diagnostics (including "old teardown + new live"
scenarios), CamBANG separates stable hardware identity from core
instance identity.

- `hardware_id`: stable platform camera identifier (as reported by the platform backend).
- `instance_id`: monotonic core identifier for a specific opened lineage of that hardware.
- `root_id`: lineage root identifier used for grouping branches.

Detached branches are those no longer attached to the active hierarchy
but still present due to teardown or retention policies.

------------------------------------------------------------------------

## 10. Internal naming (for contributors)

These names appear in the native/core implementation, build system, and
debugging logs.

- `ICameraProvider`: platform backend interface
- `CBLifecycleRegistry`: tracks CamBANG-owned native/core object lifecycles and retains recently destroyed records for inspection
- `CBStatePublisher`: assembles and publishes `CamBANGStateSnapshot` and performs retention sweeps

### Timestamp conventions

- Use suffixes to encode units: `_ns`, `_ms`, `_us`, `_100ns`, etc.
- Use `capture_` prefix for per-frame capture time and keep it distinct from snapshot publish time.
- Do not use provider/platform prefixes (e.g. `mf_`, `camera2_`) outside provider code; translate to provider-agnostic `CaptureTimestamp` at the provider boundary.

------------------------------------------------------------------------

## 11. Release-facing image/result naming discipline

Public/runtime-visible image-bearing nouns remain:

- **Stream Result**
- **Capture Result**
- **Capture Result Set**

These are distinct from:

- internal sink vocabulary (`Stream Sink`, `Capture Sink`)
- provider-facing payload vocabulary
- native-object registry categories

The fact that native truth may contain `Stream` / `FrameProducer` records
does not redefine the Godot-facing result/object vocabulary.

------------------------------------------------------------------------

## 12. Glossary (quick reference)

- **Spec**: hardware-reported truth (with optional user corrections).
- **Config**: user intent (choices made by the developer/app).
- **Capture Profile**: fidelity definition for image production (streams and stills).
- **PictureConfig**: picture/source appearance configuration, distinct from Capture Profile and from storage/backing capability.
- **Snapshot**: immutable published record of current truth.
- **Phase**: lifecycle stage of an entity/native object.
- **Mode**: operational posture of an entity.
- **Detached**: a branch no longer attached to the active tree but still present due to teardown/retention.
- **StreamIntent**: purpose of a repeating stream (`PREVIEW` or `VIEWFINDER`).
- **FrameProducer**: optional provider-reported frame-production seam, owned by `Stream` or directly by `AcquisitionSession`.