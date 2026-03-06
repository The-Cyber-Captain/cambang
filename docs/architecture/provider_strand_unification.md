# Provider Strand Unification

> This document supplements `provider_architecture.md`,
> `core_runtime_model.md`, and `state_snapshot.md`. It does not
> supersede them.
>
> It captures implementation decisions for enforcing a **single
> serialized provider callback context** ("provider strand") across
> **all provider types**, including platform-backed providers and
> Synthetic / Stub providers.
>
> The goal of this document is to prevent architectural drift during
> refactors.

------------------------------------------------------------------------

## Purpose

CamBANG requires that **provider → core callbacks occur on a single
serialized context**.

Historically, some providers (notably Synthetic and Stub) have delivered
callbacks from multiple threads (for example, Godot tick driving frame
emission).

This document records the decision to unify all providers on the
**provider strand model** (Option 2), ensuring consistent behaviour
across:

-   Platform-backed providers
-   Synthetic providers
-   Stub provider

The strand model guarantees:

-   deterministic event ordering
-   correct snapshot publication behaviour
-   consistent lifecycle reporting
-   portable threading across desktop, mobile, and web targets

------------------------------------------------------------------------

## Conceptual Model

Providers interact with Core using a **single ordered event stream**.

    Provider Backend
        │
        ├─ platform callbacks / scheduler / workers
        │
        ▼
    Provider Strand (single serialized callback context)
        │
        ▼
    Core Provider Event Ingress
        │
        ▼
    Core Runtime Event Integration

Key rule:

Only the provider strand may deliver provider facts to Core. All other
threads must **post events to the strand**, never invoke Core callbacks
directly.

------------------------------------------------------------------------

## Provider Hierarchy (unchanged)

This design **does not alter the canonical lifecycle hierarchy**:

    Provider
     └─ Device
         └─ Stream
             └─ FrameProducer (optional)
                 └─ Frame

Synthetic and Stub providers must **not shortcut this hierarchy**.

Even though synthetic streams do not represent physical cameras, they
must still:

1.  open a Device instance
2.  create Streams owned by that Device
3.  start frame production through a FrameProducer

This ensures identical lifecycle semantics across providers and
consistent native-object registry reporting.

------------------------------------------------------------------------

## Internal Implementation Objects

These objects support the strand model and are **internal implementation
details**.

### Provider → Core plumbing

**CBProviderEvent**\
Envelope describing a provider fact delivered to Core (lifecycle, frame
delivery, errors).

**CBProviderStrand**\
The **single serialized provider callback context**.\
All provider callbacks must pass through this object.

**CBProviderEventPump**\
Optional component that drains the provider strand and forwards events
to the Core provider event queue.

------------------------------------------------------------------------

## Synthetic / Stub Platform Emulation

Synthetic and Stub providers emulate the behaviour of real platform
providers.

Instead of hardware callbacks producing frames, a **scheduler produces
frame work orders**.

**CBSyntheticScheduler** - maintains per‑stream cadence - supports
`virtual_time` and `real_time` - produces deterministic frame work
orders

**CBFrameWorkOrder** - stream id - frame index - capture timestamp -
effective capture profile - picture configuration

**CBPatternRenderService** Interface to the Pattern Module responsible
for generating synthetic pixels.

**CBPatternWorkerPool (optional)** Optional worker pool used to
parallelise pattern generation.

**CBFrameReorderBuffer** Ensures deterministic ordering when worker
threads complete frames out‑of‑order.

------------------------------------------------------------------------

## Synthetic Timing Model

  -----------------------------------------------------------------------
  Timing Driver                            Behaviour
  ---------------------------------------- ------------------------------
  virtual_time                             Frame progression advances
                                           only when the host advances
                                           time

  real_time                                Frame cadence follows a
                                           monotonic clock
  -----------------------------------------------------------------------

Frame delivery still passes through the provider strand in all cases.

------------------------------------------------------------------------

## Truthfulness Rule

Lifecycle events must reflect **actual resource ownership**.

Providers must:

-   emit **created** events when a resource is acquired
-   emit **destroyed** events only when the resource is fully released

Synthetic and Stub providers must follow the same rule as platform
providers.

------------------------------------------------------------------------

## Godot API Invariants

The provider strand refactor **must not alter the public Godot‑facing
API**.

### Public surface

-   No new public classes
-   No removed classes
-   No new required methods

### Behavioural semantics

-   `CamBANGServer.start()` still produces a valid initial snapshot.
-   `CamBANGServer.stop()` still performs deterministic teardown.
-   Device and Stream lifecycle behaviour remains unchanged.

### Threading guarantees

-   Godot main thread does not deliver provider callbacks to Core.
-   Provider callbacks remain serialized and deterministic.

### Snapshot behaviour

-   `gen`, `version`, and `topology_version` semantics remain unchanged.
-   Native-object lifecycle reporting remains truthful.

------------------------------------------------------------------------

## Performance Considerations

The strand architecture must remain efficient across platforms.

Design requirements:

-   worker pools optional
-   bounded queues
-   deterministic drop behaviour
-   compatibility with Web and mobile environments

------------------------------------------------------------------------

## Non‑Goals

This document does **not** redefine:

-   provider architecture
-   core runtime processing
-   snapshot schema
-   Godot API design

Canonical architecture documents remain authoritative.

------------------------------------------------------------------------

## Summary

CamBANG standardises provider → core communication using a **provider
strand**.

This unifies behaviour across:

-   platform-backed providers
-   synthetic providers
-   stub provider

while preserving the existing Godot-facing API and deterministic runtime
model.
