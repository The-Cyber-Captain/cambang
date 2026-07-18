# Codebase audit remediation plan

Status: active remediation sequence (2026-07-18).

This file is the durable sequence and backlog for the July 2026 C++
contract-boundary audit. `docs/dev/current_tranche.md` contains only the active
tranche's detailed implementation and validation contract.

The sequence may not be reordered merely because a later finding appears urgent
or convenient. Newly discovered required work must be added here at the correct
dependency point. Changing the active tranche or reordering queued tranches
requires explicit maintainer authorization.

## Sequence

### Tranche 1 — Command truth and exception-safe startup

Status: complete and committed as `c21e48b`.

Scope: synchronous Core command cancellation, truthful provider-operation
completion, allocation-failure conversion, and transactional Core/strand
thread startup.

Sign-off correction validated on 2026-07-18 (present in the current uncommitted
snapshot): once a synchronous command crosses its side-effect boundary, its
truthful completion wait now polls the core-thread liveness policy. This closes
the watchdog blind spot exposed when the waiting Godot/main caller could no
longer perform its ordinary tick poll. The self-supervising death-test verifier
now exercises this real wait path rather than relying on the obsolete
assumption that the caller returns after two seconds while the provider
operation remains live; direct invocation owns the bounded child classification
and terminal PASS/FAIL summary.

### Tranche 2 — Provider broker call isolation and teardown drain

Status: complete and committed as `7ac8f59`.

Scope: provider call lifetime leasing, provider virtual invocation outside the
broker lifecycle mutex, shutdown admission/drain, transactional provider
publication, and provider-specific capture watchdog truth.

### Tranche 3 — Synthetic capture worker correctness

Status: complete and committed as `93e60c1`.

Scope: bounded SyntheticProvider capture execution, transactional worker
admission, exact terminal/retain-release truth on every failure path, immutable
worker inputs, exception containment, and deterministic shutdown/restart.

### Tranche 4 — Render and GPU backing seams

Status: complete; validated on 2026-07-18, not yet committed.

Authority: `docs/dev/current_tranche.md`.

This is the originally planned Render and GPU backing seams tranche. It follows
Tranche 3 because capture workers create or retain CPU/GPU payload ownership
that reaches the backing runtime and Godot display bridges; worker completion
and ownership truth must be stable before those downstream seams change.

The render-resource drain and extension-teardown findings discovered while
preparing Tranche 3 are mandatory work inside this tranche. They do not replace,
rename, or narrow its original scope.

Broader audit scope to retain:

* publication, use, clearing, and in-flight lifetime of
  `SyntheticGpuBackingRuntimeOps` across install/uninstall and concurrent
  provider/result access;
* ownership and thread-affinity of retained capture GPU backing and live-stream
  GPU backing across create, update, replacement, fallback, release, stop,
  restart, and extension teardown;
* consistency between actual backing artifacts,
  `RetainedGpuBackingDescriptor`, primary-backing truth, display availability,
  materialization availability, and CPU-sidecar/fallback behaviour;
* exact native GPU-backing object create/destroy truth when live backing is
  recreated, downgraded, invalidated, or released;
* Godot display-wrapper borrowing, conversion, materialization, invalidation,
  and final-owner destruction across main, provider-worker, and render threads;
* lock ordering, callback admission, re-entry, and teardown quiescence across
  the Synthetic runtime seam and both Godot CPU/GPU display bridges;
* focused hygiene review of obsolete, duplicate, or unreachable backing paths
  after the corrected ownership model is established.

The raw runtime-ops seam requires explicit lifetime verification: the current
atomic raw-pointer load makes publication visible but does not itself lease an
in-flight call or make `clear_synthetic_gpu_backing_runtime_ops()` wait for
callers that already loaded `kOps`. The tranche must either prove surrounding
serialization makes every call safe or implement bounded admission/drain at
that seam.

New source-backed teardown findings to retain:

* `src/godot/synthetic_gpu_backing_bridge.cpp` clears queued
  `PendingTextureWrapperRelease` entries while holding
  `g_pending_release_mutex` during teardown, even though those entries own
  `Ref<Texture2DRD>` values documented in the same source as requiring final
  unreference on the render thread;
* destruction of the accompanying `SharedDisplayTextureRidState` can enqueue a
  RID release and re-enter the same non-recursive mutex when a RID remains;
* enqueue rejection after teardown begins can leave a moved Godot wrapper to be
  destroyed on the arbitrary caller thread;
* GPU uninstall invalidates live wrappers and then abandons the releases it just
  admitted instead of proving a final render drain;
* `src/godot/cambang_stream_result_internal.cpp` similarly clears pending CPU
  texture creates/releases and drops its callback helper while accepted render
  work may remain;
* its claim that callbacks can never enter torn-down extension state conflicts
  with `docs/dev/upstream_discrepancies.md`, which records reliance on Godot's
  ObjectID no-op behaviour because `Callable(Object*, StringName)` does not
  retain the helper;
* `tests/cambang_gde/run_cpu_display_teardown_race_stress.ps1` launches Godot
  directly and the script lacks the shared harness verdict, so the current
  stress is evidence but not an authoritative hard gate.

Required teardown sub-scope when activated:

* explicit active/draining/closed admission for CPU and GPU render work;
* destruction-free queue locking: move resources out before unreference, RID
  release, renderer calls, or other re-entrant work;
* final render-thread drain proven complete before helpers or callback targets
  are released;
* deterministic handling of late wrapper destruction, without wrong-thread
  fallback or silent resource abandonment;
* focused CPU and real-renderer GPU teardown stresses, each emitting the shared
  harness verdict and launching every Godot process through `run_godot.ps1`;
* documentation reconciliation and verification of any proposed
  `RenderingServer::force_sync()` completion guarantee before relying on it;
* Windows MinGW and Android external-`godot-cpp` compile gates, relevant native
  backing/result regressions, and authoritative Godot CPU/GPU coverage,
  including backing-plan and materialization paths;
* no Godot public API or binding changes.

Expected primary files when activated:

* `src/imaging/synthetic/gpu_backing_runtime.h`;
* `src/imaging/synthetic/gpu_backing_runtime.cpp`;
* `src/imaging/synthetic/provider.h` and `provider.cpp` only for the backing
  ownership seam left after Tranche 3;
* `src/godot/synthetic_gpu_backing_bridge.cpp`;
* `src/godot/godot_gpu_display_service.cpp`;
* `src/godot/cambang_stream_result_internal.cpp`;
* capture/stream result adapters and Core result-store code only if source
  inspection proves backing/descriptor truth is inconsistent;
* related internal headers or `src/godot/module_init.cpp` only where lifecycle
  or seam ownership requires them;
* the CPU teardown stress and a focused GPU teardown stress;
* `tests/cambang_gde/README.md`;
* `docs/architecture/pixel_payload_and_result_contract.md` where the verified
  ownership/fallback contract requires clarification;
* `docs/dev/upstream_discrepancies.md`.

## Recorded follow-up debt

The legacy `60_restart_boundary_abuse` scene prints its own pass marker but does
not emit `[CamBANG][HarnessVerdict]`, so `run_godot.ps1` classifies it as
`missing_harness_verdict`. This is test-harness debt, not a substitute for the
native restart verifier and not part of Tranche 3. Sequence it with later
harness-maintenance work unless a focused active-tranche test directly requires
the shared helper.

The MinGW maintainer build also reports incomplete `NativeObjectType` switches
in `src/smoke/verify_case/verify_case_catalog.cpp`: `FrameBufferLease` and
`GpuBacking` are not handled by `native_type_name()` or
`summarize_native_shape()`. This is verifier hygiene rather than Synthetic
capture-executor correctness. Preserve it for the later verifier/audit-hygiene
tranche, unless an earlier tranche changes those native-object classes and must
make the catalog exhaustive as part of that work.
