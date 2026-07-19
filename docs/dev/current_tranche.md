# Current tranche

No active tranche.

Tranche 6 (free-running tick flush split + frame-lease teardown hardening)
was committed 2026-07-19; all validation gates are closed, including
Android-over-ADB (maintainer manual run, 2026-07-19). Completed records live
in `docs/dev/completed_tranches/`.

## Authorized next work

The durable sequence in `docs/dev/codebase_audit_remediation_plan.md` queues
the advisory static-analysis sequence (per `docs/dev/static_analysis.md`) as
the required audit follow-up: advisory `.clang-tidy` config, changed-file
clang-tidy helper, CamBANG-specific advisory smell scanner, then a dated
baseline report and gate decision — as separate narrow changes, activated
one at a time by the maintainer.

## Unqueued candidates (maintainer decision required; none approved)

From the 2026-07-19 capture-path investigation and concurrency audit,
verified against source at that date:

* `ERR_BUSY` collapse of rig-trigger failure reasons at the Godot boundary
  (`CamBANGServer::trigger_rig_capture_internal_` returns 0 for every
  orchestration-failure category); explicitly deferred from the scene 870
  fix; recommended remedy: map ImagingSpec admission failures to
  `ERR_UNCONFIGURED` plus a one-shot diagnostic log.
* Still-bracket member-arrival-order contract (a Provider-brief question:
  real hardware delivers members asynchronously), with SyntheticProvider
  member-synthesis parallelization afterward as a contract-exercising
  feature — not before the contract is written.
* Shipped-GDE posture when the provider prompt/bounded contract is violated.
  NOTE: the current posture (wait truthfully past 2s while polling the
  liveness policy; log-only in GDE) is a *recorded, signed-off decision*
  (remediation plan, Tranche 1 sign-off correction, plus the watchdog
  severity decision). Any bounded-escalation/runtime-failed-latch proposal is
  a revision of that decision and needs an explicit plan amendment, not a
  routine tranche.
* Lazy snapshot Variant export (currently eager per publish); measure before
  scheduling.
* `to_image()` COW byte-cache at the wrapper layer; design constraint: cache
  hits must not be recorded as access-cost evidence (the public path records
  measured durations into `result_access_cost_evidence`).
* Consolidate the duplicated `*_trace_enabled()` env-var helpers
  (`display_demand_trace_enabled` defined in 4 files, `gpu_trace_enabled`
  in 2). This is the only residue of audit-ledger #6: the env vars
  themselves are all documented in `docs/dev/maintainer_tools.md` as of
  2026-07-19.

When the next tranche is activated, replace this file with its scope,
acceptance criteria, and near-term constraints -- do not accumulate a
history of completed work here (see `docs/dev/agent_context.md` §"Agent-
guidance maintenance").
