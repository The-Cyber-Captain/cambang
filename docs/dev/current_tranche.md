# Current Tranche

This document records active development context for CamBANG. It is intentionally more volatile than `AGENTS.md` and `docs/dev/agent_context.md`.

Agents should use this file for current workstream direction, recent decisions, and near-term constraints. Durable engineering rules belong in `AGENTS.md` or `docs/dev/agent_context.md`.

## Current focus

Continue source-grounded CamBANG development without broadening scope or weakening verification.

The current work is centred on result/backing truth, provider seams, and performance-sensitive routing decisions, especially where SyntheticProvider behaviour informs but must not constrain future platform-backed providers.

Active narrow slice: implement the internal retained-result access calibration seam for the currently active concrete production posture, while future-proofing that seam for a near-following higher-level candidate-posture evaluator.

Important active concerns:

* use truthful Synthetic producer output-form / retained-plan control as the reference seam for Synthetic-produced result backing/access behaviour, across repeating-stream and still-capture paths where that control is intended to apply;
* preserve provider-local staging vs Core-retained result truth;
* avoid silently falling back between CPU and GPU modes when a selected mode should be authoritative;
* avoid treating SyntheticProvider-only optimisations as representative of Windows MF, Android Camera2, or future platform-backed providers;
* protect synchronous Core/provider boundaries that exist for thread safety, snapshot truth, or deterministic sequencing;
* begin platform-backed provider preparation from the provider contract and retained-result model, not from Synthetic implementation shortcuts;
* keep Synthetic retained GPU backing materialization scoped and named accurately: it is bridge-backed Synthetic materialization, not generic platform GPU/RD readback;
* treat retained-result access timing evidence as classification input gathered at the real result-access seam;
* renew/invalidate that evidence from live applied production-posture acceptance/application boundaries rather than from first user-visible `to_image()` demand;
* do not treat per-frame retained-form fluctuation inside one live posture as a posture change or as a reason to renew calibration;
* keep calibration non-stream-only: the intended internal seam covers release-facing retained-result access across both Stream Result and Capture Result surfaces, including capture image-member access where relevant;
* keep current Scene 70 / Godot-side reporting and Synthetic dev metrics understood as verification/reporting surfaces, not the final architecture seam;
* future-proof the calibration work so the same measurement primitive can later be reused by a higher-level evaluator that compares multiple viable concrete postures against intent, without implementing that outer evaluator in this tranche.

Synthetic maintainer tooling direction:

* Synthetic provider backing advertisement reports current runtime output-form truth;
* retained-plan policy chooses primary and auxiliary retention within that truthful set;
* Synthetic maintainer output-form selection is exposed through the project setting `cambang/maintainer/synthetic_producer_output_form=runtime_default|cpu_only|cpu_gpu|gpu_only`; `runtime_default` is the explicit no-forcing/default value that preserves the normal Synthetic runtime policy; host command-line runs may pass `--cambang-synth-producer-output-form=...`, which feeds that same project-setting authority path before provider construction; there is no environment-variable fallback for this selection;
* the selection is Synthetic-only and drives both truthful output-form reporting and actual retained/produced behaviour for repeating-stream and still-capture Synthetic outputs where those backing seams exist;
* `gpu_only` selections fail deterministically when the Synthetic GPU runtime cannot realize them; `cpu_gpu` selects the allowed CPU/GPU set and is truthfully narrowed by provider/runtime realizability, collapsing to CPU-backed behaviour when GPU backing is unrealizable;
* do not add controls that misstate provider output-form truth, harness selectors, public API, or platform-backed-provider equivalents while preparing this base.

## Immediate implementation direction

The immediate implementation work is the inner calibration mechanism, not the outer posture chooser.

Implement the smallest justified internal seam that:

* calibrates the real retained-result access/materialization paths of the currently active concrete production posture;
* measures at the true result-operation seam rather than at provider-local generation/staging, snapshot publication, later render-thread draw/UI scheduling, or unrelated GPU upload/update work;
* covers both stream and capture result surfaces, including capture image-member materialization where relevant;
* keys evidence by concrete posture identity and operation/access path so that later cross-posture comparison can reuse the same measurement primitive;
* renews/invalidates evidence from live posture acceptance/application boundaries and lifecycle end, not from first user demand and not from per-frame retained-form fluctuation inside one posture;
* refines cost classification only where calibration belongs, preserving support/availability truth and retained direct-availability truth as separate concerns.

