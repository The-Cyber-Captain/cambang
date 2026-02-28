# FrameView Stage (Dev Visibility – Server-Owned Runtime)

This stage exists to produce a **visible, end-to-end validation** of the CamBANG pipeline inside Godot:

Provider  
→ ingress  
→ core thread  
→ dispatcher  
→ frame sink  
→ mailbox  
→ Godot main thread  
→ ImageTexture

It is intentionally **development-only scaffolding**.

This document reflects the current architecture in which:

- `CoreRuntime` is owned by `CamBANGServer`
- Dev nodes do not own the runtime
- Snapshot publication is canonical and dirty-driven
- Visibility is implemented via a development-only frame sink

This document supplements:

- `core_runtime_model.md`
- `state_snapshot.md`
- `architecture/frame_sinks.md`

It does not redefine canonical architecture.

---

## 1. Architectural Context (Current Model)

### 1.1 Runtime ownership

`CoreRuntime` is owned by `CamBANGServer`, registered as an Engine-level singleton.

- `CamBANGServer.start()` starts the core thread.
- `CamBANGServer.stop()` performs deterministic shutdown.
- `CamBANGServer` owns snapshot publication and exposes:
   - `get_state_snapshot()`
   - `state_published(gen, topology_gen)`

Dev nodes do **not** own `CoreRuntime`.

This aligns with the canonical runtime model.

---

## 2. What Is Dev-Only

The following are development scaffolding and not release-facing API:

- `CamBANGDevNode`
- `CamBANGDevFrameViewNode`
- `LatestFrameMailbox`
- Dev autostart convenience logic
- Stub frame emission from `_process()`
  (stub pixel content is generated via the Pattern Module; the `_process()` driving model remains dev-only convenience)

All dev-only code is gated behind:

```
CAMBANG_ENABLE_DEV_NODES
```

These components may evolve or be removed without affecting canonical architecture.

---

## 3. Dev Node Behaviour

### 3.1 `CamBANGDevNode`

Responsibilities:

- Optionally starts `CamBANGServer` if not already running (dev convenience).
- Optionally stops it if this node started it.
- Drives stub-provider frame emission (dev-only).
- Coordinates visibility stream start/stop (dev-only).

Important:

- It does not own `CoreRuntime`.
- It does not publish snapshots.
- It does not bypass core arbitration.

Autostart behaviour is convenience scaffolding only.

---

## 4. Baseline Snapshot Behaviour

On successful `CamBANGServer.start()`:

- Core begins logically dirty.
- First loop iteration publishes a baseline snapshot.
- First snapshot has:
   - `gen = 0`
   - `topology_gen = 0`
   - valid monotonic `timestamp_ns`

Until that publish occurs:

- `get_state_snapshot()` may return null.

This guarantees each runtime session produces exactly one deterministic baseline snapshot.

---

## 5. Frame Sink Wiring

### 5.1 Core hook

Core exposes:

```
ICoreFrameSink
```

Properties:

- Invoked on the core thread.
- Receives `FrameView` by value (moved).
- Must preserve deterministic release semantics.

Dispatcher enforces release-on-drop before invoking the sink.

---

### 5.2 LatestFrameMailbox (Dev Sink)

`LatestFrameMailbox` is development-only.

It:

- Accepts tightly packed 32-bit RGBA-class frames.
- Normalizes stride.
- Swizzles BGRA → RGBA.
- Drops unsupported formats deterministically.
- Stores only the latest frame.

Mailbox wiring occurs inside `CamBANGServer` when dev nodes are enabled.

It is not part of the production design.

---

## 6. Accepted Formats (Visibility Phase Policy)

Accepted:

- `FOURCC_RGBA`
- `FOURCC_BGRA` (swizzled to RGBA)

Not supported:

- NV12
- YUY2
- MJPG
- RAW formats
- Any YUV format

Unsupported formats are dropped and released immediately.

No YUV → RGB CPU conversion is performed.

This isolates lifecycle validation from color conversion policy.

---

## 7. Stub Provider Visibility

The stub provider remains useful for:

- Lifecycle determinism validation
- Dispatcher release semantics
- Snapshot counter validation
- Visual feedback (synthetic pixel generation)

Stub frames are generated from `_process()` for convenience only.

This does not represent production threading.

Pixel content for stub frames is generated via the provider-agnostic Pattern Module into packed RGBA/BGRA buffers.
Frames enter the pipeline identically to hardware-origin frames; visibility policy remains format-gated and dev-only.

---

## 8. Windows Media Foundation Visibility

When:

```
MF_READWRITE_DISABLE_CONVERTERS = TRUE
```

Only native camera formats are selectable.

Many cameras expose YUV-only formats, resulting in:

- Frames received
- Frames dropped as unsupported
- No visible pixels

This is expected behaviour.

It validates provider ↔ core contract without introducing conversion.

See:

```
docs/dev/windows_mf_visibility_phase.md
```

---

## 9. Determinism Guarantees Preserved

Even in visibility mode:

- All core state mutation occurs on the core thread.
- Provider callbacks remain serialized.
- Release-on-drop discipline is preserved.
- Shutdown remains deterministic.
- Snapshot publication remains dirty-driven.

Visibility scaffolding does not alter invariants.

---

## 10. Production Expectations

Production display paths are expected to use alternative sinks:

- GPU-native YUV import
- Shader-based color conversion
- Platform-native texture paths
- Compute-based processing

These can be introduced without modifying:

- Ingress ordering
- Dispatcher discipline
- Snapshot schema
- Lifecycle invariants

The mailbox is intentionally temporary.

---

## 11. Exit Criteria for This Stage

This dev visibility stage is considered complete when:

- Pixels appear for providers capable of native RGBA-class output.
- Drop-heavy providers behave deterministically.
- Start/stop cycles do not hang.
- Snapshot publication remains correct (`gen`, `topology_gen`).
- No release leaks occur under stress.

Once production GPU paths are introduced, this stage may be retired.

---

End of FrameView Stage (Dev Visibility) document.