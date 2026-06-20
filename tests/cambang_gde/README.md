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
    synchronous handler/snapshot consistency, NIL-after-stop, and no stale generation leakage.
  - Expected pass string: `OK: godot public boundary verify PASS`
- `scenes/66_public_lifecycle_verify.tscn`
  - Self-terminating suite verifier for Godot public lifecycle semantics.
  - Expected pass string: `OK: godot public lifecycle verify PASS`
- `scenes/67_status_panel_scenario_runtime.tscn`
  - Manual/status-panel runtime observation scene: starts synthetic timeline mode,
    selects/starts builtin scenario `stream_lifecycle_versions`, and observes publishes via
    `CamBANGStatusPanel` without any `CamBANGDevNode` / `dev_node_path` orchestration.
- `scenes/70_result_retrieval_verification.tscn`
  - Verifies Godot-facing object-level result retrieval/materialization for `CamBANGStreamResult` and `CamBANGCaptureResult`, including grouped Dictionary fact/provenance accessors and visible image presentation.
  - Authors a three-member still capture profile via `still_image_bundle` (ordered still-event image members), not `image_sequence`.
  - Triggers capture through the public `CamBANGDevice.trigger_capture() -> Error` flow, polls `CamBANGDevice.get_result()`, and verifies `CamBANGCaptureResult` exposes three indexed image members with member metadata/materialization coverage.
  - Scene 70 is a user-flow/result-wrapper verifier; exact Device/AcquisitionSession snapshot-shape proof belongs to native/snapshot verification harnesses.
  - Uses builtin scenario `stream_inspection_live` so headed verification can stay open with a visibly live stream for manual inspection/capture.
  - Expected pass string: `OK: result_retrieval_verification passed`
- `scenes/71_capture_session_matrix_v3.tscn`
  - Capture/session matrix confidence harness covering staged stream-result binding and capture-result checkpoints.
  - Supported exercises: `display_oneshot`
  - Default when `CAMBANG_EXERCISE` is unset: `display_oneshot`
  - This default represents the current customer/API confirmation behavior: one-shot `get_display_view()` binding.
- `scenes/72_stream_load_isolation.tscn`
  - Stream-load isolation/regression harness with retained display-path diagnostics and timing summaries.
  - Supported exercises:
    - `display_oneshot` (default)
    - `display_latest`
    - `no_display_default`
    - `no_display_eager`
  - `display_oneshot` is default because it represents the intended Godot-facing API contract:
    bind once and remain live without repeated `get_display_view()` refresh burden.
  - `no_display_eager` is self-contained via `CAMBANG_EXERCISE`: it implies effective GPU update policy `always` when no explicit low-level policy is set.
  - Precedence: explicit `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY` > exercise-derived (`no_display_eager` => `always`) > default (`display_demanded`).
  - Conflict: `CAMBANG_EXERCISE=no_display_eager` with explicit `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY` not equal to `always` fails clearly.
- `scenes/73_rig_capture_result_set_verification.tscn`
  - Focused Godot proof for `CamBANGRig` object API: `CamBANGServer.get_rig(rig_id)`, `CamBANGRig.get_id()`, `CamBANGRig.trigger_capture() -> Error`, and `CamBANGRig.get_result()` consistency.
  - Uses dedicated external scenario fixture `scenarios/rig_capture_result_basic.json`.
  - Fixture topology: six devices A-F; Rig A = A+E; Rig B = B; Rig C = C+F; Device D standalone.
  - Capture remains API-driven via `CamBANGRig.trigger_capture()` (not scenario timeline-triggered), and verification asserts the object-level `CaptureResultSet` contains exactly Rig A members.

## Dev-node/mailbox scene retirement (May 2026)

The following legacy dev-node/mailbox scenes were retired:

- `scenes/00_extension_load.tscn`
- `scenes/10_lifecycle_smoke.tscn`
- `scenes/20_frameview_smoke.tscn`
- `scenes/25_frameview_smoke_with_singleton_snapshots.tscn`
- `scenes/40_frameview_mf_stress_test.tscn`
- `scenes/50_frameview_cycling_patterns_chose_provider.tscn`
- `scenes/51_heavy_probe_registry.tscn`

Migration guidance:

- `25_frameview_smoke_with_singleton_snapshots` → use `70_result_retrieval_verification` for modern result retrieval/materialization and `60-63` for snapshot/boundary confidence.
- `40_frameview_mf_stress_test` → use `72_stream_load_isolation` for load/perf isolation and `60_restart_boundary_abuse` for restart semantics.
- `50_frameview_cycling_patterns_chose_provider` → use `67_status_panel_scenario_runtime` for server-driven scenario/runtime observation.
- `51_heavy_probe_registry` → use `63_snapshot_observer_minimal` for lightweight snapshot diagnostics plus `65_public_boundary_verify` for boundary contract checks.
- `00_extension_load` / `10_lifecycle_smoke` / `20_frameview_smoke` → use `60_restart_boundary_abuse`, `61_tick_bounded_coalescing_abuse`, `62_snapshot_polling_immutability_abuse`, `63_snapshot_observer_minimal`, `65_public_boundary_verify`, and `66_public_lifecycle_verify` for boundary/status/snapshot/lifecycle confidence; use `70_result_retrieval_verification` for result retrieval, `71_capture_session_matrix_v3` for capture/session matrix, `72_stream_load_isolation` for stream-load isolation, and `73_rig_capture_result_set_verification` for rig-capture result-set proof.


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
```

PowerShell helper for local/Codex runs:

```powershell
.\run_godot.ps1 -Scene res://scenes/63_snapshot_observer_minimal.tscn -QuitAfter 10
.\run_godot.ps1 -Scene res://scenes/65_public_boundary_verify.tscn -QuitAfter 10
.\run_godot.ps1 -Script res://scripts/status_panel_harness.gd -ScriptArgs fixtures/status_panel/fixture_valid_basic_authoritative.json
```

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
  `63_snapshot_observer_minimal`, `65_public_boundary_verify`, `66_public_lifecycle_verify`, and
  `70_result_retrieval_verification` are intended to self-terminate with an explicit terminal `OK: ... PASS`
  or `FAIL: ...` line; `--quit-after` is an outer iteration/frame guard for CLI runs.
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
