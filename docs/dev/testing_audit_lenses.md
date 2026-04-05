# CamBANG Test Suite Audit Lenses

This document defines the **three required lenses** for reviewing,
maintaining, and evolving the CamBANG test suite.

These lenses are mandatory for any future audit, refactor, or failure
investigation.\
They exist to prevent shallow fixes, false positives, and silent
weakening of tests.

------------------------------------------------------------------------

## 1) Contract Correctness Lens

### Definition

Every test must be evaluated as a **consumer → provider contract**.

A test is only valid if: - the consumer calls a **valid, supported
surface** - the provider **actually supplies** everything required on
the reachable execution path

------------------------------------------------------------------------

### Required Method

For any test or harness:

1.  Identify the **consumer**
2.  Identify the **concrete provider**
3.  Determine the **reachable execution path**
4.  Enumerate required contract (methods, signals, constants, data
    shape)
5.  Verify provider supplies all of them

------------------------------------------------------------------------

### Example

``` gdscript
_server.PROVIDER_KIND_PLATFORM_BACKED
```

-   Consumer: Status panel\
-   Provider: MockServer\
-   Issue: MockServer did NOT define required constants

**Conclusion:** Contract mismatch (not runtime bug)

------------------------------------------------------------------------

### Rules

-   ❌ "method exists" is NOT sufficient\
-   ❌ static grep is NOT sufficient\
-   ✅ must validate **path-sensitive contract**

------------------------------------------------------------------------

## 2) Architectural Alignment Lens

### Definition

Tests must reflect the **current architecture**, not historical
patterns.

### Current Direction

-   ✅ Server-owned model (`CamBANGServer`)
-   ❌ Dev-node orchestration (legacy)

------------------------------------------------------------------------

### What to Check

-   Uses `CamBANGServer`
-   Avoids `CamBANGDevNode` unless justified
-   Avoids `dev_node_path`

------------------------------------------------------------------------

### Classification

  Status     Meaning
  ---------- ----------------------
  Aligned    Current architecture
  Stale      Works but legacy
  Obsolete   No longer relevant

------------------------------------------------------------------------

### Rules

-   ❌ Do not fix bugs inside legacy architecture\
-   ❌ Do not preserve dev-node patterns\
-   ✅ Prefer rewrite or retirement

------------------------------------------------------------------------

## 3) Semantic Intent Lens

### Definition

A test must be validated against **what it is trying to prove**, not
just whether it passes.

------------------------------------------------------------------------

### Required Questions

1.  What behavior is being tested?\
2.  Is it still valid and meaningful?\
3.  If failing: test wrong or system wrong?

------------------------------------------------------------------------

### Critical Rule

> A passing test is worthless if it no longer tests the intended
> behavior.

------------------------------------------------------------------------

### Example

Old:

    "panel-local"

New:

    "copied from a previously rendered authoritative panel"

**Conclusion:** wording evolved → test must follow semantics

------------------------------------------------------------------------

### Rules

-   ❌ Do not weaken assertions\
-   ❌ Do not delete failing checks\
-   ✅ preserve semantic strength

------------------------------------------------------------------------

## Putting It Together

  Lens           Question
  -------------- -------------------------
  Contract       Does interaction match?
  Architecture   Is test aligned?
  Semantics      Is behavior meaningful?

------------------------------------------------------------------------

## Audit Flow

1.  Contract\
2.  Semantics\
3.  Architecture

------------------------------------------------------------------------

## Anti-Patterns

-   Blindly updating tests\
-   Ignoring execution paths\
-   Weakening assertions\
-   Keeping obsolete tests\
-   Fixing symptoms only

------------------------------------------------------------------------

## Success Criteria

-   Contract-valid\
-   Architecturally aligned\
-   Semantically meaningful\
-   Trusted

------------------------------------------------------------------------

## Final Note

If a failure feels confusing:

> One of the three lenses has not been applied correctly.
