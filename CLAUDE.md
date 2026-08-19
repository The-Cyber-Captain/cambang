# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Required reading before non-trivial work

This repo has its own agent-instruction chain that takes precedence over generic behavior:

1. `AGENTS.md` — workflow rules (minimal high-confidence changes, never weaken tests to get PASS, never commit unless explicitly asked, required validation is a hard completion gate).
2. `docs/dev/agent_context.md` — durable project expectations (source authority, public-API lock, snapshot/lifecycle rules, camera-fact model boundaries).
3. `docs/INDEX.md` — canonical-vs-supplement doc hierarchy. When docs and source disagree: source and tests win; report the mismatch rather than silently picking one.

**The active work order lives in conversation, not in a file.** There was a `docs/dev/current_tranche.md`; it was deleted because of one repeated failure: an agent writes scope into it, then cites its own writing back as authority. That produced a breaking schema change made on the authority of a scope item the agent had authored, a hardware-validation requirement naming scenes that cannot exercise the provider in question, and a mandate reported as out of scope by quoting the one sentence of the spec that supported deferring it. Do not recreate it under another name. What is true belongs in the canonical doc's status section, cited to source; what is next is agreed in conversation.

Likewise do not create tranche-completion records, remediation-plan backlogs, or deferred-task files anywhere in the repo — git history is the record of completed work, and validation detail belongs in the commit message, describing what was actually run rather than what was hoped for.

The Godot-facing public API (methods, signals, constants, dictionary shapes) and `schema/` are **locked**. A change requires the maintainer's explicit agreement in conversation, named as a public-surface change at the time. A scope line in any document — including one you wrote — is not authorization; a document records what was agreed, it does not confer permission.

## Build

SCons only (no CMake). Windows host, **MSVC** toolchain — the `windows_winrt` platform provider is C++/WinRT and MinGW cannot compile it. The working invocations on this machine:

```sh
# GDE plugin only / maintainer tools only
scons gde use_mingw=no godot_cpp=external -j8
scons gde=no use_mingw=no godot_cpp=external -j8

# Both artifact families
scons use_mingw=no godot_cpp=external -j8

# Android GDE for hardware validation (no mingw flags; arch is not inferred)
scons gde platform=android arch=arm64 godot_cpp=external -j8
```

Three standing rules, each of which has cost real time when broken:

- **`godot_cpp=external` always.** The `delegated` default is wrapped in `AlwaysBuild()`, so it re-runs the whole `thirdparty/godot-cpp` sub-build on every invocation regardless of changes (~10 min) and writes into the submodule tree. Preparing a genuinely new platform/target/arch/precision tuple is the only case for `delegated`, and it is ask-first.
- **MinGW is opt-in, and produces a synthetic-only plugin.** `use_mingw` defaults to `auto`, which resolves to MSVC on every host — the invocations above pass `use_mingw=no` for explicitness, not because it changes anything. Passing `use_mingw=yes` is a deliberate diagnostic choice: that build still succeeds but silently omits the WinRT provider (~63 MB, vs ~8 MB for MSVC), announced only by `gde_provider_status=not_compiled` in the config banner. Check that banner before trusting any platform-backed run.
- **Never pass `use_mingw` / `mingw_prefix` to Android builds**, and always state `arch=arm64` there — it defaults to `x86_64`, an emulator architecture.

Outputs: maintainer tools → `out/*.exe`; Windows GDE artifact → `tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll`. Both `windows_winrt` and `android_camera2` platform providers are implemented; `linux`/`macos`/`ios`/`web` are seams only. Full variable/clean-alias reference: `docs/dev/build_and_scaffolding.md`.

`CAMBANG_INTERNAL_SMOKE` is defined only for maintainer-tool builds, never for GDE builds — maintainer-only code paths (including hard-abort watchdog behavior) are gated on it.

## Validation

Native verifiers (host-native, deterministic, run directly):

```sh
out/core_spine_smoke.exe              # core lifecycle/shutdown spine
out/provider_compliance_verify.exe    # provider contract (43 checks as of 2026-08-02; trust the tool's own count, not this number)
out/restart_boundary_verify.exe
out/verify_case_runner.exe --run-all  # runs authored verification cases (26); with no case name it prints usage and exits 2
out/core_thread_liveness_watchdog_verify.exe   # self-supervising death test (abort + failed-latch modes; ~30s)
```

Godot scene verification runs from `tests/cambang_gde/` (PowerShell). The **only** authoritative classification is the shared harness verdict line `[CamBANG][HarnessVerdict] scene=<name> status=<ok|expected_unsupported|fail|error> ...`, checked by the launcher — never add runner-side regex exceptions; fix the scene's verdict instead.

Pass `-Windowed` as the standard for development iteration. `run_godot.ps1`
defaults to headless, which silently skips texture materialization and leaves
the maintainer with nothing to watch — a scene that renders nothing looks the
same as a scene that renders wrongly. Headless keeps its value for post-release
deployment checking, where nobody is watching anyway; it is not the default to
iterate against.

