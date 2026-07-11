# ImagingSpec Seam

This note supplements:

- `docs/naming.md`
- `docs/provider_architecture.md`
- `docs/state_snapshot.md`

It locks the current architectural meaning of `ImagingSpec` without redesigning
existing public API surfaces, result-fact design, calibration behavior, or provider-specific
implementation strategy.

---

## Purpose

`ImagingSpec` is CamBANG's retained cross-camera / imaging-subsystem capability
seam.

It exists for capability truth that is not owned by one camera in isolation but
can still matter to current Core operation. Typical examples include admission
or validation constraints that span multiple cameras or the shared imaging
subsystem.

`ImagingSpec` is therefore distinct from:

- `CameraSpec`, which carries per-camera stable specification/capability truth
- `camera_state`, which carries current target/applied runtime posture
- result fact groups, which describe realized image-time facts where available
- post-result metadata enrichment or calibration products, which are not
  automatically Core operational truth

---

## Operational boundary

`ImagingSpec` is the seam Core may consult when current operational truth needs
cross-camera or subsystem-scoped capability for admission and validation.

This means:

- Core retains the effective `ImagingSpec` version as runtime truth
- snapshot publication may expose that retained effective version
- provider execution may receive a validated imaging-spec patch/version through
  the internal provider boundary

This does not mean:

- `ImagingSpec` is a user-facing result-fact surface
- `ImagingSpec` is a bucket for general metadata enrichment
- `ImagingSpec` replaces provider/runtime posture, Backing Plan truth, or
  realized retained-result support/evidence

---

## Ownership split

The ownership split remains:

- Core is authoritative for retaining the effective `ImagingSpec` version as
  current runtime truth
- providers execute validated Core intent at the provider boundary
- platform-backed providers do not redefine the meaning of `ImagingSpec`

Current source-grounded consequences include:

- the snapshot's `imaging_spec_version` is the effective retained version, not
  a requested or speculative value
- the provider seam may apply an imaging-spec patch using a new retained
  version identifier supplied by Core

---

## Guardrails

The existing public API was preserved without redesign or churn; the sole
approved public addition in this tranche is
`CamBANGServer.ingest_camera_concurrency(String)`. Further unrelated public API
expansion remains out of scope.

This seam lock intentionally does not broaden into:

- result-fact redesign
- calibration or metadata-enrichment architecture
- platform-provider implementation detail
- further public Godot API changes beyond the approved ingestion method

If a future change needs one of those, it should be proposed explicitly rather
than smuggled in through `ImagingSpec` terminology.
