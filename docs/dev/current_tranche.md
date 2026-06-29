# Current Tranche

This file records the current CamBANG workstream, recent committed outcomes that affect near-term work, and the next intended development direction.

Durable project rules belong in `AGENTS.md`, `docs/dev/agent_context.md`, or the relevant architecture/dev document.

## Active workstream

Timing measurement and evidence-quality review.

Bracket whole-result scoring is now closed. The next workstream should review the quality, interpretation, and usefulness of current timing/evidence measurements before any further score tuning or provider-backed work.

Start with a source-grounded audit. Do not tune scoring constants or add new measurement machinery before identifying what current measurements mean, where they are taken, and which decisions they affect.

## Recently closed

### Bracket whole-result scoring

Committed.

Outcome:

* Capture-side materialization observations now carry image-member identity through the internal runtime seam.
* Capture candidate evidence now stores per-required-member materialization observations.
* Capture candidate completeness now requires:

    * capture readiness for the relevant capture/session identity; and
    * materialization evidence for every required member in the applied still-image bundle for that evaluation epoch.
* Capture scoring now counts readiness latency once and aggregates required-member materialization conservatively.
* The slowest required member dominates aggregate materialization cost.
* Single-member capture remains the degenerate member-0 case.
* The required member set is derived from the applied still-image bundle/profile; no fixed three-member assumption was introduced.
* Public Godot API was unchanged.

Reported validation:

* `provider_compliance_verify` passed.
* `provider_compliance_verify --only_check=run_core_capture_bracket_whole_result_scoring_check` passed.
* `core_result_path_smoke` passed.
* `synthetic_only_provider_support_verify` passed.
* Scene 568 Windows/Android matrix logs were reviewed and showed complete, accurate plan-evaluation decisions for the covered matrix.
* Scene 568 Compatibility `gpu_only` expected-negative classification remained correct.
* Scene 70 was not run in this sweep.
* Godot-side N-member whole-result scoring validation was not added or run.

### Lightweight Godot validation convention

Committed.

Outcome:

* Added a compact Godot validation convention to the Godot test-project README.
* Defined a named Godot validation surface.
* Defined reporting for supported cases, expected-negative cases, and not-run Godot validation.
* Made the native maintainer-tool versus Godot-scene distinction explicit.
* Used Scene 568 only as a current example.
* Reaffirmed that Godot validation must not be claimed unless the relevant local/helper/manual path was actually run.

### `topology_change_versions` verifier sampling cleanup

Committed.

Outcome:

* `topology_change_versions()` waits after `destroy_stream()` for the settled observable shape before asserting.
* The expected final shape remains strict: stream gone and top-level `acquisition_sessions[]` empty.
* The issue was verifier-side early sampling of a valid transitional publication boundary, not a broken Core topology invariant.

Reported validation:

* `verify_case_runner.exe topology_change_versions --repeat=1000` passed.

## Near-term guardrails

Do not reopen the following without new source evidence:

* parent ownership and migration semantics;
* retained-result calibration lifecycle;
* capture-side bundle-sensitive invalidation / seed reuse / re-arming;
* bracket whole-result scoring aggregation;
* Scene 568 terminal rollover semantics;
* Scene 568 Compatibility `gpu_only` expected-negative classification;
* `topology_change_versions` settled-snapshot wait;
* the distinction between current/live top-level `acquisition_sessions[]` truth and historical teardown visibility in `native_objects`.

## Deferred

Not part of the active timing/evidence-quality review:

* score tuning;
* platform-backed provider work;
* further removal or tightening of capture getter-side calibration fallback;
* Scene 70 latency investigation only if reproducible after a clean system state;
* optional Godot-side N-member whole-result scoring proof, only if later judged worth the extra scene/harness work.
