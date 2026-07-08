# Current Tranche

Volatile handoff for the active CamBANG workstream. Durable rules remain in `AGENTS.md`, `docs/dev/agent_context.md`, and canonical architecture/dev docs.

## Active workstream

### Scene 870 capture-readiness optimisation investigation

Goal: inspect whether Scene 870 capture readiness contains a small, source-obvious, platform-agnostic optimisation opportunity. This is a performance pass after the attribution/evidence correctness issue was resolved.

Current evidence:

* Multi-viable bracketed capture evidence is green: required/materialized/observed/returned member counts complete for the required 5-member result.
* Capture-side Backing Plan evaluation correctly scores whole-result readiness plus required-member materialization, not later `to_image_member()` timing alone.
* `to_image()` / `to_image_member()` materialization is comfortably fast and is not the current bottleneck.
* `live_cpu_display_refresh_observed` remains expected only in CPU-backed display cells. Treat it in Mobile `cpu_gpu`, `runtime_default`, or `gpu_only` as a regression.
* Android Mobile capture readiness is dominated by provider-side capture production/prep, post-frame terminal tail, and GPU retain/create cost where applicable.
* GPU-backed candidates may legitimately lose capture scoring despite faster materialization if their readiness total is higher.

Current named stage buckets:

* `pre_capture_started_total_ms`
* `post_capture_started_cpu_prep_total_ms`
* `post_capture_started_gpu_retain_total_ms`
* `post_capture_started_post_frame_total_ms`
* `capture_terminal_post_total_ms`
* `capture_ready_provider_window_total_ms`

## Immediate task

Inspect source and determine whether any narrow readiness optimisation is justified.

Primary source region:

* `src/imaging/synthetic/provider.cpp` / `.h`

Secondary regions only if source evidence requires them:

* `src/godot/cambang_server.cpp`
* `tests/cambang_gde/scripts/870_to_image_soak_benchmark.gd`
* `src/core/core_runtime.cpp`
* acquisition-session / result-store files

Answer:

* Is bracket capture repeating common prep/render/allocation work per member that can safely be shared?
* Is there avoidable work between last capture member production and terminal completion?
* Is GPU retain/create duplicated, serialized unnecessarily, or scoped too broadly?
* Are waits, locks, queue posts, joins, or timed gaps contributing avoidable readiness delay?
* Are current timing buckets broad enough to hide a small but source-obvious optimisation?

A code change is acceptable only if it is narrow, source-obvious, platform-agnostic, and preserves truth/evidence semantics.

If no safe optimisation is found, report the limiting source path and leave behaviour unchanged.

## Guardrails

Do not:

* change Backing Plan chooser policy;
* treat `to_image_member()` timing alone as the AcquisitionSession score;
* fabricate lazy GPU backing truth;
* add or change public Godot API;
* change snapshot schema;
* add Android-only, Windows-only, renderer-specific, or platform-specific branches;
* add persistent environment knobs;
* move Godot/RD ownership into Core or provider API;
* weaken Scene 870, Scene 568, smoke tools, or expected-negative checks;
* reduce Scene 870 load to make numbers look good;
* broad-refactor SyntheticProvider, worker/executor shape, or GPU display architecture.

Report source/doc mismatches instead of silently choosing one.

## Acceptance / validation

A satisfactory result provides one of:

* a small safe optimisation with source-grounded explanation; or
* a source-grounded explanation that no narrow safe optimisation is currently justified.

The result must preserve:

* green required-member evidence in multi-viable bracket cells;
* no `live_cpu_display_refresh_observed` in Mobile mixed/GPU cells;
* no runtime truth, retained backing truth, public API, chooser-policy, or Scene 870 load change.

Minimum validation after code changes:

* build touched target(s);
* `provider_compliance_verify`;
* `core_result_path_smoke`;
* `synthetic_only_provider_support_verify`;
* Scene 568 if Backing Plan evaluator evidence/reporting is touched;
* targeted Scene 870:

  * Android Mobile `runtime_default`
  * Android Mobile `cpu_gpu`
  * Android Mobile `gpu_only`
  * Windows Mobile counterparts if practical

If Android/Godot validation is not run, say so explicitly.
