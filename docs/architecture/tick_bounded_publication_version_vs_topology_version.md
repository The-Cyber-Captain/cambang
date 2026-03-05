# Companion Diagram: `version` vs `topology_version` across a lifecycle

This diagram shows how `version` and `topology_version` evolve **within a single `gen`** as observable state changes over time.

Key rules:

- `version` increments on **every** Godot-visible publish (contiguous).
- `topology_version` increments **only** when topology differs vs the previous emission.
- `topology_version` never changes without `version` also changing.
- On `start()` that creates a new generation: both reset to `0`.

---

## Example lifecycle within `gen = 0`

Legend:

- `S#` = Godot-visible publish number within the generation (not a real counter)
- `Δtopo` indicates whether topology changed since the previous emission

```
Godot-visible publishes within gen=0
┌────┬───────────────────────────────┬───────────────┬───────────────┬───────────┐
│ S# │ Observable event               │ version       │ topology_ver  │ Δtopo     │
├────┼───────────────────────────────┼───────────────┼───────────────┼───────────┤
│ 0  │ Start baseline visible         │ 0             │ 0             │ (baseline)│
│ 1  │ Stream becomes active          │ 1             │ 0             │ no        │
│ 2  │ Frame counters update          │ 2             │ 0             │ no        │
│ 3  │ Add stream (new endpoint)      │ 3             │ 1             │ yes       │
│ 4  │ More frames / stats            │ 4             │ 1             │ no        │
│ 5  │ Provider switch (topology)     │ 5             │ 2             │ yes       │
│ 6  │ Remove stream (teardown)       │ 6             │ 3             │ yes       │
│ 7  │ Final stats update on stop     │ 7             │ 3             │ no        │
└────┴───────────────────────────────┴───────────────┴───────────────┴───────────┘
```

---

## As a pair of step charts

```
version:
0 ── 1 ── 2 ── 3 ── 4 ── 5 ── 6 ── 7
```

```
topology_version:
0 ── 0 ── 0 ── 1 ── 1 ── 2 ── 3 ── 3
```

Interpretation:

- `topology_version` is a **subset counter** of `version`.
- Consumers can treat:
  - `version` change as “something changed”
  - `topology_version` change as “structure changed; rebuild topology caches”

---

## Cross-generation reset example

After a successful stop → start (new generation):

```
... gen=0 version=7 topology_version=3
start()
=> gen=1 version=0 topology_version=0   (baseline on first observable tick)
```

---

## Consumer optimisation hook

A typical consumer pattern:

```gdscript
func _on_state_published(gen, version, topology_version):
    var snap = CamBANGServer.get_state_snapshot()

    if topology_version != last_topology_version:
        rebuild_topology_caches(snap)   # expensive
        last_topology_version = topology_version

    update_metrics(snap)                # cheap
```

This remains correct even when many internal core updates are coalesced into one tick-visible publish.
