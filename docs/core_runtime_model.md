# CamBANG Core Loop Model

This document defines the **internal core model** of CamBANG core:
threading, event ordering, registries, snapshot publication,
warm/retention scheduling, and deterministic shutdown.

This document complements: - `provider_architecture.md` (core ↔ provider
boundary) - `state_snapshot.md` (public snapshot schema v1)

------------------------------------------------------------------------

## Canonical architecture authority

This document defines the runtime authority model for CamBANG core.

Related documents are subordinate in scope:

- `provider_architecture.md` defines the core ↔ provider contract
- `architecture/lifecycle_model.md` explains lifecycle structure and event flow
- `architecture/publication_model.md` explains Godot-visible snapshot publication

These supplementary documents may clarify behaviour, but they must not
redefine the runtime authority described here.

------------------------------------------------------------------------

## 1. Execution model overview

CamBANG core runs on a **dedicated core thread**.

All mutation of core-owned state occurs on that thread, including: -
arbitration state - device/stream/rig core state machines -
`CoreNativeObjectRegistry` - snapshot assembly/publish bookkeeping

Godot-facing objects never mutate core state directly; they enqueue
commands to core.

Compatibility synchronous wrappers that expose Core-owned truth or admission to
Godot-facing callers execute inline when already on the core thread; otherwise
they post to their existing CoreThread lane and wait only up to an internal
bounded timeout. Timeout or post rejection returns a conservative fallback to
the caller and does not imply cancellation of already admitted core work.

Provider callbacks never mutate core state directly; they enqueue
provider events to core (serialized callback context per provider
contract).

Providers may deliver frames originating from platform camera APIs or from synthetic sources.
Core is agnostic to pixel origin; it integrates frames solely via the provider/core contract and
published metadata.

------------------------------------------------------------------------

## 2. Thread topology

### 2.1 Threads

-   **Godot main thread**: runs the game, owns Godot objects.
-   **CamBANG core thread**: deterministic state machine + publisher
    thread.
-   **Provider internal threads**: platform required threads (e.g.,
    Android Looper) --- never call core concurrently.
-   **Provider callback context**: a single serialized context used to
    enqueue provider events into core.

### 2.2 Ownership rules

-   Core thread is the **sole writer** for core state.
-   Godot threads are **producers** of commands only.
-   Provider callback context is a **producer** of provider events only.
-   Godot reads snapshots via `CamBANGServer` without locking (immutable
    snapshots).

------------------------------------------------------------------------

## 3. Event loop model (A)

Core thread runs a **blocking event loop** using a condition variable
with a timed wait.

Core owns: - `cmd_queue` (Godot → Core) - `evt_queue` (Provider →
Core) - `timer_heap` (scheduled deadlines; min-heap)

### 3.1 Wake conditions

The core thread wakes when: - a command is enqueued (notify) - a
provider event is enqueued (notify) - the earliest scheduled timer
deadline expires (timed wake) - shutdown is requested (notify)

### 3.2 Deterministic processing order

While the active generation retains publishable canonical truth, Core processes
each normal loop turn in the following order:

1.  **Drain provider events** (bounded to
    `kMaxProviderFactsPerCoreTurn = 64` per core-loop turn when no
    commands are pending, or
    `kMaxProviderFactsBeforeRequestWhenRequestsPending = 1` when queued
    commands are waiting)
2.  **Drain commands** (bounded or full drain; v1 default is full drain)
3.  **Process due timers** (all deadlines ≤ now)
4.  **Publish snapshot if dirty** (exactly once per loop iteration)

This ordering is fixed to ensure: - "what happened" (provider events) is
integrated in deterministic FIFO slices before "what is requested" (commands) -
timers are honored deterministically - snapshot publication reflects the
current retained Core truth after the processed slice and command turn

Fatal transport containment is the exception to this normal loop ordering.
Once CoreThread observes fatal transport failure, it follows the truth-loss and
fatal-shutdown rules in §§3.5, 7.1, 9.2.a, and 10.2 instead of completing an
ordinary provider/command/timer/publication turn.

### 3.3 Batching and fairness

