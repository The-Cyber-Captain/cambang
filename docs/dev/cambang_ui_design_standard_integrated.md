# CamBANG UI Design Standard

## Status

Prototyping baseline.

This document defines the architectural rules, global UI language, and component-specific guidance governing CamBANG UI during the prototyping phase.

The goal is to ensure UI work:

- preserves runtime truth
- remains compatible with future C++ implementation
- follows a consistent UI language across all CamBANG UI nodes
- remains compliant with the host Godot project
- prioritises diagnostic clarity over decoration

This document intentionally proceeds in two layers:

1. **Global UI rules** shared by all CamBANG UI nodes
2. **CamBANGStatusPanel-specific rules** for the first concrete UI component

---

# 1. Scope

This document applies to all CamBANG UI components including:

- `CamBANGStatusPanel`
- future diagnostic panels
- inspection panels
- monitoring widgets
- any UI consuming `CamBANGServer` state snapshots

This document intentionally separates:

1. **Global UI rules**
2. **Component-specific UI designs**

Component-specific sections may refine behaviour, but must not contradict the global rules.

---

# 2. Source of Truth

## 2.1 Public truth boundary

A CamBANG UI node must treat the **`CamBANGServer` public surface** as its only runtime authority.

During this phase the canonical runtime inputs are:

- `state_published(...)`
- `get_state_snapshot()`

UI nodes must **not depend on**:

- provider internals
- core internals
- private implementation hooks
- diagnostic backdoors

for runtime truth.

---

## 2.2 Snapshot authority

The snapshot represents **published runtime truth**.

UI nodes must not:

- invent semantics
- patch missing runtime truth
- infer relationships not present in the snapshot
- silently repair incomplete data

If runtime truth is incomplete, the UI must **surface the gap**, not compensate for it.

---

## 2.3 Unknown beats wrong

If a value cannot be truthfully derived from the snapshot, it must be surfaced as one of:

- `unknown`
- `not published`
- `ambiguous`
- `unsupported`

The UI must **never substitute a plausible guess**.

---

# 3. UI Node Responsibilities

## 3.1 Consumer, not authority

A CamBANG UI node is a **consumer of published truth**.

It may:

- observe runtime publications
- fetch snapshots
- project snapshot data into presentation models
- maintain visual-only state
- render widgets

It must not:

- define runtime semantics
- repair missing core truth in presentation code
- introduce hidden hierarchy rules

---

## 3.2 Diagnostic-first orientation

CamBANG UI exists primarily for **diagnostics and architecture validation**.

Therefore UI design prioritises:

- clarity
- truthfulness
- stability
- inspectability

over polish or decoration.

---

# 4. Update Model

UI must distinguish **runtime truth updates** from **visual updates**.

## 4.1 SnapshotState

Derived only from snapshot publication events.

## 4.2 VisualState

Derived only from visual behaviour such as:

- animation clocks
- fade timers
- elapsed-time displays

VisualState must **never alter snapshot truth**.

## 4.3 Snapshot ingestion

SnapshotState must update **only when a new publication is observed**.

Standard ingestion flow:

1. observe `state_published(...)`
2. call `get_state_snapshot()`
3. compare `(gen, version, topology_version)`
4. update snapshot-derived structures

The event signal itself must **not be trusted as the source of truth**. The snapshot must always be re-fetched.

## 4.4 Update categories

UI work must be categorised as:

### Structural rebuild

Triggered by:

- `gen` change
- `topology_version` change

### Value refresh

Triggered by:

- `version` change

### Visual tick

Triggered by:

- `_process(delta)`

A visual tick may advance presentation timing without implying structural rebuild or value refresh.

---

# 5. Lifecycle and Destroyed Objects

## 5.1 DESTROYED is meaningful truth

Destroyed objects remaining visible in snapshot state are **valid diagnostic truth**.

UI must not automatically hide them.

## 5.2 Retirement while server is live

While `CamBANGServer` is running, retirement of destroyed objects is expected to arrive through normal snapshot publication.

UI timers must **not** be relied upon for correctness.

## 5.3 Post-stop visual aftercare

After `CamBANGServer` stops, `_process()` may continue driving visual behaviour from the final snapshot.

