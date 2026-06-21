# Current Tranche

This document records active development context for CamBANG. It is intentionally more volatile than `AGENTS.md` and `docs/dev/agent_context.md`.

Agents should use this file for current workstream direction, recent decisions, and near-term constraints. Durable engineering rules belong in `AGENTS.md` or `docs/dev/agent_context.md`.

## Current focus

Continue source-grounded CamBANG development without broadening scope or weakening verification.

The retained-result access calibration/classification tranche is implemented and remains accepted.

The currently landed intent-based chooser implementation is partial rather than accepted. Useful groundwork already exists in source: the three posture shapes, requested-plan plumbing through stream/capture/provider-delivered results, provider-capability distinctions for those shapes, the requested-vs-steady storage seam, the result-access evidence seam, and the access-posture/topology-triggered invalidation work.

The immediate focus is no longer to complete the old chooser model as previously described in this file. The terminology reset and parent-scoped design note are now landed, and the active direction is to implement that reset in source without allowing the older intent-based chooser model to drift forward in parallel.

Important settled state:

* Scene 68 calibration/evidence harness repair and validation are complete.
* The boundedness/posture-identity fix is complete: calibration is keyed to live applied production posture/access identity rather than per-frame retained-form fluctuation or first user demand.
* The evidence-to-classification seam is implemented for retained-result access.
* Public `get_display_view()`, `to_image()`, and `to_image_member()` access remains instrumented, but those calls are not the normal recalibration heartbeat.
* The endpoint-handle vs runtime-instance distinction is documented in the Godot boundary contract.
* Scene 70 / Godot-side reporting and Synthetic dev metrics remain verification/reporting surfaces, not the authoritative architecture seam.

Settled retained-result classification model:

* `UNSUPPORTED` is structural support/availability truth.
* `READY` is structural, operation-specific direct retained target-representation availability truth.
* Supported non-ready paths start from provisional retained access truth and may be evidence-refined between `CHEAP` and `EXPENSIVE`.
* Single-candidate supported non-ready paths retain their provisional non-ready classification after calibration rather than being auto-promoted/demoted merely for being alone.

Synthetic maintainer tooling direction remains:

* Synthetic provider backing advertisement reports the output forms available from the current runtime.
* Retained-plan policy chooses primary and auxiliary retention within that truthful set.
* Synthetic maintainer output-form selection is exposed through the project setting `cambang/maintainer/synthetic_producer_output_form=runtime_default|cpu_only|cpu_gpu|gpu_only`; `runtime_default` is the explicit no-forcing/default value that preserves the normal Synthetic runtime policy; host command-line runs may pass `--cambang-synth-producer-output-form=...`, which feeds that same project-setting authority path before provider construction; there is no environment-variable fallback for this selection.
* The selection is Synthetic-only and drives both truthful output-form reporting and actual retained/produced behaviour for repeating-stream and still-capture Synthetic outputs where those backing seams exist.
* `gpu_only` selections fail deterministically when the Synthetic GPU runtime cannot realize them; `cpu_gpu` selects the allowed CPU/GPU set and is truthfully narrowed by provider/runtime realizability, collapsing to CPU-backed behaviour when GPU backing is unrealizable.
* Verification-only parent-context downgrades are exposed through `cambang/maintainer/synthetic_stream_capability_downgrades` and `cambang/maintainer/synthetic_capture_capability_downgrades`; host command-line runs may pass `--cambang-synth-stream-capability-downgrades=...` and `--cambang-synth-capture-capability-downgrades=...`, which feed those same project-setting authority paths before provider construction.
* These downgrade controls are Synthetic-only, maintainer-only, internal, and downgrade-only: they may narrow an effectively mixed parent context to `cpu_only`, remain inert when the truthful outer envelope is already `cpu_only`, and are rejected when contradictory to a `gpu_only` outer envelope.
* Do not add controls that misstate provider output-form truth, harness selectors, public API, or platform-backed-provider equivalents while preparing the next stage.

## Immediate implementation direction

Treat the landed chooser work as a partial and now superseded implementation direction, not as accepted architecture and not as the basis for new terminology growth.

What already exists and should be preserved unless source inspection disproves it:

* posture shapes are `CPU-primary`, `GPU-primary, no CPU sidecar`, and `GPU-primary, with CPU sidecar`;
* sidecar retention is optional and must earn its keep;
* requested retained-plan carriage already exists through Core requests and provider-delivered results;
* provider capability truth already distinguishes the viable settled posture shapes;
* the project already has a real result-access measurement seam and access-posture invalidation seam that should be reused rather than replaced with per-frame polling or a broad benchmark subsystem.

