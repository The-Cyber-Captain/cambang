# CamBANG Build and Scaffolding

This document records development-stage notes for building CamBANG and
the Godot GDExtension scaffolding.

These notes are **not canonical architecture**. They capture build-system
direction, scaffolding constraints, and repeatable local workflow.

Build system: **SCons-only**. CMake is not supported.

---

## 1. Purpose and status

This document preserves agreed build direction and development scaffolding
behaviour across implementation phases.

It exists to:

- preserve build-system intent
- avoid accidental toolchain or CLI drift
- document GDE scaffolding constraints discovered in practice
- describe the repeatable local Windows / MinGW build loop

This document does **not** define runtime architecture, provider lifecycle,
resource ownership rules, or public API semantics.

---

## 2. Guiding principles

### 2.1 Mirror Godot ecosystem conventions

CamBANG’s SCons interface mirrors the vocabulary used by:

- `godot-cpp-template`
- Godot engine SCons builds

Primary arguments:

- `platform`
- `target`
- `arch`
- `precision`

Windows-specific selection (future-compatible):

- `use_mingw`
- `use_llvm`

CamBANG avoids inventing custom argument names unless strictly necessary.

### 2.2 No silent toolchain selection

On Windows systems where both MSVC and MinGW are present:

- toolchain choice must be explicit or clearly reported
- no silent fallback should change ABI expectations

When building from Git Bash / MinGW environments:

- `use_mingw=yes` is the recommended explicit choice

### 2.3 Local builds are first-class

CamBANG must:

- build locally without CI
- not depend on cloud tooling
- not assume internet access

CI may later validate release flows and artifacts, but local iteration
remains primary.

### 2.4 Future delegation to `godot-cpp`

Once GDExtension integration is in place:

- CamBANG delegates toolchain logic to `thirdparty/godot-cpp/SConstruct`
- the repo-root `SConstruct` becomes a thin orchestration layer
- CamBANG’s build file should not become a compiler-policy authority

### 2.5 SCons is the single source of truth

CamBANG builds via the repo-root `SConstruct`.

- CMake is intentionally removed
- IDE integration should call SCons directly, or via an external-build wrapper

This avoids dual-build drift and keeps CamBANG aligned with the
Godot / `godot-cpp` ecosystem.

---

## 3. Compilation database / IDE integration

CamBANG provides a SCons-native compilation database generator at:

```text
site_scons/site_tools/compdb.py
```

Behaviour:

- the tool wraps compile actions and records commands for C/C++ translation units
- the database is written to `compile_commands.json` at repo root by default
- the output path may be overridden with `COMPDB_PATH=...`

Important details:

- `compile_commands.json` is only written after a build that actually
  executes compilation actions
- it is not precomputed
- the write action is attached to the normal GDE build flow, so refreshing
  the database typically means rerunning a normal `scons ... gde ...` build

This supports tools such as:

- `clangd`
- CLion
- other language servers using `compile_commands.json`

---

## 4. Planned Windows release policy

For distributed Windows builds:

- MinGW-w64 is preferred for parity with official Godot binaries
- MSVC may remain supported as a development convenience

This remains a build-direction policy and does not by itself redefine
runtime behaviour.

---

## 5. Upstream discrepancy policy

If CamBANG must deviate from:

- `godot-cpp-template`
- `godot-cpp`
- Godot SCons conventions

then the deviation must be:

- documented
- linked to an upstream issue or reference where possible
- described as a removable workaround

The discrepancy log is maintained separately in:

```text
docs/dev/upstream_discrepancies.md
```

That file remains a log, not a narrative build guide.

---

## 6. Provider-selection scope in builds

The SCons `provider=...` option selects the **single platform-backed backend
implementation** compiled into the GDExtension build.

Exactly one platform-backed backend is compiled per build.

Synthetic support is controlled independently through:

- `synthetic=yes|no`

When synthetic is enabled, it is compiled as an optional runtime mode
within the single provider instance.

At runtime, the provider instance may support:

- `platform_backed`
- `synthetic`

