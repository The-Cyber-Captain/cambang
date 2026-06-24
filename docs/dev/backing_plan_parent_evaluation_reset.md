# Parent-Scoped Backing Plan Evaluation Reset

This document records the current design-reset direction for Backing Plan
evaluation in CamBANG. It is a development note, not canonical architecture.

It exists to replace the older partial chooser direction with a clearer,
source-grounded model before further implementation continues.

It did not by itself redefine canonical documents such as
`provider_architecture.md`, `state_snapshot.md`, `naming.md`, or
`pixel_payload_and_result_contract.md`. Those canonical documents have now been
updated deliberately to reflect the accepted parent-scoped Backing Plan model.

Source-truth note:

This note now tracks two distinct implementation states that must not be
blurred together:

- bounded provisional seeding before or between real parent epochs; and
- real parent-owned evaluation once a concrete `Stream` or
  `AcquisitionSession` exists.

At the time of this note update, source already includes:

- parent-scoped stream evaluation;
- parent-scoped capture evaluation that migrates from a provisional
  device-scoped priming owner to a real `AcquisitionSession` owner once that
  session is observed; and
- bounded same-signature capture seed reuse for later capture epochs in the
  same runtime generation; and
- an explicit provider/Core first-use still-only priming seam that can
  proactively realize and retain a truthful provider/native
  `AcquisitionSession` seam before the user's first real capture trigger when
  the provider supports it.

---

## 1. Purpose

CamBANG's older intent-based chooser direction is now treated as a partial and
superseded implementation path.

The new direction is:

- Backing Plan evaluation is owned by the Native Payload Support Parent.
- The parent's primary function defines what "lowest cost" means.
- Evaluation and reevaluation remain bounded, topology-triggered, and
  non-per-frame.
- Result-access evidence remains real public-operation evidence, not provider
  generation/staging cost.

This reset is motivated by three issues in the older direction:

- "best posture" was too vague and allowed mismatched assumptions to creep in;
- chooser "intent" mixed topological meaning with policy meaning;
- the older terms in the `retained` family were precise in a narrow prototype
  sense, but too hard to use consistently in broader architecture and project
  communication.

---

## 2. Working Terminology

This note uses the following working terms.

### Native Payload Support Parent

The evaluation owner.

For repeating-stream work, the Native Payload Support Parent is the `Stream`.

For still-capture work, the Native Payload Support Parent is the
`AcquisitionSession`.

### Backing Plan

What Core wants a Native Payload Support Parent to produce and keep.

A Backing Plan is internal Core/provider policy state. It is not public API,
snapshot schema, provider capability advertisement, or per-call routing prose.

### Backing State

What backing actually exists on the current result or capture member now.

### Operation Support

Whether a concrete operation such as `get_display_view()`, `to_image()`, or
`to_image_member()` is actually supported on the current result or member.

### Access Evidence

Measured cost evidence gathered from real public result-access operations.

Access Evidence is evidence only. It is not Backing State, Operation Support,
snapshot truth, or provider generation/staging cost.

### Requested Plan

The currently applied or probed Backing Plan for the parent in the current
evaluation epoch.

### Steady Plan

The settled winning Backing Plan for the parent in the current evaluation
epoch.

### Posture Shape

One of the three candidate plan shapes:

- `CPU-primary`
- `GPU-primary, no CPU sidecar`
- `GPU-primary, with CPU sidecar`

### Parent Context

Use this phrase only as a fixed qualifier meaning the owning `Stream` or
`AcquisitionSession` whose capability set is relevant to evaluation.

Do not use plain `context` loosely in new design writing.

---

## 3. Defunct Terminology

The following terms are defunct for new design work and should not be extended
in new docs, comments, or implementation notes:

- `best posture`
- chooser policy `intent` states such as `Default` and `Stream-active`
- `capture lane` as the primary still-capture evaluation term
- `retained backing plan`
- `retained backing truth`
- `retained access truth`

