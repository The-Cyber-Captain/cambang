# Current tranche

## Pre-Provider-brief hardening

Status: Active, multi-step. Gates the future Provider-creation brief (the
work order for a real platform-backed provider such as `windows_winrt`).
Approved by maintainer 2026-07-17, in response to the concurrency/hygiene
audit ledger produced the same day.

## Step 1: concurrency Blockers

Status: Complete, pending maintainer review/commit. Both items
implemented and validated 2026-07-17; see "Validation performed" below. Not
yet committed -- changes remain in the working tree pending explicit
maintainer instruction to commit.

### Why this tranche exists

The C++ audit/repair effort gates the future Provider-creation brief (the
work order for implementing a real platform-backed provider such as
`windows_winrt`). Two Blocker-severity findings from that audit must close
before any further sequencing (compliance-verifier enforcement work, doc
cleanup, then the brief itself). This tranche is scoped to exactly those two
items. It does not authorize broader refactor.

### Scope

1. **`CoreRuntime::stop()` / `CoreThread::join()` concurrent-call race.**
   `CoreRuntime::stop()`'s `state_.exchange(TEARING_DOWN)` idempotency guard
   only rejects re-entry once already `STOPPED`/`CREATED`; two callers racing
   while state is `LIVE`/`TEARING_DOWN` can both fall through and both reach
   `core_thread_.join()` on the same underlying `std::thread`, which is
   undefined behaviour per POSIX `pthread_join` semantics when called
   concurrently. Serialize `stop()` so a second concurrent caller waits for
   the first's teardown to complete rather than racing into its own `join()`.
   Predates this audit; not a regression from prior remediation (verified via
   `git log -L` against `core_runtime.cpp`/`core_thread.cpp`).

2. **CPU-backed live display RID release thread affinity.**
   `SharedLiveCpuTextureRidState::replace_rid()` / `invalidate_and_release()`
   (`src/godot/cambang_stream_result_internal.cpp`) call
   `RenderingServer::free_rid()` synchronously from whatever thread drops the
   last reference, unlike the GPU-backing path
   (`SharedDisplayTextureRidState::release_now()` in
   `synthetic_gpu_backing_bridge.cpp`), which marshals release to the render
   thread via `enqueue_pending_release()`/`RenderThreadDrainHelper`. Mirror
   that pattern for the CPU-backed path so both display paths are
   architecturally consistent and the finding closes outright rather than by
   documented waiver. Maintainer decision 2026-07-17: marshal, not waive.

### Non-goals for this tranche

- Converting "provider calls must be prompt/bounded" from a manual
  `provider_compliance_checklist.md` review item into an enforced runtime
  watchdog or compliance-verifier test. That is the next tranche, pending
  investigation of what `provider_compliance_verify` currently covers.
- `provider_broker.h`'s residual `imaging/synthetic/*` header coupling,
  `capture_id` issuance (`CamBANGServer::next_capture_id_`) vs.
  `arbitration_policy.md` §9 wording, the undocumented
  `CAMBANG_TIMELINE_TEARDOWN_TRACE` env var, and other Minor/Note hygiene
  items from the audit. Deferred to a later doc/hygiene sweep tranche.
- Writing the Provider-creation brief itself.

### Validation expectations

- `scons maintainer_tools use_mingw=yes mingw_prefix=/c/Compilers/mingw64`
  must build clean.
- Relevant `src/smoke/core_spine_smoke.cpp`-family deterministic shutdown
  coverage must still pass (exercises `CoreRuntime::stop()`).
- `tests/cambang_gde/scripts/cpu_display_teardown_race_stress.gd` (via its
  `run_cpu_display_teardown_race_stress.ps1` harness) must be re-run after
  the RID marshaling change and remain clean, since it directly exercises the
  code path being changed.
- Do not weaken any existing test, smoke tool, or Godot verification scene to
  obtain a pass (`AGENTS.md`).

### Acceptance criteria

- No two-thread `stop()`/`join()` race remains reachable: a second concurrent
  `CoreRuntime::stop()` call blocks on the first's completion instead of
  independently calling `core_thread_.join()`.
