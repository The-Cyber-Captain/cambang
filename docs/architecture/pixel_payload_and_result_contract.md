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
5. Expose release-facing image access through Result Objects rather than sink-storage terms.
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

Every accepted payload must carry enough provider-agnostic metadata for truthful retention and later materialization decisions.

Depending on `payload_kind`, this may include:

- dimensions
- pixel/encoding format
- plane/stride layout
- orientation/crop information where relevant
- optional source-neutral Image Acquisition Timing
- stream/capture association
- ownership/release handle/callback wrapper
- provenance of original vs derived representation where relevant

This document does not freeze exact C++ field layout, but the presence of adequate metadata is part of the contract.

## 6.1 Pixel format descriptor

Layout arithmetic for every format CamBANG can name lives in one truth table
(`src/pixels/format/pixel_format_descriptor.h`): plane count, per-plane row
bytes and row count, component depth, chroma subsampling, and whether the
format is packed RGB-family or YUV-family.

Provider, Core, and Godot layers derive size, stride, and bit-depth answers
from that table. No layer may open-code `width * 4` or infer bit depth from a
format name.

Naming a format in the table is **not** a support claim. An entry means
CamBANG can reason about that format's geometry. Whether a format can be
retained, displayed, or materialized is proven by the paths that implement it,
and every such path states its own admissible set explicitly. A format with a
descriptor but no implementing path must fail closed, not fall through to a
packed-RGBA reinterpretation.

An unnamed FourCC yields an invalid descriptor. Callers must treat that as
"geometry unknown" and reject, never guess.

## 6.2 Payload layout and colorimetry

A CPU payload is described by a plane descriptor, not a single pointer:

- per-plane data pointer, byte count, stride, and row count;
- the shared format tag and image dimensions;
- colorimetry.

`FrameView` carries this as an optional `payload_layout`. When absent, the
frame is single-plane and the legacy scalar fields
(`data`/`size_bytes`/`stride_bytes`/`format_fourcc`) are authoritative.
Consumers read `FrameView::effective_payload_layout()` so both cases answer
identically. A provider emitting a planar or semi-planar payload **must**
populate the layout, because the scalars cannot describe more than one plane;
a multi-plane format tag delivered through the scalars alone is malformed and
resolves to absent.

Colorimetry — range, matrix, transfer, primaries — is a first-class part of the
payload contract, not an afterthought. Limited-vs-full range and BT.601 vs
BT.709 matrix are facts the provider knows at acquisition and that no
downstream layer can recover from the bytes. Getting them wrong produces an
image that is plausible and incorrect, which is worse than a failure.

`UNSPECIFIED` is truthful absence, not a default value. A consumer needing a
concrete value must choose its fallback explicitly; it must not silently treat
`UNSPECIFIED` as any particular colour space.

## 6.3 Native format capability and selection

Providers declare the formats they can emit **without converting**, in their own
preference order, alongside whether they will convert to packed RGBA/BGRA on
request. This is acquisition capability truth, parallel to
`ProducerBackingCapabilities` and equally distinct from payload-kind policy.

Advertising a format the provider does not actually emit is a contract
violation, not an optimization hint.

The direction this enables — and the reason the capability is declared rather
than assumed — is that conversion should happen at the **latest** useful point,
not the earliest:

- the display path does not need CPU packed RGB at all, since subsampled YUV
  can be uploaded as its native planes and converted by the sampling shader;
- the materialization path (`to_image()` / `to_image_member()`) is the only
  consumer that genuinely requires packed RGB, and it is explicit,
  user-triggered, and already cost-classified under §11.

Converting inside the provider, on the acquisition thread, before anything has
asked for pixels, pays the cost unconditionally at the most latency-sensitive
point in the pipeline. Where a provider's backend delivers YUV, its native
format is the truthful advertisement and conversion belongs downstream of
retention.

### 6.3.0 Pixel format is not a user-facing concept

Choosing a pixel format is CamBANG's job, not the application's. A user asking
for a preview stream should not have to know, or name, the format their
hardware happens to prefer. Requiring a `format_fourcc` in a public stream
definition puts a hardware detail in the path of an ordinary request, which
conflicts with keeping normal user-facing APIs simple.

The capability advertisement in §6.3 exists so that Core can make that choice:
the provider states what it can emit natively, Core selects based on how the
result will actually be consumed, and the application never names a format.
Negotiation today only *validates* a caller-supplied format, which leaves the
burden in the wrong place. Moving Core from validating a choice to making one
is the intended direction.

`format_fourcc` on a public stream profile is therefore transitional. Where a
format must be pinned — maintainer harnesses, verification scenes, diagnostics
— that is developer tooling, and belongs with the other advanced surfaces
rather than in the ordinary path.

SyntheticProvider is a deliberate exception in a different sense. For a
generator, output format is a property of the generator itself, alongside
pattern preset and seed, so it belongs in `PictureConfig` with the rest of the
synthetic appearance controls rather than in a hardware-facing profile.
Pinning a format is meaningful for Synthetic in a way it is not for a camera.

### 6.3.1 Current implementation status

Recorded so a declared-but-unimplemented kind stays distinguishable from a
working one. This section describes what is built, not what CamBANG may
support; an absent entry means "not implemented yet", never "excluded by
design".

- `ResultPayloadKind::CPU_PLANAR` is written by the retention path and
  reported by results. Semi-planar and fully planar payloads are retained in
  the layout the producer delivered.
- Conversion to packed RGBA happens in Core (`planar_payload_to_rgba8`), at
  the point a caller asks for CPU display bytes or a Godot `Image` -- not in
  the provider, and not at all if no caller asks.
- Neither platform provider converts YUV for delivery any more. Both take the
  camera's native planar buffer through unconverted when Core selects a planar
  format. Their packed-conversion paths remain, and are reached only when a
  caller explicitly names RGBA or BGRA.
- Chroma order is carried by the descriptor (`chroma_v_first`), because
  NV21/YV12 are indistinguishable from NV12/I420 by geometry alone and a
  mis-read produces a plausible-looking red/blue swap rather than a failure.

Emission by provider, as a **progress note only**:

| Format | Synthetic stream | Synthetic capture | WinRT stream | WinRT capture | Camera2 stream | Camera2 capture |
|---|---|---|---|---|---|---|
| RGBA | yes | yes | yes | yes | yes | yes |
| BGRA | no | yes | yes | yes | yes | yes |
| NV12 | yes | yes | yes | yes | yes | yes |
| NV21 / I420 / YV12 | yes | yes | no | no | yes, device-resolved | yes, device-resolved |
| YUY2 / UYVY | no | no | no | no | no | no |

WinRT is NV12-only among planar formats because `MediaPixelFormat` has exactly
two uncompressed members, Bgra8 and Nv12. That is the API's limit, not a
CamBANG one.

