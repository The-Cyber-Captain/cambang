# Current Tranche

## Camera Facts Tranche 6 — SyntheticProvider Reference Facts

### Objective

Make `SyntheticProvider` the reference producer for the provider-camera-fact
ingress completed in Tranche 5.

### Scope

- Supply deterministic static camera facts for each synthetic device through
  the existing provider-to-Core fact ingress.
- Supply deterministic per-capture-image facts for each successfully emitted
  still-image member through that same ingress.
- Preserve exact device, capture, rig-participant, and image-member identity.
- Keep facts internally consistent with the synthetic device topology and
  emitted image dimensions.
- Use the existing source-neutral fact types, validation, ownership, and
  retention rules.

### Required behaviour

- Synthetic devices are represented as virtual cameras.
- Synthetic image geometry is represented as rectified/undistorted; do not
  invent nonzero distortion.
- Intrinsics and any supplied pose facts are deterministic and valid under the
  existing camera-fact contracts.
- Distinct synthetic devices remain distinguishable.
- Every emitted bracket member receives facts under its correct member index.
- Rig participants and their members do not cross-contaminate.
- Facts travel through the normal provider callback and Core ingress path; no
  direct Core-state bypass is permitted.
- Valid absence remains acceptable for facts that `SyntheticProvider` cannot
  truthfully supply.

### Out of scope

- External-versus-provider precedence or result resolution.
- `CaptureResult` or `StreamResult` fact exposure.
- State-snapshot fields.
- Godot bindings or any other public API change.
- ADC ingestion or retained external fact changes.
- Capture-admission context changes.
- Production-provider implementation beyond `SyntheticProvider`.
- Calibration, rectification, projection, intrinsic scaling, coordinate
  conversion, or approximate distortion fitting.

### Acceptance criteria

Focused provider-compliance coverage must demonstrate:

- static facts for each relevant synthetic device;
- deterministic values and correct source classification;
- correct handling of intentionally absent facts;
- per-image facts for single-member and multi-member captures;
- correct capture, device, participant, and member identity;
- no cross-contamination between rig participants or bracket members;
- correct device-close and generation/restart lifecycle behaviour;
- use of the normal Tranche 5 ingress and Core-owned retention;
- no regression of existing `SyntheticProvider` capture behaviour.

### Required validation

- maintainer-tools build;
- focused Tranche 6 provider-compliance check or checks;
- full `core_spine_smoke.exe --verbose`;
- Windows GDE build;
- Android arm64 GDE build.

All required validation is governed by the hard completion contract in
`AGENTS.md`.