Core caps provider-event drains at `kMaxProviderFactsPerCoreTurn = 64` per
iteration when no Core commands are pending. If queued commands are already
waiting, core uses the smaller request-aware slice
`kMaxProviderFactsBeforeRequestWhenRequestsPending = 1` before servicing the
command queue. If provider events remain after either bounded slice, core
requests another loop turn and continues from the next queued event.
Provider-event FIFO ordering is preserved, events are not dropped by this
fairness slice, and pending Core commands receive prompt service opportunities
even under continuous provider-event production. This implementation detail
supports the documented capture-over-stream arbitration policy without changing
provider or Godot APIs.


### 3.4 CoreThread ingress lanes

CoreThread separates posted work into three internal lanes before invoking the
CoreRuntime loop hook:

1. **Essential facts** for non-lossy lifecycle/native/error/capture-terminal
   delivery.
2. **Command work** for Core-owned public/request admission.
3. **Ordinary work** for droppable or lower-priority posted work such as frame
   ingress transport.

Essential tasks drain before command tasks, and command tasks drain before
ordinary tasks. FIFO order is preserved within each lane. Ordinary work is
drained in single-task slices of `kMaxOrdinaryTasksPerCoreThreadTurn = 1`, so
command-lane work posted during ordinary provider/frame transport is re-observed
before another ordinary task can run. Timer ticks remain coalesced as a pending
flag; when command-lane work appears during a pump, CoreThread may defer one
requested timer tick once so command work gets a prompt service turn, then the
coalesced timer tick continues deterministically. While CoreRuntime is executing
a timer tick, provider-fact integration also checks for newly queued command-lane
work between facts and can yield the timer hook with a continuation request so
that command admission can be observed between bounded timer/provider-fact
slices.

Inside the CoreRuntime provider-fact queue, capture-critical facts are classified
separately from repeating stream frames. Repeating stream frames are
lower-priority/latest-state integration work: they can remain queued while
pending command/request work is admitted, and a capture-critical fact may pass
only a prefix of repeating stream-frame facts. Unknown, lifecycle,
native-object, error, and other non-lossy stream facts remain conservative; they
are not dropped or reordered behind stream frames by this integration rule.
Repeating stream frames may also be coalesced before expensive dispatch only
when a newer queued frame for the same stream/session supersedes the older frame
before any non-lossy barrier; stream received/dropped counters and
framebuffer lease telemetry are updated before the stale frame is released.
The stale frame is counted as received and dropped, not delivered: Core releases
it before sink invocation.
The same pre-sink accounting rule applies when active capture preemption
suppresses a repeating stream frame before sink handoff.

### 3.5 Bounded transport and fatal truth loss

Every CoreThread ingress lane is bounded.

Ordinary and command admission may fail according to their documented
request/backpressure contracts. Failure to admit an **essential provider fact**
is different: lifecycle, native-object, error, and accepted-capture facts are
part of the authoritative provider sequence.

If an essential provider fact cannot be admitted because of capacity or
allocation failure, Core latches a fatal transport failure for the active
generation.

The fatal signal uses a first-failure-wins, non-allocating path independent of
the saturated task queues. Once latched:

* all new ordinary, command, and essential admission closes;
* CoreThread wakes and observes the failure on the Core thread;
* provider-derived work may no longer mutate publishable canonical truth;
* normal command, ordinary, timer, and publication processing does not continue
  against an incomplete provider sequence;
* CoreRuntime begins deterministic containment and teardown.

CoreThread must enforce this boundary for work still in shared queues and for
work already moved into a local execution batch. Closing the shared queue alone
is not sufficient.

Where fatal containment requires some Core-owned work to remain executable,
CoreThread distinguishes teardown-critical work from provider-derived facts and
normal request work. This classification is internal and must not become a
second public scheduling model.


------------------------------------------------------------------------

## 4. Queues and message types

### 4.1 Command queue (Godot → Core)

Commands represent requests from Godot-facing objects, e.g.: -
engage/disengage device - set warm policy - create/destroy stream -
start/stop stream - set capture profile(s) - trigger device capture -
arm/disarm rig - trigger rig capture - apply spec patches
(`ApplyMode` handled by core)

Commands are immutable message objects; core owns command execution.

Current public Godot-facing trigger/result surface is object-oriented:
- device capture via `CamBANGDevice.trigger_capture() -> Error`, then
  `CamBANGDevice.get_result()` for the current completed `CaptureResult`