One gap this table makes visible:
- **Camera2 requests NV12 or I420 but may deliver NV21 or YV12.** The provider
  resolves the concrete family member from the observed strides and pointer
  order, and the retained payload carries the delivered FourCC, so payload
  truth is correct. The stream *profile* still records the requested tag, so
  profile and payload can name different formats for the same stream.

Pixel format is independent of pattern content. The synthetic stream render
spec is packed RGBA8 regardless of the requested profile format, and format
conversion happens after rendering, so every pattern preset works with every
format the provider emits. Adding a format does not require revisiting the
pattern module.

### 6.3.2 Platform format availability

Recorded so that later payload decisions inherit this grounding instead of
re-deriving it. This describes the **shapes CamBANG must be ready to accept**,
not a device compatibility list. Actual support is always resolved by runtime
capability query against the attached hardware; no part of CamBANG may key
behaviour off a device model, and nothing here licenses a hardcoded format
assumption.

**Flexible formats are a family, not a format.** Camera2's `YUV_420_888` is
the load-bearing example: a chroma `pixelStride` of 2 means semi-planar
(NV12/NV21), 1 means fully planar (I420/YV12), and the device decides at
runtime. Per-plane strides are the only truthful description of what a given
device handed over. This is why `PayloadLayout` carries strides rather than
trusting a format tag, and why a provider emitting a flexible format must
resolve the concrete FourCC from the strides it observes.

Still-capture availability on current targets:

| Still format | Windows (WinRT) | Android (Camera2) | Payload kind |
|---|---|---|---|
| BGRA8 | `MediaPixelFormat::Bgra8` | not a capture format | `CPU_PACKED` |
| NV12 | `MediaPixelFormat::Nv12` | via `YUV_420_888` when semi-planar | `CPU_PLANAR` |
| NV21 / I420 / YV12 | no | via `YUV_420_888`, device-dependent | `CPU_PLANAR` |
| YUY2 / UYVY | stream subtypes only | no | `CPU_PACKED` |
| JPEG | `CreateJpeg()` | guaranteed at every hardware level | `ENCODED_IMAGE` |
| HEIC / JPEG_R | no | capability-gated | `ENCODED_IMAGE` |
| RAW Bayer | not exposed by this surface | capability-gated | `RAW_IMAGE` |
| Opaque GPU | n/a | `PRIVATE`, never CPU-readable | `GPU_SURFACE` |

Two consequences worth carrying forward:

- **Uncompressed stills are natively planar on both targets.** WinRT's
  uncompressed photo surface offers exactly two pixel formats, BGRA8 and NV12;
  Camera2 guarantees `YUV_420_888` stills at maximum size on LIMITED hardware
  and above. Planar still capture is therefore well-founded, not speculative.
- **JPEG is the only still format guaranteed on every Android device**,
  including LEGACY, and is the natural save/export artifact. It is also the
  one payload kind where retaining the provider's own output means performing
  no pixel work at all, which is why §10.5 already ranks an already-encoded
  still artifact first for capture retention.

Known descriptor gaps, each breaking a different current assumption:

- **Grayscale (Y8/Y16).** No descriptor exists. Thermal sensors typically
  deliver single-plane monochrome, and radiometric Y16 is semantically not a
  picture: its values are temperature, so any RGB rendering is a false-colour
  palette and therefore a derived representation under §15, never the original
  retained truth.
- **10-bit (P010, 10-bit flexible YUV).** The descriptor fields can already
  express it; no downstream path consumes it.
- **Bayer RAW.** Needs a colour-filter-array concept the descriptor has no
  vocabulary for, and packed sub-byte layouts (RAW10/RAW12) are not modelled
  by the current per-plane row arithmetic at all.

## 6.x Primary backing vs sidecar backing

A realized image-bearing artifact may have:

- one **primary backing**
- zero or more **sidecar backings**

The result’s `payload_kind` reflects the **primary backing only**.

Sidecar backings may improve access/materialization capability, but they do
not create multiple primary payload kinds for a single accepted artifact.
Primary/sidecar is an identity and retention model, not a performance ranking:
a sidecar backing may support a particular operation while the primary backing
remains the principal retained representation.

### 6.x.1 CPU and GPU backing are not treated as symmetric choices

CPU-backed and GPU-backed realization are not treated as perfectly symmetric choices
with only an abstract performance difference.

In particular:

- CPU `Image` materialization remains an **available** result-facing fallback path.
- This does **not** imply that CPU backing is always the primary retained form.
- This does **not** imply that CPU materialization is always cheap.
- GPU-native usefulness remains runtime/path dependent.

### 6.x.2 Sources that can provide more than one backing

A source may legitimately provide more than one backing for the same realized artifact,
for example:

- CPU-backed and GPU-backed realization for the same stream frame
- CPU-readable and encoded forms for the same capture artifact

Provider policy chooses one backing as primary and may optionally retain another
as sidecar backing when that improves usefulness or avoids later expensive materialization.

For synthetic stream results, current retained-primary `GPU_SURFACE` truth is
determined by the emitted/retained GPU artifact itself, not by whether a
sidecar CPU backing is also retained.

### 6.x.3 Backing Plan, Backing State, Operation Support, and Access Evidence

CamBANG separates parent-scoped retention planning from the truth carried by a
retained result.

**Backing Plan** is the internal production/retention plan owned by the
relevant Native Payload Support Parent. It states which backing forms CamBANG
intends to retain for that parent's image-bearing work. During bounded
evaluation this may temporarily exist as both a Requested Plan and a Steady
Plan. It is not public API, provider API, snapshot schema, a route table, or a
per-call route-economics mechanism.

**Backing State** is the concrete backing state actually associated with a
retained result or capture member.

**Operation Support** is operation-level support for that retained
artifact/member, expressed using `ResultCapability` for operations such as
`display_view`, `to_image`, and `encoded_bytes`. It is populated from current
Backing State and implemented access paths. Godot result capability methods
consume Operation Support, while materialization methods remain defensive and
verify the concrete backing/path they use.

**Access Evidence**, when exposed through Synthetic dev metrics, is evidence
collected at the real retained-result operation seam. It is not Operation
Support, not `CamBANGStateSnapshot`, not schema v1, and not an input to state
publication. Bounded internal calibration uses this evidence for live retained
artifacts/postures and records a refined result-facing classification beside
provisional Operation Support. Public user-triggered `get_display_view()`,
`to_image()`, and `to_image_member()` calls remain instrumented, but they are
not the normal recalibration heartbeat; calibration renewal belongs to the live
applied production-posture boundary.

Parent-scoped evaluation input is kept separate from all of the above. CamBANG
first resolves the truthful outer provider/runtime envelope, then resolves the
parent-context capability for the owning `Stream` or `AcquisitionSession`. That
parent-context capability is input to parent-scoped Backing Plan evaluation; it
is not itself Backing Plan, Backing State, Operation Support, or Access
Evidence.