- `SharedLiveCpuTextureRidState`'s only `RenderingServer::free_rid()` call
  site runs on the render thread, matching the GPU-backing path's discipline.
- Both changes documented at the call site (why, not what) per
  `cpp_code_quality_policy.md`'s comment guidance.

### Implementation summary

1. `src/core/core_runtime.h` / `.cpp`: added `stop_mutex_`; `CoreRuntime::stop()`
   now takes it for its full body, so a second concurrent caller blocks on the
   first caller's teardown (including `core_thread_.join()`) instead of racing
   into its own `join()` call.
2. `src/godot/cambang_stream_result_internal.h` / `.cpp`: added a
   pending-release queue + drain cycle for `SharedLiveCpuTextureRidState`,
   mirroring `SharedDisplayTextureRidState::release_now()` in
   `synthetic_gpu_backing_bridge.cpp`. `replace_rid()` and
   `invalidate_and_release()` now enqueue the superseded RID and request a
   render-thread drain (`LiveCpuTextureCreateDrainHelper::
   drain_pending_releases_on_render_thread()`, added alongside the existing
   `drain_pending_creates_on_render_thread()` on the same helper class)
   instead of calling `RenderingServer::free_rid()` directly from the calling
   thread. Renamed `g_pending_live_cpu_texture_create_mutex` ->
   `g_pending_live_cpu_texture_mutex` since it now guards both queues.

### Validation performed (2026-07-17)

- `scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64` -- clean build,
  exit 0, no new warnings.
- `out/core_spine_smoke.exe` -- PASS, run=35 ok=35 failed=0.
- `out/provider_compliance_verify.exe` -- PASS, run=36 ok=36 failed=0.
- `out/restart_boundary_verify.exe` -- PASS, all 8 steps OK.
- `tests/cambang_gde/run_cpu_display_teardown_race_stress.ps1 -Iterations 30
  -TimeoutSec 20` (real Godot 4.5.1 process launches, unsandboxed, on this
  machine) -- 30/30 clean, 0 flagged, no crash-like exit codes or crash
  signatures.

### Validation explicitly not run

- The stress harness's full default of 100 iterations (ran 30 for turnaround;
  30/30 clean is supportive evidence, not a substitute for a full 100-run
  pass before treating this as definitively closed).
- The remaining maintainer_tools smoke executables not directly exercising
  `CoreRuntime::stop()` or the CPU display path (e.g.
  `synthetic_timeline_verify`, `core_result_byte_budget_stress_smoke`,
  `phase3_snapshot_verify`, `core_capture_assembly_registry_smoke`,
  `core_dispatcher_bracket_routing_smoke`, `godot_result_convert_smoke`,
  `synthetic_only_provider_support_verify`, `verify_case_runner`,
  `pattern_render_bench`) -- not expected to be affected by either change,
  not run to keep this tranche's validation scoped to what the change
  actually touches.
- A synthetic two-thread-concurrent-`stop()` repro was not constructed to
  positively demonstrate the race existed pre-fix or is closed post-fix; the
  fix was verified by code inspection plus the existing deterministic
  shutdown/restart suites, not by a dedicated concurrency repro test.
- Windows GDE DLL import-table inspection (`objdump -p ... | grep "DLL
  Name"` per `build_and_scaffolding.md` §10) was not re-run; neither change
  touches linkage/runtime packaging.

## Step 2: enforce the provider prompt/bounded contract

Status: Complete, pending maintainer review/commit. Approved by maintainer
2026-07-17. Design options were written up and reviewed in chat; maintainer
decision: **log always, hard abort in maintainer builds** (not log-only, not
both-in-all-builds). Implemented and validated 2026-07-17; see "Validation
performed" below. Not yet committed.

### Why this tranche exists

