# CamBANG State Snapshot (Schema v1)

This document specifies the **public** snapshot payload published by
CamBANG core and exposed via `CamBANGServer` as `CamBANGStateSnapshot`.

The snapshot is designed for: - Deterministic diagnostics and
introspection - Safe, lock-free polling from multiple Godot nodes -
Visualisation tooling (status UIs), without embedding any UI-specific
hierarchy logic

------------------------------------------------------------------------

## 1. Snapshot fundamentals

### 1.1 Immutability

A published `CamBANGStateSnapshot` is **immutable**. Readers may keep
references to old snapshots safely.

### 1.2 Publication

Core publishes a new snapshot whenever relevant state changes.
`CamBANGServer` stores the most recently published snapshot and emits:

-   `state_published(gen, version, topology_version)`

**Godot-facing truth model (tick-bounded)**

The snapshot *contract* exposed to Godot is **tick-bounded observable truth**:

- `CamBANGServer.state_published(...)` is emitted **at most once per Godot tick**.
- It is emitted **only if** there has been any change in the observable snapshot
  since the previous tick.
- Core/native may publish intermediate transient states faster than ticks.
  Those intermediate states are **not** part of the Godot-facing contract.
  The Godot boundary coalesces them into a single per-tick observable snapshot.

### Godot Boundary Contract Reference

The observable behaviour of the Godot-facing runtime boundary
(start/stop semantics, generation baseline publication, snapshot
immutability, and tick-bounded publication) is consolidated in: docs/architecture/godot_boundary_contract.md


That document describes the **external behavioural guarantees** of the
Godot boundary without exposing internal provider lifecycle phases.

### 1.2.x Pre-publication state

Before the first publish in a new core generation (`gen`), no snapshot exists.

Godot-facing `CamBANGServer.get_state_snapshot()` returns **NIL** until the
first snapshot is published.

The first published snapshot establishes the baseline state for the
session (see §1.3).

### 1.2.y Host pause behaviour (observable effects)

Host environments such as Godot may temporarily suspend frame ticks
(e.g. when the application is paused).

Because the Godot-facing publication model is tick-bounded:

- `state_published(...)` emissions occur **only when the host tick
  executes**.
- No emissions occur while the host is paused.

Core and providers may continue to integrate internal state changes
during this time (depending on provider timing model), but these changes
are not observable until the next tick.

When ticks resume:

- `CamBANGServer` observes the latest available snapshot.
- If observable state has changed since the last emission, a single
  `state_published(gen, version, topology_version)` event is emitted.

This means:

- Multiple internal state transitions may be **coalesced** into a single
  observable snapshot when a pause ends.
- Snapshot counters (`version`, `topology_version`) reflect the
  Godot-visible publication model, not the number of internal state
  transitions.

SyntheticProvider pause semantics depend on the selected timing driver
(see `core_runtime_model.md`).


### 1.3 Counters (`gen`, `version`, `topology_version`)

CamBANG exposes three counters in the snapshot header.

These counters are defined from the **Godot-facing tick-bounded perspective** (not
from core-internal publication mechanics):

- `gen` — zero-indexed generation counter. Advances by **+1** on each successful
  `CamBANGServer.start()` that transitions from **stopped → running**.

- `version` — zero-indexed **tick-bounded publication** counter within the current
  `gen`. It increments by **+1** for each emitted `state_published(...)` signal
  within that `gen`. It is **contiguous** (no gaps).

- `topology_version` — zero-indexed **tick-bounded structural** counter within the
  current `gen`. It increments by **+1** only on ticks where the **observed topology
  differs** from the topology at the previous emission.

  It never changes without `version` also changing.

Baseline invariants (per `gen`):

- The first published snapshot in a new `gen` has `version = 0`.
- The first published snapshot in a new `gen` has `topology_version = 0`.

There is no published snapshot prior to `version = 0` for a given `gen`.

### 1.3.x Baseline publish on start (Godot-facing)

On a successful start that creates a new `gen`, there must be a valid coherent
snapshot corresponding to:

- `(gen, version=0, topology_version=0)`

and a `state_published(gen, 0, 0)` emission must occur at the **first Godot tick
where the running state becomes observable**.