Capture-side Backing Plan evaluation remains owned by the capture-side Native
Payload Support Parent. When a real `AcquisitionSession` exists, it owns the
capture Backing Plan decision. When capture admission must happen before a real
session exists, provisional priming or seed reuse may supply the first
Requested Plan, but real `AcquisitionSession` ownership takes over once the
session is truthfully realized.

### 6.x.4 Unsupported GPU-only realization

If a source offers only GPU-backed realization and the current runtime does not provide
a usable GPU realization path for that result flow, that source/path is unsupported for
that flow.

CamBANG must not treat this case as equivalent to having CPU-backed fallback when no such
CPU-backed realization is actually available.

---

### 6.x.5 Backing form by provider and platform, as observed

Recorded because a plausible inference about this was wrong, and the wrong
version is the one anybody would reach for again.

**Only SyntheticProvider has a GPU-backed path.** Both platform providers
report `ProducerBackingCapabilities{cpu=true, gpu=false, both=false}` --
`WinrtCameraProvider::stream_backing_capabilities` and the Camera2 equivalent.
Camera2's `PRIVATE` (opaque GPU) form is not implemented. So on hardware,
every CamBANG payload today is CPU-backed, and the renderer in use does not
change that.

**Backing form must be read from the retained result, not inferred from the
renderer setting.** An S20+ run against SyntheticProvider retained an RGBA
stream result reporting `payload_kind=2` (GPU_SURFACE) while `project.godot`
declared `gl_compatibility` for mobile. The cause was the harness: the Android
export path substituted `"mobile"` whenever no explicit `--rendering-method`
was given, so the handset ran Vulkan and had a RenderingDevice. That default is
fixed -- the export now follows what the project declares -- and an Android run
with no renderer argument reports `usesVulkan(): false`, `opengl3`,
`GodotGLRenderView`.

The guidance stands regardless of the cause. Anything that can rewrite project
settings before an export sits between the file a maintainer reads and the
renderer a device runs, so the retained result is the only reliable answer to
which backing form applied.

**ResultCapability is READY=0, CHEAP=1, EXPENSIVE=2, UNSUPPORTED=3.** Zero is
the BEST answer, not the absent one, and a GPU-backed stream's display view is
exactly the READY case. This is stated here because an earlier revision of this
section recorded, as a defect, that a GPU-backed stream "reported
`can_get_display_view() == 0` indefinitely and nothing ever displayed". There
was no such defect: the result was READY throughout. The caller -- scene 570 --
tested the value against 0 as though it meant unsupported, skipped a usable
display view, and returned from its per-frame block, which starved the panels
below it so one mis-skipped panel presented as three failing. See b22d261 and
the revert in 7f139d8.

Two things made that inversion durable, and both are worth avoiding. The
comparison used a bare `0` rather than the bound `CAPABILITY_UNSUPPORTED`
constant, so nothing at the call site said what the number meant. And because
only a GPU-backed stream reports READY for display, while the project pins
`gl_compatibility` where no GPU backing arises, it never fired in ordinary runs
-- it appeared only under a renderer with a RenderingDevice, which made it look
like a platform-specific fault in Core or the provider.

The display-demand mechanism itself is sound. Demand is established by calling
`get_display_view()`, and the returned texture holds a persistent demand token
for as long as the caller keeps it, so uploads continue for a bound panel; a
demand trace under `--rendering-method mobile` shows demand going active once
and never lapsing.

Planar formats are excluded from GPU backing on both the stream and capture
sides (`query_stream_producer_capabilities_`,
`query_capture_producer_capabilities_`) because that backing is RGBA8-only, so
a planar Synthetic stream takes the CPU path and is unaffected by the above.

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

Resource-aggregate telemetry must preserve an outstanding create across an
observation/reset boundary until its matching release is observed. Clearing
telemetry may retire only balanced buckets; it must not erase evidence needed
to account for a later release.

### 7.6 Adapter-layer ownership transfer

Display wrappers/adapters (for example Godot-side `Texture2DRD`) are
adapter-layer display realizations, not the retained-primary payload-kind seam.
Godot display adapter ownership belongs in the Godot layer, not in Core,
platform providers, or public result APIs.

The internal `godot_gpu_display_service` is the Godot-side GPU display adapter
resolution boundary. It is currently non-owning: it does not cache or retain
`Texture2D` refs, Godot RIDs, or backend-native handles. In the present
synthetic path it delegates the legacy retained GPU backing / primary backing
artifact to the synthetic deferred-wrapper bridge. `RetainedGpuBackingDescriptor`
remains the provider-neutral scalar display/backing metadata seam; descriptor-only
and platform-backed display resolution are future activation points. A
`backing_id` identifies a retained backing resource, not the image/frame currently
represented by that resource. A value of `0` is compatibility metadata only and
must not be treated as valid descriptor-cache identity.

Core assigns a separate non-zero retained-frame identity after successful
retention. Identity `0` means no retained frame. Identities are issued from one
monotonically increasing Core-owned sequence for the lifetime of the
`CoreResultStore` instance; ordinary result clearing and runtime stop/start do
not reset or reuse that sequence. Every retained representation of the same
frame shares its identity. A newly retained frame receives a new identity even
when it reuses the same mutable GPU backing and therefore the same `backing_id`.
Acquisition timing is not a substitute for either identity.

Platform providers must not expose Godot `Texture2D` refs, Godot RIDs, or
backend-native public handles through the provider contract.

When GPU artifact ownership (for example RID ownership) is transferred into an
adapter object, retained-backing cleanup must not also free that same resource.

For repeating-stream retained GPU display state, release timing must remain
deterministic at stream-lifetime boundaries (for example stop/destroy/provider
teardown/reconfiguration invalidation), with adapter-layer deferral limited to
narrow fallback cases where immediate release is not possible.

Synthetic provider-native `GpuBacking` lifecycle facts describe the
provider-owned live stream backing handle: create on realization/recreation and
destroy on downgrade/replacement/release. Godot RIDs, `Texture2DRD` wrappers,
and retained-result artifacts are adapter/retention-layer resources and are
accounted separately; their existence must not fabricate an additional
provider-native `GpuBacking` lifecycle.

At `CamBANGServer.stop()`, live GPU display wrappers are detached while still
addressable and both CPU/GPU render-work queues are fenced and drained before
stop returns. A wrapper retained by user code after stop is inert and owns no
thread-affine `Texture2DRD` delegate. Extension uninstall closes admission only
after accepted producers and callbacks are quiescent.

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

For retained synthetic-stream `GPU_SURFACE`, `get_display_view()` now includes a
direct GPU display path via the display adapter path; this path must not be
reframed as CPU materialization.

At the user-facing contract level, `StreamResult.get_display_view()` is the
consistent **display-oriented live view** path across supported stream backing
kinds (including CPU-backed and GPU-backed paths), even when the internal
realization path differs by backing/provider.

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

A **Capture Result** is the device-level still-capture result. It has a default image. When no bracketing is involved, the default image is the only image. When bracketing is involved, additional bracket images may be represented within the same Capture Result.