This tranche should not yet:

* implement the higher-level candidate-posture evaluator;
* iterate all viable candidate postures to choose among them;
* introduce a broad registry/taxonomy/cache architecture unless the implementation proves that the smaller seam is insufficient;
* create a new public API burden;
* let current Synthetic stream display-demand/update behaviour become the generalized model for platform-backed providers.

The expected near-following direction is:

1. keep this tranche focused on per-posture calibration of real retained-result access paths;
2. leave behind a reusable posture-keyed measurement primitive;
3. later add a higher-level evaluator that can enumerate viable concrete candidate postures, invoke the same calibration primitive for each candidate as needed, compare the resulting access-cost picture against intent, and choose a preferred posture.

Persistent concern area after this tranche remains Android CPU-backed / compatibility-style repeating-stream pressure. The immediate calibration work should be shaped so that later intent-aware output-form selection can use its evidence as one optimisation avenue, but that outer policy/evaluator work is not part of this tranche.

## Recent committed checkpoint

The most recent committed checkpoint added and validated the first narrow Synthetic retained GPU backing materialization path for stream results.

Checkpoint summary:

* `GPU_SURFACE` stream results with retained Synthetic GPU backing can now advertise `to_image()` support when the retained backing explicitly reports materialization availability.
* GPU-only stream `to_image()` remains `UNSUPPORTED` when no materializer is available.
* GPU-only stream `to_image()` is classified as `EXPENSIVE` only when the Synthetic retained backing materializer is genuinely available.
* GPU-primary stream results with a retained current CPU sidecar still prefer the CPU sidecar for `to_image()` and remain `CHEAP`.
* CPU-packed stream results remain `CHEAP`.
* The materializer is scoped to Synthetic retained GPU backing materialization through the Godot/Synthetic bridge. It is not generic platform GPU/RD readback.
* Core consumes neutral retained-backing truth and does not own Godot/RD/Image details.
* The Godot display/materialization adapter owns Godot-facing materialization.
* The Synthetic runtime wrapper owns the provider/runtime-neutral materialization-availability query.
* The ODR/linkage repair established single ownership for `synthetic_gpu_backing_can_materialize_to_image(...)` in `src/imaging/synthetic/gpu_backing_runtime.*`.

Validation reported green after the final ODR/linkage repair:

* Windows GDE/SCons build and link completed.
* Core smoke/verifier tools passed, including `core_result_path_smoke`, `provider_compliance_verify`, `synthetic_timeline_verify`, `restart_boundary_verify`, and `verify_case_runner --run-all --repeat=50`.
* `windows_mf_runtime_validate --real-hardware` passed.
* Godot suite passed with `51 passed, 0 failed, 1 review`.
* Scene 70 result retrieval verification passed across the tested Compatibility/Mobile and Android/Windows combinations.

Validation notes:

* The `Review: 1` suite result is acceptable only while it remains the known Scene 65 public-boundary review case.
* The new focused result-access coverage is inside `core_result_path_smoke`; no new standalone executable or Godot scene was reported.
* Local GPU/runtime validation exercised retained GPU display backing on Mobile renderer, but this checkpoint still does not claim platform-provider GPU readback support.


## Current design posture

Prefer the “avoid this unless” default:

* avoid new public API concepts unless clearly earned;
* avoid new abstraction layers unless they simplify or protect real design boundaries;
* avoid route/outcome descriptors unless there is a concrete consumer;
* avoid diagnostics that become permanent knobs for temporary investigations;
* avoid broad refactors during narrow correctness or performance tranches.

The desired codebase direction is logical, efficient, streamlined, and maintainable. UX simplicity depends on source-level design simplicity.

## API and UX constraints

The Godot public API is locked for this tranche unless explicitly reopened.

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
* future providers may support genuine platform bursts, provider-managed exposure sequences, or different backing behaviour.
* do not extrapolate the Synthetic bridge-retained upload-byte materializer to Android Camera2, Windows MF, or future native GPU-surface providers without a provider-specific retained-backing/materialization design;

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