- rig capture via `CamBANGRig.trigger_capture() -> Error` (rig object obtained
  with `CamBANGServer.get_rig(rig_id)`), then `CamBANGRig.get_result()` for the
  current completed `CaptureResultSet`
- stream observation via `CamBANGStream.get_result()` for the current observable
  `StreamResult`
- advanced/dev/scenario result lookup by explicit IDs remains available as
  `CamBANGServer.get_capture_result_by_id(capture_id, device_instance_id)`,
  `CamBANGServer.get_capture_result_set_by_id(capture_id)`, and
  `CamBANGServer.get_stream_result_by_stream_id(stream_id)`
- no public singleton `CamBANGServer.trigger_rig_capture(...)` entry.

### 4.2 Provider event queue (Provider → Core)

Provider events represent facts observed by the provider, e.g.: - device
opened/closed confirmations - stream started/stopped confirmations -
frame arrival (stream intent or capture) - capture completion / capture
error - device/stream errors - native object created/destroyed
notifications

Provider events must be enqueued from the provider's serialized callback
context.

Scenario execution note (current): scenarios stage provider/world/topology/config
state. Triggered capture remains API/GDScript-driven and is not modeled as a
scenario timeline capture action.

Provider still-capture admission is distinct from payload production: Core waits
for provider acceptance/rejection, while accepted providers report later
started/frame/completed/failed facts through the provider event path. Rig capture
admission materializes all participants under one capture id and submits them as
one grouped provider submission where supported.

------------------------------------------------------------------------

## 5. Core state machines (high-level)

Core maintains explicit state machines for:

-   **Rig core state** (OFF/ARMED/TRIGGERING/COLLECTING/ERROR)
-   **Device core state** (IDLE/STREAMING/CAPTURING/ERROR + engaged
    bool)
-   **Stream core state** (STOPPED/FLOWING/STARVED/ERROR + intent)

AcquisitionSession seam is provider/native-object truth and is retained in
snapshot state via `acquisition_sessions`. Current implementation status in
`SyntheticProvider` includes both stream-backed realization and capture-only
realization. The provider retains a truthful `AcquisitionSession` seam while
stream and/or capture references exist, and does not require a transient public
`CamBANGStream` for capture-only truth.

State transitions occur only on the core thread and must be
deterministic.

Provider may request or force certain transitions via events (e.g.,
device error), but core is authoritative for the resulting canonical
state and snapshot publication.

------------------------------------------------------------------------

## 6. Arbitration and preemption (v1 rules)

Core implements arbitration (providers do not arbitrate).

### 6.1 Priority (highest first)

1.  Rig-triggered sync capture
2.  Device-triggered still capture
3.  Repeating streams (`VIEWFINDER` preferred over `PREVIEW` only when
    allowed; both are preemptible)

### 6.2 One active repeating stream per device

Each device instance supports at most one repeating stream **active**
(`mode != STOPPED`) at a time.

Multiple stream records may exist for a device (e.g., PREVIEW configured
but STOPPED while VIEWFINDER is active), but only one may be active at once.

### 6.3 v1 viewfinder rule (simple)

When a triggered capture is in-flight (device or rig), `VIEWFINDER` is
denied (or preempted to STOPPED) on affected devices. `PREVIEW` may be
stopped as policy dictates; default is to stop repeating streams during
capture on affected devices for determinism.

### 6.4 Profile conflicts

Core validates capture profiles and decides whether a request can be
satisfied without violating: - rig authority (if device is rig member
and rig is armed) - provider hard constraints - one-stream-per-device
invariant

If not satisfiable, core fails the request deterministically with a
clear error code.

------------------------------------------------------------------------

## 7. Core registries, canonical truth, and transport failure

During a healthy active generation, Core registries maintain the canonical
retained interpretation of facts accepted from the provider and of
Core-directed lifecycle state.

`CoreNativeObjectRegistry` maintains the Core-truth view of provider-reported
native objects used to populate
`CamBANGStateSnapshot.native_objects`.

Its responsibilities include:

* retaining provider-reported native-object records keyed by `native_id`;
* tracking created/destroyed state and timestamps;
* retaining `DESTROYED` records for the configured retention window;
* providing read-only registry truth to `SnapshotBuilder` on the Core thread.

Snapshot publication is assembled by `SnapshotBuilder` and delivered through
the `IStateSnapshotPublisher` boundary. `StateSnapshotBuffer` is the
thread-safe latest-snapshot buffer used by current smoke and Godot bridging
paths.