```powershell
# Single scene (from tests/cambang_gde/)
.\run_godot.ps1 -Scene res://scenes/66_public_lifecycle_verify.tscn -Windowed -CaptureLogs -TimeoutSec 60 -RunLabel scene66
# Android variant: add -TargetOs android (exports APK, deploys over adb).
# This works directly/unsandboxed on this machine — run it yourself when a
# tranche requires Android coverage; don't defer it to the maintainer.
# Android always runs windowed regardless; -Windowed is the Windows-side knob.
# -Scene only (no -Script), no -QuitAfter; use -TimeoutSec ~90+.

# Broad suite (scenes + status-panel fixtures)
.\godot_test_suite.ps1

# Matrix across target OS / renderer / provider backing / producer output form.
# One child process per combination; -DryRun prints the matrix without running.
# "platform" means a platform-BACKED provider here, never the OS -- the OS axis
# is -TargetOs, which is also run_godot.ps1's parameter name (was -RunPlatform).
.\godot_matrix_runner.ps1 -Scenes res://scenes/870_to_image_soak_benchmark.tscn `
  -TargetOs windows,android -Renderers compatibility,mobile `
  -ProviderBackings synthetic,platform -ProviderOutputForms cpu_only,gpu_only `
  -TimeoutSec 600 -AndroidDeviceSerial <serial>

# Render-teardown stress gates
.\run_cpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
.\run_gpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
```

`-CaptureLogs` writes classified run directories under `run-logs/{ok,expected_unsupported,error}/` plus `run-logs/summary.jsonl` — inspect the summary first, then only failing run dirs. Local Godot: `C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe` (default in both scripts). Sandboxed Godot launches on this machine can crash with signal-11 even when the same command works unsandboxed — treat unsandboxed runs as authoritative.

Validation matrix is Windows host + Android-over-ADB only; there is no Linux/WSL/macOS here. Native-tool PASS does not prove the corresponding Godot scene, and vice versa — they are separate surfaces; report un-run surfaces plainly.

## Architecture (big picture)

CamBANG is a Godot 4.5+ GDExtension: a deterministic imaging Core with camera providers behind a strict contract. Canonical docs: `docs/provider_architecture.md`, `docs/core_runtime_model.md`, `docs/arbitration_policy.md`, `docs/state_snapshot.md`, `docs/camera_fact_model.md`.

**Threading model** — `CoreRuntime` (`src/core/`) owns all mutable core state on a single dedicated `CoreThread`. The Godot main thread and provider callback threads are producers only: they marshal work via `post()`/`try_post*()` and the ~12 Godot-facing synchronous command wrappers use a 2s `future::wait_for` bound. Provider→Core facts arrive through `CBProviderStrand`, one serialized callback context with four event classes (Lifecycle/Native-object/Error are non-lossy; Frame is lossy). Providers must be prompt/bounded inside core-thread-executed calls; `CoreRuntime::check_core_thread_liveness()` polices this (always logs; aborts only under `CAMBANG_INTERNAL_SMOKE`). Exception to core-thread-only ownership: `CoreCaptureCohortRegistry`/`CoreCaptureAssemblyRegistry` are deliberately self-locking and readable from the Godot thread, with documented never-nested cross-registry lock ordering.

**Publication** — Godot observes state via tick-bounded snapshots: ≤1 `state_published` per Godot tick, with `gen`/`version`/`topology_version` counters. Published snapshots must reflect retained truth, not provider staging. Schema: `schema/state_snapshot/v1/`.

**Layers** — `src/imaging/api/` defines the provider contract (`ICameraProvider` etc.); `src/imaging/synthetic/` is the deterministic reference provider (timeline scenarios, virtual time); `src/imaging/platform/<os>/` are the platform provider seams (`windows/` = `windows_winrt`, C++/WinRT, MSVC-only; `android/` = `android_camera2`, both implemented — `linux`/`apple`/`web` are seams only); `src/imaging/broker/` is `ProviderBroker`; `src/godot/` holds the GDExtension wrappers (`CamBANGServer` singleton, `CamBANGDevice`/`CamBANGStream`/`CamBANGRig`, result objects); `src/smoke/` are the maintainer verifier sources. Do not let Synthetic-only shortcuts leak into the provider contract, and do not shape internal records around ADC JSON, Godot Dictionaries, or one platform API.

**Render-thread discipline** — RenderingServer RID creation *and* release for display textures are marshaled to the render thread via pending-queue/drain helpers (both the CPU-backed path in `cambang_stream_result_internal.cpp` and the GPU-backing bridge). Follow that pattern; never call `free_rid()` from an arbitrary thread.

**Rig captures fail closed** — multi-device rig capture is rejected unless a camera-concurrency truth naming the exact device combination was ingested via `CamBANGServer.ingest_camera_description(...)` *before* `start()`. That configuration gap returns `ERR_UNCONFIGURED` from `trigger_capture()` (and the first rejection per session logs its concrete failure category); other orchestration failures still return `ERR_BUSY`. See `73_rig_capture_result_set_verification.gd` for the correct ingest pattern and the negative-phase proof.

**capture_id** is minted at the Godot boundary (`CamBANGServer::next_capture_id_`), shared across device and rig paths — not in Core, despite what older doc drafts implied.

## Terminology discipline

"**Verification case**" = maintainer smoke/CLI authored validation input. "**Scenario**" = SyntheticProvider timeline replay data. Don't conflate them. An "**exercise**" (`CAMBANG_EXERCISE` env var) is a named maintainer harness mode, not product configuration. C++ severity terms (Blocker/Major/Minor/Note) follow `docs/dev/cpp_code_quality_policy.md` and must be used precisely.
