# CamBANG Maintainer Tools

The CamBANG repository includes several small command-line utilities used by
maintainers to validate runtime invariants and profile subsystems.

These tools are **not user-facing** and are separate from the Godot harness
scenes intended for plugin users.

| Tool | Purpose | Category |
|---|---|---|
| `core_spine_smoke` | Minimal Core runtime invariant validation using the stub provider | Smoke test |
| `synthetic_timeline_verify` | Deterministic verification of SyntheticProvider timeline behaviour and Core registry truth | Verification |
| `pattern_render_bench` | Pattern renderer performance benchmark | Benchmark |

## Terminology

**Smoke Test**

A minimal executable that verifies the core runtime can start and perform basic
operations without crashing.

**Verification Tool**

A deterministic executable used by maintainers to validate internal invariants
and regression behaviour.

**Benchmark**

A tool used to measure subsystem performance.

**Harness**

User-facing Godot scenes or projects demonstrating features or testing real
hardware. Harnesses live under the `tests/` directory.
