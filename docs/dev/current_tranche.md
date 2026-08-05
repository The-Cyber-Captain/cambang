# Current tranche

## AcquisitionSession conformance for platform-backed providers (Camera2 first)

### Problem

`lifecycle_model.md` §2 and `provider_architecture.md` define `AcquisitionSession`
as the provider-reported acquisition seam for a device lineage, **retained while
stream and/or capture references remain live**, and brief §7 requires
created/destroyed facts only on genuine acquisition and release.

`SyntheticProvider` implements this: `ensure_native_acquisition_session_()` is
idempotent, the seam is held by three independent reference counts
(`stream_refs`, `capture_refs`, `priming_refs`), capture **admission** retains it
as a precondition and rolls the whole submission back if it cannot, and
`sync_capture_parent_priming` / `release_capture_parent_priming` are implemented.

Camera2 and WinRT implement none of it:

- The seam is realized lazily *inside the capture worker*
  (`ensure_session_configured_` / `ensure_reader_realized_`), as a side effect of
  servicing a capture.
- No reference counting. Seam lifetime is a side effect of output-set
  reconfiguration.
- Capture admission establishes nothing and takes no reference.
- The priming seam is unimplemented; both inherit `ERR_NOT_SUPPORTED`, so Core's
  `sync_capture_parent_priming_` call at profile-set is a discarded no-op on
  every platform-backed device.

Consequence on hardware: a capture with no stream runs against a session that was
improvised by an earlier capture and is retained by nothing.

### Design decision to record first

**A Camera2 `AcquisitionSession` is 1:1 with a native `ACameraCaptureSession`.**
A genuine reconfigure is a genuinely new seam and is reported as such; references
govern *when teardown is permitted*, not whether identity survives. The canonical
docs are currently ambiguous between this and a longer-lived device-lineage seam,
and must state one.

### Scope (Camera2)

1. Refcounted claimants: stream / capture / priming, mirroring Synthetic so the
   reference provider remains the readable model.
2. Establish-and-retain the seam at **capture admission**, not inside the worker.
   Failure to establish is an admission failure with full submission rollback.
3. Implement `sync_capture_parent_priming` / `release_capture_parent_priming`, so
   a profile-set realizes a truthful still-only seam at the retained geometry.
4. Resolve the `repeating_active` interlock so a legal claimant change can
   reconfigure rather than being refused outright.
5. Lifecycle honesty: no fabricated continuity, no fabricated destruction.

### Out of scope

- The identity/completion API (`capture_identity_and_lifecycle.md` §1–§5).
- WinRT conformance — same contract, different backend shape, its own tranche.
- The S20+ camera `0` capture failure.
- **Any keep-alive or repeating-request compensation.** This tranche is
  conformance. Whether conformance alone changes the Quest behaviour is a
  question to answer afterwards, not a goal to steer toward.

### Acceptance criteria

1. No Camera2 capture is admitted without an established, referenced seam;
   failure rolls the submission back.
2. A profile-set primes a truthful seam on Camera2, released per contract.
3. Session created/destroyed facts correspond to real transitions only.
4. A new `provider_compliance_verify` check binds establish-before-capture, and
   is **mutation-tested** — a provider that admits without establishing must
   fail it.
5. Existing gates green; scene 870 unregressed on Windows, Quest and S20+.

### Validation expectations

Deterministic (required):

- New compliance check plus mutation proof.
- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe`,
  `out/outstanding_payload_ledger_verify.exe` all green.

Hardware (required before acceptance):

- Quest 3, rig `50|51`, **no stream on either camera**, four captures ~6s apart.
  Record what happens. This tranche does **not** claim to fix that case.
- Windows / WinRT and S20+ / Camera2: no regression.

Report un-run surfaces plainly. Session configuration is the riskiest code in
the provider and is Android-only: no host verifier can catch a regression there.
