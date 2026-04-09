# Synthetic Timeline Reconciliation Tranche — Findings, Fixes, Verification, and Cleanup

Status: Dev note  
Scope: SyntheticProvider clustered destructive sequencing, provider compliance verification, Godot-facing startup/config surface, strict unmet-condition diagnostics, and post-tranche cleanup.

---

## 1. Purpose

This note records what was actually wrong in the synthetic timeline reconciliation tranche, what was fixed, how verification responsibilities now divide, and what diagnostic scaffolding was deliberately trimmed or retained.

It is intended as a grounded maintainer record of the tranche outcome, not as a replacement for canonical architecture documents.

Where canonical semantics are already defined elsewhere, this note references them rather than redefining them.

---

## 2. Executive summary

This tranche resolved a real provider/runtime defect in clustered destructive synthetic timeline sequencing, corrected two Godot-boundary regressions introduced during the API/config work, stabilized a canonical always-pass verification case to reflect the now-standard completion-gated model, and trimmed temporary teardown-trace chatter while preserving meaningful diagnostics.

The key durable outcome is:

- **completion-gated reconciliation** is now the standard/default synthetic timeline destructive sequencing mode
- **strict reconciliation** remains available as a power-user / diagnostic mode
- provider-side destructive promotion now waits for **actual readiness truth**, not merely authored request order or stop completion alone

---

## 3. What was actually wrong

## 3.1 The real clustered destructive sequencing defect was in `SyntheticProvider`

The originally observed clustered destructive failures were not verifier-only.

The real runtime defect was provider-side: clustered destructive timeline requests could promote `DestroyStream` too early, after stop completion but before the stream was truly ready for destruction.

The critical missing readiness condition was:

- **stream buffer release truth**

The adopted provider-side fix was therefore:

- gated clustered destructive requests wait for:
    1. **stream-stopped truth**
    2. **stream-buffer release truth**

before promoting `DestroyStream`.

This removed the intermittent gated failure where destroy still reached an effective busy/not-ready condition immediately after stop completion.

### Practical meaning

This tranche clarified an important semantic distinction:

- **stop truth**
- **destroy readiness**

are not the same thing.

A stream may be stopped but still not yet truthfully destroyable if child/buffer state has not yet resolved.

Completion-gating is therefore not merely a convenience for “clustered same-time requests.” It is a general readiness-aware mechanism for avoiding parent removal before the child is truly ready.

---

## 3.2 `provider_compliance_verify` initially exposed real runtime behavior, not just verifier fragility

The compliance verifier first surfaced the clustered destructive problem, but the eventual diagnosis showed that the failing behavior was not just a verifier expectation mistake.

Important lesson:

> Do not assume a failure is verifier-only merely because it first appears in a verifier.

In this tranche, the verifier was exposing a real provider-side sequencing/readiness problem.

---

## 3.3 Godot public-boundary regressions were introduced during the startup/config surface work

Two real Godot-facing regressions were found and fixed.

### A. Startup wiring regression

`CamBANGServer` was unconditionally pushing synthetic timeline reconciliation into the broker even for inapplicable modes, including default platform-backed startup.

This caused default platform-backed start to fail because the broker correctly rejects synthetic timeline reconciliation unless all of the following are true:

- provider mode is synthetic
- synthetic role is timeline
- timing driver is virtual_time

The correct fix was at the server/broker call-site discipline layer:

- only request broker timeline reconciliation when it is actually applicable

The broker remained strict, and public API validation remained deterministic.

### B. Active provider config readback regression

`get_active_provider_config()` did not fully align with the expected Godot-facing readback contract.

Specifically, `timeline_reconciliation` was being omitted on non-applicable paths rather than being explicitly present as `null`.

The fix was:

- always include `timeline_reconciliation`
- set it to explicit `null` on non-applicable paths
- populate it with `"completion_gated"` / `"strict"` only for synthetic timeline + virtual_time

This restored deterministic dictionary shape and explicit non-applicability signaling for Godot consumers and harnesses.

---

## 4. What was learned

## 4.1 Completion-gating is about readiness-aware destruction, not just clustering

The standard completion-gated model should be understood as:

- do not promote destruction of a parent/resource merely because authored timeline intent says so
- promote only when the relevant child/readiness truths have actually arrived

This scales beyond “same timestamp” or “clustered close spacing” cases.

It is the right conceptual model for parent-removal safety under truthful lifecycle semantics.

## 4.2 CLI verifier time and Godot coalesced engine time are different observational layers

This tranche reinforced that:

- CLI verification tools operating in synthetic virtual time observe finer-grained internal progression
- Godot-facing publication is tick-bounded and coalesced

Therefore:

