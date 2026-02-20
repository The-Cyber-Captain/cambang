# CamBANG Core Runtime Model

This document defines the **internal runtime model** of CamBANG core:
threading, event ordering, registries, snapshot publication,
warm/retention scheduling, and deterministic shutdown.

This document complements: - `provider_architecture.md` (core ↔ provider
boundary) - `state_snapshot.md` (public snapshot schema v1)

------------------------------------------------------------------------

## 1. Execution model overview

CamBANG core runs on a **dedicated core thread**.

All mutation of core-owned state occurs on that thread, including: -
arbitration state - device/stream/rig runtime state machines -
`CBLifecycleRegistry` - snapshot assembly/publish bookkeeping

Godot-facing objects never mutate core state directly; they enqueue
commands to core.

Provider callbacks never mutate core state directly; they enqueue
provider events to core (serialized callback context per provider
contract).

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

1.  **Drain provider events** (bounded or full drain; v1 default is full
    drain)
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

-   **Rig runtime state** (OFF/ARMED/TRIGGERING/COLLECTING/ERROR)
-   **Device runtime state** (IDLE/STREAMING/CAPTURING/ERROR + engaged
    bool)
-   **Stream runtime state** (STOPPED/FLOWING/STARVED/ERROR + intent)

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

### 6.2 One repeating stream per device

Each device instance supports at most one repeating stream active at a
time (design choice).

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

Sweep triggers: - opportunistically before publish - and/or via a
scheduled timer when the next record is due to expire (recommended)

------------------------------------------------------------------------

## 9. Snapshot publication

### 9.1 Dirty flag

Core maintains a `snapshot_dirty` flag that is set when: - any state
machine changes (phase/mode/engaged) - any counter changes - any error
field changes - any membership/topology changes - any registry change
occurs (create/destroy/retire) - spec/profile becomes effective

### 9.2 Publish point

Core publishes at most once per loop iteration (after draining
events/commands and processing timers).

Publish steps: 1. Run retention sweep (may mark dirty) 2. Assemble new
`CamBANGStateSnapshot` (schema v1) 3. Atomically swap the snapshot
pointer in `CamBANGServer` 4. Emit `state_published(gen, topology_gen)`
from `CamBANGServer`

### 9.3 `gen` and `topology_gen` bookkeeping

-   `gen` increments for every published snapshot
-   `topology_gen` increments only for structural changes as defined in
    `state_snapshot.md`

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

------------------------------------------------------------------------

## 11. Synthetic mode hook

Core must support a `SyntheticProvider` that: - simulates enumeration,
open/close, streams, and capture - injects deterministic timestamps -
injects deterministic errors - supports test cases for: -
arbitration/preemption - warm scheduling and expiry - retention sweep
correctness - snapshot generation correctness

Synthetic mode is a first-class test harness strategy for CI and
development confidence.

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
