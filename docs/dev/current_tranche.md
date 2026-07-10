# Current Tranche

## Active task

Docs-first lock of the `ImagingSpec` seam.

## Goal

Produce the minimum documentation updates needed to define `ImagingSpec` as the cross-camera / imaging-subsystem capability seam used for current operational truth that Core may need for admission and validation.

## Expected work

Primary deliverable:

* one new architecture note for the `ImagingSpec` seam

Only minimal consistency edits elsewhere if required.

Likely touched docs:

* new `docs/architecture/` note
* `docs/naming.md`
* `docs/provider_architecture.md`
* `docs/state_snapshot.md`

## Guardrails

Do not:

* change code
* change public Godot API
* broaden the task into result-fact redesign
* broaden the task into general metadata/calibration enrichment
* broaden the task into platform-provider implementation
* turn this file into a changelog, history note, or future-work list

Keep the change narrow, source-grounded, and reviewable.

## Done means

This tranche is complete when the docs clearly lock the `ImagingSpec` seam and the touched wording is internally consistent.
