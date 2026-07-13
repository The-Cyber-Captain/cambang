# Current Tranche

## Camera Facts Tranche 7 - Result Resolution

### Objective

Resolve retained camera facts into immutable completed still-capture results,
using the authority model in `docs/camera_fact_model.md`.

### Implementation Scope

Resolve only still-capture camera facts at completed-result assembly. Apply the
following precedence independently for each fact:

1. matching external fact;
2. matching provider per-capture-image fact;
3. provider static fact;
4. absent.

### Critical Invariants

- Facts resolve independently.
- Intrinsics, distortion, and pose remain atomic.
- Provenance is preserved.
- Exact capture, device, and member identity is maintained.
- Admission context is not resampled.
- Completed results remain immutable.

### Exclusions

- No Godot or public API work.
- No public result exposure.
- No stream-result enrichment.
- No ADC, provider-ingress, SyntheticProvider-generation, admission-context, or
  state-snapshot changes.
- No calibration, scaling, coordinate conversion, or distortion fitting.

### Acceptance Criteria

Focused checks cover precedence, fallback, provenance, absence, bracket and rig
isolation, shared admission context, and result immutability.

### Required Validation

- maintainer-tools build;
- focused Tranche 7 checks;
- full `core_spine_smoke.exe --verbose`;
- Windows GDE build;
- Android arm64 GDE build.

All required validation is governed by the hard completion contract in
`AGENTS.md`.