Registry mutation occurs only on the Core thread, based on provider facts and
Core-directed lifecycle or teardown steps.

### 7.1 Truth-loss boundary

A fatal provider/Core transport failure means at least one authoritative fact
was not delivered or could not be processed through the required transport
boundary. Core can no longer claim that its retained provider-derived state is
a complete current account of the active provider generation.

When CoreThread observes the first fatal transport failure, the active
generation immediately crosses a **truth-loss boundary**.

Beyond that boundary:

* Core registries cease to be publishable canonical provider truth;
* retained records may remain in memory only for containment, accounting,
  ownership release, and teardown;
* provider-derived queued or already-local work must not be applied as normal
  registry truth;
* no normal snapshot may be assembled from post-fault registry state;
* dirty flags or registry mutations required solely for teardown do not reopen
  publication;
* the failed generation cannot return to healthy truth status.

This does not require destroying registries immediately. Immediate destruction
could prevent correct resource release or teardown reconciliation. It changes
their authority: after truth loss they are teardown bookkeeping, not a source
of current public state.

A later successful start creates a new generation with fresh registries,
transport-failure state, and baseline publication.

------------------------------------------------------------------------

## 8. Warm and retention scheduling

### 8.1 Warm scheduling (`warm_hold_ms`)

Core schedules a warm expiry timer when: - a device transitions to
not-in-use (e.g., disengage, last stream stopped, capture completes) -
`warm_hold_ms > 0`

When the timer fires: - core initiates deterministic teardown for that
device instance - teardown steps produce provider calls and registry
transitions - snapshot is published when state changes

Core updates `warm_remaining_ms` in snapshots based on deadline vs
`timestamp_ns` at publish time.

### 8.2 Retention sweep

Core enforces native object record retention.

Rules: - DESTROYED records remain in registry until retention expires -
a retirement sweep removes expired records - **if the sweep removes records
while the generation retains publishable canonical truth, Core marks the
snapshot dirty for normal publication**

While the active generation retains publishable canonical truth, the retention
sweep runs:

- immediately before snapshot assembly; and
- optionally via a scheduled timer to ensure timely retirement even when no
  other activity occurs.

If a sweep removes records during that healthy publishable state, Core marks
the snapshot dirty.

After the truth-loss boundary, retention cleanup may still occur for teardown
or accounting, but it must not reopen snapshot publication.

------------------------------------------------------------------------

## 9. Snapshot publication

### 9.1 Dirty flag

Core maintains a `snapshot_dirty` flag that is set when: - any state
machine changes (phase/mode/engaged) - any counter changes - any error
field changes - any membership/topology changes - any registry change
occurs (create/destroy/retire) - spec/profile becomes effective

### 9.1.x Baseline publish on core loop start

On successful transition to `LIVE`, core is considered
**logically dirty** even if no provider events or commands have yet
been processed.

This ensures that the first loop iteration after `start()` produces a
baseline snapshot.

The baseline publish:

- Occurs via the normal dirty-driven publication path.
- Is not triggered by a special-case `request_publish()` call.
- Produces the first snapshot of the session with:
    - `version = 0`
    - `topology_version = 0`
    - A valid monotonic `timestamp_ns`.

There is no published snapshot prior to this baseline publish.

This rule guarantees that every successful core loop start produces
exactly one deterministic initial snapshot, even in the absence of
provider activity.

Subsequent publishes follow the normal dirty-driven semantics defined
in §9.1 and §9.2.

### 9.2 Publish point

While the active generation retains publishable canonical truth, Core publishes
at most once per loop iteration after draining the permitted work slice and
processing timers.

Publish steps: 1. Run retention sweep (may mark dirty) 2. Assemble new
`CamBANGStateSnapshot` (schema v1) 3. Atomically swap the snapshot
pointer in `CamBANGServer` 4. Emit `state_published(gen, version, topology_version)`
from `CamBANGServer`

### 9.2.a Publication quarantine after truth loss

Snapshot assembly and publication are permitted only while the active generation
retains publishable canonical truth.

Core must check the fatal transport state immediately before snapshot assembly.
Once the generation has crossed the truth-loss boundary:

