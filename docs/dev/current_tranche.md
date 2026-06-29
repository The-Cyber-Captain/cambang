# Current Tranche

This file records the current CamBANG workstream, recent committed outcomes that affect near-term work, and the next intended development direction.

Durable project rules belong in `AGENTS.md`, `docs/dev/agent_context.md`, or the relevant architecture/dev document.

## Active workstream

Lightweight Godot validation convention.

The next task is to document a small, consistent convention for naming and reporting Godot validation surfaces.

This should remain low-investment documentation work. It should not become a new framework, runner redesign, or broad scene-refactor tranche.

Desired direction:

* define how a Godot validation surface is named;
* define how supported cases, expected-negative cases, and not-run cases are reported;
* make clear that native maintainer-tool validation does not imply Godot scene validation;
* use Scene 568 as the current example of a scene that mostly meets the convention;
* adapt older scenes only later, and only where doing so is cheaper than preserving ambiguous legacy wording.

Expected output: one compact documentation update in the most appropriate existing dev/testing documentation location, or a small new dev note if no suitable location exists.

## Recently closed

### Scene 568 Compatibility `gpu_only` expected-negative cleanup

Committed.

Outcome:

* Scene 568 detects Compatibility renderer selection from ProjectSettings and command-line engine arguments.
* Scene 568 + Compatibility renderer + Synthetic `gpu_only` is classified as expected unsupported.
* The expected-negative rule remains narrow.
* Fresh `CamBANGDevice` wrappers can observe completed capture results.
* Active capture evaluators re-arm retained-result calibration consistently with stream evaluators.

Reported validation:

* native maintainer checks passed;
* Windows Scene 568 Mobile/runtime_default soak passed `30/30`;
* Windows and Android Scene 568 Compatibility/`gpu_only` classified as `EXPECTED_UNSUPPORTED`.

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

* current Backing Plan scoring;
* parent ownership and migration semantics;
* retained-result calibration lifecycle;
* Scene 568 terminal rollover semantics;
* Scene 568 Compatibility `gpu_only` expected-negative classification;
* `topology_change_versions` settled-snapshot wait;
* the distinction between current/live top-level `acquisition_sessions[]` truth and historical teardown visibility in `native_objects`.

## Deferred

Not part of the active lightweight validation-convention work:

* bracket whole-result scoring;
* timing measurement and evidence-quality review;
* score tuning;
* platform-backed provider work;
* further removal or tightening of capture getter-side calibration fallback;
* Scene 70 latency investigation only if reproducible after a clean system state.
