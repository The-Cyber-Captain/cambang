# Current tranche

## Tranche 5 - Verifier, harness, and warning-truth hygiene

Status: complete after correction and full revalidation on the authorized
Windows/Android matrix (2026-07-19), not yet committed.

This is the fifth tranche in `docs/dev/codebase_audit_remediation_plan.md`. It
follows the completed render/GPU ownership work and owns the verifier and
first-party warning debt exposed while closing that tranche. It does not reopen
Tranche 4's render-resource design.

### Purpose

Make maintainer evidence directly runnable, exhaustively classified, and free
of first-party warning ambiguity. A verifier must describe and test the current
contract, emit its own terminal verdict, and fail for a real unmet invariant—not
because its harness still assumes behavior removed by an earlier tranche.

### Source-backed problem statement

This section records the activation baseline. The validation record below
describes the corrected source and the authorized validation matrix.

* `tests/cambang_gde/README.md` identifies Scenes 60, 61, 62, 63, and 66 as
  older/non-protocol scenes. Their local `OK: ... PASS` or `FAIL: ...` lines are
  useful to a human but are not `[CamBANG][HarnessVerdict]` records, so
  `run_godot.ps1` cannot classify them authoritatively.
* `src/smoke/verify_case/verify_case_catalog.cpp::native_type_name()` and
  `summarize_native_shape()` omit `NativeObjectType::FrameBufferLease` and
  `NativeObjectType::GpuBacking`. The MinGW build reports both switches as
  incomplete. The diagnostic name function must identify them; the restart
  shape summary must explicitly count or intentionally exclude them according
  to their topology role rather than omitting cases accidentally.
* `CoreRuntime::on_core_timer_tick()` intentionally advances through several
  shutdown phases in one tick, but uses comments saying `fallthrough` rather
  than C++ `[[fallthrough]]`. GCC therefore reports six first-party implicit
  fallthrough warnings. The transitions appear deliberate, but warnings must
  be resolved without adding returns that alter shutdown sequencing.
* `src/core/provider_camera_fact_state.cpp` contains unreferenced
  `valid_acquisition_timing()` and `valid_capture_image()` helpers. This is not
  yet a simple deletion: acquisition timing moved to accepted `FrameView`
  payloads, `ImageAcquisitionTiming::create()` validates the mark and receives a
  validated `TickPeriod`, but presently accepts enum values without checking
  their domains. The tranche must locate the authoritative validation boundary,
  add negative coverage, and only then remove or reuse the abandoned helpers.
* `docs/dev/maintainer_tools.md` does not list the liveness verifier or state the
  repository-wide terminal-verdict expectation consistently with the now
  self-supervising tool.

The ordered inventory in work package 1 found these additional maintained
native terminal-contract violations before implementation began:

* `godot_result_convert_smoke` puts the tool name before `PASS`/`FAIL`;
* `synthetic_timeline_verify`, `phase3_snapshot_verify`, and
  `restart_boundary_verify` use `OK:` rather than `PASS` for successful final
  summaries;
* `verify_case_runner` ends single-case runs with `Verification case PASSED`
  and all-case runs with an unclassified `Summary:` line rather than a final
  `PASS`/`FAIL` record.

Post-validation repeated execution exposed a further verifier-boundary defect:

* `close_while_streaming` and `redundant_stop` waited for Core-owned stream
  posture but did not wait for the reference providers' asynchronous,
  serialized acquisition-session fact to become snapshot-visible before
  asserting settled topology. `redundant_stop --provider=synthetic
  --repeat=500` reproduced the resulting intermittent failure at iteration 137.
  The correction must use bounded fact-based convergence, not a sleep or a
  weakened acquisition-session assertion.

The remaining maintained native targets already emit a final `PASS` on normal
successful verification. `pattern_render_bench` is a benchmark and is
intentionally excluded. Godot demos and manual teaching scenes are likewise
not reclassified as automated verifiers merely because they print diagnostics.

