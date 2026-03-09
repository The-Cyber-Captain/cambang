# Godot Boundary Contract

This document defines the **canonical observable contract** between the CamBANG runtime and Godot consumers.

It consolidates the externally visible behaviour already defined across the architecture documents into one place for maintainers and integrators.

This document describes **observable behaviour only**. It intentionally does **not** describe internal provider lifecycle phases or dev scaffolding implementation details.

---

# Canonical Observable Surface

Godot consumers interact with the runtime exclusively through:

```
CamBANGServer.start()
CamBANGServer.stop()
CamBANGServer.get_state_snapshot()

signal state_published(gen, version, topology_version)
```

No other Godot‑facing runtime surface is considered part of the stable observable contract.

---

# Runtime Start Behaviour

After:

```
CamBANGServer.start()
```

the runtime may enter a short **pre‑baseline window** where:

```
get_state_snapshot() == NIL
```

No runtime state should be assumed during this period.

The first observable publish of a generation will always be:

```
(gen = N, version = 0, topology_version = 0)
```

This publish occurs on the **first eligible Godot tick** after runtime initialization.

Consumers may treat this snapshot as the **baseline snapshot** for that generation.

---

# Stop Behaviour

After a completed stop:

```
CamBANGServer.stop()
```

the observable boundary returns to:

```
get_state_snapshot() == NIL
```

No snapshot from the previous generation should remain visible after stop.

---

# Restart Behaviour

When the runtime restarts:

```
CamBANGServer.start()
```

the following guarantees apply:

1. No snapshot from the previous generation appears during the pre‑baseline window.
2. The next observable snapshot will be:

```
(gen = previous_gen + 1,
 version = 0,
 topology_version = 0)
```

This behaviour ensures clean generation boundaries across restarts.

---

# Tick‑Bounded Publication

The runtime may produce multiple internal updates between Godot frames.

However Godot observers will see:

```
≤ 1 state_published(...) emission per Godot tick
```

If multiple internal updates occur between ticks, they are **coalesced** and only the latest observable state is published.

This guarantees predictable behaviour for frame‑driven consumers.

---

# Version Counters

## version

`version` increments whenever observable runtime state changes within a generation.

Properties:

- contiguous within a generation
- starts at `0` for the baseline publish
- increments by `+1` for each subsequent observable change

## topology_version

`topology_version` increments only when **observable topology changes** occur.

Examples of topology change:

- device added
- device removed
- stream added
- stream removed

Non‑structural changes (for example counters or stream configuration updates) may increment `version` without incrementing `topology_version`.

---

# Snapshot Access Semantics

Two access patterns are supported.

## Signal‑driven

```
state_published(gen, version, topology_version)
→ get_state_snapshot()
```

Inside the signal handler, `get_state_snapshot()` returns the snapshot corresponding to that emission.

## Polling

```
get_state_snapshot() each frame
```

Outside the handler, `get_state_snapshot()` returns the latest observable runtime state.

Polling and signal‑driven consumption may be mixed safely.

---

# Snapshot Immutability

Published snapshots are **immutable**.

Properties:

- snapshots do not mutate after publication
- retaining references to older snapshots is safe
- older snapshots remain readable after newer publishes occur

Example:

```
old_snapshot = get_state_snapshot()

# later publishes occur

old_snapshot remains readable and unchanged
```

---

# What This Document Does Not Specify

This document intentionally does not describe:

- provider lifecycle states
- internal runtime bring‑up phases
- dev scaffolding behaviour
- scenario execution mechanisms
- platform‑specific provider behaviour

Those details belong to internal architecture documentation.

This document defines only the **observable Godot boundary contract**.