This remains true even if core publishes before the Godot tick node is active:
the Godot boundary must latch the baseline snapshot and emit it on the first
eligible tick.

Authoritative baseline content is allowed to be **provider-only**.

In particular, if the provider has already been attached/initialized for the new
generation but no device has yet been opened and no stream has yet been created,
the first published baseline may legitimately contain:

- one current-generation provider native object
- zero devices
- zero streams
- no frameproducer subtree

This is a contract-valid transient of startup/restart truth, not an incomplete
or fabricated snapshot.

Its duration is intentionally **fact-driven**, not time-seeded:

- it may remain the latest observable snapshot for multiple host ticks if no
  device-open / stream-create / start-stream facts are published yet
- it may transition directly to `NIL` on completed stop
- it may repeat across later generations under restart churn

In scenarios that explicitly realize descendants, the expected next observable
transitions are:

- provider-only → device realized (`version += 1`, `topology_version += 1`)
- device realized → acquisition session realized (`version += 1`,
  `topology_version += 1`) when provider truth first realizes an
  `AcquisitionSession` seam for that device (for example through stream-backed
  realization or a capture-only path)
- acquisition session realized → stream realized (`version += 1`,
  `topology_version += 1`)
- started/producing seam → frameproducer realized (`version += 1`,
  `topology_version` may remain unchanged if the observable topology signature
  does not change); this seam may be stream-owned or acquisition-session-owned
  for still-capture production when truthfully realized

### 1.3.y Snapshot access contract (`get_state_snapshot()`)

`CamBANGServer.get_state_snapshot()` remains **parameter-less**.

- Inside the synchronous `state_published` handler, it returns the snapshot
  corresponding to that emission (self-consistent with the signal arguments).
- Outside the handler, it returns the most recently latched snapshot (latest
  observable truth).
- 
### 1.3.z Stop/start boundary contract

`CamBANGServer.get_state_snapshot()` exposes the latest published snapshot for
the currently active generation.

After a completed `CamBANGServer.stop()`:

- there is no active published snapshot
- `CamBANGServer.get_state_snapshot()` returns `NIL`

After a subsequent successful `CamBANGServer.start()`:

- a new `gen` is created only after the prior generation has completed stop/teardown
- `CamBANGServer.get_state_snapshot()` remains `NIL` until the first published
  baseline snapshot of the new generation
- no stale snapshot from the prior generation may remain visible through the
  parameter-less `get_state_snapshot()` API

Teardown truth for the previous generation must be exposed by that generation's
final published snapshot(s), not by leaking prior-generation state into the next
generation's pre-baseline window.

### 1.4 Schema versioning

The snapshot includes a `schema_version` field so tooling can validate
compatibility.

------------------------------------------------------------------------

## 2. Lifecycle `phase` vs operational `mode`

### 2.1 Lifecycle `phase`

Lifecycle phases describe existence and teardown only:

-   `CREATED` --- allocated/registered; may not yet be established.
-   `LIVE` --- established and operationally ready.
-   `TEARING_DOWN` --- teardown in progress.
-   `DESTROYED` --- destroyed; record may be retained temporarily for
    diagnostics.

`phase` in all record schemas refers to this lifecycle enum:
CREATED | LIVE | TEARING_DOWN | DESTROYED.

### 2.2 Operational `mode`

Operational mode describes what an entity is currently doing
(entity-specific).

Examples: - Rig: `OFF`, `ARMED`, `TRIGGERING`, `COLLECTING`, `ERROR` -
Device: `IDLE`, `STREAMING`, `CAPTURING`, `ERROR` - Stream: `STOPPED`,
`FLOWING`, `STARVED`, `ERROR`

`NativeObjectRecord` always has a `phase`. Native objects do not expose an operational `mode` in schema v1.

------------------------------------------------------------------------

## 3. Identity and lineage

CamBANG separates stable hardware identity from instance
identity to support "old teardown + new live" scenarios.

-   `hardware_id` --- stable platform identifier for a camera endpoint.
-   `instance_id` --- monotonic instance identifier for an opened lineage
    of that hardware.
-   `root_id` --- lineage root identifier for grouping native object
    branches across time.