### Ordered work packages

Implement in this order.

1. **Verifier terminal-contract inventory**
   * Inventory maintained native verifiers and Godot verification scenes for a
     directly emitted terminal `PASS`/`FAIL` or
     `[CamBANG][HarnessVerdict]` record and matching process exit status.
   * Keep manual demos and benchmarks distinct; do not force verification
     protocol onto assets that are not classified as verifiers.
   * Record any additional maintained verifier violation before editing it.

2. **Legacy Godot harness migration**
   * Add exactly one terminal harness verdict to every success, expected-
     unsupported, failure, timeout, and error exit of Scenes 60, 61, 62, 63,
     and 66.
   * Preserve their existing direct human-readable completion lines.
   * Ensure cleanup cannot emit a second contradictory verdict.
   * Launch every authoritative run only through `run_godot.ps1`; do not add
     runner-side regex exceptions.

3. **Native-object verifier exhaustiveness**
   * Add truthful names for `FrameBufferLease` and `GpuBacking`.
   * Make restart-shape handling exhaustive. If those resource objects are not
     topology descendants for the summary, express that as explicit cases and
     test that the diagnostic still reports their presence where relevant.
   * Add or extend a verification case so future enum growth cannot silently
     recreate the warning or produce `unknown` for known types.

4. **First-party compiler-warning truth**
   * Replace each proven intentional shutdown transition with
     `[[fallthrough]]`; do not change phase order, timer requests, or returns.
   * Trace `CaptureImageFacts`, `ProviderCaptureImageFacts`, and `FrameView`
     acquisition timing from construction through retained result truth.
   * Enforce enum-domain validity at the narrow authoritative boundary and add
     negative tests. Do not silently discard a malformed present fact while
     reporting successful retention.
   * Remove helpers only after their intended validation responsibility is
     either active at the correct boundary or conclusively obsolete.

5. **Self-supervising verifier and documentation**
   * Directly run `core_thread_liveness_watchdog_verify` on Windows. Require
     diagnostic-first, terminal-summary-last, zero-on-PASS behavior and a
     bounded failure when the child contract is unmet.
   * Keep the internal child mode non-authoritative and undocumented as a user
     workflow.
   * Reconcile `docs/dev/maintainer_tools.md` and
     `tests/cambang_gde/README.md` with the verified tool/scene inventory.

### Scope guardrails

* Do not change the locked Godot public API, method/signal/constant bindings,
  or public dictionary shapes.
* Do not change shutdown sequencing merely to remove warnings.
* Do not weaken a scene assertion or native verifier expectation to obtain a
  terminal PASS.
* Do not add runner-side expected-output patterns in place of scene verdicts.
* Do not modify godot-cpp or generated build outputs.
* Do not treat third-party/generated compiler warnings as first-party defects.
* Do not add clang-tidy configuration, automation, or a general smell scanner
  in this tranche; the advisory static-analysis sequence remains queued after
  this focused warning/verifier correction.

### Expected implementation files

Primary expected files are:

* `tests/cambang_gde/scripts/60_restart_boundary_abuse.gd`;
* `tests/cambang_gde/scripts/61_tick_bounded_coalescing_abuse.gd`;
* `tests/cambang_gde/scripts/62_snapshot_polling_immutability_abuse.gd`;
* `tests/cambang_gde/scripts/63_snapshot_observer_minimal.gd`;
* `tests/cambang_gde/scripts/66_public_lifecycle_verify.gd`;
* `tests/cambang_gde/README.md`;
* `src/smoke/verify_case/verify_case_catalog.cpp` and focused verifier coverage;
* `src/core/core_runtime.cpp` only for explicit fallthrough annotations;
* `src/core/camera_fact_types.h`,
  `src/core/provider_camera_fact_state.cpp`, result/frame-ingress code, and
  focused tests only as justified by the acquisition-timing validation trace;
