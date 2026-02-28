# Frame Sinks
> This document supplements `core_runtime_model.md` and `provider_architecture.md`.
> It does not supersede them.

---

## Purpose

Frame sinks define the boundary between the core runtime and downstream
consumers of FrameView data.

They exist to:

-   Preserve deterministic release semantics
-   Isolate display/debug paths from provider contract semantics
-   Allow alternative consumption paths (GPU-native, shader-based, etc.)

------------------------------------------------------------------------

CamBANG core integrates provider callbacks on a dedicated core thread. Provider frames are delivered to
core as `FrameView` payloads (provider-owned until released).

## Core hook

`ICoreFrameSink` is a minimal core-side extension point:

- Called on the **core thread**.
- Receives a `FrameView` by value (moved in).
- Must ensure deterministic release semantics.

The dispatcher remains the single place that enforces release-on-drop.
The ICoreFrameSink interface is invoked on the core thread.

Implementations must:

-   Either copy frame data immediately and release
-   Or take ownership and guarantee deterministic release

The dispatcher transfers ownership of the FrameView to the sink
only after ensuring the frame is not dropped.
Once delivered to the sink, the sink becomes responsible for
deterministic release.

Release-on-drop discipline must always be preserved.

------------------------------------------------------------------------
## Dev sink: LatestFrameMailbox

`LatestFrameMailbox` is a **development-only** sink and is not part of
the intended release architecture. It may be removed or replaced
once production GPU-native paths are implemented.
LatestFrameMailbox is a development-stage sink that:

-   Accepts tightly packed 32-bit RGBA-class frames
-   Normalises stride
-   Drops unsupported formats
-   Stores only the latest frame

When `CAMBANG_ENABLE_DEV_NODES` is enabled, the mailbox sink is owned
and wired by `CamBANGServer` (which owns `CoreRuntime`). Dev nodes
never own or wire sinks directly.

The mailbox is attached as a development-only frame sink during
server initialization. It is not part of the canonical provider ↔ core
contract and does not affect snapshot publication semantics.

### Accepted formats (dev visibility phase)

The mailbox accepts:

-   `FOURCC_RGBA` (tightly packed RGBA8)
-   `FOURCC_BGRA` (tightly packed BGRA8)

BGRA frames are channel-swizzled to RGBA before storage.

This swizzle is a **byte-order normalization**, not a colourspace conversion.

No YUV→RGB conversion is performed.
No compressed formats are converted.

Unsupported formats are dropped and released deterministically.

This sink exists solely for visibility and integration validation.
It is not the intended high-performance production path for YUV, RAW,
or compressed camera formats.

Future production sinks may:

-   Upload YUV planes directly to GPU
-   Perform shader-based colour conversion
-   Integrate with platform-native texture import paths

------------------------------------------------------------------------

## Production sinks

Production paths are expected to use alternative sinks (not `LatestFrameMailbox`) for performance,
including GPU-native YUV import and shader conversion.


Frame sinks are extension points, not redefinitions of the core provider
contract.

Frame sinks do not alter stream counter semantics defined in
state_snapshot.md; counters are updated by core prior to
sink invocation.
------------------------------------------------------------------------

## Counter Semantics Alignment

Frame sinks do not modify stream counter semantics defined in
`state_snapshot.md`.

Specifically:

- `frames_received` increments when core integrates a provider frame.
- `frames_delivered` increments when the dispatcher hands a frame to
  the sink (including `LatestFrameMailbox` in dev mode).
- `frames_dropped` increments when the dispatcher drops a frame prior
  to sink invocation.

Counters are updated by core prior to sink invocation and are part of
the canonical snapshot model.