------------------------------------------------------------------------

## 4. Pixel format encoding (FourCC)

Pixel formats in the snapshot are encoded as a **FourCC-style**
`uint32`.

A FourCC is a 32-bit integer typically represented as four ASCII
characters, e.g. `'NV12'`, `'I420'`, `'RGBA'`.

CamBANG defines a **canonical set** of pixel format codes and each
platform provider maps platform-native formats into this set.

**v1 policy**: - Repeating streams (`PREVIEW`, `VIEWFINDER`) are
**raw-only** formats (no compressed JPEG/MJPG). - Triggered capture
profiles may include formats such as `'JPEG'` and `'RAW '` (the latter
may not be implemented on all platforms in v1, but is part of the
design).

------------------------------------------------------------------------

## 5. Top-level schema (v1)

``` text
CamBANGStateSnapshot {
  schema_version: uint32           // = 1
  gen: uint64                      // core generation (monotonic across app/server lifetime)
  version: uint64                  // increments every publish within this gen
  topology_version: uint64         // increments on structural change within this gen
  timestamp_ns: uint64             // monotonic publish time

  imaging_spec_version: uint64     // effective ImagingSpec version

  rigs: Array<CamBANGRigState>
  devices: Array<CamBANGDeviceState>
  acquisition_sessions: Array<AcquisitionSessionState>
  streams: Array<CamBANGStreamState>

  native_objects: Array<NativeObjectRecord>

  detached_root_ids: Array<uint64> // computed by core (see §7)
}
```
### 5.1 `acquisition_sessions` truth scope (v1)

`acquisition_sessions` is a top-level snapshot category in schema version **1**.

Current v1 meaning is intentionally narrow and implementation-synchronized:

- `acquisition_sessions[]` reports **current/live retained AcquisitionSession truth** for the active generation.
- It is **not** the destroyed-history archive for prior sessions.
- Destroyed/retained lifecycle history remains diagnosable through `native_objects` retention.

This distinction is important for diagnostics and panel projection: live session truth
comes from top-level `acquisition_sessions`, while historical teardown ordering remains
visible through retained `NativeObjectRecord` entries.

### 5.x Effective retained truth (normative)

Certain snapshot fields represent **effective retained truth** within Core.

In particular:

- `imaging_spec_version`
- `camera_spec_version` (per device)

These fields report the specification version that Core currently
retains as the **effective runtime specification**.

They do **not** represent:

- the most recently requested specification version
- a provider default
- an implied bootstrap value
- a speculative or not-yet-applied configuration

The values therefore follow these rules:

- `0` means **no effective specification is currently retained**.
- A **non-zero value** means a real specification has been accepted by
  Core and is currently retained as the active runtime specification.

Snapshot publication must **never fabricate bootstrap or placeholder
values** for these fields. The snapshot must reflect the actual retained
state held by Core at the time of publication.

More generally, snapshot fields represent **runtime truth retained by
Core at publication time**. They do not report speculative, inferred,
or not-yet-effective state.

### 5.x Timestamp semantics (normative)

`timestamp_ns` is a **monotonic publish timestamp** produced by core at snapshot assembly time.

- It is **not wall-clock** and must not be interpreted as UNIX epoch time.
- It is **generation-relative** (a monotonic counter since a core-defined epoch for the current `gen`, e.g. core loop start).
- It is intended for: ordering snapshots, computing durations (e.g. warm remaining), and
  detecting staleness — not for user-facing clock time.

Snapshot publication occurs only after core state has converged for the
current event loop iteration (see `core_runtime_model.md`).

On successful core loop start, core begins in a logically dirty state and
will publish a baseline snapshot on the first loop iteration. This first
published snapshot has:

- `gen = 0`
- `topology_gen = 0`
- a valid monotonic `timestamp_ns`

This guarantees that each core generation (`gen`) produces at least one
deterministic baseline snapshot after `CamBANGServer.start()` completes.
------------------------------------------------------------------------

## 6. Record schemas (v1)

### 6.0 `profile_version` / `capture_profile_version`

- Monotonic counter incremented when the applied capture profile changes.
- Represents change lineage only.
- Must not be used to infer configuration contents.

