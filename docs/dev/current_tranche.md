# Current Tranche

## Active workstream

Complete the camera-description and still-capture fact model that follows the
already-implemented external camera-concurrency ingestion seam.

The checked-out source currently implements stopped-time transactional
`CamBANGServer.ingest_camera_concurrency(String json_text)` ingestion of the
supported ADC v1 concurrency projection into retained `ImagingSpec` truth.
Grouped-rig admission already consumes that truth. That implementation is the
source baseline, not the final camera-description model.

The broader agreed direction is recorded in the camera-fact handover supplied
for this workstream. Source inspection remains authoritative for actual code
state.

## Current tranche

Implement the internal ADC v2 camera-description parser and retained external
camera-description state after accepted Tranche 0 architecture/schema and
Tranche 1 source-neutral type work.

Keep individual invocation tasks in their Codex prompts; keep this file
focused on tranche-wide state and constraints.

## Settled tranche direction

* The camera-fact architecture and ADC v2 documentation/schema tranche is
  accepted. ADC v2+ is the target interchange contract; CamBANG is not
  required to ingest ADC v1.
* ADC v1 human and machine schemas are historical evidence of the existing
  Aide-De-Cam output, not the target contract.
* CamBANG's internal camera-fact model must remain source-neutral rather than
  ADC-, JSON-, Godot-, Camera2-, or provider-shaped.
* Source-neutral internal types keep provider static/device facts, provider
  per-image facts, externally configured facts, Core-owned capture-admission
  context, and realized payload truth as distinct authorities.
* `ImagingSpec` remains the cross-camera/imaging-subsystem operational
  capability seam. Per-camera description and still-result facts do not become
  an `ImagingSpec` metadata bucket.
* Intrinsics, distortion, and pose are atomic descriptive records.
* CamBANG does not perform calibration, rectification, projection, intrinsic
  rescaling, coordinate-domain conversion, or approximate distortion fitting.
* Rich camera facts principally belong to still-capture results. Exact public
  result bindings remain separately user-gatekept.
* Approved future server-level direction is
  `ingest_camera_description(String json_text) -> Error` and
  `set_capture_geolocation(Dictionary geolocation) -> Error`.
* Those future methods are not yet implemented and must not be documented as
  current source behaviour.

## Tranche boundaries

This tranche introduces an internal bounded ADC v2 parser, typed configured and
active external-camera-description state, and focused deterministic verification.

Do not change public Godot API, provider ingress/interfaces, result resolution,
snapshot schema, provider behavior, or the working grouped-rig admission path.
The existing v1 `ingest_camera_concurrency(...)` path remains unchanged until
the separately approved public rename tranche.

Do not introduce additional Godot APIs, filesystem convenience APIs, a generic
metadata/configuration system, calibration algorithms, or platform-provider
implementation work.

## Tranche state

Completed before this tranche:

* retained grouped-camera capability truth exists through `ImagingSpec`;
* stopped-time transactional concurrency ingestion exists;
* grouped-rig admission consumes normalized allowed camera combinations;
* the broader camera-fact design and implementation sequence have been agreed;
* source-neutral Tranche 1 fact types and focused verification are accepted.

Required before this tranche closes:

* accept exact ADC schema version 2 only, with bounded parse and strict
  recognised-field validation plus additive unknown-member tolerance;
* retain complete externally configured per-camera static facts separately from
  `ImagingSpec`, including valid unmatched entries and exact camera-ID lookup;
* preserve stopped-only, transactional full replacement across configured and
  active generations; rejected input preserves prior accepted truth;
* project only optional concurrency truth into `ImagingSpec` without changing
  grouped-rig admission semantics;
* verify parsing, replacement, clear, persistence, lookup, limits, and the
  internal seam without a public Godot API change.

## After this tranche

Implementation next proceeds through the approved Godot ingestion rename,
capture-admission context, provider fact supply, SyntheticProvider reference
facts, result resolution, and separately approved Godot exposure.
