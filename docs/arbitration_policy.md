# CamBANG Arbitration and Preemption Policy

This document defines the deterministic rules CamBANG core uses to
decide **what runs**, **what is denied**, and **what is preempted** when
multiple operations compete for camera resources.

This policy is implemented by core; providers do not arbitrate.

Scope: - repeating streams (`StreamIntent`: `PREVIEW`, `VIEWFINDER`) -
device-triggered still capture - rig-triggered synchronised capture -
profile compatibility checks - deterministic outcomes and error
reporting

------------------------------------------------------------------------

## 1. Definitions

### 1.1 Operations

-   **Repeating stream**: a continuous frame flow for a device instance
    (`CamBANGStream`).
-   **Triggered device capture**: a single still capture requested on a
    device (`CamBANGDevice.trigger_capture()`).
-   **Triggered rig capture**: a synchronised capture requested on a rig
    (`CamBANGRig.trigger_sync_capture()`).

### 1.2 Ownership and membership

-   A device may be **standalone** (`rig_id = 0`) or a **rig member**
    (`rig_id != 0`).
-   When a rig is **ARMED**, it is authoritative for member device
    capture pipelines.

### 1.3 Profiles

CamBANG uses **Capture Profiles** for both streams and stills.

-   Stream profiles apply to repeating streams.
-   Still profiles apply to triggered capture.

Core validates and normalizes profiles before execution (§4).

------------------------------------------------------------------------

## 2. Priority order

When operations conflict, core uses the following strict priority order:

1.  **Rig-triggered synchronised capture**
2.  **Device-triggered still capture**
3.  **Repeating streams** (VIEWFINDER, PREVIEW)

Repeating streams are always preemptible by triggered capture.

------------------------------------------------------------------------

## 3. One-stream-per-device invariant

CamBANG supports at most **one active repeating stream per device
instance** (design choice).

Rules: - A second stream request on the same device instance is denied
unless it replaces the existing stream (stop old, start new), subject to
rebuild policy. - The replacement must be deterministic and must not
violate rig authority (§5).

------------------------------------------------------------------------

## 4. Profile validation and compatibility

Core validates profiles before calling provider. A request may be
rejected deterministically at validation time.

### 4.1 Provider constraints

If provider reports hard constraints that prevent a validated profile
from running, core fails the request with a deterministic error code.

### 4.2 Compatibility classes (v1)

For v1, core uses a conservative compatibility rule-set:

-   Stream formats must be raw-only.
-   Still formats may include `'JPEG'` and `'RAW '` (where supported).
-   If a device is a rig member and rig is ARMED, any device-level
    capture or stream request must be compatible with the rig's
    configured requirements, otherwise it is rejected.

Compatibility is defined as "can be satisfied without reconfiguring the
rig-authoritative pipeline beyond allowed policy". v1 defaults to strict
behaviour (no implicit reconfigure of an ARMED rig pipeline to satisfy
standalone requests).

------------------------------------------------------------------------

## 5. Rig authority rules

When a device is a member of a rig:

### 5.1 Rig disarmed (`OFF`)

Device is effectively standalone for arbitration purposes.

### 5.2 Rig armed (`ARMED`, `TRIGGERING`, `COLLECTING`)

Rig is authoritative. For member devices:

-   **Rig-triggered capture** always takes priority.
-   **Device-triggered capture** is allowed only if it is compatible and
    does not reduce rig readiness.
-   **Repeating streams** are allowed only if compatible and not in
    conflict with rig capture activity.

