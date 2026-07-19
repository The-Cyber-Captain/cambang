# Current tranche

## Tranche 9 - Provider-contract gap closure and the canonical Provider brief

Status: active; maintainer-activated 2026-07-19. This tranche explicitly
amends the recorded Tranche-1 prompt/bounded posture (unbounded truthful
wait) per the maintainer's "close any relevant gaps ... include issues
already on the ledger" instruction in service of the brief.

### Part 1 - contract gap closure (code)

1. **Bounded escalation for a violated prompt/bounded contract.**
   Today a provider call that never returns wedges the core thread; the
   blocked Godot main thread waits forever in GDE builds (maintainer builds
   abort at 5s). New posture: at a hard threshold (15s = 3x the stale
   threshold) `check_core_thread_liveness()` latches a terminal
   `core_thread_failed_` state with one loud log. Consequences of the latch:
   blocked synchronous waiters return their fallback status; all subsequent
   synchronous commands and posts refuse immediately; `stop()` performs an
   abandonment teardown (request stop, detach the wedged core thread,
   deliberately leak the provider/broker with a log) instead of joining a
   thread that provably cannot be joined; process exit must not hang on the
   wedged thread. In-process restart after a wedge is explicitly
   unsupported. Truthfulness argument: the runtime is reclassified as
   failed rather than misreporting any individual command; anything the
   wedged thread later completes belongs to a generation the boundary has
   already declared dead.
2. **Close the `close_device` check-then-act window outright** by holding
   `capture_mutex_` and `provider_state_mutex_` together (the documented
   nesting order, same as capture admission) across the in-flight check and
   the close, replacing the reachability-argument caveat.
3. **Make `SyntheticProvider::initialized_`/`shutting_down_`
   `std::atomic<bool>`** so the reference implementation contains no
   formally-racy entry-point reads.
4. **Death-test the new posture**: extend
   `core_thread_liveness_watchdog_verify` with a failed-latch mode (smoke
   hook suppresses the 5s abort; ~20s hanging provider) asserting the
   waiter unblocks at ~15s with the fallback status, subsequent commands
   refuse fast, `stop()` returns promptly, and the process exits 0.

### Part 2 - the canonical Provider brief (docs)

5. Author `docs/provider_implementation_brief.md` as a canonical document: a
   third party must be able to implement a platform-backed Provider (or
   re-implement Synthetic) from it plus the canonical architecture docs it
   references. Consolidates: the `ICameraProvider`/callback threading
   contract, strand event classes and ordering (including still-member
   ordering and the terminal-after-members rule), FrameView ownership and
   the exactly-once release contract, zero-copy adoption criteria, prompt/
   bounded obligations and the new failure semantics from item 1, shutdown
   ordering, lock discipline, the ImagingSpec/concurrency admission gate,
   what is Synthetic-specific and must not be imitated, and the compliance
   verification matrix a new provider must pass. References canonical docs
   instead of duplicating them.
6. Retire `docs/dev/provider_compliance_checklist.md` by deletion; migrate
   any content not already covered by the brief or the compliance verifier;
   update every reference (INDEX.md, CLAUDE.md, source comments).

### Constraints

* Godot public API and snapshot schema untouched. Core/Synthetic remain
  platform-agnostic.
* No behavior change below the 15s hard threshold; the 2s caller bound and
  5s stale-log/maintainer-abort behavior are unchanged.
* Do not weaken any existing verifier; the existing watchdog death-test
  modes must still pass.

### Expected implementation files

`src/core/core_thread.h/.cpp`, `src/core/core_runtime.h/.cpp`,
`src/godot/cambang_server.cpp`, `src/imaging/synthetic/provider.h/.cpp`,
`src/smoke/core_thread_liveness_watchdog_verify.cpp` (self-supervising; the
old ps1 driver was retired in Tranche 5), `docs/dev/maintainer_tools.md`,
`docs/provider_implementation_brief.md` (new), `docs/INDEX.md`,
deletion of `docs/dev/provider_compliance_checklist.md`, reference cleanup,
this file.

### Required validation

```text
scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64
out/core_spine_smoke.exe
out/provider_compliance_verify.exe
out/restart_boundary_verify.exe
out/verify_case_runner.exe --run-all
out/core_thread_liveness_watchdog_verify.exe   (both modes, self-supervising)
```

```powershell
.\run_godot.ps1 -Scene res://scenes/66_public_lifecycle_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche9_scene66
.\run_godot.ps1 -Scene res://scenes/73_rig_capture_result_set_verification.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche9_scene73
.\run_godot.ps1 -Scene res://scenes/870_to_image_soak_benchmark.tscn -CaptureLogs -TimeoutSec 180 -RunLabel tranche9_scene870
.\run_godot.ps1 -RunPlatform android -Scene res://scenes/66_public_lifecycle_verify.tscn -CaptureLogs -TimeoutSec 90 -RunLabel tranche9_scene66_android
```
