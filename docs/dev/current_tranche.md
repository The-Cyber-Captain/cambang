# Current tranche

## Capture identity: split the device and rig id spaces

Tranche 1 of the `capture_identity_and_lifecycle.md` implementation. Internal
only — no Godot-facing surface changes.

### Problem

One monotonic `uint64`, minted at the Godot boundary as
`CamBANGServer::next_capture_id_`, serves both device-triggered and
rig-triggered captures, and a rig capture shares its id with every member.
`arbitration_policy.md` §9 records this; `capture_identity_and_lifecycle.md`
§2.1 replaces it.

The consequence is not cosmetic. A rig capture and its members are
indistinguishable by id, so no registry can separate "member of rig capture R
on device D" from "standalone capture on device D". Per-member dispositions,
the per-device in-flight guard, rig preemption and membership versioning all
need that distinction, and none of them can be built while the ids collide.

### Decision

Two internal id spaces, both session-scoped `uint64`, both minted at the
boundary:

- **Device Capture Id** — every device capture, whether standalone or a rig
  member.
- **Rig Capture Id** — the rig capture itself.

A rig trigger mints one Rig Capture Id and N Device Capture Ids, one per
member. Cohort records key on the Rig Capture Id and carry the member map;
assembly, result and acquisition-session records key on the Device Capture Id.

Durable public ids (§2.2) and returning identity from a trigger (§4.1) are a
later tranche. This one keeps `trigger_capture()` returning `Error` on both
wrappers.

### Scope

1. Two counters at the boundary, replacing `next_capture_id_`.
2. `CoreCaptureCohortRegistry` keyed by rig capture id; `Participant` gains a
   `device_capture_id`.
3. `CoreCaptureAssemblyRegistry`, `CoreResultStore`,
   `CoreAcquisitionSessionRegistry` keyed by device capture id.
4. Boundary maps rig capture id to its member device capture ids;
   `latest_capture_id_by_device_instance_id_` holds device capture ids.
5. `CamBANGRig::get_result()` resolves via the rig capture id,
   `CamBANGDevice::get_result()` via the device capture id. Signatures
   unchanged.
6. `get_capture_result_set_by_id(capture_id)` takes a rig capture id;
   `get_capture_result_by_id(capture_id, device_instance_id)` takes a device
   capture id. Both stay integer-keyed — they are the advanced/diagnostic
   surface, and `device_instance_id` is already session-scoped, so a durable id
   in the other position would advertise a durability the method cannot honour.
7. Anything `snapshot_builder` / `state_snapshot_export` projects must name
   which space it is in.

### Out of scope

- Durable public ids (§2.2), result fields (§2.3), trigger returning identity
  (§4.1), and the consumer migration that follows them.
- Completion signals, canonical wrappers, outstanding set (§4.2, §4.5).
- Dispositions, cohort closure, simultaneity window (§4.3, §4.4).
- Per-device in-flight guard and rig preemption (§3).
  `has_capture_in_flight_for_device()` stays snapshot-only here.
- Provider concurrency capacity (§3.1). Settled: Camera2 NDK surfaces no
  runtime concurrency information, the ingested ADC truth is the only gate, and
  no `ICameraProvider` method will be added. §3.1's "providers therefore
  declare their concurrent device-capture capacity" is wrong as written and is
  corrected in the tranche that touches arbitration, not this one.
- Rig membership lifecycle (§5).

### Acceptance criteria

1. A rig capture and a standalone capture on one of its member devices, both in
   flight, never collide on any registry key.
2. Rig member results resolve to the correct device, and cohort membership is
   recoverable from the rig capture id alone.
3. No Godot-facing signature, return type, constant or dictionary key changes.
   The 13 consumers that break under the public-identity tranche are untouched
   by this one.
4. Mutation proof: collapsing the two counters back into one must fail a check.
   **Corrected during implementation** — this criterion originally named
   `provider_compliance_verify`, on the assumption that the boundary's minting
   could be driven host-native. It cannot: the counters live on
   `CamBANGServer`, and every maintainer verifier constructs `CoreRuntime`
   directly. Mutating the boundary minter to return the rig capture id was
   confirmed to survive all nine gates.
   The guarantee is therefore bound one level down, in
   `CoreCaptureCohortRegistry::insert()`, which refuses a member whose Device
   Capture Id equals its cohort's Rig Capture Id, duplicates a sibling's, or is
   already owned by another live cohort. `phase3_snapshot_verify` proves all
   three, and removing the first rejection fails that check. A single-counter
   regression at the boundary now fails closed at the registry rather than
   silently colliding — but note what this does and does not cover: the
   boundary's own minting still has no host-native test, and does not get one
   until something can drive `CamBANGServer` outside Godot.
5. Existing gates green — and here a green `godot_test_suite.ps1` is meaningful
   precisely because the public surface did not move.

### Validation expectations

Deterministic (required):

- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe`,
  `out/outstanding_payload_ledger_verify.exe`,
  `out/capture_sequence_settlement_verify.exe`,
  `out/acquisition_seam_claims_verify.exe`, `out/phase3_snapshot_verify.exe`.

Godot (required):

- `.\godot_test_suite.ps1` — covers 65 and 70.
- `73_rig_capture_result_set_verification.tscn`, windowed. The rig result set
  is the surface this tranche is most likely to break.

Hardware: not required. No provider or device behaviour changes in this
tranche.

**Build the GDE, not just the verifiers.** `scons gde=no` links the maintainer
tools only; a Core change is not in the plugin until `scons gde` has run.
