# CamBANG – Upstream Discrepancies Log

**Status:** Current dev discrepancy log.

This file records observed discrepancies between CamBANG development needs and upstream Godot / godot-cpp / godot-cpp-template documentation, APIs, or build conventions.

It is a non-canonical log of upstream references, local workarounds, and removal criteria.

It must not define CamBANG architecture, provider behavior, public API, or release policy.

This document records deviations from:
- godot-cpp-template
- godot-cpp
- Godot editor/plugin APIs and documentation
- Godot SCons conventions

Each entry must include:
- Upstream reference (repo + issue, proposal, or documentation link)
- Description of discrepancy
- CamBANG workaround
- Removal criteria


## godot-cpp-template #107 (Android cross-build naming mismatch)

Upstream: https://github.com/godotengine/godot-cpp-template/issues/107

Observation:
- Reported mismatch in expected library naming/prefix/arch for Windows→Android cross-compilation.

CamBANG status:
- Not yet implementing Android cross-build; no workaround applied yet.
- If encountered during CamBANG Android support, log the exact reproduction and apply an explicit, removable workaround.

## Godot docs / EditorDock (editor plugin API availability mismatch)

Upstream: https://docs.godotengine.org/en/stable/classes/class_editordock.html

Observation:
- Current Godot documentation describes `EditorDock` as the modern dock API.
- In the currently targeted editor build, a CamBANG plugin script referencing `EditorDock` failed to load, so the API cannot yet be relied upon for current-version-compatible editor tooling.

CamBANG workaround:
- Keep CamBANG editor tooling on the already validated editor plugin approach compatible with the current target editor version.
- Revisit `EditorDock` only once the targeted Godot version is confirmed to expose the API in practice.

Removal criteria:
- CamBANG formally targets a Godot version where `EditorDock` is confirmed available and usable for the plugin path.


## Godot editor debugger #112815 (diagnostic messages may not flush before immediate quit)

Upstream: https://github.com/godotengine/godot/issues/112815

Observation:
- In Godot 4.5.1-stable, diagnostic messages emitted immediately before `get_tree().quit()` may reach the shell/console output while the editor Debugger panel/message indicator does not reliably update before the scene exits.
- CamBANG observed this with `UtilityFunctions::push_warning(...)` in GPU DisplayLifetime teardown diagnostics: the warning is emitted and visible in shell output, but the editor UI may not surface the pending warning before immediate quit.
- This is an editor/debugger diagnostic flush discrepancy, not a CamBANG runtime teardown failure.

CamBANG handling:
- Keep DisplayLifetime diagnostics on Godot's warning channel because the condition is non-fatal and should remain warning-severity rather than error-severity.
- Emit both the headline DisplayLifetime message and per-stream `live_gpu_display_view` detail through `UtilityFunctions::push_warning(...)`.
- Provide the CamBANG-owned `CamBANGServer.stop_and_quit(exit_code := 0)` helper for verification scenes and other CamBANG-controlled flows that intentionally stop CamBANG and immediately exit.
- `stop_and_quit(...)` calls the existing synchronous `CamBANGServer.stop()` first, then defers `SceneTree.quit(exit_code)` by a small fixed number of Godot `process_frame` ticks (`kEditorDiagnosticQuitFlushFrames = 30`) as an explicit editor/debugger diagnostic-flush workaround.
- This helper is not a CamBANG runtime stop requirement and does not change `CamBANGServer.stop()` semantics; normal runtime stop flows should continue to call `stop()` directly.
- Treat immediate-quit editor Debugger visibility as unreliable in affected Godot versions; shell/console output remains the more reliable diagnostic sink for this case.

Removal criteria:
- Godot editor/debugger reliably surfaces diagnostics emitted immediately before quit in CamBANG's supported Godot versions.
- CamBANG no longer needs a documented quit-flush workaround for immediate-exit verification flows.

## godot-cpp selected-platform mutable generated/SCons state

Upstream: https://github.com/godotengine/godot-cpp

Observation:
- CamBANG's root build keeps its own GDE object directories and plugin binaries separated by selected platform, Godot target, architecture, and precision.
- The delegated `thirdparty/godot-cpp` sub-build still runs in one physical checkout with selected-platform mutable generated headers and SCons state.
- Alternating delegated Windows and Android root builds can therefore trigger `godot-cpp` regeneration/rebuild work even when CamBANG's own target-separated outputs are intact.

CamBANG workaround:
- Keep `godot_cpp=delegated` as the default one-command build mode.
- Provide `godot_cpp=external` for developers who prepare the selected `godot-cpp` generated header and static library themselves and want the root build to consume those artifacts without re-entering `thirdparty/godot-cpp`.

Removal criteria:
- Upstream `godot-cpp` delegated builds no longer share selected-platform mutable generated/SCons state in a way that causes cross-target rebuild churn, or CamBANG adopts an upstream-supported per-target artifact-state mechanism that does not require an external-consumption mode.
