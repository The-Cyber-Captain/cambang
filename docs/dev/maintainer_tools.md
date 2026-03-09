# Maintainer Tools

This document describes the small command-line utilities used by
maintainers to validate internal invariants, provider behaviour, and
selected performance characteristics of the CamBANG runtime.

These tools are **not user-facing applications** and are not intended to
ship as part of production builds. They exist to help maintainers
validate correctness while developing the runtime.

Most tools are **opt-in build artifacts** and will not appear in normal
build outputs unless explicitly requested.

------------------------------------------------------------------------

# Tool Categories

Maintainer tools fall into three broad categories.

| Category | Purpose |
|---|---|
| Smoke test | Minimal sanity check ensuring the core runtime spine operates correctly |
| Verification | Deterministic validation of specific runtime invariants |
| Benchmark | Performance measurement utilities |
------------------------------------------------------------------------

# Tool Overview
| Tool | Purpose | Category |
|---|---|---|
| `core_spine_smoke` | Minimal Core runtime invariant validation using the stub provider | Smoke test |
| `synthetic_timeline_verify` | Deterministic verification of SyntheticProvider timeline behaviour and Core registry truth | Verification |
| `phase3_snapshot_verify` | Focused verification for snapshot/native-object/publication Phase 3 semantics | Verification |
| `restart_boundary_verify` | Deterministic verification of the CamBANGServer stop/start boundary contract | Verification |
| `provider_compliance_verify` | Deterministic provider-contract verification using Stub and Synthetic only | Verification |
| `windows_mf_runtime_validate` | Opt-in Windows Media Foundation runtime validation against real hardware | Verification |
| `pattern_render_bench` | Pattern renderer performance benchmark | Benchmark |

------------------------------------------------------------------------

## Godot Boundary Verification Scenes

In addition to the native CLI validation tools, the repository also
contains **Godot-side boundary verification scenes**.

These scenes validate the observable contract between:

Core Runtime → Snapshot Publication → CamBANGServer → Godot consumers.

They are development diagnostics and are **not product UI**.

Location:
docs/dev/godot_abuse_scenes.md


Primary scenes:

| Scene | Purpose |
|---|---|
| `60_restart_boundary_abuse` | Verifies restart NIL-before-baseline behaviour |
| `61_tick_bounded_coalescing_abuse` | Verifies Godot-visible tick-bounded publication |
| `62_snapshot_polling_immutability_abuse` | Verifies snapshot immutability and polling safety |
| `63_snapshot_observer_minimal` | Minimal snapshot observer for diagnostics |

These scenes intentionally use **SyntheticProvider** as the deterministic
runtime driver even when validating builds configured for other
providers.

The scenes complement the CLI tools:

| Layer | Validation |
|---|---|
| CLI tools | Core invariants, provider contracts |
| Godot scenes | Godot-facing runtime boundary behaviour |
------------------------------------------------------------------------

# Terminology

## Smoke Test

A minimal executable that validates the runtime spine:

-   Core runtime start
-   provider registration
-   state publication
-   basic teardown

Smoke tests should remain **deterministic and provider-independent**.
They must not depend on real hardware.

------------------------------------------------------------------------

## Verification Tool

A deterministic executable used to validate specific internal invariants
or subsystem behaviour.

Verification tools should:

-   be deterministic
-   avoid platform hardware dependencies
-   produce a clear pass/fail result
-   run quickly

------------------------------------------------------------------------

## Platform Runtime Validation

A maintainer tool that validates a platform-backed provider against real
OS APIs and, where applicable, real hardware.

Unlike deterministic verification tools, platform validation may depend
on:

-   device presence
-   driver behaviour
-   negotiated formats
-   local machine state

Platform runtime validation must remain **explicitly opt-in**.

------------------------------------------------------------------------

## Benchmark

A performance measurement utility used to characterise runtime
behaviour.

Benchmarks do not validate correctness; they measure throughput,
latency, or resource usage.

------------------------------------------------------------------------

# Provider Validation Tools

Provider correctness is validated in **two layers**:

1.  Deterministic provider-contract verification\
2.  Platform-backed runtime validation

This layering matches the architecture rules defined in:

-   `docs/provider_architecture.md`
-   `docs/core_runtime_model.md`

Deterministic provider validation must not rely on physical hardware.
Platform validation is performed separately using explicit tools.

------------------------------------------------------------------------

# `provider_compliance_verify`

**Category:** Verification Tool

## Purpose

`provider_compliance_verify` validates the provider contract and
lifecycle rules using only deterministic providers.

It exercises:

-   StubProvider
-   SyntheticProvider

This tool is the primary maintainer check for the provider compliance
rules recorded in:

-   `docs/provider_architecture.md`
-   `docs/core_runtime_model.md`
-   `docs/dev/provider_compliance_checklist.md`

## What it validates

The tool verifies core provider-contract invariants such as:

-   serialized provider → core callback delivery
-   lifecycle ordering correctness
-   native-object create/destroy reporting
-   non-lossy lifecycle/native-object/error delivery
-   deterministic shutdown sequencing
-   SyntheticProvider ordering and timeline invariants

## What it does not do

`provider_compliance_verify` deliberately does **not**:

-   enumerate physical cameras
-   open platform-backed devices
-   validate Media Foundation or other platform APIs
-   exercise real asynchronous device callbacks