Immediate tranche goal:

* preserve the landed terminology reset and `docs/dev/backing_plan_parent_evaluation_reset.md` as the active design direction;
* redefine retained-plan selection around the Native Payload Support parent rather than the old chooser-intent model;
* preserve the bounded, topology-triggered, non-per-frame nature of evaluation and reevaluation;
* preserve provider-truth, requested-vs-steady separation, and `CoreResultStore` validation against Core-held requested plan state.

Terminology guardrail for this reset:

* treat `best posture` as defunct wording;
* treat chooser `intent` states such as `Default` and `Stream-active` as defunct policy vocabulary for new design work;
* do not extend the old `retained backing plan` / `retained backing truth` / `retained access truth` family in new design writing when the agreed replacement vocabulary is available;
* prefer the reset vocabulary in new design work: `Native Payload Support Parent`, `Backing Plan`, `Backing State`, `Operation Support`, `Access Evidence`, `Requested Plan`, `Steady Plan`, and `Posture Shape`;
* use `parent context` only as a fixed qualifier, not as a loose substitute for the defined owner concept.

Current checkpoint status:

* deterministic native verification passed:
  * `core_result_path_smoke` PASS;
  * `provider_compliance_verify` PASS;
  * `synthetic_only_provider_support_verify` PASS.
* Scene 70 `runtime_default` verification currently passes on the tested Windows/Android and Compatibility/Mobile combinations.
* Current Scene 70 evidence suggests:
  * Compatibility `runtime_default` resolves to CPU-primary;
  * Mobile `runtime_default` resolves to GPU-primary, no CPU sidecar.
* Current forced `cpu_only` / `gpu_only` comparisons support that inference.
* Deterministic maintainer coverage includes the Synthetic parent-context downgrade matrix and the targeted retained-plan evaluator check.
* The targeted retained-plan evaluator check now exercises parent-scoped stream `display_view` evaluation and independent capture-parent evaluation in `provider_compliance_verify`.
* Scene 68 still remains a verification/reporting surface, but any remaining assertions of capture-policy mirroring or chooser-intent semantics are harness drift rather than accepted architecture.

Immediate next focus:

* do not assume the chooser tranche is accepted yet;
* do not extend the old chooser terminology or intent model in code comments, tranche notes, or new design material;
* continue replacing the older chooser-intent implementation and harness assumptions with the parent-scoped Backing Plan evaluation model;
* keep Scene 68 aligned only as a verification/reporting surface unless and until it clearly proves the redesigned evaluator without reintroducing old policy semantics;
* desired harness output for the redesigned evaluator is explicit parent-scoped decision truth such as viable posture set, requested plan, steady plan, chosen posture shape, and decision-relevant evidence buckets.

Android CPU-backed / compatibility-style repeating-stream pressure remains an important motivating use-case for this validation/harness work. It continues to inform the expected value of the chooser, but does not by itself prove the tranche successful.
## Recent committed checkpoint

The most recent implemented checkpoint completed the retained-result access calibration/classification seam after the earlier Synthetic retained GPU backing materialization work.

Checkpoint summary:

* Scene 68 calibration/evidence harness repair and validation completed.
* Calibration boundedness and posture identity were corrected so evidence renewal follows live applied production-posture boundaries.
* The real retained-result operation seam now supplies measured evidence for live retained artifacts/postures.
* Refined result-facing classification now exists beside provisional retained access truth.
* `UNSUPPORTED` and `READY` remain structural truth; measured evidence only refines supported non-ready paths.
* Public result access remains instrumented for evidence, but first user-visible access is not the recalibration heartbeat.
* The endpoint-handle vs runtime-instance distinction was documented for Godot-facing object access.

Previous retained GPU backing checkpoint facts still apply:

