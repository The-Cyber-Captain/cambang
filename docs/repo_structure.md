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
│   ├── THIRD_PARTY_NOTICES.md
│   ├── naming.md
│   ├── state_snapshot.md
│   ├── provider_architecture.md
│   ├── core_runtime_model.md
│   ├── arbitration_policy.md
│   ├── repo_structure.md
│   ├── status_panel_surface_policy.md
│   ├── architecture/
│   │   ├── frame_sinks.md
│   │   ├── godot_boundary_contract.md
│   │   ├── lifecycle_model.md
│   │   ├── pattern_module.md
│   │   ├── pixel_payload_and_result_contract.md
│   │   ├── provider_state_machines.md
│   │   ├── provider_strand_model.md
│   │   ├── publication_counter_examples.md
│   │   ├── publication_model.md
│   │   ├── synthetic_picture_appearance_in_scenarios.md
│   │   └── synthetic_timeline_scenarios.md
│   ├── dev/
│   │   ├── build_and_scaffolding.md
│   │   ├── cambang_ui_design_standard_integrated.md
│   │   ├── cambangstatuspanel_mappings.md
│   │   ├── maintainer_tools.md
│   │   ├── provider_compliance_checklist.md
│   │   ├── state_snapshot_schema_mapping.md
│   │   ├── status_panel_fixture_taxonomy.md
│   │   ├── testing_audit_lenses.md
│   │   └── upstream_discrepancies.md
│   └── screenshots/
│       └── .gdignore
├── external_scenarios/
├── schema/
│   └── state_snapshot/v1/state_snapshot_schema.json
├── src/
│   ├── core/
│   ├── dev/
│   ├── godot/
│   ├── imaging/
│   ├── pixels/
│   └── smoke/
└── tests/
```

---

## Documentation structure and authority

Documentation is structured deliberately to avoid drift:

- canonical documents are explicitly listed in `docs/INDEX.md`
- top-level `docs/*.md` files may include canonical docs, policy docs,
  entry points, or contributor/support docs depending on index classification
- `docs/architecture/` contains **narrowly scoped supplements**
- `docs/dev/` contains **development-stage notes** and tooling / scaffolding docs

If contradiction appears, canonical documents listed in `docs/INDEX.md` take
precedence.

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
- `CoreNativeObjectRegistry`
- `ResourceAggregateTelemetry`
- `SnapshotBuilder`
- `IStateSnapshotPublisher` publication boundary and `StateSnapshotBuffer` latest-snapshot buffer
- spec state (`CoreSpecState`)
- result/capture assembly registries

Current layout includes:

```text
src/core/
├── core_runtime.h/.cpp
├── core_thread.h/.cpp
├── core_dispatcher.h/.cpp
├── core_*_registry.h/.cpp
├── core_spec_state.h/.cpp
├── core_result_store.h/.cpp
├── provider_callback_ingress.h/.cpp
├── resource_aggregate_telemetry.h/.cpp
├── state_snapshot_buffer.h
├── i_state_snapshot_publisher.h
├── snapshot/
│   ├── state_snapshot.h
│   └── snapshot_builder.h/.cpp
└── synthetic_timeline_request_binding.h/.cpp
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
│   ├── provider_error_string.h/.cpp
│   ├── provider_strand.h/.cpp
│   └── timeline_teardown_trace.h/.cpp
├── broker/
│   ├── banner_info.h/.cpp
│   ├── mode.h
│   └── provider_broker.h/.cpp
├── platform/
│   └── windows/
│       ├── provider.h/.cpp
│       └── mf/
│           ├── com_ptr.h
│           └── types.h
├── synthetic/
│   ├── provider.h/.cpp
│   ├── scenario*.h/.cpp
│   ├── virtual_clock.h
│   └── gpu_*
└── stub/
    └── provider.h/.cpp
```

Rules:

- `api/` defines semantic contract and provider-agnostic datatypes
- `platform/` contains platform-backed providers; platform-native headers and
  API adaptation must not leak into Core, Godot public objects, or shared
  provider API
- a platform provider may use provider-local helper files and subdirectories
  under `src/imaging/platform/<provider>/`
- `stub/` is a deterministic dev/test provider used by smoke and provider
  validation; it may be compiled into the GDE build with `provider=stub`, but it
  is not a production platform-backed provider
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
├── cambang_capture_result.h/.cpp
├── cambang_capture_result_set.h/.cpp
├── cambang_stream_result.h/.cpp
├── cambang_stream_result_internal.h/.cpp
├── cambang_result_convert.h/.cpp
├── state_snapshot_export.h/.cpp
├── synthetic_gpu_backing_bridge*.h/.cpp
└── module_init.cpp
```

Responsibilities:

- wrap core command enqueue operations
- expose snapshot copies safely
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
- providerless baseline mode is available
- stub-backed mode is enabled when built with `provider=stub`
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

Smoke verification must remain independent of platform-backed provider implementations. The
core spine smoke executable can run providerless baseline checks, while stub-backed
coverage and stress mode require a `provider=stub` smoke build.

Smoke-only code paths are gated behind:

- `CAMBANG_INTERNAL_SMOKE`

---

## 7. `src/dev/`

Development-only helpers. Current contents include `cli_log.h`.

Development helpers must not become public API or platform contract authority.

---

## 8. `tests/`

Test suites and deterministic integration tests.

Current layout includes the Godot/GDE harness under:

```text
tests/cambang_gde/
├── addons/
├── fixtures/status_panel/
├── scenes/
└── scripts/
```

Tests should:

- use the smallest harness that proves the intended invariant
- validate snapshot determinism and publication semantics
- validate provider-independent Core invariants separately from platform-backed provider behavior
- treat fixtures as authored verification artifacts, not disposable output to mutate until green

CI/local validation should run deterministic tests with synthetic support where relevant.

---

## 9. SCons structure

### Build targets

Examples from the current SCons entrypoint:

- `gde=yes` — build the GDExtension artifact
- `smoke=yes` — build smoke/verification binaries
- `platform_validate=yes` — build platform validation where available

### Platform selection

Illustrative flags:

```text
scons platform=windows provider=stub
scons platform=windows provider=windows_mediafoundation
scons platform=windows provider=stub synthetic=yes
```

Provider selection must compile exactly one selected provider implementation
into the final build. The current temporary build entrypoint explicitly rejects
`platform=android`; Android/Camera2 remains future platform work rather than a
current build path.

A platform provider may internally delegate to multiple backend modules,
but Core binds to exactly one `ICameraProvider` instance at runtime.

### Compile-time flags

Common flags include:

- `CAMBANG_ENABLE_SYNTHETIC`
- `CAMBANG_INTERNAL_SMOKE`

---

## 10. Dependency rules

- `core/` must not depend on `godot/`
- `core/` must not depend on platform-specific provider headers
- platform-backed provider code under `imaging/platform/` may depend on platform headers
- `godot/` depends on `core/` and the selected provider/broker surface through supported boundaries
- `imaging/synthetic/` depends on provider interface and provider-agnostic pixel modules only

This preserves architectural layering.

---

## 11. Future-proofing guarantees

This structure supports:

- additional provider implementations without Core structural refactor
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
