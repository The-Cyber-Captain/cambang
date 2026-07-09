# Maintainer Tools

This document describes the command-line utilities, Godot-side
verification scenes, and tool-facing diagnostic conventions used by
maintainers to validate internal invariants, provider behaviour,
boundary semantics, and selected performance characteristics of the
CamBANG runtime.

These tools are **not user-facing applications** and are not intended to
ship as part of production builds. They exist to help maintainers validate
correctness while developing the runtime.

Deterministic maintainer tools are default development-build artifacts under
`maintainer_tools=yes`. They are still not production artifacts; they are built
by default so local development builds continuously exercise deterministic
runtime checks. Platform-backed runtime validation remains separate and explicit
via `platform_runtime_validate=yes`.

---

## 1. Tool categories

Maintainer tools fall into three broad categories.

| Category | Purpose |
|---|---|
| Smoke test | Minimal sanity check ensuring the core runtime spine operates correctly |
| Verification | Deterministic validation of specific runtime invariants |
| Benchmark | Performance measurement utilities |

---

## 2. Tool overview

| Tool / asset | Purpose | Category |
|---|---|---|
| `core_spine_smoke` | Minimal Core runtime invariant validation using the stub provider | Smoke test |
| `synthetic_timeline_verify` | Deterministic verification of SyntheticProvider timeline behaviour and Core registry truth | Verification |
| `phase3_snapshot_verify` | Focused verification for snapshot/native-object/publication semantics | Verification |
| `restart_boundary_verify` | Deterministic verification of the CamBANGServer stop/start boundary contract | Verification |
| `provider_compliance_verify` | Deterministic provider-contract verification using Stub and Synthetic only | Verification |
| `synthetic_only_provider_support_verify` | Deterministic build-support and access/readiness preflight for synthetic-only maintainer builds | Verification |
| `pattern_render_bench` | Pattern renderer performance benchmark | Benchmark |
| Godot boundary verification scenes | Validation of the Godot-facing runtime boundary | Verification |

For mechanical/static-analysis guidance supporting C++ audits, see
`docs/dev/static_analysis.md`.

---

## 3. Terminology

### Smoke test

A minimal executable that validates the runtime spine:

- core runtime start
- provider registration
- state publication
- basic teardown

Smoke tests should remain **deterministic and provider-independent**.
They must not depend on real hardware.

### Verification tool

A deterministic executable used to validate specific internal invariants
or subsystem behaviour.

Verification tools should:

- be deterministic
- avoid platform hardware dependencies where possible
- produce a clear pass / fail result
- run quickly

### Verification case terminology (maintainer tooling)

For maintainer smoke/CLI tooling, authored validation inputs are called
**verification cases**.

- Use **verification case** in prose/help text for smoke/CLI selections.
- Reserve **scenario** for SyntheticProvider/provider-core timeline replay,
  diagnostics, metrics, fault reproduction, and recorded/authored behavior
  playback.
- Keep **fixture** distinct: fixtures are input artifacts; verification
  cases are authored validation procedures that may consume fixtures.

### Platform runtime validation

A maintainer tool that validates a platform-backed provider against real
OS APIs and, where applicable, real hardware.

Unlike deterministic verifiers, platform validation may depend on:

- device presence
- driver behaviour
- negotiated formats
- local machine state

Platform runtime validation must remain explicitly opt-in.

### Benchmark

A performance measurement utility.

Benchmarks do not validate correctness; they measure throughput,
latency, or resource usage.

---

## 4. Provider validation layering

Provider correctness is validated in **two layers**:

1. deterministic provider-contract verification
2. platform-backed runtime validation

This layering matches the architecture rules defined in:

- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`

Deterministic provider validation must not rely on physical hardware.
Platform validation is performed separately using explicit tools.

The terse provider-audit checklist remains separately documented in:

```text
docs/dev/provider_compliance_checklist.md
```
## 4.x Synthetic provider output-form and Backing Plan controls

Synthetic provider backing advertisement reports the output forms available from
the current runtime. Backing Plan policy chooses primary and auxiliary
retention within that truthful set. Maintainer controls for Synthetic
output-form and Backing Plan behavior belong on truthful production paths and
must not redefine provider-contract truth.

For backing-contract truth and result/payload semantics, use:

- `docs/architecture/pixel_payload_and_result_contract.md`

### 4.x.1 Synthetic producer output-form / Backing Plan maintainer control

This control is a maintainer/verification aid only. It is Synthetic-only, is not
product or user runtime configuration, and does not affect platform-backed
providers or public Godot API.

- Project setting: `cambang/maintainer/synthetic_producer_output_form`
  - values: `runtime_default|cpu_only|cpu_gpu|gpu_only`
  - default/unset: `runtime_default`
  - `runtime_default` is the explicit no-forcing value: it preserves the normal
    Synthetic runtime default policy rather than acting as a disguised spelling
    for one of the forced modes
  - this project-backed setting is the single authoritative maintainer surface
    for the selection and works for deploy-based verification from the same
    Godot project
  - host command-line runs may pass
    `--cambang-synth-producer-output-form=runtime_default|cpu_only|cpu_gpu|gpu_only`;
    startup feeds that value through the same project-setting authority path
    before the provider reads it, so command-line selection is not a separate
    authority path; Scene 70 reports both the stored project setting and the
    effective runtime selection when this transient command-line input is active
  - there is no environment-variable fallback
  - controls truthful Synthetic producer output-form reporting and the matching
    retained/produced behaviour for repeating-stream frames and still-capture
    frames where Synthetic has the corresponding backing seam
  - `cpu_only` reports and retains CPU-backed output only
  - `cpu_gpu` allows CPU and GPU output; provider/runtime realizability narrows
    that truthful set, so unavailable GPU backing collapses to CPU-backed
    behaviour while available GPU backing retains GPU primary output with a CPU
    sidecar
  - `gpu_only` reports GPU output only when the Synthetic GPU runtime is
    actually available, retaining GPU primary output without a CPU sidecar
  - `gpu_only` selections that cannot realize GPU backing fail deterministically
    with `ERR_NOT_SUPPORTED` instead of advertising GPU truth or falling back to
    CPU

Use this control for local validation of real result/backing/materialization
routes and Backing Plan behaviour. It must not be used to fabricate capability
advertisement.

### 4.x.2 Synthetic parent-context capability downgrade controls

These controls are maintainer/verification aids only. They are Synthetic-only,
internal, downgrade-only, and must not be treated as public API, platform
provider configuration, or a second authority for the outer output-form
envelope.

- Project setting:
  `cambang/maintainer/synthetic_stream_capability_downgrades`
  - default/unset: empty
  - host command-line runs may pass
    `--cambang-synth-stream-capability-downgrades=...`
  - startup feeds the command-line value through that same project-setting
    authority path before provider construction
  - condition grammar is `;`-separated entries of comma-separated `key=value`
    pairs
  - required key: `device=<hardware_id>`
  - optional keys: `intent=preview|viewfinder`, `width=<u32>`,
    `height=<u32>`, `format=<FOURCC-or-u32>`, `fps=<u32>`
- Project setting:
  `cambang/maintainer/synthetic_capture_capability_downgrades`
  - default/unset: empty
  - host command-line runs may pass
    `--cambang-synth-capture-capability-downgrades=...`
  - startup feeds the command-line value through that same project-setting
    authority path before provider construction
  - condition grammar is `;`-separated entries of comma-separated `key=value`
    pairs
  - required key: `device=<hardware_id>`
  - optional keys: `width=<u32>`, `height=<u32>`,
    `format=<FOURCC-or-u32>`, `bundle=default|multi`

Behavior:

- these conditions narrow only the **parent-context capability**
- they do not change the truthful outer provider/runtime envelope reported by
  `SyntheticProvider`
- they may only narrow an effectively mixed context to `cpu_only`
- they are inert when the outer envelope is already `cpu_only`
- they are rejected as contradictory when `gpu_only` is the configured outer
  envelope
- stream conditions apply to stream-owned contexts
- capture conditions apply to capture/`AcquisitionSession`-owned contexts
- they are intended for deterministic verification of backing-plan evaluation consumption and
  must not be used to fabricate capability truth

### 4.x.3 Synthetic stream GPU/update maintainer controls

These controls are maintainer/verification aids only. They are not product or
user runtime configuration and do not redefine provider-contract truth.

- `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=display_demanded|always`
  - default: `display_demanded`
  - `always` is a maintainer eager-update comparison override


Scene 68 (`68_inner_evidence_reset_verify`) remains only a secondary
retained-result evidence/reset surface.

Scene 568 (`568_backing_plan_evaluation_verify`) is the canonical automatable
Backing Plan evaluator verifier. It uses the existing
`CamBANGServer.get_synthetic_metrics_snapshot()` maintainer surface to read
parent-scoped `backing_plan_evaluation_reports`, including candidate-specific
decision evidence and explicit completion reasons. This is a verification-only
seam, not public API and not snapshot schema. Decision provenance for Scene 568
comes from Core's per-candidate report payload rather than from the global
`result_access_timing_evidence` aggregates.

Retained maintainer diagnostics:

- `CAMBANG_DEV_SYNTH_TRIAGE_TRACE`
- `CAMBANG_DEV_SYNTH_CATCHUP_CAP`
  - default: `0` (uncapped catchup)
  - normal `SyntheticProvider` runtime should leave catchup cap unset
  - `2` is the smallest cap preserving common two-stream due-together behavior
  - `1` is intentionally lossy for dual-stream steady flow and should be used
    only for stress/loss diagnostics
- `CAMBANG_DEV_DISPLAY_DEMAND_TRACE`
- `CAMBANG_DEV_SYNTH_GPU_TRACE`
- `CAMBANG_STREAM_LOAD_FRAME_SPIKE_TRACE`
- `CAMBANG_STREAM_LOAD_FRAME_SPIKE_TOP_N`

Harness selector:

- `CAMBANG_EXERCISE`
  - maintainer harness selector (not product API configuration)
  - `no_display_eager` resolves to effective
    `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=always` via exercise selection
    unless explicitly conflicted

Aggregate telemetry note:

- Per-frame `FrameBufferLease` native-object create/destroy snapshot rows remain
  removed.
- High-frequency lease/backing observability is via
  `scoped_resource_telemetry` counters, while long-lived identity-bearing native
  resources remain in `native_objects`.

Synthetic dev metrics also expose retained-result access timing evidence at
`CamBANGServer.get_synthetic_metrics_snapshot()["result_access_timing_evidence"]`.
This evidence is gathered only from the real Godot result-operation seam for
retained-result access operations. It is not `CamBANGStateSnapshot`, is not
schema v1 truth, and does not participate in state publication. Its purpose is
to inform refined result-facing classification for supported non-ready paths
under the current live applied production posture; `UNSUPPORTED` support truth
and operation-specific `READY` direct-retained availability remain structural.
Evidence collection/renewal belongs to live applied production-posture
acceptance/application boundaries, not first user-visible `to_image()` demand.
Public access calls remain instrumented, but are not the normal recalibration
heartbeat. Retained GPU display-view timings measure wrapper/display-view
acquisition, not later render-thread draw, UI scheduling, or Synthetic GPU
upload/update cost.

Timing semantics reminder:

- `display_view_elapsed_ns` measures the real Godot `get_display_view()` seam
  for stream results.
- Capture member `materialization_elapsed_ns` measures the real Godot
  `to_image_member()` seam for that member.
- Capture `capture_ready_elapsed_ns` is lifecycle latency from dispatcher and
  acquisition-session timestamps, not a Godot result-access call boundary.
- Capture `total_elapsed_ns` is a conservative chooser score for the whole
  required still result: readiness once plus the slowest required member
  materialization.
- `normalized_cost_units` is byte-normalized classification evidence for
  supported non-ready access, not a chooser latency score or a general
  provider/hardware benchmark.
- These timings must not be read as provider-local generation/staging cost,
  snapshot publication cost, or UI draw cost.

Removed temporary knobs:

- `CAMBANG_DEV_SYNTH_UPDATE_GPU_ONLY_WHEN_DISPLAY_REQUESTED`
- `CAMBANG_DEV_SYNTH_SKIP_GPU_TEXTURE_UPDATE`
- `CAMBANG_DEV_SYNTH_REUSE_RENDERED_FRAME`

---

## 5. Snapshot truth requirements

Maintainer tools and Godot-side verification scenes assume that providers
follow the canonical snapshot and publication rules.

Canonical authority lives in:

- `docs/state_snapshot.md` for public snapshot schema, retained runtime truth,
  timestamp semantics, and snapshot field meaning.
- `docs/architecture/godot_boundary_contract.md` for Godot-facing observable
  start/stop/restart, NIL-before-baseline, generation baseline, and tick-bounded
  publication behaviour.

The notes below are maintainer interpretation guidance for verifier output.
They are an audit/triage aid, not a second source of truth.

### Interpreting verifier output

The following output shapes usually indicate a snapshot-truth violation
rather than a mere presentation issue:

- a value appears before any runtime event authorizes it
- an identifier changes without a corresponding lifecycle transition
- a supposedly retained value resets to a convenience default
- a new generation exposes non-`NIL` state before authoritative
  publication
- the same published snapshot mixes truths from different runtime moments

When a verifier reports one of these patterns, the likely defect is in
retained runtime state, publication timing, or snapshot projection.

---

## 6. `provider_compliance_verify`

**Category:** Verification tool

### Purpose

`provider_compliance_verify` validates the provider contract and lifecycle
rules using deterministic providers only.

It exercises:

- `StubProvider`
- `SyntheticProvider`

This is the primary maintainer check for provider-contract rules such as:

- serialized provider → core callback delivery
- lifecycle ordering correctness
- native-object create / destroy reporting
- AcquisitionSession-aware native/snapshot expectations
- non-lossy lifecycle/native-object/error delivery
- deterministic shutdown sequencing
- synthetic ordering and timeline invariants
- clustered destructive sequencing behavior under strict vs completion-gated modes
- verifier-only still-capture metadata divergence proof for Case B
  (`applied_exposure_compensation_milli_ev` differs from
  `realized_exposure_compensation_milli_ev` on a successfully emitted member)
- verifier-only still-capture realized-unknown propagation proof for Case C
  (`has_realized_exposure_compensation_milli_ev=false` survives provider →
  Core retained result truth)
- still-image-bundle capability-gate checks proving:
  - default-only bundle validity is not gated by multi-member capability
  - multi-member authored bundle rejection when multi-member capability is false
- parent-scoped Backing Plan evaluator regressions proving:
  - capture winners require complete readiness-plus-materialization evidence
  - stale or duplicate result observations cannot satisfy another candidate
  - direct single-viable selection remains visibly non-evaluated
  - partial stream comparison reports the live-display-demand family-crossing guard truthfully

### What it proves for synthetic timeline destructive sequencing

For clustered destructive synthetic timeline cases, the verifier accepts the
runtime-valid strict and completion-gated outcomes rather than assuming a single
destroy/close realization shape.

In particular:

- **strict** validation may legitimately result in either:
  - retained stopped state with in-band destroy/close failure, or
  - full in-band destroy/close success

- **completion-gated** validation proves eventual successful destructive
  realization once the relevant readiness truth exists

This tool is therefore the authoritative deterministic verifier for provider
contract behavior in this area.

### What it does not do

It deliberately does **not**:

- enumerate physical cameras
- open platform-backed devices
- validate real platform-backed OS APIs
- exercise hardware-driven asynchronous device callbacks

Those concerns belong to platform validation tools.

### Build and usage

Typical deterministic maintainer-tool build forms:

```text
scons gde=no
scons maintainer_tools
```

Maintainer tools are host-native and are not scoped by `platform=<...>`. The GDE
build does not require a public provider-selection variable.

Usage:

```text
./out/provider_compliance_verify.exe
```

Expected result:

```text
PASS provider_compliance_verify
```

---

## 6.1 `canonical_timeline_realization`

**Category:** Verification case (run via `verify_case_runner`)

### Purpose

`canonical_timeline_realization` is a small, readable, default-path proof that
an authored synthetic timeline realizes correctly end-to-end.

It is intended to remain:

- canonical
- completion-gated by default
- always-pass
- easy to interpret

It is **not** intended to act as a second provider compliance verifier or as a
strict-edge diagnostic probe.

### Current role

This verification case keeps a clear authored destructive sequence, including:

- `StopStream @ T`
- `DestroyStream @ T+1`
- `CloseDevice @ T+2`

without widening those timings arbitrarily.

Under the current default completion-gated model, the case continues advancing
synthetic virtual time while waiting for realized destroy/close truth, rather
than assuming one tiny post-stop advance must always suffice.

This means the case proves:

> given a straightforward authored timeline, the standard/default synthetic
> timeline path realizes cleanly

### Usage

```text
./out/verify_case_runner.exe canonical_timeline_realization
```

This case should be stable under repetition and is suitable as a fast regression
signal for default synthetic timeline realization behavior.

## 6.2 Scene 65 (`65_public_boundary_verify`)

**Category:** Godot-side boundary verification scene

### Purpose

Scene 65 verifies public provider-start/config boundary behavior at the Godot
surface.

It verifies:

- deterministic invalid-argument rejection for invalid `start(...)` argument
  combinations
- applicable vs non-applicable `timeline_reconciliation` handling at start
  boundary
- `get_active_provider_config()` explicit-null shape for non-applicable
  `timeline_reconciliation` modes
- public runtime-command admission only after the first observable baseline
  `state_published(gen, 0, 0)` for a generation, including reset across stop/restart
- provider discovery and hardware-id endpoint startup intent before baseline, with
  actual endpoint effects delayed until after the clean baseline publish and
  repeated profile/warm-policy startup calls resolved by last-write-wins
- the narrow synthetic timeline UX exception where scenario staging and pending
  `start_scenario()` intent may be accepted before baseline while effects remain
  post-baseline

This scene complements deterministic provider verification, but it does not
replace `provider_compliance_verify` or change the role of
`canonical_timeline_realization`.

---

## 6.2 `synthetic_only_provider_support_verify`

**Category:** Verification tool

### Purpose

`synthetic_only_provider_support_verify` validates the deterministic provider
selection preflight for maintainer builds compiled with synthetic support and
without a compiled platform provider.

It checks that:

- platform-backed mode reports build-unsupported
- synthetic mode reports build-supported
- synthetic mode reports access/readiness as ready

### Usage

```text
./out/synthetic_only_provider_support_verify.exe
```

Expected output shape:

```text
OK platform_backed_build_support
OK synthetic_build_support
OK synthetic_access_readiness
PASS synthetic_only_provider_support_verify
```

Failures identify the failed check and exit nonzero.

---

## 7. `restart_boundary_verify`

**Category:** Verification tool

### Purpose

`restart_boundary_verify` is the authoritative deterministic verifier for
the CamBANGServer stop/start boundary contract.

It directly validates the **NIL-before-baseline** rule across a full
stop → restart cycle.

This tool exists because restart behaviour must be provable independently
of Godot-scene scheduling behaviour.

### Contract validated

The verifier enforces these runtime rules:

1. after completed `CamBANGServer.stop()`, the public snapshot is `NIL`
2. after a subsequent `start()`, `get_state_snapshot()` remains `NIL`
   until the first published snapshot of the new generation
3. the first post-restart publish yields a valid snapshot
4. stale publications from the previous generation do **not** repopulate
   the public snapshot
5. restart initiated from callback-context-equivalent execution remains safe

### Usage

```text
./out/restart_boundary_verify.exe
```

Expected output shape:

```text
step 0 OK
step 1 OK
step 2 OK
step 3 OK
step 4 OK
OK: restart_boundary_verify passed
```

Any failure indicates a regression in Godot-boundary snapshot exposure.

---

## 8. Other native tools

### `core_spine_smoke`

Minimal runtime sanity check validating the Core runtime spine using deterministic
maintainer-tool provider coverage.



# Godot Boundary Verification Scenes

Status: Dev Note  
Purpose: Documents the Godot-side scenes used to verify runtime publication behaviour.

## Goal

These scenes verify behaviour observable from the Godot boundary including:

- restart semantics
- snapshot publication ordering
- tick-bounded coalescing
- NIL-before-baseline behaviour

## Provider Selection

Scenes normally use **SyntheticProvider** to produce deterministic behaviour.

## Test Philosophy

Native tools verify internal runtime correctness.

Godot scenes verify:

- boundary stability
- snapshot immutability
- restart behaviour
- publication ordering

## Output Discipline

Scenes must flush output before quitting so that PASS/FAIL messages are reliably captured by automated runs.


## Scene 70 maintainer-teaching note (`stream_inspection_live`)

Scene 70 is a maintainer-facing verification/teaching scene for stream result
surfaces.

Its replacement summary for the older temporary `CaptureLatencyTrace` path is
transported through framed log records that `tests/cambang_gde/run_godot.ps1`
parses back into recovered files. This is a runner/tooling transport
convention, not a shared Godot harness layer.

Harnesses remain encapsulated; another harness can locally implement the same
tiny emitter if needed. Runner recovery preserves incomplete records too, so
failed runs can still retain partial structured data.

It now demonstrates:

- live stream display via `StreamResult.get_display_view()` (display-oriented
  live view over stream-owned live backing)
- a separate manual/discrete stream `to_image()` request shown in its own panel
  as explicit materialization onto CPU-backed storage
- consumer responsibility to unbind/drop active display-view UI bindings before
  runtime teardown

For the built-in `stream_inspection_live` scenario, checker appearance at
startup is now authored through the verified effective timeline path:

- `UpdateStreamPicture(... checker ...)` at `0 ns`

This removes ambiguity in the initial requested stream `to_image()` panel for
scene 70.

Capture-picture behavior was intentionally left unchanged in this correction.

## Scene 73 rig capture result-set verification (`rig_capture_result_set_verification`)

Contract truth for capture-result retrieval/assembly remains canonical in:

- `docs/architecture/pixel_payload_and_result_contract.md`

Maintainer proof inventory for Godot-visible rig capture uses:

- Scene 73: `tests/cambang_gde/scenes/73_rig_capture_result_set_verification.tscn`
- Scenario: `scenarios/rig_capture_result_basic.json`
- Trigger path: `CamBANGRig.trigger_capture() -> Error`
- Verification surface: object-level `CamBANGRig.get_result()` returning `CaptureResultSet`

Scene 70 remains the maintainer-facing stream/result teaching and verification
coverage; Scene 73 is the canonical Godot-visible rig `CaptureResultSet` proof.
