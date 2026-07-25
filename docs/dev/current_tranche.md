# Current tranche

## Alternate payload support — stage 2: planar retention

Follows `b23bbfc` (layout contract + descriptor-driven Core validators), which
added the vocabulary but left `ResultPayloadKind::CPU_PLANAR` without a writer.
This tranche gives it one.

Unlike stage 1, this **is** a behaviour change: a stream can, for the first
time, retain a payload that is not packed RGB.

### Scope

1. **Per-plane CPU retention.** Rename `CoreResultPayloadCpuPacked` to
   `CoreResultPayloadCpu` (18 usages across 5 files) and extend it with
   per-plane offset/stride/row metadata over the existing single contiguous
   byte buffer. Keeping one buffer preserves the `retained_bytes` zero-copy
   adoption path and matches how camera stacks actually hand over NV12 (Y then
   interleaved UV in one allocation).

2. **`CPU_PLANAR` writer.** `try_copy_cpu_planar_payload()` alongside the
   packed path, driven by `FrameView::effective_payload_layout()`. Planar
   frames retain with `payload_kind = CPU_PLANAR`; byte-budget accounting sums
   across planes.

3. **Format negotiation, first real use.** Core validates a requested
   `CaptureProfile.format_fourcc` against the provider's
   `stream_format_capabilities()` advertisement, rejecting an unsupported
   request deterministically rather than letting the provider fail late.
   `format_fourcc` is already authorable from GDScript for both stream and
   capture profiles, so no public API change is needed.

4. **SyntheticProvider NV12 output.** Synthetic advertises NV12 in
   `stream_format_capabilities()` and emits it. This is the reference provider
   finally modelling the awkward case rather than only the convenient one, and
   it makes stages 3-5 testable without hardware in the loop.

   Synthetic may convert from its existing packed render internally. That is a
   provider-local implementation detail, not a contract shortcut, and must be
   commented as such — determinism matters here, throughput does not.

5. **Truthful unsupported access.** At the end of this tranche an NV12 stream
   is retained but has no usable access path: `to_image()` and
   `get_display_view()` must report `UNSUPPORTED`, not produce wrong pixels.
   That intermediate state is the point — it is what stages 3 and 4 then close.

### Out of scope

- The NV12 GPU display path (stage 3).
- Lazy/memoized materialization conversion (stage 4).
- Platform providers advertising native YUV, and removal of their internal
  converters (stage 5).
- Encoded and RAW payload kinds.
- Any Godot-facing public API change.

### Acceptance criteria

- `CPU_PLANAR` has a real writer and is observable end to end.
- An NV12 stream retains, reports `payload_kind = cpu_planar`, and reports
  both access capabilities as `UNSUPPORTED` — verified by assertion, not by
  absence of a crash.
- A profile requesting a format the provider does not advertise is rejected
  deterministically at Core, with a stable error category.
- Packed RGBA/BGRA behaviour is bit-identical to `b23bbfc`; no existing scene
  changes verdict or timing class.
- No CPU access path can receive planar bytes.

### Validation expectations

Native verifiers (hard gate):

```sh
out/core_spine_smoke.exe
out/provider_compliance_verify.exe
out/restart_boundary_verify.exe
out/verify_case_runner.exe --run-all
```

Godot scenes (hard gate — retention and access truth both moved):

```powershell
.\run_godot.ps1 -Scene res://scenes/70_result_retrieval_verification.tscn -Windowed -CaptureLogs -TimeoutSec 90 -RunLabel scene70
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -Windowed -CaptureLogs -TimeoutSec 120 -RunLabel scene568
```

Scene 70's `result_access_timing_evidence` must still show
`capture_to_image.cpu_packed` and `stream_to_image.cpu_packed` at zero
failures with unchanged byte counts, proving the packed path did not regress
while the planar path was added.

New planar coverage is required, not optional: a verification case asserting
NV12 retention, `cpu_planar` payload kind, and `UNSUPPORTED` access truth.

Display teardown race stress gates should run this time, since retention
lifetime changed:

```powershell
.\run_cpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
```
