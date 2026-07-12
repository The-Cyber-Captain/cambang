# CamBANG Repository Structure (v1)

This document defines the canonical source-tree layout, module boundaries, and
current build structure for CamBANG v1.

This structure reflects the current intended layout and may evolve as the
project matures.

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
|-- SConstruct
|-- README.md
|-- docs/
|   |-- INDEX.md
|   |-- README.md
|   |-- CONTRIBUTING.md
|   |-- HOWTO-build_draft.txt
|   |-- THIRD_PARTY_NOTICES.md
|   |-- naming.md
|   |-- state_snapshot.md
|   |-- provider_architecture.md
|   |-- core_runtime_model.md
|   |-- arbitration_policy.md
|   |-- camera_fact_model.md
|   |-- adc_camera_description_v2.md
|   |-- repo_structure.md
|   |-- status_panel_surface_policy.md
|   |-- architecture/
|   |   |-- frame_sinks.md
|   |   |-- godot_boundary_contract.md
|   |   |-- imaging_spec_seam.md
|   |   |-- lifecycle_model.md
|   |   |-- pattern_module.md
|   |   |-- pixel_payload_and_result_contract.md
|   |   |-- provider_state_machines.md
|   |   |-- provider_strand_model.md
|   |   |-- publication_counter_examples.md
|   |   |-- publication_model.md
|   |   |-- synthetic_picture_appearance_in_scenarios.md
|   |   `-- synthetic_timeline_scenarios.md
|   |-- dev/
|   |   |-- build_and_scaffolding.md
|   |   |-- cambang_ui_design_standard_integrated.md
|   |   |-- cambangstatuspanel_mappings.md
|   |   |-- maintainer_tools.md
|   |   |-- provider_compliance_checklist.md
|   |   |-- state_snapshot_schema_mapping.md
|   |   |-- status_panel_fixture_taxonomy.md
|   |   |-- testing_audit_lenses.md
|   |   `-- upstream_discrepancies.md
|   `-- screenshots/
|       `-- .gdignore
|-- external_scenarios/
|-- ide/
|-- schema/
|   |-- adc/camera_description/v2/adc_camera_description_schema.json
|   `-- state_snapshot/v1/state_snapshot_schema.json
|-- scripts/
|-- site_scons/
|-- src/
|   |-- core/
|   |-- dev/
|   |-- godot/
|   |-- imaging/
|   |-- pixels/
|   `-- smoke/
`-- tests/
```

---

## Documentation structure and authority

Documentation is structured deliberately to avoid drift:

- canonical CamBANG architecture and policy documents are explicitly listed in
  `docs/INDEX.md`
- top-level `docs/*.md` files may include canonical docs, provisionally hosted
  external contract material, policy docs, entry points, or contributor/support
  docs depending on index classification
- `docs/architecture/` contains **narrowly scoped supplements**
- `docs/dev/` contains **development-stage notes** and tooling / scaffolding
  docs

If contradiction appears, follow the authority classification in
`docs/INDEX.md`.

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
- `IStateSnapshotPublisher` publication boundary and `StateSnapshotBuffer`
  latest-snapshot buffer
- spec state (`CoreSpecState`)
- result/capture assembly registries

Current layout includes:

```text
src/core/
|-- camera_concurrency_adc.h/.cpp
|-- core_runtime.h/.cpp
|-- core_thread.h/.cpp
|-- core_dispatcher.h/.cpp
|-- core_*_registry.h/.cpp
|-- core_spec_state.h/.cpp
|-- core_result_store.h/.cpp
|-- provider_callback_ingress.h/.cpp
|-- resource_aggregate_telemetry.h/.cpp
|-- state_snapshot_buffer.h
|-- i_state_snapshot_publisher.h
|-- snapshot/
|   |-- state_snapshot.h
|   `-- snapshot_builder.h/.cpp
`-- synthetic_timeline_request_binding.h/.cpp
```

Core must not include platform headers.

---

## 3. `src/imaging/`

Imaging provider domain root: the `ICameraProvider` surface, the Core-bound
facade naming surface, and concrete providers.

```text
src/imaging/
|-- api/
|   |-- icamera_provider.h
|   |-- provider_access_status.h
|   |-- provider_contract_datatypes.h
|   |-- provider_error_string.h/.cpp
|   |-- provider_strand.h/.cpp
|   `-- timeline_teardown_trace.h/.cpp
|-- broker/
|   |-- banner_info.h/.cpp
|   |-- mode.h
|   `-- provider_broker.h/.cpp
|-- synthetic/
|   |-- provider.h/.cpp
|   |-- builtin_scenario_library.h/.cpp
|   |-- scenario*.h/.cpp
|   |-- virtual_clock.h
|   `-- gpu_*
`-- stub/
    `-- provider.h/.cpp
