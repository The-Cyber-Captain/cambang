# FrameView Stage (Dev Visibility – Server-Owned Core Loop)

This stage exists to produce a **visible, end-to-end validation** of the
CamBANG pipeline inside Godot:

```text
Provider
→ ingress
→ core thread
→ dispatcher
→ frame sink
→ mailbox
→ Godot main thread
→ ImageTexture
```

It is intentionally **development-only scaffolding**.

This document reflects the current architecture in which:

- `CoreRuntime` is owned by `CamBANGServer`
- dev nodes do not own the core
- core snapshot publication is dirty-driven
- Godot-visible publication is tick-bounded
- visibility is implemented via a development-only frame sink

This document supplements:

- `core_runtime_model.md`
- `state_snapshot.md`
- `architecture/frame_sinks.md`
- `architecture/publication_model.md`

It does not redefine canonical architecture.

---

## 1. Architectural context

### `CoreRuntime` ownership

`CoreRuntime` is owned by `CamBANGServer`, registered as an Engine-level singleton.

- `CamBANGServer.start()` starts the core thread
- `CamBANGServer.stop()` performs deterministic shutdown
- `CamBANGServer` owns snapshot publication and exposes:
  - `get_state_snapshot()`
  - `state_published(gen, version, topology_version)`

Dev nodes do **not** own `CoreRuntime`.

---

## 2. What is dev-only

The following are development scaffolding and not release-facing API:

- `CamBANGDevNode`
- `CamBANGDevFrameViewNode`
- `LatestFrameMailbox`
- dev autostart convenience logic
- stub frame emission from `_process()`

Stub pixel content may be generated through the Pattern Module, but the
`_process()`-driven model remains a dev-only convenience.

All such code is gated behind:

```text
CAMBANG_ENABLE_DEV_NODES
```

These components may evolve or disappear without affecting canonical architecture.

---

## 3. Dev-node behaviour

### `CamBANGDevNode`

Responsibilities:

- optionally starts `CamBANGServer` if not already running
- optionally stops it if this node initiated the start
- drives stub-provider frame emission for development visibility
- coordinates visibility stream start / stop in dev flows

Important non-responsibilities:

- it does not own `CoreRuntime`
- it does not publish snapshots
- it does not bypass core arbitration

Autostart is convenience scaffolding only.

---

## 4. Baseline snapshot behaviour

On successful `CamBANGServer.start()`:

- core begins logically dirty
- core publishes a baseline snapshot as soon as the core loop becomes `LIVE`
- the first snapshot has:
  - `version = 0`
  - `topology_version = 0`
  - valid monotonic `timestamp_ns`

Godot-facing publication remains tick-bounded:

- `state_published(gen, 0, 0)` is emitted on the first Godot tick where
  the running state becomes observable
- until that emission, `get_state_snapshot()` may return `NIL`

This guarantees each generation produces exactly one deterministic baseline.

---

## 5. Frame-sink wiring

### Core hook

Core exposes a sink hook conceptually equivalent to:

```text
ICoreFrameSink
```

Properties:

- invoked on the core thread
- receives `FrameView` by value (moved)
- must preserve deterministic release semantics

The dispatcher enforces release-on-drop before sink invocation.

### `LatestFrameMailbox` (dev sink)

`LatestFrameMailbox` is development-only.

It:

- accepts tightly packed 32-bit RGBA-class frames
- normalises stride
- swizzles BGRA → RGBA
- drops unsupported formats deterministically
- stores only the latest frame

Mailbox wiring occurs inside `CamBANGServer` when dev nodes are enabled.

It is not part of the production design.

---

## 6. Accepted formats (visibility phase policy)

Accepted:

- `FOURCC_RGBA`
- `FOURCC_BGRA` (swizzled to RGBA)

Not supported in this phase:

- NV12
- YUY2
- MJPG
- RAW formats
- other YUV formats

Unsupported formats are dropped and released immediately.

No YUV → RGB CPU conversion is performed.

This isolates lifecycle validation from colour-conversion policy.

---

## 7. Stub-provider visibility

The stub provider remains useful for:

- lifecycle determinism validation
- dispatcher release semantics
- snapshot counter validation
- visual feedback during early integration

Stub frames generated from `_process()` are a convenience only.
They do **not** represent production threading behaviour.

Pixel content may be generated via the provider-agnostic Pattern Module
into packed RGBA/BGRA buffers.

---

## 8. Windows Media Foundation visibility

When:

```text
MF_READWRITE_DISABLE_CONVERTERS = TRUE
```

only native camera formats are selectable.

Many cameras expose YUV-only formats, which leads to:

- frames received
- frames dropped as unsupported
- no visible pixels

This is expected behaviour.

It validates provider ↔ core contract without silently introducing
format conversion.

See:

```text
docs/dev/windows_mf_visibility_phase.md
```

---

## 9. Determinism guarantees preserved

Even in visibility mode:

- all core state mutation occurs on the core thread
- provider callbacks remain serialized
- release-on-drop discipline is preserved
- shutdown remains deterministic
- snapshot publication remains dirty-driven internally
- Godot-visible publication remains tick-bounded

Visibility scaffolding does not alter canonical invariants.

---

## 10. Production expectations

Production display paths are expected to use alternative sinks:

- GPU-native YUV import
- shader-based colour conversion
- platform-native texture paths
- compute-based processing

These can be introduced without modifying:

- ingress ordering
- dispatcher discipline
- lifecycle invariants
- snapshot schema
- publication model

The mailbox is intentionally temporary.

---

## 11. Exit criteria for this stage

This dev visibility stage is considered complete when:

- pixels appear for providers capable of native RGBA-class output
- drop-heavy providers behave deterministically
- start / stop cycles do not hang
- snapshot publication remains correct (`gen`, `version`, `topology_version`)
- no release leaks occur under stress

Once production GPU paths are introduced, this stage may be retired.
