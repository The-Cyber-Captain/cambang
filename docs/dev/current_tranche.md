# Current tranche

## Tranche 7 - Truthful rig-trigger failure mapping at the Godot boundary

Status: active; maintainer-activated 2026-07-19.

### Purpose

Stop collapsing every rig-capture orchestration failure to `ERR_BUSY`.
`CamBANGServer::trigger_rig_capture_internal_()` discards the failure
category (`return 0` on any `!orchestration.ok`), so `CamBANGRig::
trigger_capture()` reports a permanent configuration gap — a missing
camera-concurrency truth for a multi-device rig — identically to ordinary
transient busy-ness. This masked the scene 870 rig-capture regression and
will trap every future integrator who forgets `ingest_camera_description()`.
The device-capture path already maps its statuses to distinct errors
(`map_try_trigger_device_capture_status()`); this tranche removes the rig
path's asymmetry, minimally.

### Scope

1. Surface the orchestration failure category from
   `CoreRuntime::RigTriggerOrchestrationResult` through
   `trigger_rig_capture_internal_()` to `CamBANGRig::trigger_capture()`
   via an internal seam change only (out-param or small struct return; the
   public method signature is unchanged).
2. Map exactly the two ImagingSpec admission-gate categories
   (`ImagingSpecUnavailable`, `ImagingSpecRejected`) to
   `godot::ERR_UNCONFIGURED`. Every other failure category keeps its
   current `ERR_BUSY` result in this tranche.
3. On the first rejected rig trigger per runtime session, emit one
   `ERR_PRINT` naming the concrete failure category (preflight/admission/
   submission enum value), so any collapse that remains is at least
   self-describing in the log.
4. Extend `tests/cambang_gde/scripts/73_rig_capture_result_set_verification.gd`
   with a leading negative phase: start without ingesting the camera
   description, trigger the multi-device rig, assert `ERR_UNCONFIGURED`,
   stop; then continue the existing ingest-then-start flow with all current
   assertions preserved unweakened.
5. Reconcile documentation that describes the old behavior: CLAUDE.md's
   rig-capture note, and `docs/architecture/godot_boundary_contract.md`
   only if it enumerates rig-trigger return codes.

### Non-goals / constraints

* No public API change: no new methods, signals, constants, or dictionary
  shapes; `trigger_capture() -> Error` signature untouched. The changed
  *values* for the two ImagingSpec categories are the maintainer-authorized
  semantic refinement this tranche exists for.
* No mapping changes for preflight or submission categories beyond the
  diagnostic log (revisit only with evidence they mislead in practice).
* No change to the fail-closed admission gate itself, Core orchestration,
  or provider code.
* Confirm scenes 73/870 do not assert the literal `ERR_BUSY` value before
  changing it; do not weaken any existing assertion.

### Expected implementation files

* `src/godot/cambang_server.h` / `cambang_server.cpp` (internal seam +
  mapping + one-shot log);
* `src/godot/cambang_rig.cpp`;
* `tests/cambang_gde/scripts/73_rig_capture_result_set_verification.gd`;
* `CLAUDE.md`; `docs/architecture/godot_boundary_contract.md` only if
  required by item 5;
* this file (reset to stub on acceptance).

### Acceptance criteria

* A multi-device rig trigger with no ingested camera-concurrency truth
  returns `ERR_UNCONFIGURED` (proven by the new scene 73 negative phase),
  and the same run's log names the failure category once.
* All pre-existing scene 73 steps still pass unmodified in strength.
* Genuine transient conditions still return `ERR_BUSY`.

### Required validation

```text
scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64
out/core_spine_smoke.exe
out/provider_compliance_verify.exe
```

```powershell
.\run_godot.ps1 -Scene res://scenes/73_rig_capture_result_set_verification.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche7_scene73
.\run_godot.ps1 -Scene res://scenes/65_public_boundary_verify.tscn -CaptureLogs -TimeoutSec 60 -RunLabel tranche7_scene65
.\run_godot.ps1 -Scene res://scenes/870_to_image_soak_benchmark.tscn -CaptureLogs -TimeoutSec 180 -RunLabel tranche7_scene870
.\run_godot.ps1 -RunPlatform android -Scene res://scenes/73_rig_capture_result_set_verification.tscn -CaptureLogs -TimeoutSec 90 -RunLabel tranche7_scene73_android
```
