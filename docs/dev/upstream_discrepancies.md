# CamBANG – Upstream Discrepancies Log

> Working Note — Not Frozen

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
