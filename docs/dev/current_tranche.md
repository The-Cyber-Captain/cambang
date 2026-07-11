# Current Tranche

## Active task

Confirm the grouped-rig preflight/admission/submission/orchestration shaping line is complete, and remove only any remaining tiny local inconsistency inside that immediate cluster if one truly remains.

## Goal

Keep the accepted authoritative `ImagingSpec` gate, shared grouped-rig orchestration helper, centralized orchestration failure mapping, and constructor-based shaping across preflight, admission, submission, and orchestration exactly as they now exist, and perform one final narrow completion pass over that full grouped-rig cluster.

## Expected work

* keep cohort admission as the authoritative grouped-rig `ImagingSpec` gate
* keep the shared grouped-rig orchestration helper as the single orchestration path
* keep centralized orchestration failure mapping as the top-level truth surface
* keep constructor-based shaping across grouped-rig preflight, admission, submission, and orchestration
* inspect that immediate grouped-rig cluster for any remaining tiny local inconsistency or avoidable duplication
* make only minimal source-grounded touch-ups if something real remains
* otherwise leave code unchanged and report the grouped-rig shaping line complete
* add or adjust focused verification only if needed

Likely touched areas:

* `src/core/` grouped-rig preflight / admission / submission / orchestration helpers
* focused maintainer verification where needed

Only minimal documentation touch-up if source-grounded and necessary.

## Guardrails

Do not:

* change public Godot API
* implement platform providers
* broaden into a general `ImagingSpec` schema system
* add unrelated new `ImagingSpec` consumers
* move the authoritative `ImagingSpec` gate away from cohort admission
* reintroduce duplicate later-stage `ImagingSpec` enforcement
* broaden into result-fact redesign
* broaden into calibration/metadata enrichment
* broaden into large admission-policy redesign
* turn this file into a changelog, history note, or future-work list

Keep the change narrow, source-grounded, and reviewable.

## Done means

This tranche is complete when:

* grouped-rig preflight, admission, submission, and orchestration all use the same consistent constructor-based shaping pattern
* no remaining real tiny local inconsistency or avoidable duplication remains in that immediate grouped-rig cluster
* grouped-rig behavior remains unchanged
* focused verification still proves deterministic failure propagation
* no public API changes are introduced
