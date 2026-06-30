# Current Tranche

This file records the current CamBANG workstream, recent committed outcomes that affect near-term work, and the next intended development direction.

Durable project rules belong in `AGENTS.md`, `docs/dev/agent_context.md`, or the relevant architecture/dev document.

## Active workstream

No small cleanup tranche is currently open.

The recent cleanup sequence is ready to close. The next substantial direction is platform-backed provider implementation readiness, likely best handled as a fresh independent session/workstream rather than continued in this cleanup context.

## Recently closed

### Capture getter-side calibration fallback cleanup

Outcome:

* Removed broad capture calibration/reporting side effects from ordinary capture-result getter paths.
* Capture result retrieval no longer acts as a calibration heartbeat.
* Explicit `to_image_member(index)` access remains instrumented and reports observed member access back into Core through a narrow internal server path.
* Scene 568 now exercises the explicit capture member access seam rather than relying on legacy getter-side calibration.
* No public Godot API, scoring constants, parent-scoped evaluation semantics, or result retrieval semantics changed.

Final validation was run only after stray debug prints were removed and the real GDE DLL was rebuilt.

Reported final validation:

* `provider_compliance_verify` passed.
* `core_result_path_smoke` passed.
* `synthetic_only_provider_support_verify` passed.
* Windows unsandboxed Scene 568 Mobile/runtime_default passed.
* Windows unsandboxed Scene 68 passed.
* Windows unsandboxed Scene 70 passed.

Not run:

* Android Godot validation.
* Final matrix/soak rerun.

### Timing/evidence semantics clarification

Outcome:

* Clarified maintainer documentation for the current timing/evidence fields.
* No runtime behaviour, public API, scoring constants, or measurement machinery changed.
* Score tuning remains deferred.

### Bracket whole-result scoring

Outcome:

* Capture-side scoring now uses readiness once plus conservative required-member materialization.
* Required members are derived from the applied still-image bundle/profile.
* Single-member capture remains the member-0 degenerate case.
* No fixed three-member assumption was introduced.

## Near-term guardrails

Do not reopen the following without new source evidence:

* parent ownership and migration semantics;
* retained-result calibration lifecycle;
* bracket whole-result scoring aggregation;
* capture getter-side fallback removal;
* explicit `to_image_member(index)` capture observation reporting;
* Scene 568 terminal rollover semantics;
* Scene 568 Compatibility `gpu_only` expected-negative classification;
* `topology_change_versions` settled-snapshot wait.

## Deferred

Not part of any currently open cleanup tranche:

* score tuning;
* platform-backed provider implementation readiness;
* Scene 70 latency investigation only if reproducible after a clean system state;
* optional Godot-side N-member whole-result scoring proof, only if later judged worth the extra scene/harness work.
