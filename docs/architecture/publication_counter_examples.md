# Publication Counter Examples (Reference)

This document provides concrete examples of **`version` and `topology_version`
evolution** during runtime.

It complements the explanatory document:

- `architecture/publication_model.md`

Normative definitions remain in:

- `state_snapshot.md`

---

# 1. Counter definitions

| Counter | Meaning |
|---|---|
| `gen` | runtime generation |
| `version` | increments on every observable publish |
| `topology_version` | increments when observable topology changes |

Properties:

- `version` increments for every observable publish.
- `topology_version` increments only when topology changes.
- `topology_version` never increments without `version`.

---

# 2. Basic startup sequence

Example:

Core start
Device discovered
Stream created

Observable publications:

| Publish | gen | version | topology_version | Meaning |
|---|-----|---|---|---|
| baseline | 0   | 0 | 0 | runtime started |
| update | 0   | 1 | 1 | device added |
| update | 0   | 2 | 2 | stream added |

---

# 3. Non-topology updates

Frame counters change frequently but do not alter topology.

Example:

| Publish | version | topology_version |
|---|---|---|
| baseline | 0 | 0 |
| frame update | 1 | 0 |
| frame update | 2 | 0 |
| frame update | 3 | 0 |

Only `version` advances.

---

# 4. Topology mutation

Device removal example:

| Publish | version | topology_version |
|---|---|---|
| baseline | 0 | 0 |
| device added | 1 | 1 |
| stream added | 2 | 2 |
| device removed | 3 | 3 |

Topology changes cause both counters to advance.

---

# 5. Combined sequence

Example runtime progression:

| Event | version | topology_version |
|---|---|---|
| baseline | 0 | 0 |
| device discovered | 1 | 1 |
| stream created | 2 | 2 |
| frame updates | 3 | 2 |
| frame updates | 4 | 2 |
| stream destroyed | 5 | 3 |
| device removed | 6 | 4 |

Interpretation:

- `version` shows observable state progression.
- `topology_version` indicates structural changes.

---

# 6. Consumer pattern

Typical consumer logic:

func _on_state_published(gen, version, topology_version):
    var snap = CamBANGServer.get_state_snapshot()

    if topology_version != last_topology_version:
        rebuild_topology(snap)
        last_topology_version = topology_version

    update_runtime_metrics(snap)

This avoids rebuilding topology unnecessarily while still tracking runtime updates.

---

# 7. Key invariant

topology_version changes
        ⇒
version must change

But:

version may change
without topology_version changing
