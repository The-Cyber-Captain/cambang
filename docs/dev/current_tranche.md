# Current Tranche

## Unified Image Acquisition Timing

### Objective

Replace the ambiguous `capture_timestamp` model with one truthful Image
Acquisition Timing model shared by capture members and stream results.

Separate provider-authored acquisition timing from Core-owned retained-frame
identity, remove duplicate timing transport, retire the old public timestamp
surface directly, and preserve stream hot-path performance.

### Locked Decisions

* Image Acquisition Timing contains acquisition mark, rational tick period,
  clock domain, reference event, and comparability scope.
* It is provider-authored, optional where unavailable, and carried once on the
  existing `FrameView` event. Acquisition mark zero is valid.
* It is distinct from Capture Date-Time, lifecycle/performance timing, and
  retained-frame identity.
* Core-owned retained-frame identity, not acquisition timing, provides retained
  payload/backing correlation, freshness, deduplication, evidence identity, and
  result signatures.
* No separate per-frame timing callback, eager Godot conversion, compatibility
  alias, or avoidable stream hot-path allocation is permitted.

### Stage A — Internal Model and Authority

Implement and review this stage before public API replacement:

* establish one source-neutral acquisition-timing contract usable by providers
  and Core;
* carry complete optional timing on `FrameView`;
* retain that timing on stream results and capture image members;
* source capture-member timing only from the accepted member frame and remove
  acquisition timing from `ProviderCaptureImageFacts`;
* introduce Core-owned retained-frame identity at successful retention;
* replace timestamp-based identity, correlation, freshness, deduplication,
  evidence, and signature uses;
* remove redundant timestamp fields used only for payload/backing correlation;
* update SyntheticProvider and StubProvider truthfully.

The existing public timestamp methods and member key may remain during Stage A
only as projections derived from canonical retained timing. They are not
internal truth or identity and remain mandatory Stage B removals.

Stage A must make no public Godot API shape change.

### Stage B — Public Replacement and Closeout

Expose complete timing lazily as:

```text
get_image_member(index).camera_facts.acquisition_timing
stream_result.camera_facts.acquisition_timing
```

Directly remove:

```text
CamBANGCaptureResult.get_capture_timestamp()
get_image_member(index).capture_timestamp
CamBANGStreamResult.get_capture_timestamp()
```

Also remove every Stage A projection and remaining in-scope acquisition
`timestamp` term, field, fixture, assertion, and document reference that exists
solely for the old surface.

Do not retain aliases, deprecated keys, compatibility wrappers, duplicate
transport, or parallel scalar timing truth.

`get_capture_datetime_unix_nanoseconds()` remains unchanged and distinct.

### Verification

Stage A must prove one authoritative internal timing path; truthful absent,
zero, equal, opaque, and incomparable timing; identity independent of timing;
and no extra frame event, eager Godot conversion, or public API change.

Stage B must prove the approved equivalent public shapes, direct absence of all
three old timestamp surfaces, exact bracket/rig/stream timing retention, and
removal of every intermediate projection.

Complete the affected focused checks, full Core and provider-compliance
verification, Windows and Android arm64 GDE builds, Scene 569, affected stream
verification, representative Scene 870 baseline comparison, `git diff --check`,
and final source/documentation terminology audit.

### Constraints

No Capture Date-Time or geolocation redesign, unrelated camera-fact/CameraSpec/
ImagingSpec work, or Camera2/WinRT provider implementation.

### Completion

The tranche closes only when one authoritative timing model and transport serve
stream and capture, retained-frame identity is separate, the approved Godot
surfaces replace the old API, every intermediate projection and obsolete term
is removed, validation passes, and stream performance remains consistent with
the accepted baseline.
