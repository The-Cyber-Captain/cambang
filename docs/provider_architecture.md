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

- future `windows_winrt`
- future `android_camera2`
- future additional platform providers

A platform-backed provider is an **adapter to the contract**, not a
source of truth for what the contract means.

### 2.2.x Platform target families

CamBANG is designed to support platform-backed provider families including:

- `windows_winrt` — Windows / WinRT
- `android_camera2` — Android / Camera2
- `linux_v4l2` — Linux / Video4Linux2
- `apple_avfoundation` — macOS / iOS / iPadOS / AVFoundation
- `web_getusermedia` — Web / Media Capture and Streams (`getUserMedia`)

These target families identify the backend API surfaces CamBANG expects to adapt through the provider contract.

They do not imply equal implementation maturity, and they do not alter the canonical rule that platform-backed providers are adapters to the same provider contract.

### 2.3 Capability maturity

Provider implementations may differ in capability maturity.

A provider may be:

- **minimal / transitional** — enough to validate architecture and basic
  runtime integration
- **release-quality** — robust negotiation, lifecycle handling,
  acquisition-timing reporting, and functional feature coverage

Capability maturity does **not** change the contract. A minimal provider
may support fewer features, but it must still obey the same lifecycle,
threading, native-object, and acquisition-timing rules.

---

## 3. Architectural rule: canonical semantics come from Core + reference providers + verifier

CamBANG must not allow each platform provider to redefine provider
behaviour in its own image.

The canonical semantics come from:

1. Core architecture documents
2. the platform-agnostic reference providers
3. the provider compliance verifier

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
- retention of effective `CameraSpec` / `ImagingSpec` runtime truth
- provider-type defaulting via `StreamTemplate`
- `CoreNativeObjectRegistry` and destroyed-record retention
- `SnapshotBuilder`, `IStateSnapshotPublisher`, and snapshot publication
- Godot-facing observable publication semantics

### 4.2 Provider responsibilities

A provider owns and implements:

- backend API calls and native handles
- backend worker threads / loopers required to operate those APIs
- frame acquisition, truthful image-acquisition timing extraction or synthesis, and delivery
- mapping backend pixel formats into CamBANG pixel formats
- truthful reporting of owned native object lifecycle to Core
- backend-specific error detection and reporting

Providers execute validated Core intent; they do not reinterpret the
model.

Reference-provider note:

- `SyntheticProvider` may perform synthetic-only frame-generation work such as
  deterministic exposure-variant synthesis for bracket members. That is part of
  Synthetic's role as an executable reference/test provider, not a requirement
  that platform-backed providers fabricate equivalent source frames.
- Format adaptation is different in kind. Any provider may need to map
  backend-local pixel memory into CamBANG's negotiated packed pixel contract
  such as `FOURCC_RGBA` / `FOURCC_BGRA`. The contract requires truthful mapped
  delivery, but it does not require platform-backed providers to use
  SyntheticProvider's frame-generation strategy to achieve it.

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

For retained specification truth, the split remains explicit:

- `CameraSpec` is the per-camera capability seam
- `ImagingSpec` is the cross-camera / imaging-subsystem capability seam Core
  may use for current admission and validation truth

Providers may receive validated imaging-spec patch/version application through
the internal provider boundary, but they do not redefine what `ImagingSpec`
means.

A provider may reject a request only when:

- the backend cannot support the validated request
- the backend reports a transient or permanent execution failure
- the request is still incomplete / invalid when observed at the
  provider boundary

Providers must return deterministic, explicit error codes for such
rejections.

---

## 6. StreamTemplate, CaptureTemplate, and provider-type defaults

Each provider type exposes deterministic default templates:

- `StreamTemplate` defines default repeating-stream configuration:
  `CaptureProfile profile` + `PictureConfig picture`.
- `CaptureTemplate` defines default still-capture configuration:
  `CaptureProfile profile` + `PictureConfig picture`.

### 6.1 Defaulting boundary

Provider-type defaults are applied by **Core** at materialization and
retention boundaries, not by execution-time provider repair.

Core merges caller-supplied request state with the relevant provider
template into the effective stream or still-capture configuration supplied
to the provider.

Providers must execute that effective configuration.

### 6.2 Provider prohibition

Providers must not silently invent fallback values during
`create_stream()`, `start_stream()`, or `trigger_capture()` for fields
such as:

- width
- height
- pixel / still-result format
- picture defaults

If the effective configuration reaching the provider is still invalid or
incomplete, the provider must fail deterministically rather than repair
it silently.

### 6.3 Scope of current defaulting model

The current model treats the provider template as the source of omitted
configuration at the **request/template materialization** boundary and as
the source for retained baseline profile state.

Providers must not add a second hidden layer of defaulting beneath that.

### 6.4 Requested vs Applied Profile

- A capture profile provided by the caller represents requested configuration.
- Core validates, normalizes, and materializes this into an applied profile.
- The applied profile is the configuration actually governing runtime behaviour.
- The provider default still-capture profile is snapshot-visible as the applied
  still profile with `capture_profile.still.version` /
  `capture_profile_version` equal to `0` until explicitly changed.

### 6.5 Snapshot and format naming policy

- Snapshot exposes the applied profile, not merely the requested profile or a version reference.
- Snapshot-visible configuration must reflect the applied profile exactly (no transformation or inference).
- `capture_profile.still.format`, CoreDeviceRegistry `capture_format`,
  `CaptureRequest.format_fourcc`, and status-panel `capture_fmt` are the same
  provider-agnostic CamBANG FourCC-style still-result format value at different
  layers.
- Public Godot constants for currently exposed raw pixel-buffer format values use
  the `PIXEL_FORMAT_*` naming family, for example
  `CamBANGServer.PIXEL_FORMAT_RGBA`; this is a Godot-facing name for the same
  provider-agnostic FourCC-style integer value, not a separate
  provider/backend format namespace.
- Current implemented displayable still paths use packed pixel formats such as
  `FOURCC_RGBA` / `FOURCC_BGRA`; encoded JPEG or RAW still outputs require
  matching payload-kind/result support and are not implied by writing those
  names into the FourCC-style format field.

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

### 7.3 Admitted-frame release lifetime invariant

Once a provider admits a frame to Core, the frame's payload memory,
release token / `release_user`, and any storage touched by the release
callback must remain valid until Core actually invokes the release callback.

Providers may use provider-internal frame lease/release-safety state to
uphold this invariant. This lease state is provider-internal bookkeeping
and is **not** a registry-visible native object type.

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

### 8.1 Prompt submission and non-reentrancy

Provider API methods reached through `CoreRuntime` / `ProviderBroker`
synchronous paths must be prompt, bounded submission or control operations.
Future platform-backed providers must not:

- perform long backend drains while public / Godot callers are synchronously
  blocked
- wait on core-thread work, Godot-thread work, render-thread work, or provider
  callbacks from inside provider API calls
- synchronously invoke callbacks that can re-enter `CoreRuntime`,
  `ProviderBroker`, Godot, or wait on public / core completion
- re-enter `ProviderBroker` while `active_provider_mutex_` may be held
- treat one provider's stop / shutdown shape as a contract template

Backend-specific long work belongs on provider-owned worker / looper threads.
Completion, errors, and lifecycle truth must be reported back through provider
facts / callbacks according to the provider strand model.

---

## 9. Lifecycle truthfulness

Providers must report native-object and lifecycle state truthfully.

Stream lifecycle reconciliation distinguishes Core-directed lifecycle
transitions from provider-fact lifecycle acknowledgements. Delayed provider facts
that acknowledge already-applied Core-directed transitions must not overwrite
newer Core truth. Non-OK acknowledgement facts still preserve/report error
information, but they do not resurrect or stop a stream through stale lifecycle
truth.

### 9.1 No normal-operation auto-cascade

During normal API operation, providers must not hide bad lifecycle
ordering by automatically stopping or destroying owned children behind
the caller's back.

Examples:

- `close_device()` must fail if a child stream still exists
- `destroy_stream()` must fail if the stream is still started / producing

The purpose is to preserve truthful ownership boundaries in the registry
and published snapshot.

This is a provider/runtime truthfulness rule. It does **not** by itself
forbid future Core/playback orchestration decisions at a higher layer.
No such orchestration is implemented by the current runtime.

Future work may consider (future-only, non-normative)
strictly reduction-facing if introduced at all (close/destroy-facing),
not implicit upward realization (`open/create/start`).

### 9.2 Shutdown is different

