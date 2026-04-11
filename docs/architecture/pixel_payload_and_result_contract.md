# Pixel Payload and Result Contract

Status: Draft architecture contract  
Purpose: Defines the canonical contract for image-bearing payloads, retained results, ownership, and materialization in CamBANG’s release-facing stream and capture paths.

This document supplements, and does not supersede, the canonical runtime and provider architecture documents including:

- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`
- `docs/architecture/frame_sinks.md`
- `docs/state_snapshot.md`
- `docs/naming.md`

---

## 1. Why this contract exists

CamBANG already distinguishes between:

- runtime snapshot / introspection truth
- provider ↔ core frame ingress
- release-facing image access in result-oriented terms

However, the release product requires a clearer contract for the image-bearing path itself.

Specifically, CamBANG must support:

- efficient non-CPU-only image paths where platforms permit
- explicit CPU/image materialization where required
- truthful ownership and release of retained image-bearing resources
- separate stream and still-capture result behavior
- rich image-associated fact surfaces without bloating the hot runtime snapshot path

This document defines that contract.

---

## 2. Design goals

The image-bearing path should satisfy the following goals.

1. Avoid forcing all runtime image-bearing artifacts through a CPU-only path.
2. Avoid forcing all runtime image-bearing artifacts into GPU-only access semantics.
3. Keep backend/platform specifics contained inside Providers.
4. Keep Core authoritative for routing, retained truth, lifecycle/resource accounting, and result semantics.
5. Expose release-facing image access through Result Objects rather than mailbox-oriented terms.
6. Make expensive conversions/readbacks/materializations explicit rather than silently universal.
7. Preserve truthful original image artifacts while allowing derived materializations.

---

## 3. Layer model

## 3.1 Provider acquisition layer

Providers acquire or produce image-bearing data using the most efficient truthful mechanism available on that backend/platform.

This may include, depending on provider:

- CPU-readable packed buffers
- CPU-readable planar/semi-planar buffers
- GPU/native-surface-backed payloads
- encoded still-image payloads
- raw still-image payloads
- synthetic generated payloads

Providers remain responsible for:

- backend API adaptation
- backend-specific resource handling
- truthful reporting of provider-owned lifecycle/resource state
- mapping backend semantics into CamBANG payload vocabulary

Providers do **not** define release-facing result semantics.

## 3.2 Core routing and retention layer

Core receives accepted provider payloads and is responsible for:

- deterministic ownership transfer
- release-on-drop discipline
- stream/capture routing
- retained-result policy
- requested/effective/observed truth where applicable
- truthful lifecycle/resource accounting consequences
- publication/query surfaces for retained truth

Core is not the generic platform conversion engine.

## 3.3 Result/materialization layer

Result Objects expose image-bearing runtime outputs to Godot-facing/runtime-visible code.

The canonical public/runtime-visible result nouns remain:

- **Stream Result**
- **Capture Result**
- **Capture Result Set**

Result Objects may expose:

- direct access to already-retained forms
- explicit materialization into derived forms
- capability and cost classification for those access paths
- qualified image-associated fact groups

---

## 4. Core principle

CamBANG does **not** define one universal canonical in-memory image representation for all runtime image-bearing outputs.

Instead, CamBANG defines:

- a provider-facing payload contract
- a Core-owned routing/retention/release contract
- a result-facing access/materialization contract

A given image-bearing artifact may therefore legitimately exist in one or more retained or materializable representations.

---

## 5. Payload taxonomy

Every accepted image-bearing payload has exactly one primary `payload_kind`.

Recommended first-pass kinds:

## 5.1 `CPU_PACKED`

A CPU-readable packed-pixel payload.

Typical examples:

- RGBA8
- BGRA8
- other tightly/row-packed formats as later supported

Properties:

- directly CPU-readable
- usually convenient for CPU-side image materialization
- may still be expensive to copy at high cadence/high resolution

## 5.2 `CPU_PLANAR`

A CPU-readable multi-plane or semi-planar payload.

Typical examples:

- NV12
- I420
- related YUV-family representations

Properties:

- directly CPU-readable
- often acquisition-efficient
- often suitable for GPU conversion/display paths
- not directly equivalent to a display-ready RGB image

## 5.3 `GPU_SURFACE`

An opaque GPU-native or native-surface-backed payload.

Typical examples include platform-native shareable/importable GPU/surface resources.

Properties:

- not required to be CPU-readable
- may be the cheapest display path
- may require explicit readback/materialization for CPU/image access

## 5.4 `ENCODED_IMAGE`

An encoded image payload.

Typical examples:

- JPEG
- future encoded still representations where supported

Properties:

- especially relevant for still capture
- may already be the best save/export artifact
- not the same thing as raw/display-ready pixels

## 5.5 `RAW_IMAGE`

A raw or raw-domain still payload.

Properties:

- high-fidelity capture-oriented artifact
- not inherently display-ready
- may require explicit processing/materialization

---

## 6. Payload metadata requirements

Every accepted payload must carry enough provider-agnostic metadata for truthful routing, retention, and later materialization decisions.

Depending on `payload_kind`, this may include:

- dimensions
- pixel/encoding format
- plane/stride layout
- orientation/crop information where relevant
- provider-agnostic capture timestamp representation
- stream/capture association
- ownership/release handle/callback wrapper
- provenance of original vs derived representation where relevant

This document does not freeze exact C++ field layout, but the presence of adequate metadata is part of the contract.

---

## 7. Ownership and deterministic release

## 7.1 General rule

Every payload crossing the provider → Core boundary must have a clear single ownership/release path.

At all times, one layer is responsible for eventual release.

## 7.2 Possible fates of a provider payload

A payload presented to Core may be:

1. dropped before retention
2. accepted into the stream path
3. accepted into the capture path
4. materialized into one or more derived representations
5. released

## 7.3 Drop discipline

If a payload is rejected or dropped before retention:

- release must occur deterministically
- counters/diagnostics must reflect the path truthfully
- dropping image-bearing payloads must not suppress lifecycle/native-object/error truth

## 7.4 Retained-result ownership

If a payload is retained as part of a Stream Result or Capture Result:

- the retained-result layer becomes responsible for eventual release
- any retained provider-native resource must remain truthfully accounted for
- any derived representations must also have clear ownership and release rules

## 7.5 Truthfulness

No resource may be reported as released before it is actually released.

Original payload truth and derived-retention truth must remain auditable through Core/runtime accounting.

---

## 8. Stream Sink and Capture Sink split

Frame sinks are internal/runtime extension points, not public API nouns.

Where stream and still paths need to be distinguished internally, the preferred terms remain:

- **Stream Sink**
- **Capture Sink**

These correspond to the public/runtime-visible result nouns:

- Stream Sink → Stream Result
- Capture Sink → Capture Result / Capture Result Set

---

## 9. Stream path contract

## 9.1 Purpose

The repeating-stream path is primarily:

- latency/display oriented
- latest-result oriented
- tolerant of replacement/coalescing
- not required to retain every produced frame

## 9.2 Stream Sink responsibilities

The Stream Sink is responsible for:

- accepting/rejecting stream payloads according to policy/capability
- retaining the latest accepted stream artifact
- releasing superseded retained resources deterministically
- recording relevant stream-path counters/diagnostics
- exposing materialization capabilities through Stream Result

## 9.3 Stream Result semantics

A **Stream Result** represents the latest retained repeating-stream image-bearing output for that stream.

A Stream Result is **not** required to mean:

- every stream frame is retained
- the retained form is already CPU `Image`-ready
- the retained form is necessarily save/export oriented

A Stream Result may retain one or more of:

- provider-native GPU/native surface
- CPU planar payload
- CPU packed payload
- derived display-oriented representation
- derived CPU/image representation

## 9.4 Preferred stream retention direction

Preferred retained form order is roughly:

1. displayable GPU/native surface
2. planar payload suitable for GPU conversion/display
3. packed CPU payload
4. additional derived forms only when policy or explicit request requires them

This is a preference order, not a correctness rule.

---

## 10. Capture path contract

## 10.1 Purpose

The still-capture path is primarily:

- fidelity/persistence oriented
- discrete-result oriented
- processing/export/save oriented

## 10.2 Capture Sink responsibilities

The Capture Sink is responsible for:

- associating payloads with the correct capture event/device
- retaining the realized still artifact
- exposing materialization capabilities through Capture Result
- supporting grouped rig-capture results

## 10.3 Capture Result semantics

A **Capture Result** represents a discrete device-associated still-capture artifact.

A Capture Result is **not** required to already be:

- a CPU `Image`
- a display-ready RGB pixel buffer
- an encoded artifact

A Capture Result may retain one or more of:

- encoded still artifact
- raw still artifact
- GPU/native surface
- CPU planar payload
- CPU packed payload
- derived forms requested or retained by policy

## 10.4 Capture Result Set semantics

A **Capture Result Set** represents the grouped result of a rig-triggered capture and contains the subset of device-associated Capture Results that actually realized.

## 10.5 Preferred capture retention direction

Preferred retained form order is roughly:

1. already-encoded still artifact when save/export is the natural path
2. high-fidelity CPU-readable artifact when processing is the expected path
3. truthful native/GPU artifact when readback can remain explicit
4. additional derived forms only when requested or policy-configured

Again, this is a preference order, not a correctness rule.

---

## 11. Capability and cost-aware materialization

## 11.1 Core principle

Materialization is the process of obtaining a derived representation from a retained result artifact.

Materialization is explicit.

CamBANG must not assume that every retained result is already available in every desired form.

## 11.2 Capability/cost states

Result access/materialization paths should be classifiable in provider-agnostic terms.

Recommended first-pass capability/cost states:

- `READY`
- `CHEAP`
- `EXPENSIVE`
- `UNSUPPORTED`

### Intended meaning

- `READY`  
  The representation is already retained and directly accessible.

- `CHEAP`  
  The representation is not currently retained but can be obtained without substantial additional work.

- `EXPENSIVE`  
  The representation is supported but requires meaningful additional work such as readback, conversion, decode, or full-frame copy/repack.

- `UNSUPPORTED`  
  The representation cannot currently be obtained through a supported CamBANG path.

## 11.3 Naming discipline

A method/access path whose name sounds cheap should actually be cheap.

For example:

- display-view access should not secretly force full CPU readback
- encoded-byte access should not secretly re-encode raw pixels unless made explicit
- image materialization methods may legitimately be `EXPENSIVE`

---

## 12. “Useful display” tiers

A retained result/payload is usefully displayable only when CamBANG can expose a defined supported display path for it.

Recommended conceptual tiers:

## 12.1 Directly displayable

The retained representation can be used in the supported display path without substantial conversion.

## 12.2 Displayable via supported materialization

The retained representation is not directly displayable, but CamBANG has a defined supported path to display it.

## 12.3 Not currently displayable through a supported path

The retained representation may still be useful for save/export/processing, but CamBANG does not currently expose a supported display path for it.

These are capability truths, not vague UX impressions.

---

## 13. Qualified fact groups on results

Result Objects should expose qualified image-associated fact groups rather than a generic metadata bag.

Recommended first-pass groups:

- `ImageProperties`
- `CaptureAttributes`
- `LocationAttributes`
- `OpticalCalibration`

## 13.1 ImageProperties

Structural image facts such as:

- width
- height
- format
- orientation
- bit depth
- crop state where meaningful

## 13.2 CaptureAttributes

Capture/device-state facts associated with the realized image such as:

- exposure time
- aperture
- focal length
- focus distance
- sensor sensitivity / ISO-equivalent
- flash state
- white-balance-related state

## 13.3 LocationAttributes

Capture-associated location facts such as:

- latitude / longitude / altitude
- location accuracy where known
- location timestamp where known

## 13.4 OpticalCalibration

Calibration/optics facts such as:

- intrinsics
- focal lengths in calibration space
- principal point
- distortion model
- distortion coefficients

---

## 14. Provenance model

Provenance is attached per fact (or per small fact group), not as one result-wide blanket classification.

Recommended first-pass provenance vocabulary:

- `HARDWARE_REPORTED`
- `PROVIDER_DERIVED`
- `RUNTIME_INJECTED`
- `USER_DEFAULT`
- `USER_OVERRIDE`
- `UNKNOWN`

The intended meaning of these values is documented in the direction note:
- `docs/dev/pixel_result_architecture_direction.md`

### 14.1 Important distinction

Provenance answers:

> where did this fact come from?

It does **not** by itself answer:

> how good/correct is this fact?

A hardware-reported fact may still be known-bad on some device; a user override may actually be more correct than the hardware/API.

---

## 15. Original vs derived result truth

Every retained result should conceptually distinguish between:

- the **original** image artifact
- any **derived** materializations

The original artifact remains authoritative result truth.

Derived forms may include, for example:

- display-oriented imports/views
- CPU/image materializations
- encoded exports
- future rectified/undistorted materializations

Derived forms must not erase the truth of the original retained artifact.

---

## 16. Rectification / undistortion direction

Default policy is:

- retain the original image artifact
- carry calibration/distortion with the result
- do not silently rectify by default

However, the architecture should allow future derived materializations such as:

- rectified display view
- rectified image materialization

These should participate in the same capability/cost-aware materialization model as other derived paths.

---

## 17. Relationship to State Snapshot

State Snapshot remains the operational/runtime truth surface.

It should generally expose only the compact subset of image-path truth that is broadly useful for runtime introspection.

Examples of possible compact summary truth might include:

- whether a device/stream/capture currently has associated calibration available
- whether a latest result contains attached location
- whether latest result fact provenance is mixed
- compact visibility/materialization path summaries

Full rich result-associated fact bundles should generally remain on Result Objects rather than in the hot snapshot path.

---

## 18. Relationship to System Graph

System Graph is the slower query-oriented truth surface for structural/configuration/calibration knowledge.

System Graph is the more natural home for things such as:

- platform-known devices and relations
- rig/group structural truth
- corrected specification/calibration truth
- capability and combination constraints
- user defaults and overrides that describe device/lens/system understanding

Result Objects remain the natural home for per-image realized truth.

---

## 19. Explicit non-goals

This contract does **not** yet define:

- final Godot method names/signatures
- final C++ type layout
- final System Graph schema
- full provider-specific GPU interop details
- full RAW-processing pipeline design
- sequence/recording/broadcast APIs
- complete rectification implementation design

Those belong in narrower follow-on design work.

---

## 20. Compressed contract summary

CamBANG’s release-facing image path is a **multi-representation, provider-adapted, Core-routed, result-oriented** pipeline in which:

- Providers surface the most efficient truthful payload available on that backend/platform.
- Core routes, retains, accounts for, and releases image-bearing artifacts deterministically.
- Stream paths are latest-result and latency/display oriented.
- Capture paths are discrete-result and fidelity/persistence oriented.
- Result Objects expose capability/cost-aware access and explicit materialization.
- Rich image-associated facts remain attached to results rather than bloating the hot snapshot path.
- Original image truth is preserved, while derived materializations remain explicit.

---