If not allowed, core returns a deterministic error ("rig
authoritative").

------------------------------------------------------------------------

## 6. Preemption rules (v1)

### 6.1 Triggered capture vs streams

When a triggered capture begins on a device instance:

-   Any repeating stream on that device instance may be stopped
    (preempted) according to policy.
-   v1 default policy: stop repeating streams during the capture window
    for determinism on affected devices.

### 6.2 VIEWFINDER rule (v1 simple)

When a triggered capture is in-flight (device or rig), `VIEWFINDER` is
denied or preempted to `STOPPED` on affected devices.

v1 behaviour is intentionally strict: - VIEWFINDER does not attempt
adaptive downgrades - VIEWFINDER does not "queue" behind capture by
default (requests fail fast)

### 6.3 PREVIEW behaviour

PREVIEW may also be stopped during capture, depending on the selected
sync-integrity / determinism policy. v1 default is to stop repeating
streams on affected devices during capture.

------------------------------------------------------------------------

## 7. Deny vs preempt vs fail

CamBANG distinguishes outcomes:

-   **Deny**: request is not started; returns error immediately.
-   **Preempt**: an already-running lower-priority operation is stopped
    to satisfy a higher-priority operation.
-   **Fail**: an operation was started but could not complete (provider
    error, timeout, etc.).

### 7.1 Deterministic denial

Denials must be deterministic based on current core state and policy
knobs.

### 7.2 Deterministic preemption

Preemption must be deterministic: - which operations are stopped - in
what order - what state transitions occur - what counters are
incremented

------------------------------------------------------------------------

## 8. Counters and state updates

This section defines minimum counter semantics required for consistency.

### 8.1 Rig counters

-   `captures_triggered` increments when rig capture accepted and
    started.
-   `captures_completed` increments on successful completion.
-   `captures_failed` increments on completion with error.

`last_capture_id`, `last_capture_latency_ns`, `last_sync_skew_ns` are
updated on completion (success or failure where meaningful).

### 8.2 Device counters

-   `errors_count` increments on device-level error events (provider
    reports).
-   `rebuild_count` increments when core directs a pipeline rebuild for
    that device instance.

### 8.3 Stream counters

-   `frames_received` increments when provider delivers a frame.
-   `frames_delivered` increments when core delivers frame to consumer.
-   `frames_dropped` increments when core drops a frame (queue full /
    latest-only).
-   `queue_depth` reports instantaneous queue size at publish time.

### 8.4 Denial does not mutate counters

Denied requests do not increment "triggered" counters; they increment
only error fields if an error is reported (policy-dependent). v1
default: return error code but do not increment failure counters.

------------------------------------------------------------------------

## 9. Capture IDs and determinism

Core issues `capture_id` as a monotonic `uint64`.

Rules: - capture_id is assigned when a capture request is accepted (not
when it completes). - rig-triggered capture assigns one capture_id for
the sync set. - device-triggered capture assigns one capture_id for that
device.

No cross-satisfaction in v1: - a rig capture does not satisfy a
device-triggered request, even if equivalent. - a device capture does
not satisfy a rig request.

(Optimization may be considered for v2 with explicit rules.)

------------------------------------------------------------------------

## 10. v2 direction (non-normative)

Future policy may include: - Adaptive VIEWFINDER downgrades
(resolution/FPS) under capture pressure - Soft queuing of VIEWFINDER
requests - Metric-driven arbitration (thermal/bandwidth/latency) -
Optional reuse of rig frames to satisfy device requests when explicitly
enabled

v1 intentionally avoids fuzzy behaviour; failures are explicit.

------------------------------------------------------------------------

## 11. Error codes (placeholder)

Exact numeric error codes are not frozen here. v1 requires stable named
categories, such as:

-   `ERR_NOT_SUPPORTED`
-   `ERR_RIG_AUTHORITATIVE`
-   `ERR_PROFILE_INCOMPATIBLE`
-   `ERR_BUSY`
-   `ERR_PROVIDER_FAILED`

Numeric mapping is defined in code and documented separately.

------------------------------------------------------------------------

## 12. Summary

-   Rig capture has highest priority.
-   Device capture has priority over repeating streams.
-   One repeating stream active per device instance.
-   Rig authority applies when ARMED.
-   v1 VIEWFINDER is strict: deny/preempt during capture; no adaptive
    fallback.
-   Deny/preempt/fail semantics are explicit and deterministic.
