# CamBANG Lifecycle Model

This document explains the structural lifecycle model used by CamBANG
for provider-reported native truth, lifecycle ordering, registry interpretation,
and detached-root reasoning.

It supplements:

- `docs/provider_architecture.md`
- `docs/state_snapshot.md`
- `docs/core_runtime_model.md`

It does not redefine those canonical documents.

------------------------------------------------------------------------

## 1. Structural hierarchy

CamBANG standardises provider-reported native truth into a cross-provider
viewing/modeling structure:

```text
Provider
 └─ Device
     └─ AcquisitionSession
         └─ Stream
```

Meaning of each level:

| Level | Meaning |
|---|---|
| Provider | Backend adapter instance bound to Core |
| Device | Provider owns an opened camera device-handle lineage |
| AcquisitionSession | Provider-reported acquisition seam for that device lineage |
| Stream | Provider owns a configured repeating capture pipeline when such a pipeline exists |

Important clarifications:

- this hierarchy is a **CamBANG-imposed viewing structure** for intelligibility
  and cross-provider reasoning
- it must not be mistaken for a claim that underlying platform APIs are
  hierarchical in the same way
- native truth is not limited only to these structural nouns

Frame/sample delivery is related to this model, but **frame/sample delivery is
not itself a first-class structural native-object category in this hierarchy**.

This hierarchy is reflected in:

- provider lifecycle reporting
- native-object registry ownership relationships
- snapshot ancestry reconstruction
- detached-root visibility and diagnostics

------------------------------------------------------------------------

## 2. Why `AcquisitionSession` exists

`AcquisitionSession` is the provider-reported acquisition seam for a device lineage.

It exists because different backends expose the acquisition boundary in different
ways:

- some APIs map naturally to a session-like native resource
- others require the provider to impose a session-shaped seam for coherent truth

`AcquisitionSession` is therefore a **truth boundary**, not a promise that every
backend API uses the same noun or hierarchy natively.

Implementation-scope reminder:

- `SyntheticProvider` currently realizes `AcquisitionSession` truth for both
  stream-backed and capture-only paths
- the seam is retained while stream and/or capture references remain live

------------------------------------------------------------------------

## 3. Production interpretation without a producer row

Production is not a first-class structural noun in the canonical CamBANG
model for this tranche.

CamBANG keeps a smaller structural hierarchy: `Provider`, `Device`,
`AcquisitionSession`, and `Stream`.

Production is interpreted through structural context, payload delivery truth,
and provider-owned native support entities. This avoids using a separate
producer row as a catch-all for production or payload-support truth.

------------------------------------------------------------------------

## 4. Native truth beyond the structural hierarchy

The CamBANG structural hierarchy is the preferred cross-provider viewing
structure. It is **not** the full limit of native truth.

Providers must report provider-owned native resources truthfully whenever their
creation, retention, or release is relevant to:

- runtime truth
- ownership diagnostics
- leak prevention
- queue health
- teardown correctness
- retained-result or backing-resource truth

This includes resource-bearing native objects or leases such as:

- retained samples
- acquired images
- mapped or attached buffers
- shared-buffer references
- other provider-owned resource handles whose release duty is not safely and
  wholly subsumed by parent-object destruction alone

A native object may therefore be truthfully reported without becoming a new
first-class structural category in the CamBANG viewing hierarchy.

Resource-bearing native truth beyond the structural hierarchy is grouped by
calling context rather than by a separate producer row:

- stream-originated resource-bearing truth beneath `Stream`
- capture-originated resource-bearing truth beneath `AcquisitionSession`

This preserves the distinction between structural seams, production-seam
truth, and additional provider-owned resource/buffer truth whose lifetime
matters independently.

------------------------------------------------------------------------

## 5. Lifecycle phases

Canonical lifecycle phase values remain:

- `CREATED`
- `LIVE`
- `TEARING_DOWN`
- `DESTROYED`

These are registry/native-object truth values.

Operational posture such as “open”, “configured”, “flowing”, or “producing”
may be described by additional state axes, but they do not replace lifecycle
phase truth.

------------------------------------------------------------------------

## 6. Provider selection and attachment

Core binds to exactly one provider instance at runtime.

High-level attachment sequence:

```text
Provider selected
      │
      ▼
Provider created / attached
      │
      ▼
Provider enters LIVE
```

This does not imply that any device/session/stream resources exist yet.
Those must be reported separately as they are actually acquired.

