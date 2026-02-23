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

---

### 2.5 SCons Is the Single Source of Truth (No CMake)

CamBANG is configured to build via the repo-root `SConstruct`.

- CMake is intentionally removed.
- IDE integration must call SCons (directly or via an IDE “external build” wrapper).

This avoids dual-build drift and keeps CamBANG aligned with the Godot / godot-cpp ecosystem.

---

### 2.6 IDE / clangd Support via compile_commands.json

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

---

## 6. GDE Scaffolding Plan (Temporary)

- Initial Godot-facing scaffolding uses `CamBANGDevNode` (temporary).
- `CamBANGDevNode` owns a `CoreRuntime` for the duration of a play session.
- Lifecycle hooks: `_enter_tree()` starts, `_exit_tree()` stops.
- Single-instance guard is enabled to emulate final server-like layout.

Godot compatibility target:

- Godot 4.5+.
- Release artifacts are expected per minor series (4.5, 4.6, 4.7, …), with matching `godot-cpp`.

Build strategy:

- Follow godot-cpp-template conventions (`platform/target/arch/precision`).
- Delegate toolchain logic to `godot-cpp` SCons as soon as the submodule is introduced.