During provider shutdown, ordered internal teardown is allowed so the
provider can release resources cleanly.

Once a provider is attached to `CoreRuntime`, attached-provider shutdown is
owned by `CoreRuntime::stop()`. External owners may allocate and store the
provider, but while the runtime is live and the provider is attached they must
keep it alive and must not call `provider->shutdown()` directly. The normal
release order is:

```cpp
runtime.stop();
runtime.attach_provider(nullptr);
// then release/reset provider ownership
```

Detaching before `runtime.stop()` is not the default: Core needs the attached
provider during deterministic shutdown to stop streams, destroy streams, close
devices, and call `provider->shutdown()`.

Shutdown teardown must still be truthful:

- destruction events must correspond to actual release
- providers must not fabricate destruction for resources that were never
  successfully acquired or released

---

## 10. Native-object reporting

Providers report creation and destruction of CamBANG-owned native
objects to Core.

The minimum commonly reported canonical structural nouns remain:

- `Provider`
- `Device`
- `AcquisitionSession`
- `Stream`

These canonical nouns define CamBANG's preferred cross-provider viewing
structure. They do **not** limit native truth to only those categories,
and they do not assert that a platform API uses the same hierarchy.

Creation must be reported when the resource is actually acquired.

Destruction must be reported only when the resource is actually
released.

CamBANG no longer treats `FrameProducer` as a first-class structural noun in
the canonical model.

Production is interpreted through structural context, payload-delivery truth,
and provider-owned native support entities whose lifetime/release matters.
Providers may still realize production seams internally in backend-specific
ways, but those seams are not modeled as separate first-class structural rows.

### Resource-bearing native truth beyond the structural spine

Provider-owned resource-bearing native truth must not depend on a separate
producer row for its right to surface.

When provider-owned native resources or leases have lifetime/release
significance not safely subsumed by parent destruction alone, providers
may report that truth directly.

Examples may include:

- acquired images
- retained samples
- mapped buffers
- attached GPU/native backings
- shared-buffer references
- similar provider-owned resource-bearing leases

These rows remain separate from the canonical structural spine.

### Context placement of resource-bearing native truth

Provider-owned resource-bearing native truth is grouped by the context that
called it into being.

Placement rules:

- **stream-originated** resource-bearing native truth belongs beneath the
  owning `Stream` context
- **capture-originated** resource-bearing native truth belongs beneath the
  owning `AcquisitionSession` context

This rule applies to provider-owned native resources or leases whose
lifetime/release significance is not safely subsumed by parent destruction
alone, including cases such as retained samples, acquired images, mapped
buffers, attached GPU/native backings, shared-buffer references, and similar
resource-bearing native truth.

This placement rule does not require a public `CamBANGStream` to exist for
capture-originated truth, and it does not depend on any `FrameProducer` row.

Native Payload Support is interpreted through this placement rule and is not
parented by a producer-row concept.

### Backing Plan evaluation capability placement

Backing/output-form capability relevant to parent-scoped Backing Plan
evaluation is not treated as one undifferentiated provider-wide input.

CamBANG keeps two levels of truth separate:

- the **provider/runtime envelope capability**, which is the truthful outer set
  available from the current provider/runtime configuration
- the **parent-context capability**, which is the capability of the specific
  Native Payload Support Parent that carries the image-bearing truth for the
  operation

For parent-scoped image-bearing work, the Native Payload Support Parent is:

- `Stream` for stream-originated payload/backing operations
- `AcquisitionSession` for capture-originated payload/backing operations

This distinction is internal architecture truth. It does not require a new
public Godot API surface and it must not be blurred with the parent's Backing
Plan, any result Backing State, result-facing Operation Support, or measured
Access Evidence.

### Native Payload Support as a projection grouping concept

Native Payload Support is the canonical projection grouping concept for
provider-owned native support entities whose lifetime/release matters
independently and which support image-bearing payload/backing truth.

It is not, in this tranche, a required provider-reported native-object type.
Providers continue to report the actual support entities as native truth rows.
Projection/UI may group them beneath a Native Payload Support row within the
relevant `Stream` or `AcquisitionSession` context.

Native `Stream` truth does not automatically imply a matching public
Godot-facing object. Providers may truthfully use stream-like native
resources to service device-level still capture without promoting those
resources into public `CamBANGStream` semantics.