The older source identifiers may remain temporarily during migration, but new
design work should use the working terminology in this note.

`StreamIntent` remains valid as the public purpose term for repeating streams.
This reset does not remove or rename that concept.

---

## 4. Evaluation Owner

Backing Plan evaluation is parent-scoped, not provider-wide and not per frame.

The evaluation owner is the Native Payload Support Parent:

- `Stream` for stream-originated payload/backing work
- `AcquisitionSession` for capture-originated payload/backing work

This matches the current architecture split already documented in
`provider_architecture.md`: provider/runtime capability remains the outer viable
set, while the owning parent's capability remains the evaluation input.

Rig-triggered capture does not create one global cross-device Backing Plan.
Each participating `AcquisitionSession` remains its own evaluation owner even
when capture admission is grouped across devices.

---

## 5. Decision Target

The thing being chosen is the parent's Backing Plan.

The evaluator compares the viable Posture Shapes for one parent and one
evaluation epoch.

The evaluator does not choose:

- a provider-wide default;
- a per-frame route;
- a per-user-materialization-call route;
- a Native Payload Support child resource in isolation.

The evaluator may hold two parent-scoped plan states:

- `Requested Plan`: the currently applied or probed plan
- `Steady Plan`: the settled winning plan

The Requested Plan and Steady Plan must remain distinct while evaluation is
still active.

---

## 6. Primary Function by Parent

The parent's primary function defines the score function.

### Stream

For a `Stream`, the primary function is live display delivery.

Therefore Stream-scoped Backing Plan evaluation should optimize the
`get_display_view()` path first.

`to_image()` Access Evidence for a `Stream` remains useful and should continue
to exist, but it is not the primary scoring target for Stream plan selection.

### AcquisitionSession

For an `AcquisitionSession`, the primary function is fast still-result
realization after capture is triggered.

Therefore AcquisitionSession-scoped Backing Plan evaluation should optimize the
path from accepted still-capture work to a user-meaningful completed result,
not merely the narrow cost of one later method call viewed in isolation.

Pure `to_image()` cost is not sufficient to define the AcquisitionSession
objective on its own, because still capture in CamBANG is a fast-photo UX
problem, not just a later materialization micro-benchmark.

---

## 7. Score Function

### Stream score

For `Stream`, the winning Backing Plan is the viable plan that yields the
lowest-cost successful `get_display_view()` behaviour for the parent.

If a plan does not support `get_display_view()`, that plan is not a winning
display plan for that parent.

`to_image()` Access Evidence may still refine result-facing classification, but
it is secondary to Stream plan selection.

### AcquisitionSession score

For `AcquisitionSession`, the winning Backing Plan is the viable plan that
minimizes still-result readiness and materialization cost for that parent's
normal capture path.

This score should include:

- the latency from admitted capture work to retained result readiness for the
  session under the applied plan; and
- the relevant result-materialization cost for that session's still-result path.

For single-image still capture, that means the default completed result path.

For bracketed still capture, the score must be derived from the required result
as a whole rather than only one convenience member. The evaluator must not
quietly optimize only the default metered member if that would hide slower
required bracket members.

For a bracketed result, the default rule should be conservative: the session is
not "fast" until the required result is fast. The slowest required member
should therefore dominate aggregate capture readiness.

---

## 8. Evidence Sources

CamBANG should continue to preserve the distinction between:

- Backing State
- Operation Support
- Access Evidence

### Stream evidence

Stream plan evaluation should use real `get_display_view()` Access Evidence
from the public result seam.

### AcquisitionSession evidence

AcquisitionSession plan evaluation should combine:

- still-result readiness evidence from the real capture/result path; and
- real materialization Access Evidence from the public result seam.

Comparable capture evidence must remain internally consistent:

- readiness and materialization must belong to the same relevant parent epoch,
  capture/result identity, and realized Posture Shape; and