`provider_architecture.md` §8.1 and `docs/dev/provider_compliance_checklist.md`
§2 require provider API calls reached through `CoreRuntime`/`ProviderBroker`
synchronous paths to be prompt and bounded. Confirmed by direct grep of
`src/smoke/provider_compliance_verify.cpp` (8,690 lines, 36 checks, all
currently passing): there is zero automated coverage of this property today.
It is enforced only by a human reading a checklist. `CoreThread` executes
tasks strictly serially with no per-task timeout or cancellation; if a
provider call inside a posted command lambda (or inside the shutdown timer-tick
hook, which calls `prov->shutdown()`) never returns, the sole core thread
wedges permanently. The existing 2s `future::wait_for()` bound on the ~12
Godot-facing command wrappers protects the *caller* (it always gets an answer)
but does nothing for the *core thread itself* -- every subsequent call to any
wrapper, forever, will individually time out and report `Busy` with no single
point where this is surfaced as "the runtime is dead."

Explicitly out of scope: moving provider calls off the core thread entirely
(discussed as "Option C" in chat and rejected for now -- it would reopen an
already-deliberate, three-document architecture decision that CoreRuntime
calls providers synchronously and it is the provider's job to be prompt; that
is real Provider-brief-level future direction, not this tranche).

### Scope

1. **`CoreThread` liveness primitive** (`src/core/core_thread.h`/`.cpp`).
   A `std::atomic<uint64_t> current_task_started_ns_` (0 = idle), set to
   `steady_clock` now immediately before, and cleared immediately after, each
   of: an essential-task run, a command-task run, an ordinary-task run, and
   the `on_core_timer_tick()` hook call -- i.e. every point in `thread_main()`
   that currently calls `run_guarded(...)`. Public accessor
   `current_task_started_ns()`, readable from any thread. No policy lives
   here -- pure mechanism, matching `CoreThread`'s existing scope as a generic
   executor with no provider/build-flag awareness.

2. **`CoreRuntime::check_core_thread_liveness()`** (`src/core/core_runtime.h`/
   `.cpp`). Policy layer. Reads the primitive; no-ops if idle or under a
   5s threshold (`kCoreThreadStaleTaskThresholdNs`, fixed constant, not an
   env knob per `cpp_code_quality_policy.md`'s guidance against persistent
   diagnostic env vars -- 2.5x the existing 2s caller-facing bound, chosen so
   it never fires on legitimately slow-but-compliant work). On a new stale
   episode, or every 5s while the same episode continues: always logs to
   `stderr` (`[CamBANG][CoreThread] stale task detected...`) and increments a
   diagnostic counter (`core_thread_stale_detections()`); additionally calls
   `std::abort()` when `CAMBANG_INTERNAL_SMOKE` is defined. That macro is
   already the established gate for maintainer-tool-only code paths
   (`repo_structure.md` §6) and is never defined for GDE/Godot plugin builds
   regardless of `target=debug|release` (verified against `SConstruct`), so
   this can never newly abort a shipped game or a developer's own Godot
   editor session -- only `maintainer_tools` binaries (including
   `provider_compliance_verify`) can hard-abort on this.
   Called from `CamBANGServer::_on_godot_process_frame()` once per Godot
   tick (cheap: one atomic load, early-return when idle), and from the new
   verifier below.

3. **Death-test verifier**: a deliberately-hanging test provider (a `Provider`
   implementing `ICameraProvider` whose `trigger_capture()` sleeps well past
   both the 2s caller bound and the 5s staleness threshold, e.g. 8s) attached
   via the existing `ProviderBroker::install_active_provider_for_smoke()`
   seam, exercised from a new standalone maintainer_tools binary,
   `src/smoke/core_thread_liveness_watchdog_verify.cpp`. Because this binary
   is itself a `CAMBANG_INTERNAL_SMOKE` build, the watchdog firing inside it
   means the process aborts -- it cannot self-report PASS/FAIL by normal
   return (abort() ends the process at the point of detection). A small
   PowerShell driver, `scripts/run_core_thread_liveness_watchdog_verify.ps1`,
   launches it as a child process and asserts it terminates via abort/crash
   (not a clean exit) within a bounded window, mirroring the existing
   launch-a-process-and-inspect-its-exit pattern already established by
   `tests/cambang_gde/run_cpu_display_teardown_race_stress.ps1` (a
   "death test" in the conventional testing-terminology sense). This is
   intentionally a separate binary from `provider_compliance_verify.cpp`
   rather than a 37th check added there, since that binary's whole design
   assumes normal return-and-continue between checks and I don't want to
   retrofit abort-tolerance into it for one scenario.

