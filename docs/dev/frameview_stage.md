# FrameView Stage (Dev Visibility)

This stage exists to produce a **visible, end-to-end** validation of the CamBANG pipeline inside Godot:

Provider → ingress → core thread → dispatcher → frame sink → mailbox → Godot main thread → ImageTexture.

It is intentionally **dev-only scaffolding**.

## What is temporary

- **CamBANGDevNode** owns `CoreRuntime` for the duration of a play session.
- The **stub provider** is driven from Godot `_process()` for convenience.
- The **LatestFrameMailbox** is a *debug/display sink* that stores only the latest frame.

These choices avoid global state and keep iteration fast while the core contract stabilizes.

## What is intended to survive

- Provider callbacks enqueue facts strictly; the core thread integrates deterministically.
- The dispatcher enforces **release-on-drop**.
- The core offers a **sink hook** (`ICoreFrameSink`) for consuming provider frames.

These are architectural spine elements expected to remain.

## Display-normalized boundary

## Display-normalized boundary

The mailbox stores `cambang::RgbaFrame`, a **tightly packed RGBA8** image.
This is deliberate:

- Godot can upload RGBA8 to `ImageTexture` with minimal glue.
- The mailbox is conservative: it accepts only 32-bit RGBA-class formats.
- Unsupported formats (e.g. NV12, YUY2, RAW) are **dropped and released**
  without conversion.

### Accepted formats (dev visibility phase)

During the Windows Media Foundation visibility phase, it was observed that:

- Many consumer cameras expose only YUV-native formats when MF converters
  are disabled.
- Native RGB32-like formats (`MFVideoFormat_RGB32`,
  `MFVideoFormat_ARGB32`) may or may not be available depending on device.

To improve dev visibility without introducing CPU colour conversion:

- The mailbox accepts:
    - `FOURCC_RGBA` (tightly packed RGBA8)
    - `FOURCC_BGRA` (tightly packed BGRA8)

- BGRA frames are **channel-swizzled to RGBA** before storage.
- No YUV→RGB conversion is performed.
- No compressed formats are converted.

This swizzle is a *byte-order normalization*, not a colourspace conversion,
and exists solely to make 32-bit RGB-class outputs visible in Godot during
development.

### Intentional non-support

The following remain intentionally unsupported in this stage:

- NV12 / YUY2 / other YUV formats
- RAW sensor formats
- MJPG / compressed stream formats

These are dropped deterministically and released immediately.

This prevents accidentally baking CPU conversion into the default
performance path and keeps lifecycle/ownership validation isolated from
format-conversion concerns.

## Production expectation

Real-world providers will frequently deliver YUV (e.g. NV12 on Windows/Android) or RAW formats.

Production display/processing is expected to use *different sinks*, such as:

- GPU-native YUV import + shader conversion
- platform-specific zero-copy texture paths
- compute-based processing

All of these can be introduced without changing ingress, determinism, or ownership rules.
