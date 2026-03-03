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
-   Multi-camera synchronised capture where the platform supports
-   Testability via a synthetic provider
    (synthetic providers may generate pixel content via provider-agnostic rendering modules)

Providers must not:

-   Expose platform semantics directly to Godot-facing objects
-   Own or publish `CamBANGStateSnapshot`
-   Implement arbitration/policy decisions (rig priority, preemption
    rules)
-   Retain references to core objects beyond their documented lifetime

## 1.x Synthetic pixel generation (Pattern Module)

Providers may produce frames from platform camera APIs or from synthetic
sources.

Synthetic providers may generate pixel content using provider-agnostic
rendering modules (e.g., the Pattern Module) while preserving the
provider/core contract:

- Core remains agnostic to pixel origin.
- Providers remain responsible for deterministic delivery and correct
  pixel format mapping.
- Synthetic rendering does not alter arbitration, policy decisions, or
  snapshot publication.
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

Provider may still reject a request **only** when:

- The platform cannot support the validated request (hard constraint)
- The platform reports transient failure that prevents execution

Provider must return deterministic, explicit error codes for such
rejections.

------------------------------------------------------------------------

## 3.x Stream configuration inputs (CaptureProfile and PictureConfig)

Stream configuration crossing the core ↔ provider boundary is separated
into two orthogonal inputs supplied per stream:

- `CaptureProfile` — structural capture properties (resolution, pixel
  format, frame rate range).
- `PictureConfig` — picture appearance parameters.

Core owns stream state. Core validates and normalizes stream requests
and supplies effective values to the provider during stream
create/start.

Providers execute validated requests. Providers must not introduce
additional implicit picture defaults beyond those supplied by Core.

`PictureConfig` is stream-scoped. Release architecture must not rely on
provider-wide mutable picture state affecting all streams.

------------------------------------------------------------------------

### 3.x.1 Provider default stream template (`StreamTemplate`)

Each provider implementation supplies a deterministic `StreamTemplate`
used as the fallback when stream requests omit explicit fields.

A `StreamTemplate` contains:

- `CaptureProfile profile`
- `PictureConfig picture`

Defaulting is performed by Core:

- If a stream request does not specify a `CaptureProfile`, Core uses
  `StreamTemplate.profile`.
- If a stream request does not specify a `PictureConfig`, Core uses
  `StreamTemplate.picture`.

Defaults apply at stream creation time only. They do not imply a
provider-global override mechanism.

------------------------------------------------------------------------

### 3.x.2 Synthetic pixel generation (Pattern Module integration)

Synthetic and stub providers may generate pixel content using
provider-agnostic rendering modules such as the Pattern Module.

For pattern-backed streams:

- `PictureConfig` selects the pattern preset and its parameters.
- Providers convert the effective per-stream `PictureConfig` into the
  renderer-facing `PatternSpec` and render into caller-owned buffers.

The Pattern Module preset registry remains the single authority for
preset vocabulary and stable string tokens.
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

Every native object created by the provider on behalf of CamBANG
(including platform-native handles, reader objects, buffer pools, etc.)
must be reported to core for snapshot introspection.

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
providers must support synchronised multi-camera capture where the platform
permits.
Provider must: - Use platform mechanisms for synchronized capture where
available - Extract and report per-frame timestamps as delivered by the
platform - Deliver frames back to core tagged with `capture_id`,
`device_instance_id`, and timestamps

Provider does not decide whether synchronization is requested; core
does.

------------------------------------------------------------------------

## 7.x Capture timestamp domains (provider → core)

Providers must tag delivered frames with a **provider-agnostic capture timestamp** for
multi-camera alignment and tolerance checks.

Core must not depend on platform-specific timestamp concepts (e.g. Media Foundation sample
time, Android timestamp source enums). Such concepts must remain provider-internal.

Provider must supply capture time using a generic representation:

- `value`: integer tick value
- `tick_ns`: tick period in nanoseconds (e.g. 1 for ns, 100 for 100ns)
- `domain`: declared comparability/meaning of the timestamp

