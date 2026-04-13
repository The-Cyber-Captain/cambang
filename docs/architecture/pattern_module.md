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


## 2.3 IPatternRenderer

`IPatternRenderer` defines the provider-agnostic contract for pixel generation.

Properties:

- Deterministic given `PatternSpec` and per-frame overlay inputs.
- Does not own output buffers.
- Does not allocate per frame.
- Must support non-tight row strides.

The renderer must not consult global time or external state.  
All per-frame variation must be supplied explicitly by the caller.

---

## 2.4 CpuPackedPatternRenderer (v1)

`CpuPackedPatternRenderer` implements `IPatternRenderer` for packed
RGBA8 / BGRA8 targets.

It enforces:

- Base-frame caching keyed by base-affecting spec fields.
- Per-frame overlay patching applied after base rendering.
- Zero allocations inside the per-frame render path.
- Support for both cacheable and dynamic base algorithms.

### Renderer Function Contract (Unified Base Function Signature)

All base-render functions inside `CpuPackedPatternRenderer` share the
same signature:

    void render_base_X(
        uint8_t* dst,
        uint32_t dst_stride_bytes,
        const PatternSpec& spec,
        const PatternBaseKey& key,
        const PatternOverlayData& overlay);

Where:

- `dst` and `dst_stride_bytes` describe the destination buffer.
- `PatternSpec` supplies preset parameters.
- `PatternBaseKey` contains geometry and base-affecting fields.
- `PatternOverlayData` supplies per-frame inputs (e.g. frame index).

This unified signature allows:

- Rendering into the internal tight base-cache buffer (cacheable patterns).
- Rendering directly into the caller’s destination buffer (dynamic-base patterns).

Static (cacheable) patterns may ignore `overlay`.  
Dynamic-base patterns may use per-frame inputs as part of base generation.

All base-render functions must match this signature so they can be
stored in the algorithm dispatch table.

---

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

### 2.7 Selection structures (caller-defined)

The Pattern Module’s public contract for rendering is `PatternSpec`.

Callers (providers, tools, bindings) may define lightweight selection
structures that choose presets and parameters and convert them into
`PatternSpec`.

The Pattern Module does not define core/provider configuration concepts;
those are defined in canonical architecture documentation.

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

Rendering proceeds in two conceptual stages:

1. **Base frame generation**
- May be cached or dynamic (see below).
2. **Per-frame render**
- Copy base → destination (if cacheable).
- Apply overlays (small bounded patches).

This avoids full-frame recomputation when only metadata overlays change.

### 3.1.x Cacheable vs Dynamic Base

Patterns are classified as either:

#### Cacheable Base

Base content depends only on `PatternSpec` base-affecting fields.

- A base-cache key is derived from those fields.
- Base pixels are recomputed only when the key changes.
- Per-frame rendering performs a stride-aware copy followed by overlays.

This is the default behaviour for most patterns.

#### Dynamic Base

Base content depends on per-frame inputs (e.g., frame index).

- The base cache is bypassed.
- The base is rendered directly into the destination buffer each frame.
- Per-frame inputs are supplied via `PatternOverlayData`.

Dynamic-base patterns must **not** include per-frame values in the base-cache key.

This mechanism allows animated patterns (e.g., animated noise) without
misusing overlay toggles or introducing hidden time sources.


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

## 7.x Producer backing capability boundary

The Pattern Module’s current v1 implementation is CPU packed only.

However, producer-side backing capability should be understood as a boundary
below provider policy rather than as a permanent architectural CPU-only limit.

Future producer implementations may advertise backing capability such as:

- CPU-backed realization available
- GPU-backed realization available

while preserving the same higher-level pattern/preset semantics.

This does not alter the Pattern Module’s determinism rule:

- per-frame/source evolution remains driven by caller-supplied inputs
- the renderer must not consult hidden time or global state

Provider policy may choose among the backing kinds actually available from the
producer implementation in the current runtime.
---