### 10.3.1 Retrieval / assembly success gate

Device-level `CaptureResult` retrieval is successful only after assembly success.

Assembly success requires both:

- retained default image member `0`
- terminal capture lifecycle success (`capture_completed`)

`capture_failed` prevents successful `CaptureResult` retrieval even if a default
image payload was retained.

The following do not produce a successful retrievable `CaptureResult`:

- `capture_completed` without a retained default image member
- retained default image member without terminal completion

Partial additional-member success is allowed: when member `0` is retained and
capture terminal is `capture_completed`, retrieval succeeds and includes only
the retained contiguous additional-member prefix.

A Capture Result is **not** required to already be:

- a CPU `Image`
- a display-ready RGB pixel buffer
- an encoded artifact

A retained image under a Capture Result may use one or more retained or materializable representations, such as:

- encoded still artifact
- raw still artifact
- GPU/native surface
- CPU planar payload
- CPU packed payload
- derived forms requested or retained by policy

A Capture Result is structurally homogeneous across its bracket images. Shared result-level truth must not be duplicated independently for every bracket image. Per-bracket-image truth is limited to facts that genuinely vary between bracket images (for example, ordering/identity within the result, Image Acquisition Timing, exposure/capture attributes, retained backing resource instance, release state, and materialization state). Backing resource instances may vary per bracket image; structural backing kind/policy belongs to the homogeneous Capture Result unless a separate documented result shape explicitly permits otherwise.

For the current still-capture bracket tranche:

- member `0` is required and is the default metered member identity;
- additional bracket members are optional members under the same Capture Result;
- retained additional members must remain a contiguous ordered prefix (`1..K`);
- malformed sparse/gapped/duplicate/non-sequential additional output is rejected
  rather than normalized into sparse member retention;
- missing intended additional members are represented by absence, not fabricated
  members and not public per-member failure objects.

Provider-reported exposure truth is preserved at the per-member boundary:

- `applied_exposure_compensation_milli_ev` is provider execution truth;
- realized exposure truth is represented by
  `has_realized_exposure_compensation_milli_ev` +
  `realized_exposure_compensation_milli_ev`;
- unknown realized exposure uses `has_realized_exposure_compensation_milli_ev=false`
  (no sentinel numeric value);
- `realized_exposure_compensation_milli_ev != applied_exposure_compensation_milli_ev`
  remains representable when `has_realized_exposure_compensation_milli_ev=true`.

## 10.4 Capture Result Set semantics

A **Capture Result Set** is the rig/Core-curated grouping of selected device Capture Results for a rig-triggered synchronised capture. Capture Result Set curation is distinct from the definition of Capture Result.

## 10.5 Preferred capture retention direction

Preferred retained form order is roughly:

1. already-encoded still artifact when save/export is the natural path
2. high-fidelity CPU-readable artifact when processing is the expected path
3. truthful native/GPU artifact when readback can remain explicit
4. additional derived forms only when requested or policy-configured

Again, this is a preference order, not a correctness rule.

---

## 10.6 Initial Godot-facing result-object surface guardrails

To keep early implementation aligned with this contract, the initial Godot-facing
result-object surface is intentionally constrained as follows.

### 10.6.1 Stream Result initial surface

Public Godot stream observation uses `CamBANGStream.get_result()` for the current
observable `StreamResult`. Explicit `stream_id` lookup remains
advanced/dev/scenario tooling via
`CamBANGServer.get_stream_result_by_stream_id(stream_id)`.

Direct descriptive fields:

- `width`
- `height`
- `format`
- `payload_kind`
- `camera_facts.acquisition_timing` when present
- `stream_id`
- `device_instance_id`
- `intent`

Capability checks:

- `can_get_display_view()`
- `can_to_image()`

Explicit operations:

- `get_display_view()`
- `to_image()`

Non-goals:

- no encoded-byte access
- no filesystem save operations
- no stream history/sequence access
- no backend-native public handles

### 10.6.2 Capture Result initial surface

Public Godot device capture uses `CamBANGDevice.trigger_capture() -> Error` and
polls `CamBANGDevice.get_result()` for the current completed `CaptureResult`.
Explicit `capture_id` lookup remains advanced/dev/scenario tooling via
`CamBANGServer.get_capture_result_by_id(capture_id, device_instance_id)`.

Direct scalar/default-image convenience fields:

- `width`
- `height`
- `format`
- `payload_kind`
- `device_instance_id`
- `capture_id`

Scalar/default-image convenience access describes member `0` where per-member
image truth can vary. Structural homogeneous properties such as `width`,
`height`, `format`, and `payload_kind` are result-level truth.

Image-member access:

- `IMAGE_ROLE_DEFAULT_METERED`
- `IMAGE_ROLE_ADDITIONAL_BRACKET`
- `get_image_count()`
- `has_additional_images()`
- `get_image_member(index)`
- `can_to_image_member(index)`
- `to_image_member(index)`

`get_image_member(index)` returns metadata for the selected retained member,
including applied and realized exposure truth. Invalid/out-of-range access
returns an empty `Dictionary`.

For a completed member with resolved camera facts, that dictionary also has an
optional `camera_facts` dictionary. Classification entries (`facing`,
`camera_nature`, and `sensor_orientation_degrees`) are `{ "value": ..., "origin": ... }`.
`intrinsics`, `distortion`, and `pose` are complete atomic dictionaries which
each contain `origin`; they are never flattened or merged. Absent facts are
omitted, and `camera_facts` is omitted when all resolved camera facts are
absent. `origin` describes provenance, not the internal resolution authority.

Per-member image facts are optional entries in that same dictionary:

- `acquisition_timing` contains `origin`, `acquisition_mark`,
  `tick_period_numerator_ns`, `tick_period_denominator`, `clock_domain`,
  `reference_event`, and `comparability`;
- `focus_state` contains `origin`, `state`, and `distance_m` only for the
  `at_distance` state;
- `exposure_time` contains `origin` and `nanoseconds`;
- `sensor_sensitivity_iso` contains `origin` and `iso_equivalent`;
- `aperture_f_number` contains `origin` and `f_number`;
- `focal_length_mm` contains `origin` and `millimetres`;
- `realized_image_transform` contains `origin`, `rotation_degrees`,
  `mirrored`, and `pixels_already_transformed`.

`acquisition_timing` and `realized_image_transform` are provider-owned. The
other five resolve through the same external-description precedence as
intrinsics/distortion/pose, because each is device-constant on some hardware
and per-capture on other hardware (see `camera_fact_model.md` §12.2.1).
`focal_length_mm` is physical lens metadata and is a separate fact from the
calibrated `intrinsics.focal_length_x_px`.

These remain exact per-member facts. In particular,
`acquisition_timing.acquisition_mark` is in its declared provider domain and
is not Capture Date-Time, capture-admission time, geolocation sample time, or
another member's mark. Godot exposes all three numeric timing components
directly as signed `int` values. The canonical acquisition mark is a
nonnegative signed 64-bit value; the tick-period numerator in nanoseconds and
denominator are positive signed 64-bit values.