Domains are semantic, not platform names. v1 domains:

- `PROVIDER_MONOTONIC`: monotonic and comparable across streams produced by this provider instance.
- `CORE_MONOTONIC`: already mapped into core’s monotonic timebase (session-relative).
- `DOMAIN_OPAQUE`: ordering-only; provider cannot guarantee meaningful cross-stream comparability.

Providers should prefer `PROVIDER_MONOTONIC` or `CORE_MONOTONIC` where feasible.

Core uses capture timestamps only according to the declared domain.

Avoid using enum identifiers that collide with Windows macros (e.g., OPAQUE). Prefer prefixed forms (e.g., DOMAIN_OPAQUE) in provider-contract headers.
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
-   `WindowsMediaFoundationProvider`
-   `SyntheticProvider` (test harness / deterministic simulation)
-   `StubProvider` (not implemented platforms; returns deterministic
    "not supported" responses)

All providers must satisfy this contract, even if they provide only "not
supported" behaviour.

Note:

- `StubProvider` is a minimal deterministic provider used for
  lifecycle validation and smoke testing.

- `SyntheticProvider` is intended for richer deterministic simulation
  (e.g., multi-camera rigs, timestamp modelling) and may supersede
  StubProvider in later development phases.

### 11.1 WindowsMediaFoundationProvider (Windows / Media Foundation)

The Windows provider is implemented using Media Foundation (MF).

It exists primarily to:

- Validate lifecycle determinism on Windows.
- Exercise provider ↔ core contract under real camera load.
- Provide a development accelerator for Godot visibility work.

#### Determinism and shutdown

The MF provider must:

- Avoid indefinite blocking during `ReadSample` loops.
- Provide a deterministic unblock mechanism during stop/shutdown
  (e.g. flush or reader teardown).
- Preserve serialized callback semantics into core.

Validated behaviour during development:

- Continuous frame delivery (including drop-heavy cases) does not break
  release-on-drop discipline.
- Rapid start/stop cycles complete without hang.
- No double-release or native object leakage observed under stress.

#### Native format constraints (visibility phase)

When `MF_READWRITE_DISABLE_CONVERTERS = TRUE` is set:

- Only native camera output types are selectable.
- Many consumer cameras expose YUV-only formats (e.g. `NV12`, `YUY2`,
  `MJPG`) natively.
- If no RGB32-like subtype (`MFVideoFormat_RGB32`,
  `MFVideoFormat_ARGB32`, etc.) is advertised, visible pixels cannot be
  produced without enabling conversion.

CamBANG intentionally does **not** introduce CPU YUV→RGB conversion into
core or the development mailbox path.

Policy during v1 visibility phase:

- Mailbox accepts tightly packed RGBA8 only.
- A dev-only BGRA→RGBA channel swizzle is permitted.
- Unsupported formats are dropped and released deterministically.

This separation ensures lifecycle and ownership correctness can be
validated independently of colour conversion strategy.

#### MinGW toolchain notes

When building under MinGW, the link set must include:

- `mf`
- `mfplat`
- `mfreadwrite`
- `mfuuid`
- `ole32`
- `uuid`

`mf` is required for `MFEnumDeviceSources`.
`uuid` is required for `GUID_NULL` resolution.

Prefer MF helper functions (e.g. `MFGetAttributeSize`,
`MFGetAttributeUINT32`) over relying on inline convenience methods
(e.g. `GetINT32`), as MinGW headers may not expose all helpers
consistently.

## 11.x Synthetic provider timing model

SyntheticProvider is a first-class implementation of `ICameraProvider` that simulates platform camera behaviour while preserving the provider ↔ core contract defined in this document.

SyntheticProvider introduces three orthogonal configuration axes:

- `provider_mode`
- `synthetic_role`
- `timing_driver`

These axes are naming- and behaviour-stable parts of the architecture. They define capture origin and timing semantics without altering the provider/core boundary.

---

### 11.x.1 Provider mode (capture origin)

`provider_mode` selects the active backend bound to Core:

- `platform_backed` — frames originate from a real platform camera API.
- `synthetic` — frames originate from SyntheticProvider.

