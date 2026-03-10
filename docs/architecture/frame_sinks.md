# Frame Sinks

> This document supplements `core_runtime_model.md`,
> `provider_architecture.md`, and `architecture/publication_model.md`.
> It does not supersede them.

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

## Dev sink: `LatestFrameMailbox`

`LatestFrameMailbox` is a **development-only** sink and is not part of the
intended release architecture.

It may be removed or replaced once production GPU-native paths are implemented.

`LatestFrameMailbox`:

- accepts tightly packed 32-bit RGBA-class frames
- normalises stride
- drops unsupported formats
- stores only the latest frame

When `CAMBANG_ENABLE_DEV_NODES` is enabled, the mailbox sink is owned and
wired by `CamBANGServer` (which owns `CoreRuntime`). Dev nodes do not own
or wire sinks directly.

The mailbox is attached as a dev-only frame sink during server
initialization. It is not part of the canonical provider ↔ core contract
and does not alter snapshot publication semantics.

### Accepted formats (dev visibility phase)

The mailbox accepts:

- `FOURCC_RGBA` (tightly packed RGBA8)
- `FOURCC_BGRA` (tightly packed BGRA8)

BGRA frames are channel-swizzled to RGBA before storage.

This swizzle is a **byte-order normalisation**, not a colourspace conversion.

Not performed:

- YUV → RGB conversion
- compressed-format conversion

Unsupported formats are dropped and released deterministically.

This sink exists solely for visibility and integration validation.
It is not the intended production path for YUV, RAW, or compressed formats.

Future production sinks may:

- upload YUV planes directly to GPU
- perform shader-based colour conversion
- integrate with platform-native texture import paths

---

## Production sinks

Production paths are expected to use alternative sinks
(not `LatestFrameMailbox`) for performance and feature coverage,
including GPU-native YUV import and shader conversion.

Frame sinks are extension points, not redefinitions of the core provider
contract.

---

## Counter semantics alignment

Frame sinks do not modify stream counter semantics defined in
`state_snapshot.md`.

Specifically:

- `frames_received` increments when core integrates a provider frame
- `frames_delivered` increments when the dispatcher hands a frame to the sink
  (including `LatestFrameMailbox` in dev mode)
- `frames_dropped` increments when the dispatcher drops a frame before sink invocation

Counters are updated by core prior to sink invocation and remain part of
the canonical snapshot model.
