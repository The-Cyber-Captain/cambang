# Status Panel Snapshot Surface Obligation Policy (Canonical)

## Purpose

This document is the canonical, in-repo policy for how snapshot truth must be surfaced by the Status Panel UI.

It is a **guard-rail**, not optional guidance.

---

## 1) Three-tier surface model

### Tier 1 — State-defining

State-defining fields determine how a row is interpreted and must surface directly.

Examples:
- `phase`
- `mode`
- contract/error indicators

**Obligation:**
- Tier 1 fields must surface as **badges** whenever present.

### Tier 2 — Quantitative / configuration

Quantitative or configuration fields must be visible exactly once.

Examples:
- dimensions, formats, fps
- frame counts, drops, queue
- capture/still configuration

**Obligation:**
- Must be visible exactly once via:
  1. **counter** (preferred), or
  2. a single non-duplicated info line.

### Tier 3 — Structural / relational

Structural/relational fields must be traceable.

May be surfaced as:
- structural (hierarchy)
- contextual
- diagnostic (info lines)

Examples:
- ownership (`owner_stream_id`)
- lineage (`creation_gen`)
- rig membership
- detached/orphan grouping

**Obligation:**
- Must be traceable either directly or through deterministic UI structure/diagnostics.

---

## 2) Core invariants (non-negotiable)

1. Snapshot is authoritative.
2. No snapshot truth may be silently dropped.
3. UI must reflect, not invent.
4. Derived indicators must be traceable to snapshot inputs.
5. No duplication across badges, counters, and info lines.
6. Fixtures validate the current renderer contract, not future intent.

---

## 3) Direct vs traceable surfacing

### Direct surface
A field value is directly represented as one of:
- badge
- counter
- info line

### Traceable surface
A field is traceably represented through one of:
- hierarchy/parenting
- grouping bucket (for example detached/orphan grouping)
- derived-but-explainable indicator that can be tied to snapshot inputs

---

## 4) Optional field handling

- If a field is present in snapshot truth, it must satisfy its tier obligation.
- If a field is absent, UI must not invent placeholders/default fake values.

---

## 5) Tier 3 Presentation Rules

### 5.1 Structural-first principle

If Tier 3 truth is already clear via hierarchy/grouping/containment, textual repetition is optional, not mandatory.

### 5.2 Reinforcement rule (controlled duplication)

Tier 3 reinforcement is allowed only when it adds clarity.

- Allowed example direction: FrameProducer ownership can be reinforced by structure plus a concise info line.
- Not allowed: repeating the same fact in multiple text forms without added explanatory value.

Rule: reinforcement must add clarity, not restate structure verbatim.

### 5.3 Density control (summary vs detail)

Two conceptual presentation levels:

- **Summary (default):** minimal badges, minimal info, high-signal context only.
- **Detail (expanded/diagnostic):** full lineage, explanatory metadata, reason text.

Rule: verbose Tier 3 metadata should be eligible for detail-only display.

### 5.4 Terminology normalization

Use controlled vocabulary with one canonical term per concept family.

Required distinctions:
- root vs non-root
- orphaned vs detached
- continuity-only vs active

Rule: no overlapping synonyms for the same concept.

### 5.5 Badge vs label vs info rule

- **Badges:** state classification.
- **Labels:** identity/context.
- **Info lines:** explanation/causality.

Rule: do not encode the same concept across multiple surface types unless it satisfies the reinforcement rule.

### 5.6 Essential vs optional Tier 3

**Essential (always visible):**
- orphan/detached grouping
- contract/projection gaps
- ownership when not obvious structurally

**Optional / conditional:**
- verbose lineage text
- explanatory reason strings
- redundant reinforcement

---

## 6) Known Tier 3 Issues

- Terminology overlap across retained/preserved/orphaned continuity contexts still increases cognitive load.
- Density is high in retained/orphan subtree diagnostics (multiple badges + counters + reason text at once).
- Reinforcement boundaries are not fully standardized (when structure-only is enough vs when explicit line-level reinforcement is preferred).

---

## 7) Current known gaps / ambiguities (current implementation)

- **Tier 1:** No confirmed missing Tier 1 direct surfaces for rows backed by canonical device/stream/rig snapshot records.
- **Rig mode:** now surfaced as `mode=<VALUE>` badge when `rig.mode` exists (no longer a known gap).
- **Tier 2 ambiguity:** some provider/native aggregate counters (for example `native_*`) are renderer-derived rollups over snapshot arrays, not direct scalar fields; this is acceptable when traceable.
- **Tier 3 ambiguity:** several native-object ownership/lineage fields are only partially surfaced (`owner_stream_id`/`creation_gen` emphasized for frameproducer; other owner fields are mostly structural/diagnostic).

---

## 8) Schema → UI surface audit (current renderer/harness, read-only)

Legend:
- Surfaced?: `yes` / `traceable` / `no`
- Surface Type: `badge` / `counter` / `info line` / `structural` / `derived` / `missing`
- Status: `correct` / `duplicated` / `missing` / `ambiguous`
- Action: `none` / `remove duplication` / `add direct surface` / `keep traceable` / `policy decision required`

