# Maintainer Tools

This document describes the small command-line utilities used by
maintainers to validate internal invariants, provider behaviour, and
selected performance characteristics of the CamBANG runtime.

These tools are **not user-facing applications** and are not intended
to ship as part of production builds. They exist to help maintainers
validate correctness while developing the runtime.

Most tools are **opt-in build artifacts** and will not appear in normal
build outputs unless explicitly requested.

------------------------------------------------------------------------

## Tool Categories

Maintainer tools fall into three broad categories.

| Category | Purpose |
|---|---|
| Smoke test | Minimal sanity check ensuring the core runtime spine operates correctly |
| Verification | Deterministic validation of specific runtime invariants |
| Benchmark | Performance measurement utilities |

------------------------------------------------------------------------

## Tool Overview

| Tool | Purpose | Category |
|---|---|---|
| `core_spine_smoke` | Minimal Core runtime invariant validation using the stub provider | Smoke test |
| `synthetic_timeline_verify` | Deterministic verification of SyntheticProvider timeline behaviour and Core registry truth | Verification |
| `provider_compliance_verify` | Deterministic provider-contract verification using Stub and Synthetic only | Verification |
| `windows_mf_runtime_validate` | Opt-in Windows Media Foundation runtime validation against real hardware | Verification |
| `pattern_render_bench` | Pattern renderer performance benchmark | Benchmark |

------------------------------------------------------------------------

## Terminology

**Smoke Test**

A minimal executable that validates the runtime spine:

- Core runtime start
- provider registration
- state publication
- basic teardown

Smoke tests should remain **deterministic and provider-independent**.
They must not depend on real hardware.

---

**Verification Tool**

A deterministic executable used to validate specific internal invariants
or subsystem behaviour.

Verification tools should:

- be deterministic
- avoid platform hardware dependencies
- produce a clear pass/fail result
- run quickly

---

**Platform Runtime Validation**

A maintainer tool that validates a platform-backed provider against real
OS APIs and, where applicable, real hardware.

Unlike deterministic verification tools, platform validation may depend
on:

- device presence
- driver behaviour
- negotiated formats
- local machine state

Platform runtime validation must remain **explicitly opt-in**.

---

**Benchmark**

A performance measurement utility used to characterise runtime behaviour.

Benchmarks do not validate correctness; they measure throughput,
latency, or resource usage.

------------------------------------------------------------------------

## Provider Validation Tools

Provider correctness is validated in **two layers**:

1. Deterministic provider-contract verification
2. Platform-backed runtime validation

This layering matches the architecture rules defined in:

- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`

Deterministic provider validation must not rely on physical hardware.
Platform validation is performed separately using explicit tools.

------------------------------------------------------------------------

## `provider_compliance_verify`

**Category:** Verification Tool

### Purpose

`provider_compliance_verify` validates the provider contract and
lifecycle rules using only deterministic providers.

It exercises:

- StubProvider
- SyntheticProvider

This tool is the primary maintainer check for the provider compliance
rules recorded in:

- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`
- `docs/dev/provider_compliance_checklist.md`

### What it validates

The tool verifies core provider-contract invariants such as:

- serialized provider → core callback delivery
- lifecycle ordering correctness
- native-object create/destroy reporting
- non-lossy lifecycle/native-object/error delivery
- deterministic shutdown sequencing
- SyntheticProvider ordering and timeline invariants

### What it does not do

`provider_compliance_verify` deliberately does **not**:

- enumerate physical cameras
- open platform-backed devices
- validate Media Foundation or other platform APIs
- exercise real asynchronous device callbacks

Those concerns belong to platform validation tools.

### Build

This tool is built only in smoke / maintainer-tool configurations.

Typical build form:

```text
scons smoke=1 smoke
```

The executable will appear in the `out/` directory.

### Usage

```text
./out/provider_compliance_verify.exe
```

(or the platform-equivalent path)

No flags are required.

### Expected result

On a healthy runtime path the tool should complete quickly and print:

```text
PASS provider_compliance_verify
```

Any failure indicates a provider-contract regression.

Typical failures include:

- incorrect lifecycle ordering
- missing native-object lifecycle events
- shutdown sequencing regressions
- SyntheticProvider invariant violations

Failures should be treated as maintainer-facing correctness issues.

------------------------------------------------------------------------

## `windows_mf_runtime_validate`

**Category:** Verification Tool (platform-backed)

### Purpose

`windows_mf_runtime_validate` validates the Windows Media Foundation
provider under **real asynchronous hardware-backed execution**.

It exercises:

- WindowsMediaFoundationProvider
- Media Foundation device enumeration
- real device open
- stream start / stop
- shutdown behaviour

This tool complements deterministic provider verification but does not
replace it.

### Important expectation

Unlike deterministic verification tools, this executable may depend on:

- camera hardware
- driver behaviour
- Media Foundation negotiation results
- local machine state

It is therefore **opt-in** and intended only for maintainers.

### Build

The executable should only be built when explicitly requested.

Typical build form:

```text
scons platform_validate=1 windows_mf_runtime_validate
```

It must not appear in the default build output.

### Usage

To prevent accidental hardware access, the executable requires an
explicit runtime flag before it will open a camera:

```text
./out/windows_mf_runtime_validate.exe --real-hardware
```

Without this flag the tool should refuse to run and exit cleanly.

### What it validates

The tool checks Media Foundation backend behaviour including:

- device enumeration
- media-type negotiation
- stream start / stop
- shutdown sequencing
- worker thread exit discipline
- bounded shutdown behaviour
- preservation of serialized provider → core callback delivery

### Expected result

On a healthy camera path the tool should:

1. enumerate a camera
2. open a device
3. negotiate a media type
4. start a stream
5. stop and shut down cleanly

The tool should exit successfully and print a final pass message.

### Failure interpretation

Failures indicate **backend runtime issues** such as:

- shutdown timeout
- worker-thread exit failure
- Media Foundation negotiation problems
- driver or device behaviour issues

These failures do not necessarily indicate a violation of deterministic
provider-contract invariants.

------------------------------------------------------------------------

## Other Tools

### `core_spine_smoke`

Minimal runtime sanity check validating the Core runtime spine using the
Stub provider.

This tool verifies that the runtime can:

- start
- process basic provider events
- publish state
- shut down cleanly

It should remain completely deterministic.

---

### `synthetic_timeline_verify`

Validates deterministic behaviour of the SyntheticProvider timeline and
its interaction with the core lifecycle registry.

It is used primarily to validate:

- deterministic frame scheduling
- correct lifecycle event emission
- registry truthfulness under timeline-driven events

---

### `pattern_render_bench`

Benchmark tool used to measure the performance of the pattern renderer.

This tool measures throughput and rendering cost but does not validate
correctness.

------------------------------------------------------------------------

## Design Principles

Maintainer tools follow a few simple rules:

1. **Deterministic tools must remain hardware-independent.**

2. **Platform validation must be explicit and opt-in.**

3. **Smoke tests must remain minimal and fast.**

4. **Verification tools should produce a clear pass/fail outcome.**

5. **Benchmarks measure performance but do not assert correctness.**

These principles keep the development workflow predictable and ensure
that core runtime invariants can be validated independently of platform
hardware behaviour.

------------------------------------------------------------------------
