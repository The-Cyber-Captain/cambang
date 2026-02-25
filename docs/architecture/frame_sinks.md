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

Release-on-drop discipline must always be preserved.

------------------------------------------------------------------------
## Dev sink: LatestFrameMailbox

`LatestFrameMailbox` is a **dev-only** sink used to provide visible output in Godot.

LatestFrameMailbox is a development-stage sink that:

-   Accepts tightly packed 32-bit RGBA-class frames
-   Normalises stride
-   Drops unsupported formats
-   Stores only the latest frame

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