`get_capture_datetime_unix_nanoseconds()` exposes the shared UTC
capture-admission instant. It is distinct from per-member Image Acquisition
Timing.
`has_geolocation()` and `get_geolocation()` expose the shared optional
capture-admission geolocation; an absent value yields `false` and an empty
dictionary. Present dictionaries use WGS 84 geodetic decimal degrees for
`latitude_degrees`/`longitude_degrees` and optional WGS 84 ellipsoidal
`altitude_meters` in metres.

`CamBANGServer.set_capture_geolocation(Dictionary)` configures that optional
context independently of camera facts. Empty clears; non-empty input is
transactionally validated as finite WGS 84 geodetic latitude/longitude within
their public ranges and optional finite WGS 84 ellipsoidal altitude. It affects
only future successful admissions, never retained results.

`get_image_count()` reports retained member count only and does not imply all
authored/intended members were retained. Missing additional intended members are
represented by absence, not sparse members and not public per-member failure
objects.

Existing default-image methods such as `to_image()` and `can_to_image()` remain
member-0 conveniences.

Non-goals:

- no filesystem save APIs
- no RAW processing/export APIs
- no backend-native public handles
- no StreamResult camera-fact or geolocation exposure

`can_get_encoded_bytes()` / `get_encoded_bytes()` may be bound as capability
probes, but currently report unsupported / empty. Encoded output requires a
supported `ENCODED_IMAGE` payload/result path; it is not enabled by setting a
FourCC-style format value alone.

### 10.6.3 Capture Result Set initial surface

Public Godot rig capture uses `CamBANGRig.trigger_capture() -> Error` and polls
`CamBANGRig.get_result()` for the current completed capture group, returned as
a plain `Array[CamBANGCaptureResult]` (no dedicated grouping/wrapper class).
Explicit `capture_id` result-set lookup remains advanced/dev/scenario tooling
via `CamBANGServer.get_capture_result_set_by_id(capture_id) -> Array[CamBANGCaptureResult]`.

An earlier revision of this surface introduced a dedicated `CaptureResultSet`
RefCounted wrapper class (`capture_id`, `size()`, `is_empty()`, `get_results()`,
`get_result_for_device(device_instance_id)`). It was removed: every one of
those is either a native `Array` method already (`size()`, `is_empty()`), a
per-element property already exposed by `CamBANGCaptureResult` itself
(`capture_id`), or a trivial client-side iteration
(`get_result_for_device`, which had no callers). Godot/GDScript-facing class
surface is treated as a cost, not a convenience, in this project -- prefer a
built-in `Array`/`Dictionary` over a bespoke wrapper class unless the wrapper
earns its keep with real behavior beyond what the built-in type already
provides.

## 11. Capability and cost-aware materialization

## 11.1 Core principle

Materialization is the process of obtaining a derived representation from a retained result artifact.

Materialization is explicit.

CamBANG must not assume that every retained result is already available in every desired form.

## 11.2 Capability/cost states

Result access/materialization paths should be classifiable in provider-agnostic terms.

Capability/cost reporting should be interpreted with backing asymmetry in mind.

In particular:

- a primary `GPU_SURFACE` may still require expensive CPU materialization
- a primary `CPU_PACKED` may still admit cheap display-oriented realization
- the existence of more than one backing for the same artifact does not change the
  single-primary-payload rule

`to_image()` remains explicit CPU materialization. Its reported capability is
Operation Support populated from the current Backing State and the implemented
access paths, not a per-call route-economics recalculation.

Recommended first-pass capability/cost states:

- `READY`
- `CHEAP`
- `EXPENSIVE`
- `UNSUPPORTED`

### Intended meaning

- `UNSUPPORTED`
  Structural support/availability truth: the representation cannot currently be
  obtained through a supported CamBANG path.

- `READY`
  Operation-specific direct retained-availability truth. `READY` does **not**
  mean merely “very cheap.” For the specific result operation being classified,
  the target representation is already retained and directly accessible without
  materialization work such as readback, conversion, decode, repack, or
  full-frame copy.

- `CHEAP`
  The representation is supported but not directly retained for this operation,
  and can be obtained without substantial additional work.

- `EXPENSIVE`
  The representation is supported but requires meaningful additional work such
  as readback, conversion, decode, or full-frame copy/repack.

Examples:

- retained stream GPU display backing may be `READY` for `get_display_view()`;
- retained CPU payload or a CPU sidecar for `to_image()` is not automatically
  `READY`; it may still be `CHEAP`;
- supported GPU materializer/readback paths are not `READY`; they are at best
  supported non-ready paths.

## 11.3 Naming discipline

A method/access path whose name sounds cheap should actually be cheap.

For example:

- display-view access should not secretly force full CPU readback
- encoded-byte access should not secretly re-encode raw pixels unless made explicit
- image materialization methods may legitimately be `EXPENSIVE`

## 11.4 Stream display-view semantics

For repeating streams, `get_display_view()` is a **display-oriented live view**
of the **current retained stream state**.

Where supported, this display path may be backed by **live GPU-backed stream
display state** that is:

- owned by the stream
- updated in place while the stream flows
- exposed as a display-oriented live view

This display-view path is intentionally **buffer-like**. It is not a promise of
frozen historical image identity for previously obtained stream-result objects.

User-facing semantic note: this live-view contract applies across supported
CPU-backed and GPU-backed stream paths. The contract is about a
**display-oriented live view** and does not claim identical internal realization
mechanics across backing kinds.

Consumer binding responsibility: a `display_view` is a runtime-backed display
object, not a detached materialized image artifact. Consumers that bind a
`display_view` into UI/display objects are responsible for dropping those
bindings before stopping/destroying the owning runtime or stream state.

The existence of a live GPU-backed display path for repeating streams does **not**
imply that still-capture results should retain or expose per-capture GPU artifacts
at the public result seam.

### 11.4.1 Stream display-demanded freshness policy (synthetic stream live GPU backing)

This policy clarifies **stream display-view freshness** for synthetic stream live
GPU backing. It does not redefine global GPU update behavior across all
providers/paths.

- Display-view freshness is tied to **display-oriented access**.
- Polling latest `StreamResult` state alone does not imply display demand and
  does not by itself require per-frame live GPU-backing refresh.
- For synthetic stream live GPU backing, no-display operation may legitimately
  avoid per-frame live GPU-backing updates. In current implementation this
  publishes those no-demand frames as CPU-primary with current CPU bytes.
- When display demand is active and GPU update succeeds, frames may publish as
  GPU-primary (`GPU_SURFACE`).
- When the effective plan requires GPU primary and both the current update and
  its permitted recreate/retry fail, that frame is not published. CamBANG must
  not relabel stale live GPU content as the current frame and must not silently
  publish CPU primary contrary to the effective plan.
