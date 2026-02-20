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

-   `state_published(gen, topology_gen)`

### 1.3 Generation counters

-   `gen` increments on **every** publish.
-   `topology_gen` increments when the **structural hierarchy** changes
    (see §8).

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

### 2.2 Operational `mode`

Operational mode describes what an entity is currently doing
(entity-specific).

Examples: - Rig: `OFF`, `ARMED`, `TRIGGERING`, `COLLECTING`, `ERROR` -
Device: `IDLE`, `STREAMING`, `CAPTURING`, `ERROR` - Stream: `STOPPED`,
`FLOWING`, `STARVED`, `ERROR`

`NativeObjectRecord` always has a `phase`. A `mode` may be included for
native objects when useful, but is not required in v1.

------------------------------------------------------------------------

## 3. Identity and lineage

CamBANG separates stable hardware identity from runtime instance
identity to support "old teardown + new live" scenarios.

-   `hardware_id` --- stable platform identifier for a camera endpoint.
-   `instance_id` --- monotonic runtime identifier for an opened lineage
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
  gen: uint64                      // increments every publish
  topology_gen: uint64             // increments on structural change
  timestamp_ns: uint64             // monotonic publish time

  imaging_spec_version: uint64     // effective ImagingSpec version

  rigs: Array<CamBANGRigState>
  devices: Array<CamBANGDeviceState>
  streams: Array<CamBANGStreamState>

  native_objects: Array<NativeObjectRecord>

  detached_root_ids: Array<uint64> // computed by core (see §7)
}
```

------------------------------------------------------------------------

## 6. Record schemas (v1)

### 6.1 `CamBANGRigState`

``` text
CamBANGRigState {
  rig_id: uint64
  name: String

  phase: phase
  mode: OFF | ARMED | TRIGGERING | COLLECTING | ERROR

  member_hardware_ids: Array<String>

  active_capture_id: uint64              // 0 if none

  capture_profile_version: uint64        // rig still capture profile version

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

``` text
CamBANGDeviceState {
  hardware_id: String
  instance_id: uint64

  phase: phase
  mode: IDLE | STREAMING | CAPTURING | ERROR

  engaged: bool                          // holds underlying camera resources

  rig_id: uint64                         // 0 if not a rig member

  camera_spec_version: uint64            // effective CameraSpec version
  capture_profile_version: uint64        // device still capture profile version

  warm_hold_ms: uint32                   // 0 = full teardown immediately
  warm_remaining_ms: uint32              // 0 if not warming

  rebuild_count: uint64
  errors_count: uint64
  last_error_code: int32                 // 0 if none
}
```

### 6.3 `CamBANGStreamState`

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

  profile_version: uint64                // stream capture profile version

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
}
```

**Invariant (v1):** - At most one stream per `device_instance_id` may be
`phase=LIVE` and `mode != STOPPED`.

### 6.4 `NativeObjectRecord`

Native/core objects created by CamBANG are tracked as registry records.

``` text
NativeObjectRecord {
  native_id: uint64
  type: uint32                           // CamBANG-defined enum

  phase: phase

  owner_rig_id: uint64                   // 0 if none
  owner_device_instance_id: uint64       // 0 if none
  owner_stream_id: uint64                // 0 if none

  root_id: uint64

  created_ns: uint64
  destroyed_ns: uint64                   // 0 if not DESTROYED

  bytes_allocated: uint64                // 0 if not applicable
  buffers_in_use: uint32                 // 0 if not applicable
}
```

------------------------------------------------------------------------

## 7. Detached roots (`detached_root_ids`)

`detached_root_ids` is computed by core and included as a convenience
index for tooling.

A `root_id` is included if: - its controlling owner has ended, **and** -
at least one `NativeObjectRecord` with that `root_id` still exists in
the registry (either not DESTROYED, or DESTROYED but still retained).

------------------------------------------------------------------------

## 8. What increments `gen` vs `topology_gen`

### 8.1 `gen`

`gen` increments on any snapshot publish, including: - `phase` changes -
`mode` changes - counter updates - error updates - spec/profile updates
becoming effective - retention sweep removals (because snapshot contents
change)

### 8.2 `topology_gen`

`topology_gen` increments only when structural hierarchy changes,
including: - rig created/destroyed - device instance created/destroyed
(`instance_id` lineage changes) - stream created/destroyed - rig
membership changes - a new `root_id` appears or a `root_id` fully
disappears (including detached branches expiring)

------------------------------------------------------------------------

## 9. Retention and retirement sweep

-   `DESTROYED` native object records are retained for a fixed retention
    window to support diagnostics.
-   A retirement sweep removes expired retained records.
-   **If the sweep removes any records, a new snapshot must be
    published.**
