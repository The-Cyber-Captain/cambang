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

Implement source-neutral internal C++ camera-fact records after the accepted
architecture and ADC v2 documentation/schema tranche.

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

This tranche introduces internal Core-neutral types and focused deterministic
verification only. Legacy result-fact/result-store/Godot structures remain
compatibility surfaces unless a narrow compile-preserving adapter is required.

Do not change public Godot API, provider ingress/interfaces, ADC parsing or
retained external state, result resolution, snapshot schema, runtime behaviour,
or the working concurrency admission path.

Do not introduce additional Godot APIs, filesystem convenience APIs, a generic
metadata/configuration system, calibration algorithms, or platform-provider
implementation work.

## Tranche state

Completed before this tranche:

* retained grouped-camera capability truth exists through `ImagingSpec`;
* stopped-time transactional concurrency ingestion exists;
* grouped-rig admission consumes normalized allowed camera combinations;
* the broader camera-fact design and implementation sequence have been agreed.

Required before this tranche closes:

* introduce strongly typed, presence-aware internal records without interpreting
  legacy zero/default values as new-model facts;
* retain ordinary source facts as value plus origin only; defer effective
  resolution authority until the later result-resolution tranche;
* represent static camera description, capture-admission context, and per-image
  facts separately;
* cover classification, atomic intrinsics/distortion/pose, geolocation, Capture
  Date-Time, Image Acquisition Timing, focus state, and realized image transform;
* reject empty string-bearing pose identifiers/tokens and settled invalid
  calibration/geolocation values; keep finite focus distance distinct from
  infinity, unknown, and absence;
* verify explicit presence, zero, semantic-unknown, and invalid-state
  distinctions with focused deterministic coverage;
* add focused deterministic verification with no public API change.

## After this tranche

Implementation next proceeds through ADC v2 parsing and retained external
state, the approved ingestion replacement, capture-admission context, provider
fact supply, SyntheticProvider reference facts, result resolution, and
separately approved Godot exposure.
