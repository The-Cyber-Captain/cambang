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

### 2.6 Preset Registry (Single Source of Truth)

The Pattern Module exposes a preset registry table that is the only authority for:

- which presets exist
- the stable token name (`"xy_xor"`, `"solid"`, etc.)
- optional display strings (UI-facing)
- capability flags (which parameters are meaningful)

Consumers must not duplicate preset lists. CLI tools and UI should enumerate the registry.

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

This section defines the required steps to add a new pattern in a way that keeps all surfaces
(CLI tools, providers, future Godot UI) in sync.

### 8.1 Implement the Base Pattern

1. Add a new base selection (if required) to the renderer’s base-pattern enum (e.g. `PatternSpec::BasePattern`).
2. Implement generation inside `CpuPackedPatternRenderer`.
3. Ensure **base-frame-affecting fields** are part of the base-cache key (per §3.1).

### 8.2 Expose the Pattern via the Preset Registry

1. Add a new `PatternPreset` enum entry.
- Must be **zero-indexed** and contiguous.
2. Add a new registry entry mapping:
- `PatternPreset` ↔ stable `name` token (CLI/UI)
- capability flags (which parameters apply)

All consumers must enumerate the registry and must not duplicate preset lists.

### 8.3 Extend ActivePatternConfig (If Needed)

If the new pattern requires parameters:

1. Add POD fields to `ActivePatternConfig` for the parameter(s).
2. Map those fields into `PatternSpec` inside `to_pattern_spec(...)`.
3. Ensure parameters that alter the base image are treated as base-frame-affecting (cache key).

### 8.4 Optional: Extend the Microbenchmark Parser

If the microbenchmark should support controlling new parameters:

- Add new CLI flags.
- Validate combinations using preset capability flags (reject parameters for presets that do not support them).

### 8.5 Test Checklist

- Run `pattern_render_bench --pattern=<name>` for the new preset.
- Verify determinism by repeating with fixed `--seed`.
- Build and run provider smoke targets; smoke should remain deterministic because it uses provider defaults.