- what looks “obviously immediate” at a high level may not be a guaranteed single-step truth at CLI verifier granularity
- conversely, Godot may legitimately coalesce several internal transitions into a simpler observable sequence

## 4.3 Static code inspection is not enough to claim runtime validity

A script/scene may appear updated for a new surface while still failing at runtime due to underlying startup/config behavior.

Lesson:

> “Looks updated” is not the same as “is behaviorally correct.”

This was directly demonstrated by scene 65.

## 4.4 Explicit `null` can be part of the public contract

When Godot-side consumers/harnesses expect stable dictionary shape and explicit non-applicability, omission and `null` are not interchangeable.

In this tranche, explicit `null` was the correct Godot-facing signal for synthetic-only fields that do not apply to the current mode.

---

## 5. Intended runtime/provider behavior after this tranche

## 5.1 Timeline reconciliation modes

### `completion_gated`
This is now the **standard/default** mode for synthetic timeline destructive sequencing.

It is the default when all of the following are true and reconciliation is omitted:

- provider kind is synthetic
- synthetic role is timeline
- timing driver is virtual_time

Behavioral intent:

- destructive progression is gated until the relevant readiness truth exists
- in practice this includes waiting for both stop truth and stream-buffer release truth before stream destruction is promoted

### `strict`
This remains available as a **diagnostic / power-user mode**.

Behavioral intent:

- no completion-gated reconciliation smoothing is applied
- authored destructive intent is exercised more directly
- scenario conditions may fail to be satisfied fully in-band
- this mode is useful for diagnosis and for seeing where authored sequencing is tighter than truthful resource readiness permits

---

## 5.2 Public Godot-facing API surface

The agreed public surface remains:

`CamBANGServer.start(provider_kind, synthetic_role, timing_driver, timeline_reconciliation)`

with the following applicability rule:

`timeline_reconciliation` is valid only when:

- provider kind is synthetic
- synthetic role is timeline
- timing driver is virtual_time

Rules:

- omitted there => defaults to completion-gated
- supplied elsewhere => deterministic invalid-parameter rejection

No public live mutability for timeline reconciliation was reintroduced.

---

## 5.3 `get_active_provider_config()` semantics

When running, `get_active_provider_config()` now follows this shape discipline:

### Platform-backed
- `provider_kind = PLATFORM_BACKED`
- `synthetic_role = null`
- `timing_driver = null`
- `timeline_reconciliation = null`

### Synthetic nominal
- `provider_kind = SYNTHETIC`
- synthetic role/timing fields populated
- `timeline_reconciliation = null`

### Synthetic timeline + real_time
- synthetic role/timing fields populated
- `timeline_reconciliation = null`

### Synthetic timeline + virtual_time
- synthetic role/timing fields populated
- `timeline_reconciliation = "completion_gated"` or `"strict"`

This explicit-null behavior is intentional and part of the Godot-facing readback contract used by verification scenes and harnesses.

---

## 5.4 Strict unmet-condition Godot warning remains intentionally retained

The concise Godot warning for strict synthetic timeline mode not satisfying scenario conditions in-band remains intentional and should stay.

A subtle but important implementation detail was confirmed:

- strict timeline teardown trace monitoring must be re-armed **after** `runtime_.start()`

Re-arming only before start is insufficient because `runtime_.is_running()` is still false at that point, so strict monitoring does not become active.

---

## 6. Verification responsibilities after the tranche

## 6.1 `provider_compliance_verify`

`provider_compliance_verify` is now the primary deterministic proof of provider-contract behavior for this area.

It now meaningfully validates strict vs gated clustered destructive sequencing semantics.

### In strict clustered cases, it accepts both runtime-valid outcomes:
1. retained stopped state with in-band destroy/close failure
2. full in-band destroy/close success

### In gated clustered cases, it validates:
- correct callback watermark/progress handling
- eventual successful realization under gated destructive promotion

This verifier is therefore the right place for:
- contract-valid strict/gated sequencing behavior
- clustered destructive acceptance envelopes
- provider-side lifecycle/order truth

It is **not** merely a smoke check.

---

## 6.2 `canonical_timeline_realization`

`canonical_timeline_realization` was reinterpreted and corrected.

It now serves as a:

- **canonical**
- **default-path**
- **completion-gated**
- **always-pass**
- **simple/readable**

timeline realization proof.

It is no longer treated as a strict edge probe or an implicit second compliance verifier.

### Important retained design choice

The authored timeline remains clear and tightly ordered:

- `StopStream @ T`
- `DestroyStream @ T+1`
- `CloseDevice @ T+2`

Those timings were intentionally **not widened**.

Instead, the verifier logic was corrected so that while waiting for destroy/close realization, it continues advancing synthetic virtual time in bounded steps rather than assuming one tiny post-stop step must always suffice.

