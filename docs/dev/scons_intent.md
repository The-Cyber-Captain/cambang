# CamBANG – SCons Build Intent (Working Note)

> Status: Working Note — Not Frozen  
> This document captures current build-direction intent.
> It may evolve and does not carry architectural authority.
>
> Build system: **SCons-only**. CMake is not supported.

---

## 1. Purpose

This document records the intended direction of CamBANG’s SCons integration
prior to full GDExtension / godot-cpp integration.

It exists to:

- Preserve agreed direction across development phases.
- Avoid accidental drift in toolchain or CLI conventions.
- Maintain alignment with Godot’s official ecosystem practices.

This document is not part of the frozen architecture documentation.

---

## 2. Guiding Principles

### 2.1 Mirror Godot Ecosystem Conventions

CamBANG’s SCons interface mirrors the vocabulary used by:

- godot-cpp-template
- Godot engine SCons builds

Primary arguments:

- `platform`
- `target`
- `arch`
- `precision`

Windows-specific selection (future-compatible):

- `use_mingw`
- `use_llvm`

We avoid inventing custom argument names unless strictly necessary.

---

### 2.2 No Silent Toolchain Selection

On Windows systems where both MSVC and MinGW are present:

- Toolchain choice must be explicit or clearly reported.
- No silent fallback that changes ABI expectations.

When building from Git Bash / MinGW environments:

- `use_mingw=yes` is recommended.

---

### 2.3 Local Builds Are First-Class

CamBANG must:

- Build locally without CI.
- Not depend on cloud tooling.
- Not assume internet access.

CI will be introduced later for release validation and artifact production,
but local iteration remains primary.

---

### 2.4 Future Delegation to godot-cpp

Once GDExtension integration begins:

- CamBANG will delegate toolchain logic to `godot-cpp/SConstruct`
  (as per official template pattern).
- CamBANG SConstruct should become a thin orchestration layer,
  not a compiler-policy authority.
- CamBANG SConstruct is expected to delegate toolchain logic…

---

### 2.5 SCons Is the Single Source of Truth (No CMake)

CamBANG is configured to build via the repo-root `SConstruct`.

- CMake is intentionally removed.
- IDE integration must call SCons (directly or via an IDE “external build” wrapper).

This avoids dual-build drift and keeps CamBANG aligned with the Godot / godot-cpp ecosystem.

---

### 2.6 Clangd support via compile_commands.json

CamBANG provides a SCons-native compilation database generator at:

- `site_scons/site_tools/compdb.py`

How it works:

- The tool wraps SCons compile actions and records compilation commands for C/C++ translation units.
- The compilation database is written to `compile_commands.json` at repo root by default.
- The output path may be overridden with:

  - `COMPDB_PATH=...`

Important:

- `compile_commands.json` is only written **after** a build that executes compilation actions
  (it is not “precomputed”).
- On current scaffolding, the write action is attached to the `gde` build alias, so the typical
  way to refresh the database is to run a normal `scons gde ...` build.

---

## 3. Windows Release Policy (Planned)

For distributed Windows builds:

- Prefer MinGW-w64 for parity with official Godot binaries.
- MSVC may be supported as a development convenience.

This remains subject to validation during GDE integration.

---

## 4. Upstream Discrepancy Policy

If deviations from godot-cpp-template are required:

- The deviation must be documented.
- An upstream issue reference must be logged.
- The workaround must be removable.

Future document:
`docs/dev/upstream_discrepancies.md`

---

## 5. Scope

This document governs:

- Build-system direction.
- Toolchain intent.
- Compatibility philosophy.

It does not govern:

- Runtime architecture.
- Provider lifecycle.
- Core threading model.
- Resource ownership rules.

Those remain under frozen architectural documentation.

**Provider selection scope**

The SCons `provider=...` option selects the single
platform-backed backend implementation compiled into the GDExtension build.

Exactly one platform-backed backend is compiled per build.

Synthetic support is controlled independently via:

- `synthetic=yes|no`

