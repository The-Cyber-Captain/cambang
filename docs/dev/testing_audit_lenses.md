# CamBANG Test Suite Audit Lenses

**Status:** Current dev process/audit reference.

This document is a process-discipline aid for test, harness, fixture, and suite
failure triage.

It is subordinate to canonical architecture docs, current source behavior,
fixture taxonomy docs, schema-mapping docs, and verifier/tool behavior. It must
not override canonical architecture or verifier source behavior.

Cross-reference anchors:

* `docs/dev/status_panel_fixture_taxonomy.md` for fixture taxonomy and
  renderer/NPS fixture classification.
* `docs/dev/state_snapshot_schema_mapping.md` for schema-vs-semantic validation
  layering.
* `docs/dev/maintainer_tools.md` for verifier/tool roles.
* `docs/naming.md` for scenario / verification case / fixture terminology.

This document defines the required lenses for reviewing, maintaining, and
evolving the CamBANG test suite.

These lenses are mandatory for any future audit, refactor, or failure
investigation. They exist to prevent shallow fixes, false positives, and silent
weakening of tests.

---

## 1) Contract Correctness Lens

### Definition

Every test must be evaluated as a **consumer → provider contract**.

A test is valid only if:

* the consumer calls a valid, supported surface;
* the provider, fixture, mock, or harness supplies everything required on the
  reachable execution path; and
* the asserted result belongs to the documented contract for that surface.

### Required method

For any test or harness:

1. Identify the consumer.
2. Identify the concrete provider, fixture, mock, or harness endpoint.
3. Determine the reachable execution path.
4. Enumerate the required contract:

  * methods;
  * signals;
  * constants;
  * data shape;
  * lifecycle state;
  * expected error behavior.
5. Verify that the provider, fixture, mock, or harness endpoint supplies all of
   them on that path.

### Example

```gdscript
_server.PROVIDER_KIND_PLATFORM_BACKED
```

* Consumer: status panel
* Provider: mock server
* Issue: mock server did not define a required constant

**Conclusion:** contract mismatch, not a runtime bug.

### Rules

* "Method exists" is not sufficient.
* Static grep is not sufficient.
* Validate the path-sensitive contract.
* Keep mock, fixture, and harness contracts intentionally narrow but complete.
* Do not broaden a fake provider or mock until the test’s consumer contract has
  been identified.

---

## 2) Architecture Alignment Lens

### Definition

Tests must reflect the current architecture, not retired scaffolding,
transitional glue, or accidental historical patterns.

### Current direction

CamBANG tests should exercise the architecture through the same authority
boundaries used by the runtime:

* `CamBANGServer` as the Godot-facing singleton boundary;
* object-level public APIs for normal runtime flows;
* explicit maintainer APIs only for maintainer verification;
* provider-owned scenario execution for Synthetic timeline behavior;
* provider-support truth for build-aware platform availability;
* snapshot and result objects as observable truth surfaces.

### What to check

For each test, verify that it:

* uses the current Godot-facing boundary intentionally;
* distinguishes public runtime flow from maintainer-only tooling;
* keeps Synthetic timeline semantics provider-owned;
* avoids reconstructing runtime meaning in scene glue;
* respects provider-support truth before asserting platform-backed behavior;
* avoids source-shaping around removed or abandoned scaffolding;
* keeps platform-backed expectations build-aware.

### Classification

| Status       | Meaning                                                             |
| ------------ | ------------------------------------------------------------------- |
| Aligned      | Reflects current architecture and validates a meaningful invariant. |
| Stale        | Exercises a valid behavior through an old or indirect route.        |
| Obsolete     | No longer validates a current behavior or contract.                 |
| Transitional | Temporarily valid while an explicitly tracked migration is active.  |

### Rules

* Do not fix bugs inside obsolete architecture.
* Do not preserve old orchestration patterns merely because tests still pass.
* Prefer rewrite or retirement when a test depends on a removed concept.
* Preserve semantic strength when moving a test to a current boundary.
* Keep build-aware assertions explicit when platform support is optional or not
  compiled.

---

## 3) Semantic Intent Lens

### Definition

A test must be validated against **what it is trying to prove**, not merely
against whether it passes.

A passing test is not useful if it no longer proves the intended behavior.

### Required questions

1. What behavior is being tested?
2. Why does that behavior matter?
3. Is the behavior still current?
4. Is the assertion checking the behavior directly, or only a side effect?
5. If the test is failing, is the test wrong or is the system wrong?
6. If the test is passing, could it still be too weak?

### Example

Old assertion intent:

```text
panel-local
```

Current semantic intent:

```text
copied from a previously rendered authoritative panel
```

**Conclusion:** wording evolved, so the test must follow the current semantic
meaning rather than preserve old phrasing.

### Rules

