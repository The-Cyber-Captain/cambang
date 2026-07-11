# Current Tranche

## Active task

Implement the external camera-concurrency truth supply path from
`CamBANGServer.ingest_camera_concurrency(String json_text)` into the existing
Core-owned `ImagingSpec` grouped-rig admission consumer.

## Goal

Accept the Aide-De-Cam (ADC) concurrency projection, validate it
transactionally, retain normalized allowed camera combinations as effective
Core truth, and consume that truth at the existing authoritative cohort-admission
gate.

The effective concurrency state must distinguish:

* unavailable truth;
* explicitly unsupported concurrency;
* supported concurrency with allowed camera-ID combinations.

A grouped participant set is allowed when it is a subset of at least one
reported allowed combination. Single-device capture remains unaffected.

## Required boundaries

* Public API: `Error CamBANGServer.ingest_camera_concurrency(String json_text)`.
* Existing public API is otherwise preserved without redesign or churn; the
  only public addition in this tranche is
  `CamBANGServer.ingest_camera_concurrency(String json_text)`.
* The caller obtains the JSON text; add no filesystem/path convenience API.
* Keep `ImagingSpec` terminology internal.
* Support ADC schema versions through compiled constants in
  `cambang::camera_concurrency::ADC`; the current supported range is `1..1`.
* Treat `generator` as optional provenance, not required producer identity.
* Apply accepted input transactionally; failed input must preserve prior truth.
* Effective truth is immutable during an active Core generation.
* Configured accepted truth may be replaced while stopped and reused across
  later start/stop generations.
* Keep cohort admission as the single authoritative grouped-rig gate.
* Distinguish unavailable truth from an explicit combination rejection.

## Guardrails

Do not add provider-start parameters, a Java `CameraManager` bridge, generic
spec mutation/patch APIs, camera-ID heuristics or prefixes, result-fact work,
calibration work, or unrelated provider architecture.

Consume only the ADC fields needed for camera concurrency. Do not adopt the
unrelated capability document as a general CamBANG schema.

## Done means

The public method is bound and documented; supported ADC input reaches
Core-owned generation-effective truth; malformed, indeterminate, and unsupported
input is rejected without mutation; subset-based grouped admission is verified;
unavailable and explicit rejection remain distinct; accepted truth is immutable
while running and reusable/reconfigurable between generations; focused and
relevant broader deterministic verification is green.