This preserves the authored causal intent without baking arbitrary padded spacing into the test.

### What this case now proves

It proves:

> given a straightforward authored timeline, the standard/default synthetic timeline path realizes cleanly end-to-end

That is distinct from the broader contract/acceptance-envelope role of `provider_compliance_verify`.

---

## 6.3 Scene 65 (`65_public_boundary_verify`)

Scene 65 proved valuable because it exposed real Godot-boundary regressions rather than merely stale script expectations.

It now confirms:

- compact `start(...)` surface behavior
- invalid-argument rejection for inapplicable role/timing/reconciliation combinations
- default/platform/synthetic success paths
- active provider config readback shape and explicit null behavior
- restart and baseline publication boundary behavior

This scene should continue to be treated as a meaningful Godot-facing boundary verifier, not as a superficial API-shape smoke scene.

---

## 7. Status panel implications

This tranche reaffirmed the intended separation between:

- provider configuration summary/readback
- snapshot row/schema truth

### Implications

- reconciliation mode belongs in the **provider summary / active-config** sense of the UI
- it does **not** belong in snapshot rows, row schema, fixture schema, or schema-backed row logic
- status panel harnesses had to be updated because their mock server doubles had drifted behind the current server-facing config surface

This was a harness compatibility issue, not a reason to expand snapshot/schema surfaces.

---

## 8. Diagnostic cleanup completed after stabilization

Once behavior was stabilized, temporary teardown-trace chatter was deliberately trimmed in three narrow passes.

## 8.1 `src/core/synthetic_timeline_request_binding.cpp`
Removed:
- `instrumentation active`
- routine `dispatched ...`
- routine `submit ... rc=...`

Kept:
- failure-oriented submit failure lines

## 8.2 `src/core/core_runtime.cpp`
Removed:
- routine provider teardown RC chatter:
    - `provider StopStream ... rc=...`
    - `provider StopStream(for-destroy) ... rc=...`
    - `provider DestroyStream ... rc=...`
    - `provider CloseDevice ... rc=...`

Kept:
- failure-oriented provider RC lines for teardown operations

## 8.3 `src/core/provider_callback_ingress.cpp`
Removed:
- normal callback/completion breadcrumbs for:
    - `CloseDevice`
    - `DestroyStream`
    - `StopStream` OK-path

Kept:
- failure-oriented `StopStream` callback diagnostics on non-OK outcome

## 8.4 Intentionally retained diagnostics

The cleanup stopped before trimming provider-side pending-reason diagnostics in `SyntheticProvider`.

Those lines remain the most semantically useful surviving explanation of completion-gated behavior and are worth retaining unless a future deliberate diagnostics redesign supersedes them.

The strict unmet-condition Godot warning path in `cambang_server.cpp` also remains intentionally retained.

---

## 9. Resulting maintenance guidance

## 9.1 Do not collapse stop truth into destroy readiness
A stream being stopped does not imply it is already truthfully destroyable.

## 9.2 Do not assume a verifier failure is “just the verifier”
Investigate runtime/provider behavior first.

## 9.3 Keep completion-gated as the standard path
Strict remains valuable, but it should stay a diagnostic mode rather than the assumed canonical always-pass model.

## 9.4 Treat explicit null as part of the Godot-facing config contract where expected
Do not silently substitute omission when consumers/harnesses rely on deterministic dictionary shape.

## 9.5 Avoid broad churn when debugging cross-layer issues
This tranche benefited from eventually fixing defects at the correct layer:
- provider runtime behavior in `SyntheticProvider`
- call-site discipline in `CamBANGServer`
- verifier expectations in `canonical_timeline_realization`
- harness double compatibility in status panel harnesses

Each issue did not belong to the same layer.

---

## 10. Outcome

After the tranche and post-cleanup validation:

- `provider_compliance_verify` passes
- `canonical_timeline_realization` is stable as a completion-gated canonical always-pass case
- `65_public_boundary_verify` passes
- status panel harnesses were updated to current mock-surface compatibility
- teardown diagnostics are substantially less noisy while preserving meaningful failure and pending-reason signals
- strict unmet-condition Godot warning remains functional

This is the intended stable post-tranche state.

---

## 11. Suggested follow-up documentation touchpoints

If desired, the following docs may receive small follow-up updates to reflect the stabilized outcome more explicitly:

- `docs/dev/maintainer_tools.md`
    - clarify the distinct roles of `provider_compliance_verify` and `canonical_timeline_realization`

- any synthetic timeline / verification dev note
    - record completion-gated as the standard/default mode and strict as diagnostic mode

- any Godot-facing API note
    - ensure `get_active_provider_config()` explicit-null behavior is described where helpful

These should be incremental clarifications, not broad rewrites.

---