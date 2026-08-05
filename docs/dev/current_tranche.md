# Current tranche

## Rig capture returns an inconsistent result set

### Problem

`CamBANGRig.trigger_capture()` does not reliably return one
`CamBANGCaptureResult` per rig member. The reported symptom: two results on the
first rig capture, one on the second, and a caller indexing `rig_results[1]`
crashes on the second.

Whether that still reproduces is **unknown**. Every hardware observation of it
predates the Camera2 work now on this branch, and that work changed the two
things most likely to have caused it:

- Stills submitted into a session with no flowing pipeline frequently produced
  nothing. Every capture now starts a pilot stream, and single-device delivery
  went from as low as 1-in-4 to 142/142 across all five cameras available here.
- A capture that came up short recorded a payload debt that the next capture's
  own image discharged, so one lost frame starved the device permanently. A rig
  member losing one image was enough to make every later capture return fewer
  members than it had. Sequence-end settlement plus the in-flight grace removed
  both halves of that.

A rig whose members each delivered unreliably could produce exactly the reported
symptom with no rig-specific defect at all. That has to be settled before
anything is designed.

### Step 1 — reproduce, or don't

Run rig capture on hardware under the current build and record what comes back,
per capture, per member. Nothing else in this tranche is designed until that is
known.

Three outcomes, and they lead different places:

- **Does not reproduce.** Confirm it across enough captures and cameras to
  mean something, then the work is regression coverage that would have caught
  it, not a repair.
- **Reproduces, and members are individually delivering.** A genuine rig
  defect: the result set is being assembled or published wrongly. Design from
  there.
- **Reproduces, and some member delivered nothing.** The single-device work is
  incomplete under rig conditions -- concurrent sessions on two cameras is a
  configuration none of the single-device runs exercised.

Report which of these it is, with the per-member evidence, before proceeding.

### What is already known and must not be rediscovered

- Rig capture fails closed: a multi-device capture is rejected unless a
  camera-concurrency truth naming the exact device combination was ingested via
  `CamBANGServer.ingest_camera_description(...)` **before** `start()`. Missing
  that returns `ERR_UNCONFIGURED`; other orchestration failures return
  `ERR_BUSY`. `73_rig_capture_result_set_verification.gd` has the correct ingest
  pattern.
- There is no rig-capture completion protocol. Every working scene hand-rolls
  one. Whether this tranche should supply one depends on step 1's outcome;
  it is not assumed.
- `capture_id` is minted at the Godot boundary
  (`CamBANGServer::next_capture_id_`) and shared across device and rig paths.
- Acquisition marks may legitimately be identical across simultaneously
  triggered devices (`camera_fact_model.md` §12.1/§12.2). They must never be
  used for member identity, ordering or attribution. Reporting them
  diagnostically is fine.

### Out of scope

- The identity/completion API (`capture_identity_and_lifecycle.md` §1–§5),
  unless step 1 shows the result set cannot be made correct without it.
- WinRT: same contract, different backend shape, its own tranche.
- Pilot-stream lifetime. Settled by measurement: per-capture, 10-15ms better at
  p50 on every camera, delivery identical. `kPersistentPilotTest` remains only
  as a diagnostic switch.
- Further S20+ camera 0 work. It delivers 28/28; its refusal rate under a 0.1s
  schedule is the schedule outrunning capture duration, not a fault.

### Acceptance criteria

Set once step 1 has an answer. A tranche that closes without either a
reproduction or a documented failure to reproduce has not done its job.

### Validation expectations

Deterministic (required):

- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe`,
  `out/outstanding_payload_ledger_verify.exe`,
  `out/capture_sequence_settlement_verify.exe` all green.
- Any new rule gets a mutation proof, as the settlement policy did: removing it
  must fail the verifier.

Hardware (required before acceptance):

- Quest 3 rig `50|51`, and at least one two-camera rig on a handset. Windowed.
- Single-device delivery unregressed on all five cameras — the foundation this
  tranche stands on, and cheap to re-measure.
- Windows / WinRT: no regression.

Scene 1001 is read-only. Scene 999 is the untracked single-device probe and is
not to be added to git. Report un-run surfaces plainly.
