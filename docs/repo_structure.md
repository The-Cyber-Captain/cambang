# CamBANG Repository Structure (v1)

This document defines the canonical source-tree layout, module
boundaries, and current build structure for CamBANG v1.

This structure reflects the current intended layout and may evolve as
the project matures.

It is designed to:

- keep core platform-agnostic
- isolate providers cleanly
- support future platforms without restructuring
- support synthetic / testing modes
- work cleanly with SCons and Godot GDExtension
- maintain deterministic ownership boundaries defined elsewhere

---

## 1. Top-level layout

```text
cambang/
├── SConstruct
├── README.md
├── docs/
│   ├── INDEX.md
│   ├── README.md
│   ├── CONTRIBUTING.md
│   ├── HOWTO-build_draft.txt
│   ├── naming.md
│   ├── state_snapshot.md
│   ├── provider_architecture.md
│   ├── core_runtime_model.md
│   ├── arbitration_policy.md
│   ├── repo_structure.md
│   ├── architecture/
│   │   ├── frame_sinks.md
│   │   ├── godot_boundary_contract.md
│   │   ├── lifecycle_model.md
│   │   ├── pattern_module.md
│   │   ├── provider_state_machines.md
│   │   ├── provider_strand_model.md
│   │   ├── publication_counter_examples.md
│   │   └── publication_model.md
│   ├── dev/
│   │   ├── build_and_scaffolding.md
│   │   ├── frameview_stage.md
│   │   ├── godot_boundary_verification_scenes.md
│   │   ├── maintainer_tools.md
│   │   ├── provider_compliance_checklist.md
│   │   ├── snapshot_truth_rules.md
│   │   ├── upstream_discrepancies.md
│   │   └── windows_mf_visibility_phase.md
│   └── screenshots/
│       └── .gdignore
├── thirdparty/                # if needed later
├── src/
│   ├── core/
│   ├── imaging/
│   ├── pixels/
│   ├── godot/
│   ├── smoke/
│   └── util/
└── tests/
```

---

## Documentation structure and authority

Documentation is structured deliberately to avoid drift:

- top-level `docs/*.md` files define **canonical architecture and policy**
- `docs/architecture/` contains **narrowly scoped supplements**
- `docs/dev/` contains **development-stage notes** and tooling / scaffolding docs

If contradiction appears, canonical documents take precedence.

See `docs/INDEX.md` for the canonical / supplement / dev classification.

---

## 2. `src/core/`

Pure platform-independent implementation.

Responsibilities include:

- core thread implementation
- event loop (blocking + timed wait)
- arbitration engine
- capture ID issuance
- warm scheduling
- retention scheduling
- `CBLifecycleRegistry`
- `CBStatePublisher`
- snapshot assembly
- spec stores (`CameraSpec`, `ImagingSpec`)

Suggested layout:

```text
src/core/
├── core_thread.h/.cpp
├── arbitration.h/.cpp
├── lifecycle_registry.h/.cpp
├── state_publisher.h/.cpp
├── snapshot/
│   ├── snapshot_types.h
│   └── snapshot_builder.h/.cpp
├── spec/
│   ├── camera_spec_store.h/.cpp
│   └── imaging_spec_store.h/.cpp
└── ids.h
```

Core must not include platform headers.

---

## 3. `src/imaging/`

Imaging provider domain root: the `ICameraProvider` surface, the
Core-bound façade naming surface, and concrete providers.

```text
src/imaging/
├── api/
│   ├── icamera_provider.h
│   ├── provider_contract_datatypes.h
│   └── provider_error_string.h/.cpp
├── broker/
│   ├── provider_broker.h/.cpp
│   └── mode.h/.cpp
├── platform/
│   └── <platform>/
│       ├── provider.h/.cpp
│       └── <platform-specific>/
├── synthetic/
│   └── provider.h/.cpp
└── stub/
    └── provider.h/.cpp
```

Rules:

- `api/` defines semantic contract and provider-agnostic datatypes
- `platform/` contains platform-backed providers; platform headers must not leak into Core
- `stub/` is smoke-only; the smoke harness remains stub-provider-only
- `broker/` is the naming surface for the Core-bound façade term and does not
  imply multi-provider runtime arbitration

---

## 4. `src/imaging/synthetic/`