When `synthetic=yes`, the synthetic backend is compiled
as an optional runtime mode within the single provider instance.

At runtime, the provider instance supports:

- `platform_backed` (default)
- `synthetic`

Runtime selection is performed via environment variable, CLI argument,
or Godot-facing configuration (implementation detail).

Switching runtime mode requires provider teardown/restart.

Core always binds to exactly one provider instance at runtime.

The `provider=...` and `synthetic=...` options must not alter the
core smoke executable, which remains stub-provider-only.
---

## 6. GDE Scaffolding Plan (Temporary)

- Initial Godot-facing scaffolding uses `CamBANGDevNode` (temporary).
- `CamBANGServer` (Engine singleton) owns `CoreRuntime`.
- `CamBANGDevNode` may auto-start/auto-stop the server for development
  convenience but does not own the runtime.
- Lifecycle hooks: `_enter_tree()` may start the server (if not already running);
  `_exit_tree()` may stop it only if this node initiated start.
- Single-instance guard applies to dev scaffolding only and does not
  represent final API constraints.

### 6.x Current default targets and knobs (build-phase behavior)

Current build entrypoint behavior (during scaffolding phase):

- Default target builds the GDExtension (`gde`).
- Core smoke is opt-in via `smoke=1`.

When `smoke=1` is provided, the default build may additionally build the `smoke` target alongside `gde` 
(implementation detail of the current scaffolding SConstruct). If you want smoke only, build the alias explicitly:

- `scons smoke=1 smoke`

Godot compatibility target:

- Godot 4.5+.
- Release artifacts are expected per minor series (4.5, 4.6, 4.7, …), with matching `godot-cpp`.

Build strategy:

- Follow godot-cpp-template conventions (`platform/target/arch/precision`).
- Delegate toolchain logic to `godot-cpp` SCons as soon as the submodule is introduced.

### 6.y Windows Media Foundation + MinGW notes (scaffolding)

When building the Windows Media Foundation provider under MinGW, the link set must include:

- `mf` (required for `MFEnumDeviceSources`)
- `mfplat`
- `mfreadwrite`
- `mfuuid`
- `ole32`
- `uuid` (required for `GUID_NULL` on MinGW)

Additionally, MinGW headers may not expose some IMFMediaType convenience accessors consistently; prefer MF helper
functions (e.g., `MFGetAttributeSize`, `MFGetAttributeUINT32`) over relying on `GetINT32`.

### 6.z Core Smoke Executable Is Opt-In and Provider-Independent

CamBANG maintains a small **core smoke executable** whose purpose is to validate core invariants quickly, without involving Godot or platform camera stacks.

**Location**
- `src/smoke/core_spine_smoke.cpp`

**Build**
- Opt-in via SCons: `scons smoke=1 ...`
- Output: `out/core_spine_smoke(.exe)`

**Policy**
- The smoke executable is **stub-provider-only** by design.
- It must remain **independent of `provider=...` selection**.
- It is not part of the GDExtension artifact and is **not required for typical developer builds**.

**Rationale**
Provider backends (especially platform APIs like Windows Media Foundation) are allowed to evolve independently. The smoke executable must remain a fast, deterministic check that cannot be destabilized by platform/provider work.

**Naming**
- Smoke-only code paths use `CAMBANG_INTERNAL_SMOKE`.
- Legacy “IDE smoke” nomenclature has been removed.

When built with `smoke=1`, the smoke executable prints a banner
indicating the compiled provider mode (e.g. stub-only). This is a
diagnostic convenience and does not alter the stub-only invariant.

### Stress Mode (Smoke Harness)

The `core_spine_smoke` executable supports an optional stress mode:

--stress
--loops=N
--jitter_ms=K
--seed=S

Stress mode performs repeated lifecycle churn using the stub provider to validate:

- Deterministic shutdown behaviour
- Release-on-drop under overload
- No frame leaks across stop()
- EXIT phase reached on every teardown
- Admission gating during teardown

This mode remains stub-provider-only and must not depend on platform providers.