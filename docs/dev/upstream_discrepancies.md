# CamBANG – Upstream Discrepancies Log

> Working Note — Not Frozen

This document records deviations from:
- godot-cpp-template
- godot-cpp
- Godot SCons conventions

Each entry must include:
- Upstream reference (repo + issue link)
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