Examples:

- fading destroyed rows
- visual expiry of temporary elements
- elapsed-time labels

## 5.4 Visual expiry is presentation-only

Visual removal must not alter snapshot truth.

Snapshot membership remains authoritative.

---

# 6. Global UI Rules

The following rules define the **global UI language used across all CamBANG UI components**.

These rules apply to every CamBANG UI node.

---

## 6.1 Layout direction compliance

CamBANG UI must respect the active Godot **layout direction**.

UI must function correctly in:

- LTR environments
- RTL environments

Layout direction affects:

- hierarchy indentation
- expand/collapse arrows
- icon placement
- padding semantics
- start/end alignment

Implementation must prefer **logical layout semantics** rather than fixed directions such as left and right.

Examples:

| LTR | RTL |
|---|---|
| ▶ collapsed | ◀ collapsed |
| ▼ expanded | ▼ expanded |

Layout direction must be treated as **live UI state**, not startup-only configuration.

---

## 6.2 Theme compliance

CamBANG UI must comply with the **active Godot theme**.

UI must not hardcode:

- colours
- fonts
- styling values

Instead CamBANG UI uses **semantic tokens** whose resolved values may be supplied by the active theme.

### 6.2.1 Theme resolution order

Visual values resolve in this order:

1. explicit CamBANG override
2. value supplied by the active theme
3. CamBANG fallback default

This ensures components remain usable even when the active theme does not define every desired value.

---

## 6.3 Badge system

CamBANG UI uses **badges** to represent qualitative runtime state.

A badge is a **coloured indicator element**.

The default badge form is:

- rounded rectangular indicator
- no internal text
- colour-coded according to semantic role

Specific state wording is provided by an adjacent **badge label**.

Example:

```text
[green badge] RUNNING
[amber badge] WARN
[red badge] BROKEN
```

Badge plus adjacent label together form a **badge pair**.

Alternate badge forms such as dots or icon badges may be introduced later, but must follow the same semantic rules.

### 6.3.1 Badge semantic roles

Badge colours represent **diagnostic polarity**, not specific state names.

| Role | Default colour | Meaning |
|---|---|---|
| success | green | healthy / expected |
| warning | amber | caution / degraded |
| error | red | fault / contradiction |
| neutral | grey | inactive / historical |
| info | blue | informational / structural |

These colours represent **CamBANG defaults**.

Actual colours must be resolved through the active theme.

Themes may override these colours, but must preserve the semantic role.

Example semantic tokens:

```text
badge_success
badge_warning
badge_error
badge_neutral
badge_info
```

### 6.3.2 Badge accessibility

Colour must not be the sole carrier of meaning.

Important state must also be communicated through at least one additional signal such as:

- badge label text
- icon
- tooltip
- contextual UI text

---

## 6.4 Counters

Numeric values displayed in CamBANG UI are called **counters**.

Counters represent quantitative values associated with an object.

Counters consist of:

- a **counter label**
- a **counter value box**

Example:

```text
OBJ 16
ERR 2
Q   0
FR 198
```

Counters must maintain:

- fixed display width appropriate to the counter type
- stable digit capacity
- no layout jitter during updates

Counters represent **quantitative values**, while badges represent **qualitative state**.

### 6.4.1 Bounded-width counter rendering

Counter display width is bounded by the counter type's reserved digit capacity.

If a value exceeds the directly representable decimal width, the UI must use a deterministic compact overflow rule rather than expanding the counter box.

Examples:

```text
digits = 1
15 -> 9+

digits = 2
237 -> 99+

digits = 3
2378   -> 2k+
37367  -> 37k+
104552 -> 99k+

digits = 4
37367  -> 37k+
104552 -> 104k+
```

Rules:

- the trailing `+` indicates the displayed value is truncated
- `k` consumes one character position within the compact display budget
- overflow formatting is presentation-only
- the underlying counter value remains the full truthful integer
- counter boxes must reserve width for the largest formatted token they may display

This rule exists to preserve stable layout without losing the ability to convey large counter values.

---

## 6.5 Diagnostic state model

CamBANG UI separates:

- **operational state**
- **diagnostic condition**

Operational state examples:

```text
RUNNING
STOPPED
STARTING
BROKEN
```

