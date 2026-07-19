# C++ static-analysis baseline — 2026-07-19

Durable reference (noise inventory + triage), per `docs/dev/static_analysis.md`.
This is not a completion record: it exists so later audits do not rediscover
the same background noise. Findings here received a triage category only; no
source change was driven by this baseline.

## Tool versions

- clang-tidy: LLVM 17.0.6 (host PATH), consuming the MinGW g++ 13.2.0 compile
  database.
- Python 3.14.0 for `tools/audit/*.py`.

## Compile database command

```sh
scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64
# emits compile_commands.json (COMPDB_PATH default); 141 src/ TUs analyzable
```

## Checks enabled

Repo-root `.clang-tidy` (the `static_analysis.md` initial families):
`bugprone-*`, `clang-analyzer-*`, `cppcoreguidelines-*`, `performance-*`,
selected `modernize-*`/`readability-*`. `WarningsAsErrors: ''`.

Run: `python tools/audit/run_clang_tidy_changed.py --all -j 8`
(138/141 TUs analyzed; unique src findings: 2,109).

## TU-level tool limitations (triage: Not applicable — tool limitation)

- `src/core/core_runtime.cpp`: pathological analyzer runtime — killed after
  283 min CPU-bound on this one TU (path-sensitive `clang-analyzer-*` state
  explosion at ~6.5k lines). Do not include this TU in full-family runs;
  analyze it with `--checks=-clang-analyzer-*` or per-family.
- `src/core/adc_camera_description.cpp`,
  `src/imaging/synthetic/scenario_loader_parse.cpp`: clang 17 rejects the
  recursive `JsonValue` container pattern ("pointer to incomplete type" in
  `std::vector<std::pair<...>>`) that GCC 13 accepts. Toolchain strictness
  difference, not a build defect; a future restructure (indirection via
  `std::unique_ptr`) would make them clang-analyzable.

## Known noisy checks (triage: Not applicable / Defer; disable candidates)

Exactly the set `static_analysis.md` predicted, confirmed by count:

| Check | Count | Note |
| --- | --- | --- |
| cppcoreguidelines-avoid-magic-numbers | 1,260 | 60% of all findings; dominated by smoke-tool test data and pixel math. |
| cppcoreguidelines-pro-bounds-pointer-arithmetic | 181 | pixel/stride code; inherent to the domain. |
| bugprone-easily-swappable-parameters | 106 | id-heavy APIs (capture_id/device_id/...); real risk is mitigated by naming. |
| cppcoreguidelines-pro-type-vararg | 94 | fprintf/vformat diagnostics; accepted style. |
| cppcoreguidelines-pro-bounds-array-to-pointer-decay | 91 | C-string literals to logging. |
| cppcoreguidelines-avoid-non-const-global-variables | 57 | anonymous-namespace bridge state; ownership documented at sites. |

Recommend adding these six to the `.clang-tidy` disable list in the next
config revision (the `static_analysis.md` "after first baseline" step).

## Known real debt (triage: Fix separately — verify each before fixing)

High-signal, low-count leads worth human follow-up, none fixed here:

- `src/core/core_result_store.cpp:490` — bugprone-use-after-move ('payload').
  Priority verification: in the result-retention path.
- `src/core/resource_aggregate_telemetry.cpp:121` — clang-analyzer
  NullDereference (path-sensitive; verify feasibility of the path).
- `src/imaging/api/provider_strand.cpp:8` — bugprone-exception-escape in
  `~CBProviderStrand` (stop()/join can throw; destructor is implicitly
  noexcept).
- `src/core/provider_camera_fact_state.cpp:43,56` — exception-escape in
  noexcept-expected validators.
- 3× clang-analyzer-deadcode.DeadStores (core_dispatcher.cpp:369,
  synthetic/provider.cpp:2447-2448) — stale timing-instrumentation stores.
- bugprone-unchecked-optional-access (33) — needs sampling; mixed
  signal/noise expected.

Waived with reason at site: `provider_broker.cpp:156` bugprone-empty-catch —
the destructor's documented noexcept boundary around provider shutdown.

## Checks suitable for changed-code gating (recommendation only; maintainer decides)

Low-noise, high-signal on this codebase's evidence:
`bugprone-use-after-move`, `bugprone-unhandled-self-assignment`,
`bugprone-empty-catch`, `bugprone-exception-escape`,
`clang-analyzer-deadcode.DeadStores`, `clang-analyzer-core.NullDereference`
(changed-files scope only, never full-tree, because of the core_runtime.cpp
runtime pathology).

## Checks not currently useful

The six noisy checks above, plus `clang-analyzer-optin.performance.Padding`
(layout churn for no measured benefit).

## Smell-scanner baseline (tools/audit/cambang_static_smells.py)

208 leads: godot-binding-surface 137 (intended inventory — diff against this
count to spot unauthorized binding changes), const-cast 21, reinterpret-cast
16, getenv 13 (all documented in maintainer_tools.md), raw-new 12 /
raw-delete 6 (includes the deliberate FrameReleaseLease contract),
free-rid 2 (both inside approved drain paths), lock-then-callback 1
(heuristic; verify once). Zero: thread-detach, todo-marker, std-endl,
ignored-post-result, binding-latest.

## Modules needing human audit despite tool silence

Unchanged from `static_analysis.md`'s "what tools cannot prove" list; in
particular `core_runtime.cpp` is now *doubly* human-audit-dependent since the
analyzer cannot complete on it.
