# Current Tranche

This document records active development context for CamBANG. It is intentionally more volatile than `AGENTS.md` and `docs/dev/agent_context.md`.

Agents should use this file for current workstream direction, recent findings, and near-term constraints. Durable engineering rules belong in `AGENTS.md` or `docs/dev/agent_context.md`.

## Current focus

Continue source-grounded CamBANG development without broadening scope or weakening verification.

Read together:

* `AGENTS.md`;
* `docs/dev/agent_context.md`;
* this file; and
* `docs/dev/backing_plan_parent_evaluation_reset.md`.

The detailed parent-scoped design remains in `docs/dev/backing_plan_parent_evaluation_reset.md`. This file is only the compact tranche-state handoff note.

### Active issue: intermittent `topology_change_versions` acquisition-session mismatch

Before moving on to Godot validation matrix documentation, investigate and fix the intermittent native verifier failure:

```text
./out/verify_case_runner.exe topology_change_versions --repeat=100
```

Observed failure shape:

```text
[repeat] iteration 4/100
step 0 OK
step 1 OK
step 2 OK
[ERROR] step 3 FAIL (acquisition_session_count mismatch; expect_acquisition_session mismatch expected=false)
```

Treat this as a bounded native verifier / topology-publication / lifecycle-truth investigation.

Initial constraints:

* Do not weaken `topology_change_versions` expectations merely to get green.
* Do not hide intermittent topology mismatches behind retry logic.
* Do not reopen Backing Plan scoring, capture-parent ownership, retained-result calibration lifecycle, or Scene 568 terminal rollover semantics unless source evidence directly implicates them.
* Do not change public Godot API.
* Inspect the verification case, publication/topology-version handling, top-level `acquisition_sessions[]` projection, and SyntheticProvider acquisition-session lifecycle ordering before proposing changes.
* Preserve the distinction that top-level `acquisition_sessions[]` reports current/live retained `AcquisitionSession` truth, while destroyed/history diagnosis remains in `native_objects`.
* A valid fix should make `verify_case_runner.exe topology_change_versions --repeat=100` stable without masking real topology/version defects.

No new documentation-follow-on tranche should start until this intermittent native verifier issue is either fixed and validated, or explicitly documented as out-of-scope with evidence.

## Completed tranches

### Capture-parent / evidence-integrity tranche

The capture-parent/evidence-integrity tranche is signed off and committed.

Completed source truth for that tranche:

* `ProviderBroker` forwards capture-parent priming calls to the active backend.
* Capture-side Backing Plan evaluation may begin under `CapturePriming`.
* A provider-primed `AcquisitionSession` is lifecycle truth only; evaluator migration to a real `AcquisitionSession` occurs only after first truthful capture participation.
* Completed measured decisions and measured evidence do not survive real `AcquisitionSession` retirement.
* Same-signature reuse carries only the winning posture as a non-measured seed.
* Immediate terminal rollover to a clean provisional `CapturePriming` seed-only epoch is valid.
* Scene 568 accepts both visible settled decisions and valid immediate terminal rollover.
* Cross-real-session measured evidence carry remains prohibited.
* `ProviderBroker` priming forwarding is covered by a deterministic regression.

### Retained-result access calibration lifecycle tranche

The retained-result access calibration lifecycle tranche is signed off.

Completed source truth for that tranche:

* Automatic retained-result access calibration is now armed from bounded live stream/capture identity rather than the old broad live-result polling loop.
* Calibration renewal/proactive lifecycle is tied to live applied result/posture/access identity.
* Active stream evaluators use evaluator-specific arming.
* Generic live-stream arming excludes active evaluator streams, so generic live-stream calibration cannot overwrite or continually reset evaluator-specific pending calibration.
* Stream candidate/posture transitions can automatically re-arm `display_view` and `to_image` evidence without public `to_image()` calls.
* Generic live-stream calibration remains bounded by live identity and is not the old “calibrate every live result forever” architecture.
* Capture retained-result calibration remains bounded by truthful capture identity and remains image-member-aware.
* Capture getter-side calibration remains only as a temporary compatibility fallback; it is not the intended normal lifecycle.
* Real operation helpers remain the evidence seams:

  * `perform_stream_display_view_access`;
  * `perform_stream_to_image_access`;
  * `perform_capture_to_image_member_access`; and
  * Core observation reporting handlers.
* Persistent display-view refresh remains frame maintenance for live views, not calibration authority.
* Core Backing Plan candidate ordering, scoring, settle timers, parent migration, evidence acceptance, candidate advancement, and finalisation remain unchanged.
* No public calibration API, GDScript control, environment variable, diagnostic knob, user-facing setting, or required user workflow was introduced.

