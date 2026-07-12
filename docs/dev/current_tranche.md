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

Formalise the source-neutral camera-fact architecture and ADC v2 interchange
contract before C++ implementation begins.

This tranche may require more than one Codex invocation for drafting, review,
correction, and documentation reconciliation. Keep individual invocation tasks
in their Codex prompts; keep this file focused on tranche-wide state and
constraints.

## Settled tranche direction

* ADC v2+ is the target interchange contract. CamBANG is not required to ingest
  ADC v1.
* ADC v1 human and machine schemas are historical evidence of the existing
  Aide-De-Cam output, not the target contract.
* CamBANG's internal camera-fact model must remain source-neutral rather than
  ADC-, JSON-, Godot-, Camera2-, or provider-shaped.
* Provider static/device facts, provider per-image facts, externally configured
  facts, Core-owned capture-admission context, and realized payload truth remain
  distinct authorities.
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

This tranche is documentation and schema work only.

Do not change C++, Godot bindings, tests, verification scenes, build scripts,
snapshot schema, provider interfaces, runtime behaviour, or the working
concurrency admission path.

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

* establish the documentation authority and placement for the camera-fact
  architecture and ADC v2 contract;
* add matching human-readable and machine-readable ADC v2 contracts;
* reconcile affected canonical and supplementary documentation without
  duplicating the contract across unrelated files;
* distinguish implemented concurrency-only source behaviour from approved
  future camera-description APIs;
* validate the resulting schema and documentation references where practical;
* complete a contradiction and implementation-sufficiency review.

## After this tranche

Implementation proceeds through source-neutral fact types, ADC v2 parsing and
retained external state, the approved ingestion replacement, capture-admission
context, provider fact supply, SyntheticProvider reference facts, result
resolution, and separately approved Godot exposure.
