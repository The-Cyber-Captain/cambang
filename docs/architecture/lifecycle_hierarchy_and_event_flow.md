# Lifecycle Hierarchy and Event Flow

This document is an **architectural supplement**.

It illustrates the lifecycle hierarchy and event flow described in the
canonical architecture documents:

- `provider_architecture.md`
- `core_runtime_model.md`
- `state_snapshot.md`

This document **does not redefine architecture**.  
It provides a visual reference for how provider lifecycle, native object
reporting, and frame delivery interact during runtime.

---

## 1. Conceptual lifecycle hierarchy

CamBANG standardises camera stack ownership into a provider-agnostic model.

```
Provider
 └─ Device
     └─ Stream
         └─ FrameProducer (optional)
             └─ Frame (repeated samples)
```

### Meaning of each level

| Level | Meaning |
|------|--------|
| Provider | Core is bound to a provider backend instance |
| Device | Provider owns an opened camera device handle |
| Stream | Provider owns a configured capture pipeline |
| FrameProducer | Stream is actively producing frames |
| Frame | Individual frame sample delivered to Core |

`FrameProducer` is optional and used primarily for diagnostics and
synthetic testing.

---

## 2. Snapshot and registry overlay

Core publishes system state using the snapshot schema.

The lifecycle registry (`CBLifecycleRegistry`) records the **actual
existence of native resources** owned by providers.

Conceptually, the snapshot view is the semantic hierarchy with a native
object overlay:

```
Provider
 └─ Device
     └─ Stream
         └─ FrameProducer
```

Each level corresponds to native objects reported by providers:

| Native object | Description |
|---------------|-------------|
| Provider | backend attached to Core |
| Device | opened camera device |
| Stream | configured capture pipeline |
| FrameProducer | active frame production loop |

Snapshots also contain fields such as `native_objects` and
`detached_root_ids` which expose the **true lifecycle of resources**,
including leaked or unexpected objects.

---

## 3. Runtime event flow

The following diagram shows how lifecycle events propagate from providers
into Core.

```
Godot Host / DevNode
        │
        ▼
CamBANGServer
        │
        ▼
ProviderBroker (facade)
        │
        ▼
Provider Backend
        │
        ├── enumerate_endpoints()
        │
        ├── open_device()
        │       └── Device created
        │
        ├── create_stream()
        │       └── Stream created
        │
        ├── start_stream()
        │       └── FrameProducer created (optional)
        │
        ├── frame loop
        │       └── on_frame(...)
        │
        ├── stop_stream()
        │       └── FrameProducer destroyed
        │
        ├── destroy_stream()
        │       └── Stream destroyed
        │
        └── close_device()
                └── Device destroyed
```

Provider callbacks deliver lifecycle facts into Core. Typical callback
names include:

```
on_device_opened
on_stream_created
on_stream_started
on_frame
on_stream_stopped
on_stream_destroyed
on_device_closed
```

Native object lifecycle events accompany these transitions.

### 3.x Godot-visible publication (tick-bounded)

Core integrates provider facts as they arrive and may update internal state
multiple times between Godot ticks.

At the Godot boundary, `CamBANGServer` exposes snapshots using a tick-bounded
observable truth model:

- `state_published(...)` is emitted **at most once per Godot tick**.
- It is emitted only if observable snapshot state has changed since the
  previous tick.

This prevents burst emission storms during teardown/spin-up and keeps consumer
logic simple and deterministic.

---

## 4. Truthfulness rule

Lifecycle reporting must reflect **actual resource ownership**.

Providers must:

- emit **created** when the resource is successfully acquired
- emit **destroyed** only when the resource is fully released

Providers must **not** emit destruction events when teardown has merely
been requested.

---

## 5. Orphan and leak visibility

The lifecycle registry intentionally does **not cascade deletions**.

Example failure scenario:

```
Stream destroyed
FrameProducer not destroyed
```

Registry state becomes:

```
FrameProducer (detached root)
```

Snapshots expose this via fields such as `detached_root_ids`.

This behaviour is intentional and allows:

- leak detection
- teardown diagnostics
- lifecycle ordering verification

---

## 6. Platform mapping examples

The CamBANG ownership model maps naturally onto major camera APIs.

| Platform | Device | Stream |
|--------|--------|--------|
| Camera2 | `ACameraDevice` | `ACameraCaptureSession` |
| Media Foundation | `IMFMediaSource` | `IMFSourceReader` pipeline |
| V4L2 | `/dev/videoX` file descriptor | STREAMON buffer queue |
| AVFoundation | `AVCaptureDevice` | `AVCaptureSession` |
| WebRTC | device source | `MediaStreamTrack` |

`FrameProducer` corresponds to the platform mechanism that enables
continuous frame production.

Examples:

| Platform | FrameProducer |
|---------|---------------|
| Camera2 | repeating capture request |
| Media Foundation | ReadSample loop |
| V4L2 | STREAMON |
| Synthetic | pattern generator |

---

## 7. Synthetic provider example

SyntheticProvider follows the same lifecycle model.

```
Provider
 └─ Device (synthetic camera)
     └─ Stream
         └─ FrameProducer (pattern generator)
             └─ Frame stream
```

Frames are generated using the Pattern Module and delivered to Core
using deterministic virtual time.

---

## 8. Deterministic synthetic cadence

Nominal synthetic streams operate using virtual time.

```
advance(dt_ns)
    └─ update virtual clock
    └─ emit frames where next_due_ns <= now
```

Each emitted frame receives a timestamp based on the scheduled time:

```
capture_timestamp = next_due_ns
```

This allows synthetic streams to simulate real capture pipelines
deterministically.

---

## 9. Relationship to canonical architecture

This diagram supplements the following canonical documents:

- `provider_architecture.md`
- `core_runtime_model.md`
- `state_snapshot.md`

Those documents define the authoritative behaviour.

This document exists only to **visualise the lifecycle model and event flow**.
