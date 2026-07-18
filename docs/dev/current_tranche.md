# Current tranche

## Tranche 4 - Render and GPU backing seams

Status: complete; not yet committed (2026-07-18).

This is the fourth tranche in `docs/dev/codebase_audit_remediation_plan.md`.
It follows the committed command-truth, ProviderBroker isolation, and
Synthetic capture-worker tranches. It owns the runtime-ops lifetime seam,
retained GPU-backing truth, CPU/GPU Godot display bridges, render-resource
admission/drain, and extension teardown.

### Source-backed problem statement

The present backing and render bridges contain several lifetime claims that
are not established by their synchronization:

* `src/imaging/synthetic/gpu_backing_runtime.cpp` publishes an immutable
  `SyntheticGpuBackingRuntimeOps` table through one atomic raw pointer. A caller
  can load that pointer immediately before
  `clear_synthetic_gpu_backing_runtime_ops()` stores null and can then enter a
  Godot-backed function after bridge teardown has started. Atomic publication
  is not an in-flight call lease or teardown drain.
* `src/godot/synthetic_gpu_backing_bridge.cpp` destroys queued
  `PendingTextureWrapperRelease` values while holding
  `g_pending_release_mutex`. Those values own `Ref<Texture2DRD>` and shared RID
  state whose destructors can enqueue another release and re-enter the same
  non-recursive mutex. The same destruction-under-lock shape exists in
  teardown and callback early-return paths.
* `enqueue_pending_texture_wrapper_release()` takes its wrapper and state by
  value. If teardown has closed admission, returning false destroys the moved
  wrapper on the arbitrary caller thread even though the source says its final
  unreference belongs on the render thread.
* GPU uninstall invalidates live wrappers, thereby admitting release work, and
  then immediately clears/abandons that work and unreferences the callback
  helper. It does not prove a final render-thread drain.
* `src/godot/cambang_stream_result_internal.cpp` similarly clears accepted CPU
  texture creates and RID releases, drops its helper, and relies on a scheduled
  `Callable(Object*, StringName)` resolving a stale ObjectID as a no-op.
  `docs/dev/upstream_discrepancies.md` explicitly says that callable does not
  retain the helper, so the source comment claiming a callback can never enter
  torn-down extension state is not justified by CamBANG-owned lifetime.
* CPU pending-create destruction can release the last
  `SharedLiveCpuTextureRidState` while the queue mutex is held. Its destructor
  can call back into the same release queue, creating the same re-entry risk as
  the GPU path.
* `godot_gpu_display_service.cpp` has deliberately inactive descriptor-only
  lookup/invalidation paths while current display and materialization behavior
  is carried by a legacy retained `shared_ptr<void>` artifact. The tranche must
  verify that descriptor claims, actual retained artifacts, access posture,
  and fallback behavior remain mutually truthful; it must not silently promote
  the future descriptor seam into an unsupported cache.
* `tests/cambang_gde/run_cpu_display_teardown_race_stress.ps1` launches Godot
  directly, classifies crashes itself, and its GDScript does not emit the
  shared harness verdict. It is evidence, but it is not an authoritative
  `run_godot.ps1` gate. No equivalent focused real-renderer GPU teardown stress
  currently exists.

These are ownership, wrong-thread destruction, deadlock, callback-lifetime,
and silent-resource-abandonment risks. The correction must establish explicit
admission and drain truth; changing comments or adding another best-effort
clear is insufficient.

### Ordered work packages

Implement in this order. Do not start a later package while an earlier lifetime
contract remains ambiguous.

1. **Runtime-ops call lifetime**
   * Replace or wrap the atomic raw-pointer seam with explicit
     active/draining/closed admission and an in-flight call lease.
   * Installation publishes one stable generation. Clear closes new calls and
     waits for admitted calls outside Godot/provider/render locks before the
     bridge can be dismantled.
   * Preserve prompt normal calls and the existing non-GDE no-op behavior.

