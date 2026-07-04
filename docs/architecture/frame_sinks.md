# Frame Sinks

> This document supplements `core_runtime_model.md`,
> `provider_architecture.md`, and `architecture/publication_model.md`.
> It does not supersede them.
> 
> > Current release-facing payload/result contract:
> See `docs/architecture/pixel_payload_and_result_contract.md`.
>
> `frame_sinks.md` defines the internal/runtime sink boundary.
> The payload/result contract defines the retained-result, ownership,
> and materialization model used by release-facing `Stream Result`,
> `Capture Result`, and `Capture Result Set`.

---

## Purpose

Frame sinks define the boundary between the core runtime and downstream
consumers of `FrameView` data.

They exist to:

- preserve deterministic release semantics
- isolate display / debug paths from provider contract semantics
- allow alternative consumption paths such as GPU-native or shader-based pipelines

CamBANG core integrates provider callbacks on a dedicated core thread.
Provider frames are delivered to core as `FrameView` payloads
(provider-owned until released).

---

## Core hook: `ICoreFrameSink`

`ICoreFrameSink` is a minimal core-side extension point.

Properties:

- called on the **core thread**
- receives a `FrameView` by value (moved in)
- must ensure deterministic release semantics

The dispatcher remains the single place that enforces release-on-drop.
The sink interface is invoked only after the dispatcher decides the frame
is not being dropped.

Implementations must:

- either copy frame data immediately and release it
- or take ownership and guarantee deterministic release

Once delivered to the sink, the sink becomes responsible for release.

Release-on-drop discipline must always be preserved.

---

## Relationship to publication

Frame sinks participate in frame ingestion and dispatch, but they do **not**
define Godot-visible snapshot publication semantics.

Observable publication is defined separately by the tick-bounded model
described in:

- `architecture/publication_model.md`
- `architecture/godot_boundary_contract.md`

This separation is important:

- frame sinks consume frame payloads
- snapshot publication exposes runtime truth to Godot consumers

Frame cadence must not redefine signal cadence.

---

## Current sink status

`ICoreFrameSink` remains the internal extension point for accepted provider
frames that leave the dispatcher under deterministic release rules.

The current release-facing image model is result-oriented. Godot-facing image
access is expressed through `Stream Result`, `Capture Result`, and
`Capture Result Set`, not through developer sink storage terminology.

Frame sinks are extension points, not redefinitions of the core provider
contract. Any future named sink implementation must be introduced deliberately
in source and documented as current behaviour at the same time.

## Release-facing naming and role split

Frame sinks are an **internal/runtime boundary concept**.

They define how accepted `FrameView` payloads leave the core runtime and
enter downstream handling under deterministic ownership/release rules.

They do **not** by themselves define the Godot-facing image access API.

### Godot-facing image access

Release-facing/public API should be expressed in **result-oriented** terms
rather than sink-storage terms.

Canonical Godot-facing image-access nouns are:

- **Stream Result**
- **Capture Result**
- **Capture Result Set**

These describe what the user/runtime-visible API exposes, not which sink
implementation populated that result.

### Internal sink specialization vocabulary

When implementation discussion needs to distinguish repeating-stream and
still-capture paths, prefer:

- **Stream Sink**
- **Capture Sink**

This keeps sink terminology aligned with the corresponding public runtime concepts:

- repeating stream output
- still-capture output

without promoting development-only sink/storage semantics into release
architecture.

### Initial release-facing mapping

The initial release-facing image-access model uses a result-oriented split
between repeating-stream and still-capture paths.

#### Stream Sink → Stream Result

A **Stream Sink** is the internal/runtime path responsible for handling
accepted repeating-stream `FrameView` payloads for downstream stream-image
access purposes.

In the initial release-facing model:

- accepted repeating-stream payloads may populate the latest retained
  **Stream Result**
- this is a result-oriented release-facing model, not public sink-storage semantics
- the model does not imply that every flowing stream frame is retained,
  exported, or fanned out

This mapping is intentionally compatible with later expansion to
additional stream-consumption paths such as recording, broadcast, or
third-party hand-off without redefining the public **Stream Result** noun.

#### Capture Sink → Capture Result

A **Capture Sink** is the internal/runtime path responsible for handling
accepted still-capture payloads for downstream still-image access purposes.

**Capture Sink** populates **Capture Result** objects as device-level still-capture results. Each Capture Result has a default image; when no bracketing is involved, that default image is the only image, and when bracketing is involved, additional bracket images may be represented within the same Capture Result.

Rig-triggered grouped still capture is exposed publicly as a **Capture Result Set**, the rig/Core-curated grouping of selected device **Capture Result** objects for that trigger; this curation remains distinct from the definition of **Capture Result**.

### Initial non-goals

This model does not by itself define:

- final GPU-native presentation architecture
- full stream-sequence / recording / broadcast APIs
- complete third-party fanout design
- a requirement that release-facing stream access reuse any specific internal
  sink implementation or storage terminology

---

## Counter semantics alignment

Frame sinks do not modify stream counter semantics defined in
`state_snapshot.md`.

Specifically:

- `frames_received` increments when core integrates a provider frame
- `frames_delivered` increments when the dispatcher hands a frame to an installed
  core frame sink
- `frames_dropped` increments when the dispatcher drops a frame before sink invocation

Counters are updated by core prior to sink invocation and remain part of
the canonical snapshot model.

Visibility-path diagnostics that are retained by core and exported through the
snapshot are:

- `visibility_frames_presented`
- `visibility_frames_rejected_unsupported`
- `visibility_frames_rejected_invalid`
- `visibility_last_path`

These are sink-neutral stream truth fields. They reflect the current retained
visibility path disposition without exposing sink-local storage internals.

Current visibility rejection counters are still interim semantics. During this
phase, valid GPU-primary/no-CPU-payload frames may temporarily surface under the
nearest existing unsupported/format-style rejection bucket; these should not be
read as authoritative result-correctness failures.