* no new registry-derived snapshot is assembled or published;
* previously queued dirty state does not force a post-fault publication;
* timer, retention, provider-fact, and teardown activity cannot publish an
  intermediate representation of incomplete provider truth;
* the latest snapshot published before the fault remains the last coherent
  observation of that generation.

The last coherent immutable snapshot may remain available through the existing
public boundary while fatal teardown is in progress. It is not replaced by a
partially reconciled teardown snapshot.

Completed stop clears the public latest snapshot to `NIL` through the existing
stop contract. This rule does not introduce an immediate-on-fault `NIL`
transition or a new Godot-visible API state.

The next successful start advances `gen` and produces a fresh baseline snapshot
through the normal baseline-publication rule.


### 9.2.x Core publication vs Godot-visible publication

Core publication is an **internal mechanism**: core may build and publish
snapshots whenever dirty, including multiple times between Godot ticks.

Godot-visible publication is a **boundary contract** enforced by
`CamBANGServer`:

- `CamBANGServer.state_published(...)` is emitted **≤ 1 per Godot tick**.
- It is emitted only if observable state has changed since the previous tick.
- Godot-facing `gen/version/topology_version` are defined by this tick-bounded
  observable truth, not by core-internal publication frequency.

This separation allows core to integrate provider facts quickly without forcing
Godot consumers to handle bursty emissions.

#### Boundary marker (implementation hook)

To support the tick-bounded emission policy without per-frame heavy work,
core exposes O(1) publication markers at the boundary:

- a monotonic publish sequence counter ("changed since last tick")
- the latest topology signature (for boundary-side topology diffing)

The Godot bridge reads these markers once per tick and decides whether to emit.

### 9.3 Publish time vs capture time

Snapshot `timestamp_ns` is core publish time (monotonic, generation-relative).
Per-frame Image Acquisition Timing is optional provider-authored metadata carried on
the accepted `FrameView` in its declared source-neutral clock domain (see
`provider_architecture.md`). Core must not use it for identity, ordering, or
chronology.

### 9.4 `gen`, `version`, and `topology_version` bookkeeping

- `gen` advances by +1 on each successful `CamBANGServer.start()` that transitions
  from stopped → running.
- `version` increments by +1 on each **Godot-visible** snapshot publish
  (each `state_published` emission) within the current `gen`.
- `topology_version` increments by +1 only when the **observed topology differs**
  from the topology at the previous emission.

Core may maintain additional internal counters/markers to support boundary
coalescing cheaply; those do not redefine the Godot-visible counters.

------------------------------------------------------------------------

## 10. Deterministic shutdown sequence

### 10.1 Normal requested shutdown

When shutdown is requested while canonical provider truth remains intact:

1. Stop accepting new commands, or reject them deterministically.
2. Notify the Core loop.
3. Drain already-admitted provider facts according to the normal ordering
   contract and stop streams.
4. Tear down device instances in deterministic dependency order.
5. Request provider shutdown.
6. Perform the final retention sweep.
7. Publish the final coherent snapshot where the normal shutdown contract
   requires it.
8. Exit the Core thread.
9. Clear the public latest snapshot to `NIL` at completed stop.

For an attached provider, `CoreRuntime::stop()` is the shutdown owner. Hosts keep
the provider attached and alive until `stop()` returns, then detach and release
external ownership. Detaching first skips the provider object Core needs for
deterministic stream/device teardown and provider shutdown.

Core may internally model normal shutdown as explicit ordered phases, but the
architectural guarantee is completion of the required teardown and publication
steps before thread termination.

### 10.2 Fatal transport shutdown

Fatal transport shutdown begins when the active generation crosses the
truth-loss boundary.

Its priorities are containment, exact ownership release, and deterministic
termination—not reconstruction or publication of a provider sequence known to
be incomplete.

Core must:

1. Close all new ordinary, command, and essential admission.
2. Quarantine snapshot publication immediately.
3. Prevent queued and already-local provider-derived work from mutating normal
   canonical registries.
4. Discard pending normal command and ordinary work safely.
5. Retain or execute only explicitly teardown-critical Core-owned work.
6. Stop provider production and invoke the existing Core-owned teardown
   choreography.
7. Release or destroy rejected and discarded payload-owning work exactly once.
8. Clear internal generation state during completed teardown.
9. Exit the Core thread deterministically.
10. Clear the public latest snapshot to `NIL` at completed stop.

