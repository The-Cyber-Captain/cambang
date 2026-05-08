# Producer Backing Capability Notes

Status: Dev note  
Purpose: Freeze the current working understanding of producer backing capability,
provider backing-selection policy, and non-release capability override use in
the current synthetic-stream GPU-primary exemplar phase.

## 1. Producer capability vs provider policy

Producer backing capability is the truthful statement of which backing kinds a
producer implementation can realize for a given artifact in the current runtime.

Examples:

- CPU-backed realization available
- GPU-backed realization available

This is capability truth, not provider policy.

Providers choose among the backing kinds actually advertised by the producer.

## 2. Synthetic producer implementation invariant

For the current synthetic producer implementation:

- CPU backing is always available by implementation invariant.
- GPU backing is available only when a real GPU-backing implementation is
  available in the current runtime.

This does not imply that all producers or future platform-backed providers share
the same invariant.

Synthetic stream results now exercise a concrete GPU-primary retained path
(`GPU_SURFACE`) when GPU backing is selected and supported.

## 3. Policy space still includes GPU-only

Provider/core/result policy must still be able to reason about:

- CPU-only
- CPU+GPU
- GPU-only

even if a particular synthetic producer implementation does not naturally
realize all of those capability sets truthfully in release operation.

## 4. Dev-only capability override

A dev-only override may temporarily falsify synthetic producer backing
advertisement to validate provider/core/result policy handling.

Such overrides are:

- non-release
- verification-only
- not runtime truth

## 5. Primary vs auxiliary backing

A realized image-bearing artifact has exactly one primary payload kind.

If more than one backing is available, one backing is primary and others are
auxiliary.

Auxiliary backings may improve capability/cost outcomes but do not create
multiple primary payload kinds.

## 6. CPU fallback asymmetry

CPU `Image` materialization remains an available result-facing fallback path.

This does not imply that CPU backing is always primary or always cheap.
CPU fallback availability also does not imply eager auxiliary CPU retention for
GPU-primary results.
It does imply that CPU and GPU backing should not be treated as perfectly
symmetric choices.


## 7. Synthetic stream GPU update env-control inventory

### 7.1 Active policy control

- `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=display_demanded|always`
  - default: `display_demanded`
  - `always` is the maintainer eager-update comparison override

### 7.2 Maintainer diagnostics (retained)

- `CAMBANG_DEV_SYNTH_TRIAGE_TRACE`
  - default: off
  - enables SyntheticTriage startup/path markers and `Synthetic*TriageMetrics` output
- `CAMBANG_DEV_SYNTH_CATCHUP_CAP`
  - default: `0` (uncapped catchup)
  - maintainer harness control for virtual-time catchup shaping; `2` is used for
    two-stream due-together tests to avoid intentionally lossy single-frame catchup
- `CAMBANG_DEV_DISPLAY_DEMAND_TRACE`
  - default: off
  - maintainer-only demand lifetime tracing for display-demand activation/deactivation
- `CAMBANG_DEV_SYNTH_GPU_TRACE`
  - default: off
  - maintainer-only retained-GPU allocation/free and runtime truth tracing
- `CAMBANG_STREAM_LOAD_FRAME_SPIKE_TRACE`
- `CAMBANG_STREAM_LOAD_FRAME_SPIKE_TOP_N`

### 7.3 Maintainer harness selector

- `CAMBANG_EXERCISE`
  - preferred harness-level exercise selector (not product API config)
  - includes Scene 71/72 maintainer exercises such as `display_oneshot`,
    `display_latest`, `no_display_default`, and `no_display_eager`
  - `display_oneshot` is the default Scene 71/72 exercise when unset
  - `display_latest` is the repeated latest-binding stress exercise (Scene 72)
  - `no_display_eager` resolves to effective `CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=always`
    via exercise selection unless an explicit conflicting low-level policy is set

Aggregate telemetry note:
- Per-frame `FrameBufferLease` native-object create/destroy snapshot rows remain removed.
- High-frequency lease/backing observability is surfaced via `resource_aggregate` counters,
  while long-lived identity-bearing native resources remain in `native_objects`.

### 7.4 Removed temporary knobs

- `CAMBANG_DEV_SYNTH_UPDATE_GPU_ONLY_WHEN_DISPLAY_REQUESTED` (deprecated alias removed)
- `CAMBANG_DEV_SYNTH_SKIP_GPU_TEXTURE_UPDATE` (diagnostic bypass removed)
- `CAMBANG_DEV_SYNTH_REUSE_RENDERED_FRAME` (diagnostic forced-reuse removed)
