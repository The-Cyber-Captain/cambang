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

Status: complete; validated on 2026-07-18; committed as `527a193`/`efe916a`
("Fix render and GPU backing teardown seams").

Completed record:
`docs/dev/completed_tranches/tranche_4_render_and_gpu_backing_seams.md`.

The detailed findings and required-work language below preserve the activation
baseline for traceability. Words such as "current" describe the source at
Tranche 4 activation, not the remediated source after its validated completion.

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

### Tranche 5 — Verifier, harness, and warning-truth hygiene

Status: complete after correction and full revalidation on the authorized
Windows/Android matrix on 2026-07-19; committed (see `d87f183` and
subsequent history).

Authority (archived):
`docs/dev/completed_tranches/tranche_5_verifier_harness_and_warning_truth_hygiene.md`.

Scope: migrate the five maintained legacy Godot verification scenes (60, 61,
62, 63, and 66) to authoritative terminal harness verdicts; make native-object
verification exhaustive for `FrameBufferLease` and `GpuBacking`; resolve
first-party fallthrough and abandoned-validator warnings without changing
shutdown behavior or discarding malformed fact truth; and directly validate the
self-supervising liveness verifier on Windows.

This tranche absorbs the previously recorded Scene 60 and
`NativeObjectType`-switch debt. Source inspection expanded Scene 60's item to
all five scenes that `tests/cambang_gde/README.md` explicitly classifies as
older/non-protocol. It also established that the unused acquisition-timing
validator cannot be deleted mechanically: acquisition timing now travels on
`FrameView`, and its enum-domain validation boundary must be proved first.

No Godot public API change, runner-side regex waiver, shutdown-sequencing
change, generated-output edit, or godot-cpp change is authorized.

### Tranche 6 — Free-running tick flush split and frame-lease teardown hardening

Status: complete; committed 2026-07-19 as `394ed90`/`cd76a93`; full authorized
Windows/Android matrix validated.

This tranche was a maintainer-authorized insertion from the capture-path
performance investigation and follow-up concurrency audit (2026-07-19), not
part of the original audit sequence; it is recorded here after the fact so
this file remains the complete durable sequence. Record:
`docs/dev/completed_tranches/tranche_6_free_running_tick_flush_and_frame_lease_hardening.md`.

Scope: `flush_strand` split so the free-running Godot tick no longer blocks
on strand delivery (host-stepped `advance_timeline()` unchanged); enforcement
of the frame-lease release invariant at both CoreRuntime teardown boundaries;
in-code documentation of `close_device()`'s core-thread-serialization-
dependent check-then-act window.

## Queued after Tranche 6

The advisory static-analysis sequence in `docs/dev/static_analysis.md` remains
required audit follow-up, but must stay in separate narrow changes after the
focused verifier/warning tranche:

1. add an advisory `.clang-tidy` configuration;
2. add a changed-file clang-tidy helper;
3. add a CamBANG-specific advisory smell scanner;
4. generate a dated baseline report and decide which checks, if any, are
   suitable as changed-code gates.

These items are ordered backlog, not permission to start them without
maintainer activation of the next tranche.