No final registry-derived snapshot is published after fatal truth loss. The last
coherent pre-fault snapshot remains the final published observation of the
failed generation.

Fatal transport teardown should reuse normal ownership and provider-shutdown
machinery wherever that machinery does not depend on accepting or publishing
further provider truth. It must not create a second independent shutdown
architecture.

A failed generation cannot be resumed. Restart creates a fresh generation and
fresh baseline.

------------------------------------------------------------------------

## Provider Shutdown Barrier Semantics

Providers must perform shutdown with deterministic ownership release and
termination.

When provider transport remains healthy, shutdown also preserves complete,
ordered lifecycle and native-object reporting through the strand.

When the strand has entered fatal transport failure, the provider must still
stop production and release actual resources truthfully, but it cannot claim
that the resulting lifecycle sequence was delivered completely to Core.
The failed generation is already under the truth-loss and publication-quarantine
contract.

The following phases describe the common ownership sequence. Their event-delivery
guarantees apply only while provider transport remains healthy.

### Phase A — Admission close

The provider transitions to a state where:

- new public operations are rejected deterministically
- frame production is gated off
- no new long-lived provider activity may begin

During normal healthy shutdown, work already admitted to the provider strand may
still complete.

After fatal strand failure, further normal callback delivery is prohibited.
Queued work instead follows the strand's failure-aware release, barrier, and
cleanup rules.

### Phase B — Stop production

Frame production is halted:

- repeating stream-production work is stopped
- platform capture loops are disabled
- synthetic schedulers or timers are cancelled

While provider transport remains healthy, this phase may generate lifecycle
stop events or provider error events through the normal strand path.

After fatal strand failure, resource shutdown still proceeds, but attempted
reporting must not reopen strand admission or imply that Core received a
complete sequence.

### Phase C — Resource release

Providers release owned resources in dependency order:

1. Stream-scoped provider-owned/native resources and stream-owned resources
2. Stream
3. AcquisitionSession (when realized), including acquisition-session-scoped provider/native resources
4. Device
5. Provider

While provider transport remains healthy, the provider emits the appropriate
lifecycle and native-object events at each boundary.

After fatal strand failure, the provider still releases resources in the same
truthful dependency order, but unavailable transport may prevent those release
facts from reaching Core. The provider must neither fabricate delivery nor keep
resources alive merely to preserve an already-invalid registry sequence.

Native-object lifecycle truth is emitted against current structural owner
contexts (stream, acquisition_session, device, provider) rather than through a
separate `FrameProducer` structural participant.

Native-object destruction events must be emitted **only when the
resource has actually been released**.

### Phase D — Strand drain barrier

After teardown events have been posted, the provider performs a **strand
drain barrier**.

A successful barrier guarantees that:

- all events admitted to the provider strand prior to the barrier
- have been processed by the strand
- and forwarded to Core's provider event ingress queue

The barrier does not require Core to have integrated those events into
snapshot state before returning.

If the provider strand has already entered fatal transport failure, normal
barrier delivery may no longer be possible. In that case the barrier must
complete with failure through a failure-aware path; it must not wait
indefinitely and must not imply that the admitted provider-fact sequence reached
Core intact.


### Phase E — Strand stop

After a successful normal drain barrier:

* the provider strand stops accepting new events;
* any strand worker thread exits;
* provider shutdown may return.

If the strand has entered fatal transport failure, shutdown does not require a
successful normal barrier. The strand instead performs its documented
failure-aware completion and cleanup, rejects further admission, releases queued
payload ownership exactly once, reports the fatal failure once, and exits
deterministically.

### Timeout behaviour

Providers must not block indefinitely during shutdown.

If platform APIs fail to terminate within a bounded timeout:

- an error event may be emitted
- shutdown may return with failure
- resources that could not be released must **not emit false destruction
  events**

Unreleased resources may therefore remain visible in the lifecycle
registry and snapshot system as surviving native objects.

This behaviour preserves the **truthfulness rule** of the lifecycle
registry.

Provider-strand transport failure is stronger than an ordinary backend shutdown
timeout. It means the provider/Core authoritative fact sequence is no longer
known to be complete.

Core therefore applies the fatal truth-loss and publication-quarantine contract.
Surviving registry records may still be used for containment and teardown, but
they must not be published as current canonical provider truth after the
truth-loss boundary.


