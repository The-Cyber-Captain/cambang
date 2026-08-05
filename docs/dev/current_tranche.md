# Current tranche

## AcquisitionSession conformance for WinRT

### Decision, taken

**A WinRT `AcquisitionSession` is the realized `MediaFrameReader`.** The output
set belongs to the `MediaFrameSource`, not to the seam, so a `SetFormatAsync`
geometry change is a mutation within one seam: identity persists and no
destroyed/created pair is reported.

`lifecycle_model.md` needs no rule change — its reconfiguration rule is already
conditional on the native object being 1:1 with the seam, which WinRT's reader
is not. It carries one line asserting that `WinrtCameraProvider` does not meet
this contract, which this tranche falsifies and must correct.

The mapping itself belongs with the provider, in its own source comments — and
in a provider-specific document only if that proves genuinely insufficient.

Two consequences to implement against:

- References govern when the **reader** may be torn down. They do not govern
  reconfiguration, because a format change does not destroy the reader.
- Zero references therefore makes teardown permitted, not mandatory. A warm
  reader may be kept; an explicit release from Core must still be honoured.

### Problem

The WinRT seam is **not reference-held**. `lifecycle_model.md` §2 and brief §7.1
require it to be retained while any of three claimants — stream, capture,
capture parent — needs it, and require the provider to be able to say which
currently do. WinRT has one session id on the backend and no claimant concept.

Everything below is that one defect at its various call sites:

- Capture admission establishes nothing and takes no claim; the seam is
  realized later inside the capture worker.
- `sync_capture_parent_priming` / `release_capture_parent_priming` are not
  implemented, so the seam can never be created from the retained still
  profile — only from whatever the first stream or capture happens to ask for.
- Session teardown fires only at device close and shutdown. Stream stop
  releases nothing.
- A started stream pins geometry via a hard-coded `ERR_PLATFORM_CONSTRAINT`
  refusal, standing in for the reference check and covering streams only, so an
  in-flight capture is not protected.

### Scope

1. Record the seam mapping in the provider source, and correct the stale
   conformance line in `lifecycle_model.md`.
2. Refcounted claimants: stream / capture / capture parent, mirroring
   `SyntheticProvider` so the reference provider stays the readable model.
3. Establish-and-retain the seam at **capture admission**. Failure to establish
   is an admission failure with full submission rollback. Realizing the native
   object may still be deferred to the bounded control executor.
4. Implement the capture-parent path so a retained-profile set creates a
   truthful seam at the retained geometry, idempotent for equivalent requests,
   and releasing a claim never destroys the seam.
5. Generalise the started-stream geometry pin to cover in-flight captures. This
   is a separate question from teardown and must stay separate: reconfiguration
   consults the stream and capture claims only, never the capture-parent latch,
   whose whole purpose is to set geometry from the retained profile.
6. Lifecycle honesty: created and destroyed facts on real transitions only. No
   fabricated destruction, and no created fact for a reader that was never
   realized.

### Out of scope

- Camera2, which already conforms.
- Rig-capture repair. Complete, and its evidence is in git history.
- WinRT bracketing.
- Any use of Media Foundation. `windows_winrt` means real WinRT APIs; surface a
  toolchain blocker rather than substituting a backend.

### Acceptance criteria

1. The seam mapping is stated in the provider source, the implementation
   matches it, and `lifecycle_model.md` no longer claims WinRT fails this
   contract.
2. No WinRT capture is admitted without an established, referenced seam;
   failure rolls the submission back whole.
3. A retained-profile set creates a truthful seam at the retained geometry, and
   releasing a claim leaves it standing.
4. Session created/destroyed facts correspond to real transitions only.
5. A new `provider_compliance_verify` check binds establish-before-capture on
   the WinRT shape and is **mutation-tested** — a provider that admits without
   establishing must fail it.
6. Existing gates green.

### Validation expectations

Deterministic (required):

- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe`,
  `out/outstanding_payload_ledger_verify.exe`,
  `out/capture_sequence_settlement_verify.exe` all green.
- Any new rule gets a mutation proof: removing it must fail the verifier.

`provider_compliance_verify` exercises the contract through recording providers
and never instantiates `WinrtCameraProvider`. Decide early what conformance can
be bound host-native and what genuinely needs the Windows device surface;
otherwise a green gate will say nothing about this work.

Hardware (required before acceptance):

- Windows platform-backed scenes, windowed. Camera2 must be re-run only if
  shared Core code changes.

Report un-run surfaces plainly.
