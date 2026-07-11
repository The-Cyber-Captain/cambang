# CamBANG Godot dev scenes

## Shared maintainer exercise selector

Maintainer harnesses in this folder use a shared selector:

- `CAMBANG_EXERCISE`

An **exercise** is a named maintainer validation configuration/mode for a harness.
It is **not** product API configuration, **not** capture profile configuration, and
**not** scenario data.

Harnesses should:

- document supported exercise names,
- document default exercise when `CAMBANG_EXERCISE` is unset,
- fail clearly on unsupported exercise values (do not silently fall back).

## Godot validation convention

A **Godot validation surface** is one named Godot-side verification entry point
that a maintainer can run and report as a distinct validation result. In
practice this is usually a scene such as
`568_backing_plan_evaluation_verify`, but the same naming/reporting convention
also applies to other explicit Godot-side harness entry points under this
project.

For scenes driven by `run_godot.ps1`, terminal classification is defined by the
shared harness verdict protocol, not by scene-specific success/error regexes.
The authoritative line is:

```text
[CamBANG][HarnessVerdict] scene=<scene_name> status=<ok|expected_unsupported|fail|error> exit_code=<integer> reason=<token>
```

Rules:

- `status=ok` means the scene completed successfully.
- `status=expected_unsupported` means the scene deliberately bailed out because
  the requested configuration is structurally unsupported.
- `status=fail` means the scene reached a meaningful verifier/benchmark failure.
- `status=error` means the scene detected a harness/runtime error distinct from
  an ordinary verifier assertion failure.
- the verdict line must be emitted before any large structured payload, so
  Android/logcat tail loss cannot hide the classification.
- `run_godot.ps1` classifies from this protocol only. It does not accept
  per-scene expected OK or expected-unsupported regex parameters.

Keep reporting simple:

- supported cases should be reported as the named surface plus the exercised
  case/matrix label and the observed `OK` verdict from `run_godot.ps1`
- expected-negative cases should be reported as the named surface plus the
  exercised case and the observed `EXPECTED_UNSUPPORTED` verdict from
  `run_godot.ps1`
- if a Godot surface was not executed, say so plainly as
  `not run; requires local/manual Godot validation`
- native maintainer-tool validation and Godot scene validation are different
  surfaces: a passing native verifier or smoke tool does not by itself prove the
  corresponding Godot scene or headed/runtime path

Scenes that have not yet been migrated to the shared harness verdict protocol
may still be run directly with Godot for manual/local observation, but they are
not suitable for `run_godot.ps1` log classification until migrated.

Do not claim Godot validation unless the relevant local helper/manual path was
actually run.

## Tranche 4 boundary-hardening scenes

These scenes are dev-only abuse/diagnostic checks for the Godot runtime boundary.

- `scenes/60_restart_boundary_abuse.tscn`
  - Verifies stop/restart NIL gating and first post-restart baseline counters.
  - Expected pass string: `OK: godot restart boundary abuse PASS`
- `scenes/61_tick_bounded_coalescing_abuse.tscn`
  - Verifies max one `state_published` per Godot tick, contiguous `version`, and
    `topology_version` updates only on snapshot-observed topology changes.
  - Expected pass string: `OK: godot tick-bounded coalescing abuse PASS`
- `scenes/62_snapshot_polling_immutability_abuse.tscn`
  - Verifies per-frame polling is safe, old snapshot references remain readable,
    and cached old snapshots do not mutate after newer publishes.
  - Expected pass string: `OK: godot snapshot polling/immutability abuse PASS`
- `scenes/63_snapshot_observer_minimal.tscn`
  - Minimal snapshot-only observer diagnostics (`gen/version/topology_version`,
    device/stream counts, frame counters, stream error count).
  - Expected pass string: `OK: godot snapshot observer minimal PASS`
