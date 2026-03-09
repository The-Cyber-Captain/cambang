# Godot Boundary Abuse Scenes

This document describes the **Godot boundary verification scenes** used to validate the CamBANG runtime from the Godot side.

These scenes are **development tooling**, not product UI.

Their purpose is to verify that the minimal Godot‑facing runtime contract behaves correctly under repeated abuse.

---

# Purpose

The scenes exercise the chain:

```
Provider
↕
Core Runtime
↕
Snapshot Publication
↕
CamBANGServer
↕
Godot Consumers
```

They verify that the Godot‑visible runtime boundary remains:

- deterministic
- crash‑free
- semantically correct
- safe for polling and signal consumption

---

# Provider Selection

These scenes intentionally use:

```
SyntheticProvider
```

as the deterministic abuse engine.

Even when the wider build is configured for:

- StubProvider
- Windows Media Foundation
- other platform providers

the Godot abuse scenes remain **synthetic‑driven**.

This ensures deterministic behaviour independent of hardware.

---

# Scenes

## 60_restart_boundary_abuse

Verifies restart semantics.

Checks:

- snapshot becomes `NIL` after stop
- no stale snapshot appears before new baseline
- first publish of a new generation is `(gen+1,0,0)`

Expected output:

```
OK: godot restart boundary abuse PASS
```

---

## 61_tick_bounded_coalescing_abuse

Verifies tick‑bounded publication.

The scene drives bursty synthetic activity and confirms:

- at most one `state_published` signal per Godot tick
- `version` remains contiguous
- `topology_version` changes only when observable topology changes

Expected output:

```
OK: godot tick-bounded coalescing abuse PASS
```

---

## 62_snapshot_polling_immutability_abuse

Verifies snapshot immutability and polling safety.

The scene:

- polls snapshots every frame
- listens to `state_published`
- retains an old snapshot reference

Checks:

- cached snapshots remain readable
- cached snapshots do not mutate
- polling + signals behave consistently

Expected output:

```
OK: godot snapshot polling/immutability abuse PASS
```

---

## 63_snapshot_observer_minimal

A minimal observer scene used for diagnostics.

Displays snapshot fields such as:

- generation
- version
- topology_version
- device count
- stream count
- frame counters

Expected output:

```
OK: godot snapshot observer minimal PASS
```

---

# Running the Scenes

Scenes are intended to be run individually from the Godot project.

Each scene:

1. starts the runtime
2. optionally triggers a deterministic scenario
3. performs boundary assertions
4. prints a final PASS or FAIL line
5. exits automatically

---

# Final Output Rule (Test Harness Hardening)

Any self‑running Godot verification scene that prints a final result line such as:

```
OK: ...
FAIL: ...
```

must **not** call:

```
get_tree().quit()
```

immediately in the same call stack.

Instead the scene must:

1. print the final result line
2. schedule a deferred exit
3. quit after a short delay (for example 2 frames or ~50 ms)

This ensures console output is reliably visible for developers and maintainers.

Important:

- this rule **does not weaken assertions**
- it only improves output reliability during shutdown

---

# Relationship to Native Verification Tools

The Godot abuse scenes complement the native verification tools.

Native tools verify:

- core runtime invariants
- provider compliance
- scenario correctness

Godot scenes verify:

- the Godot‑facing runtime boundary
- snapshot semantics
- publication behaviour

Both layers together provide full runtime validation.