* `src/smoke/core_thread_liveness_watchdog_verify.cpp` only if direct Windows
  validation exposes a real verifier defect;
* `src/smoke/godot_result_convert_smoke.cpp`;
* `src/smoke/synthetic_timeline_verify.cpp`;
* `src/smoke/phase3_snapshot_verify.cpp`;
* `src/smoke/restart_boundary_verify.cpp`;
* `src/smoke/verify_case_runner.cpp`;
* `docs/dev/maintainer_tools.md`;
* this tranche record and the durable remediation plan.

Any expansion beyond these files must be source-justified before editing.

### Acceptance criteria

* Every maintained native verifier in scope emits its own final `PASS` or
  `FAIL` summary and returns 0 or nonzero consistently.
* Scenes 60, 61, 62, 63, and 66 each emit exactly one authoritative harness
  verdict on every terminal path and classify correctly through
  `run_godot.ps1`.
* Known `NativeObjectType` values never render as `unknown`; restart-shape
  handling is exhaustive and resource-object treatment is explicit.
* The maintainer build emits no first-party implicit-fallthrough or unused-
  helper warning covered by this tranche.
* Invalid acquisition-timing enum values cannot enter retained result truth;
  absence and a valid zero acquisition mark remain distinguishable.
* Cases that assert settled stream-backed acquisition-session truth wait for
  that provider fact explicitly; focused repeated runs do not depend on callback
  scheduling luck.
* The liveness verifier passes by direct invocation on Windows, prints
  diagnostics before its terminal verdict, and terminates within its bound.
* Documentation describes the current verifier inventory and no longer calls
  migrated scenes non-protocol.
* No Godot public binding or godot-cpp submodule change is present.

### Required validation

Builds require approved unsandboxed execution and external godot-cpp:

```text
scons gde=no maintainer_tools=yes godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=windows target=debug godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=android target=debug arch=arm64 maintainer_tools=no godot_cpp=external ANDROID_HOME=<configured-sdk-root>
```

Native floor:

```text
out/core_thread_liveness_watchdog_verify.exe
out/provider_compliance_verify.exe
out/synthetic_only_provider_support_verify.exe
out/core_spine_smoke.exe
out/core_result_path_smoke.exe
out/core_result_byte_budget_stress_smoke.exe
out/godot_result_convert_smoke.exe
out/synthetic_timeline_verify.exe
out/phase3_snapshot_verify.exe
out/restart_boundary_verify.exe
out/verify_case_runner.exe --run-all
out/verify_case_runner.exe redundant_stop --provider=synthetic --repeat=500
out/verify_case_runner.exe close_while_streaming --provider=synthetic --repeat=500
out/verify_case_runner.exe redundant_stop --provider=stub --repeat=50
out/verify_case_runner.exe close_while_streaming --provider=stub --repeat=50
```

Required Windows Godot coverage, launched unsandboxed from
`tests/cambang_gde` through `run_godot.ps1`:

```powershell
.\run_godot.ps1 -Scene res://scenes/60_restart_boundary_abuse.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche5_scene60_restart
.\run_godot.ps1 -Scene res://scenes/61_tick_bounded_coalescing_abuse.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche5_scene61_coalescing
.\run_godot.ps1 -Scene res://scenes/62_snapshot_polling_immutability_abuse.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche5_scene62_snapshot_polling
.\run_godot.ps1 -Scene res://scenes/63_snapshot_observer_minimal.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche5_scene63_observer
.\run_godot.ps1 -Scene res://scenes/66_public_lifecycle_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche5_scene66_lifecycle
```

Required Android deploy/run coverage over ADB, using the same launcher and
public lifecycle scene:

```powershell
.\run_godot.ps1 -RunPlatform android -Scene res://scenes/66_public_lifecycle_verify.tscn -CaptureLogs -TimeoutSec 90 -RunLabel tranche5_scene66_lifecycle_android
```

Finally run `git diff --check`, inspect Godot public binding changes, and verify
the `thirdparty/godot-cpp` gitlink and working HEAD remain unchanged.

