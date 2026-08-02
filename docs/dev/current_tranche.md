# Current tranche

## Stale capture payload attribution repair

### Problem

On a device whose capture session carries no repeating request, a platform may
deliver a capture payload *after* the collecting capture has been abandoned.
The next capture on that device then adopts it, because a single-member capture
whose payload matches no result of its own falls through to positional pairing
and is handed the current capture's facts.

Observed on Quest 3 / Camera2 (2026-08-02): rig capture 2 on the stream-less
member timed out with `images=0 ... settled=no` after the 5s sample wait, and
capture 3 collected a payload in 9 ms — less than its own 6 ms exposure plus
pipeline, i.e. an already-exposed buffer. Confirmed on device: from that point
the stream-less camera is **one image behind**, every capture reporting success.

The defect is not Quest-specific. Any device that ever abandons a capture can
then serve stale payloads indefinitely, all reporting success.

### Scope

1. **Outstanding-payload accounting, per device backend.** When a burst ends
   with fewer payloads than expected, record the shortfall as debt owed by that
   device. The next burst on that device discards exactly that many arrivals
   before accepting any as its own, decrementing as it goes. Discards are
   counted and logged, never silent.

2. **Positional pairing is conditional.** A single-member capture may fall back
   to positional pairing only when that device's debt is zero. With debt
   outstanding, an unmatched payload is refused rather than attributed.

3. **Explicit per-device single-capture guard in the provider.** Camera2's
   admission currently rejects only a duplicate `(capture_id, device)` and
   otherwise serialises by blocking a worker on `still_capture_mutex`. Refuse
   deterministically instead: a device with any in-flight capture returns
   `ERR_BUSY`. This implements `capture_identity_and_lifecycle.md` §3's
   per-device rule early, because debt attribution is unsound without it.

4. **Audit the WinRT provider for the same shape** and apply equivalent
   accounting if the same abandon/pair path exists. Not yet verified either way.

Attribution must be by accounting only. Acquisition marks must not be used to
decide ownership, freshness, or ordering (`camera_fact_model.md` §12.2; marks
may legitimately be identical across simultaneously triggered devices).

### Out of scope

- The capture identity split, public API shape, completion signals, cohort
  states, and rig membership mutation — all of
  `capture_identity_and_lifecycle.md` except §3's per-device rule above.
- Core-side arbitration policy.
- Tuning `kCaptureSampleWaitMs`, or any attempt to make the platform deliver on
  time. This tranche makes the failure honest, not absent.

### Acceptance criteria

1. A payload delivered after its capture was abandoned is **never** attributed
   to a subsequent capture on that device. Deterministically demonstrated by a
   maintainer verification case, not only on hardware.
2. A capture that cannot be satisfied reports its terminal error. It must not
   silently become a shorter result set with no reason recorded.
3. A second concurrent capture on one device is refused with `ERR_BUSY` rather
   than blocking a provider worker.
4. Discarded (debt-settling) payloads are counted and visible in provider
   diagnostics.

### Validation expectations

Deterministic (required):

- A verification case reproducing withheld-then-late payload delivery without
  hardware. Designing this is part of the tranche: the reference providers must
  be able to withhold a payload and release it on a later request. Do not weaken
  the assertion to fit the existing providers.
- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe` all green.

Hardware (required before acceptance):

- **Quest 3, Camera2, platform-backed, rig of 50/51 with a stream on one member
  only.** Capture 2 is still expected to fail — the platform withholds the
  payload. Capture 3 must be **freshly exposed**, not capture 2's frame. The
  human check is lens occlusion: cover the stream-less camera for exactly one
  click; the blackout must appear in that click's result or in none, and must
  never appear in the next click's.
- Windows / WinRT: scene 870 platform-backed, no regression.
- S20+ / Camera2: no regression.

Report un-run surfaces plainly. Native-tool PASS does not prove the Godot
scenes, and one handset does not prove another.
