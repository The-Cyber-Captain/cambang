# CamBANG Lifecycle Model

This document explains the **lifecycle structure, ownership hierarchy, and
provider → core event flow** used by the CamBANG runtime.

It supplements the canonical architecture documents:

- `provider_architecture.md`
- `core_runtime_model.md`
- `state_snapshot.md`

This document is **explanatory only**.  
It does **not** redefine canonical architecture rules.

---

## 1. Structural hierarchy

CamBANG standardises camera-stack ownership into a provider-agnostic model:

```text
Provider
 └─ Device
     └─ AcquisitionSession
         └─ Stream
             └─ FrameProducer (optional)
                 └─ Frame (repeated samples)
```

Meaning of each level:

| Level | Meaning |
|---|---|
| Provider | Core is bound to one provider backend instance |
| Device | Provider owns an opened camera device handle |
| AcquisitionSession | Provider-reported acquisition seam for that device lineage |
| Stream | Provider owns a configured capture pipeline |
| FrameProducer | Stream is actively producing frames |
| Frame | Individual frame sample delivered to Core |

`FrameProducer` is optional and used primarily for diagnostics,
synthetic testing, and meaningful platform-backed lifecycle boundaries.

Implementation status (current repo truth):

- Concrete `AcquisitionSession` realization is stream-backed in `SyntheticProvider`
  (first successful `create_stream(...)` to last stream destroy for that device).
- Still-only `AcquisitionSession` realization is not yet implemented.

This hierarchy is reflected in:

- provider callbacks
- lifecycle reporting
- `NativeObjectRecord` ownership relationships
- detached-root visibility in snapshots

The hierarchy is intentionally **provider-agnostic**.

Platform terminology such as:

- reader
- pipeline
- track

remains provider-internal. `AcquisitionSession` itself is now a canonical
CamBANG/shared lifecycle noun and is not treated as provider-private wording.

---

## 2. Provider selection and attachment

Providers are selected (latched) by the broker and attached to Core as a
single active backend instance.

```text
ABSENT ── attach ──> BOUND
  ^                    │
  └──── detach ────────┘
```

Native-object reporting:

- create native object record: `Provider` on transition to **BOUND**
- destroy native object record: `Provider` on transition to **ABSENT**

Important consequence:

- provider backend switching requires teardown / restart of the Core runtime
- platform-backed providers are adapters to the same contract exercised by
  `SyntheticProvider` and `StubProvider`

---

## 3. Lifecycle event flow

Lifecycle facts originate in the provider layer and propagate towards the
core runtime through a fixed path:

```text
Platform API / synthetic scheduler
        │
        ▼
Provider adapter / backend
        │
        ▼
Provider strand (single serialized callback context)
        │
        ▼
Core provider event ingress
        │
        ▼
Core runtime event integration
        │
        ▼
Lifecycle registry + snapshot publication
```

Important properties:

- platform callbacks **must not** invoke core lifecycle or registry services directly
- provider facts are delivered through the **provider strand**
- core integrates lifecycle facts on the **core thread**
- snapshot truth therefore reflects ordered provider observations

Typical callback names in the conceptual flow include:

```text
on_device_opened
on_stream_created
on_stream_started
on_frame
on_stream_stopped
on_stream_destroyed
on_device_closed
```

Native-object lifecycle events accompany these transitions.

---

## 4. Provider strand model

Providers deliver all provider → core facts through a **single serialized
callback context**, referred to as the **provider strand**.

Properties of the strand:

- ensures deterministic ordering of provider events
- prevents concurrent mutation of core-owned state
- allows platform callbacks and worker threads to post work safely
- unifies behaviour across platform-backed, synthetic, and stub providers

Typical pattern:

```text
Platform callback / scheduler / worker
      │
      ▼
post fact to provider strand
      │
      ▼
Provider strand executes callback
      │
      ▼
Core runtime enqueues / integrates event
```

Only the provider strand may deliver observable provider facts to Core.
All other provider threads must **post** to the strand, not bypass it.

### Event-class policy

Provider facts fall into four broad classes:

| Event class | Examples | Delivery policy |
|---|---|---|
| Lifecycle | device opened/closed, stream started/stopped | Non-lossy |
| Native-object | created/destroyed object reports | Non-lossy |
| Error | provider/device/stream error | Non-lossy |
| Frame | repeating or capture frame delivery | Lossy allowed |

Required guarantees:

- lifecycle/native-object/error events must not be silently discarded once admitted
- frame events may be dropped under pressure
- dropping frames must not distort lifecycle truthfulness

---

## 5. Provider, device, AcquisitionSession, stream, and FrameProducer lifecycles

### 5.1 Provider lifecycle

```text
CREATED
   │
   ▼
LIVE
   │
   ▼
TEARING_DOWN
   │
   ▼
DESTROYED
```

This corresponds to the lifetime of the backend adapter instance bound to Core.

### 5.2 Device lifecycle

```text
CREATED
   │
   ▼
LIVE
   │
   ▼
TEARING_DOWN
   │
   ▼
DESTROYED
```

A `Device` represents an opened handle to a specific camera endpoint.

Ordering constraints:

