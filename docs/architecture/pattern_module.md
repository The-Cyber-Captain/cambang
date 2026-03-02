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

### 2.5 PatternPreset (Preset Vocabulary)

`PatternPreset` is the **canonical preset vocabulary** used to select patterns.

Properties:

- **Zero-indexed** contiguous enum (`0..N-1`)
- Stable within a build
- Used by programmable surfaces (providers, tests, and future GDScript bindings)

The Pattern Module also defines a **1:1 mapping** between `PatternPreset` and a stable string token
used by edge surfaces (CLI/UI).

### 2.5.x Preset Identity vs Algorithm Identity

CamBANG separates external preset vocabulary from internal algorithm identity.

- `PatternPreset`  
  Stable, zero-indexed enum used by programmable surfaces (providers, tools, bindings).

- `PatternAlgoId`  
  Internal renderer algorithm selector used by renderers.

Multiple presets may map to the same algorithm, and algorithm dispatch
is table-driven by `PatternAlgoId`.

This separation prevents preset vocabulary changes from coupling directly
to renderer switch logic and allows future backend expansion (e.g. NV12,
GPU) without altering preset identity.

### 2.6 Preset Registry (Single Source of Truth)

The Pattern Module exposes a preset registry table that is the only authority for:

- which presets exist
- the stable token name (`"xy_xor"`, `"solid"`, etc.)
- optional display strings (UI-facing)
- capability flags (which parameters are meaningful)

Consumers must not duplicate preset lists. CLI tools and UI should enumerate the registry.

### 2.6.x Canonical Pattern Definition List

The preset registry, preset enum, and algorithm identity are generated from a
single canonical definition list located in:

    src/pixels/pattern/pattern_defs.inc

This file is an intentionally multi-include X-macro fragment and is the
**only location where new patterns are defined**.

From this definition list, the build generates:

- `PatternPreset` (external preset vocabulary)
- `PatternAlgoId` (internal algorithm identity)
- The preset registry table (token, display name, caps)
- The preset → algorithm mapping

Contributors must not introduce parallel enums, switches, or preset lists.
All new patterns begin with a single entry in `pattern_defs.inc`.

### 2.7 ActivePatternConfig (Provider-facing Selection)

`ActivePatternConfig` is a provider-facing configuration describing the **active selected preset**
and its parameters.

Properties:

- POD / trivially copyable fields (no strings, no heap ownership)
- May be swapped at runtime by providers (copy-on-write selection)
- Converted into renderer-facing `PatternSpec` via `to_pattern_spec(...)`

This keeps `PatternSpec` as the renderer contract, while allowing providers and tools to work with a
stable preset vocabulary.

### 2.8 Invalid Selection Behaviour

The Pattern Module defines deterministic behaviour for invalid selection requests:

Provider/runtime (enum/index selection):

- If an invalid `PatternPreset` value is observed (out of range / no registry entry), it is treated as
  **invalid** and the render request must deterministically **fall back** to the default preset
  (typically `XyXor`).
- Providers should increment a small counter (and may log once in dev) when this occurs.

Edge surfaces (string selection):

- If an invalid preset name is requested (not present in the registry), the caller should treat this as
  an **input error** (CLI tools should exit non-zero; UI should refuse selection).
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

Usage:

- `pattern_render_bench --pattern=<name> [--seed=<u32>] [--rgba=R,G,B[,A]] [--checker_size=<px>]`

Valid pattern names are enumerated from the preset registry; the tool must not hardcode a preset list.
---

## 7. Relationship to Providers

Providers may produce frames from platform camera APIs or from synthetic sources.

Synthetic providers use the Pattern Module to generate pixel content while preserving the
provider/core contract:

- core remains agnostic to pixel origin
- providers remain responsible for deterministic delivery and correct metadata
- snapshot publication is unchanged

---

## 8. How to Add a New Pattern

Adding a new pattern must be a single-source operation.

### Step 1 — Add Canonical Definition

Add one entry to:

    src/pixels/pattern/pattern_defs.inc

This defines:

- Preset enum entry
- Stable string token
- Display name
- Capability flags
- Associated `PatternAlgoId`

No other registry updates are required.

---

### Step 2 — Implement the Algorithm (if new)

If the pattern introduces a new algorithm:

- Implement the base render function inside the appropriate renderer
  (e.g., `CpuPackedPatternRenderer`).
- Register the function in the renderer’s algorithm dispatch table.

Algorithm dispatch is table-driven via `PatternAlgoId`.
No switch statements over presets should exist.

---

### Step 3 — Extend Parameters (If Required)

If the pattern introduces new parameters:

1. Extend `ActivePatternConfig` (POD only).
2. Extend `PatternSpec`.
3. Add any base-affecting fields to the base-cache key.

Capability flags must describe which parameters are valid for the preset.

---

### Step 4 — Validate via Bench Tool

The smoke benchmark tool (`pattern_render_bench`) enumerates presets
directly from the registry and must not hardcode preset lists.

Verify:

- Determinism
- Correct cache behaviour
- No per-frame allocations
- No switch duplication