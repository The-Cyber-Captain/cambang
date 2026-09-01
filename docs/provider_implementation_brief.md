# CamBANG Provider Implementation Brief

**Status:** Canonical. This document is the implementer-facing provider
contract: a third party must be able to implement a platform-backed Provider
(`windows_winrt`, `android_camera2`, ...) — or re-implement the Synthetic
reference — from this brief plus the canonical documents it references. It
supersedes the retired provider compliance checklist; audit criteria now live
here and in the executable verifiers.

Authority and reading order:

1. `provider_architecture.md` — the provider/Core contract boundaries.
2. `core_runtime_model.md` — Core's runtime authority model.
3. `architecture/provider_strand_model.md` — serialized delivery rules.
4. `architecture/provider_state_machines.md` — valid lifecycle transitions.
5. `architecture/pixel_payload_and_result_contract.md` — payload/result truth.
6. `camera_fact_model.md` — source-neutral camera facts.
7. This brief — the consolidated implementer obligations.

When this brief and source/verifiers disagree, source and verifiers win;
report the mismatch.

---

## 1. What a Provider is

A Provider is an adapter from one imaging backend to the CamBANG contract.
Core owns lifecycle, ownership, publication, and result truth; the Provider
supplies facts and executes effective configuration it is given. A Provider
must never redefine lifecycle, defaulting, registry, snapshot, or timestamp
semantics to match its backend, and no single platform provider defines
provider behaviour — the contract, the reference providers
(`SyntheticProvider`, `StubProvider`), and `provider_compliance_verify` do.

Implementation surface: implement `ICameraProvider`
(`src/imaging/api/icamera_provider.h`) and deliver every Provider→Core fact
through `CBProviderStrand` (`src/imaging/api/provider_strand.h`) into
`IProviderCallbacks`. Core-issued synchronous services (native-id allocation,
monotonic now) are direct calls and must not be routed through the strand.

## 2. Threading contract

* **Entry points are core-thread-serialized.** Every mutating
  `ICameraProvider` call arrives from CamBANG's single core thread (public
  commands, rig submission, warm-expiry close, shutdown). Your internal locks
  exist for *your own* worker threads, not to referee concurrent Core calls.
  Do not rely on that serialization for check-then-act correctness anyway:
  the reference implementation holds its admission lock across such windows
  (see `SyntheticProvider::close_device`).
