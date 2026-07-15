# Current Tranche

## Pre-Dispatch Stream-Frame Coalescing Accounting

### Objective

Correct and verify Core accounting for repeating stream frames superseded in
`CoreRuntime` before dispatcher delivery.

A superseded frame that never reaches a sink must be counted as received and
dropped, but not delivered. Its payload must still be released exactly once.

### Confirmed defect

The current pre-dispatch coalescing path records the stale frame through:

- `on_frame_received`;
- `on_frame_released`;
- `on_frame_dropped`;
- direct payload release without sink invocation.

The snapshot builder projects the internal `frames_released` counter as public
`frames_delivered`. This therefore reports a frame that never reached a sink as
both delivered and dropped.

### Locked accounting semantics

For two eligible frames with the same coalescing key, where the older frame is
superseded and the newer frame reaches the sink:

```text
frames_received  = 2
frames_delivered = 1
frames_dropped   = 1
```

Payload release and delivery accounting are distinct:

- a stale pre-dispatch frame is released by Core but is not delivered;
- a surviving frame is delivered only when handed to an installed sink;
- each payload must be released exactly once through its established ownership
  path.

Within the current model, `frames_released` is the internal backing counter for
public `frames_delivered`; it must not be incremented for pre-sink release.

### Coalescing rules to preserve

The production coalescing key is:

```text
(stream_id, acquisition_session_id)
```

Eligibility requires:

```text
capture_id == 0
stream_id != 0
```

Coalescing must not cross the first non-repeating provider fact. Lifecycle,
native-object, error, capture, and other non-repeating facts remain non-lossy
barriers.

Acquisition timing, retained-frame identity, backing identity, and payload
identity do not participate in the coalescing key.

### Required work

1. Make the narrowest production correction so a stale coalesced frame records
   received and dropped accounting, releases exactly once, and does not record
   delivery.
2. Audit other pre-sink drop paths for the same delivered-and-dropped
   contradiction; correct only confirmed equivalent cases.
3. Add native verification through the real `CoreRuntime` provider-fact queue
   for:
    - same-key supersession;
    - a non-lossy barrier between otherwise coalescible frames;
    - frames differing in one coalescing-key component.
4. Verify sink observations, exact counters, ordering, Core-owned receipt
   chronology, acquisition-timing separation, and exactly-once payload release.

A downstream direct-dispatcher test does not satisfy the runtime-queue coverage
requirement.

### Scope constraints

No:

- public Godot API or binding changes;
- snapshot schema or field-name changes;
- acquisition-timing, Capture Date-Time, or identity redesign;
- provider API or platform-provider work;
- broad counter-system rename;
- test-only duplicate coalescing implementation;
- weakening of exact accounting assertions.

Production behaviour outside confirmed pre-sink accounting defects must remain
unchanged.

### Documentation

Update existing canonical documentation only where needed to make the durable
accounting distinction explicit:

- `docs/architecture/frame_sinks.md`: release-on-drop does not constitute sink
  delivery; pre-dispatch coalesced frames are dropped, not delivered.
- `docs/architecture/core_runtime_model.md`: preserve the coalescing key,
  eligibility, barrier rule, and stale-frame accounting.
- `docs/architecture/state_snapshot.md`: only if its counter definitions do not
  already unambiguously define delivered as sink handoff.

Do not create a new architecture document for this tranche.

### Validation

Run the repository-supported maintainer build and the new or extended
runtime-queue coalescing verifier.

Also run all affected deterministic verification, including at minimum:

```text
phase3_snapshot_verify
core_spine_smoke
core_result_path_smoke
core_dispatcher_bracket_routing_smoke
provider_compliance_verify
```

Run the required GDE build matrix only if shared production GDE source changes.

Run `git diff --check`.

### Completion criteria

This tranche is complete when:

- stale pre-dispatch frames are no longer projected as delivered;
- same-key supersession, barrier preservation, and key separation are directly
  verified through the real runtime queue;
- accounting, chronology, ordering, and exactly-once release are proven;
- canonical documentation is consistent;
- affected validation passes;
- the final diff receives human review and approval.