------------------------------------------------------------------------

## 11. Synthetic mode hook

Core must support a `SyntheticProvider` that: - simulates enumeration,
open/close, streams, and capture - injects deterministic timestamps -
injects deterministic errors - supports test cases for: -
arbitration/preemption - warm scheduling and expiry - retention sweep
correctness - snapshot generation correctness

Synthetic mode is a first-class verification strategy for CI and
development confidence.

Note:

The current development smoke executable uses a minimal
`StubProvider` for lifecycle and determinism validation.

`SyntheticProvider` is intended as a richer deterministic
simulation provider and may supersede StubProvider in
future development phases.

## 11.x Host pause semantics (SyntheticProvider timing drivers)

When CamBANG runs inside a host environment (e.g. Godot), the host may
temporarily suspend frame ticks (for example when the application is
paused).

Pause behaviour depends on the SyntheticProvider timing driver.

### virtual_time

When `timing_driver = virtual_time`:

- Time advances **only** when the host explicitly advances synthetic
  time (e.g. via a tick or `advance(dt_ns)` call).
- If the host pauses and no advancement occurs, **synthetic time does not
  progress**.
- No scheduled events are executed during the pause.
- Frame production, lifecycle transitions, and scenario events remain
  pending until the next advancement.

This preserves deterministic replay behaviour. Timeline scenarios and
nominal synthetic streams simply **resume where they left off** when the
host resumes ticking.

### real_time

When `timing_driver = real_time`:

- Time continues to advance according to the provider’s monotonic clock,
  independent of host ticks.
- Scheduled events (including frame production and lifecycle events)
  continue to occur internally while the host is paused.

However, the Godot boundary remains tick-bounded (see §9.2.x):

- No `state_published(...)` emissions occur while the host is not ticking.
- When ticks resume, the boundary observes the **latest snapshot state**
  and emits a single publish reflecting the most recent truth.

Intermediate states that occurred during the pause are therefore
coalesced by the tick-bounded observable model.

### Design intent

This behaviour preserves two important guarantees:

- **Virtual-time determinism** for testing and timeline-scenario replay.
- **Real-time realism** for live cadence simulation, without requiring
  the host to tick continuously.

The pause semantics do not alter the provider → core contract or the
ordering guarantees of provider callbacks.

------------------------------------------------------------------------

## 12. Invariants summary

- All Core-owned state mutation occurs on the dedicated Core thread.
- Provider callbacks are serialized into Core through one provider callback
  context.
- At most one repeating stream is active per device instance.
- Retention-sweep removals mark the snapshot dirty only while the active
  generation retains publishable canonical truth.
- Snapshot publication is atomic and lock-free for readers.
- Shutdown proceeds deterministically; completed stop clears the public
  snapshot to `NIL`.
- Core never publishes registry-derived state after the active generation has
  lost authoritative provider truth.
- Provider-strand and Core essential transport are hard-bounded.
- Failure to admit an authoritative provider fact is fatal to the active
  generation.
- Fatal transport failure is signalled independently of the failed queue.
- Rejected, reclaimed, or discarded payload ownership is released exactly once.
- Restart after fatal transport failure creates a fresh generation and baseline.


## 13. Validation Strategy (Core vs Platform)
------------------------------------------------------------------------

CamBANG distinguishes between **core invariant validation** and
**platform provider integration validation**.

### 13.1 Core invariant validation (portable)

Core lifecycle and determinism invariants are validated in development
builds via the `core_spine_smoke` smoke executable. It supports a providerless
baseline mode and a stub-backed mode for paths that require a deterministic
provider.

The smoke executable validates:

- Deterministic shutdown choreography
- Release-on-drop semantics under overload
- Command rejection during `TEARING_DOWN`
- Late provider fact integration
- EXIT phase reached before thread termination
- No frame leaks across repeated start/stop cycles

This validation is platform-independent and must remain independent of
platform-backed providers. Stub-backed stress/provider paths are part of the
host-native `maintainer_tools` build family.

### 13.2 Platform integration validation

Platform providers are validated separately under real platform-backed conditions. Platform validation should ensure:
- Correct threading integration
- Correct callback serialization
- No deadlocks under real API pressure
- Deterministic teardown under platform constraints

Platform validation supplements but does not replace core invariant validation.