- Payload kind may therefore vary across successive stream frames under
  display-demanded policy, while remaining truthful per retained frame.
- `get_display_view()` is the demand-establishing access path for stream
  display-view freshness.

This is a stream result/display policy clarification only. It does not change
AcquisitionSession architecture or lifecycle semantics.

## 11.5 Stream `to_image()` semantics

`to_image()` remains the explicit path for **materialization onto CPU-backed
storage**.

For stream results, `to_image()` materializes onto CPU-backed storage from the
**current retained stream state at the time of the call**.

This wording describes the storage class of the returned artifact. It does **not**
commit the contract to any particular hidden implementation mechanism used to
perform that materialization.

Display-view access and materialization onto CPU-backed storage are therefore
distinct paths:

- `get_display_view()` — display-oriented live view
- `to_image()` — explicit materialization onto CPU-backed storage

`get_display_view()` must not be reframed as implicit materialization onto
CPU-backed storage.

For synthetic stream GPU-primary/live-backing paths, `to_image()` must remain an
explicit materialization outcome and must not silently return stale content:

- use a current retained CPU sidecar payload when available (current
  synthetic `to_image()` path preference), or
- perform/require explicit supported materialization from a fresh source, or
- report unsupported/expensive rather than presenting stale image content as
  current materialization truth.

Timing evidence for stream `to_image()` is collected around this real Godot call
path because it is the real retained-result access seam. CPU-packed stream
results and GPU-primary results with a current retained CPU sidecar are expected
to calibrate within the cheap access class; GPU-only stream results without
materialization remain unsupported; GPU-only stream results with the Synthetic
GPU backing materializer remain expensive. Invalidation/renewal belongs to
live applied production-posture changes as described in 11.7.

## 11.6 Capture-result guardrail

Still-capture public result semantics remain distinct from repeating-stream
display-view semantics.

A Capture Result is the device-level still-capture result at the public result seam. Its still-capture backing and materialization behaviour must remain explicit and capture-result-specific; the stream-side live GPU-backed display model must not be generalized into a public model of retained or exposed per-capture GPU artifacts.

For a capture member that retains both a current CPU sidecar and a GPU primary
artifact, `to_image_member()` prefers the current CPU bytes. The GPU
materializer is used only when no current CPU sidecar is available. Capability
and access-cost evidence must describe the route actually selected.

### 11.6.1 Capture Compute Texture

A **Capture Compute Texture** is a GPU-resident, frozen texture of one
completed capture image member, obtainable for as long as that `CaptureResult`
is retained, so that a caller can run GPU compute over the captured image.

It is a capture-native concept. It is defined here, under the capture-result
guardrail, because the guardrail above is the rule it must satisfy: this is not
the stream display-view model extended to captures, and nothing in §11.4 or
§11.4.1 applies to it.

#### What it is not

- **Not a display view.** Its purpose is compute, not presentation. It carries
  no display-demand semantics, no freshness policy, no refresh, no staleness
  question, and no live-view contract. A caller that only wants to show a
  capture on screen does not need it.
- **Not a replacement for `to_image_member()`.** CPU access is unchanged and
  remains the path for saving, encoding, and pixel inspection. A Capture
  Compute Texture is additive; asking for one never removes or degrades CPU
  access to the same member.
- **Not a per-frame or per-tick object.** One capture image member has one
  Capture Compute Texture for the life of the retained result.

The distinction from stream display state is not a naming convention. A stream
display view is deliberately buffer-like and explicitly disclaims frozen
historical image identity (§11.4). A Capture Compute Texture asserts the
opposite: it is exactly the pixels of that member, frozen, for as long as the
result exists. Reasoning that transfers from one to the other is wrong in both
directions.

#### Why it exists

Two reasons, and the second is the one that must not be forgotten when the
first is unavailable:

1. When the source already delivers the captured image in GPU-resident form,
   handing that to a compute shader avoids a GPU-to-CPU-to-GPU round trip.
   That round trip is expensive everywhere and disproportionately expensive on
   mobile hardware.
2. When the source does not, a caller still needs a compute-usable texture,
   and should get one by the same route rather than reimplementing the upload
   per application. The cost differs; the availability of the capability does
   not.

#### Identity, immutability, and caching

A retained capture image member is immutable. Its Capture Compute Texture is
therefore safe to produce once and retain alongside the member: the pixels
cannot change underneath it, so a retained texture can never be stale.

A repeat request for the same retained member must be served from the texture
already produced, not materialized again. The source pixels are frozen, so a
second production is necessarily identical to the first and is therefore pure
waste -- and a caller polling for its result must be able to ask for the
compute texture each time without paying a full-frame upload each time.

CamBANG may bound how many produced textures it holds, and a request whose
texture has already been released under that bound legitimately produces again.
What this forbids is producing afresh on every request while the previous
result was still held.

This is the reverse of the stream case, where retained display state is updated
in place while the stream flows and caching a materialized artifact would be
wrong. The conclusion here is drawn from capture immutability, not imported
from stream policy.

#### Operation Support

Capture Compute Texture availability is expressed as Operation Support
(§6.x.3), using `ResultCapability`, per image member -- alongside
`display_view`, `to_image`, and `encoded_bytes`, not folded into any of them.

Provisional classification follows from Backing State:

| Backing State for that member | Operation Support |
| --- | --- |
| A compute-usable GPU texture is already retained | `READY` |
| No retained GPU texture; a CPU payload is retained; a GPU device exists | `EXPENSIVE` |
| A GPU backing is retained but reaching a compute-usable texture requires a real import step | `EXPENSIVE` |
| No GPU device is available to the runtime | `UNSUPPORTED` |
| Neither a retained GPU backing nor a CPU payload | `UNSUPPORTED` |

`EXPENSIVE` for the CPU-payload case is not a hedge. Producing the texture is a
full-frame upload, which is the worked example of `EXPENSIVE` in §11.2.
Reporting `UNSUPPORTED` there would be false -- the caller can have the
texture, it simply costs -- and reporting `CHEAP` or `READY` would breach §11.3,
because the method would be hiding a full-frame copy behind a cheap-sounding
name.

As with every other supported non-ready operation, bounded calibration may
refine a supported non-ready classification to `CHEAP` from measured evidence.
No path may be *declared* `CHEAP` without it.

#### No eager materialization

A Capture Compute Texture is produced on first request and not before.

The reason is capture-specific. Capture results are retained per capture
identity under a byte budget with eviction, so eager materialization scales GPU
memory with retention depth: N retained results means N textures, most of which
no caller ever samples. There is no comparable pressure where a single live
artifact is retained. A caller that wants the cost paid earlier can ask
earlier; CamBANG must not decide that on their behalf.

Where a texture is produced, its footprint is counted in the same retained-byte
accounting as the member's other backings, so eviction sees it.

#### Obligations on the texture itself

A Capture Compute Texture is only a Capture Compute Texture if a caller can
actually compute over it:

