# Tick-Bounded Publication: Core → Godot Coalescing

This document explains how **internal core snapshot publications** are coalesced into **Godot-visible `state_published()` signals**.

The core may publish state updates faster than the Godot frame loop. The Godot boundary exposes a **tick-bounded observable truth model**:

- **≤ 1 publish per Godot tick**
- Only emitted **if observable state changed since the previous tick**

This prevents signal storms while preserving correctness.

---

## Conceptual model

Three layers participate:

```
Provider / CoreRuntime
        │
        │ internal snapshot publishes (unbounded frequency)
        ▼
Snapshot mailbox / published_seq marker
        │
        │ polled once per Godot tick
        ▼
CamBANGServer (Godot boundary)
        │
        │ emits at most once per tick
        ▼
Godot scripts / consumers
```

---

## Timeline example (startup burst)

```
Time →
```

```
Core thread (internal publishes)
│
│   P0        P1       P2
│   │         │        │
│   ▼         ▼        ▼
│  [baseline] [frame] [mode active]
│
│──────────────────────────────────────────────
│
Godot ticks
│
│      T0                T1
│      │                 │
│      ▼                 ▼
│  check mailbox    check mailbox
│
│──────────────────────────────────────────────
│
Godot-visible publishes
│
│      S0                S1
│      │                 │
│      ▼                 ▼
│ (gen=0 ver=0 topo=0) (gen=0 ver=1 topo=0)
│
│──────────────────────────────────────────────
│
Snapshots observed by scripts
```

### What happened here

Core emitted **three internal publishes**:

- `P0`: baseline snapshot
- `P1`: first frame delivered
- `P2`: stream mode becomes active

But both `P1` and `P2` occurred before the next Godot tick, so they are **coalesced**.

Godot therefore sees only:

- `S0`: baseline
- `S1`: latest state as of next tick

Intermediate states are intentionally **not observable**.

---

## Detailed steps

### Step 1 — Core publishes

On each internal publish:

- `published_seq++`
- `topology_sig` computed

Example sequence:

- `P0 seq=1 topology_sig=A`
- `P1 seq=2 topology_sig=A`
- `P2 seq=3 topology_sig=A`

These may occur on worker threads.

### Step 2 — Godot tick polls marker

Once per frame:

```
CamBANGServer._process():
    if published_seq changed since last tick:
        latch latest snapshot
        emit state_published(...)
```

Important:

- Only **one** snapshot is latched per tick.
- It is always the **latest** available snapshot.

### Step 3 — Counters are updated

When emitting:

- `version++`
- if `topology_sig` changed since the previous emission: `topology_version++`

Even if many internal updates occur between ticks.

---

## Visual summary

```
Core publishes
│
│   P0 ── P1 ── P2 ── P3 ── P4
│    │    │     │
│    │    │     │
│    └────┴─────┴──────────────┐
│                               │
Godot ticks                     │
│                               │
│   T0        T1         T2     │
│   │         │          │      │
│   ▼         ▼          ▼      │
│  emit      emit       emit    │
│                               │
│  S0        S1         S2      │
│                               │
│  (P0)      (P2)       (P4)    │
│                               │
└───────────────────────────────┘
```

Each tick publishes the **latest** available snapshot.

---

## Guarantees

### Bounded emission rate

```
≤ 1 state_published per Godot tick
```

Even if the core publishes hundreds of updates.

### No loss of final truth

Although intermediate states are coalesced, the most recent snapshot is always delivered.

### Deterministic counters

Within a generation:

- `version` increments on every Godot-visible publish
- `topology_version` increments only when the topology changed vs the previous emission

---

## Consumer interpretation

Consumers should treat each publish as:

> **Authoritative system state at this Godot tick.**

They should not assume intermediate transitions occurred.

Example:

```gdscript
func _on_state_published(gen, version, topology_version):
    var snap = CamBANGServer.get_state_snapshot()

    if topology_version != last_topology_version:
        rebuild_topology(snap)
    else:
        update_runtime_metrics(snap)
```

---

## Summary

The publication pipeline behaves like a **tick-synchronised state sampler**:

- core publishes continuously
- Godot samples once per frame (at most)

This yields deterministic scripting behaviour, prevents storms, and keeps topology caching efficient.