* `GPU_SURFACE` stream results with retained Synthetic GPU backing can advertise `to_image()` support when the retained backing explicitly reports materialization availability.
* GPU-only stream `to_image()` remains `UNSUPPORTED` when no materializer is available.
* GPU-only stream `to_image()` is classified as `EXPENSIVE` only when the Synthetic retained backing materializer is genuinely available.
* GPU-primary stream results with a retained current CPU sidecar still prefer the CPU sidecar for `to_image()` and remain `CHEAP`.
* CPU-packed stream results remain `CHEAP`.
* The materializer is scoped to Synthetic retained GPU backing materialization through the Godot/Synthetic bridge. It is not generic platform GPU/RD readback.
* Core consumes neutral retained-backing truth and does not own Godot/RD/Image details.
* The Godot display/materialization adapter owns Godot-facing materialization.
* The Synthetic runtime wrapper owns the provider/runtime-neutral materialization-availability query.

Validation notes:

* Manual local validation remains authoritative for Windows, Godot, hardware, GPU, and platform-provider behaviour.
* Scene 68/Synthetic metrics are verification/reporting surfaces; retained-result classification architecture is documented in `docs/architecture/pixel_payload_and_result_contract.md`.
* Post-documentation local validation for the retained-result access calibration/classification checkpoint reported:
    * `core_result_path_smoke` PASS;
    * Godot suite `51 passed, 0 failed, 1 review`, with the review acceptable only while it remains the known Scene 65 public-boundary review case;
    * representative Scene 68 retained-result evidence/reset runs exercised on CPU-backed and GPU-backed/runtime-default paths;
    * Scene 70 result retrieval verification remained passing.
    * 
## Current design posture

Prefer the “avoid this unless” default:

* avoid new public API concepts unless clearly earned;
* avoid new abstraction layers unless they simplify or protect real design boundaries;
* avoid route/outcome descriptors unless there is a concrete consumer;
* avoid diagnostics that become permanent knobs for temporary investigations;
* avoid broad refactors during narrow correctness or performance tranches.

The desired codebase direction is logical, efficient, streamlined, and maintainable. UX simplicity depends on source-level design simplicity.

## API and UX constraints

The Godot public API is locked unless explicitly reopened.

Normal users should interact with friendly object-level API shapes. Avoid making normal users handle capture IDs, route internals, retained-backing implementation details, or diagnostic-only concepts.

Preferred result API direction:

* `CamBANGDevice.trigger_capture()`;
* wait/check capture completion through the existing public flow;
* `CamBANGDevice.get_result()` for the device’s current completed result;
* `CamBANGRig.get_result()` for a curated result set;
* `CamBANGStream.get_result()` for current observable stream state.

Avoid “latest” in public method names. Keep ID-based lookup on server/dev tooling paths rather than normal user paths.

## Provider and performance cautions

SyntheticProvider must remain deterministic and useful across builds, but Synthetic-specific performance work must not distort platform-provider architecture.

For future platform-backed providers:

* Android Camera2 is the first release target;
* Windows Media Foundation remains a development accelerator;
* future providers may support genuine platform bursts, provider-managed exposure sequences, or different backing behaviour;
* do not extrapolate the Synthetic bridge-retained upload-byte materializer to Android Camera2, Windows MF, or future native GPU-surface providers without a provider-specific retained-backing/materialization design.

When assessing performance:

* gather source-grounded evidence before proposing architectural changes;
* distinguish CPU image generation overhead from Core sequencing overhead;
* do not assume stream responsiveness and capture sluggishness share the same root cause;
* do not remove synchronous boundaries unless their safety purpose has been understood and preserved.

## Validation posture

Manual local validation remains authoritative.

Agents may suggest validation commands and may run available sandbox checks, but must not claim validation of paths that were not actually exercised, especially:

* Windows MF runtime behaviour;
* Godot editor/runtime scenes;
* GPU display-view behaviour;
* hardware camera behaviour;
* Android Camera2 behaviour;
* local build environment behaviour.

Do not weaken tests or scenes merely to get PASS.

If a test must change because the design changed, preserve or increase the strength of verification and explain the design reason.

## Expected agent behaviour for this tranche

For read-only work:

* inspect source before making claims;
* separate confirmed facts from hypotheses;
* cite files and functions inspected;
* identify likely validation points;
* do not prepare patches unless a concrete issue is found.

For implementation work:

* keep changes narrow;
* avoid unrelated cleanup;
* preserve public API unless explicitly instructed;
* preserve provider seams;
* report changed files and rationale;
* list validation commands;
* say plainly what was not run.

For Codex Web:

* assume cloud validation cannot prove local Windows/Godot/hardware/GPU paths;
* do not claim local validation unless it actually happened;
* do not create commits unless explicitly asked;
* do not add persistent environment variables or diagnostic knobs without approval.