### Non-goals for this tranche

- Option C (moving provider calls off the core thread) -- see above.
- Per-task labeling/identification for the stale-task log line beyond "core
  thread has been stuck since T." Would require threading a debug label
  through every `try_post`/`try_post_essential`/`try_post_command` call site
  in `core_runtime.cpp` (dozens), for marginal diagnostic value beyond what
  the log timestamp plus recent stderr context already gives. Flagged as a
  possible future refinement, not required now.
- Exposing the staleness signal through the *public* Godot-facing snapshot so
  a host game can implement its own policy (log/restart/notify). Worth
  doing later; not required to close this tranche's actual gap (the
  compliance-verifier enforcement).
- Any change to `ICameraProvider`'s interface or the provider contract.

### Validation expectations

- `scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64` must build clean.
- `out/provider_compliance_verify.exe` and `out/core_spine_smoke.exe` must
  still pass unchanged (neither is expected to be affected; the new watchdog
  is a no-op below the 5s threshold, and normal maintainer-tool runs never
  approach it).
- The new `core_thread_liveness_watchdog_verify` death test must demonstrate:
  the stale-task stderr line is emitted, and the process terminates via
  abort/crash (not a clean/timeout exit) within the expected window.
- Do not weaken any existing test, smoke tool, or Godot verification scene to
  obtain a pass (`AGENTS.md`).

### Acceptance criteria

- A provider call that blocks past 5s inside a `CoreThread`-executed task or
  the timer-tick hook is always logged, in every build.
- The same condition hard-aborts in any `CAMBANG_INTERNAL_SMOKE` build,
  verified by a real, re-runnable, automated death test -- not a one-off
  manual check.
- No GDE/Godot plugin build gains a new abort path under any `target` value.
- `CoreThread` itself remains policy-free (mechanism only); the threshold,
  logging, and abort decision live in `CoreRuntime`.

### Implementation summary

1. `src/core/core_thread.h`/`.cpp`: added `current_task_started_ns_` (0 =
   idle) plus `mark_task_start_()`/`mark_task_end_()`, wrapped around every
   `run_guarded(...)` call site in `thread_main()` (`on_core_start`,
   essential/command/ordinary task loops, `on_core_timer_tick`,
   `on_core_stop`). Public accessor `current_task_started_ns()`.
2. `src/core/core_runtime.h`/`.cpp`: added `check_core_thread_liveness()`
   (5s `kCoreThreadStaleTaskThresholdNs`, 5s re-report cadence, always logs,
   `#if defined(CAMBANG_INTERNAL_SMOKE)`-gated `std::abort()`) and the
   diagnostic accessor `core_thread_stale_detections()`.
3. `src/godot/cambang_server.cpp`: `_on_godot_process_frame()` now calls
   `runtime_.check_core_thread_liveness()` once per tick.
4. `src/smoke/core_thread_liveness_watchdog_verify.cpp` (new): death-test
   binary. `HangingCaptureProvider` wraps a real `StubProvider` (composition,
   since `StubProvider` is `final`) and forwards every `ICameraProvider` call
   unchanged except `trigger_capture()`, which sleeps 8s first. Drives
   `CoreRuntime` directly (start, attach provider, open device, trigger
   capture, then poll `check_core_thread_liveness()` exactly as a Godot tick
   would) and is expected to be killed by the watchdog's abort, not to return
   normally.
5. `scripts/run_core_thread_liveness_watchdog_verify.ps1` (new): launches
   the above as a child process; PASS requires both the stale-task stderr
   line and a non-controlled (non-0/1) exit, mirroring
   `run_cpu_display_teardown_race_stress.ps1`'s launch-and-inspect pattern
   with inverted polarity (crash-like exit is the expected/required outcome
   here, not the anomaly).