Synthetic, stub, and platform-backed providers all participate in the same
lifecycle/native-truth model.

------------------------------------------------------------------------

## 7. Lifecycle event flow

At a high level, provider lifecycle/native-object truth flows like this:

```text
backend/native fact realized
      │
      ▼
provider reports fact truthfully
      │
      ▼
provider strand serialization
      │
      ▼
core lifecycle registry integration
      │
      ▼
snapshot/native-object truth becomes observable
```

Key rule:

- lifecycle/native-object/error facts are **non-lossy**
- frame delivery may be lossy
- lossiness of frames must not suppress lifecycle truth

------------------------------------------------------------------------

## 8. Provider strand model (relationship only)

All provider → Core facts cross the provider boundary through the
provider strand.

The strand guarantees:

- ordered delivery of lifecycle/native-object/error facts
- deterministic sequencing
- absence of concurrent provider → Core mutation

This document does not restate the full strand model.
See `docs/architecture/provider_strand_model.md`.

------------------------------------------------------------------------

## 9. Event classes and delivery policy

Provider facts fall into four broad classes:

| Event class | Examples | Delivery policy |
|---|---|---|
| Lifecycle | device add/remove, stream start/stop, acquisition-session create/destroy | must never be dropped |
| Native-object | provider/device/acquisition-session/stream/native-support create/destroy | must never be dropped |
| Error | provider, device, stream, or session errors | must never be dropped |
| Frame | repeating frame delivery, capture frame delivery | may be coalesced or dropped |

Topology change is not a separate event class. It is an effect that emerges
from lifecycle/native-object truth and later from observable snapshot
`topology_version`.

------------------------------------------------------------------------

## 10. Provider lifecycle

```text
CREATED
   │ attach to core
   ▼
LIVE
   │ shutdown requested
   ▼
TEARING_DOWN
   │ resources released
   ▼
DESTROYED
```

`Provider` represents the backend adapter instance bound to Core.

Ordering rule:

- Provider destruction must occur **after** owned device/session/stream/support
  resources are actually released.

------------------------------------------------------------------------

## 11. Device lifecycle

```text
CREATED
   │ open device
   ▼
LIVE
   │ close requested
   ▼
TEARING_DOWN
   │ hardware handle released
   ▼
DESTROYED
```

A `Device` represents an opened device-handle lineage.

Ordering constraints:

- AcquisitionSession/streams require a `LIVE` device
- a device should not be closed while owned children still exist
- providers should preserve ordering truth rather than silently hide violations

------------------------------------------------------------------------

## 12. AcquisitionSession lifecycle

```text
CREATED
   │ acquisition seam realized
   ▼
LIVE
   │ teardown requested / owner seam ended
   ▼
TEARING_DOWN
   │ native session released
   ▼
DESTROYED
```

An `AcquisitionSession` represents provider-reported acquisition seam truth for
the device lineage.

Current implementation reminder:

- `SyntheticProvider` realizes `AcquisitionSession` truth for both
  stream-backed and capture-only paths
- a capture-only `AcquisitionSession` does not require a public `CamBANGStream`

Destroyed-retained session history may remain visible in `native_objects`, while
top-level `acquisition_sessions[]` remains authoritative for current/live session
truth.

------------------------------------------------------------------------

## 13. Stream lifecycle

```text
CREATED
   │ stream configured
   ▼
LIVE
   │ stop / destroy requested
   ▼
TEARING_DOWN
   │ pipeline resources released
   ▼
DESTROYED
```

A `Stream` represents an owned repeating capture pipeline.

Important distinction:

- stream existence does **not** by itself imply ongoing payload delivery
- payload delivery and provider-owned native support truth are interpreted
  separately from stream structural existence

Core also preserves the invariant:

> At most one repeating stream may be active per device instance.

Multiple stream records may exist, but only one may be active
(`mode != STOPPED`) at a time.

------------------------------------------------------------------------

## 14. Production and support truth interpretation

Production truth is interpreted from context and payload delivery, not from a
separate structural producer row.

Provider-owned native support entities may appear beneath `Stream` for
stream-originated truth and beneath `AcquisitionSession` for
capture-originated truth.

These support entities remain distinct from the structural spine and remain
essential for truthful lifetime/release diagnostics.

------------------------------------------------------------------------

## 15. Operational state vs lifecycle phase

Lifecycle phase and operational state must not be collapsed into one axis.

Examples:

- a `Stream` may be `LIVE` as a lifecycle/native-object record while operationally
  stopped
- payload delivery may be idle while structural rows remain LIVE
- an `AcquisitionSession` may be `LIVE` while not currently producing frames

Lifecycle phase answers:

> does this native/logical resource currently exist, tear down, or remain retained?

Operational state answers:

> what is it currently doing?

------------------------------------------------------------------------

## 16. Lifecycle truthfulness

Providers must report lifecycle transitions **only when the underlying
resource state actually changes**.

Examples:

| Report | Meaning |
|---|---|
| device created | hardware or equivalent resource acquired |
| device destroyed | hardware or equivalent resource released |
| stream created | capture pipeline created |
| stream destroyed | pipeline released |
| acquisition session created | acquisition seam realized |
| acquisition session destroyed | acquisition seam released |

Providers must **not** fabricate lifecycle events merely to tidy state.

### 16.1 No normal-operation auto-cascade

During normal operation, providers must not hide bad ordering by silently
stopping or destroying owned children behind the caller’s back.

Examples:

- `close_device()` should fail if child streams still exist
- `destroy_stream()` should fail if the stream is still started / producing

### 16.2 Shutdown is different

During provider shutdown, ordered internal teardown is allowed so the
provider can release resources cleanly.

Even then, shutdown must remain truthful:

- destruction events correspond to actual release
- unreleased resources must not emit false destruction events

------------------------------------------------------------------------

## 17. Relationship to the lifecycle registry

Lifecycle and native-object events populate the core lifecycle registry.

Each record may appear as a `NativeObjectRecord` in snapshots.

Relevant record fields include:

- `native_id`
- `type`
- `phase`
- ownership fields (`owner_device_instance_id`, `owner_acquisition_session_id`,
  `owner_stream_id`, `owner_provider_native_id`, `owner_rig_id`)
- `root_id`
- `creation_gen`
- `created_ns`
- `destroyed_ns`

Destroyed records may remain temporarily retained for diagnostics.

The registry records **observed reality**, not intended state.

Core does not auto-cascade child destruction in the registry merely to make
the published state appear tidy.

------------------------------------------------------------------------

## 18. Detached roots

If a native object survives destruction of its logical owner, that branch
becomes a **detached root** in the snapshot system.

Example failure scenario:

```text
Stream destroyed
Native support entity not destroyed
```

Result:

```text
Native support entity (detached root)
```

Detached roots are intentional and diagnostically useful. They help reveal:

- lifecycle ordering bugs
- delayed teardown
- resource leaks
- unexpected lifetime extension
- platform-specific shutdown problems

They are therefore a feature of truthfulness, not an error in the registry.

------------------------------------------------------------------------

## 19. Still capture and public-object parity

Providers may truthfully realize native `Stream` and additional provider-owned
native support entities while servicing a device-level still capture request
when that is how the backend actually works.

This does **not** require creation of a corresponding user-addressable
`CamBANGStream`.

CamBANG therefore keeps two related but distinct surfaces:

- the public Godot-facing object model (`CamBANGDevice`, `CamBANGStream`, etc.)
- the native-object truth model used for lifecycle reporting and diagnostics

A provider-internal backend choice (for example, a short-lived stream-like
native resource used to service still capture) must remain visible in
native-object truth if it is lifecycle-significant, but it must not be promoted
automatically into a public Godot-facing `CamBANGStream` merely for parity.

------------------------------------------------------------------------

## 20. Synthetic and stub providers

Synthetic and stub providers must obey the **same lifecycle truth rules**
as platform-backed providers.

Even though synthetic lifecycles do not map to physical hardware, they must
still report provider-owned resource truth faithfully and preserve the same
ordering, ownership, and registry expectations.

Synthetic timing drivers may vary (`virtual_time`, `real_time`), but all
observable facts still pass through the provider strand.

------------------------------------------------------------------------

## 21. Godot-facing invariants preserved

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

------------------------------------------------------------------------

## 22. Invariants summary

- lifecycle/native-object truth is provider-reported and must be truthful
- canonical structural nouns define the preferred viewing structure, not the full limit of native truth
- production is interpreted through context and payload delivery rather than a
  separate producer structural noun
- provider-owned native support truth is grouped by stream-originated vs
  capture-originated context
- frame/sample delivery is conceptually distinct from structural/native-object hierarchy
- platform-backed providers must adapt to this model rather than redefining it
- detached roots are expected and diagnostically useful