Diagnostic condition examples:

```text
WARN
ERR
STALE
DEGRADED
```

UI elements may display multiple badge pairs simultaneously.

Example:

```text
[green badge] RUNNING
[amber badge] WARN
```

This allows operational state and health state to be represented independently.

---

## 6.6 Stable layout principle

CamBANG UI must prioritise **visual stability**.

Elements must not shift position during runtime updates.

Examples include:

- counter widths
- badge size within a given component
- hierarchy indentation
- row alignment

Visual jitter reduces diagnostic readability and must be avoided.

---

## 6.7 Accessibility and legibility

CamBANG UI prioritises:

- readability
- stability
- predictable layout
- information density

Animation and visual effects must remain minimal and must never interfere with diagnostic clarity.

---

# 7. CamBANGStatusPanel

This section describes the **specific design and implementation rules** for the first CamBANG UI component.

This section builds upon the global UI rules above.

The status panel must not redefine the shared UI language. It must use it.

---

## 7.1 Purpose of the status panel

`CamBANGStatusPanel` is the first diagnostic UI node.

Its purpose during the prototyping phase is to:

- consume `CamBANGServer` truth
- pressure-test the snapshot contract
- expose topology and lifecycle structure clearly
- reveal unsupported or ambiguous cases rather than hiding them

---

## 7.2 Initial rendering scope

The first status panel should render at minimum:

- snapshot header
- snapshot presence / `NIL` state
- topology summary
- devices
- streams
- native objects
- detached roots
- contract-gap notes

This scope exists to pressure-test the snapshot contract rather than to produce a polished end-user panel.

---

## 7.3 Projection layer required

The status panel should separate three layers:

1. boundary intake
2. snapshot projection / presentation model
3. UI node rendering

UI nodes should render from a presentation model rather than parsing raw snapshot structures directly.

---

## 7.4 Stable presentation model

Snapshot data should be projected into stable internal sections such as:

- header
- topology summary
- rigs
- devices
- streams
- native objects
- detached roots
- warnings / contract gaps

These are **internal projection sections**.

They do not imply that every section becomes a visible wrapper in the rendered hierarchy.

This internal structure should be preserved during the later C++ rewrite.

---

## 7.5 Contract-gap reporting

If the presentation layer cannot represent snapshot content cleanly, the issue should be surfaced as a contract gap such as:

- missing field
- ambiguous identity
- unclear naming
- missing semantic
- unsupported snapshot schema

These issues must not be hidden.

---

## 7.6 Status entry layout grammar

The panel’s primary visible unit is a **StatusEntry**.

A StatusEntry represents one object in the visible hierarchy.

Each StatusEntry is composed of two primary regions:

```text
IdentitySegment | InfoPanel
```

The IdentitySegment communicates hierarchy and identity, while the InfoPanel contains the runtime state and numeric counters associated with the object.

This separation keeps structural identity visually distinct from diagnostic information.

StatusEntry is a presentation concept used by the panel renderer. It is not a separate runtime object or public truth source.

### 7.6.1 IdentitySegment

The **IdentitySegment** communicates hierarchy and object identity.

It contains:

- disclosure arrow
- object label

Example:

```text
▼ Looper
```

Responsibilities:

- hierarchy indentation
- expand / collapse affordance
- object name display

The IdentitySegment represents **structural identity**, not runtime state.

### 7.6.2 InfoPanel

The **InfoPanel** is the inset container that displays runtime information associated with the object.

It visually groups state indicators and numeric counters.

Structure:

```text
InfoPanel
  ├ StateSegment
  └ CounterSegment
```

The InfoPanel typically appears as a rounded inset container.

Both segments must respect the active layout direction (LTR or RTL).

### 7.6.3 StateSegment

The **StateSegment** displays qualitative runtime state.

It contains one or more **badge pairs**.

Example:

```text
[green badge] RUNNING
[amber badge] ERR
```

Each badge pair consists of:

```text
Badge + BadgeLabel
```

Where:

- **Badge** is the coloured indicator element
- **BadgeLabel** is the adjacent textual state description

StateSegment may contain multiple badge pairs simultaneously to represent different diagnostic axes.

Example:

```text
RUNNING + WARN
RUNNING + ERR
```

The StateSegment communicates **qualitative state**, not numeric values.

### 7.6.4 CounterSegment

The **CounterSegment** displays quantitative values associated with the object.

It contains a collection of **Counters**.

Example:

```text
OBJ 16   ERR 2   Q 0   FR 198
```

Each Counter is structured as:

```text
Counter
  CounterLabel
  CounterValueBox
```

Where:

- **CounterLabel** appears above the numeric value
- **CounterValueBox** contains the numeric display

Example:

```text
OBJ
[16]
```

Counters must maintain:

- stable digit width
- fixed layout
- no runtime jitter

CounterValueBox must satisfy the bounded-width overflow rule defined in section 6.4.1.

### 7.6.5 Counter availability

The CounterSegment is expected to contain counters that prove useful for routine diagnostic inspection.

However, the exact initial counter inventory is **not fixed by this document**.

Counters rendered by the panel must be derived from:

- the current snapshot schema
- observed runtime behaviour
- future schema revisions where additional counters may be published

The UI must therefore support a variable counter set without assuming the initial inventory is complete.

Optional counters may later be enabled through exported configuration options on the UI node once real system usage reveals which counters are most useful.

The StatusEntry layout must accommodate growth of the CounterSegment without introducing layout jitter.

### 7.6.6 Layout behaviour

StatusEntry layout must remain stable while counters and state change.

Key requirements:

- badge sizes remain stable within a component
- counter boxes maintain fixed width
- enabling additional counters expands layout without shifting existing elements
- layout must respect the active **LTR / RTL direction**

In LTR environments:

```text
IdentitySegment | InfoPanel(StateSegment | CounterSegment)
```

In RTL environments the layout must mirror appropriately.

The InfoPanel must expand to accommodate additional counters without shifting the IdentitySegment.

---

## 7.7 StatusEntry presentation model

To keep UI implementation simple and maintain architectural separation from the runtime core, the status panel must render from a **presentation model** rather than directly traversing snapshot structures.

The presentation model acts as a stable intermediate representation between:

- the snapshot schema
- the UI node tree

The projection pipeline is therefore:

```text
snapshot
→ presentation model
→ UI nodes
```

The presentation model must contain all information necessary for rendering a StatusEntry without requiring further snapshot inspection.

### 7.7.1 Panel model

The entire panel renders from a **PanelModel**.

Example conceptual structure:

```text
PanelModel
  entries[]
```

Each element of `entries[]` represents one **StatusEntryModel**.

Entries are ordered according to the hierarchy projection.

The panel should not perform hierarchy calculations during rendering.

Hierarchy structure must be resolved during projection.

### 7.7.2 StatusEntryModel

Each visible row in the panel corresponds to a **StatusEntryModel**.

Example structure:

```text
StatusEntryModel
  id
  parent_id
  depth
  label
  expanded
  badges[]
  counters[]
```

Field meanings:

| Field | Purpose |
|------|------|
| `id` | unique identifier for the entry |
| `parent_id` | identifier of the parent entry |
| `depth` | hierarchy depth used for indentation |
| `label` | object name displayed in the IdentitySegment |
| `expanded` | current disclosure state used by the UI |
| `badges[]` | collection of badge pairs displayed in the StateSegment |
| `counters[]` | collection of counters displayed in the CounterSegment |

This structure contains all information required to render a StatusEntry.

Additional optional fields may exist for rendering convenience, such as:

```text
can_expand
info_lines[]
kind
```

Such fields remain presentation-only and must not be treated as runtime truth.

### 7.7.3 Badge model

Each element of `badges[]` represents a **BadgePair**.

Example structure:

```text
BadgeModel
  role
  label
```

Where:

| Field | Meaning |
|------|------|
| `role` | badge semantic role (`success`, `warning`, `error`, `neutral`, `info`) |
| `label` | text displayed beside the badge |

Example:

```text
role: success
label: RUNNING
```

Multiple badge pairs may appear simultaneously within the StateSegment.

### 7.7.4 Counter model

Each element of `counters[]` represents a **Counter**.

Example structure:

```text
CounterModel
  name
  value
  digits
```

Where:

| Field | Meaning |
|------|------|
| `name` | counter label displayed above the value |
| `value` | full truthful integer value |
| `digits` | compact display width policy for the value box |

Example:

```text
name: ERR
value: 2
digits: 2
```

The `digits` field allows counters to reserve a stable width in the UI and prevent layout jitter when values change.

The bounded-width overflow behaviour is a **rendering rule**, not a change to runtime truth or projection semantics.

The appropriate digit width for each counter type should be determined after observing real runtime behaviour.

### 7.7.5 Projection responsibilities

The projection layer is responsible for:

- walking the snapshot
- resolving hierarchy relationships
- constructing StatusEntryModel objects
- flattening hierarchy into display order
- inserting orphan branches where necessary
- mapping runtime state into badge roles
- mapping published numeric values into counters

The projection layer must **not** perform rendering.

UI nodes must **not** traverse the snapshot directly.

### 7.7.6 Projection truth rule

The projection layer must not invent runtime semantics.

Projection may:

- translate snapshot structures into presentation structures
- map snapshot values into badge roles
- expose published numeric values as counters
- aggregate directly published values where the aggregation itself is truthful and mechanically defined

Projection must not:

- infer relationships not present in the snapshot
- synthesise counters that do not originate from runtime truth
- hide or reinterpret contradictory snapshot data

If the snapshot contains ambiguous or unsupported information, the panel should surface the issue rather than silently correcting it.

### 7.7.7 Rendering contract

Rendering should become almost mechanical once the presentation model exists.

The panel should perform the following steps:

```text
1. observe state_published(...)
2. fetch snapshot
3. build presentation model
4. diff existing entries against model
5. create / update / remove StatusEntry UI nodes
```

Each StatusEntry UI node renders directly from its corresponding `StatusEntryModel`.

The UI layer must not derive additional semantics beyond those already contained in the presentation model.

### 7.7.8 VisualState separation

Visual-only behaviour must remain outside the presentation model.

Example structure:

```text
VisualState
  fadeTimers
  animationClocks
  destroyedAge
```

VisualState must never modify:

- the presentation model
- snapshot truth

---

## 7.8 Relationship rules

The status panel must use **explicit relationships** published in the snapshot.

Parentage must not be inferred from:

- naming
- ordering
- implementation accidents

Detached roots are diagnostically important and must remain inspectable.

Frame visibility must remain visually separate from snapshot-truth sections. Frame delivery is not equivalent to runtime state truth.

---

## 7.9 Core hierarchy principle

The visible hierarchy must primarily follow the **Godot-facing object model**, because that is how users reason about the system.

The visible root is the **Server**, representing the discovered `CamBANGServer` singleton.

Beneath Server, the runtime loop is represented by **Provider nodes**.

Each Provider row is **grounded in the provider `native_object` published in the snapshot**.

Every snapshot is expected to contain a provider native object representing the currently active runtime provider.

The Provider row therefore serves two simultaneous roles:

1. it **represents the provider native object itself**, preserving its identity, lifecycle state, and counters as published in the snapshot
2. it acts as the **presentation container** beneath which projected Godot-facing objects and other native objects appear

This means the Provider row is not a synthetic grouping node and does not imply the existence of a separate top-level provider record in the snapshot schema.

Instead, it is a **truth-preserving projection** of the provider native object into the UI hierarchy.

Although normally only one Provider is active, the UI must allow multiple Providers to appear simultaneously so that DESTROYED or leaked providers remain visible until their lifecycle truly ends.

To avoid unnecessary depth and category clutter, the hierarchy should be **collapsed rather than category-bucketed**.

In particular, the UI should not require explicit intermediate branches such as:

- `Devices`
- `Rigs`
- `Streams`
- `Native objects`

Instead, child rows should appear directly beneath their owning visible parent.

Meaning should be conveyed by:

- hierarchy position
- label text
- unique identifier in the label
- colour coding by concept / classification
- contextual counters and state fields

A device that is a rig member may need to appear in **two visible contexts simultaneously**:

1. as a Device in its own right beneath the Provider
2. as that same Device beneath its Rig, where rig-owned pipelines and native objects are shown

This dual appearance represents two distinct published truths:

- device existence / ownership
- rig membership / rig-owned capture context

It must not be treated as two independent runtime devices.

Example structure:

```text
Server
 ├─ Provider
 │   ├─ Device [id]
 │   │   ├─ Stream [id]
 │   │   │   ├─ FrameProvider [id]
 │   │   │   ├─ FormatConverter [id]
 │   │   │   └─ Renderer [id]
 │   │   └─ Device-owned native object [id]
 │   ├─ Rig [id]
 │   │   └─ Device [id]
 │   │       └─ Rig-owned native object [id]
 │   └─ Orphaned native objects
 └─ Provider [id] (DESTROYED)
```

Provider rows carry runtime metadata such as lifecycle state and snapshot generation / version information.

---

## 7.10 Native object placement

Native objects must appear under the **visible parent whose pipeline or lifecycle owns them**.

The UI should therefore prefer direct placement beneath the owning visible node, rather than introducing an extra `Native objects` category branch.

Provider-scoped objects appear directly beneath **Provider**.

Device-specific objects appear directly beneath the **Device** that owns them.

Rig-triggered capture objects for a rig-member device appear beneath **Rig → Device**, not beneath the standalone Device branch.

Stream pipeline objects appear directly beneath the **Stream** whose capture or preview pipeline created them.

For example, a platform object such as `ACameraManager` created by a camera2-backed Provider would appear directly beneath the Provider node, because it is provider-scoped rather than belonging to a specific device or stream.

Example:

```text
Server
 └─ Provider [id]
     └─ ACameraManager [id]
```

This preserves truthful ownership and avoids forcing platform concepts into inappropriate hierarchy levels or unnecessary category wrappers.

If the snapshot does not publish enough information to distinguish origin accurately, the UI must not guess. It should place the object beneath its canonical owner and surface the ambiguity as a contract gap or diagnostic note.

---

## 7.11 Detached and orphaned branches

If an owning object disappears but native objects remain, the remaining branch must remain visible.

Such branches appear under the **Orphaned native objects** group beneath the Provider.

This preserves the diagnostic ability to observe delayed teardown or lifecycle leaks.

The orphan group is retained because it communicates a distinct diagnostic condition, unlike the collapsed category wrappers above.

---

## 7.12 Visual state storage

Visual-only information should live outside the presentation model.

Examples:

```text
VisualState
  destroyedFadeTimers
  animationTimers
  ageClocks
```

VisualState must never alter the presentation model itself.

---

## 7.13 GDScript prototyping rule

GDScript is acceptable during this phase to discover:

- layout
- snapshot consumption patterns
- UI vocabulary
- missing snapshot semantics

However, the prototype must follow the same architectural rules expected of the final C++ implementation.

The prototype should allow a later C++ rewrite that preserves:

- inputs
- update rules
- presentation model
- behaviour

---

## 7.14 Dev-only adversarial snapshot playback

SyntheticProvider scenarios may eventually become too correct to simulate UI failure cases.

For UI hardening, a dev-only snapshot playback or mimic for `CamBANGServer` may be used.

### Allowed uses

A dev-only mimic may generate snapshots representing:

- lifecycle leaks
- broken teardown shapes
- contradictory states
- unsupported schema versions
- malformed snapshot content

### Containment rule

This mimic must remain strictly **dev-only**.

Production UI nodes must always consume the real `CamBANGServer` surface.

### Hardening goal

Adversarial playback should prove that the UI:

- fails visibly
- distinguishes truth from uncertainty
- remains usable under hostile inputs
- does not crash on malformed snapshots

---

## 7.15 Non-goals for the status panel

During this phase the status panel should not:

- become a control surface
- invent missing runtime semantics
- depend on private runtime hooks
- suppress lifecycle diagnostics

---

## 7.16 Open questions for later refinement

Areas expected to evolve in later revisions include:

- fade policy for destroyed rows
- redraw thresholds
- standard section ordering within the panel
- badge vocabulary where the panel needs more specific status wording
- contract-gap presentation details
- exact proportions and sizing heuristics for status-entry layout

---

# 8. Status

This document represents the **prototyping-phase baseline** for CamBANG UI.

It should evolve deliberately as additional UI components are designed and implemented.