Core still binds to exactly one provider instance.

These knobs must **not** alter the core smoke executable, which remains
stub-provider-only by design.

---

## 7. GDE scaffolding architecture

Initial Godot-facing scaffolding uses:

```text
CamBANGDevNode
```

This is a development-stage convenience node, not final API design.

Canonical ownership model:

- `CamBANGServer` (Engine singleton) owns `CoreRuntime`
- `CamBANGDevNode` may start / stop the server for convenience
- dev nodes do not own the runtime

Lifecycle convenience behaviour:

- `_enter_tree()` may start the server if not already running
- `_exit_tree()` may stop it only if that node initiated the start

Single-instance guards in scaffolding are temporary integration aids and
do not define final product constraints.

---

## 8. Current default targets and knobs

During the scaffolding phase:

- the default target builds the GDExtension (`gde`)
- core smoke is opt-in via `smoke=1`

If `smoke=1` is provided, the default build may also build the smoke target
alongside `gde` as an implementation detail.

If smoke-only output is desired, build the alias explicitly.

Godot compatibility target:

- Godot 4.5+

Release artifacts are expected per Godot minor series
(4.5, 4.6, 4.7, …) with a matching `godot-cpp` revision.

---

## 9. `godot-cpp` delegation and generated headers

The root `SConstruct` delegates to the `thirdparty/godot-cpp` submodule.

Important facts:

- `godot-cpp` must generate headers (`gen/include/*`) before extension compilation
- the extension build must depend on header generation, not only the static library
- a generated header such as `global_constants.hpp` can act as a dependency sentinel

Do **not** assume that linking the static library alone implies generated
headers are already available.

---

## 10. SCons object separation (`VariantDir`)

Both the smoke verifier target and GDE build compile shared `src/` sources with
different flags.

Without separated object directories, SCons may fail with collisions such as:

```text
Two environments with different actions were specified for the same target
```

Required solution:

- use `VariantDir`
- keep object trees separate, e.g.:
  - `out/smoke_obj`
  - `out/gde_obj`

This is mandatory for multi-target builds sharing sources.

---

## 11. Windows / MinGW runtime environment

MinGW-built DLLs commonly depend on runtime DLLs such as:

- `libstdc++-6.dll`
- `libgcc_s_seh-1.dll`
- `libwinpthread-1.dll`

If Godot is launched outside the MSYS2 MinGW environment, the Windows loader
may report:

```text
Error 126: The specified module could not be found
```

This typically means a dependent runtime DLL is missing, not that the plugin
DLL itself is absent.

Two common fixes:

### Option A — copy runtime DLLs beside the extension DLL

```sh
cp /mingw64/bin/libstdc++-6.dll tests/cambang_gde/bin/
cp /mingw64/bin/libgcc_s_seh-1.dll tests/cambang_gde/bin/
cp /mingw64/bin/libwinpthread-1.dll tests/cambang_gde/bin/
```

### Option B — launch Godot from the MSYS2 shell with PATH set

```sh
export PATH=/mingw64/bin:$PATH
"/c/path/to/Godot_v4.5.1-stable_win64.exe"
```

---

## 12. Media Foundation provider notes under MinGW

When building with:

- `provider=windows_mediafoundation`

under MinGW, the link set must include:

- `mf`
- `mfplat`
- `mfreadwrite`
- `mfuuid`
- `ole32`
- `uuid`

Important additional notes:

- `uuid` is required for `GUID_NULL` resolution under MinGW
- prefer `MFGetAttribute*` helpers over relying on inline convenience
  accessors that differ between SDK/toolchain environments
- use `MFVideoFormat_RGB32` rather than `RGBA32`

### Visibility-phase constraint

During the initial Windows visibility phase:

- `MF_READWRITE_DISABLE_CONVERTERS` is enabled
- only native output types are selectable
- many consumer cameras therefore expose only YUV-native formats

If no RGB32-like subtype is advertised natively, visible pixels will not
appear. This is expected and validates lifecycle / contract behaviour
without silently introducing conversion.

