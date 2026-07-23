# Current tranche

## Permission-aware harnesses (standard Android) + Scene 270

### Goal

Make camera-permission handling in the test harnesses explicit and reliable on
standard Android, instead of the current implicit reliance on a persisted grant.
Then write Scene 270, the first human-interactive (2##) harness, carrying a
suitable interactive permission solution.

### Background

Camera access is a runtime permission on Android; the harness grants nothing
today (`adb install -r`, no `-g`; no `pm grant`), so platform-backed scenes only
work because `android.permission.CAMERA` happens to be granted and persists. The
Camera2 provider's early readiness gate cannot see the runtime permission (no
NDK query), so a missing grant surfaces only at `open_device`, and the public
API returns a bare `FAILED` at engage that a scene cannot distinguish from other
failures. Godot's own permission APIs are unreliable as a readiness primitive
(docs/dev/upstream_discrepancies.md). Two harness classes therefore need two
approaches.

### Scope

1. **run_godot.ps1: grant camera permission on Android before launch.** After
   install, `adb shell pm grant <pkg> <perm>` for an extensible permission list
   (default `android.permission.CAMERA`). Report a failed grant loudly rather
   than proceeding to a silent downstream failure. Windowless technical
   verifiers (768) rely on this. Windows path unaffected.

2. **Scene 270: first human-interactive harness.** A public-API lifecycle scene
   (loosely a UI-driven facsimile of 70_result_retrieval_verification) that
   builds topology through the GDScript Device/Stream API (no scenario), lets the
   user switch provider kind (synthetic / platform-backed), and carries the
   interactive permission flow: prerequisites listed in the scene; check the
   granted list; if unsatisfied, request and await `on_request_permissions_result`
   (matched+false = explicit denial -> bail; matched+true -> re-check granted
   list; non-match -> keep waiting). Granted-list re-check is the source of truth;
   `request()`'s return is not trusted. All Android-branched via
   `if OS.get_name() == "Android"` so it no-ops on Windows/WinRT. This flow sits
   BEFORE provider start and composes with the provider's own readiness gate.

### Explicitly out of scope

- **Meta Horizon headset specifics.** Keep `horizonos.permission.HEADSET_CAMERA`
  compatibility in mind (the interactive flow's prerequisite list and export
  custom_permissions are the seams), but do NOT implement Meta specifics until
  the headset is in hand. Two unknowns are unresolved without it: how a scene
  detects a Horizon headset vs generic Android, and whether Godot's
  request/get_granted machinery handles a custom HorizonOS permission at all.
- Writing a Godot permissions system (GDE plugin etc.) -- not CamBANG scope, not
  deferred.
- The GPU-fill payload tranche and further soak harnesses (queued separately).

### Acceptance / validation

- run_godot.ps1 grant verified on device: revoke `android.permission.CAMERA`,
  run 768 platform-backed, confirm the harness grants it and the platform pass
  succeeds (proving the grant does real work, not relying on a persisted grant).
- Existing windowless scenes unaffected: 768 still PASS on Android and Windows
  host; 568 still PASS.
- Scene 270 is human-interactive, so its acceptance is a maintainer interactive
  run (both provider kinds, permission grant and explicit-denial paths), not a
  windowless harness verdict. It must still degrade cleanly (skip / clear
  message) when a provider or permission is unavailable.
- No platform-specific branching outside the provider except the permitted small
  `if OS.get_name()` checks in GDScript harness code.
