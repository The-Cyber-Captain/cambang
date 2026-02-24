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

The mailbox stores `cambang::RgbaFrame`, a **tightly packed RGBA8** image. This is deliberate:

- Godot can upload RGBA8 to `ImageTexture` with minimal glue.
- The mailbox is conservative: it only accepts provider `FrameView` with `FOURCC_RGBA`.
- Unsupported formats (e.g. NV12/YUY2/RAW) are **dropped and released** without conversion.

This prevents accidentally making CPU YUV/RAW conversion “the default” performance path.

## Production expectation

Real-world providers will frequently deliver YUV (e.g. NV12 on Windows/Android) or RAW formats.

Production display/processing is expected to use *different sinks*, such as:

- GPU-native YUV import + shader conversion
- platform-specific zero-copy texture paths
- compute-based processing

All of these can be introduced without changing ingress, determinism, or ownership rules.