- `scenes/65_public_boundary_verify.tscn`
  - Verifies Godot public-boundary semantics: NIL-before-baseline, baseline-first publish,
    synchronous handler/snapshot consistency, stopped-time camera-concurrency ingest/error mapping,
    NIL-after-stop, and no stale generation leakage.
  - Authoritative terminal verdict: `[CamBANG][HarnessVerdict] scene=65_public_boundary_verify status=<ok|fail|error> exit_code=<n> reason=<token>`
  - Expected pass string: `OK: godot public boundary verify PASS`
- `scenes/66_public_lifecycle_verify.tscn`
  - Self-terminating suite verifier for Godot public lifecycle semantics.
  - Expected pass string: `OK: godot public lifecycle verify PASS`
- `scenes/67_status_panel_scenario_runtime.tscn`
  - Manual/status-panel runtime observation scene: starts synthetic timeline mode,
    selects/starts builtin scenario `stream_lifecycle_versions`, and observes publishes via
    `CamBANGStatusPanel` without extra helper-node orchestration.
- `scenes/70_result_retrieval_verification.tscn`
  - Verifies Godot-facing object-level result retrieval/materialization for `CamBANGStreamResult` and `CamBANGCaptureResult`, including grouped Dictionary fact/provenance accessors and visible image presentation.
  - Authors a three-member still capture profile via `still_image_bundle` (ordered still-event image members), not `image_sequence`.
  - Triggers capture through the public `CamBANGDevice.trigger_capture() -> Error` flow, polls `CamBANGDevice.get_result()`, and verifies `CamBANGCaptureResult` exposes three indexed image members with member metadata/materialization coverage.
  - Scene 70 is a user-flow/result-wrapper verifier; exact Device/AcquisitionSession snapshot-shape proof belongs to native/snapshot verification harnesses.
  - Uses builtin scenario `stream_inspection_live` so headed verification can stay open with a visibly live stream for manual inspection/capture.
  - Expected pass string: `OK: result_retrieval_verification passed`
- `scenes/71_capture_session_matrix_v3.tscn`
  - Capture/session matrix confidence harness covering staged stream-result binding and capture-result checkpoints.
  - Supported exercises: `bind_once_live_display`
  - Backward-compatible alias: `display_oneshot`
  - Default when `CAMBANG_EXERCISE` is unset: `bind_once_live_display`
  - This default represents the current customer/API confirmation behavior: bind once with `get_display_view()` and keep a persistent live display view while the returned object is held.
- `scenes/72_stream_load_isolation.tscn`
  - Stream-load isolation/regression harness with retained display-path diagnostics and timing summaries.
  - Supported exercises:
    - `bind_once_live_display` (default)
    - `reacquire_display_each_frame_debug`
    - `no_display_default`
    - `no_display_eager`
  - Backward-compatible aliases:
    - `display_oneshot` => `bind_once_live_display`
    - `display_latest` => `reacquire_display_each_frame_debug`
  - `bind_once_live_display` is default because it represents the intended Godot-facing API contract:
    bind once and remain live without repeated `get_display_view()` refresh burden.
  - `no_display_eager` is self-contained via `CAMBANG_EXERCISE`: it implies effective GPU update policy `always` when no explicit low-level policy is set.
  - Precedence: explicit `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY` > exercise-derived (`no_display_eager` => `always`) > default (`display_demanded`).
  - Conflict: `CAMBANG_EXERCISE=no_display_eager` with explicit `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY` not equal to `always` fails clearly.
- `scenes/73_rig_capture_result_set_verification.tscn`
  - Focused Godot proof for `CamBANGRig` object API: `CamBANGServer.get_rig(rig_id)`, `CamBANGRig.get_id()`, `CamBANGRig.trigger_capture() -> Error`, and `CamBANGRig.get_result()` consistency.
  - Uses dedicated external scenario fixture `scenarios/rig_capture_result_basic.json`.
  - Fixture topology: six devices A-F; Rig A = A+E; Rig B = B; Rig C = C+F; Device D standalone.
  - Capture remains API-driven via `CamBANGRig.trigger_capture()` (not scenario timeline-triggered), and verification asserts the object-level `CaptureResultSet` contains exactly Rig A members.
