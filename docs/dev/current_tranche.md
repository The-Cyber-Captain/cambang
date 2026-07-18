# Current tranche

## Provider broker call isolation and teardown drain

Status: implementation and required validation complete (2026-07-18).

This tranche addresses the next dependency group from the July 2026 C++
contract-boundary audit. It is limited to `ProviderBroker` call lifetime,
lock/re-entry safety, and deterministic shutdown admission/drain behaviour.

### Scope

* stop holding `active_provider_mutex_` while invoking provider virtual methods;
* preserve the active provider's lifetime while calls execute outside the
  broker lifecycle mutex;
* close provider-call admission before shutdown and wait for already-admitted
  prompt/bounded calls to drain before provider destruction;
* keep concurrent post-close calls fast and failure-visible;
* make initialization publication transactional so partially initialized
  providers are never callable;
* forward provider-specific capture-admission watchdog timing through the
  Core-bound broker instead of silently substituting the interface default;
* preserve synthetic timeline dispatch deferral and callback invocation outside
  broker locks;
* add focused provider-compliance regressions and update the affected contracts.

### Invariants

* The locked Godot public API is unchanged.
* Core remains bound to one broker and one latched active provider per session.
* Provider methods remain prompt, bounded submission/control operations.
* Provider callbacks and host dispatch hooks are not invoked while a broker
  lifecycle mutex is held.
* Shutdown cannot destroy a provider while an admitted broker call still uses it.
* Tests and verification tools are not weakened.
* `godot-cpp` is consumed with `godot_cpp=external`; it is not rebuilt.
* Windows builds use MinGW with `use_mingw=yes` and
  `mingw_prefix=/c/Compilers/mingw64`.

### Acceptance criteria

* A provider method can perform a benign broker query without self-deadlocking.
* A concurrent broker query is not blocked merely because another thread is
  inside a provider virtual method.
* Shutdown closes new admission, waits for an admitted call, then invokes
  provider shutdown exactly once outside the lifecycle mutex.
* Calls racing after shutdown admission closes fail without reaching the provider.
* Initialization failure leaves no active/callable provider.
* Provider-specific capture watchdog timing remains truthful through the broker.
* Existing broker timeline, provider compliance, restart, and command-truth
  verification remains green.
* Relevant Godot boundary/result scenes pass through
  `tests/cambang_gde/run_godot.ps1`, and native timeline verification passes.
* The final diff contains no Godot public binding changes.

### Required validation

```text
scons gde=no maintainer_tools=yes godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
scons platform=windows target=debug godot_cpp=external use_mingw=yes mingw_prefix=/c/Compilers/mingw64
out/provider_compliance_verify.exe
out/core_spine_smoke.exe
out/restart_boundary_verify.exe
out/synthetic_timeline_verify.exe
out/synthetic_only_provider_support_verify.exe
```

Godot verification must be launched through `tests/cambang_gde/run_godot.ps1`.
SCons and authoritative Godot runs require approved unsandboxed execution on
this machine.

### Validation record

Both required SCons commands passed with the configured MinGW toolchain and
`godot_cpp=external`. The required native executables passed:

* `provider_compliance_verify`: 39/39 checks;
* `core_spine_smoke`: 36/36 checks;
* `restart_boundary_verify`;
* `synthetic_timeline_verify`;
* `synthetic_only_provider_support_verify`.

Authoritative headless Godot runs through `run_godot.ps1` passed for:

* `65_public_boundary_verify`;
* `70_result_retrieval_verification` in its self-terminating mode;
* `569_capture_result_camera_facts_verify`.

`git diff --check` passed, `git diff -- src/godot` was empty, and the
`thirdparty/godot-cpp` gitlink and working HEAD remained at
`dcfab8de26e62c17b0f06c796125a63b1e437569`.

An additional legacy `60_restart_boundary_abuse` run printed
`OK: godot restart boundary abuse PASS`, but `run_godot.ps1` classified the run
as `missing_harness_verdict` because that scene does not emit the newer
`[CamBANG][HarnessVerdict]` marker. It was not counted as a passing hard gate;
the required native restart verifier and authoritative marked Godot scenes above
remain green. Adding verdict-marker coverage to legacy scenes is follow-up test
harness debt, separate from this broker tranche.