`SyntheticProvider` exists for deterministic simulation and testing.

Capabilities may include:

- deterministic timestamps
- deterministic error injection
- profile mismatch simulation
- configurable latency simulation

`SyntheticProvider` must satisfy the `ICameraProvider` contract fully.

Build flag:

- `CAMBANG_ENABLE_SYNTHETIC`

Synthetic is not instantiated alongside a platform-backed provider.
When compiled in, it is selected as an alternate runtime mode of the
single provider instance bound to Core.

Core does not arbitrate between multiple providers.

---

## 4.x `src/pixels/`

Contains provider-agnostic pixel processing and synthetic rendering modules.

Current contents:

- `pattern/` — CPU packed RGBA/BGRA synthetic renderer

Pixel modules must remain independent of:

- core threading
- provider lifecycle
- snapshot schema

See `docs/architecture/pattern_module.md`.

---

## 5. `src/godot/`

Godot-facing objects (GDExtension layer).

```text
src/godot/
├── cambang_server.h/.cpp
├── cambang_rig.h/.cpp
├── cambang_device.h/.cpp
├── cambang_stream.h/.cpp
├── registration.cpp
└── bindings/
```

Responsibilities:

- wrap core command enqueue operations
- expose snapshot pointer safely
- emit `state_published` signal
- map error codes to Godot-friendly form
- keep logic minimal (no arbitration here)

Godot layer must never mutate core state directly.

---

## 6. `src/smoke/`

Contains internal **core smoke executable** entrypoints.

This code exists to validate core invariants quickly and deterministically
without involving Godot or platform camera stacks.

Properties:

- opt-in build
- stub-provider-only by design
- not part of the GDExtension artifact
- intended to exercise:
  - CoreRuntime lifecycle determinism
  - strict ingress ordering
  - dispatcher release-on-drop semantics
  - shutdown choreography under load

Primary location:

```text
src/smoke/core_spine_smoke.cpp
```

The smoke harness must remain independent of `provider=...` platform selection.

Smoke-only code paths are gated behind:

- `CAMBANG_INTERNAL_SMOKE`

---

## 7. `src/util/`

Shared utilities.

```text
src/util/
├── fourcc.h
├── thread_utils.h
├── time_utils.h
├── lockfree_queue.h
└── logging.h
```

Utilities must remain platform-neutral.

---

## 8. `tests/`

Test harness and deterministic integration tests.

Illustrative layout:

```text
tests/
├── synthetic_arbitration_tests.cpp
├── lifecycle_tests.cpp
├── warm_policy_tests.cpp
└── snapshot_tests.cpp
```

Tests should:

- use `SyntheticProvider`
- validate snapshot determinism
- validate preemption correctness
- validate retention sweep logic

CI should run deterministic tests with synthetic support enabled.

---

## 9. SCons structure

### Build targets

Examples:

- `cambang` (GDExtension shared library)
- optional test or validation binaries

### Platform selection

Illustrative flags:

```text
scons platform=android provider=android_camera2
scons platform=linux provider=stub
scons synthetic=yes
```

Provider selection must compile exactly one **platform provider implementation**
into the final build.

A platform provider may internally delegate to multiple backend modules,
but Core binds to exactly one `ICameraProvider` instance at runtime.

### Compile-time flags

Common flags include:

- `CAMBANG_ENABLE_SYNTHETIC`
- `CAMBANG_DEBUG_LIFECYCLE`
- `CAMBANG_STRICT_ASSERTS`

---

## 10. Dependency rules

- `core/` must not depend on `godot/`
- `core/` must not depend on platform-specific provider headers
- `provider/` may depend on platform headers
- `godot/` depends on `core/`
- `synthetic/` depends on provider interface only

This preserves architectural layering.

---

## 11. Future-proofing guarantees

This structure supports:

- multiple providers without structural refactor
- test-only builds without full platform SDKs
- headless simulation builds
- new stream intents
- additional snapshot fields
- cross-platform expansion

---

## 12. Invariants

- Core is platform-agnostic.
- Providers are isolated by directory.
- Synthetic provider is first-class.
- Godot layer is thin and non-authoritative.
- Build selection chooses one platform provider implementation per build.
- Core always interacts with exactly one provider instance at runtime.