* **One serialized callback context.** All `IProviderCallbacks` invocations
  must come from a single serialized context (the strand's worker). CoreThread
  cannot recover your event order once two threads post concurrently.
* **Workers post; they never call Core services directly**, never invoke
  platform callbacks into Core synchronously, and never wait on core-thread,
  Godot-thread, or render-thread work from inside a callback.
* **Prompt and bounded — enforced.** Every `ICameraProvider` method executed
  on the core thread is a submission/control operation, not a drain.
  `trigger_capture` success means you accepted responsibility for later
  terminal reporting, not that pixels exist. Capability/template queries
  perform no backend I/O. The enforcement ladder if you violate this:
  * **2s** — the blocked public caller's cancellation window closes; once
    your call has begun, Core waits for its truthful completion.
  * **5s** — the liveness watchdog logs a stale-task diagnostic
    (`CoreRuntime::check_core_thread_liveness()`); maintainer/verifier builds
    abort here, which is how `core_thread_liveness_watchdog_verify` will fail
    your provider in development.
  * **15s** — *when a provider violates promptness in production*: the
    runtime latches a terminal failed state. Blocked callers are released
    with fallback statuses, all further commands are refused, and `stop()`
    abandons (detaches and deliberately leaks) the wedged core thread and
    your provider. In-process restart after this is unsupported. A stalled
    camera driver must therefore be handled *inside* your adapter (your own
    worker + timeout + failure fact), never by blocking the contract call.

## 3. Event classes, ordering, and loss

Per `architecture/provider_strand_model.md`:

* **Non-lossy:** lifecycle, native-object, error facts — and still-capture
  frames plus terminal capture facts (they are exact capture truth).
* **Lossy:** repeating stream frames only. Core may drop them under pressure;
  dropping frames must never cost a non-lossy event.
* **Capture ordering obligations** (FIFO within your serialized posting
  context): post `capture_started` before any member frame; post member
  frames with their exact `image_member_index` (Core assembly is
  index-keyed); post exactly one terminal `capture_completed`/
  `capture_failed` per admitted device capture, after all of that capture's
  member frames. Exactly one terminal fact per admitted device job on every
  path — worker failure and shutdown included.

## 4. Frame delivery and buffer ownership

`FrameView` release is **manual and exactly-once**: Core calls your
`release` hook (from the core thread or strand worker; it must be safe and
non-blocking) for every frame it consumes or drops. Never free a delivered
buffer on your own schedule, and never require a second release.

Zero-copy retention: populate `cpu_payload_owner`
(`shared_ptr<const vector<uint8_t>>`) with tightly-packed bytes
(`data == owner->data()`, stride == row bytes) and Core adopts your buffer
into retained results without copying. Anything else forces a full-frame
copy per retained frame. For repeating streams, use a bounded reusable
buffer pool with in-use flags cleared by the release hook (see
`SyntheticProvider::StreamState`); for still captures, buffers are retained
long-term by the result store, so fresh per-member allocations are correct —
avoid zero-filling storage you fully overwrite.

GPU-backed frames carry an opaque `primary_backing_artifact` plus a truthful
`RetainedGpuBackingDescriptor` (display/materialization availability must
match reality). See `architecture/pixel_payload_and_result_contract.md`.

**Declare only postures you can actually deliver through.** Your
`ProducerBackingCapabilities` is the *only* gate on Core's Backing Plan
candidate set — nothing measures it. Core's access-cost evidence is gathered by
calibrating results that were delivered, so a posture that fails to produce a
payload generates no evidence at all and Core cannot rank its way off it
(`pixel_payload_and_result_contract.md` §11.7.1). An over-declared posture is
therefore not a performance mistake that evaluation will correct; it is a dead
end evaluation is blind to.

## 5. Capture execution model

* Hard-bound both concurrent capture workers and queued device jobs;
  saturation is an admission failure (`ERR_BUSY`), never hidden queue growth.
* Grouped (rig) submission admission is atomic: admit every device job or
  none; a rejected submission must emit no started/frame/terminal facts.
* Contain worker exceptions; convert them to the failed terminal fact.
* Cancellation on shutdown/restart is generation-based: workers observe a
  closed generation at their next checkpoint and terminalize as failed with
  `ERR_SHUTTING_DOWN` rather than delivering into a dead session.

### 5.2 An abandoned capture's payloads are never a later capture's

Giving up on a capture does not cancel the backend's obligation to produce it.
A platform may deliver the payload later, and a stale payload wearing a fresh
capture's facts is indistinguishable from a correct result.

**An obligation only exists while the platform still has one.** Ask the backend
whether the submission is over rather than inferring it from elapsed time. On
Camera2 that is `onCaptureSequenceCompleted`/`Aborted`: once a sequence has
ended, a member that never arrived is simply short, and no payload is
outstanding for it.

Getting this wrong is worse than not accounting at all, because the error
compounds. A device that produces one image fewer than requested leaves a debt
that can never be paid; the next capture's own payload discharges it, which
leaves the next capture short, and the device delivers nothing again ever.
Measured on Quest 3 as `2 -> 0 -> 0 -> 0` across a four-capture rig run, with
submissions and arrivals differing by exactly one for the whole run. Settling on
sequence end instead restored `2 -> 0 -> 2 -> 2` — the one genuine loss, and
nothing more.

**Sequence end settles the debt; it does not end the wait.** These are separate
questions and conflating them costs images. Camera2 defines sequence completion
over results and failures, not buffers, and does not order the buffer callback
against it: on Galaxy S20+ camera 0 completion fires 7–13 ms *after* the
buffer's delivery callback has begun. A capture that stops waiting the instant
its sequence ends snapshots its collector mid-delivery, and the image lands in a
collector nobody reads — 21 of 30 captures lost, every loss with completion
falling between arrival and collection, none of the delivered ones. Camera 1 of
the same device fires them in the opposite order and lost nothing, as did Quest
3, where buffers preceded sequence end by 19–50 ms every time. **Callback
ordering is a per-camera property; do not generalise it from one device.**

So a capture that is short when its sequence ends waits out a bounded grace for
buffers already in flight, then settles. The grace costs nothing on a capture
that got everything it asked for, and does not change what is owed: a capture
that waits out its grace and is still short owes nothing, exactly as it would
have without one. Restoring the grace took S20+ camera 0 from 12/28 to 28/28
with no change to its request→exposure latency.

An earlier version of this section attributed that run to a still-only session
withholding a buffer until the next request pushed the pipeline. The sequence
trace does not support it: every payload arrives 3–9 ms after **its own**
submission, streamed or not. The withholding was an artefact of attributing each
capture's prompt payload to its predecessor.

The obligation:

* A payload delivered after its capture was abandoned **must not** be
  attributed to any later capture. Discard it, and count the discard.
* Attribution is **by accounting**: a capture abandoned with fewer payloads
  than expected leaves that many owed, and the next arrivals discharge the debt
  before any collector may claim one. Members the platform explicitly failed
  owe nothing — no payload is in flight for them.
* Attribution **must not** use acquisition marks. `camera_fact_model.md` §12.2
  forbids acquisition timing as identity, freshness, or ordering evidence, and
  marks may legitimately be identical across simultaneously triggered devices,
  so they cannot discriminate even in principle.
* Where an unmatched payload would otherwise be paired to metadata by position
  (the degenerate single-member case), that pairing is permitted only while the
  device owes nothing. Facts are enrichment and never gate pixels, so
  withholding them is the conservative outcome.
* Debt is per device and must not outlive it: settle or forgive it on close, or
  a reopened device inherits discards that are not its own.

`imaging/api/outstanding_payload_ledger.h` implements this accounting and is
the expected mechanism. A provider whose backend model makes late delivery
structurally impossible — one where each payload returns only to its own
awaiting call, with no shared queue a later capture could drain — may state
that instead, but must say so explicitly rather than leave the question open.

### 5.3 An admitted capture must not wait on the caller

Admission transfers ownership. From that point the provider owes a terminal
fact, and obtaining the payload is *its* problem: it must drive whatever
backend activity delivery requires, on its own initiative, within its declared
`capture_admission_watchdog_timeout_ns()`.

A provider must never depend on a later caller action — another capture, a
stream start, any external request — to obtain a payload it has already
accepted. Doing so makes correctness a property of caller behaviour: the
capture appears to work when captures happen to be frequent, and stalls
whenever the caller pauses. Worse, the payload then surfaces during a *later*
capture, which is how §5.2's misattribution begins.

Observed behaviour that motivates this, recorded as observation and not as
explanation: on Quest 3 / Camera2, a rig member with no stream delivers its
first capture after session configuration and then strands every subsequent
one — capture *result metadata* arrives on time while the payload does not, and
a stranded payload has been seen released 1.73 ms after the next capture's
submission, 51 seconds after its own.

**The mechanism is not yet established.** Result metadata arriving on time
means the backend did process the request, so explanations resting on an
inactive pipeline do not fit. Investigation continues; nothing in this section
prescribes a remedy, and no session shape, submission pattern, or keep-alive
scheme should be inferred from it.

Per-provider positions:

- **Synthetic** satisfies this inherently: payloads are produced and posted
  within the capture worker.
- **WinRT** satisfies it through `LowLagPhotoCapture::CaptureAsync()`, which
  returns each payload to its own awaiting call and needs no ongoing activity.
- **Camera2** does not currently satisfy it for devices with no user stream.
  This is an open defect, not a documented design.

A provider that cannot meet this obligation must fail the capture at admission
rather than accept work it cannot complete unaided.

### 5.1 Multi-image bundles: coherence outranks per-image quality

A multi-image still bundle exists to be *combined*. That makes it a different
design problem from a single capture, and the difference is easy to get
backwards.

The members of a bundle are only useful together if they describe the same
scene. Two things break that, and both are temporal:

* **Between members** — the interval separating them. Anything that moves
  (subject, or the camera in a hand) displaces between members, and no
  post-processing recovers the alignment.
* **Within a member** — its own exposure duration is the window over which
  its content is smeared. A member exposing far longer than its neighbours
  disagrees with them about *when* it saw the world, even if every member is
  perfectly aligned at its start.

So when a provider must trade image quality against temporal extent while
realizing a bundle, **temporal coherence takes precedence**. The governing
rule is that no member may be temporally wider than the default-metered
member: auto-exposure already judged that duration acceptable for the scene,
so it is a scene-derived bound rather than a chosen constant, and it means no
member is blurrier than the reference it brackets.

Concretely, for exposure bracketing: darken by shortening exposure, brighten
by raising sensitivity, and lengthen exposure beyond the metered duration only
when gain has reached its ceiling and the requested range is otherwise
unreachable. Sensor gain costs noise but takes no time; exposure costs time.

This is a deliberate trade, not a free win. Gain noise lands hardest on the
brightening member, which is the one carrying shadow detail. It is the right
default because blur is unrecoverable while noise is partially averaged out by
the combination itself — but for a static subject on a fixed mount the
opposite policy produces better images.

Providers implement this as a fixed internal policy for now: the public
request expresses an intended compensation only, with no way to state scene
context or a preference. A future version may let Core (and through it the
user) select the policy, or declare a handheld/fixed-mount context, at which
point today's behaviour becomes the handheld branch rather than the whole of
it. Do not invent a provider-local knob for this in the meantime.

Where the backend can execute a bundle as a single submission (a burst, or an
equivalent native sequence), prefer it: it removes the per-member round trip
entirely. Note the ordering dependency — a burst is only safe once each
member's exposure is fixed and independent, because back-to-back members leave
an auto-exposure algorithm no time to converge. Hold whatever else would drift
across the bundle, notably white balance and focus, and hold focus by locking
the algorithm where it settled rather than by switching the control mode
underneath a running stream.

Some backends can only hold focus while a repeating stream is running, because
their auto-focus converges over a continuous frame sequence and exposes its
state only on that stream's results. Where that is so, say so: a stream is a
legitimate documented precondition for a focus-locked bundle. Do not
manufacture a private frame source to work around it. A lock reported without
the evidence to support it is worse than an absent one — a bundle whose members
share one *unfocused* lens position looks internally consistent and will pass
inspection that a visibly failed capture would not.

## 6. Templates and the defaulting boundary

Expose deterministic `StreamTemplate`/`CaptureTemplate`. Core materializes
effective configuration from requests, your templates, and retained profile
state; you execute what you are given. Never invent width/height/format/
picture defaults inside `create_stream`/`start_stream`/`trigger_capture`;
if the effective config is invalid, fail deterministically.

## 7. Lifecycle and native-object truthfulness

* `close_device` fails while child streams exist; `destroy_stream` fails
  while started/producing. No auto-stop/auto-destroy cascades to satisfy a
  caller.
* Emit native-object created/destroyed facts only when the resource is
  actually acquired/released, for: Provider, Device, AcquisitionSession
  (when concretely realized), Stream, and lifetime-significant support
  resources (frame-buffer leases, GPU backings). Never fabricate destruction
  to tidy state; keep ownership relationships visible.
* Still-capture callbacks alone do not satisfy retained AcquisitionSession
  truth when no concrete session seam was realized.

### 7.1 The acquisition seam is reference-held, not incidental

An AcquisitionSession exists because something needs it, and it is retained
while that need lasts. Three independent claimants can hold one — a **stream**,
a **device capture**, and **priming** — and a provider must be able to say which
of them currently do.

Two consequences that are easy to get wrong:

* **Establish at admission, not on first use.** A capture that is admitted
  without an established seam is a capture whose seam is whatever an earlier,
  unrelated operation happened to leave behind. Failure to establish is an
  admission failure, and a grouped submission rolls back whole (§5). Realizing
  the *native* object may still be deferred to a bounded worker — session
  configuration is heavyweight and admission runs on the core thread — but the
  seam's lifetime is governed from the moment the capture is accepted.
* **The operation is creation when none is in place.** A retained-profile set
  and a capture on a device with no seam are both first-class reasons to create
  one; neither is a fallback for the other, and neither is an optimisation. The
  API name `sync_capture_parent_priming` predates this understanding and is
  kept for compatibility, but do not implement it as warming, and do not
  short-circuit it on "a session already exists". The seam must follow the
  retained capture shape: on the engage path it is necessarily created from
  your capture template, because the caller's retained still profile can only
  be applied after the device has an instance id. Re-create when the shape
  changes, and let the reference check refuse the rebuild if a stream or an
  in-flight capture holds the seam. It must be idempotent for equivalent
  requests.

  **A stream or an in-flight capture — not the capture parent.** The
  capture-parent claim must never refuse a reconfiguration, including one your
  backend can only perform by replacing the seam's native object. Where the two
  cannot be told apart, as on Camera2, keying the refusal on "is the object
  replaced" excludes the capture parent in theory and includes it in practice,
  and a retained still profile then blocks the geometry it exists to set. Key
  it on whether anything is put back instead: a replacement that reports its
  destroyed and created facts leaves Core knowing exactly where it stands,
  which an outright teardown does not.
* **Releasing a claim is not a request to destroy the seam.** Core holds the
  capture-parent claim for as long as the retained still profile stands, so the
  release that eventually arrives says the profile is changing or going away —
  not that the seam should be destroyed. Drop the claim and leave the seam.

References govern when teardown is **permitted**, not whether seam identity
survives a reconfiguration. Where a backend's session object is 1:1 with the
seam — Camera2's `ACameraCaptureSession` is — changing the output set really is
a new seam and must be reported as one, with a destroyed fact for the old and a
created fact for the new. Reporting continuity across a rebuild would be a
fabricated fact, and the rule against fabricating destruction cuts both ways.

Zero references makes teardown permitted, not mandatory. A provider may keep a
warm session to avoid paying reconfiguration on the next capture, but an
explicit release from Core is a statement of intent and must be honoured.

## 8. Camera facts and Image Acquisition Timing

Per `camera_fact_model.md`: static facts key by opened device identity;
per-image facts (other than acquisition timing) post through the callback
path with exact capture ID, device instance ID, and member index; fact
replacement is transactional (malformed input mutates nothing; omissions
stay absent — omit anything you cannot supply truthfully).

Five facts — focus state, exposure time, sensor sensitivity (ISO), aperture
f-number, and physical focal length — exist in both the static and per-image
containers, because each is device-constant on some hardware and per-capture
on other hardware. Post whichever tier your backend can actually evidence:
per-image when you read a realized value for that member, static when the
device reports a lifetime-constant value at open. Core resolves external
camera description > your per-image fact > your static fact, so a maintainer
can describe a value your platform cannot report. Two rules follow:

* **Absence is the correct report** when a control is unsupported. Hardware
  that exposes no exposure or focus API is common, and reporting nothing is
  truthful; do not treat "no control" as evidence the value is fixed, and do
  not restate a requested value as a realized one.
* **Never convert across unit systems you cannot evidence.** A normalized,
  hardware-dependent, or preset-valued reading is not a physical quantity.
  Post a fact only when your backend gives you the actual unit the fact is
  defined in (nanoseconds, ISO equivalent, f-number, millimetres, metres);
  otherwise omit it. Note `focal_length_mm` is lens metadata and is *not*
  interchangeable with the calibrated `intrinsics.focal_length_x_px`.
* **A readable control value is not automatically a realized one.** Many
  drivers return the control's *set-point* — what was last configured, or a
  default — rather than what the sensor actually did, and they do so even
  while the device is streaming and its own auto algorithms are running.
  Such a value is indistinguishable from a realized reading by inspection,
  and a plausible-looking default is the most dangerous case of all. Before
  publishing any realized fact sourced from a control read, prove the value
  tracks reality: vary the scene (cover the lens; change the subject
  distance), sample the value repeatedly, and confirm it moves. Measure
  something independent — mean frame brightness, image sharpness — over the
  same window, so a pinned value cannot be confused with a scene that never
  changed. A value that never moves is a set-point; omit it.

Image Acquisition Timing is frame-borne wherever your backend supplies it: a
semantically valid mark/tick-period/clock-domain/reference-event/
comparability/origin record in *your* clock domain. Zero is a valid mark and
distinct from absence. Never substitute admission time, Capture Date-Time,
lifecycle timing, geolocation time, or another member's timing.

Some capture paths carry no frame timestamp at all — a still-capture API may
hand you pixels with no time of its own even when the streaming path has one.
A provider-observed mark, taken as close to the capture as the API allows, is
truthful there provided you declare it honestly: `PROVIDER_OBSERVED` as the
reference event, and comparability no wider than you can actually support. If
that mark comes from a different clock than your stream frames use, or from
one you have not verified to be the same, say `SAME_IMAGE_ONLY` rather than
`SAME_DEVICE` — an unverified domain assumption is not a basis for a
cross-frame claim. Do not reach for a wall clock (EXIF `DateTimeOriginal` or
equivalent) to fill the gap: that is Capture Date-Time's quantity, it can
jump, and it is not a monotonic mark.

Core never uses your timing for identity, ordering, freshness, or
correlation.

## 9. The concurrency admission gate

Core fail-closes multi-device rig capture unless a camera-concurrency truth
naming the exact device combination was ingested
(`CamBANGServer.ingest_camera_description(...)`, ADC v2) before `start()`;
the public boundary reports that gap as `ERR_UNCONFIGURED`. Providers do not
implement this gate — but platform providers are the eventual source of
truthful concurrency capability descriptions, and must not admit grouped
captures their backend cannot actually run concurrently.

## 9A. Profile catalogs, and the three answers

`stream_profile_catalog(hardware_id)` and `capture_profile_catalog(hardware_id)`
report the configurations an endpoint advertises. They are keyed by hardware id,
not device instance, so they can be answered before the camera is opened -- a
caller chooses a configuration before opening anything.

Implementing them is OPTIONAL. Getting the answer wrong is not.

The return carries a three-state `availability`, and the distinction between the
first two states is the one that matters:

| Answer | Meaning | Consequence |
|---|---|---|
| `NOT_THIS_PROVIDER` | not an endpoint you own | no catalog; an ingested description may NOT stand in |
| `CANNOT_ENUMERATE` | yours, but you cannot list it | an ingested description MAY supply the catalog |
| `ENUMERATED` | yours, and `entries` is what it advertises | an empty list means it offers nothing |

`NOT_THIS_PROVIDER` is the default, so a provider that has not implemented these
yields no catalog rather than one for hardware that may not be there.

**Answer `NOT_THIS_PROVIDER` for any hardware id you do not own.** This is not a
formality. A camera description may supply a catalog for an endpoint you own but
cannot enumerate; if you answer `CANNOT_ENUMERATE` for an id that is not yours,
that description will be reported as the catalog of a camera that does not
exist. The two states look interchangeable and are not.

**Answer `CANNOT_ENUMERATE`, not an empty `ENUMERATED`, when you cannot list a
camera you own.** A backend that reads formats from an open camera cannot
enumerate while it is closed; that is truthful, and reporting an empty list
instead would claim the camera offers nothing.

**Advertise only configurations you will accept.** A caller hands an advertised
entry straight back to `create_stream()`. A catalog listing geometries the
backend then refuses is worse than no catalog, because the refusal surfaces far
from the advertisement that caused it.

`max_fps` per entry is optional: omit it rather than fabricating a rate, because
a stated zero reads as a measurement. It is a capability, never a request, and
must not be confused with the `target_fps` fields of a capture profile.

Core intersects an ingested description with your enumeration; you do not
implement that, and you never see the description.
## 10. Shutdown ordering

Reference order (see `SyntheticProvider::shutdown()`): mark shutting-down →
close capture admission and join workers (each in-flight job terminalizes
exactly once) → under the state lock: stop streams, destroy streams, close
devices, emit truthful native destruction, release backings → then, with no
locks held, flush and stop the strand. The broker closes call admission and
drains active calls before your `shutdown()` runs; post-close calls never
reach you. `stop_*`/`destroy_*`/`close_*`/`shutdown()` are never long
backend drains on the calling thread — move real drains to your workers
before acknowledging.

## 11. Synthetic-specific machinery (do not imitate)

These are implementation details of the deterministic reference, not
contract: the virtual clock and timeline scenarios; `advance()` and its
`flush_strand` split (host-stepped determinism vs. the free-running tick);
pattern rendering and its buffer-pool sizes; capture-worker counts;
timeline destructive-sequencing modes. A platform provider driven by real
hardware callbacks needs none of this — it needs the contract above.
Equally: a Synthetic shortcut (for example, same-thread frame production) is
never evidence that the contract permits it.

## 12. Compliance validation

A provider is not done until it passes, unmodified:

* `out/provider_compliance_verify.exe` — the executable contract. The check
  source is the authoritative audit list; do not rely on a count quoted here.
* `out/core_spine_smoke.exe`, `out/restart_boundary_verify.exe`,
  `out/verify_case_runner.exe --run-all` — lifecycle/restart/authored cases.
* `out/core_thread_liveness_watchdog_verify.exe` — both prompt/bounded
  enforcement modes.
* The Godot verification scenes for the public surface it feeds (65, 66, 73
  including its no-ingest negative phase, and the scene 870 soak), per
  `tests/cambang_gde/README.md`.

Hardware-backed providers add platform runtime validation
(`docs/dev/build_and_scaffolding.md` §11) as a supplement — never a
replacement — for these contract checks. Do not weaken a verifier to pass
it; a verifier failure is a contract finding.
