# Provider Lifecycle State Machine

This document is an **architectural supplement**.

It illustrates provider lifecycle state transitions described in the
canonical architecture documents:

- `provider_architecture.md`
- `core_runtime_model.md`
- `state_snapshot.md`

This document **does not redefine architecture**.  
It provides a compact reference for reasoning about provider/device/stream
ordering, teardown correctness, and native object reporting.

---

## 1. Structural hierarchy (recap)

```
Provider
 └─ Device
     └─ Stream
         └─ FrameProducer (optional)
```

The state machines below describe the typical lifecycle transitions for
each level.

---

## 2. Provider lifecycle

Providers are selected (latched) by the broker and attached to Core as a
single active backend instance.

```
┌──────────┐          attach           ┌──────────┐
│  ABSENT  │ ────────────────────────> │  BOUND   │
└──────────┘                           └──────────┘
     ^                                      │
     │               detach / shutdown      │
     └──────────────────────────────────────┘
```

Native object reporting:

- Create native object record: `Provider` on transition to **BOUND**
- Destroy native object record: `Provider` on transition to **ABSENT**

Notes:

- Provider selection is latched at initialization; switching provider
  backend requires teardown/restart of the Core runtime.

---

## 3. Device lifecycle

A Device represents an opened handle to a specific camera endpoint.
Devices are identified by provider-defined `hardware_id` and Core-defined
`instance_id` / `root_id` lineage.

```
┌──────────┐       open_device OK       ┌──────────┐
│  CLOSED  │ ─────────────────────────> │   OPEN   │
└──────────┘                            └──────────┘
     ^                                       │
     │          close_device complete         │
     └───────────────────────────────────────┘
```

Native object reporting:

- Create native object record: `Device` on transition to **OPEN**
- Destroy native object record: `Device` on transition to **CLOSED**
  (only after the device handle is actually released)

Ordering constraints:

- Streams require a Device to be **OPEN**.
- A Device should not be closed while any owned Stream exists.
  Providers should log if this constraint is violated.

---

## 4. Stream lifecycle

A Stream represents an owned capture pipeline capable of producing
frames (e.g. Camera2 session, WMF source reader pipeline, V4L2 streaming
queue).

The Stream lifecycle separates *existence* of a configured pipeline from
*production* of frames.

```
┌──────────┐     create_stream OK      ┌──────────┐
│  ABSENT  │ ────────────────────────> │  PRESENT │
└──────────┘                           └──────────┘
     ^                                      │
     │        destroy_stream complete        │
     └──────────────────────────────────────┘
```

Native object reporting:

- Create native object record: `Stream` on transition to **PRESENT**
- Destroy native object record: `Stream` on transition to **ABSENT**
  (only after the pipeline is actually released)

Ordering constraints:

- A Stream implies a Device is OPEN.
- Destroying a Stream should typically occur only after production has
  been stopped (FrameProducer not present), but ordering violations must
  remain visible in the registry if they occur.

---

## 5. FrameProducer lifecycle (optional, per-stream)

A FrameProducer represents active production of frames for a Stream.
This corresponds to "repeating request active" (Camera2), "ReadSample
loop active" (WMF), STREAMON (V4L2), or "pattern generator producing"
(Synthetic).

```
┌────────────┐        enable            ┌────────────┐
│   IDLE     │ ───────────────────────> │ PRODUCING  │
└────────────┘                          └────────────┘
      ^                                      │
      │              disable                 │
      └──────────────────────────────────────┘
```

Native object reporting:

- Create native object record: `FrameProducer` on transition to **PRODUCING**
- Destroy native object record: `FrameProducer` on transition to **IDLE**
  (only after production is actually disabled / stopped)

Notes:

- FrameProducer is per-stream and optional.
- Synthetic and Stub providers implement FrameProducer by design.
- Platform-backed providers may implement FrameProducer if it remains an
  abstract, meaningful diagnostic boundary.

---

## 6. Frame delivery and in-flight resources (important nuance)

Frames are delivered as repeated samples while a Stream is PRESENT and
production is enabled (FrameProducer PRODUCING).

Per-frame resources are *not* tracked as native objects. They are too
numerous and transient.

However, in-flight frame resources may outlive a stop request.

Example:

- Stream stop requested
- Provider halts acquisition
- One or more frames remain held by Core until release callbacks execute

This is expected and does not require per-frame native object records.
If additional diagnostics are needed, prefer aggregated counters (e.g.
in-flight frame count) over per-frame object tracking.

---

## 7. Truthfulness rule (applies to all transitions)

Providers must report lifecycle events as observed reality:

- Emit `created` only when the resource has been successfully acquired.
- Emit `destroyed` only when the resource has been fully released.
- Do not report destruction when teardown is merely requested.
- Do not cascade deletions in reporting.

If a child object survives its parent (e.g. FrameProducer remains alive
after Stream is destroyed), the registry must still reflect the child as
alive until it is actually destroyed.

Such objects may appear as detached roots in snapshots and are valuable
diagnostic signals.
