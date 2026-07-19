# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Required reading before non-trivial work

This repo has its own agent-instruction chain that takes precedence over generic behavior:

1. `AGENTS.md` — workflow rules (minimal high-confidence changes, never weaken tests to get PASS, never commit unless explicitly asked, required validation is a hard completion gate).
2. `docs/dev/agent_context.md` — durable project expectations (source authority, public-API lock, snapshot/lifecycle rules, camera-fact model boundaries).
3. `docs/dev/current_tranche.md` — the *active* work order: scope, acceptance criteria, validation expectations. This is forward-looking steering, not a changelog — replace its content when a tranche is accepted; never append retrospective "what I did" sections to it. For the audit-remediation workstream, the authorized sequence lives in `docs/dev/codebase_audit_remediation_plan.md`; do not promote queued findings ahead of the active tranche.
4. `docs/INDEX.md` — canonical-vs-supplement doc hierarchy. When docs and source disagree: source and tests win; report the mismatch rather than silently picking one.

The Godot-facing public API (methods, signals, constants, dictionary shapes) is **locked** unless the active tranche explicitly authorizes a change.

## Build

SCons only (no CMake). Windows host, MinGW toolchain. The working invocation on this machine:

```sh
# Full build (GDE plugin DLL + maintainer tools)
scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64

# GDE plugin only / maintainer tools only
scons gde use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons gde=no
```

`mingw_prefix` matters: without it, delegated godot-cpp discovery can pick up Git-for-Windows' bundled MinGW. Outputs: maintainer tools → `out/*.exe`; Windows GDE artifact → `tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll`. Android GDE builds use `platform=android` (synthetic-only; no platform provider is compiled yet on any platform). Full variable/clean-alias reference: `docs/dev/build_and_scaffolding.md`.

`CAMBANG_INTERNAL_SMOKE` is defined only for maintainer-tool builds, never for GDE builds — maintainer-only code paths (including hard-abort watchdog behavior) are gated on it.

## Validation

Native verifiers (host-native, deterministic, run directly):

```sh
out/core_spine_smoke.exe              # core lifecycle/shutdown spine
out/provider_compliance_verify.exe    # provider contract (36 checks)
out/restart_boundary_verify.exe
out/verify_case_runner.exe            # runs authored verification cases
scripts/run_core_thread_liveness_watchdog_verify.ps1   # death test; abort IS the pass condition
```

Godot scene verification runs from `tests/cambang_gde/` (PowerShell). The **only** authoritative classification is the shared harness verdict line `[CamBANG][HarnessVerdict] scene=<name> status=<ok|expected_unsupported|fail|error> ...`, checked by the launcher — never add runner-side regex exceptions; fix the scene's verdict instead.

```powershell
# Single scene (from tests/cambang_gde/)
.\run_godot.ps1 -Scene res://scenes/66_public_lifecycle_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel scene66
# Android variant: add -RunPlatform android (exports APK, deploys over adb)

# Broad suite (scenes + status-panel fixtures)
.\godot_test_suite.ps1

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

**Layers** — `src/imaging/api/` defines the provider contract (`ICameraProvider` etc.); `src/imaging/synthetic/` is the deterministic reference provider (timeline scenarios, virtual time); `src/imaging/platform/<os>/` are the (not yet implemented) platform provider seams; `src/imaging/broker/` is `ProviderBroker`; `src/godot/` holds the GDExtension wrappers (`CamBANGServer` singleton, `CamBANGDevice`/`CamBANGStream`/`CamBANGRig`, result objects); `src/smoke/` are the maintainer verifier sources. Do not let Synthetic-only shortcuts leak into the provider contract, and do not shape internal records around ADC JSON, Godot Dictionaries, or one platform API.

**Render-thread discipline** — RenderingServer RID creation *and* release for display textures are marshaled to the render thread via pending-queue/drain helpers (both the CPU-backed path in `cambang_stream_result_internal.cpp` and the GPU-backing bridge). Follow that pattern; never call `free_rid()` from an arbitrary thread.

**Rig captures fail closed** — multi-device rig capture is rejected unless a camera-concurrency truth naming the exact device combination was ingested via `CamBANGServer.ingest_camera_description(...)` *before* `start()`. The Godot boundary collapses all rig-trigger failure reasons to `ERR_BUSY`, so a scene where every rig `trigger_capture()` returns 44 forever usually means this ingest call is missing (see `73_rig_capture_result_set_verification.gd` for the correct pattern), not actual busy-ness.

**capture_id** is minted at the Godot boundary (`CamBANGServer::next_capture_id_`), shared across device and rig paths — not in Core, despite what older doc drafts implied.

## Terminology discipline

"**Verification case**" = maintainer smoke/CLI authored validation input. "**Scenario**" = SyntheticProvider timeline replay data. Don't conflate them. An "**exercise**" (`CAMBANG_EXERCISE` env var) is a named maintainer harness mode, not product configuration. C++ severity terms (Blocker/Major/Minor/Note) follow `docs/dev/cpp_code_quality_policy.md` and must be used precisely.
