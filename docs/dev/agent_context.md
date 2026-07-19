# Agent Context

This document gives repo-specific context for AI coding agents working on CamBANG. It supports the root `AGENTS.md` by recording durable project expectations that are too specific for the root file but too stable to repeat in every prompt.

For active tranche work, `docs/dev/current_tranche.md` may provide additional current steering. Keep tranche-specific detail there, not here.

## Source authority

The checked-out repository is authoritative.

Do not infer current code state from previous chats, summaries, memories, or stale documentation when source inspection is possible. Inspect the relevant files before proposing or making changes.

When documentation and source disagree:

1. Treat source and tests as the first authority.
2. Treat documentation as possibly stale.
3. Report the mismatch instead of silently choosing one.

## Project shape

CamBANG is a deterministic imaging core with a Godot-facing API.

The system is organised around:

* a Core runtime responsible for lifecycle, ownership, publication, and result truth;
* providers that supply platform-backed or synthetic imaging behaviour;
* Godot-facing wrappers and scenes that expose and verify the public API;
* smoke, scenario, compliance, and runtime validation tools.

Core design priorities:

* truthful state publication;
* deterministic lifecycle and teardown;
* thread-safe ownership boundaries;
* simple user-facing API;
* provider/platform seams that can support Synthetic, Windows WinRT, Android Camera2, and future providers.

Avoid adding concepts, layers, public API names, diagnostics, or abstractions unless they clearly earn their keep.

## Public API caution

The Godot public API is considered locked unless a task explicitly says otherwise.

Do not rename or reshape public Godot methods, signals, constants, or expected scene behaviours merely to make an implementation easier.

Normal users should not need capture IDs, internal route descriptors, or debugging concepts. Keep advanced and developer tooling separate from the friendly object-level API.

## Snapshot and lifecycle expectations

Snapshot truth matters more than cosmetic simplicity.

Important expectations:

* published snapshots should reflect retained truth, not provider-local staging details;
* lifecycle phases must not be faked to satisfy tests;
* NIL, baseline, restart, and teardown behaviour must remain deterministic;
* destroyed, retained, and live objects must remain distinguishable when that distinction is meaningful;
* teardown should avoid blocking render-sensitive or Godot-sensitive paths.

When changing snapshot, result, or lifecycle logic, consider effects on:

* CoreRuntime publication;
* native object registry/state;
* provider compliance;
* Godot wrappers;
* status panel/editor visibility;
* scenario verification;
* restart-boundary verification.

## Provider boundaries

Provider seams are important.

SyntheticProvider is deterministic and cross-platform. Platform-backed providers should not be constrained by Synthetic-only implementation shortcuts unless the design explicitly justifies it.

Do not assume a SyntheticProvider optimisation represents Windows WinRT, Android Camera2, or future platform-backed behaviour.

When working near provider code, preserve the distinction between:

* provider-local staging data;
* retained Core-visible payload or backing truth;
* CPU payload availability;
* GPU/display backing availability;
* advertised capability;
* actual retained result access.

## Specifications, runtime posture, and result facts

Keep these surfaces distinct:

* `CameraSpec` and `ImagingSpec` are retained specification/capability truth.
* `camera_state` is current runtime posture, including target/applied/constrained/rejected state.
* result fact groups describe actual image-time facts where available.
* post-result metadata enrichment or user-side calibration data is not automatically core operational truth.

Cross-camera or imaging-subsystem capability truth belongs to `ImagingSpec`. Per-camera stable characteristics belong to `CameraSpec`.

## Camera-fact model

CamBANG uses a source-neutral internal camera-fact model. Do not shape internal
Core/provider records around ADC JSON, Godot `Dictionary` layouts, or one
platform API.

Keep provider static/device facts, provider result-specific image facts,
externally configured per-camera facts, Core-owned capture-admission context,
and realized payload/member truth as distinct authorities.

Provider result-specific image facts include truthful image acquisition timing
in the provider or backend clock domain, realized image-time focus state,
realized delivered-image transform, and optional image-specific calibration or
pose where the provider genuinely knows them. Providers must not substitute Core
capture-admission time, Capture Date-Time, geolocation or geolocation sample
time, Core capture lifecycle timing, or another image member's timing for image
acquisition timing.

`ImagingSpec` remains the retained cross-camera or imaging-subsystem operational
capability seam. It must not become a general camera-metadata, calibration, or
result-fact bucket.

