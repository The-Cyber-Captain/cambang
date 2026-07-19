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

## 5. Capture execution model

* Hard-bound both concurrent capture workers and queued device jobs;
  saturation is an admission failure (`ERR_BUSY`), never hidden queue growth.
* Grouped (rig) submission admission is atomic: admit every device job or
  none; a rejected submission must emit no started/frame/terminal facts.
* Contain worker exceptions; convert them to the failed terminal fact.
* Cancellation on shutdown/restart is generation-based: workers observe a
  closed generation at their next checkpoint and terminalize as failed with
  `ERR_SHUTTING_DOWN` rather than delivering into a dead session.

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

## 8. Camera facts and Image Acquisition Timing

Per `camera_fact_model.md`: static facts key by opened device identity;
per-image facts (other than acquisition timing) post through the callback
path with exact capture ID, device instance ID, and member index; fact
replacement is transactional (malformed input mutates nothing; omissions
stay absent — omit anything you cannot supply truthfully).

Image Acquisition Timing rides on the delivered frame only: a semantically
valid mark/tick-period/clock-domain/reference-event/comparability/origin
record in *your* clock domain. Zero is a valid mark and distinct from
absence. Never substitute admission time, Capture Date-Time, lifecycle
timing, geolocation time, or another member's timing. Core never uses your
timing for identity, ordering, freshness, or correlation.

## 9. The concurrency admission gate

Core fail-closes multi-device rig capture unless a camera-concurrency truth
naming the exact device combination was ingested
(`CamBANGServer.ingest_camera_description(...)`, ADC v2) before `start()`;
the public boundary reports that gap as `ERR_UNCONFIGURED`. Providers do not
implement this gate — but platform providers are the eventual source of
truthful concurrency capability descriptions, and must not admit grouped
captures their backend cannot actually run concurrently.

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

* `out/provider_compliance_verify.exe` — the executable contract (currently
  41 checks; the check source is the authoritative audit list).
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
