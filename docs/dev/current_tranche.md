# Current tranche

## Command truth and exception-safe startup

Status: implementation and required validation complete (2026-07-18).

This tranche addresses the first dependency group from the July 2026 C++
contract-boundary audit. It is intentionally limited to Core command admission,
truthful completion, and thread-start failure handling.

### Scope

* cancel synchronous Core commands that time out before execution;
* prevent a timed-out queued command from mutating provider or Core state later;
* return stream creation and mutable configuration success only after the
  corresponding Core/provider operation has accepted the work;
* convert command-envelope allocation failures to established status values;
* make `CoreThread` and `CBProviderStrand` startup transactional when
  `std::thread` construction fails;
* add focused native regression coverage and update the affected internal
  contract documentation.

### Invariants

* The locked Godot public API is unchanged: no method, signal, property,
  constant, argument, return type, or binding changes.
* Provider API calls remain prompt, bounded submission/control operations.
* Snapshot and lifecycle state remain Core-owned and truthful.
* Tests and verification tools must not be weakened.
* `godot-cpp` is consumed with `godot_cpp=external`; it is not rebuilt.
* Windows builds use MinGW with `use_mingw=yes` and
  `mingw_prefix=/c/Compilers/mingw64`.

### Acceptance criteria

* A queued synchronous command cancelled by its caller cannot execute later.
* Public stream creation cannot return a handle for a provider-rejected stream.
* Configuration setters report provider rejection instead of queue admission.
* Allocation or worker-thread creation failure preserves a stable stopped/not-
  running state and does not escape a Godot or thread boundary.
* Existing deterministic native verification remains green.
* Relevant Godot boundary/restart/lifecycle/result scenes pass through
  `tests/cambang_gde/run_godot.ps1`.
* The final diff contains no Godot public binding changes.

### Required validation

```text
scons gde=no maintainer_tools=yes godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=windows target=debug godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
out/core_spine_smoke.exe
out/provider_compliance_verify.exe
out/restart_boundary_verify.exe
out/core_result_path_smoke.exe
out/core_result_byte_budget_stress_smoke.exe
out/synthetic_timeline_verify.exe
```

Godot verification must be launched through `tests/cambang_gde/run_godot.ps1`.
Windowed runs and SCons builds require approved unsandboxed execution on this
machine.

### Validation record

Both SCons commands above passed with the configured MinGW toolchain and
`godot_cpp=external`. All six listed native executables passed; the focused
Core smoke reported 36/36 checks.

Authoritative headless Godot runs through `run_godot.ps1` passed for:

* `65_public_boundary_verify`;
* `70_result_retrieval_verification`;
* `569_capture_result_camera_facts_verify`.

`git diff --check` passed and `git diff -- src/godot` was empty. The
`thirdparty/godot-cpp` gitlink remained at
`dcfab8de26e62c17b0f06c796125a63b1e437569`.
