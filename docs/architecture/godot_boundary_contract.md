# Godot Boundary Contract

This document defines the **canonical observable contract** between the CamBANG runtime and Godot consumers.

It consolidates the externally visible behaviour already defined across the architecture documents into one place for maintainers and integrators.

This document describes **observable behaviour only**. It intentionally does **not** describe internal provider lifecycle phases or dev scaffolding implementation details.

---

# Canonical Observable Snapshot / Publication Surface

This document specifies the stable observable snapshot/publication lifecycle surface:

```
CamBANGServer.start()
CamBANGServer.stop()
CamBANGServer.get_state_snapshot()

signal state_published(gen, version, topology_version)
```

Other Godot-facing APIs may exist for object access, scenario/dev control, provider configuration, or result retrieval. They are outside the scope of this document unless explicitly tied to snapshot visibility, generation boundaries, or tick-bounded publication semantics. New Godot-facing API surface must still be added deliberately and documented at the appropriate boundary; this clarification must not be read as permission for ad-hoc public API growth.

Stopped-time imaging-subsystem truth ingestion is a narrow public-boundary
exception:

```
CamBANGServer.ingest_camera_description(String json_text) -> Error
```

This method accepts caller-supplied JSON text only; CamBANG performs no
filesystem access on the caller's behalf. It accepts the complete supported ADC
v2 camera-description document, including optional concurrency truth projected
into retained `ImagingSpec`. Ingestion is stopped-time and transactional:
accepted input becomes configured truth for the next generation,
active-generation ingestion returns `ERR_BUSY`, and any parse/validation
failure leaves the previously configured truth unchanged. Legacy ADC v1
concurrency-only documents are not accepted through this public surface.

Completed `CamBANGCaptureResult` objects expose resolved still-camera facts
through optional `get_image_member(index).camera_facts`, and
`CamBANGStreamResult` exposes stream-frame facts through `get_camera_facts()`.
These surfaces include optional `acquisition_timing` with direct Godot `int`
values for `acquisition_mark`, `tick_period_numerator_ns`, and
`tick_period_denominator`, plus `get_capture_datetime_unix_nanoseconds()`,
`has_geolocation()`, and `get_geolocation()`. This does not add a camera-fact
wrapper class or any legacy scalar capture-timestamp alias.

Capture geolocation is configured independently through:

```
CamBANGServer.set_capture_geolocation(Dictionary geolocation) -> Error
```

It accepts an empty dictionary to clear the configured value. A non-empty
dictionary requires finite WGS 84 geodetic `latitude_degrees` in `[-90, 90]`
and `longitude_degrees` in `[-180, 180]`, with optional finite WGS 84
ellipsoidal `altitude_meters` in metres. Invalid input is rejected
transactionally without changing the prior value. The setter is valid while
stopped or running and changes only the value sampled by future successful
capture admissions; existing admitted captures and completed results remain
unchanged.

Boundary lifecycle note: Godot/CamBANGServer may own provider storage and release
that storage after a run, but once the provider is attached and the runtime is
running, attached-provider shutdown belongs to `CoreRuntime::stop()`. Godot must
not directly call `provider->shutdown()` while the provider is live/attached.

---

# Runtime Start Behaviour

`CamBANGServer.start()` reports that startup was accepted; it is not a synchronous readiness barrier for runtime commands.

After:

```
CamBANGServer.start()
```

the runtime may enter a short **pre‑baseline window** where:

```
get_state_snapshot() == NIL
```

No runtime state should be assumed during this period. Public runtime-effect surfaces are not commandable until the generation baseline is observed at the Godot boundary. During the pre-baseline window, mutating runtime commands fail visibly (for example `ERR_BUSY` or `ERR_UNAVAILABLE`, according to the method convention), runtime object lookups such as `get_device(instance_id)` return no object, result lookups return no result, and runtime-effect commands are neither queued for later nor reported as successful while dropped.

Provider discovery is a narrow startup-safe exception because it observes
provider endpoint identity rather than mutating runtime/core state. After
`start()` has been accepted and provider storage exists, `enumerate_devices()`
and `get_device_for_hardware_id(...)` may be used before the first baseline
publish. A returned `CamBANGDevice` can be a **hardware-id endpoint handle**: an
object representing the endpoint selected by hardware ID. That handle does not
necessarily imply that a live runtime **device-instance object** has been
resolved, or that a current `device_instance_id` exists. These identities are
not interchangeable for all operations. Runtime operations that require a live
resolved device instance, including device-triggered capture, depend on the
runtime-instance seam rather than endpoint-handle identity alone;
`get_device(instance_id)` therefore remains baseline/current-generation gated.

