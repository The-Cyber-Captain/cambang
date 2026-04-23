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
- ownership (`owner_acquisition_session_id`, `owner_stream_id`)
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


### 3.1 Current implemented hierarchy note

Current StatusPanel projection is AcquisitionSession-aware and uses CamBANG's imposed viewing structure for intelligibility over provider-reported native truth.

Current ancestry reconstruction includes:

- `Provider -> Device -> AcquisitionSession -> Stream -> optional FrameProducer`
- `Provider -> Device -> AcquisitionSession -> optional FrameProducer`

Rows of the form `acquisition_session/<id>` are first-class projection entries.
`owner_acquisition_session_id` is used in ancestry reconstruction for native rows, including legitimate acquisition-session-owned `FrameProducer` rows with no intermediate `Stream` ownership.

When descendants survive beyond an ended controlling AcquisitionSession seam,
the panel must preserve explicit **Acquisition Session boundary breach**
classification rather than collapsing that condition into a generic stream breach.

### Orphaned placement and boundary-breach indication

Rows that cannot be shown beneath an expected live parent may require
orphan-style placement.

When that condition exists because a descendant survived beyond a
meaningful controlling seam, the panel should preserve an explicit
boundary-breach indication rather than collapsing the condition into
generic orphan handling alone.

Visible anomaly vocabulary should remain compact:

- **orphaned** expresses the grouping/placement consequence
- **boundary breach** expresses the specific diagnostic subtype

More specific causal distinctions may remain internal or detail-only.

### Context placement for resource-bearing native truth

When snapshot truth includes additional provider-owned resource-bearing native
rows, the panel should group them by the context that called them into being.

Preferred placement:

- beneath the owning `Stream` context for stream-originated resource truth
- beneath the owning `AcquisitionSession` context for capture-originated
  resource truth

This grouping does not depend on `FrameProducer`.
If normal live-parent placement is unavailable, the panel may fall back to
orphaned placement and, where appropriate, preserve a compact
boundary-breach indication.

## 4) Optional field handling

- If a field is present in snapshot truth, it must satisfy its tier obligation.
- If a field is absent, UI must not invent placeholders/default fake values.

---

## 5) Tier 3 Presentation Rules

### 5.1 Structural-first principle

If Tier 3 truth is already clear via hierarchy/grouping/containment, textual repetition is optional, not mandatory.

### 5.2 Reinforcement rule (controlled duplication)

Tier 3 reinforcement is allowed only when it adds clarity.

- Allowed example direction: AcquisitionSession/FrameProducer ownership can be reinforced by structure plus a concise info line.
- Not allowed: repeating the same fact in multiple text forms without added explanatory value.

Rule: reinforcement must add clarity, not restate structure verbatim.

### 5.3 Density control (summary vs detail)

Two conceptual presentation levels:

- **Summary (default):** minimal badges, minimal info, high-signal context only.
- **Detail (expanded/diagnostic):** full lineage, explanatory metadata, reason text.

Rule: verbose Tier 3 metadata should be eligible for detail-only display.

### 5.4 Terminology normalization

Use one canonical term per concept family.

Preferred distinctions:

- **continuity-only** — a row is shown for continuity rather than as
  current active truth
- **detached** — structural/topology separation from expected live structure
- **orphaned** — projection/grouping consequence when a row cannot be
  shown beneath its expected live parent
- **boundary breach** — explicit diagnostic subtype for survival beyond a
  meaningful controlling seam

Rules:

- **continuity-only** is the single preferred continuity term in both
  surfaced panel truth and panel/projection logic.
- **retained** should not be used as the preferred continuity-state term
  in panel vocabulary where **continuity-only** is intended.
- **orphaned** is not itself the full diagnosis; it is a
  projection/grouping consequence.
- **boundary breach** is not a generic synonym for **orphaned**.
- A row may be orphaned without being a boundary breach.
- A boundary breach may require orphan-style placement, but the two
  terms are not interchangeable.
- Exact seam identity may remain internal or detail-only unless broader
  surfacing is clearly justified.

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


## Health Summary Badge Semantics

### Purpose

The health summary badge is a **single leading badge per row** and must always appear first in row badge order.

It is a **derived, row-type-specific, viewer-facing summary** that provides a fast attention cue for the row as presented. It must remain explainable from already-visible row truth.

The health summary badge is additive and does not replace detailed badges, counters, or info lines. Detailed row truth remains authoritative; the health summary badge is only a summary layer above that truth.

### Semantics

The health summary badge is not a simple reduction of technical liveness or engineering purity.

