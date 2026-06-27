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

## Completed tranche

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

## Do not reopen in the next tranche

Do not reopen these unless new source evidence disproves them:

* Synthetic refcount semantics;
* `ProviderBroker` forwarding;
* `CapturePriming -> first truthful real AcquisitionSession` migration;
* cross-real-session evidence rehome/merge prohibition;
* same-signature seed-only reuse;
* Scene 568 terminal rollover semantics; and
* current Backing Plan scoring.

## Next tranche

### Retained-result access calibration lifecycle

The first step of this tranche is a read-only audit before any implementation.

That audit should map:

* duplicate settle authority;
* Core candidate settle scheduling;
* Godot/helper first-seen timing;
* permanent Godot-side live-result calibration sweep;
* getter-triggered calibration fallback;
* public result access helper routes shared with internal calibration;
* stream display-view evidence;
* stream `to_image` evidence;
* capture readiness evidence; and
* capture materialization evidence.

Intended direction, conservatively:

* retain real result-operation seams for evidence gathering;
* move renewal/proactive lifecycle toward the live applied Backing Plan/posture boundary;
* avoid permanent per-frame calibration polling as the architecture;
* do not introduce a generic benchmark, queue, registry, or public API; and
* remove getter-side fallback only after automatic lifecycle is proven complete.

Deferred items:

* Compatibility `gpu_only` expected-negative Scene 568/runner cleanup;
* bracket whole-result scoring; and
* score tuning.
