# CamBANG Provider Architecture

This document defines the **core ↔ provider boundary** for CamBANG and
the obligations of platform providers.

A **provider** is the platform-specific backend responsible for
enumerating camera endpoints, controlling native camera APIs, producing
frames, and reporting owned native object lifecycles back to core.

Providers are execution engines. **Core is the authority**.

------------------------------------------------------------------------

## 1. Goals

Providers must support:

-   Deterministic behaviour (ordering, threading, teardown)
-   Correct object lifecycles and resource management
-   Canonical mapping of platform formats into CamBANG pixel formats
    (FourCC-style `uint32`)
-   Multi-camera synchronised capture where platform/hardware supports
    it
-   Testability via a synthetic provider

Providers must not:

-   Expose platform semantics directly to Godot-facing objects
-   Own or publish `CamBANGStateSnapshot`
-   Implement arbitration/policy decisions (rig priority, preemption
    rules)
-   Retain references to core objects beyond their documented lifetime

------------------------------------------------------------------------

## 2. Responsibilities

### 2.1 Core responsibilities

Core owns and decides:

-   Arbitration and preemption (rig \> device \> stream intent)
-   Warm timing (`warm_hold_ms`) and teardown scheduling
-   Validation and normalization of Capture Profiles
-   Spec stores (CameraSpec, ImagingSpec) and application of patches
-   `CBLifecycleRegistry` and retention of destroyed records
-   `CBStatePublisher` and snapshot publication

### 2.2 Provider responsibilities

Provider owns and implements:

-   Platform API calls and native handles (camera devices, sessions,
    readers)
-   Platform threads/loopers required to operate those APIs
-   Frame acquisition, timestamp extraction, and delivery
-   Mapping platform pixel formats into CamBANG FourCC formats
-   Reporting creation/destruction of CamBANG-owned native objects to
    core
-   Platform-specific error detection and reporting to core

------------------------------------------------------------------------

## 3. Interface contract

The provider interface is internal to core (not public API). The exact
C++ signatures are an implementation detail, but the semantic contract
is fixed.

### 3.1 Required capabilities (semantic)

-   Enumerate camera endpoints
-   Open/close a camera endpoint into a runtime instance
-   Create/destroy a repeating stream for a device instance
    (`StreamIntent`)
-   Start/stop a repeating stream
-   Trigger a still capture for a device instance (or for
    rig-coordinated capture)
-   Apply spec changes (via core-validated patch application)
-   Shutdown provider cleanly

### 3.2 "Core validates, provider executes"

Core must validate and normalize profiles/specs before calling provider.

Provider may still reject a request **only** when: - The platform cannot
support the validated request (hard constraint) - The platform reports
transient failure that prevents execution

Provider must return deterministic, explicit error codes for such
rejections.

------------------------------------------------------------------------

## 4. Determinism and threading

Determinism is a non-negotiable requirement.

### 4.1 Provider-to-core callback thread rule

Provider must invoke callbacks into core using a **single serialized
callback context**:

-   One dedicated callback thread, **or**
-   One event queue consumed by core, **or**
-   One "provider event pump" thread feeding core

Provider must not call core concurrently from multiple threads.

### 4.2 Ordering

Provider must preserve ordering of events as they occur within
provider: - stream start → frames → stream stop - capture trigger →
capture frames → capture completion/error - native object create → later
destroy

### 4.3 No indefinite blocking

Provider must not block indefinitely in any call path required for
teardown or shutdown.

### 4.4 Core call serialization

Core will call provider methods in a deterministic order and will not
concurrently invoke mutually exclusive lifecycle operations for the same
instance unless explicitly documented.

------------------------------------------------------------------------

## 5. Native object reporting

Every native object created by CamBANG (including platform-native
handles, reader objects, buffer pools, etc.) must be reported to core
for snapshot introspection.

### 5.1 Registration

On creation, provider must report:

-   `type` (CamBANG-defined numeric enum)
-   ownership references (rig/device/stream ids where applicable)
-   `root_id` lineage identifier
-   timestamps (monotonic) where possible

### 5.2 Destruction

On destruction, provider must report:

-   `native_id`
-   destruction timestamp (monotonic)

Provider must never "forget" to report destruction; missing destroys are
treated as leaks in diagnostics.

------------------------------------------------------------------------

## 6. Pixel format mapping (FourCC)

CamBANG uses a **canonical FourCC-style** `uint32` pixel format code
set.

Provider must map platform-native formats into this set.

Requirements: - Deterministic mapping - Stable across runtime -
Rejection of unsupported formats at creation time

### 6.1 v1 policy

-   Repeating streams (`PREVIEW`, `VIEWFINDER`) are **raw-only**
    formats.
-   Still capture profiles may include `'JPEG'` and `'RAW '` where
    platform support exists.

Provider must not silently substitute compressed formats for streams.

------------------------------------------------------------------------

## 7. Capture synchronisation (multi-camera)

On platforms that support it (Android camera2 being the primary target),
providers must support synchronised multi-camera capture where hardware
permits.

Provider must: - Use platform mechanisms for synchronized capture where
available - Extract and report per-frame timestamps as delivered by the
platform - Deliver frames back to core tagged with `capture_id`,
`device_instance_id`, and timestamps

Provider does not decide whether synchronization is requested; core
does.

------------------------------------------------------------------------

## 8. Error reporting

Provider must report errors via deterministic callbacks (exact names are
implementation detail), scoped to:

-   Device instance (`instance_id`)
-   Stream (`stream_id`)
-   Capture (`capture_id`)

Error reporting requirements: - Deterministic ordering - Non-spammy
(avoid repeated identical error floods) - Idempotent where feasible
(core may de-duplicate, but provider should not explode noise)

Core increments counters and publishes snapshots.

------------------------------------------------------------------------

## 9. Warm policy interaction

Provider does not decide warm timing.

Core tracks `warm_hold_ms` and schedules teardown.

Provider must support: - Clean stop of a stream - Clean teardown while
frames may still be in flight - Reopen after close (new `instance_id`
lineage)

Provider must not retain resources past a core-directed teardown.

------------------------------------------------------------------------

## 10. Preemption

Provider does not arbitrate.

Core may instruct provider to: - Stop a repeating stream (e.g., preempt
`VIEWFINDER` for capture) - Abort a capture if supported (optional;
platform-dependent) - Reconfigure pipelines when policy allows

Provider must obey in deterministic order and report resulting lifecycle
transitions.

------------------------------------------------------------------------

## 11. Provider implementations (v1 targets)

-   `AndroidCamera2Provider`
-   `SyntheticProvider` (test harness / deterministic simulation)
-   `StubProvider` (not implemented platforms; returns deterministic
    "not supported" responses)

All providers must satisfy this contract, even if they provide only "not
supported" behaviour.

------------------------------------------------------------------------

## 12. Compliance checklist

A provider is considered compliant when:

-   All core callbacks occur on a single serialized callback context
-   All created native objects are reported and destroyed objects are
    reported
-   Stream formats are mapped into canonical FourCC and streams are
    raw-only
-   Teardown/shutdown completes without indefinite blocking
-   Errors are reported deterministically and scoped correctly
-   Synchronised capture support (if implemented) reports timestamps per
    frame