- a candidate is not complete merely because materialization exists while
  readiness is missing.

The still-result readiness part is explicitly broader than result-access
classification and must not be mislabeled as ordinary result-access evidence.

### Guardrail

Access Evidence remains evidence from real public operations. It must not be
reframed as provider-local generation/staging cost, snapshot publication cost,
or unrelated GPU upload/update cost.

---

## 9. Evaluation Lifecycle

Evaluation remains bounded and non-per-frame.

There are at most three viable Posture Shapes, so the evaluator should treat a
parent evaluation epoch as a bounded comparison across all viable parent plans.

The conceptual model is:

1. determine the viable plan shapes for the parent;
2. establish the parent evaluation epoch;
3. apply a Requested Plan;
4. wait for any required settle delay;
5. gather the relevant evidence for that plan;
6. repeat for the other viable plans in the epoch;
7. choose the winning plan and record it as the Steady Plan.

For capture-scoped evaluation, "gather the relevant evidence" means waiting for
both readiness and materialization for the same accepted candidate/result
identity. A materialization-only observation may be retained as partial
evidence, but it must not settle the comparison, advance the candidate as
though it were complete, or win via a synthesized zero-readiness total.

The evaluator must not:

- poll continuously per frame;
- rerun on every `to_image()` or `get_display_view()` request;
- invent a broad benchmark subsystem;
- treat one-at-a-time stepping as the architecture itself rather than as an
  implementation detail.

---

## 10. Invalidation and Reevaluation

Reevaluation should be driven by parent-scoped structural or applied-state
changes, not by repeated access calls.

The evaluation epoch should be invalidated by changes such as:

- parent realization or destruction;
- parent-owned capture profile or bundle changes;
- parent-owned picture/configuration changes that affect backing behaviour;
- parent capability-set changes;
- applied Backing Plan changes;
- any other change that alters the realized backing/access domain for that
  parent.

This keeps reevaluation aligned with the existing access-posture invalidation
and live-applied-boundary work already present in source.

---

## 11. Priming and Capture-Only Parents

Capture-only parents require special treatment.

Some providers realize a concrete `AcquisitionSession` only when stream or
capture work actually needs it. That is architecturally valid, but it creates a
problem for first-use capture latency if evaluation cannot begin until the first
real capture request.

The design direction therefore includes bounded priming.

### Priming rule

Core may request priming only when the provider can realize and retire a
concrete acquisition-session seam safely.

Priming must:

- be leak-safe;
- use truthful provider/native-object lifetime;
- avoid fabricating `AcquisitionSession` truth when no real seam exists;
- seed the first real evaluation epoch only when the same structural/capability
  assumptions still hold.

### No fabricated session truth

Still-capture callbacks alone must not be treated as synthetic
`AcquisitionSession` realization when no real provider/native session seam has
been realized.

### Device-scoped provisional cache

Before the first real `AcquisitionSession` exists, Core may hold a provisional
device-scoped priming cache for future evaluation seeding, but that provisional
cache is not itself the final evaluation owner and must not replace the real
session-scoped decision once a concrete parent exists.

### Current implemented provisional behavior

Current source implements only the bounded provisional cache/seeding part of
this direction.

That means:

- capture evaluation may begin under a provisional device-scoped priming owner
  before a real `AcquisitionSession` has been resolved;
- when later capture truth identifies a real `AcquisitionSession`, the active
  evaluation state migrates onto that real parent;
- once a capture evaluation epoch settles, Core may reuse the winning plan as
  the first `Requested Plan` for a later capture epoch only when the later
  device-level effective signature still matches the prior one.

That same-signature seed reuse is intentionally narrow:

- it does not skip bounded evaluation across viable plans;
- it does not fabricate `AcquisitionSession` lifecycle truth;
- it does not treat the provisional cache as the final evaluation owner; and
- it does not yet solve the first-ever still-only capture latency problem by
  itself.

### Current explicit priming implementation

