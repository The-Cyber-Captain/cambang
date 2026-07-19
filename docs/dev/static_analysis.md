# CamBANG Static Analysis

This document describes how CamBANG should use mechanical/static-analysis tooling to support C++ code-quality audits.

Static analysis is advisory evidence. It does not replace architectural review, source-grounded diagnosis, or CamBANG-specific lifecycle reasoning.

## Purpose

Use static analysis to help find:

- obvious bugs;
- undefined-behaviour risks;
- ownership and lifetime smells;
- unchecked results;
- suspicious casts;
- include hygiene issues;
- accidental style drift in touched code;
- temporary diagnostics that should not remain in production code.

Do not use static analysis to justify broad churn, public API changes, or test weakening.

## Initial posture

Use a ratcheted approach:

| Stage | Behaviour |
| --- | --- |
| 1. Baseline | Run tools manually to understand current noise. Do not mass-fix. Do not block changes. |
| 2. Changed-files advisory | Run selected checks on files touched by a task. Triage results. |
| 3. New-code gate | Avoid introducing new untriaged major/blocker findings. Existing debt may remain recorded. |
| 4. Module cleanup | Clean selected modules only when risk and scope justify the change. |

The first repo step should be documentation only. Tooling should be added later in a separate, narrow change.

## clang-tidy

clang-tidy is the preferred first mechanical checker because it can run Clang Static Analyzer checks and selected C++ guideline/style/performance checks against a compile database.

Recommended initial check families:

```yaml
Checks: >
  -*,
  bugprone-*,
  clang-analyzer-*,
  cppcoreguidelines-*,
  performance-*,
  modernize-use-nullptr,
  modernize-use-override,
  modernize-make-unique,
  modernize-use-emplace,
  readability-redundant-member-init,
  readability-delete-null-pointer,
  readability-qualified-auto

WarningsAsErrors: ''
```

Likely noisy checks to consider disabling or scoping after the first baseline:

```yaml
Checks: >
  -*,
  bugprone-*,
  clang-analyzer-*,
  cppcoreguidelines-*,
  performance-*,
  modernize-use-nullptr,
  modernize-use-override,
  modernize-make-unique,
  modernize-use-emplace,
  -cppcoreguidelines-avoid-magic-numbers,
  -cppcoreguidelines-pro-bounds-pointer-arithmetic,
  -cppcoreguidelines-pro-type-vararg,
  -cppcoreguidelines-pro-type-reinterpret-cast,
  -cppcoreguidelines-pro-bounds-array-to-pointer-decay
```

These checks are not inherently wrong. They are often too noisy in systems, Godot, ABI, and platform-boundary code unless scoped carefully.

## Example clang-tidy use

Refresh the compile database using the project-supported SCons path, then run clang-tidy against a specific file.

Example shape:

```bash
scons compdb=1
clang-tidy src/core/provider_callback_ingress.cpp -p .
```

If the SCons option name differs, use the project’s actual compile-database generation command.

Changed-file workflow shape:

```bash
git diff --name-only origin/main...HEAD -- '*.cpp' '*.h' '*.hpp'
```

Then run clang-tidy only over those files.

A later helper script may automate this as:

```text
tools/audit/run_clang_tidy_changed.py
```

Do not add such a script as part of the initial documentation-only change.

## Include-What-You-Use

Include-What-You-Use may be useful for header hygiene, especially when auditing self-contained headers and transitive include reliance.

Use it sparingly at first:

- advisory only;
- not blocking;
- not applied mechanically across the repo;
- reviewed against Godot, platform, and generated/binding constraints;
- preferably scoped to touched files or selected modules.

Potentially useful audit questions:

- Does this header compile independently?
- Does this file rely on transitive includes?
- Is a heavy platform/Godot include present in a core header without need?
- Would a forward declaration preserve the boundary more clearly?

## CamBANG static smell checks

Generic tools will not catch many project-specific risks. A later advisory script may scan for CamBANG smells.

Possible future helper:

```text
tools/audit/cambang_static_smells.py
```

Initial smell patterns:

```text
std::thread(...).detach()
new / delete outside approved wrappers
getenv / environment diagnostic knobs
TODO / FIXME / TEMP / DEBUG / checkpoint / phase trace
std::endl in hot paths
reinterpret_cast
const_cast
mutex lock followed by callback-like calls
ignored PostResult / enqueue result patterns
public API names containing "latest"
Godot API binding changes
direct RID release outside approved drain path
```

This script should not pretend to prove correctness. It should produce audit leads:

```text
file:line
smell
why it matters
suggested audit lens
```

Example output shape:

```text
src/core/foo.cpp:118
Smell: getenv
Reason: persistent environment diagnostic knobs are discouraged unless accepted as maintainer tooling.
Audit lens: diagnostics/temp-code
```

## Baseline reports

After advisory tooling exists, generate a baseline report rather than immediately cleaning everything.

Suggested location:

```text
docs/dev/audit_baselines/cpp_static_analysis_baseline_YYYY_MM_DD.md
```

Suggested contents:

```text
Tool versions:
Compile database command:
Checks enabled:
Known noisy checks:
Known real debt:
Checks suitable for changed-code gating:
Checks not currently useful:
Modules needing human audit despite tool silence:
```

This prevents every later audit from rediscovering the same background noise.

## Triage categories

Every tool finding should be classified as one of:

| Category | Meaning |
| --- | --- |
| Fix now | Real issue, narrow correction, appropriate to current task. |
| Fix separately | Real issue, but outside current safe scope. |
| Waive | Intentional exception; document why. |
| Defer | Possible issue; needs broader design context or module cleanup. |
| Not applicable | Tool false positive or unsuitable for this boundary. |

Do not leave major/blocker findings untriaged.

## Suppression and waiver posture

Prefer code clarity over tool suppression.

If suppression is necessary:

- keep it local;
- explain why;
- reference the architectural or API constraint;
- avoid broad file-level suppression unless unavoidable.

Acceptable waiver examples:

```cpp
// Non-owning. Owned by CoreRuntime and cleared before provider teardown.
ProviderBroker* broker_ = nullptr;
```

```cpp
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
// Required at the platform ABI boundary; bytes are validated before use.
```

A waiver without a reason is not a waiver; it is hidden debt.

## What tools cannot prove

Static analysis cannot fully validate:

- Godot public API contract preservation;
- snapshot-truth semantics;
- provider/core architectural separation;
- renderer/RD thread-affinity correctness;
- restart-boundary determinism;
- whether a test was weakened;
- whether a dormant service has real architectural value;
- whether SyntheticProvider behaviour has leaked into platform-backed design.

These require human audit using `docs/dev/cpp_audit_checklist.md`.

## Recommended future tooling sequence

Do not combine these with the initial documentation change.

1. Add an advisory `.clang-tidy`.
2. Add a changed-file clang-tidy helper.
3. Add a CamBANG smell scanner.
4. Generate a baseline report.
5. Decide which checks, if any, should become changed-code gates.

Each step should be a separate narrow change.
