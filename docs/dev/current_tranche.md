# Current Tranche

## Active workstream

Complete the camera-description and still-capture fact model that follows the
already-implemented external camera-concurrency ingestion seam.

The checked-out source implements stopped-time transactional
`CamBANGServer.ingest_camera_description(String json_text)` replacement of a
supported ADC v2 camera-description document. Optional concurrency projects
into retained `ImagingSpec`; grouped-rig admission consumes that projection.

The broader agreed direction is recorded in the camera-fact handover supplied
for this workstream. Source inspection remains authoritative for actual code
state.

## Current tranche

Complete the public ADC v2 camera-description ingestion replacement after the
accepted internal parser and retained external camera-description state work.

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
* `ingest_camera_description(String json_text) -> Error` is the sole public
  camera-description ingestion surface. Capture geolocation remains separately
  gatekept and is not implemented.

## Tranche boundaries

This tranche exposes the existing bounded ADC v2 parser and typed retained
external-camera-description state through the single public ingestion method.

Do not add further public Godot API, provider ingress/interfaces, result
resolution, snapshot schema, provider behavior, or change the working
grouped-rig admission path. The legacy v1 concurrency parser has no Godot
binding.

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
* verify public stopped-time replacement, rejection preservation, exact v2
  acceptance, legacy v1 public rejection, and projected grouped-rig admission.

## After this tranche

Implementation next proceeds through capture-admission context, provider fact
supply, SyntheticProvider reference facts, result resolution, and separately
approved Godot exposure.
