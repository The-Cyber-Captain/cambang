# CamBANG Godot dev scenes

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

## Running

Run from this directory (`tests/cambang_gde`) using Godot headless:

```bash
godot4 --headless --path . --scene res://scenes/60_restart_boundary_abuse.tscn --quit-after 10
godot4 --headless --path . --scene res://scenes/61_tick_bounded_coalescing_abuse.tscn --quit-after 1000
godot4 --headless --path . --scene res://scenes/62_snapshot_polling_immutability_abuse.tscn --quit-after 1000
godot4 --headless --path . --scene res://scenes/63_snapshot_observer_minimal.tscn --quit-after 10
godot4 --headless --path . --scene res://scenes/65_public_boundary_verify.tscn --quit-after 10
```

Notes:
- Scenes force provider mode to `synthetic` before `start()`.
- `61` and `62` instantiate a dev node and trigger existing scenario names via
  `CamBANGDevNode.start_scenario(name)`.
- `61` and `62` are bounded-observation verifiers: they watch a short internal observation window
  and then emit an explicit PASS/FAIL verdict based on the Godot-visible publishes they observed.
- For bounded-observation verifiers (`61`, `62`), either omit `--quit-after` or set a generously
  large value such as `--quit-after 1000`.
- `60`, `61`, `62`, `63`, and `65` are intended to self-terminate with an explicit terminal `OK: ... PASS`
  or `FAIL: ...` line; `--quit-after` is an outer iteration/frame guard for CLI runs.

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
- `CamBANGServer.stop()`
- `CamBANGServer.set_provider_mode()`
- `CamBANGServer.get_provider_mode()`
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