### 6.1 `CamBANGRigState`

`capture_width`, `capture_height`, and `capture_format` form part of the applied still capture
profile for this rig.

``` text
CamBANGRigState {
  rig_id: uint64
  name: String

  phase: phase
  mode: OFF | ARMED | TRIGGERING | COLLECTING | ERROR

  member_hardware_ids: Array<String>

  active_capture_id: uint64              // 0 if none

  capture_profile_version: uint64        // monotonic change lineage for the applied still capture profile
  capture_width: uint32
  capture_height: uint32
  capture_format: uint32                 // FourCC-style CamBANG pixel format

  captures_triggered: uint64
  captures_completed: uint64
  captures_failed: uint64

  last_capture_id: uint64                // 0 if none
  last_capture_latency_ns: uint64        // 0 if unknown
  last_sync_skew_ns: uint64              // 0 if unknown

  error_code: int32                      // 0 if none
}
```

### 6.2 `CamBANGDeviceState`

`capture_width`, `capture_height`, and `capture_format` form part of the applied still capture
profile for this device.

``` text
CamBANGDeviceState {
  hardware_id: String
  instance_id: uint64

  phase: phase
  mode: IDLE | STREAMING | CAPTURING | ERROR

  engaged: bool                          // holds underlying camera resources

  rig_id: uint64                         // 0 if not a rig member

  camera_spec_version: uint64            // effective CameraSpec version
  capture_profile_version: uint64        // monotonic change lineage for the applied still capture profile
  capture_width: uint32
  capture_height: uint32
  capture_format: uint32                 // FourCC-style CamBANG pixel format

  warm_hold_ms: uint32                   // 0 = full teardown immediately
  warm_remaining_ms: uint32              // 0 if not warming

  rebuild_count: uint64
  errors_count: uint64
  last_error_code: int32                 // 0 if none
}
```

### 6.3 `AcquisitionSessionState` (provisional tranche-1 shape)

`AcquisitionSessionState` is introduced as a first-class snapshot category for
acquisition-session truth. This tranche intentionally uses a provisional field
shape borrowed from the existing still-capture summary vocabulary. Field
residency is not final long-term approval in this tranche.

Boundary direction remains:

- Device remains the hardware/resource posture boundary.
- Stream remains the repeating-flow boundary.
- AcquisitionSession is the acquisition-session truth boundary.

`AcquisitionSession` is runtime/provider-originated truth and is not a directly
scenario-authored object.

Implementation-status clarification (current repo truth):

- `AcquisitionSession` is runtime/provider-originated truth and is not a directly
  scenario-authored object.
- `SyntheticProvider` now realizes truthful `AcquisitionSession` state for both
  stream-backed and capture-only paths.
- Capture activity/counters in `AcquisitionSessionState` reflect retained session
  truth when a live session seam exists.
- Still capture callbacks alone must not be treated as retained AcquisitionSession
  realization in core state when no concrete session seam has been realized.

``` text
AcquisitionSessionState {
  acquisition_session_id: uint64
  device_instance_id: uint64

  phase: phase

  capture_profile_version: uint64
  capture_width: uint32
  capture_height: uint32
  capture_format: uint32

  captures_triggered: uint64
  captures_completed: uint64
  captures_failed: uint64

  last_capture_id: uint64
  last_capture_latency_ns: uint64

  error_code: int32
}
```

### 6.4 `CamBANGStreamState`

A repeating stream. Each device supports at most **one active repeating
stream** at a time (design choice).

``` text
CamBANGStreamState {
  stream_id: uint64
  device_instance_id: uint64

  phase: phase
  intent: PREVIEW | VIEWFINDER
  mode: STOPPED | FLOWING | STARVED | ERROR

  stop_reason: NONE | USER | PREEMPTED | PROVIDER // meaningful when mode=STOPPED

  profile_version: uint64                // monotonic change lineage for the applied stream capture profile

  width: uint32
  height: uint32
  format: uint32                         // FourCC-style CamBANG pixel format

  target_fps_min: uint32                 // 0 if unspecified/not applicable
  target_fps_max: uint32                 // 0 if unspecified/not applicable

  frames_received: uint64
  frames_delivered: uint64
  frames_dropped: uint64
  queue_depth: uint32

  last_frame_ts_ns: uint64               // 0 if none
  visibility_frames_presented: uint64
  visibility_frames_rejected_unsupported: uint64
  visibility_frames_rejected_invalid: uint64
  visibility_last_path:
    NONE | RGBA_DIRECT | BGRA_SWIZZLED | REJECTED_UNSUPPORTED | REJECTED_INVALID
}
```
**Field semantics (v1):**