Current source now closes that first-use still-only gap through an explicit
provider/Core priming seam.

That means:

- when no live `AcquisitionSession` parent exists yet for a still-only device,
  Core may ask the provider to synchronize a truthful primed capture parent for
  the current effective capture request;
- providers that support this seam may realize and retain a concrete native
  acquisition-session resource ahead of the user's first real capture trigger;
- Core continues to treat this as lifetime truth, not fabricated capture truth;
  and
- same-signature settled seed reuse remains a separate mechanism from the
  provider-backed primed parent seam.

The explicit priming seam is still bounded:

- it is driven by parent-topology/capture-shape invalidation boundaries rather
  than per-frame or per-access polling;
- it does not itself fabricate capture-ready/materialization measurements; and
- it does not replace bounded real-capture reevaluation across viable plans.

---

## 12. Settle Delay

Measurement is not instantaneous and should not be modeled as such.

Providers should be able to define a small bounded settle delay before probe
evidence is accepted for a newly realized parent or a newly applied Backing
Plan.

This settle delay is conceptually separate from `warm_hold_ms`.

- `warm_hold_ms` is teardown-retention policy;
- settle delay is probe-validity policy.

The scheduler support already present in Core should be reused for this bounded
timed work rather than introducing ad hoc polling loops.

Public result access may still return and instrument immediately, but decision-
driving evaluation evidence must continue to respect the provider-defined
settle boundary.

---

## 13. Provider Responsibilities

Providers continue to own:

- truthful capability advertisement;
- actual backing/resource realization;
- real native-object lifetime;
- still-capture admission/submission behaviour.

Providers do not own Backing Plan policy.

Providers execute the Requested Plan supplied by Core and report truthful
results under that plan. They do not reinterpret the policy independently.

For capable providers, grouped rig capture remains one admitted submission for
the shared capture id, but parent-scoped evaluation remains per participating
`AcquisitionSession`.

---

## 14. Verification Consequences

The verification target for the redesigned evaluator changes.

The key proof is no longer "did the old chooser settle according to the old
intent model?" It becomes:

- did the correct Native Payload Support Parent own the decision;
- were all viable parent plans evaluated in a bounded way for the epoch;
- did Requested Plan remain distinct from Steady Plan while evaluation was
  active;
- did reevaluation happen only on the correct invalidation boundaries;
- did priming remain truthful and leak-safe;
- did the winning plan match the parent's defined primary-function score.

Maintainer-facing decision reports should therefore expose, per candidate:

- whether the candidate was merely viable or actually evaluated;
- the accepted decision evidence for that candidate;
- enough realized result/access identity to prove attribution;
- whether the final decision was measured or direct single-viable;
- and, when evaluation ends early, the explicit termination reason
  (for example `live_display_demand_family_crossing`).

Scene 68 may or may not remain the best host for that proof. That should be
decided after the redesign is agreed, not assumed up front.

---

## 15. Immediate Implementation Consequences

The expected implementation direction after this note is accepted is:

- replace the old chooser `intent` policy model with parent-scoped evaluation;
- move capture-side plan ownership from the coarse device/capture lane toward
  real `AcquisitionSession` ownership;
- preserve the existing requested-vs-steady separation;
- preserve `CoreResultStore` validation against Core-held Requested Plan state;
- preserve the real result-access evidence seam;
- extend evidence ingestion where needed so AcquisitionSession scoring can
  include still-result readiness as well as result-materialization cost.

This is expected to require a meaningful refactor rather than another local
patch on the older chooser implementation.

Current source-progress note:

- the older chooser intent model has been displaced further by parent-scoped
  evaluation;
- stream-side parent evaluation and demand-aware probing guardrails are now
  implemented;
- provider-defined settle delays are now implemented;
- capture-side provisional parent evaluation and same-signature seed reuse are
  now implemented;
- explicit first-use still-only priming is now implemented through a
  provider/Core seam when the provider supports it.
