# Producer Backing Capability Notes

Status: Dev note  
Purpose: Freeze the current working understanding of producer backing capability,
provider backing-selection policy, and non-release capability override use in
preparation for the first non-CPU exemplar tranche.

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
It does imply that CPU and GPU backing should not be treated as perfectly
symmetric choices.