- `width`, `height`, `format`, `target_fps_min`, and `target_fps_max` form part of
  the applied capture profile for this stream.
- `profile_version` is change lineage metadata for that applied profile and must
  not be used to infer configuration contents.
- `frames_received` counts frames reported by the provider and integrated by core.
- `frames_delivered` counts frames successfully handed to the core frame sink
  (e.g., latest-frame mailbox in v1). This does not imply consumption by Godot.
- `frames_dropped` counts frames dropped due to queue pressure or shutdown gating.
- `queue_depth` reflects provider → core ingress buffering depth as observed
  by core at publish time.
- `visibility_frames_presented` counts frames accepted by the current visibility path
  and retained for visibility/output.
- `visibility_frames_rejected_unsupported` counts visibility-path rejections caused by
  unsupported frame format for that path.
- `visibility_frames_rejected_invalid` counts visibility-path rejections caused by
  invalid payload/shape/metadata for presentation.
- `visibility_last_path` records the most recent retained visibility-path disposition.
  It remains `NONE` until authoritative visibility-path truth exists.

**Invariant (v1):** - At most one stream per `device_instance_id` may be
`phase=LIVE` and `mode != STOPPED`.

### Context placement for resource-bearing native truth

CamBANG distinguishes structural context from additional provider-owned
resource-bearing native truth.

Boundary direction remains:

- Device remains the hardware/resource posture boundary.
- Stream remains the repeating-flow boundary.
- AcquisitionSession remains the acquisition-session truth boundary.

For additional provider-owned native resources whose lifetime matters beyond
ordinary parent destruction:

- **stream-originated** resource-bearing native truth is interpreted beneath
  the owning `Stream` context
- **capture-originated** resource-bearing native truth is interpreted beneath
  the owning `AcquisitionSession` context

This rule applies whether or not a `FrameProducer` row is present.

The snapshot does not require a public-object analogue for such native truth.
Native placement and interpretation are based on provider-owned runtime truth,
not on 1:1 parity with public Godot-facing objects.

### 6.5 `NativeObjectRecord`

Native/core objects created by the provider on behalf of CamBANG
are tracked as registry records.

``` text
NativeObjectRecord {
  native_id: uint64
  type: provider | device | acquisition_session | stream | frameproducer

  phase: phase

  owner_device_instance_id: uint64       // 0 if none/unknown
  owner_acquisition_session_id: uint64   // 0 if none/unknown
  owner_stream_id: uint64                // 0 if none/unknown
  owner_provider_native_id: uint64       // 0 if none/unknown
  owner_rig_id: uint64                   // 0 if none/unknown

  root_id: uint64

  creation_gen: uint64                // snapshot `gen` when this record was created

  created_ns: uint64
  destroyed_ns: uint64                   // 0 if not DESTROYED

  bytes_allocated: uint64                // 0 if not applicable
  buffers_in_use: uint32                 // 0 if not applicable
}
```

Ownership interpretation note:

- Native `Stream` means a truthfully realized provider/native stream-like resource.
- Native `FrameProducer` means a truthfully realized frame-production seam.
- `FrameProducer` ownership may be represented as either:
  - `owner_stream_id != 0`, or
  - `owner_acquisition_session_id != 0` with `owner_stream_id = 0`.
- For resource-bearing native truth associated with repeating flow,
  `owner_stream_id != 0` is the primary placement signal.
- For capture-originated resource-bearing native truth with no repeating-stream
  context, `owner_acquisition_session_id != 0` is the primary placement signal.
- `FrameProducer` presence is not required for either placement rule.
- Native-object truth is not constrained to 1:1 public-object parity.
- Additional provider-owned native resources may also surface through
  `native_objects` when their lifetimes matter for ownership diagnostics,
  leak prevention, queue health, teardown correctness, or
  retained-result/backing-resource truth.

