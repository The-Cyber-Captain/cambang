# Maintainer Tools

This document describes the command-line utilities, Godot-side
verification scenes, and tool-facing diagnostic conventions used by
maintainers to validate internal invariants, provider behaviour,
boundary semantics, and selected performance characteristics of the
CamBANG runtime.

These tools are **not user-facing applications** and are not intended to
ship as part of production builds. They exist to help maintainers validate
correctness while developing the runtime.

Most tools are **opt-in build artifacts** and will not appear in normal
build outputs unless explicitly requested.

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
| `windows_mf_runtime_validate` | Opt-in Windows Media Foundation runtime validation against real hardware | Verification |
| `pattern_render_bench` | Pattern renderer performance benchmark | Benchmark |
| Godot boundary verification scenes | Validation of the Godot-facing runtime boundary | Verification |

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
- Reserve **scenario** for the user-facing / Godot / SyntheticProvider
  timeline meaning.
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
## 4.x Dev-only producer capability override

For internal verification only, maintainer tooling may temporarily force synthetic
producer backing advertisement to shapes such as:

- CPU-only
- CPU+GPU
- GPU-only

This exists solely to validate provider/core/result policy handling for backing
selection and fallback behavior.

These overrides are:

- non-release
- intentionally capability-falsifying when selected
- not runtime truth
- not part of the public/provider contract

Maintainers should treat such modes as verification aids rather than as evidence
that the synthetic producer truthfully supports those capability sets in normal
operation.

---

## 5. Snapshot truth requirements

Maintainer tools and Godot-side verification scenes assume that providers
follow the snapshot truth rules.

Those rules define how snapshots represent:

- absence and unknown values
- retained identity
- publication consistency
- non-fabricated state

The canonical source for those requirements is:

```text
docs/dev/snapshot_truth_rules.md
```

This document does not restate the rules. When diagnosing a suspected
truth-discipline failure, consult the canonical rules document first and
then interpret the relevant verifier output in that light.

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
- validate real Media Foundation or other platform APIs
- exercise hardware-driven asynchronous device callbacks

Those concerns belong to platform validation tools.

### Build and usage

Typical build form:

```text
scons smoke=1 smoke
```

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

## 8. `windows_mf_runtime_validate`

**Category:** Verification tool (platform-backed)

### Purpose

`windows_mf_runtime_validate` validates the Windows Media Foundation
provider under real asynchronous hardware-backed execution.

It exercises:

- device enumeration
- real device open
- stream start / stop
- shutdown behaviour

This complements deterministic provider verification but does not replace it.

### Build and usage

```text
scons platform_validate=1 windows_mf_runtime_validate
./out/windows_mf_runtime_validate.exe --real-hardware
```

Expected high-level behaviour:

1. enumerate camera
2. open device
3. negotiate media type
4. start stream
5. shut down cleanly

---

## 9. Other native tools

### `core_spine_smoke`

Minimal runtime sanity check validating the Core runtime spine using the
stub provider.



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