2. **GPU render-work ownership and final drain**
   * Model queued RID releases, `Texture2DRD` wrapper releases, scheduled
     callbacks, and helper lifetime as one owned drain protocol.
   * Move queued resources out while locked; destroy/unreference wrappers,
     shared states, and RIDs only after releasing the queue mutex and on the
     required render path.
   * Define deterministic ownership for enqueue rejection and late wrapper
     destruction after draining begins. No arbitrary-thread fallback and no
     silent abandonment are acceptable.
   * Keep the helper alive through the final accepted callback and prove
     quiescence before uninstall returns.

3. **CPU render-work ownership and final drain**
   * Apply the same admission/drain discipline to pending CPU texture creates,
     superseded creates, RID releases, and helper callbacks.
   * Remove reliance on stale-ObjectID no-op behavior for CamBANG correctness.
   * Ensure accepted creates either complete and their eventual RID is owned,
     or are cancelled before creation without leaving a later callback target.

4. **Backing, descriptor, and native-object truth**
   * Audit capture GPU backing and live-stream GPU backing across create,
     update, recreate, fallback, replacement, result retention, stop, restart,
     and extension teardown.
   * Keep `RetainedGpuBackingDescriptor`, `primary_backing_kind`, display
     availability, materialization availability, CPU sidecar, retained
     artifact, and public access posture consistent with actual capability.
   * Verify exact native `GpuBacking` create/destroy facts on realization,
     recreation, downgrade, failed update/retry, and release.
   * Keep descriptor-only display lookup inactive unless source-backed provider
     requirements justify activation; do not invent a cache for this tranche.

5. **Verification and hygiene**
   * Convert the CPU teardown race into an authoritative launcher/harness gate.
   * Add a focused real-renderer GPU wrapper/RID teardown race with the same
     classification and repeatability.
   * Remove obsolete, duplicate, unreachable, or contradicted backing paths
     only after the corrected ownership model identifies them conclusively.
   * Reconcile `tests/cambang_gde/README.md`,
     `docs/dev/upstream_discrepancies.md`, and the pixel payload/result contract
     with the behavior actually proved.

### Scope guardrails

* Do not change the locked Godot public API, registered public classes, method
  names, signals, constants, or public dictionary shapes.
* Do not change `ICameraProvider` or provider contract datatypes without
  separate maintainer authorization.
* Do not broaden into Core command orchestration, capture-worker redesign,
  platform-provider implementation, a generic renderer abstraction, or a new
  public display API.
* Do not make platform-backed providers depend on Synthetic-only artifacts.
* Do not activate descriptor-only caching merely because the seam exists.
* Do not rely on `RenderingServer::force_sync()` or another engine completion
  operation until its precise guarantee has been verified against authoritative
  Godot documentation/source. If it cannot prove callback completion, use an
  explicit CamBANG-owned completion/fence protocol.
* Do not weaken Scene 568, Scene 70, Scene 870, or teardown stresses merely to
  obtain a passing verdict.
* Every Godot process must be launched through
  `tests/cambang_gde/run_godot.ps1`, including each stress iteration.

### Expected implementation files

Primary expected files are:

* `src/imaging/synthetic/gpu_backing_runtime.h`;
* `src/imaging/synthetic/gpu_backing_runtime.cpp`;
* `src/godot/synthetic_gpu_backing_bridge.cpp` and its internal header;
* `src/godot/godot_gpu_display_service.cpp` and header;
* `src/godot/cambang_stream_result_internal.cpp` and header;
* `src/godot/module_init.cpp` where install/uninstall ordering requires it;
* `src/imaging/synthetic/provider.h` and `provider.cpp` only for backing
  ownership/native-object truth left after Tranche 3;
* `src/godot/cambang_stream_result.cpp` and
  `src/godot/cambang_capture_result.cpp` only where result access truth requires
  correction;