## Lifecycle registry truth model

The lifecycle registry records the **actual lifecycle of native
objects owned by providers**.

Registry entries represent **observed reality**, not intended state.

Core does not automatically cascade destruction of child objects
when a parent object is destroyed.

This allows the snapshot system to reveal important diagnostics
including:

- leaked objects
- delayed teardown
- unexpected lifetime extension
- provider lifecycle ordering bugs

If a native object survives the destruction of its logical parent
it will appear as a **detached root** in the snapshot.

Detached roots are expected during debugging and are valuable
indicators of lifecycle problems.

Providers must therefore emit destruction events **only when the
resource has actually been released**.
------------------------------------------------------------------------

## 7. Detached roots (`detached_root_ids`)

`detached_root_ids` is computed by core and included as a convenience
index for tooling.

A `root_id` is included if: - its controlling owner has ended, **and** -
at least one `NativeObjectRecord` with that `root_id` still exists in
the registry (either not DESTROYED, or DESTROYED but still retained).

“Controlling owner” refers to the AcquisitionSession, device instance, or stream
that originally owned the native object branch associated with the
root_id.

If descendants survive past a dead controlling AcquisitionSession, this must be
treated as an **Acquisition Session boundary breach** in diagnostics and not
collapsed into generic orphan meaning.

------------------------------------------------------------------------

## 8. What increments `gen` vs `version` vs `topology_version`

### 8.0 `gen`

`gen` increments only when `CamBANGServer.start()` successfully begins a new
running session after a complete stop/teardown.

`gen` is defined from the Godot-facing perspective (tick-bounded observation),
but aligns 1:1 with core generations because `start()` is the authoritative
boundary operation.

A generation is not considered closed merely because stop has been requested.
A new generation must not begin until deterministic shutdown of the prior
generation has completed.

For this purpose, "complete stop/teardown" means that no prior-generation live
runtime state remains active at the Godot-facing boundary. Retained diagnostic
records for already-destroyed objects do not by themselves keep the prior
generation open.

### 8.1 `version`

`version` increments on any **tick-bounded observable** snapshot publish
(i.e., any `state_published(...)` emission), including:

- `phase` changes
- `mode` changes
- counter updates
- error updates
- spec/profile updates becoming effective
- retirement sweep removals (because snapshot contents change)

### 8.2 `topology_version`

`topology_version` increments only on ticks where the **observed topology differs
from the topology at the previous emission**, including cases such as:

- rig created/destroyed
- device instance created/destroyed (`instance_id` lineage changes)
- stream created/destroyed
- rig membership changes
- a new `root_id` appears or a `root_id` fully disappears (including detached branches expiring)

The initial publish of a new `gen` establishes the baseline topology and sets `topology_version = 0`.

The baseline is not considered a "change from -1"; it is the first structural state of the generation.

------------------------------------------------------------------------

## 9. Retention and retirement sweep

-   `DESTROYED` native object records are retained for a fixed retention
    window to support diagnostics.
- Destroyed-record retention is a diagnostic facility, not a reason to keep the
  active generation open indefinitely. Retained `DESTROYED` records may remain
  available within the prior generation's final published truth, but they must not
  cause prior-generation state to appear as the live baseline of a later
  generation.

-   A retirement sweep removes expired retained records.
  
- **If the sweep removes any records while the generation is still active,
a new snapshot must be published.**

A generation must not be considered fully stopped/closed until any final
retention-sweep-driven snapshot changes required for that generation have
been published.

After completed stop/teardown, the Godot-facing `get_state_snapshot()`
returns `NIL` until the first publish of the next generation.

## Warm Retention Semantics

`warm_hold_ms`  
: Effective warm-retention policy currently applied to the device.

`warm_remaining_ms`  
: Remaining time in the active warm-retention grace interval at the moment the snapshot was published.

Rules:

- `warm_hold_ms = 0` when no warm retention policy is active.
- `warm_remaining_ms = 0` when no warm grace interval is active.
- Non-zero `warm_remaining_ms` implies the core currently owns a valid warm deadline.
