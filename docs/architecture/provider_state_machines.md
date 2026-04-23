# Provider State Machines (Reference)

This document provides compact state-machine references that complement:

- `docs/provider_architecture.md`
- `docs/architecture/lifecycle_model.md`

It is intentionally compact and specification-like.
It defines transition correctness and state axes, not narrative architecture.

Canonical lifecycle phase values remain:

- `CREATED`
- `LIVE`
- `TEARING_DOWN`
- `DESTROYED`

Operational states such as `BOUND`, `OPEN`, or payload-active are separate
axes that describe resource-specific runtime conditions.

------------------------------------------------------------------------

## 1. Overview

Provider state machines exist to make the provider contract mechanically
checkable across:

- synthetic providers
- stub providers
- platform-backed providers

They define:

- what kinds of provider/native facts exist
- how those facts progress over time
- which state axes are structural/native-object truth
- which state axes are operational/runtime posture

They do not redefine:

- snapshot schema
- public Godot-facing API
- core arbitration rules

------------------------------------------------------------------------

## 2. Provider lifecycle

Provider instances follow this lifecycle:

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

Rules:

- Provider lifecycle events must be reported truthfully.
- Provider destruction must occur **after all owned device/session/stream/support
  resources are released**.
- Providers must not emit fabricated destroy events to tidy state.

------------------------------------------------------------------------

## 3. Device lifecycle

Device instances represent an opened hardware or virtual camera.

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

Constraints:

- AcquisitionSession seams and streams require a `LIVE` device.
- Devices should not close while owned children still exist.
- Providers should surface ordering violations rather than hide them.

------------------------------------------------------------------------

## 4. AcquisitionSession lifecycle

AcquisitionSession represents provider-reported acquisition boundary truth.

```text
CREATED
   │ acquisition session realized
   ▼
LIVE
   │ teardown requested / owner seam ended
   ▼
TEARING_DOWN
   │ native session released
   ▼
DESTROYED
```

Constraints:

- AcquisitionSession lifecycle must be reported from actual provider native-object truth.
- `SyntheticProvider` currently realizes `AcquisitionSession` truth for both
  stream-backed and capture-only paths.
- Capture-only realization does not require fabrication of a public stream.

------------------------------------------------------------------------

## 5. Stream lifecycle

Streams represent repeating capture pipelines.

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

Important distinction:

- stream existence does **not** imply active frame production
- stream existence does not by itself imply ongoing payload delivery
- payload delivery and provider-owned native support truth are interpreted
  separately from stream structural existence

------------------------------------------------------------------------

## 6. Payload delivery and native support interpretation

CamBANG no longer treats FrameProducer as a canonical state-machine axis in
this tranche.

Payload delivery is interpreted from delivery truth, while provider-owned native
support entities are interpreted from ownership/context truth beneath
`Stream` (stream-originated) or `AcquisitionSession` (capture-originated).

------------------------------------------------------------------------

## 7. Event-class model

Provider facts fall into four broad classes:

| Event class | Example | Delivery policy |
|---|---|---|
| Lifecycle | device add/remove, acquisition-session create/destroy, stream start/stop | must never be dropped |
| Native-object | provider/device/acquisition-session/stream/native-support create/destroy reports | must never be dropped |
| Error | provider, device, or stream error reports | must never be dropped |
| Frame | video frame delivery / capture frame delivery | may be coalesced or dropped under backpressure |

Lifecycle, native-object, and error events must always preserve ordering.

Topology change is not a separate event class. It is an effect reflected by
lifecycle and native-object truth, and later by snapshot `topology_version`.

------------------------------------------------------------------------

## 8. Provider strand delivery rules

All provider facts must be delivered through the **provider strand**.

```text
Platform callback / worker
      │
      ▼
post to provider strand
      │
      ▼
strand invokes core callback
```

Rules:

1. Only the provider strand may deliver provider facts.
2. Lifecycle, native-object, and error events are **non-droppable**.
3. Frame events **may be dropped** under pressure.
4. Synthetic and stub providers must obey the same strand and truthfulness rules.

------------------------------------------------------------------------

## 9. Axes summary

| Axis | Purpose |
|---|---|
| Lifecycle phase | registry/native-object truth (`CREATED` → `DESTROYED`) |
| Operational state | provider/device/acquisition-session/stream runtime posture |
| Context placement / support interpretation | stream-originated vs capture-originated native support truth |

These axes must not be collapsed into a single state machine.

------------------------------------------------------------------------

## 10. Operational state reference

The following compact state references describe runtime posture, not
registry/native-object lifecycle phase.

### 10.1 Provider Binding State

```text
ABSENT
BOUND
```

Meaning:

- `ABSENT` — provider is not bound to the device lineage
- `BOUND` — provider owns the device lineage and may create downstream resources

Transitions:

- `ABSENT → BOUND` — provider binds device
- `BOUND → ABSENT` — provider releases device

### 10.2 Device Open State

```text
CLOSED
OPEN
```

Meaning:

- `CLOSED` — device handle not active
- `OPEN` — device handle active and capable of downstream acquisition

Transitions:

- `CLOSED → OPEN` — device opened
- `OPEN → CLOSED` — device closed

### 10.3 AcquisitionSession Presence State

```text
ABSENT
PRESENT
```

Meaning:

- `ABSENT` — no AcquisitionSession seam is currently realized for the device lineage
- `PRESENT` — AcquisitionSession seam is realized and reported in provider native-object truth

Current implementation note:

- `SyntheticProvider` realizes `AcquisitionSession` truth for both
  stream-backed and capture-only paths
- session presence remains provider/native truth rather than scenario-authored truth

### 10.4 Stream Presence State

```text
ABSENT
PRESENT
```

Meaning:

- `ABSENT` — no stream exists
- `PRESENT` — stream object exists but may not yet be producing

### 10.5 Payload delivery interpretation

```text
IDLE
DELIVERING
```

Meaning:

- `IDLE` — no current payload delivery is being observed
- `DELIVERING` — payload delivery is being observed

Context placement note:

- stream-originated native support truth is interpreted beneath `Stream`
- capture-originated native support truth is interpreted beneath `AcquisitionSession`

------------------------------------------------------------------------

## 11. Truthfulness guardrails

These compact state machines rely on the same truthfulness rules defined
canonically elsewhere.

In particular:

- providers must not fabricate lifecycle/native-object truth merely to tidy state
- providers must not suppress lifecycle/native-object truth when resources are actually realized
- normal-operation bad ordering should be surfaced, not silently repaired
- shutdown may perform ordered internal teardown, but destruction must still correspond to actual release

------------------------------------------------------------------------

## 12. Synthetic and stub scope note

SyntheticProvider and StubProvider are reference expressions of the contract.
They are not exempt from the state-machine rules.

That means:

- SyntheticProvider must report lifecycle/native-object truth with the same honesty expected of platform-backed providers
- StubProvider may be simpler, but it must still obey the same ordering and strand guarantees
- platform-backed providers are adapters to this same model, not authorities over it