- Non-live does not automatically mean non-OK.
- Continuity-only or destroyed rows may still be OK when coherent and expected in context.
- Derivation must emphasize coherence, expectedness, and viewer concern/attention-worthiness rather than mere deviation from an ideal live state.

### Canonical provisional label set

The canonical provisional health-summary labels are:

- `UNKNOWN`
- `OK`
- `ATTN`
- `BAD`

No additional health-summary labels are introduced in this policy pass.

### Label meanings

- `UNKNOWN`: The panel cannot yet make a confident viewer-facing judgment for the row.
- `OK`: The row is coherent and acceptable as presented to the viewer, even if not actively live.
- `ATTN`: The row is intelligible and not necessarily broken, but deserves notice or monitoring.
- `BAD`: The row is broken, contradictory, or strongly concerning and should command viewer attention.

### Derivation principles

Health derivation must be:

- deterministic
- documented
- row-type-specific
- traceable to already-surfaced truth
- conservative

Additional constraints:

- Until a row type has an explicitly documented health rule, its health should remain `UNKNOWN`.
- Health must not invent new hidden semantics.
- Health should never contradict the row's detailed visible truth.

### Relationship to contract/projection gaps

Unmapped or unexpected schema truth must surface as explicit contract/projection gap signals and must not be silently ignored.

Contract/projection gaps are expected to be important inputs to health derivation; however, the health badge does not replace direct visibility of those gap signals.

This preserves the existing principle: surface the gap, do not mask it.


## Health Summary Framework Rules

These framework rules define how future health-summary derivation must be structured in panel presentation logic.

### 1) Health scope is strictly row-local

A row health summary must be derived only from that row's own:

- payload truth
- contract/projection truth
- row-type-specific derived checks
- prior observed values, when a row-local history/rate rule is explicitly defined

Health must not be derived from descendant rows, subtree aggregate health, or bubbled-up child health states.

Problems deeper in the tree must continue to surface via structure and explicit diagnostic/anomaly rows rather than by parent-row health aggregation.

### 2) History basis is the Godot-visible observable series only

Any health rule using deltas, growth, rate, or thresholded evolution over time must compare the row's current observed state against prior observed row state from previous visible panel updates.

The basis is the Godot-visible publication sequence, not internal core/publication churn hidden within a tick.

### 3) Threshold configuration is project-configurable and optional

Optional thresholded health checks may be configured by the embedding project/application through panel-facing configuration rather than hardcoded per project.

Constraints:

- thresholded checks are row-type-specific
- individual checks may be disabled by configuration
- the conventional meaning of threshold value `0` is: disable that optional thresholded check

### 4) Health evaluation uses a lightweight panel-owned rule registry

Health-summary evaluation must use a panel-owned rule framework/registry rather than ad hoc per-row branching scattered across rendering paths.

Required model:

- one uniform mechanism
- row-type-specific rule sets
- priority-ordered evaluation
- first applicable rule wins
- otherwise fall back to default health (`UNKNOWN` until explicitly defined)

This registry is presentation logic, not snapshot schema truth, and should remain lightweight (policy/rule definitions rather than a heavy class hierarchy).


## 6) Known Tier 3 Issues

- Cognitive load remains high where continuity-only and orphaned states coincide.
- Density is high in continuity-only/orphan subtree diagnostics (multiple badges + counters + reason text at once).
- Reinforcement boundaries are not fully standardized (when structure-only is enough vs when explicit line-level reinforcement is preferred).

---

## 7) Current known gaps / ambiguities (current implementation)

- **Tier 1:** No confirmed missing Tier 1 direct surfaces for rows backed by canonical device/stream/rig snapshot records.
- **Rig mode:** now surfaced as `mode=<VALUE>` badge when `rig.mode` exists (no longer a known gap).
- **Tier 2 ambiguity:** some provider/native aggregate counters (for example `native_*`) are renderer-derived rollups over snapshot arrays, not direct scalar fields; this is acceptable when traceable.
- **Tier 3 ambiguity:** several native-object ownership/lineage fields are only partially surfaced (`owner_acquisition_session_id`/`owner_stream_id`/`creation_gen` emphasized for frameproducer; other owner fields are mostly structural/diagnostic).

---

## 8) Schema → UI surface audit (current renderer/scene projection, read-only)

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
| native object (generic) | owner_acquisition_session_id/owner_stream_id/owner_device_instance_id/root_id | 3 | traceable | structural parent/orphan grouping | no | correct | keep traceable |
| native object (generic) | owner_provider_native_id/owner_rig_id/created_ns/destroyed_ns | 3/2 | no | missing | n/a | ambiguous | policy decision required |
| frameproducer | owner_acquisition_session_id/owner_stream_id | 3 | yes | info line + structural parent | no | correct | none |
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