```

Rules:

- `api/` defines semantic contract and provider-agnostic datatypes
- `platform/` is the reserved root for future platform-backed providers; no
  platform-backed provider source is present in the current tree
- when introduced, platform-native headers and API adaptation must not leak
  into Core, Godot public objects, or shared provider API
- a platform provider may use provider-local helper files and subdirectories
  under `src/imaging/platform/<provider>/`
- `stub/` is a deterministic dev/test provider used by host-native maintainer
  tools and provider validation; it is not a public GDE provider selection and
  is not a production platform-backed provider
- `broker/` is the naming surface for the Core-bound facade term and does not
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

Synthetic is not instantiated alongside a platform-backed provider. When
compiled in, it is selected as an alternate runtime mode of the single provider
instance bound to Core.

Core does not arbitrate between multiple providers.

---

## 4.x `src/pixels/`

Contains provider-agnostic pixel processing and synthetic rendering modules.

Current contents:

- `pattern/` - CPU packed RGBA/BGRA synthetic renderer

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
|-- cambang_server.h/.cpp
|-- cambang_rig.h/.cpp
|-- cambang_device.h/.cpp
|-- cambang_capture_result.h/.cpp
|-- cambang_capture_result_set.h/.cpp
|-- cambang_stream_result.h/.cpp
|-- cambang_stream_result_internal.h/.cpp
|-- cambang_result_convert.h/.cpp
|-- state_snapshot_export.h/.cpp
|-- godot_gpu_display_service.h/.cpp
|-- synthetic_gpu_backing_bridge*.h/.cpp
`-- module_init.cpp
```

Responsibilities:

- wrap core command enqueue operations
- expose snapshot copies safely
- emit `state_published` signal
- map error codes to Godot-friendly form
- keep logic minimal (no arbitration here)

Godot layer must never mutate core state directly.

`godot_gpu_display_service` is the narrow, non-owning Godot-side display
adapter resolver for GPU-backed stream display views. Today it forwards the
synthetic legacy retained backing to `synthetic_gpu_backing_bridge`; future
descriptor-only or platform-backed display adapters should attach at this
Godot-layer seam rather than moving Godot `Texture2D`/RID ownership into Core,
providers, or public result APIs.

---

## 6. `src/smoke/`

Contains internal maintainer-tool entrypoints.

This code exists to validate core invariants quickly and deterministically
without involving Godot or platform camera stacks. The tools are built by the
`maintainer_tools` alias and by default development builds under
`maintainer_tools=yes`.

Properties:

- host-native deterministic build artifacts
- providerless baseline coverage is available where applicable
- StubProvider and SyntheticProvider are used where needed for deterministic
  maintainer coverage
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

Maintainer-tool verification must remain independent of platform-backed
provider implementations except for explicit
`platform_runtime_validate=yes` validators. The deterministic tools use
providerless, StubProvider, and SyntheticProvider coverage as needed.

Maintainer-tool code paths are gated behind:

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
|-- addons/
|-- fixtures/status_panel/
|-- scenes/
`-- scripts/
```

Tests should:

- use the smallest harness that proves the intended invariant
- validate snapshot determinism and publication semantics
- validate provider-independent Core invariants separately from
  platform-backed provider behavior
- treat fixtures as authored verification artifacts, not disposable output to
  mutate until green

CI/local validation should run deterministic tests with synthetic support where
relevant.

---

## 9. SCons structure

The repo-root `SConstruct` is the source of truth for the build contract.
Assignment-style variables outside the declared public set are rejected.

### Supported variables

- `gde=yes|no` - include the selected GDE/plugin artifact family; default `yes`
- `maintainer_tools=yes|no` - include host-native deterministic maintainer
  tools; default `yes`
- `platform=<windows|android|linux|macos|ios|web>` - select the GDE target
  platform; default host-detected
- `target=<debug|release|template_debug|template_release>` - select
  debug/release shape; default `debug`
- `arch=<x86_64|x86_32|arm64|arm32>` - select target architecture naming;
  default `x86_64`
- `precision=<single|double>` - forwarded to `godot-cpp`; default `single`
- `godot_cpp=<delegated|external>` - choose root handling for selected
  `godot-cpp` artifacts; default `delegated`
- `platform_runtime_validate=yes|no` - include selected platform runtime
  validation artifacts; default `no`
- `COMPDB_PATH=<path>` - compile database path; default `compile_commands.json`
- `use_mingw=yes|no|auto` - Windows MinGW selection; default `auto`
- `use_llvm=yes|no|auto` - Windows MinGW-LLVM selection; default `auto`
- `mingw_prefix=<path>` - optional MinGW installation prefix forwarded to
  `godot-cpp`; default empty
- `windows_mingw_static_runtime=auto|yes|no` - Windows MinGW GDE static-runtime
  link mode; `auto` enables it for Windows MinGW GDE builds; default `auto`
- `warnings_as_errors=yes|no` - treat warnings as errors; default `no`
- `android_api_level=<level>` - Android GDE NDK Clang target API level; default
  `24`
- `ndk_version=<version>` - Android NDK version used with `ANDROID_HOME` /
  `ANDROID_SDK_ROOT`; default `28.1.13356709`
- `ANDROID_HOME=<path>` - optional Android SDK root for Android GDE builds;
  default process environment fallback

Assignment-style variables outside this declared public set are rejected by
`SConstruct`.

### Build aliases

- `all` - default build target; builds the selected families controlled by
  `gde`, `maintainer_tools`, and `platform_runtime_validate`
- `maintainer_tools` - host-native deterministic smoke/verifier/benchmark
  tools; not platform-scoped by `platform=<...>`
- `gde` - selected CamBANG GDE/plugin artifact family selected by `platform`,
  `target`, `arch`, and `precision`
- `godot_cpp` - selected root-modelled `thirdparty/godot-cpp` outputs;
  delegated by default, or externally prepared with `godot_cpp=external`
- `platform_runtime_validate` - selected platform runtime validation artifacts
- `cambang` - ownership-wide CamBANG clean alias
- `gde_all` - clean-only utility alias for all known CamBANG GDE object/output
  paths
- `build_all` - compatibility alias to `all`

### Clean scope

- `scons -c` and `scons -c all` clean CamBANG-owned outputs plus selected
  root-modelled `godot_cpp` outputs in `godot_cpp=delegated`; in
  `godot_cpp=external`, broad root cleans preserve selected `godot-cpp` outputs
- `scons -c cambang` cleans CamBANG-owned outputs, including `COMPDB_PATH`, and
  preserves `thirdparty/godot-cpp`
- `scons -c maintainer_tools` cleans maintainer-tool executables and
  `out/maintainer_tools_obj` only
- `scons -c gde` cleans the selected GDE object tree and selected plugin
  artifact only, preserving `thirdparty/godot-cpp` and `COMPDB_PATH`
- `scons -c gde_all` cleans all known CamBANG GDE object/output paths
- `scons -c godot_cpp` cleans only the selected generated-header sentinel and
  selected static library modelled by the root build
- `scons -c platform_runtime_validate` cleans selected platform runtime
  validation artifacts only

Root clean does not deep-clean all internal `thirdparty/godot-cpp` object
files. The default `godot_cpp=delegated` mode invokes
`python -m SCons -C thirdparty/godot-cpp ...` for one-command builds.
`godot_cpp=external` never invokes that delegated build and requires the
selected generated header and static library to already exist, which helps
developers avoid selected-platform rebuild churn when alternating targets.

### Platform/provider mapping

`platform=<...>` selects the GDE target platform, not the maintainer-tool host
platform.

- `windows` -> `windows_winrt` -> `src/imaging/platform/windows`
- `android` -> `android_camera2` -> `src/imaging/platform/android`
- `linux` -> `linux_v4l2` -> `src/imaging/platform/linux`
- `macos` -> `apple_avfoundation` -> `src/imaging/platform/apple`
- `ios` -> `apple_avfoundation` -> `src/imaging/platform/apple`
- `web` -> `web_getusermedia` -> `src/imaging/platform/web`

Declared GDE platforms may build synthetic-capable artifacts without a compiled
platform-backed provider. In those artifacts platform-backed mode is reported
as unavailable by compiled capability metadata and must fail visibly at runtime;
SyntheticProvider remains an alternate runtime mode of the single provider
instance, and StubProvider remains maintainer/dev-only rather than a production
GDE fallback. Clean mode is safe for all declared platforms.

### Object/output paths

- maintainer tools object tree: `out/maintainer_tools_obj`
- GDE object tree: `out/gde_obj/<platform>/<godot_target>/<arch>/<precision>`
- Windows debug GDE artifact example:
  `tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll`
- selected `godot-cpp` generated-header sentinel:
  `thirdparty/godot-cpp/gen/include/godot_cpp/core/ext_wrappers.gen.inc`
- selected Windows MinGW `godot-cpp` static library example:
  `thirdparty/godot-cpp/bin/libgodot-cpp.windows.template_debug.x86_64.a`

### Compile-time flags

Common internal defines include:

- `CAMBANG_ENABLE_SYNTHETIC`
- `CAMBANG_INTERNAL_SMOKE`

## 10. Dependency rules

- `core/` must not depend on `godot/`
- `core/` must not depend on platform-specific provider headers
- platform-backed provider code under `imaging/platform/` may depend on
  platform headers
- `godot/` depends on `core/` and the selected provider/broker surface through
  supported boundaries
- `imaging/synthetic/` depends on provider interface and provider-agnostic
  pixel modules only

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
