# Current tranche

## Provider-neutral backing-plan diagnostics + Scene 768

### Goal

Give the platform-backed providers (`windows_winrt`, `android_camera2`) their
first machine-checkable coverage of backing-plan evaluation, by exposing the
Core-owned evaluation reports through a provider-neutral accessor and asserting
the decision/reset contract against both synthetic and platform-backed modes in
a single windowless scene.

### Background

`CoreRuntime::backing_plan_evaluation_reports()` already builds these reports
from Core registry state (`stream_retained_plan_evaluators_` /
`capture_retained_plan_evaluators_`), provider-agnostically. Today the only
GDScript route to them is `get_synthetic_metrics_snapshot()`, a dev-only crutch
that fetches synthetic-provider metrics first and bails on a
`dynamic_cast<SyntheticProvider*>` before reaching the Core-sourced reports.
So the reports are invisible under any platform-backed provider purely because
of where they were surfaced, not because of what they are.

### Scope

1. Add `CamBANGServer.get_backing_plan_evaluation_diagnostics()` — a
   provider-neutral GDScript accessor sourced directly from
   `runtime_.backing_plan_evaluation_reports()`, independent of provider type.
   Reuse the existing report→Dictionary conversion. This is a public-API
   addition, authorized by this tranche. Structure it so a future capture-config
   *setter* can sit cleanly beside it (see out-of-scope note).
2. Add Scene 768 (`768_backing_plan_provider_modes_verify`): windowless,
   strict PASS/FAIL via the harness verdict line. Synthetic mode stages the
   existing `568_backing_plan_single_access_live` scenario unchanged; platform
   mode builds the same device+stream shape through the public GDScript API from
   the first enumerated endpoint. Both modes assert the same contract: a
   backing-plan decision is reached for stream and capture, and `stop()` clears
   the evaluation evidence. Modes that cannot run (no platform provider
   compiled, no endpoint, access refused) are reported and skipped, not failed;
   only "no mode ran at all" is `expected_unsupported`.

### Known follow-up (its own future tranche, NOT this one)

- **Platform-backed GPU-fill payload path.** Both platform providers
  (`windows_winrt`, `android_camera2`) advertise CPU-backed only
  (`ProducerBackingCapabilities{true,false,false}`) and deliver CPU payloads
  with a per-frame CPU colour conversion. Real GPU-fill paths exist and are
  unused: Camera2 `AImageReader_newWithUsage` +
  `AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE` (AHardwareBuffer, zero-copy into a
  Vulkan/GL external texture), and WinRT D3D memory preference
  (`IDirect3DSurface`-backed frames). The `FrameView` seam already exists
  (`primary_backing_artifact`, `RetainedGpuBackingDescriptor`,
  `ProducerBackingKind::GPU`) and synthetic exercises it; only the producing
  side is missing in the platform providers. Must be capability-gated
  (advertise GPU only when the specific buffer usage validates on the device),
  never assumed. Implementing it makes platform backing-plan evaluation
  MULTI-candidate, at which point Scene 768 grows a per-candidate probe arm and
  a concurrent capture+stream parent-scoping case (both deferred with it).

### Explicitly out of scope

- The EV-vs-ISO ("exposure vs gain") bracket policy knob. It is a capture-config
  *write* through a different path (device/core/provider + CaptureRequest),
  needs an unmade design decision on how the policy is expressed, and forces a
  full dual-device bracket revalidation. Deferred to its own tranche. Brief
  §5.1 still stands: providers must not invent a local knob meanwhile.
- Snapshot schema changes. These reports stay off the published snapshot by
  design (schema stability, per-tick cost, StatusPanel accounting obligation);
  they are registry-backed truth reached by direct Core query, like the
  `*_for_smoke` family.
- Renaming the Core `CoreBackingPlanEvaluationReport` type (wide blast radius
  through the smoke verifiers). Only the new GDScript accessor is named afresh.
- Scenes 270 and 870. This tranche is 768 only; the others follow separately.

### Acceptance / validation

- Scene 768 PASS on synthetic mode (host and Android).
- Scene 768 PASS on platform-backed mode on device: `android_camera2` over ADB
  on real hardware, exercising the enumerate→engage→stream→capture path and both
  contract assertions.
- Existing scenes unaffected: 65, 66, 73, 568, 870 still status=ok (568 shares
  the report accessor and the scenario; must be confirmed unbroken).
- Host verifiers green: `core_spine_smoke`, `provider_compliance_verify`
  (41/41), `restart_boundary_verify`, `synthetic_only_provider_support_verify`,
  `verify_case_runner --run-all`.
- Provider TU and GDE build clean under `-Wall -Wextra -Wpedantic`.
- Public-API addition is additive only; no existing Godot-facing method,
  signal, constant, or scene behaviour changes.