Validation status for this tranche:

* `provider_compliance_verify` passed.
* `core_result_path_smoke` passed.
* `synthetic_only_provider_support_verify` passed.
* Scene 568 supported matrix passed, excluding the known Compatibility `gpu_only` expected-negative cases.
* Scene 70 single-metered supported matrix passed.
* Scene 70 three-member bracket supported matrix passed.

### Scene 568 Compatibility `gpu_only` expected-negative cleanup

The Scene 568 Compatibility `gpu_only` expected-negative cleanup is landed.

Completed source truth for that tranche:

* Scene 568 now recognizes Compatibility renderer selection from both ProjectSettings and command-line engine arguments.
* The known unsupported combination of Scene 568 + Compatibility renderer + Synthetic producer output form `gpu_only` is classified as expected unsupported / expected-negative rather than a generic regression.
* The expected-negative rule is narrow and must not cover Compatibility `cpu_only`, Compatibility `cpu_gpu`, Mobile `gpu_only`, Scene 70 failures, timeouts, parse/load failures, or different failure reasons.
* Fresh `CamBANGDevice` wrappers can observe completed capture results without relying only on a stale local cached capture id.
* Active capture evaluators now re-arm retained-result calibration consistently with stream evaluators.
* These runtime corrections are bug fixes to the settled result-observation and evaluator lifecycle model; they are not changes to Backing Plan scoring, parent ownership, public Godot API semantics, or Scene 568 terminal rollover semantics.

Validation status for this tranche:

* `provider_compliance_verify` passed.
* `core_result_path_smoke` passed.
* `synthetic_only_provider_support_verify` passed.
* Windows Scene 568 Mobile/runtime_default soak passed `30/30`.
* Windows Scene 568 Compatibility/`gpu_only` classified as `EXPECTED_UNSUPPORTED`.
* Android Scene 568 Mobile/runtime_default passed.
* Android Scene 568 Compatibility/`gpu_only` classified as `EXPECTED_UNSUPPORTED`.
* Scene 70 single-metered and three-member bracket matrices were not rerun in that sweep.

## Do not reopen without new source evidence

Do not reopen these unless current source evidence disproves them:

* Synthetic refcount semantics;
* `ProviderBroker` capture-parent priming forwarding;
* `CapturePriming -> first truthful real AcquisitionSession` migration;
* provider-primed `AcquisitionSession` as lifecycle truth only;
* cross-real-session evidence rehome/merge prohibition;
* same-signature seed-only reuse;
* Scene 568 terminal rollover semantics;
* Scene 568 Compatibility `gpu_only` expected-negative classification;
* fresh-wrapper capture result lookup as a narrow result-observation bug fix;
* active capture evaluator retained-result calibration re-arming as part of the settled evaluator lifecycle;
* current Backing Plan scoring;
* Core Backing Plan candidate ordering, settle timers, parent migration, evidence acceptance, candidate advancement, and finalisation;
* retained-result access evidence being gathered through real operation seams;
* active stream evaluator arming being separate from generic live-stream arming; and
* persistent display-view refresh being frame maintenance rather than calibration authority.

If source contradicts one of these points, report the contradiction explicitly before proposing changes.

## Known temporary fallback

Capture getter-side calibration remains as a temporary compatibility fallback.

Do not expand this fallback into the normal lifecycle. Future work may remove or further constrain it only after automatic calibration coverage is proven sufficient without it.

## Deferred items

These are known follow-on items, not part of the completed retained-result access calibration lifecycle tranche, the completed Scene 568 expected-negative cleanup, or the active `topology_change_versions` investigation:

* Godot validation matrix documentation.
* Bracket whole-result scoring.
* Score tuning.
* Platform-backed provider work.
* Further removal or tightening of the temporary capture getter-side calibration fallback.
* Investigation of Scene 70 latency only if it becomes reproducible after a clean system state.

The Scene 568 Compatibility `gpu_only` expected-negative cleanup is complete and should not remain listed as deferred work.

## Validation guidance

Manual local validation remains authoritative for Godot scene, GPU/display-view, and platform-sensitive behaviour.

Do not claim Godot Scene 568 or Scene 70 validation unless the relevant local helper path was actually run in an approved local environment.

When Godot validation is not run, report it explicitly rather than implying coverage.

For the active `topology_change_versions` issue, the minimum native closure signal should include a successful repeated run, for example:

```text
./out/verify_case_runner.exe topology_change_versions --repeat=100
```

A fix that changes the verifier expectation, skips the acquisition-session assertion, or masks intermittent failures with retry logic is not acceptable validation.
