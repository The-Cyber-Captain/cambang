# Current tranche

## Scene 870: provider-agnostic rig soak (platform-backed + synthetic), unified on the public API

### Goal

Expand the existing Scene 870 to-image soak/benchmark from synthetic-only into a
provider-agnostic rig soak that runs against **SyntheticProvider (default) or a
platform-backed provider (Camera2 on Android, WinRT on Windows), selected at
launch**. The soak's purpose broadens from "synthetic throughput under a
deterministic timeline" to **cross-provider capture/rig/bracket latency and
parallelism baselines** (recorded by the maintainer for anti-regression), and
specifically to prove **rig-trigger works with bracket captures** on this
branch. Stays on `first-camera2-provider`.

### Key architectural decision (agreed)

**870 stops being a scenario.** A scenario is synthetic-only timeline-replay
data; the platform path cannot use one, and driving synthetic on virtual time
while platform runs on the wall clock would make the new latency/parallelism
metrics non-comparable. So **both providers are driven through the public
Device/Stream/Rig API (the 270 pattern), no scenario for either.** Synthetic
presents its default two endpoints (endpoint_count=2, scenario-independent).
Consequence: the synthetic baselines are **re-recorded once** under the unified
harness (the maintainer records them); the new numbers measure the same real
processing cost the platform path does.

### Scope

0. **NEW public API (authorized): `CamBANGServer.create_rig(member_hardware_ids)
   -> CamBANGRig`.** Rig *formation* is currently synthetic-scenario-only (the
   scenario's `rigs` block -> synthetic staged topology -> Core
   `retain_member_hardware_ids`); there is no way to form a rig for a platform
   provider, and no public rig-creation API. This adds a server factory that
   mints a Core rig record + rig_id from member hardware_ids, **admitted only if
   the ingested concurrency truth authorizes the combination** (reuses the
   existing fail-closed gate), and returns the bound `CamBANGRig` handle -- the
   same server-minted-handle pattern as `get_device_for_hardware_id ->
   CamBANGDevice` and `create_stream -> CamBANGStream`. This is the enabler for
   provider-agnostic rigs and gates everything rig-shaped, so it is step 1.
   Touches Core + the locked public API (authorized for this tranche).

1. **Unify the bootstrap on the public API.** Replace the `provider_not_supported`
   bail and the synthetic scenario load with one provider-agnostic topology:
   enumerate -> engage two devices -> streams -> rig (+ ingest concurrency truth)
   -> profile/bracket -> soak phases. Provider chosen by `--cambang-bench-provider=`
   (default `synthetic`).
2. **Curated equipment table, in-scene.** A GDScript table declares the testable
   equipment (ids/names + which id-combinations are concurrent). Device identity
   is resolved by **enumerate/map** (WinRT ids are machine-specific symbolic
   links); the curated concurrency capability, resolved against enumeration,
   builds the `ingest_camera_description` truth fed to Core.
3. **Platform bootstrap caveats.** Android camera permission is **pre-granted**
   (run_godot.ps1 `-AndroidGrantPermissions`) and the scene **fails closed to
   expected_unsupported** if not granted (no interactive dialog in a soak). Rig
   needs two concurrently-usable cameras: where a platform can't provide them,
   the rig phase **degrades to a single-device soak** rather than failing. WinRT
   two-camera concurrency is **discovered, not asserted** -- ingest the claimed
   combination, attempt the rig, and report the runtime result either way.
4. **Rig-trigger with brackets.** The rig capture path submits bracket bundles
   per device; verify per-member realized facts and acquisition marks.
5. **Acquisition-mark survey.** Per rig device, present 270-style per-member
   bracket facts, plus each device's **member-0 result-return timestamp** as the
   parallelism probe (does Camera A's work delay Camera B's result?). Kept
   minimal to start; refined once real facts are in hand.
6. **run_godot.ps1.** Teach the Android ExtraArgs translator
   `--cambang-bench-provider=` (it currently whitelists four args and throws on
   unknown), so the mode is injectable on-device.
7. **Reporting compatibility.** Additive fields only; keep the summary
   provider-tagged. Synthetic is re-baselined under the unified harness.

### Explicitly out of scope

- GPU-fill payloads (platform providers are CPU-only; keep the harness
  GPU-forward-compatible, nothing more). Its own later tranche.
- Raising `kMaxBracketMembers`; the scene-70 Android-verdict / headless gap.

### Acceptance / validation

- Synthetic 870 (unified, no scenario) runs headless on host and windowed on
  Android, self-terminates, emits the summary record; maintainer re-records
  baselines. Two synthetic devices engage + stream + rig concurrently.
- Platform 870 on the S20 (Camera2): rig where concurrency is supported (else
  documented single-device degrade), bracket rig-trigger works, acquisition-mark
  survey populated with real marks.
- Platform 870 on Windows (WinRT): two-camera rig attempted; concurrency result
  (works / refused) reported truthfully.
- Native verifiers unaffected (scene + run_godot.ps1 changes only; no provider
  behaviour change in this tranche).
