# Provider Compliance Checklist

**Status:** Current dev maintainer checklist.

This checklist helps maintainers audit whether a provider
implementation conforms to the CamBANG provider contract.

Canonical provider/runtime authority lives in:
- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`
- relevant `docs/architecture/` supplements

Maintainer tool and validation layering guidance lives in:
- `docs/dev/maintainer_tools.md`

This checklist is subordinate to canonical architecture and current
verifier source behavior. It must not be treated as an independent
provider contract.

It is intentionally provider-agnostic: a minimal backend adapter and a
release-quality backend adapter must both satisfy the same contract,
even if their supported features differ.

---

## 1. Contract source of truth

Before auditing a provider, confirm that review criteria come from:

- canonical architecture docs
- reference providers (`SyntheticProvider`, `StubProvider`)
- compliance verifier

Do not treat any single platform-backed provider as the definition of
provider behaviour.

---

## 2. Threading and callback discipline

- All provider → Core facts are delivered through the provider strand.
- Platform callbacks never invoke Core callbacks directly.
- Worker threads post work to the strand rather than calling Core
  services directly.
- No bypass path exists for lifecycle, native-object, or error events.

### Prompt-submission / non-reentrant provider calls

Audit provider API calls that may run while `CoreRuntime` / `ProviderBroker`
callers are synchronously blocked. Confirm that:

- provider API methods are prompt and bounded submission / control operations
- `trigger_capture(...)` / grouped capture submission success means the provider
  accepted responsibility for later terminal success or failure reporting; it does
  not mean image payloads have already been acquired, generated, retained, or
  made Godot-visible
- `stop_*`, `destroy_*`, `close_*`, and `shutdown()` are not long backend drains
  on the calling thread
- provider methods called under `ProviderBroker` serialization do not re-enter
  the broker
- provider callbacks are posted through the strand and do not synchronously wait
  on core-thread, Godot-thread, or render-thread work
- capability and template queries do not perform backend I/O
- any bounded waits are dev-provider-only or explicitly justified

Blocking stop / shutdown in a provisional adapter is retirement /
deprioritised debt. Future release providers must not treat that
implementation shape as a template.

---

## 3. Event-class guarantees

Lifecycle, native-object, and error events are **non-lossy**.

Frame events may be dropped under pressure.

Dropping frames must not cause lifecycle, native-object, or error events
to be lost.

---

## 4. StreamTemplate, CaptureTemplate, and defaulting boundary

- The provider exposes deterministic `StreamTemplate` and `CaptureTemplate`
  values.
- Provider-type stream and still-capture defaults live in those templates.
- Core materializes effective stream and still-capture config from requests,
  provider templates, and retained profile state.
- The provider executes the effective config it is given.
- The provider does not silently invent width / height / format /
  picture defaults during `create_stream()`, `start_stream()`, or
  `trigger_capture()`.
- If the effective config reaching the provider is incomplete or
  invalid, the provider fails deterministically rather than repairing it
  silently.

---

## 5. Lifecycle truthfulness

### Normal operation

- `close_device()` fails if child streams still exist.
- `destroy_stream()` fails if the stream is still started / producing.
- The provider does not auto-stop or auto-destroy children merely to
  satisfy the caller.

### Shutdown

- Provider shutdown follows ordered teardown.
- Shutdown cleanup is truthful and corresponds to actual release.
- Shutdown does not fabricate destruction events for resources never
  acquired or actually released.

---

## 6. Native-object reporting

Providers emit truthful registry events for implemented native-object
classes, typically including:

- `Provider`
- `Device`
- `AcquisitionSession` (when concretely realized)
- `Stream`
- stream/capture support resources whose lifetime/release matters (for example
  frame buffer leases, GPU backings, retained samples, mapped buffers)

Audit that:

- creation occurs when the resource is actually acquired
- destruction occurs when the resource is actually released
- no destruction is fabricated just to tidy state
- ownership relationships remain visible rather than being hidden by
  provider-side auto-cascade logic

Current implementation reminder:

- `SyntheticProvider` now realizes concrete `AcquisitionSession` truth for both
  stream-backed and capture-only paths.
- Capture-only truth may realize acquisition-session-owned support resources
  without fabricating a public stream.
- Still capture callbacks alone do not satisfy retained AcquisitionSession truth
  obligations when no concrete session seam has been realized.

---

## 7. Timestamp correctness

All frames delivered to Core include a contract-valid
`CaptureTimestamp` containing:

- `value`
- `tick_ns`
- `domain`

Audit that:

- `tick_ns` is non-zero
- `domain` is semantically valid
- timestamp values are not placeholder-shaped
- synthetic / stub providers are held to the same timestamp contract as
  platform-backed providers

---

## 8. Platform adaptation discipline

For platform-backed providers:

- backend quirks remain contained within the provider adapter
- Core semantics are not changed to match one backend API
- provisional / minimal providers do not become accidental sources of
  truth for later providers

---

## 9. Camera-fact callback discipline

- Static camera facts are keyed by the opened provider device identity.
- Per-image facts are posted through the normal provider callback path with the
  exact capture ID, device instance ID, and image-member index.
- Per-image fact replacement is complete and transactional: malformed input
  must not mutate retained truth, and omitted facts remain absent.
- Image acquisition timing uses the provider-domain mark associated with that
  exact member. It must not be substituted from Capture Date-Time, admission,
  Core lifecycle, geolocation, or another member.
- Focus state and realized image transform are per-image facts, distinct from
  sensor orientation and static pose. Providers omit facts they cannot supply
  truthfully.

---

## 10. Verification expectations

Providers should be validated using the maintainer tools and verifier.

Validation should establish confidence in:

- lifecycle ordering correctness
- absence of dropped non-frame events
- truthful native-object reporting
- contract-valid timestamps
- deterministic shutdown behaviour
- consistency with registry / snapshot truthfulness rules

Where a provider is hardware-backed and environment-sensitive, runtime
validation may supplement the verifier, but should not replace contract
checks.

For synthetic timeline destructive sequencing specifically, maintainers should
distinguish between:

- **provider contract verification**
  - primarily covered by `provider_compliance_verify`, including strict vs
    completion-gated clustered destructive cases

- **default-path canonical realization**
  - covered by `canonical_timeline_realization`, which serves as a simple
    always-pass completion-gated realization proof rather than a strict-edge
    diagnostic
