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
`CBLifecycleRegistry` - snapshot assembly/publish bookkeeping

Godot-facing objects never mutate core state directly; they enqueue
commands to core.

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

On each wake, core processes in the following order:

1.  **Drain provider events** (full drain unless explicitly documented otherwise)
2.  **Drain commands** (bounded or full drain; v1 default is full drain)
3.  **Process due timers** (all deadlines ≤ now)
4.  **Publish snapshot if dirty** (exactly once per loop iteration)

This ordering is fixed to ensure: - "what happened" (provider events) is
integrated before "what is requested" (commands) - timers are honored
deterministically - snapshot publication reflects converged state

### 3.3 Batching and fairness

To avoid starvation under pathological loads, core may cap drains per
iteration (implementation detail). Any cap must preserve determinism and
should be documented as a constant.

------------------------------------------------------------------------

## 4. Queues and message types

### 4.1 Command queue (Godot → Core)

Commands represent requests from Godot-facing objects, e.g.: -
engage/disengage device - set warm policy - create/destroy stream -
start/stop stream - set capture profile(s) - trigger device capture -
arm/disarm rig - trigger rig sync capture - apply spec patches
(`ApplyMode` handled by core)

Commands are immutable message objects; core owns command execution.

### 4.2 Provider event queue (Provider → Core)

Provider events represent facts observed by the provider, e.g.: - device
opened/closed confirmations - stream started/stopped confirmations -
frame arrival (stream intent or capture) - capture completion / capture
error - device/stream errors - native object created/destroyed
notifications

Provider events must be enqueued from the provider's serialized callback
context.

------------------------------------------------------------------------

## 5. Core state machines (high-level)

Core maintains explicit state machines for:

-   **Rig core state** (OFF/ARMED/TRIGGERING/COLLECTING/ERROR)
-   **Device core state** (IDLE/STREAMING/CAPTURING/ERROR + engaged
    bool)
-   **Stream core state** (STOPPED/FLOWING/STARVED/ERROR + intent)

AcquisitionSession seam is provider/native-object truth and is retained in
snapshot state via `acquisition_sessions`. Current implementation status is
stream-backed realization (SyntheticProvider); still-only AcquisitionSession
realization is not yet implemented.

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

## 7. `CBLifecycleRegistry`

`CBLifecycleRegistry` tracks all CamBANG-created native/core objects for
introspection.

Responsibilities: - assign monotonic `native_id` - track `phase`
transitions (CREATED → LIVE → TEARING_DOWN → DESTROYED) - retain
DESTROYED records for a fixed retention window - compute
`detached_root_ids` based on ended owners and retained records - expose
registry view to `CBStatePublisher` (read-only on core thread)

Registry mutation occurs only on the core thread, based on provider
events and core-directed teardown steps.

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
a retirement sweep removes expired records - **if the sweep removes any
records, core marks snapshot dirty and publishes**

Retention sweep runs:

- Immediately before snapshot assembly (always), and
- Optionally via scheduled timer to ensure timely retirement
  even when no other activity occurs.

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

Core publishes at most once per loop iteration (after draining
events/commands and processing timers).

Publish steps: 1. Run retention sweep (may mark dirty) 2. Assemble new
`CamBANGStateSnapshot` (schema v1) 3. Atomically swap the snapshot
pointer in `CamBANGServer` 4. Emit `state_published(gen, version, topology_version)`
from `CamBANGServer`

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
Per-frame capture timestamps are separate metadata carried by provider events and must
use a provider-agnostic time domain representation (see `provider_architecture.md`).
Core must not assume capture time is wall-clock.

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

When shutdown is requested:

1.  Stop accepting new commands (or reject with deterministic error)
2.  Notify core loop
3.  Core drains provider events (best effort) and stops streams
4.  Core tears down device instances (respecting deterministic ordering)
5.  Core requests provider shutdown
6.  Core performs final retention sweep
7.  Core publishes a final snapshot
8.  Core thread exits

Providers must not block indefinitely on shutdown (provider contract).
Core may internally model shutdown as explicit ordered phases,
but the architectural guarantee is completion of steps 1–8
before thread termination.
------------------------------------------------------------------------

## Provider Shutdown Barrier Semantics

Providers must perform shutdown in a manner that preserves truthful
lifecycle reporting while ensuring deterministic termination.

Provider shutdown proceeds through the following conceptual phases.

### Phase A — Admission close

The provider transitions to a state where:

- new public operations are rejected deterministically
- frame production is gated off
- no new long-lived provider activity may begin

Existing work already admitted to the provider strand may still
complete.

### Phase B — Stop production

Frame production is halted:

- repeating frame producers stop
- platform capture loops are disabled
- synthetic schedulers or timers are cancelled

This phase may generate lifecycle stop events or provider error events.

### Phase C — Resource release

Providers release owned resources in dependency order:

1. FrameProducer
2. Stream
3. Device
4. Provider

At each boundary the provider emits the appropriate lifecycle and
native-object events reflecting the actual state transition.

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

### Phase E — Strand stop

Once the drain barrier succeeds:

- the provider strand stops accepting new events
- any strand worker thread exits
- provider shutdown may return

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

------------------------------------------------------------------------
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

-   All core state mutation occurs on the dedicated core thread.
-   Provider callbacks are serialized into core via a single callback
    context.
-   One repeating stream active per device instance (design choice).
-   Retention sweep removals must trigger snapshot publish.
-   Snapshot publication is atomic and lock-free for readers.
-   Shutdown proceeds deterministically and leaves a final published
    snapshot.

## 13. Validation Strategy (Core vs Platform)
------------------------------------------------------------------------

CamBANG distinguishes between **core invariant validation** and
**platform provider integration validation**.

### 13.1 Core invariant validation (portable)

Core lifecycle and determinism invariants are validated in development
builds via the `core_spine_smoke` smoke executable (stub-provider-only).

The smoke executable validates:

- Deterministic shutdown choreography
- Release-on-drop semantics under overload
- Command rejection during `TEARING_DOWN`
- Late provider fact integration
- EXIT phase reached before thread termination
- No frame leaks across repeated start/stop cycles

This validation is platform-independent and must remain stub-provider-only.

### 13.2 Platform integration validation

Platform providers (e.g., Windows Media Foundation) are validated separately under real platform-backed conditions to ensure:
- Correct threading integration
- Correct callback serialization
- No deadlocks under real API pressure
- Deterministic teardown under platform constraints

Platform validation supplements but does not replace core invariant validation.