## 8. How to Add a New Pattern

Adding a new pattern must be a single-source operation.

### Step 1 — Add Canonical Definition

Add one entry to:

    src/pixels/pattern/pattern_defs.inc

This defines:

- `PatternPreset` enum entry
- Stable string token
- Display name
- Capability flags
- Associated `PatternAlgoId`
- Whether the pattern is cacheable or dynamic-base

No other preset lists or enums should be modified.

---

### Step 2 — Implement the Algorithm

Implement a base-render function in `CpuPackedPatternRenderer`
matching the unified signature:

    void render_base_X(
        uint8_t* dst,
        uint32_t dst_stride_bytes,
        const PatternSpec& spec,
        const PatternBaseKey& key,
        const PatternOverlayData& overlay);

Guidelines:

- Write pixels into `dst` using `dst_stride_bytes`.
- Use `PatternSpec` for preset parameters.
- Use `overlay` only if the pattern is dynamic-base.
- Do not allocate memory.
- Do not access global time.

Notes: If two patterns share logic (e.g. static vs animated variants), prefer a shared helper to avoid duplicated inner loops. See: `render_base_noise_common` 
---

### Step 3 — Register in the Dispatch Table

Add the function pointer to the renderer’s `PatternAlgoId` dispatch table.

The dispatch table order must match the generated `PatternAlgoId` enum.

No switch statements over presets should exist.

---

### Step 4 — Update Base-Cache Key (If Required)

If the pattern is cacheable and introduces new base-affecting parameters:

- Extend `PatternSpec`.
- Extend `PatternBaseKey`.
- Ensure all base-affecting fields are included in the key.

Failure to include a base-affecting parameter in the key will cause
incorrect cache reuse.

Dynamic-base patterns must not include per-frame values in the key.

---

### Step 5 — Validate via Bench Tool

Use:

    pattern_render_bench --pattern=<name> [flags...]

The bench enumerates presets from the registry and must not hardcode lists.

Validate:

- Determinism
- Correct cache behaviour
- No per-frame allocations
- No duplicated preset lists

---

## 9. Architectural Invariants (Pattern Module)

The following invariants protect the Pattern Module from structural drift.

### 9.1 Single Source of Truth

All patterns must be defined in exactly one location:

    src/pixels/pattern/pattern_defs.inc

This canonical definition list generates:

- `PatternPreset`
- `PatternAlgoId`
- The preset registry table
- The preset → algorithm mapping
- Cacheable vs dynamic-base classification

If adding a new pattern requires modifying more than:

1. One entry in `pattern_defs.inc`, and
2. One base-render function implementation,

then the architecture has regressed.

No parallel preset lists, switch statements over presets, or duplicated
enum definitions are permitted.

---

### 9.2 Base Cache Integrity

For cacheable patterns:

- All base-affecting fields must be included in `PatternBaseKey`.
- Per-frame values (e.g. frame index, timestamp) must not be included in the base key.

For dynamic-base patterns:

- The base cache must be bypassed.
- Per-frame inputs must be supplied explicitly via `PatternOverlayData`.
- The renderer must not consult hidden time sources.

---

### 9.3 Determinism

Render output must be deterministic given:

- `PatternSpec`
- `PatternOverlayData`

No global state, hidden PRNG state, or external clocks may influence output.

---

### 9.4 No Per-Frame Allocation

The per-frame render path must not:

- Allocate memory
- Grow containers
- Perform string formatting

Base-cache reallocation is permitted only when the base-cache key changes.

---

### 9.5 Backend Independence

Preset identity (`PatternPreset`) and algorithm identity (`PatternAlgoId`)
must remain decoupled.

Renderer backends (CPU packed, future NV12, GPU, etc.) must dispatch
based on `PatternAlgoId` and must not duplicate preset vocabulary.

This separation ensures that adding new render backends does not require
changing preset identity.