Implementation-scope note (current verified state):

- `SyntheticProvider` reports truthful `AcquisitionSession` realization for both
  stream-backed and capture-only paths.
- The concrete seam is retained while the device has active stream and/or
  capture references, and is retired when those references are released.
- Providers must not fabricate `AcquisitionSession` lifecycle events from
  still-capture callbacks alone when no concrete acquisition-session seam has
  actually been realized.
- Capture-originated native support truth may therefore appear under an
  `AcquisitionSession` without implying a public `CamBANGStream`.

Providers must also report and retire provider-owned native resources
truthfully whenever resource lifetime matters for runtime truth,
ownership diagnostics, leak prevention, queue health, teardown
correctness, or retained-result / backing-resource truth.

This includes retained samples, acquired images, mapped or attached
buffers, shared-buffer references, and similar resource-bearing native
objects or leases whose release is not safely and wholly subsumed by
parent destruction alone.

Providers must not collapse ownership boundaries merely to make the
registry look tidy.

The registry / snapshot model depends on truthful object history,
including diagnostically useful ordering failures.

---

## 11. Image Acquisition Timing contract

A provider may attach source-neutral `ImageAcquisitionTiming` to each delivered
frame when the backend can supply it truthfully. The timing record carries:

- an acquisition mark;
- a rational tick period;
- a declared clock domain;
- a reference event;
- a comparability scope;
- fact origin/provenance.

The acquisition mark is provider-authored descriptive metadata in the declared
clock domain. It is not required to be wall-clock time, globally monotonic,
unique, or comparable with another device/session, and a mark of zero is valid.
Absence must remain distinct from a present zero-valued mark. Canonically, the
mark is a nonnegative signed 64-bit value and the tick-period numerator in
nanoseconds and denominator are positive signed 64-bit values retained in
reduced form.

Providers that start from unsigned or wider native counters must perform one
checked conversion at the provider boundary. If a native acquisition mark
cannot be represented within the canonical nonnegative signed 64-bit range,
acquisition timing remains unavailable rather than wrapping, clamping, or being
reinterpreted.

The delivered frame is the single provider-to-Core transport for acquisition
timing. Providers must not send the same acquisition event through a separate
per-image fact callback.

Core must not use Image Acquisition Timing as retained-frame identity, backing
identity, freshness, ordering, deduplication, lifecycle chronology, or latency
evidence. Core supplies its own retained-frame identity wherever those semantics
are required.

Synthetic, stub, and platform-backed providers owe the same truthfulness rules;
a provider that cannot supply valid timing omits it rather than fabricating a
placeholder.

---

## 12. Compliance strategy for future providers

When adding a new provider, including future platform-backed providers:

1. implement the provider as an adapter to the existing contract
2. define provider-type defaults in `StreamTemplate` and `CaptureTemplate`
3. route all provider → Core facts through the provider strand
4. report native-object lifecycles truthfully
5. pass the provider compliance verifier
6. contain backend quirks inside the provider adapter layer

Do **not**:

- copy incidental behaviour from a provisional backend adapter and treat
  it as canonical
- copy one backend adapter's teardown, callback ownership,
  synchronization, timeout, or stride-handling patterns into another
  provider without an explicit contract reason
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

For the future Release Windows provider specifically, do not copy a
provisional backend adapter unless each unsafe pattern is replaced: no worker
detach while
provider/stream state can still be accessed; no callback lifetime based on raw
provider/stream pointers unless teardown proves drain; one consistent
synchronization discipline for camera/reader handles; correct signed/negative
stride handling or safe normalization; shutdown timeout failure that leaves no
callbacks against destroyed state; and validation for repeated lifecycle,
callback drain, stop/destroy races, timeout paths, and frame release semantics.

## Still capture admission boundary

Provider still-capture entry points are admission/submission boundaries. A
successful provider `trigger_capture(...)` return means the provider has accepted
responsibility for the request or grouped submission and will later report
terminal success or failure through the provider callback/strand path. It does
not mean image payloads have already been acquired/generated, retained,
assembled, or become Godot-visible.

SyntheticProvider may generate pixels internally, but that production is
provider-owned work after admission rather than synchronous public/Core
admission work. Rig capture should be represented to capable providers as one
grouped submission containing all admitted member-device requests for the shared
capture id.