Those concerns belong to platform validation tools.

## Build

Typical build form:

    scons smoke=1 smoke

The executable will appear in the `out/` directory.

## Usage

    ./out/provider_compliance_verify.exe

Expected result:

    PASS provider_compliance_verify

------------------------------------------------------------------------

# `restart_boundary_verify`

**Category:** Verification Tool

## Purpose

`restart_boundary_verify` is the **authoritative deterministic
verifier** for the CamBANGServer stop/start boundary contract.

It directly validates the **NIL-before-baseline rule** across a full
stop → restart cycle.

This tool exists because restart behaviour must be provable
**independently of Godot scene scheduling behaviour**.

## Contract validated

The verifier enforces the following runtime rules:

1.  After a completed `CamBANGServer.stop()`, the public snapshot must
    be `NIL`.
2.  After a subsequent `start()`, `get_state_snapshot()` must remain
    `NIL` until the first published snapshot of the new generation.
3.  The first post-restart publish must produce a valid snapshot.
4.  Stale publications from the previous generation must **not**
    repopulate the public snapshot.
5.  Restart initiated from callback-context-equivalent execution must be
    safe.

These rules correspond to the snapshot-generation semantics described
in:

-   `docs/state_snapshot.md`
-   `docs/naming.md`
-   `docs/core_runtime_model.md`

## Usage

    ./out/restart_boundary_verify.exe

Expected output:

    step 0 OK
    step 1 OK
    step 2 OK
    step 3 OK
    step 4 OK
    OK: restart_boundary_verify passed

Any failure indicates a regression in the **Godot boundary snapshot
exposure semantics**.

------------------------------------------------------------------------

# `windows_mf_runtime_validate`

**Category:** Verification Tool (platform-backed)

## Purpose

`windows_mf_runtime_validate` validates the Windows Media Foundation
provider under **real asynchronous hardware-backed execution**.

It exercises:

-   WindowsMediaFoundationProvider
-   Media Foundation device enumeration
-   real device open
-   stream start / stop
-   shutdown behaviour

This tool complements deterministic provider verification but does not
replace it.

## Build

    scons platform_validate=1 windows_mf_runtime_validate

## Usage

    ./out/windows_mf_runtime_validate.exe --real-hardware

Expected behaviour:

1.  enumerate camera
2.  open device
3.  negotiate media type
4.  start stream
5.  shut down cleanly

------------------------------------------------------------------------

# Other Tools

## `core_spine_smoke`

Minimal runtime sanity check validating the Core runtime spine using the
Stub provider.

This tool verifies the runtime can:

-   start
-   process provider events
-   publish state
-   shut down cleanly

Two modes:

  Mode         Purpose
  ------------ --------------------------------------------
  default      baseline runtime validation
  `--stress`   lifecycle churn and queue pressure testing

------------------------------------------------------------------------

## `synthetic_timeline_verify`

Validates deterministic behaviour of the SyntheticProvider timeline and
its interaction with the core lifecycle registry.

Supported scenarios:

-   `basic_lifecycle`
-   `invalid_sequence`
-   `catchup_stress`

This tool validates:

-   deterministic frame scheduling
-   lifecycle event emission
-   registry truthfulness under timeline-driven events

------------------------------------------------------------------------

## `phase3_snapshot_verify`

Focused verifier for Phase 3 snapshot/publication correctness.

Covers:

-   detached-root visibility semantics
-   retirement/removal observability
-   topology-version transitions
-   timestamp semantics
-   delivered vs dropped accounting

This tool intentionally **does not verify restart behaviour**.

Restart boundary behaviour is verified by:

-   `restart_boundary_verify` (deterministic native verifier)
-   `31_restart_nil_before_baseline.tscn` (Godot smoke scene)

------------------------------------------------------------------------

# Godot Boundary Smoke Test

## `31_restart_nil_before_baseline.tscn`

This Godot scene exercises the **consumer-facing restart behaviour**
through the Godot runtime boundary.

It verifies the same contract as `restart_boundary_verify`, but from the
perspective of the Godot plugin integration.

Expected behaviour:

-   restart may be initiated from callback context
-   after restart the snapshot must remain `NIL` until the next publish
-   the first publish must produce a valid snapshot

On success the scene prints:

    OK: restart NIL-before-baseline verified

This scene is a **smoke-level contract check** for the Godot boundary.

Because it runs inside the Godot scene lifecycle, its execution may be
influenced by editor/debug scheduling behaviour. The deterministic
verifier for this contract is `restart_boundary_verify`.

------------------------------------------------------------------------

# `pattern_render_bench`

Benchmark tool used to measure pattern renderer performance.

Measures:

-   throughput
-   rendering cost
-   memory bandwidth

Benchmarks **do not validate correctness**.

------------------------------------------------------------------------

# Design Principles

Maintainer tools follow several rules:

1.  **Deterministic verification tools must remain
    hardware-independent.**
2.  **Platform validation must be explicit and opt-in.**
3.  **Smoke tests must remain minimal and fast.**
4.  **Verification tools must produce clear pass/fail results.**
5.  **Benchmarks measure performance but do not assert correctness.**

These principles ensure the CamBANG runtime can be validated
deterministically while keeping platform-dependent behaviour clearly
separated.
