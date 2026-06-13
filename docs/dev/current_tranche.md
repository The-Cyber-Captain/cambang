# Current Tranche

This document records active development context for CamBANG. It is intentionally more volatile than `AGENTS.md` and `docs/dev/agent_context.md`.

Agents should use this file for current workstream direction, recent decisions, and near-term constraints. Durable engineering rules belong in `AGENTS.md` or `docs/dev/agent_context.md`.

## Current focus

Continue source-grounded CamBANG development without broadening scope or weakening verification.

The current work is centred on result/backing truth, provider seams, and performance-sensitive routing decisions, especially where SyntheticProvider behaviour informs but must not constrain future platform-backed providers.

Important active concerns:

* preserve truthful retained CPU/GPU access reporting;
* keep provider-local staging separate from Core-retained result truth;
* avoid silently falling back between CPU and GPU modes when a selected mode should be authoritative;
* avoid treating SyntheticProvider-only optimisations as representative of Windows MF, Android Camera2, or future platform-backed providers;
* protect synchronous Core/provider boundaries that exist for thread safety, snapshot truth, or deterministic sequencing;
* look for genuine performance opportunities without misdiagnosing intentional synchronous sequencing as accidental overhead.

## Recent completed work

The most recent completed tranche added and validated:

* internal Synthetic stream backing mode selection with `Auto`, `CpuOnly`, `GpuOnly`, and `CpuAndGpu`;
* provider-to-Core CPU sidecar retention intent on `FrameView`;
* Core retained backing planning that retains CPU sidecar data only when explicitly marked for retention;
* GPU-mode Synthetic stream production that does not silently fall back to CPU;
* refusal of unsupported GPU-only modes when runtime GPU backing is unavailable;
* smoke coverage for CPU, GPU-only without retained CPU sidecar, and GPU-primary with retained CPU sidecar;
* provider compliance coverage for frame backing facts and GPU-mode production truth.

Last reported validation status for that work: green.

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
