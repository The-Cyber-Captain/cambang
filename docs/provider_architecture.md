# CamBANG Provider Architecture

This document defines the **core ↔ provider boundary** for CamBANG and
the obligations of provider implementations.

A **provider** is the backend responsible for enumerating endpoints,
controlling a camera API or synthetic source, producing frames, and
reporting owned native object lifecycles back to Core.

Providers are **execution engines**. **Core is the authority**.

---

## 1. Goals

Providers must support:

- deterministic behaviour (ordering, threading, teardown)
- correct object lifecycles and resource management
- canonical mapping of backend formats into CamBANG pixel formats
  (`FourCC`-style `uint32`)
- provider-agnostic delivery into Core's registry / snapshot / publish
  model
- testability through synthetic and stub implementations

Providers must not:

- expose backend-native semantics directly to Godot-facing objects
- own or publish `CamBANGStateSnapshot`
- implement arbitration / policy decisions
- invent alternative lifecycle models
- retain references to Core objects beyond their documented lifetime

---

## 2. Provider categories

CamBANG supports more than one kind of provider implementation.

### 2.1 Reference providers

Reference providers define and exercise the **contract semantics**.

Today this role is filled by:

- `SyntheticProvider`
- `StubProvider`

These providers are intentionally platform-agnostic and are expected to
remain the clearest executable expression of provider behaviour.

### 2.2 Platform-backed providers

Platform-backed providers adapt a concrete backend API into the CamBANG
provider contract.

Examples include:

- `windows_mediafoundation`
- future `android_camera2`
- future additional platform providers

A platform-backed provider is an **adapter to the contract**, not a
source of truth for what the contract means.

### 2.3 Capability maturity

Provider implementations may differ in capability maturity.

A provider may be:

- **minimal / transitional** — enough to validate architecture and basic
  runtime integration
- **release-quality** — robust negotiation, lifecycle handling,
  timestamping, and functional feature coverage

Capability maturity does **not** change the contract. A minimal provider
may support fewer features, but it must still obey the same lifecycle,
threading, native-object, and timestamp rules.

---

## 3. Architectural rule: canonical semantics come from Core + reference providers + verifier

CamBANG must not allow each platform provider to redefine provider
behaviour in its own image.

The canonical semantics come from:

1. Core architecture documents
2. the platform-agnostic reference providers
3. the provider compliance verifier / harness

Platform-backed providers must conform to that model.

This means, for example:

- provider lifecycle rules are not allowed to vary by platform
- snapshot truthfulness is not allowed to vary by platform
- native-object ownership semantics are not allowed to vary by platform
- backend quirks must be contained inside the provider adapter layer,
  not pushed upward into Core semantics

---

## 4. Responsibilities

### 4.1 Core responsibilities

Core owns and decides:

- arbitration and preemption
- warm timing (`warm_hold_ms`) and teardown scheduling
- validation and normalization of capture / picture configuration
- provider-type defaulting via `StreamTemplate`
- `CBLifecycleRegistry` and destroyed-record retention
- `CBStatePublisher` and snapshot publication
- Godot-facing observable publication semantics

### 4.2 Provider responsibilities

A provider owns and implements:

- backend API calls and native handles
- backend worker threads / loopers required to operate those APIs
- frame acquisition, timestamp extraction or synthesis, and delivery
- mapping backend pixel formats into CamBANG pixel formats
- truthful reporting of owned native object lifecycle to Core
- backend-specific error detection and reporting

Providers execute validated Core intent; they do not reinterpret the
model.

---

## 5. Interface contract

The provider interface is internal to Core. The exact C++ signatures are
an implementation detail; the semantic contract is fixed.

### 5.1 Required semantic capabilities

A provider must support, where applicable:

- endpoint enumeration
- open / close device instance
- create / destroy repeating stream
- start / stop repeating stream
- still-capture trigger where the provider advertises that capability
- patch / spec application through Core-validated flows
- clean provider shutdown

### 5.2 Core validates, provider executes

Core validates and materializes effective request state before calling a
provider.

A provider may reject a request only when:

- the backend cannot support the validated request
- the backend reports a transient or permanent execution failure
- the request is still incomplete / invalid when observed at the
  provider boundary

Providers must return deterministic, explicit error codes for such
rejections.

---

## 6. StreamTemplate and provider-type defaults

Each provider type exposes a deterministic `StreamTemplate` containing:

- `CaptureProfile profile`
- `PictureConfig picture`

The `StreamTemplate` defines that provider type's **default stream
configuration**.

### 6.1 Defaulting boundary

Provider-type defaults are applied by **Core**, not by execution-time
provider repair.

Core merges:

- caller-supplied request
- provider `StreamTemplate`

into the **effective per-stream configuration** supplied to the provider.

Providers must execute that effective configuration.

### 6.2 Provider prohibition