- `scenes/568_backing_plan_evaluation_verify.tscn`
  - Canonical backing-plan evaluation verifier; supersedes legacy Scene 68 as the focused automatable behavioral check for this topic.
  - Scene 68 remains only a secondary retained-result evidence/reset reporting surface; it is not the canonical parent-scoped evaluation proof.
  - Proves stream-parent and capture-parent evaluation scoping, access-only evidence seeding without relying on capture-result getter-side calibration fallback; the scene uses explicit `to_image_member(0)` access to exercise the capture-member materialization seam, stop/reset clearing, and a paused `advance_timeline(...)` edge path with exact-same-time device+stream realization plus teardown/recreation cleanup.
  - Audits Core's parent-scoped `candidate_evidence` and `completion_reason` directly rather than recomputing winners from global route aggregates.
  - Distinguishes viable candidates from measured candidates, direct single-viable decisions from measured decisions, and intentionally partial stream comparison from full all-candidate completion.
  - Fails when a capture winner lacks complete readiness-plus-materialization evidence, when one result/access-posture observation is attributed to incompatible candidates, or when the selected winner does not match the reported score.
  - Its paused/clocked path spends explicit virtual-time budget for stream-evaluation completion; it does not rely on `advance_timeline(...)` to hide evaluator quiescence behind a single host step.
  - At startup it reports the stored and effective Maintainer Synthetic producer output-form selection, so matrix failures can be correlated against the actual runtime selection in force.
  - On each observed evaluation or re-evaluation event it emits `INFO:` lines describing the parent, chosen or active backing-plan state, current synthetic timeline position, completion reason, and candidate-specific decision evidence.
  - Matrix expectation: supported combinations terminate with a shared harness verdict `status=ok`; structurally unsupported combinations such as Compatibility renderer + `gpu_only` terminate with `status=expected_unsupported` and reason `compatibility_gpu_only`, not by timeout.
  - Uses dedicated external scenario fixtures:
    - `scenarios/568_backing_plan_single_access_live.json`
    - `scenarios/568_backing_plan_dual_live.json`
    - `scenarios/568_backing_plan_edge_clocked.json`
  - Authoritative terminal verdict: `[CamBANG][HarnessVerdict] scene=backing_plan_evaluation_verify status=<ok|expected_unsupported|fail|error> exit_code=<n> reason=<token>`
- `scenes/870_to_image_soak_benchmark.tscn`
  - Automated two-device + rig soak benchmark for `StreamResult.to_image()` and `CaptureResult.to_image()` under randomized human-like and tick-scale load.
  - Runs the full phase matrix twice: metered-only still capture (`[0]`) and five-member exposure bracket (`[-2,-1,0,+1,+2]`).
  - Displays live stream `get_display_view()` panels separately from latest explicit stream `to_image()` panels, standalone capture panels, bracket thumbnail strips, and rig-triggered multi-device capture panels.
  - Emits the shared harness verdict before any large benchmark payload; Compatibility renderer + `gpu_only` exits as `status=expected_unsupported` with reason `compatibility_gpu_only`.
  - Emits framed JSON record `scene870_to_image_soak_summary`; `run_godot.ps1 -CaptureLogs` recovers it and records `scene870_summary_json` in `meta.json`. On Android, Scene 870 also writes the same summary to `user://cambang_records/scene870_to_image_soak_summary.json`, and the runner recovers that file when the log marker is present.
  - Authoritative terminal verdict: `[CamBANG][HarnessVerdict] scene=870_to_image_soak_benchmark status=<ok|expected_unsupported|fail|error> exit_code=<n> reason=<token>`

## Running

Run from this directory (`tests/cambang_gde`) using Godot headless:

```bash
godot4 --headless --path . --scene res://scenes/60_restart_boundary_abuse.tscn --quit-after 10
godot4 --headless --path . --scene res://scenes/61_tick_bounded_coalescing_abuse.tscn --quit-after 1000
godot4 --headless --path . --scene res://scenes/62_snapshot_polling_immutability_abuse.tscn --quit-after 1000
godot4 --headless --path . --scene res://scenes/63_snapshot_observer_minimal.tscn --quit-after 10
godot4 --headless --path . --scene res://scenes/65_public_boundary_verify.tscn --quit-after 10
godot4 --headless --path . --scene res://scenes/66_public_lifecycle_verify.tscn --quit-after 1000
godot4 --headless --path . --scene res://scenes/67_status_panel_scenario_runtime.tscn --quit-after 10
godot4 --headless --path . --scene res://scenes/70_result_retrieval_verification.tscn --quit-after 20
godot4 --headless --path . --scene res://scenes/73_rig_capture_result_set_verification.tscn --quit-after 20
godot4 --headless --path . --scene res://scenes/568_backing_plan_evaluation_verify.tscn --quit-after 1000
```

PowerShell helper for local/Codex runs of harness-verdict scenes:

```powershell
.\run_godot.ps1 `
  -Scene res://scenes/568_backing_plan_evaluation_verify.tscn `
  -QuitAfter 10 `
  -CaptureLogs `
  -RunLabel scene568_runtime_default `
  -ExtraArgs @("--cambang-synth-producer-output-form=runtime_default")
```

```powershell
.\run_godot.ps1 `
  -Scene res://scenes/568_backing_plan_evaluation_verify.tscn `
  -QuitAfter 10 `
  -CaptureLogs `
  -RunLabel scene568_gpu_only_expected_unsupported `
  -ExtraArgs @("--rendering-method=compatibility", "--cambang-synth-producer-output-form=gpu_only")
```

To collect the Scene 870 `to_image()` soak benchmark baseline:

```powershell
.\run_godot.ps1 `
  -Scene res://scenes/870_to_image_soak_benchmark.tscn `
  -Windowed `
  -CaptureLogs `
  -TimeoutSec 180 `
  -RunLabel scene870_synthetic_mobile `
  -ExtraArgs @("--rendering-method=mobile", "--cambang-bench-seed=870001")
```

Android export/deploy uses the same launcher and harness-verdict classification:

```powershell
.\run_godot.ps1 `
  -RunPlatform android `
  -Scene res://scenes/568_backing_plan_evaluation_verify.tscn `
  -CaptureLogs `
  -TimeoutSec 25 `
  -RunLabel scene568_android_runtime_default `
  -ExtraArgs @("--cambang-synth-producer-output-form=runtime_default")
```

When `-CaptureLogs` is enabled, `run_godot.ps1` writes one run directory beneath:

- `run-logs/ok/` for runs whose latest harness verdict is `status=ok` and which have no timeout/process/hard-failure override
- `run-logs/expected_unsupported/` for runs whose latest harness verdict is `status=expected_unsupported` and which have no timeout/process/hard-failure override
- `run-logs/error/` for timeout, non-zero process exit, hard-failure marker, missing harness verdict, or harness verdict `status=fail|error`
- `run-logs/summary.jsonl` as one compact JSON record per run for summary-first inspection

Each run directory contains:

- `stdout.log`
- `stderr.log`
- `meta.json`
- `verdict.txt`

`meta.json` records all observed harness verdict markers in `harness_verdicts` and the classification-driving final marker in `harness_verdict`.

This is intended to keep Codex/token usage bounded: inspect `summary.jsonl` first, then open only the failing run directories unless you are explicitly doing later OK-run timing/statistics work.

Android-mode notes:

- it exports a debug APK, installs it with `adb`, launches it, and stores host logs plus `device_logcat.log` in the same per-run directory
- it currently supports `-Scene` only, not `-Script`
- it currently translates maintainer settings from `--cambang-synth-producer-output-form=...`, `--cambang-synth-stream-capability-downgrades=...`, and `--cambang-synth-capture-capability-downgrades=...`
- it does not support `-QuitAfter`; use `-TimeoutSec` as the outer observation guard
- after the Android app exits naturally, the runner performs a short bounded logcat drain so the harness verdict and any trailing record-file markers can be captured
- it supports `--rendering-method=mobile` and `--rendering-method=gl_compatibility` during export/deploy runs; `--rendering-method=compatibility` is normalized to `gl_compatibility`
- for Scene 870, the runner can recover the summary JSON from the scene-written `user://cambang_records/scene870_to_image_soak_summary.json` file when the log marker is present; this file-backed recovery is for large structured data, not verdict classification

Launcher note:

- `run_godot.ps1` accepts `--rendering-method=mobile`, `--rendering-method=compatibility`, and `--rendering-method=gl_compatibility`; it normalizes `compatibility` to `gl_compatibility`
- on Windows, it passes rendering-method/driver engine args in the split `--flag value` form expected by the local Godot executable, while maintainer override args remain user args after `--`
- `-ExpectedOkPattern` and `-ExpectedUnsupportedPattern` are intentionally not supported; add or fix a scene-level `[CamBANG][HarnessVerdict]` line instead of adding runner-side regex exceptions

Notes for Codex/agent validation on this machine:

- prefer `run_godot.ps1` or `godot_test_suite.ps1` from PowerShell so the Godot executable path and project path are explicit
- Codex sandboxed Godot launches on this machine can crash with a signal-11 / "memory could not be read" dialog even when the same command succeeds unsandboxed
- treat an approved unsandboxed Godot run as the authoritative local validation path for scene/harness verification

Notes:
- Scenes that need synthetic mode start directly with
  `CamBANGServer.start(CamBANGServer.PROVIDER_KIND_SYNTHETIC, CamBANGServer.SYNTHETIC_ROLE_TIMELINE, CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME)`.
- `61` and `62` stage/start scenarios via `CamBANGServer.select_builtin_scenario(...)`
  and `CamBANGServer.start_scenario()`.
- `61` and `62` are bounded-observation verifiers: they watch a short internal observation window
  and then emit an explicit PASS/FAIL verdict based on the Godot-visible publishes they observed.
- For bounded-observation verifiers (`61`, `62`), either omit `--quit-after` or set a generously
  large value such as `--quit-after 1000`.
- `60_restart_boundary_abuse`, `61_tick_bounded_coalescing_abuse`, `62_snapshot_polling_immutability_abuse`,
  `63_snapshot_observer_minimal`, `66_public_lifecycle_verify`, and
  `70_result_retrieval_verification` are older/non-protocol scenes unless separately migrated.
  Their terminal `OK: ... PASS` / `FAIL: ...` lines are useful for direct Godot/manual checks,
  but they are not `run_godot.ps1` classification verdicts.
- Large structured harness payloads should not rely on a single huge log line; `run_godot.ps1`
  understands framed log records and writes recovered payloads into `records/` under the run directory.
- Harnesses that need this may implement the tiny local framed-record emitter in their own script.
- Scene 70 emits `scene70_summary` through that framed-record protocol, and the runner recovers it into
  the run directory.
- Scene 70 exercises public device still-profile authoring using a three-member `still_image_bundle`.
- Scene 70 verifies the object-level `CamBANGDevice.get_result()` `CamBANGCaptureResult` exposes three indexed image members with member metadata/materialization coverage.
- Exact Device/AcquisitionSession snapshot-shape proof remains in native/snapshot verification harnesses, not Scene 70 pre-trigger gating.
- `67_status_panel_scenario_runtime` is a manual runtime integration proof scene for status-panel
  observation of a server-driven builtin scenario; it prints concise publish diagnostics and is
  commonly run with `--quit-after` for bounded CLI execution.
