# Current tranche

## Tranche 3 — Synthetic capture worker correctness

Status: implementation and required validation complete; awaiting maintainer
review/acceptance (2026-07-18).

This is the third tranche in `docs/dev/codebase_audit_remediation_plan.md`. It follows
the completed command-truth and ProviderBroker isolation tranches. It is
limited to the SyntheticProvider capture executor, accepted-capture ownership,
terminal truth, and deterministic worker teardown.

### Source-backed problem statement

`SyntheticProvider` currently creates one submission `std::thread` per accepted
capture and then creates one more `std::thread` per device job. Completed
submission threads remain joinable in `capture_threads_` until provider
shutdown. `join_finished_capture_threads_()` is an unreferenced no-op that says
it exists for future maintenance, so sustained capture traffic retains an
unbounded number of thread handles and thread objects.

The worker failure seams also contradict the provider contract that successful
capture admission accepts responsibility for later terminal reporting:

* a catch at the submission-thread boundary logs an exception but does not
  terminalize every admitted device job or release its capture-session retain;
* a catch at a device-thread boundary has the same log-only behaviour for
  non-standard exceptions;
* partial per-device thread construction failure terminalizes all jobs while
  already-started device threads may continue producing frames because no
  per-submission cancellation state is latched;
* worker code calls
  `capture_parent_context_backing_capabilities_locked_()` without holding the
  provider-state mutex expected by that helper, allowing worker reads of
  `devices_` to overlap provider-state mutation;
* shutdown correctness depends on joining every historically retained capture
  thread rather than only currently active work.

The implementation must replace these seams with bounded, explicitly owned
execution. It must not merely detach threads, suppress exceptions, or increase
a limit.

### Scope

* replace per-capture/per-device thread proliferation with a bounded internal
  capture execution model;
* make admission transactional across in-flight registration, acquisition-
  session retain, executor admission, and returned `ProviderResult`;
* snapshot all immutable job inputs needed by workers while the appropriate
  provider-state locks are held;
* give every accepted device capture exactly one terminal outcome and exactly
  one capture-session release, including executor rejection, allocation
  failure, partial submission failure, worker exception, and shutdown;
* define cancellation/generation behaviour so no frame or `capture_started`
  fact can appear after that device capture has terminalized;
* close admission before shutdown, cancel or drain active jobs according to the
  documented provider contract, and join only owned active workers outside
  provider/capture state locks;
* preserve provider-callback serialization through `CBProviderStrand` and avoid
  calling callbacks while SyntheticProvider state locks are held;
* remove the abandoned `join_finished_capture_threads_()` path and any executor
  state made obsolete by the correction;
* add deterministic failure/race/resource-bound verification using internal
  test seams only;
* update the provider contract documentation where the implementation exposes
  an ambiguity, without redefining the public provider or Godot API.

### Expected implementation files

The expected change set is:

* `src/imaging/synthetic/provider.h`;
* `src/imaging/synthetic/provider.cpp`;
* `src/smoke/provider_compliance_verify.cpp` and/or
  `src/smoke/synthetic_only_provider_support_verify.cpp`;
* `tests/cambang_gde/scripts/73_rig_capture_result_set_verification.gd`, for
  the shared harness-verdict correction required to make its existing
  assertions an authoritative launcher gate;
* `docs/dev/provider_compliance_checklist.md`, if the verified executor rules
  need to be made explicit;
* `docs/provider_architecture.md` or `docs/core_runtime_model.md`, only for a
  genuine contract clarification;
* this tranche record and the status entry in
  `docs/dev/codebase_audit_remediation_plan.md`.

Do not broaden this tranche into `CBProviderStrand` capacity redesign, Core
result assembly, render/GPU backing seams, platform-provider work, or public
binding changes. The agreed **Render and GPU backing seams** work is preserved
as Tranche 4 in the audit plan, including the newly identified render-resource
teardown defects, and starts only after this tranche is accepted.

### Invariants

* The locked Godot public API is unchanged.
* The `ICameraProvider` interface and provider contract datatypes are unchanged
  unless separately authorized.
* Provider capture submission remains a prompt, bounded admission operation;
  success means responsibility for later terminal truth, not synchronous image
  generation.
* Every accepted device capture has one ordered lifecycle: optional started and
  frames, followed by exactly one completed or failed terminal fact.
* Capture-session ownership is retained and released exactly once per admitted
  device capture.
* Provider callbacks continue through the single serialized strand context.
* No worker outlives its owning SyntheticProvider generation.
* No callback, blocking join, or expensive render operation occurs while
  `capture_mutex_` or `provider_state_mutex_` is held.
* Tests and verification tools are not weakened merely to obtain `PASS`.
* `godot-cpp` is consumed with `godot_cpp=external`; it is not rebuilt.
* Windows builds use MinGW with `use_mingw=yes` and
  `mingw_prefix=/c/Compilers/mingw64`.
* Godot is launched only through `tests/cambang_gde/run_godot.ps1`.

### Acceptance criteria

* Capture executor concurrency and queued work have explicit, tested hard
  bounds independent of total captures completed during the process lifetime.
