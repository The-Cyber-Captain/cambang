# Windows MF Visibility Phase (Build Phase – Dev Accelerator)

Branch: `build-phase-windows-provider-min-1`

This document captures development-phase findings during implementation of
the Windows Media Foundation (MF) provider as a dev accelerator for Godot
visibility work.

It is intentionally separate from architecture and state snapshot documents.
It records implementation validation and platform-specific realities,
not architectural invariants.

---

## 1. Objective of This Phase

The goal of this phase was:

- Open the default Windows camera via Media Foundation.
- Start streaming frames.
- Deliver frames into the CamBANG provider → core → sink pipeline.
- Preserve strict release-on-drop semantics.
- Preserve deterministic shutdown behaviour.
- Avoid introducing CPU colour conversion.
- Achieve visible pixels in Godot *if native RGB output exists*.

Constraints:

- No YUV→RGB CPU conversion.
- No hidden MF colour converters.
- No new Godot-facing APIs.
- No global state.
- Keep provider minimal and auditable.


## Runtime Ownership Clarification

In the current architecture, `CoreRuntime` is owned by
`CamBANGServer` (Engine singleton).

Dev nodes do not own the runtime. Visibility scaffolding interacts
with the server but does not bypass canonical runtime ownership.

This aligns Windows provider validation with the canonical runtime model.

---

## 2. Verified Behaviour

The following behaviours were validated under stress:

### 2.1 Stream Delivery

- Camera opens successfully.
- Media Foundation stream starts.
- Frames are delivered continuously to the provider.
- Frames are forwarded into core via the normal ingress path.

### 2.2 Drop-Heavy Operation

On typical consumer cameras (with MF converters disabled):

- `frames_received` increases steadily.
- `frames_dropped_unsupported == frames_received`.
- `accepted_rgba == 0`.
- `accepted_bgra_swizzled == 0`.

(These counters are provider-level diagnostics and do not redefine
the canonical snapshot schema.)

This indicates:

- Native camera formats were not RGBA-class.
- All frames were correctly dropped as unsupported.
- No conversion was silently introduced.


### 2.3 Release Semantics Under Load

Under sustained drop-heavy operation:

- No memory leaks observed.
- No double-release observed.
- No native buffer retention.
- Dispatcher release-on-drop discipline preserved.

### 2.4 Shutdown / Teardown

Under repeated rapid start/stop cycles:

- No `ReadSample` hang.
- Deterministic teardown.
- Worker thread exits cleanly.
- No deadlocks observed.
- No shutdown-order regressions.

This validates that provider lifecycle choreography remains correct even
when every frame is dropped.

This satisfies the provider contract requirement that teardown
must not block indefinitely.

---

## 3. Native Format Reality (Media Foundation)

With `MF_READWRITE_DISABLE_CONVERTERS = TRUE`:

- Only native camera output types are selectable.
- Many consumer cameras expose only YUV formats:
    - NV12
    - YUY2
    - MJPG
    - other YUV-class types

If no RGB32-like subtype (`MFVideoFormat_RGB32`,
`MFVideoFormat_ARGB32`, etc.) is advertised natively,
visible pixels cannot be produced without enabling conversion.

This behaviour is expected and does not indicate failure of the provider.

---

## 4. Dev Sink Policy (Mailbox)

During this phase:

- Mailbox accepts tightly packed RGBA8.
- Mailbox accepts tightly packed BGRA8.
- BGRA frames are swizzled to RGBA before storage.
- No YUV conversion is performed.
- Unsupported formats are dropped deterministically.

The BGRA swizzle is a byte-order normalization, not a colourspace conversion.

This concession exists solely to enable visibility when 32-bit RGB-class
formats are delivered in BGRA memory order (common on Windows).

---

## 5. MinGW Toolchain Notes

When building MF under MinGW, the following link set is required:

- `mf`
- `mfplat`
- `mfreadwrite`
- `mfuuid`
- `ole32`
- `uuid`

Additional observations:

- Prefer `MFGetAttribute*` helpers over inline `GetINT32` accessors.
- Use `MFVideoFormat_RGB32` (not `RGBA32`).
- Some headers differ from MSVC SDK behaviour.

These constraints are toolchain-specific and do not affect architecture.

### 5.x Windows Macro Collision: OPAQUE

Windows headers included via `windows.h` (commonly `wingdi.h`)
define a macro named `OPAQUE` (typically value `2`).

If CamBANG shared/provider headers define an enum member named
`OPAQUE`, and that header is included after `windows.h`,
compilation fails with errors such as:

    expected identifier before numeric constant

Policy:

- Avoid Windows-macro-prone identifiers (`OPAQUE`, `ERROR`, `DELETE`,
  `IN`, `OUT`, etc.) as unqualified enum members in shared headers.
- Prefer prefixed identifiers such as `DOMAIN_OPAQUE`.

CamBANG v1 uses `DOMAIN_OPAQUE` for capture timestamp domains to
avoid this collision.

---

## 6. What This Phase Proves

This phase proves:

- Provider ↔ core contract holds under real Windows camera load.
- Deterministic lifecycle semantics are preserved.
- Drop-heavy operation is safe and leak-free.
- Shutdown choreography remains robust.
- Format negotiation can be isolated from lifecycle validation.

It does *not* prove:

- Visible pixels are guaranteed on all Windows devices.
- Native RGB32 output exists on all cameras.
- MF conversion paths are acceptable in production.

---

## 7. Future Work (Beyond This Phase)

Possible next steps:

- Enumerate and negotiate native RGB32-like subtypes.
- Log and confirm actual MF subtype delivered.
- Explore GPU-based conversion paths (future production path).
- Explicitly document production visibility strategy.

No CPU YUV conversion is to be added without architectural review.

---

End of visibility phase record.