CamBANG may validate, retain, resolve, and expose descriptive calibration and
camera facts. It does not perform lens calibration, rectification, projection,
intrinsic rescaling, coordinate-domain conversion, or approximate distortion
model fitting.

Godot-facing camera-fact APIs and result shapes remain explicitly gatekept. Do
not add, rename, or retire public surfaces unless the active tranche records the
approved change.

## Image acquisition timing

Image Acquisition Timing is provider-authored descriptive truth for an acquired
image or stream frame. Its numeric value is an acquisition mark in a declared
provider or backend clock domain, not necessarily a wall-clock timestamp.

Keep Image Acquisition Timing distinct from:

* Core-owned Capture Date-Time sampled at successful capture admission;
* snapshot publication time;
* Core or provider lifecycle timing;
* latency and performance measurements;
* internal frame or result identity.

Do not use acquisition marks as identity merely because they are numeric or
monotonic in one provider implementation.

Where stream and capture expose the same fact, use one source-neutral internal
model and avoid duplicate provider-to-Core transport for the same acquisition
event.


## Automatic measurement and classification

Backing Plan measurement and retained-result access classification are internal, automatic runtime responsibilities. They must not impose a calibration-management workflow on normal users or GDScript.

Durable expectations:

* arm proactive calibration from truthful live applied-posture/access identity and keep it bounded;
* do not wait for first public `get_display_view()`, `to_image()`, or `to_image_member()` demand to begin normal calibration;
* use the same real operation implementation as the public API without requiring the public call or creating public-demand/lifetime semantics;
* do not require repeated `get_result()` calls, public access calls, calibration polling, acknowledgements, candidate selection, or user-supplied timing to advance the lifecycle;
* result retrieval must not become the hidden substitute for automatic calibration progress;
* verification scenes observe and assert the automatic lifecycle; public access calls are valid stimuli only in phases explicitly testing genuine public demand;
* do not add public calibration API, GDScript controls, user-facing settings, or maintainer actions required for ordinary runtime correctness.

## Testing philosophy

Do not weaken tests, smoke tools, or Godot verification scenes merely to get PASS.

Tests may be updated when the design intentionally changes, but changes should preserve or increase the strength of verification. If a test failure reveals a real sequencing, ownership, lifecycle, or truth mismatch, report it rather than papering over it.

Manual local validation remains authoritative for:

* Windows-specific behaviour;
* Godot editor/runtime scenes;
* hardware-backed camera behaviour;
* GPU/display-view behaviour;
* platform-provider behaviour.

Cloud or sandbox validation is useful, but it does not prove those paths unless they were actually exercised.

The configured local verification matrix is Windows host execution plus Android
build/deploy/run over ADB. This machine has no Linux, WSL, or macOS environment.
Do not invent a POSIX or macOS hard completion gate unless the maintainer
explicitly supplies and authorizes such an external environment. Keep
platform-independent code portable by design and source review, but distinguish
that from runtime validation actually performed here. Platform-backed provider
verification remains specific to the platform implementation exercised.

## Build and generated files

Avoid editing generated outputs or build artefacts.

Common generated/build-output areas include:

* `out/`;
* object files and compiler outputs;
* generated compile databases unless the task explicitly concerns them;
* Godot binary artefacts;
* third-party build outputs.

Prefer source, docs, tests, and build scripts over generated files.

## Documentation expectations

Documentation should describe current design.

Avoid preserving named tombstones for deliberately removed concepts unless there is clear maintainer value. Do not keep references to things that no longer exist by design.

Keep documentation:

* source-grounded;
* concise;
* clear about stable design decisions;
* free of obsolete implementation detail;
* separated from temporary planning notes.

Agent-guidance maintenance:

* use `current_tranche.md` only for the maintainer-approved active work order
  (scope, acceptance criteria, validation expectations, near-term
  constraints); reset it to its stub once the tranche is accepted and
  committed;
* do not keep tranche-completion records, remediation-plan backlogs, or
  deferred-task lists as repository files — git history is the record of
  completed work, and future work is queued only when the maintainer
  activates it in `current_tranche.md`; put validation detail in commit
  messages, not record files;
* use `docs/dev/agent_context.md` only for durable cross-tranche expectations that should persist beyond the current workstream;
* do not duplicate canonical architecture in either guidance file.