Providers must not silently invent fallback values during
`create_stream()` or `start_stream()` for fields such as:

- width
- height
- pixel format
- picture defaults

If the effective configuration reaching the provider is still invalid or
incomplete, the provider must fail deterministically rather than repair
it silently.

### 6.3 Scope of current defaulting model

The current model treats the provider template as the source of omitted
configuration at the **request/template materialization** boundary.

Providers must not add a second hidden layer of defaulting beneath that.

### 6.4 Requested vs Applied Profile

- A capture profile provided by the caller represents requested configuration.
- Core validates, normalizes, and materializes this into an applied profile.
- The applied profile is the configuration actually governing runtime behaviour.

### 6.5 Snapshot Policy

- Snapshot exposes the applied profile, not merely the requested profile or a version reference.
- Snapshot-visible configuration must reflect the applied profile exactly (no transformation or inference).

---

## 7. Provider event classes and delivery guarantees

Providers communicate facts to Core through the **provider strand**
(see `architecture/provider_state_machines.md` and
`architecture/lifecycle_model.md`).

The strand is the single serialized callback context through which
provider → Core facts are delivered.

Provider facts fall into four event classes:

| Event class | Examples | Delivery policy |
|---|---|---|
| Lifecycle | device opened/closed, stream created/destroyed, stream started/stopped | Non-lossy |
| Native-object | native object created/destroyed | Non-lossy |
| Error | device error, stream error, provider error | Non-lossy |
| Frame | repeating frame delivery, capture frame delivery | Lossy |

### 7.1 Non-lossy classes

Lifecycle, native-object, and error events are authoritative facts.
These must:

- always be delivered once admitted to the strand
- never be silently discarded due to queue pressure
- preserve observed ordering relative to other non-frame facts

### 7.2 Frame class

Frame events may be dropped under pressure, but frame dropping must not:

- suppress lifecycle events
- suppress native-object events
- distort registry truthfulness

---

## 8. Threading discipline

All provider → Core facts must be delivered through the provider strand.

Platform callbacks and worker threads must not call Core lifecycle or
registry services directly.

The provider may use backend-specific worker threads internally, but all
observable facts cross into Core through the single serialized strand
context.

This rule is backend-independent and applies equally to synthetic,
stub, and platform-backed providers.

---

## 9. Lifecycle truthfulness

Providers must report native-object and lifecycle state truthfully.

### 9.1 No normal-operation auto-cascade

During normal API operation, providers must not hide bad lifecycle
ordering by automatically stopping or destroying owned children behind
the caller's back.

Examples:

- `close_device()` must fail if a child stream still exists
- `destroy_stream()` must fail if the stream is still started / producing

The purpose is to preserve truthful ownership boundaries in the registry
and published snapshot.

### 9.2 Shutdown is different

During provider shutdown, ordered internal teardown is allowed so the
provider can release resources cleanly.

Shutdown teardown must still be truthful:

- destruction events must correspond to actual release
- providers must not fabricate destruction for resources that were never
  successfully acquired or released

---

## 10. Native-object reporting

Providers report creation and destruction of CamBANG-owned native
objects to Core.

At minimum, implemented providers normally report:

- `Provider`
- `Device`
- `Stream`
- `FrameProducer`

Creation must be reported when the resource is actually acquired.

Destruction must be reported only when the resource is actually
released.

Providers must not collapse ownership boundaries merely to make the
registry look tidy.

The registry / snapshot model depends on truthful object history,
including diagnostically useful ordering failures.

---

## 11. Timestamp contract

Every frame delivered to Core must carry a contract-valid
`CaptureTimestamp` containing:

- `value`
- `tick_ns`
- `domain`

Providers should use a meaningful monotonic or provider-monotonic domain
where feasible.

Placeholder-shaped timestamps are not contract-valid merely because the
fields are present.

Timestamp validity is backend-independent: synthetic, stub, and
platform-backed providers all owe the same contract.

---

## 12. Compliance strategy for future providers

When adding a new provider, including future platform-backed providers:

1. implement the provider as an adapter to the existing contract
2. define provider-type defaults in `StreamTemplate`
3. route all provider → Core facts through the provider strand
4. report native-object lifecycles truthfully
5. pass the provider compliance verifier / harness
6. contain backend quirks inside the provider adapter layer

Do **not**:

- copy incidental behaviour from a provisional backend adapter and treat
  it as canonical
- add provider-side execution-time defaulting
- normalize invalid lifecycle ordering by silent cleanup
- special-case Core semantics to fit one backend API

---

## 13. Immediate design consequence for upcoming platform providers

A future platform-backed provider such as `android_camera2` should be
implemented against the same contract already exercised by the reference
providers and compliance tools.

That provider may become release-quality before other platform-backed
providers do, but its higher capability level still does not redefine
Core/provider semantics.

Release-quality providers extend functional coverage; they do not change
what the provider contract means.