* `src/core/core_result_store.cpp` or related result records only if inspection
  proves retained descriptor/artifact truth is inconsistent;
* focused native verifier coverage where the runtime-ops seam can be exercised
  without Godot;
* `tests/cambang_gde/scripts/cpu_display_teardown_race_stress.gd`;
* `tests/cambang_gde/run_cpu_display_teardown_race_stress.ps1`;
* a focused GPU teardown stress script/driver;
* `tests/cambang_gde/README.md`;
* `docs/architecture/pixel_payload_and_result_contract.md`;
* `docs/dev/upstream_discrepancies.md`;
* this tranche record and the durable remediation plan.

Any expansion beyond these files must be source-justified before editing.

### Invariants

* No runtime-ops function can begin after admission closes, and bridge teardown
  cannot proceed while an admitted call still references Godot-backed code.
* No render callback, Godot/RenderingServer/RenderingDevice call, blocking wait,
  resource destructor, or user callback runs while a bridge queue mutex,
  provider-state mutex, or broad Core mutex is held.
* Queue locks protect only queue/protocol state. Resource destruction and
  re-entrant work occur after resources are moved into local ownership.
* Every accepted render work item has one terminal ownership path: completed on
  the render thread or explicitly cancelled before it can create/own a render
  resource.
* Every realized RID and Godot texture wrapper is released exactly once through
  its approved thread-affine path.
* Helper/callback targets outlive every callback that may invoke them.
* Outstanding public wrappers become truthfully invalid after stream/extension
  teardown and cannot draw or materialize stale-generation content.
* Actual backing artifacts and advertised descriptor/access truth agree.
* `godot-cpp` remains external and is not rebuilt.
* Windows builds use MinGW with `use_mingw=yes` and
  `mingw_prefix=/c/Compilers/mingw64`.

### Acceptance criteria

* A deterministic runtime-ops race proves clear/install versus concurrent calls
  cannot invoke an unpublished generation and that clear reaches quiescence.
* GPU and CPU bridge protocols expose active, draining, and closed state with
  tested admission rejection and final completion.
* Teardown never clears a queue of destructible Godot/shared resource owners
  while holding that queue's mutex.
* Teardown cannot deadlock through `SharedDisplayTextureRidState` or
  `SharedLiveCpuTextureRidState` destructor re-entry.
* Late GPU wrapper destruction and late CPU RID-state destruction have a
  deterministic correct-thread release owner even after ordinary admission is
  closed.
* Uninstall waits for or otherwise proves completion of every accepted render
  callback before dropping its helper/target and before extension-owned code
  becomes unavailable.
* No correctness claim depends on stale ObjectID callable dispatch.
* Stream and capture results preserve truthful CPU-only, GPU-only, and
  GPU-primary-with-CPU-sidecar display/materialization behavior across failure,
  fallback, stop, and restart.
* Native GPU-backing lifecycle facts are balanced under create, recreate,
  downgrade, update failure, release, and shutdown.
* Repeated CPU and real-renderer GPU teardown stresses emit the shared harness
  verdict, use only `run_godot.ps1` to launch Godot, and produce no crash,
  timeout, missing verdict, leak-accounting failure, or wrong-thread release.
* Final diff has no Godot public binding changes; the `thirdparty/godot-cpp`
  gitlink and working HEAD remain unchanged.

### Required validation

Builds require approved unsandboxed execution:

```text
scons gde=no maintainer_tools=yes godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=windows target=debug godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=android target=debug arch=arm64 maintainer_tools=no godot_cpp=external ANDROID_HOME=<configured-sdk-root>
```

Native regression floor:

```text
out/provider_compliance_verify.exe
out/synthetic_only_provider_support_verify.exe
out/core_spine_smoke.exe
out/core_result_path_smoke.exe
out/core_result_byte_budget_stress_smoke.exe
out/restart_boundary_verify.exe
```

The implementation must add deterministic native/internal verification for the
runtime-ops lease where that seam can be tested without Godot. CPU/GPU render
thread ownership remains authoritative only when exercised through Godot.

