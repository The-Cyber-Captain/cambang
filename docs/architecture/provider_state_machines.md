# Provider State Machines (Reference)

This document restores the **precise state-machine references** that complement
`architecture/lifecycle_model.md`.

It is intentionally compact and specification-like.
It defines **transition correctness**, not narrative explanations.

Canonical lifecycle phase values remain:

- `CREATED`
- `LIVE`
- `TEARING_DOWN`
- `DESTROYED`

Operational states such as `BOUND`, `OPEN`, or `PRODUCING` are **separate axes**
that describe resource-specific runtime conditions.

---

# 1. Provider lifecycle

Provider instances follow this lifecycle:

CREATED
   │ attach to core
   ▼
LIVE
   │ shutdown requested
   ▼
TEARING_DOWN
   │ resources released
   ▼
DESTROYED

Rules:

- Provider lifecycle events must be reported truthfully.
- Provider destruction must occur **after all devices and streams are released**.
- Providers must not emit fabricated destroy events to tidy state.

---

# 2. Device lifecycle

Device instances represent an opened hardware or virtual camera.

CREATED
   │ open device
   ▼
LIVE
   │ close requested
   ▼
TEARING_DOWN
   │ hardware handle released
   ▼
DESTROYED

Constraints:

- Streams require a `LIVE` device.
- Devices should not close while streams exist.
- Providers should surface ordering violations rather than hide them.

---

# 3. Stream lifecycle

Streams represent capture pipelines.

CREATED
   │ stream configured
   ▼
LIVE
   │ stop / destroy requested
   ▼
TEARING_DOWN
   │ pipeline resources released
   ▼
DESTROYED

Important distinction:

Stream existence ≠ frame production

Frame production is controlled by the **FrameProducer** state.

---

# 4. FrameProducer state machine

FrameProducer describes **active frame generation** for a stream.

IDLE ── enable ──> PRODUCING
  ▲                   │
  └──── disable ──────┘

Examples:

| Platform | Meaning |
|---|---|
| Media Foundation | active ReadSample loop |
| Camera2 | repeating capture request |
| V4L2 | STREAMON |
| Synthetic | pattern renderer active |

FrameProducer events may occur many times within a single stream lifecycle.

---

# 5. Provider strand delivery rules

All provider facts must be delivered through the **provider strand**.

Platform callback / worker
      │
      ▼
post to provider strand
      │
      ▼
strand invokes core callback

Rules:

1. Only the provider strand may deliver provider facts.
2. Lifecycle, native-object, and error events are **non-droppable**.
3. Frame events **may be dropped** under pressure.
4. Synthetic and stub providers must obey the same hierarchy and strand rules.

---

# 6. Axes summary

| Axis | Purpose |
|---|---|
| Lifecycle phase | registry truth (`CREATED` → `DESTROYED`) |
| Operational state | provider/device/stream runtime status |
| FrameProducer state | frame generation enablement |

These axes must not be collapsed into a single state machine.


## Operational State Reference

The following state machines define the runtime behavior of providers
and their resources.

### Provider Binding State

ABSENT
Provider is not bound to the device.

BOUND
Provider owns the device and may create streams.

Transitions
ABSENT → BOUND     provider binds device
BOUND  → ABSENT    provider releases device


### Device Open State

CLOSED
Device handle not active.

OPEN
Device handle active and capable of streaming.

Transitions
CLOSED → OPEN      device opened
OPEN   → CLOSED    device closed


### Stream Presence State

ABSENT
No stream exists.

PRESENT
Stream object exists but may not yet be producing.


### Frame Production State

IDLE
Stream exists but no frames are being produced.

PRODUCING
Frames are actively being emitted.