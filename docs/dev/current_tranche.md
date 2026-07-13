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

Implement internal provider fact ingress after the accepted ADC v2 ingestion
and capture-admission-context work.

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

This tranche adds source-neutral provider-to-Core ingress for provider static
camera facts and per-capture-image intrinsics, distortion, and pose. Provider
facts remain distinct from external configured facts and admission context;
they are not yet resolved into results, snapshots, or public surfaces.

Do not add further public Godot API, provider reference-fact implementation,
result resolution, snapshot schema, or change the working grouped-rig
admission path. The legacy v1 concurrency parser has no Godot binding.

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

* retain provider static facts by active provider device identity;
* retain per-image facts by exact capture, device, and member identity;
* reject malformed or mismatched callback facts without mutating retained truth;
* retain owned copies across the callback/Core boundary;
* preserve bracket and rig member identity without resolving precedence against
  external configured facts.

## After this tranche

Implementation next proceeds through SyntheticProvider reference facts, result
resolution, and separately approved Godot exposure.