Required Windows Godot coverage, launched unsandboxed from
`tests/cambang_gde` through `run_godot.ps1`:

```powershell
.\run_godot.ps1 -Scene res://scenes/70_result_retrieval_verification.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche4_scene70_result_access
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche4_scene568_runtime_default -ExtraArgs @("--cambang-synth-producer-output-form=runtime_default")
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche4_scene568_gpu_only_mobile -ExtraArgs @("--rendering-method=mobile", "--cambang-synth-producer-output-form=gpu_only")
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche4_scene568_gpu_only_compatibility -ExtraArgs @("--rendering-method=compatibility", "--cambang-synth-producer-output-form=gpu_only")
```

The compatibility/GPU-only run must terminate with the documented
`expected_unsupported` verdict. Add explicit CPU-only and
GPU-primary-with-CPU-sidecar runs if the implementation changes either path.

The corrected CPU teardown stress and new GPU teardown stress are hard gates.
Each stress driver must invoke `run_godot.ps1` for every process, preserve each
run's structured artifacts, require the shared harness verdict, and run enough
fresh processes to exercise teardown interleavings. The GPU stress must use a
real renderer and a windowed run where the exercised path requires it.

Run a bounded Scene 870 mobile-renderer windowed soak if the changed display or
materialization path is exercised there. Windowed runs require approved
unsandboxed execution.

If an Android device is configured, run Scene 568 through the same launcher for
runtime-default and GPU-only/mobile postures. If no device is available, report
that environmental limitation without substituting a host run.

Finally run `git diff --check`, inspect `src/godot` public binding changes
explicitly, and verify both the `thirdparty/godot-cpp` gitlink and working HEAD.

### Validation record

Completed on 2026-07-18:

* the MinGW maintainer, Windows GDExtension, and Android arm64 builds passed;
  every build used `godot_cpp=external`, and the Windows builds used
  `use_mingw=yes mingw_prefix=/c/Compilers/mingw64`;
* the complete native floor passed: provider compliance 41/41, Synthetic-only
  provider support, Core spine 37/37, Core result path, Core result byte-budget
  stress, and restart-boundary verification;
* the new deterministic runtime-ops lease verifier passed, including
  clear-versus-call and replacement-generation races;
* focused provider verification passed for live GPU-backing create, recreate,
  failed update/retry, downgrade, release, and balanced native lifecycle truth;
* the Windows Godot matrix passed for Scene 70 result access and Scene 568
  runtime-default, CPU-only, GPU-only/mobile, and
  GPU-primary-with-CPU-sidecar postures; GPU-only/compatibility produced the
  required `expected_unsupported` verdict;
* the Scene 870 windowed mobile-renderer `to_image()` soak passed;
* the authoritative GPU teardown stress passed 25/25 fresh windowed/mobile
  processes, with artifacts under
  `tests/cambang_gde/run-logs/tranche4_gpu_teardown_authoritative`;
* the authoritative CPU teardown stress passed 25/25 fresh processes, with
  artifacts under
  `tests/cambang_gde/run-logs/tranche4_cpu_teardown_authoritative`;
* all three PowerShell launch/stress drivers parsed successfully; a final
  Scene 70 run passed through the hardened launcher, and a deliberate
  one-second probe returned the expected timeout classification in 1.66 s;
* Android device Scene 568 passed in runtime-default and GPU-only/mobile
  postures on the connected Samsung SM-G986U1;
* `git diff --check` passed, inspection found no Godot public binding change,
  and the `thirdparty/godot-cpp` gitlink, working HEAD, and submodule worktree
  all remained at `dcfab8de26e62c17b0f06c796125a63b1e437569` with no submodule
  modification.

All required Tranche 4 gates passed. Existing warnings were confined to the
pre-existing godot-cpp/macro-facing compiler surface; no required validation
remains unrun.