### Superseded validation record

The following validation completed on the authorized Windows/Android matrix on
2026-07-19, but the later repeated-run failure reopened the tranche and
invalidated its completion claim:

* the MinGW maintainer-tools build, Windows GDExtension build, and Android
  arm64 NDK build passed; every build used `godot_cpp=external`, the Windows
  builds used `use_mingw=yes mingw_prefix=/c/Compilers/mingw64`, and the
  Android build used the configured Windows SDK/NDK paths;
* the maintainer build emitted no first-party implicit-fallthrough or abandoned
  camera-fact-helper warning covered by this tranche; remaining reported
  warnings were third-party godot-cpp/Godot macro-expansion warnings outside
  this scope;
* the native floor passed: liveness self-supervision, provider compliance
  41/41, Synthetic-only provider support, Core spine 37/37, Core result path,
  Core result byte-budget stress, Godot result conversion, Synthetic timeline,
  phase-3 snapshot, restart boundary, and all 23 verification cases;
* the new `native_object_type_diagnostics` case passed and classified all six
  known native-object types, including explicit non-topology resource counts;
* acquisition-timing negative coverage rejected invalid clock-domain,
  reference-event, and comparability enum values while existing zero-mark and
  absence coverage remained passing;
* Scenes 60, 61, 62, 63, and 66 each passed through `run_godot.ps1`; captured
  metadata recorded exactly one `status=ok exit_code=0 reason=pass` harness
  verdict for each run and each process returned zero;
* direct Windows liveness invocation emitted its stale-task diagnostic before
  `PASS core_thread_liveness_watchdog_verify ...`, observed the expected child
  abort, and returned zero;
* `git diff --check` passed, no Godot public binding addition/removal was
  detected, repository HEAD remained
  `d87f183b8255906a80caa1a267de9f59dcf20ae4`, and the godot-cpp gitlink,
  submodule HEAD, and clean submodule worktree all remained at
  `dcfab8de26e62c17b0f06c796125a63b1e437569`.

This repository's configured local verification matrix is Windows host
execution plus Android build/deploy/run over ADB. Linux, WSL, and macOS are not
available and are not completion gates. Platform-independent code remains
portable by design and source review, without claiming runtime validation on an
environment that was not exercised. The required Tranche 5 gates must be rerun
before the tranche can return to complete status.

### Revalidation after reopening

Completed on 2026-07-19 after the verifier-convergence correction:

* the MinGW maintainer-tools build, Windows GDExtension build, and Android
  arm64 NDK build passed with `godot_cpp=external`; Windows used
  `use_mingw=yes mingw_prefix=/c/Compilers/mingw64`, and Android used the
  configured SDK/NDK paths;
* `redundant_stop` and `close_while_streaming` each passed 500/500 with the
  Synthetic provider and 50/50 with the Stub provider after waiting on the
  existing bounded acquisition-session realization predicate;
* the complete native floor passed: liveness self-supervision, provider
  compliance 41/41, Synthetic-only provider support, Core spine 37/37, Core
  result path, Core result byte-budget stress, Godot result conversion,
  Synthetic timeline, phase-3 snapshot, restart boundary, and all 23
  verification cases;
* Windows Scenes 60, 61, 62, 63, and 66 each passed through
  `run_godot.ps1` with `VERDICT: OK` and exit zero;
* Android Scene 66 passed after export, ADB deployment, and on-device execution
  through `run_godot.ps1`, with `VERDICT: OK` and exit zero;
* `git diff --check` passed, no Godot public binding change was detected,
  repository HEAD remained `d87f183b8255906a80caa1a267de9f59dcf20ae4`,
  and the godot-cpp gitlink, submodule HEAD, and clean submodule worktree all
  remained at `dcfab8de26e62c17b0f06c796125a63b1e437569`.

All required Tranche 5 gates in the authorized matrix now pass.
