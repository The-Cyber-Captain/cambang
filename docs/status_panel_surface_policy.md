# Status Panel Snapshot Surface Obligation Policy

## Purpose

This policy defines **non-negotiable rendering obligations** for snapshot-backed Status Panel UI surfaces.
It is a guard-rail for future changes and review.

Scope: status-panel row rendering and fixture expectations derived from snapshot fields.

---

## Tier System

### Tier 1 — State-defining (must surface directly)

Fields that determine how a row is interpreted.

Examples:
- `phase`
- `mode`
- contract/error indicators

**Rule:**
- When present in snapshot/runtime projection truth, Tier 1 fields **must be rendered as badges**.
- Tier 1 fields must not be hidden behind detail-only text.

### Tier 2 — Quantitative / configuration (must be visible)

Measurable or configuration values.

Examples:
- dimensions / formats / fps
- counters (frames, drops, queue)
- still capture configuration

**Rule:**
- Tier 2 fields must appear as either:
  1. **Counters (preferred)**, or
  2. **A single, non-duplicated info surface**.
- Tier 2 fields must not be duplicated across badge/counter/info surfaces.

### Tier 3 — Structural / relational (must be traceable)

Relationship and lineage fields.

Examples:
- `owner_stream_id`
- `creation_gen`
- rig membership
- detached root membership

**Rule:**
- Tier 3 information must be:
  - visible via structure (tree placement / row identity), **or**
  - shown explicitly when diagnostically relevant.

---

## Core Invariants

1. **No snapshot field may be silently dropped.**
2. **No duplication across surfaces.**
3. **UI must reflect, not invent.**
4. **Derived indicators must be traceable to snapshot inputs.**
5. **Fixtures validate the current renderer contract, not future intent.**

---

## Current Known Gaps

- **Tier 1 gap:** rig `mode` is currently not surfaced in row UI (known follow-up).
- Additional Tier 1 gaps should be recorded here as they are discovered in implementation reviews.

---

## Enforcement Notes

- Any status-panel UI change should identify which tier each affected field belongs to.
- PRs should explicitly call out whether a change:
  - preserves existing obligations,
  - closes a known gap, or
  - introduces a temporary exception (must be documented and justified).
