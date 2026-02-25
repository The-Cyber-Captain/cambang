# CamBANG Repository Structure (v1)

This document defines the canonical source tree layout, module
boundaries, and build structure for CamBANG v1.

This structure is designed to:

-   Keep core platform-agnostic
-   Isolate providers cleanly
-   Support future platforms without restructuring
-   Support synthetic/testing modes
-   Work cleanly with SCons and Godot GDExtension
-   Maintain deterministic ownership boundaries defined in earlier docs

------------------------------------------------------------------------

## 1. Top-Level Layout

``` text
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
│   │   └── frame_sinks.md
│   ├── dev/
│   │   ├── frameview_stage.md
│   │   ├── gde_build_loop_windows.md
│   │   ├── gde_scaffolding_windows_mingw.md
│   │   ├── scons_intent.md
│   │   └── upstream_discrepancies.md
│   └── screenshots/
│       └── .gdignore
├── thirdparty/                # (if needed later)
├── src/
│   ├── core/
│   ├── provider/
│   ├── godot/
│   ├── synthetic/
│   ├── smoke/
│   └── util/
└── tests/
```

------------------------------------------------------------------------


### Documentation structure and authority

Documentation is structured deliberately to avoid drift:

- Top-level `docs/*.md` files define **canonical architecture and policy**.
- `docs/architecture/` contains **narrowly scoped supplements** to canonical docs.
- `docs/dev/` contains **development-stage notes** and scaffolding documentation.

If a contradiction appears, canonical documents take precedence.

See `docs/INDEX.md` for the canonical/supplement/dev classification.

## 2. src/core/

Pure platform-independent implementation.

Responsibilities: - Core thread implementation - Event loop (blocking +
timed wait) - Arbitration engine - Capture ID issuance - Warm
scheduling - Retention scheduling - CBLifecycleRegistry -
CBStatePublisher - Snapshot assembly - Spec stores (CameraSpec,
ImagingSpec)

Suggested layout:

``` text
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

------------------------------------------------------------------------

## 3. src/provider/

Provider interface and implementations.

``` text
src/provider/
├── icamera_provider.h
├── provider_types.h
├── android/
│   ├── android_camera2_provider.h/.cpp
│   ├── android_device.h/.cpp
│   └── android_stream.h/.cpp
├── stub/
│   └── stub_provider.h/.cpp
```

Rules: - `icamera_provider.h` defines the semantic interface. -
Platform-specific code is isolated under subdirectories. - Android code
must not leak into core headers.

Future platforms: - linux_v4l2/ - windows_mediafoundation/ - etc.

------------------------------------------------------------------------

## 4. src/synthetic/

SyntheticProvider for deterministic simulation and testing.

``` text
src/synthetic/
├── synthetic_provider.h/.cpp
├── synthetic_device.h/.cpp
└── synthetic_stream.h/.cpp
```

Capabilities: - deterministic timestamps - deterministic error
injection - profile mismatch simulation - configurable latency
simulation

Synthetic provider must satisfy `ICameraProvider` contract fully.

Build flag: - `CAMBANG_ENABLE_SYNTHETIC`

------------------------------------------------------------------------

## 5. src/godot/

Godot-facing objects (GDExtension layer).

``` text
src/godot/
├── cambang_server.h/.cpp
├── cambang_rig.h/.cpp
├── cambang_device.h/.cpp
├── cambang_stream.h/.cpp
├── registration.cpp
└── bindings/
```

Responsibilities: - Wrap core command enqueue operations - Expose
snapshot pointer safely - Emit `state_published` signal - Map error
codes to Godot-friendly form - Maintain minimal logic (no arbitration
here)

Godot layer must never mutate core state directly.

------------------------------------------------------------------------
## 6. src/smoke/

Contains internal **core smoke executable** entrypoints.

This code exists to validate core invariants quickly and deterministically
without involving Godot or platform camera stacks.

**Properties**

- Opt-in build (not a default target).
- Stub-provider-only by design (must not depend on platform providers).
- Not part of the GDExtension artifact.
- Intended to exercise:
    - CoreRuntime lifecycle determinism
    - strict ingress ordering
    - dispatcher release-on-drop semantics
    - shutdown choreography under load

**Location**

- `src/smoke/core_spine_smoke.cpp`

**Build / gating**

Smoke-only code paths are gated behind `CAMBANG_INTERNAL_SMOKE` to keep
the production/core surface free of temporary instrumentation hooks.
Legacy “IDE smoke” nomenclature is intentionally not used.

------------------------------------------------------------------------

## 7. src/util/

Shared utilities.

``` text
src/util/
├── fourcc.h
├── thread_utils.h
├── time_utils.h
├── lockfree_queue.h (optional)
└── logging.h
```

Utilities must remain platform-neutral.

------------------------------------------------------------------------

## 8. tests/

Test harness and deterministic integration tests.

``` text
tests/
├── synthetic_arbitration_tests.cpp
├── lifecycle_tests.cpp
├── warm_policy_tests.cpp
└── snapshot_tests.cpp
```

Tests should: - use SyntheticProvider - validate snapshot determinism -
validate preemption correctness - validate retention sweep logic

CI must run tests with synthetic provider enabled.

------------------------------------------------------------------------

## 9. SCons Structure

### 9.1 Build Targets

-   `cambang` (GDExtension shared library)
-   Optional: `cambang_tests` (unit test binary)

### 9.2 Platform Selection

Use SCons flags:

``` text
scons platform=android provider=android_camera2
scons platform=linux provider=stub
scons synthetic=yes
```

Provider selection must compile only one provider implementation into
the final binary.

### 9.3 Compile-time flags

-   `CAMBANG_ENABLE_SYNTHETIC`
-   `CAMBANG_DEBUG_LIFECYCLE`
-   `CAMBANG_STRICT_ASSERTS`

------------------------------------------------------------------------

## 10. Dependency Rules

-   `core/` must not depend on `godot/`
-   `core/` must not depend on `provider/android/`
-   `provider/` may depend on platform headers
-   `godot/` depends on `core/`
-   `synthetic/` depends on `provider/` interface only

This preserves architectural layering.

------------------------------------------------------------------------

## 11. Future-Proofing Guarantees

This structure supports:

-   Multiple providers without refactor
-   Test-only builds without Android SDK
-   Headless simulation builds
-   Addition of new stream intents
-   Addition of new snapshot fields
-   Cross-platform expansion

------------------------------------------------------------------------

## 12. Implementation Order (Recommended)

1.  Stub provider + core thread skeleton
2.  Snapshot builder
3.  Synthetic provider
4.  Arbitration engine
5.  Warm + retention scheduling
6.  Godot bindings
7.  Android provider
8.  Integration tests

This order allows early deterministic validation before touching
camera2.

------------------------------------------------------------------------

## 13. Invariants

-   Core is platform-agnostic.
-   Providers are isolated by directory.
-   Synthetic provider is first-class.
-   Godot layer is thin and non-authoritative.
-   SCons build selects exactly one provider at compile time.