6. `SConstruct`: registered `core_thread_liveness_watchdog_verify` in the
   `maintainer_tools` alias and clean-outputs list, built from
   `runtime_maintainer_tools_sources` (core + stub sources) exactly like
   `core_spine_smoke`.

### Validation performed (2026-07-17)

- `scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64` -- clean build,
  exit 0, no new warnings; `core_thread_liveness_watchdog_verify.exe` present
  in `out/`.
- `out/core_spine_smoke.exe` -- PASS, run=35 ok=35 failed=0 (unaffected).
- `out/provider_compliance_verify.exe` -- PASS, run=36 ok=36 failed=0
  (unaffected).
- `out/restart_boundary_verify.exe` -- PASS, all 8 steps OK (unaffected).
- `scripts/run_core_thread_liveness_watchdog_verify.ps1` -- **PASS**. Exit
  code `-1073740791` (`0xC0000409`, Windows' STATUS_STACK_BUFFER_OVERRUN-style
  abnormal-termination code -- consistent with MinGW `std::abort()`, not a
  controlled 0/1 return); "stale task detected" stderr line observed; process
  exited on its own well inside the 20s driver timeout (no force-kill
  needed, no evidence of a blocking Windows Error Reporting prompt on this
  machine). Both halves of the mechanism (always-log, maintainer-build abort)
  are now proven by a real, re-runnable, automated test, not a one-off manual
  check.

### Validation explicitly not run

- No test exercises the "GDE/Godot plugin build never aborts" acceptance
  criterion directly (e.g. compiling a GDE artifact with the watchdog firing
  and confirming no abort occurs). This follows directly from
  `CAMBANG_INTERNAL_SMOKE` never being defined for `gde_env` in `SConstruct`
  (verified by direct inspection, not by a dedicated build-matrix test).
- No test exercises the re-report cadence (repeated logging every 5s while
  the same stale episode continues) specifically -- the death test's 8s hang
  is shorter than two re-report intervals from first detection, so only the
  first log/abort was exercised, not the periodic re-log path for a still-
  ongoing (non-fatal, log-only build) episode.
- The Windows Error Reporting non-blocking behavior observed here is
  machine-specific (WER configuration is a per-machine/policy setting this
  audit does not control or verify); a differently-configured machine could
  behave differently for the timeout/force-kill path, though the script's
  existing timeout+`taskkill` fallback (already proven on this machine by the
  Step 1 stress harness) bounds that risk regardless.

## Step 3: doc/hygiene sweep

Status: Complete, pending maintainer review/commit. Approved by maintainer
2026-07-17 ("Proceed with Step 3"). Implemented and validated 2026-07-17;
see "Validation performed" below. Not yet committed.

This closes the pre-Provider-brief hardening tranche (Steps 1-3). Nothing
further is queued; the next deliberate step would be starting the
Provider-creation brief itself, which is explicitly outside this tranche's
scope.
Closes out the remaining Minor/Note findings from the original audit ledger
so the Provider-creation brief isn't built on top of documentation that
already doesn't match the code, or unexplained coupling exceptions.

### Scope

1. **`capture_id` issuance doc/code mismatch.** `arbitration_policy.md`
   Section 9 states "Core issues `capture_id`"; the actual code mints it in
   `CamBANGServer::next_capture_id_` (Godot boundary layer) and passes it
   into `CoreRuntime`. No correctness issue (uniqueness/monotonicity
   preserved, one counter shared across device and rig paths). Per
   `docs/dev/agent_context.md`'s explicit rule ("treat source and tests as
   the first authority; treat documentation as possibly stale"), fix the
   doc to describe reality rather than changing working, tested code to
   match a doc that was already imprecise.

2. **Undocumented `CAMBANG_TIMELINE_TEARDOWN_TRACE`.** Add to
   `docs/dev/maintainer_tools.md`'s existing "retained maintainer
   diagnostics" list, matching the format already used for the
   `CAMBANG_DEV_*` knobs.