| Entity | Field | Tier | Surfaced? | Surface Type | Duplication? | Status | Action |
|---|---|---:|---|---|---|---|---|
| server | gen | 2 | yes | counter | no | correct | none |
| server | version | 2 | yes | counter | no | correct | none |
| server | topology_version | 2 | yes | counter | no | correct | none |
| server | schema validity / contract state | 1 | yes | badge | no | correct | none |
| server | detached_root_ids (count) | 3 | traceable | structural + derived counter (`roots`) | no | correct | keep traceable |
| provider | native phase (from provider native object) | 1 | yes | badge (`native_phase=...`) | no | correct | none |
| provider | provider mode (runtime/provider selection, not snapshot field) | 1 | yes | row label context | no | ambiguous | policy decision required |
| provider | rigs/devices/streams totals | 2 | yes | derived counters | no | correct | none |
| provider | native_all/native_cur/native_prev/native_dead | 2 | yes | derived counters | no | correct | none |
| rig | phase | 1 | yes | badge | no | correct | none |
| rig | mode | 1 | yes (when present) | badge | no | correct | none |
| rig | member_hardware_ids | 3 | traceable | structural child rows + `members` counter | no | correct | keep traceable |
| rig | capture_width/capture_height | 2 | yes | counters (`still_w`/`still_h`) | no | correct | none |
| rig | capture_format/capture_profile_version | 2 | no | missing | n/a | missing | add direct surface |
| rig | active_capture_id/captures_*/last_capture_*/last_sync_skew_ns/error_code | 2 | no | missing | n/a | missing | add direct surface |
| device | phase | 1 | yes | badge | no | correct | none |
| device | mode | 1 | yes | badge | no | correct | none |
| device | errors_count | 1/2 | yes | counter (`errors`) | no | correct | none |
| device | capture_width/capture_height | 2 | yes | counters (`still_w`/`still_h`) | no | correct | none |
| device | capture_format/capture_profile_version | 2 | yes | counters (`still_fmt`/`still_prof`) | no | correct | none |
| device | engaged/rig_id/hardware_id linkage | 3 | traceable | structural + label | no | correct | keep traceable |
| device | camera_spec_version/warm_hold_ms/warm_remaining_ms/rebuild_count/last_error_code | 2 | no | missing | n/a | missing | add direct surface |
| stream | phase | 1 | yes | badge | no | correct | none |
| stream | mode | 1 | yes | badge | no | correct | none |
| stream | width/height/format/profile_version/fps_min/fps_max | 2 | yes | counters | no | correct | none |
| stream | frames_received/frames_delivered/frames_dropped/queue_depth | 2 | yes | counters | no | correct | none |
| stream | visibility_frames_presented/rejected_* | 2 | yes | counters (`shown`/`rej_fmt`/`rej_inv`) | no | correct | none |
| stream | visibility_last_path | 2 | no | missing | n/a | missing | add direct surface |
| stream | intent/stop_reason/last_frame_ts_ns/device_instance_id | 3/2 | partly traceable | structural + partial counter (`last_ts`) | no | ambiguous | policy decision required |
| native object (generic) | phase | 1 | yes | badge | no | correct | none |
| native object (generic) | bytes_allocated/buffers_in_use | 2 | yes | counters | no | correct | none |
| native object (generic) | owner_stream_id/owner_device_instance_id/root_id | 3 | traceable | structural parent/orphan grouping | no | correct | keep traceable |
| native object (generic) | owner_provider_native_id/owner_rig_id/created_ns/destroyed_ns | 3/2 | no | missing | n/a | ambiguous | policy decision required |
| frameproducer | owner_stream_id | 3 | yes | info line + structural parent | no | correct | none |
| frameproducer | creation_gen | 3 | yes | info line (and prior-gen notes where applicable) | no | correct | none |
| orphan/detached structures | detached roots grouping | 3 | yes | structural row + `detached` badge + `roots` counter | no | correct | none |
| contract/projection diagnostics | contract_gaps / projection_gaps | 1 | yes | dedicated rows + warning badges + count counter | no | correct | none |

---

## 9) Audit summary

### Tier 1
- **Complete for core row entities** (provider/device/stream/rig/native/frameproducer lifecycle state surfaces).
- No confirmed missing direct Tier 1 surfaces in current renderer for canonical row entities.

### Tier 2
- No confirmed duplication cases for the previously duplicated still/visibility info lines (those lines are removed).
- Remaining Tier 2 work items are primarily **missing direct surfaces** (not duplication), e.g. several rig/device/stream/native fields listed above.

### Tier 3
- Traceability is generally sufficient for ownership and detached/orphan structure via hierarchy/grouping.
- Some lineage/ownership metadata remains ambiguous (present in snapshot but not always directly visible).

### Additional findings
- Renderer includes deliberate derived/provider summary surfaces (`native_*`, counts) that are traceable to snapshot arrays.
- Policy ambiguity remains on which currently-missing Tier 2/Tier 3 fields must be mandatory direct surfaces versus acceptable traceable-only surfaces.

---

## 10) Next actions (no implementation in this pass)

1. Decide Tier 2 minimum direct-surface set for rig/device/stream fields currently marked missing.
2. Decide explicit policy for `visibility_last_path` surfacing (counter vs info line) without reintroducing duplication.
3. Decide policy for native ownership/lineage fields currently marked ambiguous (`owner_provider_native_id`, `owner_rig_id`, timestamps).
4. After policy decisions, update fixtures to match current contract changes incrementally.
