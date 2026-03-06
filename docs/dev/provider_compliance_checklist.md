# Provider Compliance Checklist

This document provides a temporary checklist for verifying that provider
implementations conform to the CamBANG provider contract and lifecycle
truthfulness guarantees.

The checklist is intended for maintainers while provider implementations
are being brought into full specification compliance.

------------------------------------------------------------------------

## Threading and callback discipline

- All provider → core facts are delivered through the provider strand.
- Platform callbacks never invoke core callbacks directly.
- Worker threads post work to the strand rather than calling core
  services directly.

------------------------------------------------------------------------

## Event-class guarantees

Lifecycle, native-object, and error events are **non-lossy**.

Frame events may be dropped under pressure.

Dropping frames must not cause lifecycle or native-object events to be
lost.

------------------------------------------------------------------------

## Native object reporting

Providers emit lifecycle registry events for:

- Provider
- Device
- Stream
- FrameProducer

Creation events occur when resources are acquired.

Destruction events occur only when resources are actually released.

------------------------------------------------------------------------

## Timestamp correctness

All frames delivered to Core include a `CaptureTimestamp` containing:

- value
- tick_ns
- domain

Timestamp domain semantics follow `naming.md`.

------------------------------------------------------------------------

## Shutdown discipline

Provider shutdown must follow the ordered phases:

1. admission close
2. stop production
3. resource release
4. strand drain barrier
5. strand stop

Shutdown must not fabricate destruction events for resources that were
not successfully released.

------------------------------------------------------------------------

## Verification

Providers should be validated using the maintainer tools and smoke
harness to confirm:

- lifecycle ordering correctness
- absence of dropped lifecycle events
- accurate snapshot reporting
- deterministic shutdown behaviour

------------------------------------------------------------------------