* A long sequence of completed captures does not accumulate joinable thread
  objects or OS thread handles.
* Failure to admit executor work rolls back every in-flight entry, pause depth,
  and acquisition-session retain before returning failure.
* Standard and non-standard worker exceptions produce one failure terminal for
  every affected accepted device capture and release ownership exactly once.
* Partial grouped-capture scheduling failure cannot produce frames or started
  facts after a failure terminal and cannot produce duplicate terminals.
* Workers do not read `devices_`, `streams_`, the virtual clock, or other
  provider-state-owned data without the documented lock or an immutable
  admission-time snapshot.
* Shutdown closes capture admission, reaches a bounded quiescent state, joins
  all active executor workers outside state locks, and cannot leave stale
  generation callbacks for a subsequent restart.
* Reinitialize/restart begins with empty executor, in-flight, pause-depth, and
  capture-session ownership state.
* Existing single-device, grouped/rig, multi-image, CPU-backed, GPU-backed, and
  capture-parent replacement verification remains green.
* The final diff contains no Godot public binding changes and the
  `thirdparty/godot-cpp` gitlink and working HEAD remain unchanged.

### Required validation

Builds require approved unsandboxed execution on this machine:

```text
scons gde=no maintainer_tools=yes godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=windows target=debug godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
```

Native deterministic regressions:

```text
out/provider_compliance_verify.exe
out/synthetic_only_provider_support_verify.exe
out/core_spine_smoke.exe
out/core_result_path_smoke.exe
out/core_result_byte_budget_stress_smoke.exe
out/restart_boundary_verify.exe
out/synthetic_timeline_verify.exe
```

The focused native verification added by this tranche must exercise executor
saturation, sustained completed-capture volume, standard and non-standard
worker failure, partial grouped scheduling failure, shutdown during active
generation, and restart. It must assert exact terminals, exact ownership
release, maximum observed worker/queue counts, and absence of post-terminal or
stale-generation events.

Authoritative Godot runs must be launched unsandboxed from
`tests/cambang_gde` through `run_godot.ps1`:

```powershell
.\run_godot.ps1 -Scene res://scenes/70_result_retrieval_verification.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche3_capture_worker_scene70
.\run_godot.ps1 -Scene res://scenes/569_capture_result_camera_facts_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche3_capture_worker_scene569
.\run_godot.ps1 -Scene res://scenes/73_rig_capture_result_set_verification.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche3_capture_worker_scene73
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche3_capture_worker_scene568_runtime_default -ExtraArgs @("--cambang-synth-producer-output-form=runtime_default")
```

Because `SyntheticProvider` is compiled into the Android GDE artifact, also run
the Android arm64 external-artifact build with the configured SDK/NDK path,
unsandboxed:

```text
scons platform=android target=debug arch=arm64 maintainer_tools=no godot_cpp=external ANDROID_HOME=<configured-sdk-root>
```

If an Android device is configured, run Scene 568 through the same launcher and
record the result. If no device is available, report that environmental limit
without substituting a host run or claiming Android runtime validation.

Finally run `git diff --check`, inspect the Godot public binding diff
explicitly, and verify both the `thirdparty/godot-cpp` gitlink and submodule
working HEAD before claiming completion.

### Validation record

Completed 2026-07-18:

* MinGW maintainer build passed with `godot_cpp=external`.
* Focused executor check passed after exercising a 64-job saturated queue, an
  atomic two-device grouped rejection, forced allocation failure after the
  first session retain and after the first in-flight/pause registration, 256
  sustained completed captures, standard and non-standard worker faults, four
  active jobs at shutdown, and clean restart.
* Full native gate passed: provider compliance `40/40`, synthetic-only support,
  core spine `36/36`, result path, result byte-budget stress, restart boundary,
  and synthetic timeline.
* Windows debug GDE build passed with external `godot-cpp` and MinGW.
* Windows launcher gates passed for Scenes 70, 569, 73, and 568.
* The first Scene 73 run completed all assertions and exited 0 but was correctly
  rejected by the launcher for `missing_harness_verdict`; its legacy scene was
  corrected to emit the shared verdict and the authoritative rerun passed.
* The originally prepared Scene 568 command was correctly rejected after
  `-QuitAfter 10` stopped the 1,200-frame/20-second asynchronous verifier after
  only five steps. The contradictory frame cap was removed and the complete
  verifier passed in the authoritative rerun.
* Android arm64 GDE build passed using SDK
  `C:\Users\The-Captain\AppData\Local\Android\Sdk`, NDK `28.1.13356709`, and
  external `godot-cpp`.
* ADB reported connected device `R5CN219ZXWF` (Samsung SM-G986U1); Android
  Scene 568 runtime-default verification passed through `run_godot.ps1`.

* `git diff --check` passed.
* Explicit `src/godot` diff inspection was empty; the locked Godot public
  binding implementation is unchanged.
* The `thirdparty/godot-cpp` gitlink and working HEAD both remain
  `dcfab8de26e62c17b0f06c796125a63b1e437569`, with a clean submodule status.