- The caller must be able to obtain a **RenderingDevice texture RID** by one
  documented route that does not vary with which internal path produced the
  texture. A capability whose access method depends on unstated internals is
  not a capability.
- The texture must have a pixel format the caller can reason about. A GPU
  resource that can only be imported under a vendor-defined external format is
  **not** a Capture Compute Texture: such an image is sampled-only, requires an
  immutable sampler with a format conversion, and cannot be bound as a storage
  image. If a native backing can only be reached that way, the honest answer
  for that member is that the native path did not produce a Capture Compute
  Texture -- fall back or report accordingly, rather than handing over
  something the stated purpose cannot use.
- Geometry and format must agree with the member's other truth. A Capture
  Compute Texture that disagrees with `get_image_member()` about size, or with
  the retained payload about colour interpretation, is a defect, not a variant.

#### Lifetime and release

The texture's lifetime is bounded by the retained result. It is released when
the last of the retained member and any caller-held reference is dropped, and
never before either.

RID release follows the existing render-thread discipline: creation and release
of rendering resources are marshaled to the render thread, and `free_rid()` is
never called from an arbitrary thread.

A caller that binds a Capture Compute Texture into its own rendering or compute
work is responsible for dropping that binding before CamBANG is stopped, on the
same terms as any other runtime-backed display object.

#### Current implementation status

Recorded so a declared-but-unimplemented capability stays distinguishable from
a working one. This section describes what is built, not what CamBANG intends;
an absent entry means "not implemented yet", never "excluded by design".

- **The CPU-upload path is implemented; the native GPU-resident path is not
  reachable on real hardware.** `CamBANGCaptureResult` exposes
  `can_get_compute_texture()`, `can_get_compute_texture_member(i)`,
  `get_compute_texture()` and `get_compute_texture_member(i)`. Production is
  lazy and cached per retained member in the Godot layer
  (`src/godot/capture_compute_texture.cpp`).
- The cache is bounded by entry count and deliberately has **no** coupling to
  Core's capture eviction. It cannot be: the texture is a Godot-layer object,
  and Core must not own Godot display adapters. Dropping an entry is safe
  because a caller holding the returned reference keeps the texture alive
  independently -- eviction costs a later re-upload, never a dangling texture.
  This narrows the "footprint is counted in the same retained-byte accounting"
  intent above: the upload's footprint is visible in the
  `capture_compute_textures` diagnostic, and is **not** an input to Core's
  capture byte budget.
- Verified in scene 74 (`74_capture_compute_texture_verify`), which dispatches a
  real compute shader over the texture and cross-checks the result against a
  CPU sum of the same member. Under the mobile renderer: support EXPENSIVE,
  `ImageTexture` produced, resolved through
  `RenderingServer.texture_get_rd_texture()`, repeat access served from cache
  without a second upload, 921600 of 1280x720 pixels covered, and compute
  content matching the CPU reference exactly (117539567 both sides). Headless
  under Compatibility the same scene verdicts `expected_unsupported` with
  support UNSUPPORTED and nothing produced.
- The READY row is implemented and exercised, but only from Synthetic. Scene 74
  run with `--cambang-synth-producer-output-form=gpu_only` under the mobile
  renderer reports support READY, returns the GPU wrapper class rather than an
  `ImageTexture`, and records **zero** uploads -- the already-GPU-resident
  backing is wrapped and no pixels move. That is the zero-copy premise
  demonstrated, on Synthetic only; both platform providers still declare
  CPU-only capture backing, so no real device can reach this row.
- What the content cross-check proves differs by row, and the scene does not
  distinguish the two. On the EXPENSIVE row the compute texture and the
  `to_image()` reference derive from the same retained CPU payload, so a match
  proves the upload was faithful. On the READY row the compute texture is the
  retained GPU backing while `to_image()` is satisfied from whatever CPU route
  is available for that member, so a match proves those two agree. Which CPU
  route served it there has not been checked.
- Core already retains a per-member GPU backing handle and a neutral descriptor
  for it (`CoreCaptureResultData::ImageMemberData::retained_gpu_backing` and
  `retained_gpu_backing_descriptor`, `src/core/core_result_store.h`), and
  already counts its footprint in the capture byte budget
  (`effective_member_bytes`, `src/core/core_result_store.cpp`). Today that
  backing's only use at the result seam is as a readback source for
  `to_image_member()` when no CPU sidecar is current
  (`src/godot/cambang_capture_result.cpp`).
- A GPU-primary capture posture is reachable only from `SyntheticProvider`.
  Both platform providers declare no GPU capture backing capability
  (`capture_backing_capabilities` returns CPU-only in
  `src/imaging/platform/android/camera2_camera_provider.cpp` and
  `src/imaging/platform/windows/winrt_camera_provider.cpp`), so Core's capture
  Backing Plan evaluation cannot select a GPU posture on real hardware. The
  `EXPENSIVE` CPU-upload row of the table above is therefore the only row
  reachable on any currently supported device.
- The descriptor can distinguish a linear backing from an opaque external one
  and can declare that display or import costs real work
  (`GpuBackingLayoutKind`, `display_requires_import`,
  `src/imaging/api/provider_contract_datatypes.h`). No producer sets the opaque
  form yet.
- **The RD-RID route is settled.** `Texture2D.get_rid()` returns a
  RenderingServer texture RID on every CamBANG-provided display object, and the
  single route to the underlying RenderingDevice texture is
  `RenderingServer.texture_get_rd_texture(tex.get_rid())`. A Capture Compute
  Texture accessor is expected to satisfy the same route rather than introduce
  a second one.

  This previously did not hold. `DeferredDisplayTexture2DRD` returned the
  RenderingDevice RID from `_get_rid()` -- a different RID space -- which
  drawing never noticed, because its `_draw*` overrides delegate to the
  `Texture2DRD` and never resolve a RID. It now returns the delegate's
  RenderingServer RID (`src/godot/synthetic_gpu_backing_bridge.cpp`). The
  CPU-backed wrapper was never affected: it creates its texture with
  `RenderingServer::texture_2d_create()`, so its RID was already in the right
  space (`src/godot/cambang_stream_result_internal.cpp`).

  Verified in scene 70 (`_verify_display_view_rid_route`), which asserts the
  route on whichever wrapper the run produces and reports the class it saw.
  Observed: `DeferredDisplayTexture2DRD` and `LiveCpuDisplayTexture2D` both
  resolve to distinct, valid RD RIDs under the mobile renderer, and under
  Compatibility the absence of a RenderingDevice is reported rather than
  treated as a failure. With the fix reverted the same assertion fails with
  `rd_rid=0`.

## 11.7 Access-cost evidence guardrail

Real access-cost evidence exists to inform actual retained-result
Operation Support classification for a supported access path. It does not
change snapshot truth, structural support/availability truth, direct retained
target-representation availability truth, or the underlying Backing State.