Exactly one provider instance is bound to Core at runtime. Core does not arbitrate between multiple providers.

When `provider_mode = synthetic`, the following two axes apply.

---

### 11.x.2 Synthetic role (semantic behaviour)

`synthetic_role` defines *what kind of behaviour* the synthetic provider emits:

- `nominal` — steady, happy-path provider semantics (no implicit jitter, drops, or errors).
- `timeline` — replay of an explicit external scenario (e.g., a `.cambang_scenario.json`), including jitter, drops, errors, and lifecycle events exactly as encoded.

Role affects *behavioural semantics*, not timing mechanics. Role does not alter the provider/core contract.

---

### 11.x.3 Timing driver (time advancement semantics)

`timing_driver` defines *how time advances* for SyntheticProvider:

- `virtual_time` — time advances only via explicit host calls (e.g., `advance(dt_ns)` or equivalent tick mechanism). No sleeping. No reliance on wall-clock.
- `real_time` — time advances using a monotonic clock. Provider schedules events relative to wall-clock progression.

Timing driver affects scheduling and cadence, but not ordering guarantees or callback serialization rules.

---

### 11.x.4 Determinism guarantees (normative)

The following guarantees apply to SyntheticProvider.

#### Contract compliance

- SyntheticProvider must fully satisfy the `ICameraProvider` contract.
- It must not reach into Core internals.
- It must not bypass arbitration.
- It must report native object creation/destruction exactly as a platform provider would.
- It must use provider-agnostic capture timestamp representation (see §7.x).

#### Callback serialization

In all modes and roles:

- All provider → core callbacks occur on a single serialized callback context.
- SyntheticProvider must not call Core concurrently from multiple threads.

#### Virtual-time determinism

When `timing_driver = virtual_time`:

- Event ordering and timing are strictly deterministic given:
    - identical configuration
    - identical role inputs (e.g., identical scenario file for `timeline`)
    - identical sequence of host-driven time advancement
- No internal sleeping or wall-clock access may influence scheduling.
- Frame timestamps and lifecycle events are derived solely from synthetic scheduling state.

This mode is suitable for CI, replay, and invariant validation.

#### Real-time determinism

When `timing_driver = real_time`:

- Event ordering and content must remain deterministic for a given configuration and input.
- Precise inter-arrival timing is best-effort and subject to host scheduling and clock resolution.
- Real-time mode must not introduce non-deterministic reordering of events.

Real-time mode is intended to approximate live cadence while preserving contract correctness.

---

### 11.x.5 Host-driven time advancement

Core does not drive provider time.

When `timing_driver = virtual_time`, time advancement must be performed by the host environment.

In GDE builds, `CamBANGServer` is responsible for advancing synthetic time (e.g., from Godot frame ticks).

The ProviderBroker:

- selects the backend,
- forwards calls,
- does not own scheduling policy,
- does not arbitrate cadence.

SyntheticProvider must remain clock-driven (scheduler-based), not “Godot-driven.” The host provides time advancement; the provider executes scheduled work accordingly.

---

### 11.x.6 Implementation status (non-normative)

The architectural model above defines the intended behaviour space of SyntheticProvider.

As of the current repository state:

- Implemented:
    - `synthetic_role = nominal`
    - `timing_driver = virtual_time`
- Declared but not yet implemented:
    - `synthetic_role = timeline`
    - `timing_driver = real_time`

Enum definitions and configuration surfaces may exist for these declared modes, but only the implemented combination is currently active in provider logic.

This section defines the target specification for future implementation work and must not be interpreted as a claim that all combinations are presently available.
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

### 12.x Validation layering

Provider compliance is validated in two stages:

1) **Core smoke (stub provider)**  
   Validates lifecycle determinism, release semantics, and shutdown invariants
   independently of platform APIs.

2) **Platform provider runtime validation**  
   Validates real API behaviour (e.g., Windows MF, Android camera2)
   under platform-backed asynchronous load.
Core smoke must remain provider-independent.
Platform validation must not redefine core invariants.