- For Scene 72, prefer `CAMBANG_EXERCISE` for normal validation shape selection.
  Low-level env knobs remain supported as maintainer escape hatches:
  - `CAMBANG_STREAM_LOAD_POLL_RESULTS`
  - `CAMBANG_STREAM_LOAD_BIND_DISPLAY`
  - `CAMBANG_STREAM_LOAD_DISPLAY_PATH_TRACE`
  - `CAMBANG_DEV_SYNTH_CATCHUP_CAP`
  - `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY` (maintainer override/comparison escape hatch)
  - `CAMBANG_DEV_SYNTH_CATCHUP_CAP` guidance:
    - normal SyntheticProvider runtime and Scene 71 runs: leave unset
    - Scene 72 (stream-load/perf isolation) may use `2` as bounded two-stream setting
    - `2` preserves common two-stream due-together correctness
    - `1` is intentionally lossy for dual-stream steady flow (stress diagnostics only)
  - `no_display_eager` no longer requires setting the low-level env separately; examples:
    - default/no-display comparison:
      - `CAMBANG_EXERCISE=no_display_default godot4 --headless --path . --scene res://scenes/72_stream_load_isolation.tscn --quit-after 30`
    - eager/no-display comparison via exercise only:
      - `CAMBANG_EXERCISE=no_display_eager godot4 --headless --path . --scene res://scenes/72_stream_load_isolation.tscn --quit-after 30`
    - bind-once live display comparison:
      - `CAMBANG_EXERCISE=bind_once_live_display godot4 --headless --path . --scene res://scenes/72_stream_load_isolation.tscn --quit-after 30`
    - reacquire-each-frame debug comparison:
      - `CAMBANG_EXERCISE=reacquire_display_each_frame_debug godot4 --headless --path . --scene res://scenes/72_stream_load_isolation.tscn --quit-after 30`
    - explicit low-level override comparison (still supported):
      - `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=always CAMBANG_EXERCISE=no_display_default godot4 --headless --path . --scene res://scenes/72_stream_load_isolation.tscn --quit-after 30`

## Shared status panel and editor dock

This branch now includes:

- `res://addons/cambang/cambang_status_panel.gd`
  - shared status-only observer panel for runtime and editor use
- `res://addons/cambang_editor/plugin.cfg`
  - editor plugin that adds a CamBANG dock and stops the server in `_build()` before Play
- `res://scenes/64_status_panel_runtime_smoke.tscn`
  - minimal runtime scene hosting `CamBANGStatusPanel`

The editor dock uses only the public singleton API:

- `CamBANGServer.start()`
- `CamBANGServer.start(provider_kind[, role[, timing_driver]])`
- `CamBANGServer.stop()`
- `CamBANGServer.is_running()`
- `CamBANGServer.get_active_provider_config()`
- `CamBANGServer.get_state_snapshot()`

## Status panel harness

`res://scripts/status_panel_harness.gd` keeps semantic fixture validation headless-friendly by default:

```bash
godot4 --headless --path . --script res://scripts/status_panel_harness.gd -- fixtures/status_panel/fixture_valid_basic_authoritative.json
```

You can also set an explicit harness window size with:

- `--window-width <px>`
- `--window-height <px>`

If you request a screenshot output path, run the harness with a real window/render path instead of `--headless`. Screenshot runs default to a portrait harness window size of `1400x1600` when neither size flag is provided:

```bash
godot4 --path . --script res://scripts/status_panel_harness.gd -- fixtures/status_panel/fixture_valid_basic_authoritative.json artifacts/status_panel.png
```

If you need a different real-window capture size, pass the flags explicitly:

```bash
godot4 --path . --script res://scripts/status_panel_harness.gd -- fixtures/status_panel/fixture_valid_basic_authoritative.json artifacts/status_panel.png --window-width 1200 --window-height 2200
```

When a screenshot path is provided in headless mode, or when the active renderer/display server cannot expose the real window texture, the harness fails explicitly instead of warning/skipping.