3. **`snapshot_publisher_` missing ownership comment**
   (`src/core/core_runtime.h`). One-line non-owning comment, matching the
   adjacent `provider_` member's existing style.

4. **Two orphaned headers**, confirmed zero includers anywhere in `src/`
   and no `SConstruct` references: `src/core/rgba_frame.h` (self-described
   as scaffolding for a dev-only sink that was never built) and
   `src/smoke/verify_case/verify_case_model.h`. Delete both.

5. **`provider_broker.h` residual `imaging/synthetic/*` coupling.**
   Investigated a forward-declaration fix before deciding: `SyntheticRole`,
   `TimingDriver`, `TimelineReconciliation`, `SyntheticProducerOutputFormMode`
   are `enum class ... : std::uint8_t` and could be forward-declared with
   their fixed underlying type. But `stream_capability_downgrade_conditions_
   requested_`/`_latched_` and their capture-side counterparts are
   `std::vector<SyntheticStreamCapabilityDowngradeCondition>` /
   `std::vector<SyntheticCaptureCapabilityDowngradeCondition>` **data
   members with in-class default initializers**, not just function
   parameters -- removing the include would require those two struct types
   to stay complete wherever `ProviderBroker`'s implicit destructor is
   instantiated, which in practice means restructuring which members exist
   in the header at all (e.g. moving default-initialization into the
   out-of-line constructor), not just adding forward declarations. That is
   a real structural change with real risk to a file the whole GDE build
   depends on, for a Minor finding whose concrete-class removal (the
   actually risky part, `SyntheticProvider` itself) was already done in an
   earlier ledger item. Write an explicit, documented waiver in the header
   instead of attempting that refactor speculatively in a hygiene pass,
   per `cpp_code_quality_policy.md`'s own Waivers section ("the exception
   should be explicit").

### Explicitly deferred, not part of this tranche

- **Repeated `const_cast<CoreRuntime*>(this)` pattern** (11 sites across
  `core_runtime.cpp`, `provider_camera_fact_state.cpp`, `cambang_server.cpp`).
  Reviewed again; this is Note-severity only, each instance is individually
  defensible (logical-vs-physical constness, a well-known idiom), and
  `cpp_code_quality_policy.md` is explicit that findings should not be
  raised "merely because code differs from a personal formatting
  preference." Touching 11 call sites for a style-consistency comment isn't
  worth the diff for a non-correctness Note. Left as-is, consciously, not
  silently.
- **Duplicated `*_trace_enabled()` helper functions** (6+ files: 2 defs in
  `cambang_stream_result.cpp` alone, plus `cambang_stream_result_internal.cpp`,
  `gpu_backing_runtime.cpp`, `core_result_store.cpp`, `core_runtime.cpp`,
  `synthetic_gpu_backing_bridge.cpp`). Real but low-value-density
  maintainability nit; consolidating would touch both `src/core/` and
  `src/godot/` layers for a purely non-functional cleanup. Deferred rather
  than folded into this tranche.

### Validation expectations

- `scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64` must build clean
  (the only item here with real build risk is the provider_broker.h waiver
  comment, which changes no code).
- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe` must
  still pass unchanged.
- Do not weaken any existing test, smoke tool, or Godot verification scene
  to obtain a pass (`AGENTS.md`).

### Validation performed (2026-07-17)

- `scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64` -- clean build,
  exit 0, no new warnings; `provider_broker.o` compiled and the GDE DLL
  linked successfully with the waiver-comment-only header change.
- `out/core_spine_smoke.exe` -- PASS, run=35 ok=35 failed=0.
- `out/provider_compliance_verify.exe` -- PASS, run=36 ok=36 failed=0.

### Validation explicitly not run

- No test specifically re-verifies "the two deleted headers are truly
  unreferenced" beyond the `Grep` sweep already performed before deletion
  and the clean build afterward (a successful build is itself strong
  evidence, since either header being silently required would have failed
  compilation).
- `restart_boundary_verify` and the Godot GDE stress harnesses were not
  re-run for this step; none of the five changes touch stream/device
  lifecycle, teardown sequencing, or the CPU/GPU display paths those cover.
