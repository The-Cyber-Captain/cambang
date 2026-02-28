# Pattern Module (Synthetic Pixel Rendering)

The Pattern Module provides **provider-agnostic, deterministic synthetic pixel generation** for CamBANG.

It generates patterns on CPU into **packed RGBA/BGRA 32-bit buffers** and is intended for:

- deterministic CI and replay
- functional profiling and stress testing
- validating pipeline behaviour without platform camera dependencies

The module does **not** define provider behaviour, core behaviour, or snapshot semantics. It produces pixels only.

---

## 1. Location and Scope

Location:

- `src/pixels/pattern/`

Scope (v1):

- CPU renderer for packed 4-bytes-per-pixel targets:
  - RGBA8
  - BGRA8
- Base + overlay rendering strategy
- No per-frame allocations in the render path

Explicitly out of scope:

- NV12/YUV renderers
- GPU surfaces
- Provider threading policy
- Core ingestion/publishing policy

---

## 2. Public Types

### 2.1 PatternSpec

`PatternSpec` describes **what to render**.

It is structured so that **base-frame-affecting fields** are distinguishable from
**overlay-affecting fields**, enabling base-frame caching.

Typical fields include:

- geometry (`width`, `height`)
- packed format (`RGBA8`, `BGRA8`)
- base pattern selection + parameters (e.g., seed, grid size, bars)
- overlay toggles (e.g., frame index, timestamp, stream id)

### 2.2 PatternRenderTarget

`PatternRenderTarget` describes **where to render**.

It is a caller-owned buffer description:

- `data` pointer
- `size_bytes`
- `stride_bytes`
- `width`, `height`
- packed format

Renderers must support non-tight strides (row-based rendering/copy).

### 2.3 IPatternRenderer

`IPatternRenderer` is the renderer contract.

Properties:

- provider-agnostic
- deterministic
- does not own output buffers
- does not allocate per frame

The render path accepts dynamic per-frame overlay inputs via a small POD
(e.g., frame index, timestamp, stream ordinal).

### 2.4 CpuPackedPatternRenderer (v1)

`CpuPackedPatternRenderer` implements `IPatternRenderer` for packed RGBA/BGRA targets.

It enforces:

- base-frame caching keyed by base-affecting spec fields
- per-frame overlay patching applied onto the destination buffer
- zero allocations inside the per-frame render function

---

## 3. Rendering Model

### 3.1 Base + Overlay Strategy

Rendering proceeds in two steps:

1. **Base frame generation**
  - computed only when the base-cache key changes
  - stored in an internal tight-packed cache buffer

2. **Per-frame render**
  - copy cached base → destination (stride-aware)
  - apply overlays (small bounded patches)

This avoids full-frame recomputation for dynamic metadata overlays.

### 3.2 Determinism

Render output is deterministic given:

- `PatternSpec`
- the per-frame overlay inputs

No global state or time source is consulted internally; timestamps are supplied by the caller.

---

## 4. Allocation Discipline

The Pattern Module is designed for **no per-frame allocations**.

Allowed allocations:

- base-cache allocation when base-cache key changes
- any one-time static initialization (compile-time tables preferred)

Disallowed in the per-frame render path:

- heap allocations
- string formatting or streaming
- container growth

---

## 5. Performance Expectations (CPU Packed v1)

These patterns are generated on CPU into packed RGBA/BGRA buffers. At high resolution/FPS,
throughput is **memory-bandwidth limited**. This is intended for deterministic CI/replay and
functional profiling. Future renderers may target native camera formats (e.g., NV12) and/or GPU
surfaces.

### 5.1 Guidance Tiers (Not Promises)

- Tier 1 (CI / mobile sanity): ~720p–1080p at modest FPS
- Tier 2 (desktop stress): ~1080p–1440p at higher FPS
- Tier 3 (extreme): requires future NV12/GPU renderer

---

## 6. Renderer Microbenchmark (Smoke Tool)

A smoke-only microbenchmark measures renderer throughput in isolation.

Location:

- `src/smoke/pattern_render_bench.cpp`

Purpose:

- quantify achievable FPS and derived memory bandwidth for a given spec
- validate that packed CPU rendering is bandwidth-limited as expected
- avoid conflating renderer cost with provider, core, dispatcher, or Godot overhead

Build:

- smoke-only target
- not linked into release artifacts
- not part of the provider contract

---

## 7. Relationship to Providers

Providers may produce frames from platform camera APIs or from synthetic sources.

Synthetic providers use the Pattern Module to generate pixel content while preserving the
provider/core contract:

- core remains agnostic to pixel origin
- providers remain responsible for deterministic delivery and correct metadata
- snapshot publication is unchanged