For the dev-phase record and counter interpretation, see:

```text
docs/dev/windows_mf_visibility_phase.md
```

### Windows macro collision: `OPAQUE`

Windows headers brought in via `windows.h` may define a macro named
`OPAQUE`.

Policy:

- avoid Windows-macro-prone identifiers such as `OPAQUE`, `ERROR`,
  `DELETE`, `IN`, `OUT` as unqualified enum members in shared headers
- prefer prefixed names such as `DOMAIN_OPAQUE`

CamBANG uses `DOMAIN_OPAQUE` for capture timestamp domains.

---

## 13. `.gdextension` encoding requirement

Godot’s `.gdextension` parser does not tolerate a UTF-8 BOM.

If the file begins with BOM bytes, Godot may report missing configuration
entries such as `entry_symbol`.

Ensure:

- UTF-8 encoding
- **no BOM**
- file begins with `[configuration]` at byte 0

---

## 14. Entry-point signature and method visibility

For Godot 4.x + `godot-cpp`, the entry signature uses the current
`GDExtensionInterfaceGetProcAddress` / `GDExtensionInitialization` form.

Do not use legacy signatures.

Also:

- `_enter_tree()` / `_exit_tree()` overrides in dev-node scaffolding must be
  public if binding code needs to take their method pointers during registration

---

## 15. Repeatable local development loop (Windows / MinGW)

Prerequisites:

- Godot 4.5.x Windows editor
- MSYS2 MinGW x64 environment
- `scons`
- `thirdparty/godot-cpp` submodule initialised and pinned to the intended
  Godot minor version

Typical build loop:

```sh
scons -c
scons -j 8 gde platform=windows target=template_debug arch=x86_64 use_mingw=yes
```

Expected outputs include:

- `tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll`
- `tests/cambang_gde/bin/libcambang.windows.template_debug.x86_64.a`

This build also refreshes `compile_commands.json` when compilation runs.

To place the compilation database elsewhere:

```sh
scons -j 8 gde platform=windows target=template_debug arch=x86_64 use_mingw=yes COMPDB_PATH=out/compile_commands.json
```

Then open the Godot test project:

```text
tests/cambang_gde/project.godot
```

Confirm that:

```text
tests/cambang_gde/bin/cambang_dev.gdextension
```

exists and is UTF-8 without BOM.

---

## 16. Development scenes and boundary checks

Typical scaffolding scenes include:

- extension load / lifecycle smoke scenes
- deterministic boundary-abuse scenes in the Godot test project

Current Godot-side boundary verification is documented in:

```text
docs/dev/maintainer_tools.md
```

and includes restart, tick-bounded coalescing, polling / immutability,
and observer scenes.

---

## 17. Core smoke executable policy

CamBANG maintains a small **core smoke executable** for fast, deterministic
runtime-spine validation without involving Godot or platform camera stacks.

Location:

```text
src/smoke/core_spine_smoke.cpp
```

Properties:

- opt-in build
- stub-provider-only by design
- independent of `provider=...` platform selection
- not part of the GDExtension artifact

Purpose:

- deterministic invariant validation of core runtime
- stub-provider-only lifecycle verification
- optional stress mode for repeated churn testing

Smoke-only code paths are gated behind:

- `CAMBANG_INTERNAL_SMOKE`

Legacy “IDE smoke” terminology is intentionally not used.

### Stress mode

`core_spine_smoke` supports optional churn testing via flags such as:

- `--stress`
- `--loops=N`
- `--jitter_ms=K`
- `--seed=S`

Stress mode validates:

- deterministic shutdown
- release-on-drop under overload
- no frame leaks across stop cycles
- exit-phase reachability
- admission gating during teardown

This remains stub-provider-only and must not depend on platform providers.

---

## 18. Scope boundary

This document governs:

- build-system direction
- scaffolding and toolchain behaviour
- local developer workflow
- repeatable build-loop expectations

It does **not** govern:

- runtime architecture
- provider lifecycle contract
- core threading model
- resource ownership rules

Those remain defined in canonical architecture documents.
