
# Provider Strand Model

Status: Architecture Supplement  
Supplements: `provider_architecture.md`  
Purpose: Clarifies the serialized callback / provider strand delivery model used between providers and the core runtime.

## Overview

Providers deliver events into the core using a **single serialized callback context** referred to as the *provider strand*.  
The strand guarantees:

- ordered delivery of non-frame events
- deterministic lifecycle sequencing
- absence of concurrent provider → core mutation

This document explains the delivery model but **does not redefine the canonical provider rules**.

## Event Classes

Provider facts fall into four broad classes:

| Event Class | Example | Delivery Policy |
|---|---|---|
| Lifecycle | device add/remove, acquisition-session create/destroy, stream start/stop | must never be dropped |
| Native-object | provider/device/acquisition-session/stream/support-resource object create/destroy reports | must never be dropped |
| Error | provider, device, or stream error reports | must never be dropped |
| Frame | video frame delivery | may be coalesced or dropped under backpressure |

Lifecycle, native-object, and error events must always preserve ordering.

After strand delivery, CoreRuntime may classify queued provider facts before
integration. Capture-critical facts (capture started/frame/completed/failed,
including rig capture facts identified from core admission metadata) are above
repeating stream frames. Repeating stream frames can be deferred behind pending
command/request work and can be passed by capture-critical facts. This does not
change strand delivery, public API, or the non-lossy treatment of lifecycle,
native-object, error, or unknown facts. Repeating stream frames remain
latest-state work and may be coalesced after strand delivery only with a newer
same-stream/session frame before a non-lossy barrier; capture frames are never
coalesced.

Topology change is not a separate event class. It is an effect reflected by
lifecycle and native-object truth, and later by snapshot `topology_version`.

Current concrete reminder for synthetic realization: `SyntheticProvider` now
realizes truthful `AcquisitionSession` seams for both stream-backed and
capture-only paths. The seam is created when provider truth first requires it
for the device and is destroyed when both stream and capture references have
been released.

## Backpressure

Frame delivery may be coalesced or dropped to avoid unbounded buffering.  
Providers must never drop lifecycle, native-object, or error events.

## Synthetic and Stub Providers

SyntheticProvider and StubProvider emulate platform callbacks but still deliver events through the same strand model so that runtime behaviour matches real providers.

## Non‑Goals

This supplement does not define public APIs or lifecycle state machines. Those remain canonical in:

- `provider_architecture.md`
- `provider_state_machines.md`
