# Current Tranche

Volatile handoff for the active CamBANG workstream. Durable rules remain in `AGENTS.md`, `docs/dev/agent_context.md`, and canonical architecture/dev docs.

## Active workstream

### Scene 870 provider-side no-sidecar readiness residual

Goal: explain the remaining Android Mobile `GPU-primary, no CPU sidecar` capture-readiness penalty in Scene 870 without changing runtime behaviour.

Current evidence:

- Multi-viable bracketed capture evidence is green: required/materialized/observed/returned member counts complete for the required 5-member result.
- `live_cpu_display_refresh_observed` remains expected only in CPU-backed display cells. Treat it in Mobile `cpu_gpu`, `runtime_default`, or `gpu_only` as a regression.
- Capture-side Backing Plan evaluation correctly scores whole-result readiness plus required-member materialization, not later `to_image_member()` timing alone.
- `capture_ready_elapsed_ns` means acquisition-session `capture_started -> capture_completed` lifecycle/readiness timing. Do not redefine it unless a narrow, source-obvious correctness bug is found and explicitly justified.
- GPU retain/create timing is material and explains `GPU-primary, with CPU sidecar` readiness loss well.
- GPU retain/create, first retain-call cost, provider-to-core ingress delay, and removed trace-formatting overhead do **not** explain the remaining Android `GPU-primary, no CPU sidecar` residual.
- Post-cleanup evidence shows `capture_ready_elapsed_ns` closely matches provider-side `post_capture_started -> post_capture_completed` wall time.
- The remaining residual appears to be unmeasured provider-side wall time inside `post_capture_started -> post_capture_completed`, outside current named stage buckets.

Current named stage buckets:

- `pre_capture_started_total_ms`
- `post_capture_started_cpu_prep_total_ms`
- `post_capture_started_gpu_retain_total_ms`
- `post_capture_started_post_frame_total_ms`
- `capture_terminal_post_total_ms`
- `capture_ready_provider_window_total_ms`

## Immediate task

Perform one narrow attribution pass. Do **not** optimise.

Start with source inspection. If a narrow missing region is confirmed, add only compact maintainer-only diagnostics through existing synthetic metrics / Scene 870 summary surfaces.

Answer:

- What provider-side work or wait inside `post_capture_started -> post_capture_completed` is not covered by the current stage buckets?
- Is the gap caused by sleeps/simulated latency, worker scheduling/joining, lock waits, queue/post waits, per-member gaps between timed regions, or posture-specific branches?
- Are current stage bucket definitions missing real work between measured blocks?

Primary source regions:

- `src/imaging/synthetic/provider.cpp` / `.h` — primary focus: capture production interval, per-member gaps, worker/device job boundaries, joins, locks, frame posting, capture started/completed posting.
- `src/godot/cambang_server.cpp` — synthetic metrics export, only if new metrics are added.
- `tests/cambang_gde/scripts/870_to_image_soak_benchmark.gd` — Scene 870 summary attachment, only if new fields are exported.
- `src/core/core_runtime.cpp` and acquisition-session registry files only if source evidence shows provider-side attribution is insufficient.

Preferred shape:

- compact per-posture or per-candidate totals, not broad timeline dumps;
- preserve existing GPU retain and ready-stage metrics;
- maintainer-only diagnostics only;
- no public Godot API or snapshot schema changes;
- no runtime optimisation in this task.

## Guardrails

Do not:

- change Backing Plan chooser policy;
- treat `to_image_member()` timing alone as the AcquisitionSession score;
- fabricate lazy GPU backing truth;
- add or change public Godot API;
- add persistent environment knobs;
- move Godot/RD ownership into Core or provider API;
- add Android-only/Core platform conditionals;
- weaken Scene 870, Scene 568, smoke tools, or expected-negative checks;
- reduce Scene 870 load to make numbers look good;
- optimise GPU retain/create, add texture pooling, add warmup/settle policy, or change worker/executor shape in this diagnostic task.

Report source/doc mismatches instead of silently choosing one.

## Acceptance / validation

A satisfactory result provides:

- a source-grounded explanation of the remaining no-sidecar residual, or a bounded statement of what remains unknown;
- Scene 870 output separating readiness, GPU retain/create, materialization, named provider-side stages, and remaining unmeasured provider-side wall time;
- green required-member evidence in multi-viable bracket cells;
- no `live_cpu_display_refresh_observed` in Mobile mixed/GPU cells;
- no runtime behaviour, chooser policy, public API, Scene 870 load, or retained backing truth change.

Minimum validation after code changes:

- build touched target(s);
- `provider_compliance_verify`;
- `core_result_path_smoke`;
- `synthetic_only_provider_support_verify`;
- Scene 568 if Backing Plan evaluator evidence/reporting is touched;
- targeted Scene 870 before any full matrix:
  - Android Mobile `runtime_default`
  - Android Mobile `cpu_gpu`
  - Android Mobile `gpu_only`
  - Windows Mobile counterparts if practical

If Android/Godot validation is not run, say so explicitly.

## Deferred

- chooser redesign;
- public API changes;
- platform-backed provider work;
- broad GPU display architecture redesign;
- Scene 870 load-model redesign;
- runtime optimisation, texture pooling, warmup policy, or Synthetic worker/executor changes before the residual is attributed.