* Do not weaken assertions merely to obtain a pass.
* Do not delete failing checks before identifying the behavior they protected.
* Preserve or increase semantic strength when updating tests.
* Prefer explicit assertions over incidental log matching.
* Keep user-facing behavior, snapshot truth, and maintainer diagnostics separate.

---

## 4) Failure Classification Lens

Before changing source, tests, fixtures, or harnesses, classify the failure as
one primary class.

### Primary failure classes

* fixture schema drift
* fixture expectation drift
* stale fixture intent
* projection / health bug
* harness bug
* suite-runner / parser bug
* renderer-mode mismatch
* build-support expectation mismatch
* provider-support expectation mismatch
* actual production/runtime bug
* documentation drift
* generated-output or stale-build-artifact noise

### Rules

* Do not patch from one observed failure alone.
* Do not weaken tests merely to match current output.
* Do not mutate fixtures until green without first classifying intent and failure
  stage.
* Prefer read-only audit, then narrow patch, then smallest relevant validation,
  then broader validation.
* When renderer-sensitive expectations are involved, check `renderer_profile`
  and `nps_scope` rather than making one fixture accept all shapes.
* When platform support is optional or absent, assert provider-support truth
  before asserting startup behavior.
* When an audit grep reports hits, separate maintained source from generated
  output, logs, archives, and stale build artifacts before drawing conclusions.

---

## 5) Suite Runner Lens

A suite failure can be caused by the runner rather than by the scene or runtime.

### What to check

For any scene or script suite failure, confirm:

* whether the scene emitted its explicit success marker;
* whether the scene emitted an explicit failure marker;
* whether the suite runner terminated the scene early;
* whether timeout units match the tool’s actual semantics;
* whether review-log detection is matching expected diagnostic noise;
* whether the suite parser is reading the intended log stream;
* whether generated logs from prior runs are being mistaken for current output.

### Rules

* Treat explicit scene failure markers as stronger evidence than missing success
  markers.
* Treat premature runner termination as a harness issue until proven otherwise.
* Do not infer a runtime regression from a missing OK marker without checking
  cutoff, timeout, and parser behavior.
* Expected diagnostic noise must be documented or filtered by the harness; it
  must not leave unexplained permanent review states.

---

## 6) Build and Provider Support Lens

Tests that touch startup, provider selection, or platform-backed behavior must be
build-aware.

### Required checks

Before asserting provider behavior, inspect the provider-support surface exposed
by the current build.

Tests must distinguish:

* provider family label;
* provider compiled support;
* access/readiness state;
* explicit unsupported behavior;
* startup failure after compiled support is present;
* unsupported mode because no provider implementation is compiled.

### Rules

* A platform family label does not imply an implemented provider.
* Unsupported/not-compiled platform-backed mode is a valid build truth.
* Synthetic support must not silently stand in for platform-backed support.
* A test may require platform-backed support only when it explicitly declares
  that requirement.
* Public boundary tests should assert visible failure and clean stopped/NIL state
  when an unsupported provider mode is requested.

---

## Putting the lenses together

| Lens                   | Question                                            |
| ---------------------- | --------------------------------------------------- |
| Contract               | Does the interaction match the reachable contract?  |
| Architecture           | Is the test aligned with the current system shape?  |
| Semantics              | Does the test still prove the intended behavior?    |
| Failure classification | What class of problem is this before patching?      |
| Suite runner           | Did the test fail, or did the harness mishandle it? |
| Build/provider support | Is the assertion valid for this compiled build?     |

---

## Audit flow

Use this order for failure triage:

1. Confirm the log belongs to the current run.
2. Classify the failure surface:

  * scene;
  * script;
  * harness;
  * suite runner;
  * build;
  * documentation.
3. Apply the Contract Correctness Lens.
4. Apply the Semantic Intent Lens.
5. Apply the Architecture Alignment Lens.
6. Apply the Build and Provider Support Lens where startup/provider behavior is
   involved.
7. Apply the Suite Runner Lens where markers, review logs, or timeouts are
   involved.
8. Patch narrowly.
9. Run the smallest meaningful validation.
10. Run the broader suite only after the narrow validation explains the failure.

---

## Anti-patterns

* Blindly updating tests.
* Ignoring execution paths.
* Weakening assertions.
* Keeping obsolete tests.
* Fixing symptoms only.
* Treating generated output as maintained source.
* Treating a build family label as compiled provider support.
* Treating missing success markers as runtime failures without checking runner
  cutoff.
* Allowing unexplained permanent review states.
* Preserving retired scaffolding patterns in new tests.

---

## Success criteria

A trusted test is:

* contract-valid;
* architecturally aligned;
* semantically meaningful;
* build-aware where relevant;
* renderer-aware where relevant;
* clear about harness and suite-runner responsibilities;
* resistant to false positives from logs, archives, generated output, or stale
  build artifacts.

---

## Final note

If a failure feels confusing, at least one lens has not been applied carefully
enough.