- AcquisitionSession/streams require a device to be open / live
- a device should not be closed while any owned stream exists
- providers should preserve ordering truth rather than silently hide violations

### 5.3 AcquisitionSession lifecycle

```text
CREATED
   │
   ▼
LIVE
   │
   ▼
TEARING_DOWN
   │
   ▼
DESTROYED
```

An `AcquisitionSession` represents provider-reported acquisition seam truth for
the device lineage.

Current implementation scope:

- concrete realization is stream-backed in `SyntheticProvider`
- still-only realization is not yet implemented

### 5.4 Stream lifecycle

```text
CREATED
   │
   ▼
LIVE
   │
   ▼
TEARING_DOWN
   │
   ▼
DESTROYED
```

A `Stream` represents an owned capture pipeline.

The stream lifecycle separates:

- existence of a configured pipeline
- active production of frames

Core also enforces the invariant:

> At most one repeating stream may be active per device instance.

Multiple stream records may exist, but only one may be active
(`mode != STOPPED`) at a time.

### 5.5 FrameProducer lifecycle

```text
IDLE ── enable ──> PRODUCING
  ^                  │
  └──── disable ─────┘
```

`FrameProducer` is the active production loop for a stream.

Examples:

| Platform / provider | FrameProducer meaning |
|---|---|
| Camera2 | repeating capture request |
| Media Foundation | `ReadSample` loop active |
| V4L2 | `STREAMON` |
| Synthetic | pattern generator producing |
| Stub | deterministic synthetic emitter |

Per-frame resources are **not** tracked as individual native objects.

---

## 6. Lifecycle truthfulness

Providers must report lifecycle transitions **only when the underlying
resource state actually changes**.

Examples:

| Report | Meaning |
|---|---|
| device created | hardware or equivalent resource acquired |
| device destroyed | hardware or equivalent resource released |
| stream created | capture pipeline created |
| stream destroyed | pipeline released |
| AcquisitionSession created | acquisition seam realized |
| AcquisitionSession destroyed | acquisition seam released |
| FrameProducer created | production actually enabled |
| FrameProducer destroyed | production actually stopped |

Providers must **not** fabricate lifecycle events merely to tidy state.

### No normal-operation auto-cascade

During normal operation, providers must not hide bad ordering by silently
stopping or destroying children behind the caller’s back.

Examples:

- `close_device()` should fail if child streams still exist
- `destroy_stream()` should fail if the stream is still started / producing

### Shutdown is different

During provider shutdown, ordered internal teardown is allowed so the
provider can release resources cleanly.

Even then, shutdown must remain truthful:

- destruction events correspond to actual release
- unreleased resources must not emit false destruction events

---

## 7. Relationship to the lifecycle registry

Lifecycle and native-object events populate the core lifecycle registry.

Each record may appear as a `NativeObjectRecord` in snapshots.

Relevant record fields include:

- `native_id`
- `type`
- `phase`
- ownership fields (`owner_device_instance_id`, `owner_stream_id`, `owner_provider_native_id`, `owner_rig_id`)
- `root_id`
- `creation_gen`
- `created_ns`
- `destroyed_ns`

Destroyed records may remain temporarily retained for diagnostics.

The registry records **observed reality**, not intended state.

Core does not auto-cascade child destruction in the registry merely to make
the published state appear tidy.

---

## 8. Detached roots

If a native object survives destruction of its logical owner, that branch
becomes a **detached root** in the snapshot system.

Example failure scenario:

```text
Stream destroyed
FrameProducer not destroyed
```

Result:

```text
FrameProducer (detached root)
```

Detached roots are intentional and diagnostically useful. They help reveal:

- lifecycle ordering bugs
- delayed teardown
- resource leaks
- unexpected lifetime extension
- platform-specific shutdown problems

They are therefore a feature of truthfulness, not an error in the registry.

---

## 9. Synthetic and stub providers

Synthetic and stub providers must obey the **same lifecycle hierarchy**
as platform-backed providers.

Even though synthetic streams do not map to physical devices, they must still:

1. open a device instance
2. create streams owned by that device
3. start frame production through a `FrameProducer`

This preserves identical lifecycle semantics, native-object reporting,
and diagnostic expectations across provider types.

Synthetic timing drivers may vary (`virtual_time`, `real_time`), but all
observable facts still pass through the provider strand.

---

## 10. Godot-facing invariants preserved

The lifecycle model does **not** alter the public Godot-facing API.

In particular:

- `CamBANGServer.start()` still produces a baseline generation snapshot
- `CamBANGServer.stop()` still performs deterministic teardown
- `gen`, `version`, and `topology_version` semantics remain unchanged
- native-object lifecycle reporting remains truthful
- provider callback threading remains serialized

Lifecycle refactors must therefore preserve:

- the provider strand rule
- ordering truthfulness
- detached-root visibility
- snapshot consistency

---

## 11. Determinism guarantees

The lifecycle model preserves deterministic behaviour through:

- provider-strand serialization
- core-thread integration
- ordered teardown expectations
- non-lossy delivery for lifecycle/native-object/error facts
- truth-preserving native-object reporting
- snapshot publication after converged core integration

Normative definitions remain in:

- `provider_architecture.md`
- `core_runtime_model.md`
- `state_snapshot.md`
