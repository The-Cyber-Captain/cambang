# Current Tranche

This document records active development context for CamBANG. It is intentionally more volatile than `AGENTS.md` and `docs/dev/agent_context.md`.

Agents should use this file for current workstream direction, recent findings, and near-term constraints. Durable engineering rules belong in `AGENTS.md` or `docs/dev/agent_context.md`.

## Current focus

Continue source-grounded CamBANG development without broadening scope or weakening verification.

The parent-scoped Backing Plan design remains the accepted direction, and the older intent-based chooser remains superseded. However, Scene 568 log review and follow-up source inspection have reopened the implementation of the Backing Plan evaluation and retained-result access-calibration lifecycle.

Do not treat the current implementation, existing PASS results, or prior wording that this tranche had fully landed as architecture acceptance.

The immediate task is a read-only design-to-source reconciliation, followed by narrow implementation tranches. Preserve the agreed CamBANG-specific design; repair its implementation rather than replacing it with a generic calibration, scheduler, or benchmarking architecture.

## Design authority for this tranche

Read together:

* `AGENTS.md`;
* `docs/dev/agent_context.md`;
* this file; and
* `docs/dev/backing_plan_parent_evaluation_reset.md`.

The detailed parent-scoped design remains in `docs/dev/backing_plan_parent_evaluation_reset.md`. The following points are especially important for the current repair:

* Backing Plan evaluation is owned by the Native Payload Support Parent: `Stream` for repeating-stream work and `AcquisitionSession` for still-capture work.
* Stream evaluation optimizes the real display-access operation.
* Capture evaluation optimizes still-result readiness plus real required-result materialization cost.
* Measurement and classification are automatic, behind-the-scenes runtime work. Normal users and GDScript do not initiate, advance, acknowledge, or poll calibration.
* Internal calibration uses the same real operation implementation as public `get_display_view()`, `to_image()`, and `to_image_member()` access, without requiring those public calls or creating public-demand semantics.
* Calibration remains proactive at the live applied-posture boundary, bounded, and non-per-frame.
* Single-viable-candidate Backing Plan selection is direct, but supported retained-result access operations for that applied posture are still automatically calibrated.
* Capture may begin under the documented provisional device-scoped priming owner and migrate onto the first truthful `AcquisitionSession`.
* Same-signature winner reuse seeds later candidate order but does not skip bounded reevaluation.
* Explicit provider/Core first-use still-only priming remains distinct from same-signature seed reuse and must preserve truthful provider/native lifetime.

## Reopened implementation findings

Current source and Scene 568 logs indicate the following implementation concerns. These are the subjects of the planned reconciliation and repair; they are not permission to redefine the design.

### Capture parent and evidence integrity

* Completed capture decisions can contain candidate measurements attributed to several successive real `AcquisitionSession` IDs.
* The documented provisional-to-first-real migration is valid; evidence preservation or minimum-merging between different real session parents is not established by the design.
* SyntheticProvider appears to have native priming-reference support capable of holding a truthful session, so the exact Core/provider parent-lifetime failure must be traced before changing the provider seam.

### Calibration scheduling and settle authority

* Core already carries bounded candidate settle state and timer support, including capture `current_candidate_ready_after_ns` and pending-observation processing.
* Godot-side retained-result calibration also maintains an independent first-seen settle timer.
* `CamBANGServer::_calibrate_live_retained_result_access_()` currently discovers work by scanning live streams, acquisition sessions, and rigs on ordinary Godot process ticks.
* Real materialization is bounded by completed identities, but discovery remains permanently frame-driven rather than armed and retired from the applied-posture lifecycle.
* The repair should reunify the existing lifecycle around Core/applied-posture settle truth and existing scheduler support. Do not invent a generic task queue, registry, benchmark subsystem, or public calibration surface.

### Getter-side calibration

* Capture result lookup currently calls calibration with a null runtime, bypassing the normal settle delay and potentially performing `to_image`-equivalent work during `get_result()` retrieval.
* This appears to be an implementation workaround for unreliable proactive progress, not the intended user-facing lifecycle.
* Do not remove it in isolation before the automatic bounded lifecycle is proven to progress without it.

### Capture result completeness

* Lower-level calibration may visit all capture members, while Backing Plan evaluator reporting currently accepts only `DEFAULT_METERED` member 0.
* This does not yet satisfy the documented whole-required-result objective for bracketed capture.

### Verification and reporting

* Scene 568 currently proves that the recorded scorer chooses the lowest accepted logged score, but does not yet prove the repaired lifecycle.
* Most stream phases deliberately encounter the live-display-demand family guard; complete comparison and guard behaviour must remain separate verification concerns.
* Final reopened capture evaluation, whole-result bracket coverage, and getter-independence require stronger proof.
* Capture reports need readiness, materialization, and total timing components to diagnose anomalous measurements.
* Windows Compatibility unsupported detection and Android expected-failure collection have narrow harness/runner defects.

## Preserved implementation seams

Do not replace working project-specific seams without concrete source evidence that they cannot support the repair:

* the three Posture Shapes: `CPU-primary`, `GPU-primary, no CPU sidecar`, and `GPU-primary, with CPU sidecar`;
* Requested Plan versus Steady Plan;
* provider capability truth and requested-plan validation;
* real public-operation helper paths shared by internal calibration;
* access-posture identity and invalidation;
* Core timer/scheduler support for bounded settle work;
* provisional capture priming, truthful first-use parent priming, and same-signature preference seeding;
* public result-access evidence recording as additional real evidence, without making public access the normal calibration heartbeat;
* the settled retained-result classification model:
  * `UNSUPPORTED` is structural support/availability truth;
  * `READY` is structural direct-target-representation availability truth;
  * supported non-ready operations may be evidence-refined between `CHEAP` and `EXPENSIVE`;
  * single-candidate status alone does not promote or demote that classification.

Synthetic maintainer output-form and parent-context downgrade controls remain established verification tooling and are outside the repair unless a concrete defect requires a narrow change.

## Non-negotiable user and GDScript behaviour

Backing Plan measurement and retained-result access classification impart no calibration-management burden on normal users or GDScript.

Normal code must not be required to:

* call `get_display_view()`, `to_image()`, or `to_image_member()` to start or advance calibration;
* repeatedly call `get_result()` to make evaluation progress;
* poll or acknowledge a calibration object;
* select candidates or supply timing information;
* manage calibration lifecycle state.

Scene 568 observes and verifies this automatic lifecycle. Except in a separately identified phase deliberately testing genuine public display demand, it must not use public materialization/display calls as calibration stimuli.

The friendly result API remains unchanged:

* `CamBANGDevice.trigger_capture()`;
* normal capture-completion observation;
* `CamBANGDevice.get_result()`;
* `CamBANGRig.get_result()`;
* `CamBANGStream.get_result()`.

Do not add public calibration API, user-facing settings, signals, required waits, or maintainer actions needed for ordinary runtime correctness.

## Planned Codex sequence

Use a fresh Codex conversation for each accepted tranche. Do not ask Codex to commit.

1. **Read-only implementation reconciliation**
  * Trace capture parent migration/lifetime, settle authorities, automatic calibration dispatch, getter-side forcing, and capture-member aggregation.
  * Distinguish confirmed defects from surprising but intended behaviour.
  * Identify the smallest repair boundaries supported by current source.

2. **Capture parent/evidence integrity**
  * Preserve provisional-to-first-real migration, truthful priming, and same-signature preference seeding.
  * Prevent evidence migration or merging between different real `AcquisitionSession` parents.
  * Reject stale observations after real-parent loss.

3. **Bounded proactive calibration lifecycle**
  * Reunify settle authority around existing Core/applied-posture lifecycle and scheduler support.
  * Keep automatic single- and multi-candidate calibration.
  * Retain the real Godot operation helpers.
  * Replace permanent all-result discovery and the independent Godot settle authority with the narrowest existing-mechanism solution established by the audit.

4. **Getter cleanup**
  * Remove getter-forced calibration only after the automatic lifecycle is proven to progress independently.
  * Prove that result retrieval neither performs hidden materialization nor changes evaluator state.

5. **Whole-required-result capture verification**
  * Complete bracket-member evidence using the documented capture objective.
  * Strengthen Scene 568 reporting and lifecycle assertions.
  * Keep complete comparison and deliberate live-display-demand tests separate.

6. **Documentation reconciliation**
  * Update this file after each accepted/committed tranche.
  * Update canonical or maintainer documentation only to reflect accepted source truth.

Do not introduce new scoring robustness machinery until parent attribution, settle timing, automatic dispatch, and result completeness are trustworthy. Reassess single-sample scoring only from repaired evidence.

## Current verification interpretation

Previously reported deterministic and Godot PASS results remain useful regression baselines, but they do not prove the reopened lifecycle correct.

In particular, a Scene 568 PASS currently means that the harness completed and the scorer was internally consistent with accepted logged evidence. It does not yet prove:

* one real capture parent across a completed measured epoch;
* one authoritative settle lifecycle;
* absence of getter-driven calibration;
* bounded non-per-frame automatic dispatch;
* whole-required-result bracket scoring; or
* stable decisions from uncontaminated evidence.

Manual local validation remains authoritative for Windows, Godot, GPU, Android, hardware, and platform-provider behaviour. Do not weaken tests or scenes to obtain PASS.

## Terminology and scope guardrails

* Treat `best posture` and chooser policy `intent` states such as `Default` and `Stream-active` as defunct for new design work.
* Prefer `Native Payload Support Parent`, `Backing Plan`, `Backing State`, `Operation Support`, `Access Evidence`, `Requested Plan`, `Steady Plan`, and `Posture Shape`.
* Keep support/availability, retained truth, materialization choice, and cost classification distinct.
* Keep changes narrow and source-grounded.
* Avoid unrelated cleanup.
* Preserve the locked Godot public API and provider seams.
* Avoid new abstraction layers, registries, route descriptors, diagnostics, or settings unless a concrete need survives the read-only reconciliation.

## Expected agent behaviour for this tranche

For read-only work:

* inspect source before making claims;
* cite files, functions, and relevant line ranges;
* separate confirmed facts from hypotheses;
* identify existing validation surfaces;
* do not prepare patches or detailed replacement architecture unless a concrete source finding requires it.

For implementation work:

* state expected changed files before editing;
* keep each tranche narrow;
* preserve automatic behind-the-scenes behaviour;
* report changed files and rationale;
* list validation commands run;
* say plainly what was not run;
* do not create commits unless explicitly asked.