Provisional result-facing classification starts from structural Operation
Support. `UNSUPPORTED` remains structural support/availability truth. `READY`
remains operation-specific direct retained target-representation availability
truth. Measured evidence is used only to refine supported non-ready paths,
primarily the split between `CHEAP` and `EXPENSIVE`.

Evidence must be tied to the current live applied Backing Plan and the realized
Backing State/access path. It must not be treated as a generic property of a
method name, and it must not be treated as repeating-stream-only evidence.

When a new live applied stream or still-capture Backing Plan changes the
realized Backing State/access domain, prior evidence for that domain is stale.
Renewal should be launched from that live-acceptance boundary rather than from
first user-visible `to_image()` demand.

Evidence gathering must remain at the real result-access seam. It must not be
interpreted as provider-local generation/staging cost, snapshot publication
cost, later render-thread draw/UI scheduling cost, or unrelated GPU
upload/update cost.

The implemented calibration policy is intentionally simple and relative, not a
benchmarking subsystem. For supported non-ready candidates, it compares
normalized measured cost using a single explicit CamBANG-wide multiplier
constant, currently `kResultAccessCheapWithinBestMultiplier = 2`.

That multiplier is a documented policy constant, not a hidden threshold or a
benchmark-tuning subsystem.

Single-candidate supported non-ready paths retain their provisional
non-ready classification after calibration rather than being automatically
promoted or demoted merely because they are alone.

### 11.7.1 The evidence channel is success-only

Backing Plan evaluation decides against two inputs, and they are not the same
kind of thing:

1. **The candidate set** comes from the provider's declared
   `ProducerBackingCapabilities`. It is an assertion by the provider, filtered
   through `viable(posture)`. Nothing measures it.
2. **The ranking between candidates** comes from access-cost observations —
   `to_image` classification, materialization elapsed time, normalized cost
   units — produced by retained-result access calibration.

Calibration only ever runs on a result that was **delivered**. A capture that
produces no payload produces no observation of any kind: there is nothing to
materialize, nothing to time, and nothing to classify.

**Evaluation can therefore learn that a candidate is expensive, but never that
it is broken.** A posture that reliably fails to deliver is silent rather than
costly, and silence is indistinguishable from "not yet measured". Evaluation
cannot probe its way off a non-functional candidate, because the failure
generates no evidence to move against.

This is a live gap, not a resolved design point. It does not arise on a
provider advertising a single viable posture — `ordered_count == 1` settles the
plan before any capture runs, so no evidence is consulted at all (Camera2 is
such a provider today: CPU-backed only). It becomes reachable on any provider
advertising two or more viable postures where one of them does not work.

Two consequences worth holding in mind when that case arrives:

- A provider must not declare a posture viable that it cannot actually deliver
  through. Declaration is the only gate; there is no measurement behind it.
- Any future remedy has to introduce negative evidence deliberately — a
  delivery failure attributed to a posture — rather than expecting the existing
  cost-observation channel to carry it.

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

Current flattened groups:

- `ImageProperties`

`ImageProperties.width/height/format` are derived from (not independently
resolved from) the same top-level scalar fields `get_width()`/`get_height()`/
`get_format()` read, so the flattened copy and the direct accessor cannot
structurally drift; the flattened group's value over the scalars is carrying
per-field provenance.

Location, optical-calibration, focus, and capture-device-state truth
deliberately do NOT use flattened result groups:

- capture-associated location is exposed from the Core-owned
  capture-admission context (`get_geolocation()`);
- optical calibration, pose, and realized optical/exposure state are exposed
  through the resolved per-member camera facts
  (`get_image_member(i)["camera_facts"]` — intrinsics/distortion/pose plus
  focus_state/exposure_time/sensor_sensitivity_iso/aperture_f_number/
  focal_length_mm, each with per-fact origin).

Earlier flattened `LocationAttributes`, `OpticalCalibration`, and
`CaptureAttributes` groups were writer-less duplicates and were removed. The
five quantities `CaptureAttributes` covered were subsequently restored into
the per-member camera-facts model proper, which is the correct home for them;
they must not be reintroduced as a flattened group.

## 13.1 ImageProperties

Structural image facts such as:

- width
- height
- format
- orientation
- bit depth
- crop state where meaningful

These properties should be populated from authoritative realized frame metadata,
not inferred from incidental CPU payload presence.

---

## 14. Provenance model

Provenance is attached per fact (or per small fact group), not as one result-wide blanket classification.

Recommended first-pass provenance vocabulary:

- `HARDWARE_REPORTED` — reported directly by platform/hardware/provider API metadata.
- `PROVIDER_DERIVED` — derived by the provider from backend/platform data before handing truth to Core.
- `RUNTIME_INJECTED` — supplied by CamBANG runtime/Core because it is runtime context rather than image-origin metadata.
- `USER_DEFAULT` — supplied from user/application default configuration when no more specific fact is available.
- `USER_OVERRIDE` — supplied from explicit user/application correction or override.
- `UNKNOWN` — no authoritative source for the fact is currently known or retained.

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
- whether a current observable result contains attached location
- whether current retained result fact provenance is mixed
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
- Stream paths are current-retained-result and latency/display oriented.
- Capture paths are discrete-result and fidelity/persistence oriented.
- Result Objects expose capability/cost-aware access and explicit materialization.
- Rich image-associated facts remain attached to results rather than bloating the hot snapshot path.
- Original image truth is preserved, while derived materializations remain explicit.

---


---

## 7.x Still-capture image-member contract clarification

For still capture, `CaptureResult` is image-member based.

- The canonical minimum valid image-member bundle is one member: index `0`, role
  `DEFAULT_METERED`, with exposure-compensation baseline `0` milli-eV.
- Bracketed still capture uses the same image-member model with additional members at
  indices `1..N`, role `ADDITIONAL_BRACKET`.

This means a one-image capture is the minimum valid still image bundle (ordered
image-member bundle for one still event), not a separate legacy path.

For public still-profile authoring at the Godot boundary, the profile Dictionary
key is `still_image_bundle` (not `image_sequence`) to avoid implying a
video/time sequence.


Exposure-compensation semantics for still image members are intentionally split:

- `intended_exposure_compensation_milli_ev`: authored profile/bundle member intent.
- `applied_exposure_compensation_milli_ev`: execution/result member identity instruction.
- `has_realized_exposure_compensation_milli_ev` + `realized_exposure_compensation_milli_ev`: provider-observed effective truth when known (presence-flag based; no sentinel).

Case A (unsupported bundle shape/member count) remains admission/validation failure; Case B (executed member with differing effective exposure) is represented by applied vs realized divergence when realized is present.

`CaptureResultSet` remains a rig/Core curation container for grouping device
`CaptureResult` objects and must not be treated as the container for bracket
members of a single device capture result.

- Snapshot still-capture profile shape is nested at `capture_profile.still`; realized per-image metadata remains on `CaptureResult.image_member`.
