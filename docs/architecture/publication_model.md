# CamBANG Publication Model

This document explains **how runtime state becomes observable to Godot
consumers**.

It supplements the normative documents:

- `state_snapshot.md`
- `core_runtime_model.md`
- `godot_boundary_contract.md`

This document explains **mechanics, coalescing, and interpretation**.
It does **not** redefine the normative schema or the Godot-facing contract.

---

## 1. Internal state vs observable state

The core runtime may publish or update internal state faster than the host
environment (Godot) produces frame ticks.

Therefore two related but distinct layers exist:

| Layer | Description |
|---|---|
| Internal state | Updated continuously on the core thread as facts are integrated |
| Observable state | Exposed to Godot on ticks through `CamBANGServer` |

The observable layer is **tick-bounded**.

This means that core may integrate multiple changes between host ticks,
but Godot consumers only observe the latest converged truth at the next
eligible tick.

---

## 2. Tick-bounded publication

Godot observers see **at most one `state_published(...)` emission per
Godot tick**.

If multiple internal updates occur between ticks:

```text
internal publish P0
internal publish P1
internal publish P2
```

Godot will observe only one publication at the next tick:

```text
Godot tick T1
   │
   ▼
emit latest observable snapshot
```

Intermediate states are intentionally hidden.

This keeps the Godot-facing model:

- deterministic
- bounded
- easy to consume from frame-driven logic
- resistant to signal storms

Counter invariant:

- `topology_version` is a subset of `version`
- every topology change increments `version`
- not every `version` change increments `topology_version`

---

## 3. Startup burst example

A typical startup burst may look like this:

```text
Core thread internal publishes:
  P0 baseline
  P1 frame counter update
  P2 stream mode active

Godot ticks:
  T0                  T1
```

Observable result:

- the first visible publish of the generation is the baseline snapshot:
  `(gen = N, version = 0, topology_version = 0)`
- if multiple internal updates happen after the baseline and before the next
  tick, only the latest snapshot at that tick is emitted

Thus Godot scripts consume **authoritative state at the tick**, not every
intermediate transition. Intermediate internal states may be coalesced, but
the generation does not become observable mid-stream.

---

## 4. Publication pipeline

The publication path is conceptually:

```text
Provider events / commands / timers
      │
      ▼
Core runtime integrates converged state
      │
      ▼
Snapshot assembly
      │
      ▼
Internal publication marker + latest snapshot
      │
      ▼
CamBANGServer polls once per Godot tick
      │
      ▼
state_published(gen, version, topology_version)
```

Key properties:

- snapshots are immutable
- one Godot-visible signal corresponds to one observable snapshot
- polling and signal-driven consumption are both supported
- Godot-visible publication is boundary-side coalescing over core truth

---

## 5. Boundary markers

To support cheap tick-bounded coalescing, the boundary uses O(1)-style
publication markers rather than rescanning all state each frame.

Conceptually these include:

- a monotonic “published since last tick?” sequence marker
- the latest topology signature

The Godot boundary reads these markers once per tick and decides whether
to emit.

This allows:

- frequent core integration
- bounded Godot-side emission
- cheap topology-change detection

---

## 6. Baseline publication

On a successful start that creates a new generation:

- core is logically dirty when it transitions to `LIVE`
- a coherent baseline snapshot exists for the new generation
- the first Godot-visible publish of that generation is:

```text
(gen = N, version = 0, topology_version = 0)
```
Coalescing never permits Godot to observe a generation for the first time at
a non-baseline version.

There is no published observable snapshot prior to that baseline.

Coalescing never permits Godot to observe a generation for the first time at
a non-baseline version.

This ensures every generation begins with exactly one coherent baseline
visible at the Godot boundary.

---

## 7. Version counters

Three counters describe observable runtime state. They are zero-indexed by convention.

### 7.1 `gen`

A generation represents one runtime session.

`gen` advances by +1 on each successful `CamBANGServer.start()` that
transitions from stopped → running.

Example:

```text
stop()
start()
→ new generation
```

### 7.2 `version`

`version` increments on **every observable publish** within a generation.

Example:

```text
baseline  version=0
update    version=1
update    version=2
```

Properties:

- contiguous
- resets for each new generation
- advances on any observable state change

### 7.3 `topology_version`

`topology_version` increments only when the **observable structure**
changes relative to the previous emission.

Examples of topology change:

- rig created/destroyed
- device added/removed
- stream created/destroyed
- membership changes
- root-visibility changes that alter observable topology

Illustrative sequence:

```text
version:          0 1 2 3 4 5 6
topology_version: 0 0 0 1 1 2 3
```

Interpretation:

- `version` means “something observable changed”
- `topology_version` means “structure changed; rebuild topology caches”

`topology_version` never changes without `version` also changing.

---

## 8. Consumer interpretation

Consumers should treat each publish as:

> Authoritative runtime state as of this Godot tick.

They should **not** assume they will observe every intermediate internal
transition.

Typical consumer pattern:

```gdscript
func _on_state_published(gen, version, topology_version):
    var snap = CamBANGServer.get_state_snapshot()

    if topology_version != last_topology_version:
        rebuild_topology_cache(snap)
        last_topology_version = topology_version

    update_runtime_metrics(snap)
```

This allows expensive rebuilds only when topology changes, while cheap
metrics update on every observable publish.

---

## 9. Polling and signal-driven access

Two access patterns are supported.

### Signal-driven

```text
state_published(...)
→ get_state_snapshot()
```

Inside the signal handler, `get_state_snapshot()` returns the snapshot
corresponding to that emission.

### Polling

```text
get_state_snapshot() each frame
```

Outside the handler, the call returns the most recently latched observable
snapshot.

These two models can be mixed safely.

---

## 10. Pause behaviour

Host environments may pause frame ticks.

During a pause:

- core may continue integrating state, depending on provider timing model
- no Godot-visible `state_published(...)` emissions occur

When ticks resume:

- the boundary observes the latest available snapshot
- if observable truth changed since the previous emission, a single publish occurs
- intermediate internal states remain hidden

This preserves:

- tick-bounded observability
- coalescing semantics
- deterministic consumer behaviour after pause/resume

Synthetic timing-driver semantics remain defined in
`core_runtime_model.md`.

---

## 11. Relationship to frame delivery

Frame delivery and snapshot publication are related, but distinct.

- frame sinks consume frame payloads
- publication exposes **runtime truth** to Godot

A system may integrate many frame events internally while still exposing
only one tick-bounded observable publish.

This separation prevents frame cadence from directly defining signal cadence.

---

## 12. Relationship to the normative schema

The normative snapshot structure remains defined in:

- `state_snapshot.md`

The observable Godot-facing runtime contract remains defined in:

- `architecture/godot_boundary_contract.md`

This document exists to explain:

- coalescing
- internal vs observable publication
- counter interpretation
- consumer-side reasoning
