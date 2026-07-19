# Current tranche

## Tranche 8 - Advisory static-analysis sequence

Status: active; maintainer-activated 2026-07-19 ("Activate the
static-analysis sequence"). Authority for content and posture:
`docs/dev/static_analysis.md` (the ratcheted advisory model, check
families, smell patterns, baseline-report shape, and triage categories are
defined there and are not restated here).

### Purpose

Stand up the advisory static-analysis tooling that `static_analysis.md`
prescribes, in its required order, without letting any tool finding drive
source churn inside this tranche. Output is tooling plus a triaged baseline;
fixes, if any are warranted, are future maintainer-activated work.

### Ordered items (each lands as its own narrow change)

1. **Advisory `.clang-tidy`** at repo root using `static_analysis.md`'s
   recommended initial check families (`WarningsAsErrors: ''`). Noisy-check
   scoping happens only after the item-4 baseline, per that document.
2. **Changed-file helper** `tools/audit/run_clang_tidy_changed.py`:
   resolves changed `src/` C++ files against a base ref (default
   `origin/main...HEAD`), runs clang-tidy with the repo compile database,
   and supports `--all -j N` for full-tree runs. Advisory: exit status
   reflects tool execution health, never finding counts.
3. **Smell scanner** `tools/audit/cambang_static_smells.py`: scans `src/`
   (never `thirdparty/`, never generated output) for the smell patterns
   listed in `static_analysis.md`, emitting its prescribed
   file:line/smell/reason/audit-lens shape. Produces audit leads, not
   verdicts.
4. **Dated baseline report** at
   `docs/dev/audit_baselines/cpp_static_analysis_baseline_2026_07_19.md`
   from a real full-tree run of items 1-3 on this machine, using the report
   shape and triage categories from `static_analysis.md`. Every
   major/blocker-looking finding gets a triage category; none gets a fix
   here.
5. **Gate decision material only**: the report ends with a
   checks-suitable-for-changed-code-gating recommendation. The decision
   itself is reserved to the maintainer and is out of scope.

Note on the baseline file vs. the no-record-files rule: the dated baseline
is durable *reference* (a noise inventory later audits consult so they do
not rediscover it), prescribed by `static_analysis.md` -- it is not a
tranche-completion record. If the maintainer disagrees with that
distinction, say so and item 4's content moves into the commit message
instead.

### Constraints

* No production source changes in this tranche -- no fixes, no NOLINT
  insertions, no include reshuffling driven by findings. Findings land in
  the baseline with a triage category only.
* No CI wiring, no blocking gates, no `warnings_as_errors` interaction.
* Tools must tolerate this machine's reality: MinGW g++ compile commands
  consumed by LLVM 17 clang-tidy (flag-compatibility issues are triaged as
  tool limitations in the baseline, not "fixed" by altering build flags).
* Python 3 stdlib only for the helpers; no new dependencies.
* Public API, snapshot schema, and build behavior untouched.

### Expected implementation files

* `.clang-tidy`;
* `tools/audit/run_clang_tidy_changed.py`;
* `tools/audit/cambang_static_smells.py`;
* `docs/dev/audit_baselines/cpp_static_analysis_baseline_2026_07_19.md`;
* `docs/dev/static_analysis.md` only if a factual command/path in it needs
  reconciling with what was actually built;
* this file (reset to stub on acceptance).

### Acceptance criteria

* `clang-tidy <file> -p .` works against the SCons-generated compile
  database for a representative core file, and the helper reproduces that
  for changed files and for `--all`.
* The smell scanner reports the documented patterns over `src/` in the
  documented output shape.
* The baseline exists, names tool versions and commands, classifies noisy
  vs. real-debt vs. gate-candidate checks, and leaves no major/blocker
  finding untriaged.
* Zero diffs under `src/` from this tranche.

### Required validation

```text
scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64   # compdb refresh + build stays clean
clang-tidy src/core/provider_callback_ingress.cpp -p .   # doc's own example file
python tools/audit/run_clang_tidy_changed.py --all -j 8   # full-tree baseline input
python tools/audit/cambang_static_smells.py               # full-tree smell leads
out/core_spine_smoke.exe                                  # proves no build/source impact
```

No Godot scene coverage is required: this tranche adds no runtime code.