Endpoint startup intent is also narrow and hardware-id scoped. During the pre-baseline startup window, endpoint-handle `engage()`, `set_still_capture_profile(...)`, and `set_warm_policy(...)` may be accepted as startup intent. Those calls do not execute core/provider runtime effects before baseline: the clean `state_published(gen, 0, 0)` snapshot is emitted and latch-visible first, and accepted endpoint intents are applied only afterward, so their observable device/profile/warm-policy effects appear in later snapshots (normally `version >= 1`). Multiple pre-baseline profile or warm-policy calls for the same endpoint/session are deterministic last-write-wins. Accepted endpoint startup intent either applies after baseline or fails visibly; it is not retained indefinitely. Stream creation, capture triggering, result lookup, rig capture, timeline advancement, and commands that depend on runtime instance lineage are not part of this exception.

Synthetic timeline scenario staging is also a narrow exception because it is startup configuration intent, not a runtime-effect command: when synthetic timeline mode is active and provider storage exists, `select_builtin_scenario(...)` and `load_external_scenario(...)` may stage provider-owned scenario data during the pre-baseline window. If a valid scenario has been staged, `start_scenario()` may also be accepted during this window as pending playback intent. Actual scenario playback is not started until after the baseline `state_published(gen, 0, 0)` has been emitted and is latch-visible, so scenario effects must appear only after baseline (normally at `version >= 1`). These exceptions are not a general pre-baseline command queue; stream, rig, capture, result lookup, instance-id lookup, non-startup endpoint operations, and `advance_timeline(...)` runtime effects remain baseline-gated.

The first observable publish of a generation will always be:

```
(gen = N, version = 0, topology_version = 0)
```

This publish occurs on the **first eligible Godot tick** after runtime initialization.

Consumers may treat this snapshot as the **baseline snapshot** for that generation. Consumers that need runtime-effect commands should wait for the first `state_published(gen, 0, 0)` after `start()`; inside that baseline signal handler, `get_state_snapshot()` is non-`NIL` and command admission for the active generation is established. This contract does not introduce a public `is_ready()` or `is_command_ready()` getter.

---

# Stop Behaviour

After a completed stop:

```
CamBANGServer.stop()
```

the observable boundary returns to:

```
get_state_snapshot() == NIL
```

No snapshot from the previous generation should remain visible after stop.

---

# Restart Behaviour

When the runtime restarts:

```
CamBANGServer.start()
```

the following guarantees apply:

1. No snapshot from the previous generation appears during the pre‑baseline window.
2. The next observable snapshot will be:

```
(gen = previous_gen + 1,
 version = 0,
 topology_version = 0)
```

This behaviour ensures clean generation boundaries across restarts.

---

# Tick‑Bounded Publication

The runtime may produce multiple internal updates between Godot frames.

However Godot observers will see:

```
≤ 1 state_published(...) emission per Godot tick
```

If multiple internal updates occur between ticks, they are **coalesced** and only the latest observable state is published.

This guarantees predictable behaviour for frame‑driven consumers.

---

# Version Counters

## version

`version` increments whenever observable runtime state changes within a generation.

Properties:

- contiguous within a generation
- starts at `0` for the baseline publish
- increments by `+1` for each subsequent observable change

## topology_version

`topology_version` increments only when **observable topology changes** occur.

Examples of topology change:

- device added
- device removed
- stream added
- stream removed

Non‑structural changes (for example counters or stream configuration updates) may increment `version` without incrementing `topology_version`.

---

# Snapshot Access Semantics

Two access patterns are supported.

## Signal‑driven

```
state_published(gen, version, topology_version)
→ get_state_snapshot()
```

Inside the signal handler, `get_state_snapshot()` returns the snapshot corresponding to that emission.

## Polling

```
get_state_snapshot() each frame
```

Outside the handler, `get_state_snapshot()` returns the latest observable runtime state.

Polling and signal‑driven consumption may be mixed safely.

---

# Snapshot Immutability

Published snapshots are **immutable**.

Properties:

- snapshots do not mutate after publication
- retaining references to older snapshots is safe
- older snapshots remain readable after newer publishes occur

Example:

```
old_snapshot = get_state_snapshot()

# later publishes occur

old_snapshot remains readable and unchanged
```

---

# What This Document Does Not Specify

This document intentionally does not describe:

- provider lifecycle states
- internal runtime bring‑up phases
- dev scaffolding behaviour
- scenario execution mechanisms
- platform‑specific provider behaviour

Those details belong to internal architecture documentation.

This document defines only the **observable Godot boundary contract**.
