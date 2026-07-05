# CamBANG Build and Scaffolding

This document records development-stage notes for building CamBANG and the
Godot GDExtension scaffolding. It is **not canonical architecture**; it captures
build-system direction, scaffolding constraints, and repeatable local workflow.

Build system: **SCons-only**. CMake is not supported. The repo-root
`SConstruct` is the source of truth for the build contract.

---

## 1. Purpose and status

This document preserves agreed build direction and development scaffolding
behaviour across implementation phases.

It exists to:

- preserve build-system intent
- avoid accidental toolchain or CLI drift
- document GDE scaffolding constraints discovered in practice
- describe the repeatable local Windows / MinGW build loop

This document does **not** define runtime architecture, provider lifecycle,
resource ownership rules, or public API semantics.

---

## 2. Guiding principles

### 2.1 Mirror Godot ecosystem conventions

CamBANG's SCons interface mirrors the vocabulary used by `godot-cpp-template`,
Godot engine SCons builds, and `thirdparty/godot-cpp` where practical.

Primary target-shape variables are:

- `platform`
- `target`
- `arch`
- `precision`

Windows-specific selection remains explicit:

- `use_mingw`
- `use_llvm`
- `mingw_prefix`

### 2.2 No silent ABI surprise

On Windows systems where both MSVC and MinGW are present, the selected toolchain
must be explicit or clearly reported. From Git Bash / MinGW environments,
`use_mingw=yes` is the recommended explicit choice.

### 2.3 Local builds are first-class

CamBANG must build locally without CI, cloud tooling, or internet access during
normal iteration. CI may later validate release flows and artifacts, but local
iteration remains primary.

### 2.4 Delegate `godot-cpp` build policy

The repo-root `SConstruct` delegates the selected `godot-cpp` build to
`thirdparty/godot-cpp`. CamBANG's root build remains an orchestration layer and
should not become a replacement compiler-policy authority for `godot-cpp`.

### 2.5 SCons is the single source of truth

CamBANG builds via the repo-root `SConstruct`.

- CMake is intentionally removed.
- IDE integration should call SCons directly, or via an external-build wrapper.
- Assignment-style variables outside the declared public set are rejected.

---

## 3. Public SCons interface

Supported assignment-style variables are exactly:

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `gde` | `yes`, `no` | `yes` | Include the selected GDE/plugin artifact family. |
| `maintainer_tools` | `yes`, `no` | `yes` | Include host-native deterministic maintainer tools. |
| `platform` | `windows`, `android`, `linux`, `macos`, `ios`, `web` | host-detected | Select the GDE target platform. |
| `target` | `debug`, `release`, `template_debug`, `template_release` | `debug` | Select debug/release shape; `debug` maps to Godot `template_debug`, and `release` maps to `template_release`. |
| `arch` | `x86_64`, `x86_32`, `arm64`, `arm32` | `x86_64` | Select target architecture naming. |
| `precision` | `single`, `double` | `single` | Forwarded to `godot-cpp`. |
| `godot_cpp` | `delegated`, `external` | `delegated` | Select root handling for selected `godot-cpp` artifacts. |
| `platform_runtime_validate` | `yes`, `no` | `no` | Include selected platform-backed runtime validation artifacts. |
| `COMPDB_PATH` | path | `compile_commands.json` | Compilation database output path. |
| `use_mingw` | `yes`, `no`, `auto` | `auto` | Windows MinGW selection, mirrored to `godot-cpp` when applicable. |
| `use_llvm` | `yes`, `no`, `auto` | `auto` | Windows MinGW-LLVM selection, meaningful with MinGW. |
| `mingw_prefix` | path | empty | Optional MinGW installation prefix forwarded to `godot-cpp`. |
| `windows_mingw_static_runtime` | `auto`, `yes`, `no` | `auto` | Windows MinGW GDE static-runtime link mode. `auto` enables it for Windows MinGW GDE builds. |
| `warnings_as_errors` | `yes`, `no` | `no` | Treat compiler warnings as errors. |
| `android_api_level` | Android API level | `24` | Android GDE NDK Clang target API level. |
| `ndk_version` | Android NDK version | `28.1.13356709` | NDK version used with `ANDROID_HOME` / `ANDROID_SDK_ROOT` for Android GDE builds. |
| `ANDROID_HOME` | path | process env fallback | Optional Android SDK root for Android GDE builds. |

Assignment-style variables outside this declared public set are rejected by
`SConstruct`.

---

## 4. Build aliases and default build behaviour

### `all`

`all` is the default build target. It builds the selected build families,
controlled by:

- `gde=yes|no`
- `maintainer_tools=yes|no`
- `platform_runtime_validate=yes|no`

### `maintainer_tools`

Builds host-native deterministic smoke/verifier/benchmark tools. These tools are
not platform-scoped by `platform=<...>`. They are built by default because
`maintainer_tools=yes`, and can be skipped with `maintainer_tools=no`.

Useful forms:

```sh
scons maintainer_tools
scons gde=no
```

### `gde`

Builds the selected CamBANG GDE/plugin artifact family. Selection is controlled
by `platform`, `target`, `arch`, and `precision`. With no `platform` supplied,
CamBANG uses the host-detected platform.

Example Windows MinGW GDE-only build:

```sh
scons gde use_mingw=yes mingw_prefix=/c/Compilers/mingw64
```

### `godot_cpp`

Models the selected root-level `thirdparty/godot-cpp` outputs. With the default
`godot_cpp=delegated`, the root build delegates selected `godot-cpp`
construction to `thirdparty/godot-cpp`. With `godot_cpp=external`, the root
build does not enter the `thirdparty/godot-cpp` SCons project and instead
requires the selected generated header and static library to already exist. This
alias remains most important for selected clean behaviour.

### `platform_runtime_validate`

Builds selected platform runtime validation artifacts only. Normal build
inclusion is controlled by `platform_runtime_validate=yes`. The currently
implemented runtime validator is the Windows Media Foundation validator.

### `cambang`, `gde_all`, and `build_all`

- `cambang` is primarily an ownership-wide CamBANG clean alias.
- `gde_all` is a clean-only utility alias for all known CamBANG GDE object/output
  paths across declared platform/target/arch/precision combinations.
- `build_all` is a compatibility alias to `all` for build/clean convenience.

---

## 5. Clean aliases and clean scope

Root clean does **not** currently delegate a recursive clean inside
`thirdparty/godot-cpp`. Internal `thirdparty/godot-cpp/*.o` files may remain
after root `scons -c`; this is expected under the current root-modelled clean
design.

| Command | Scope |
|---|---|
| `scons -c` | Same practical scope as `scons -c all`: CamBANG-owned outputs plus selected root-modelled `godot_cpp` outputs in `godot_cpp=delegated`; preserves selected `godot-cpp` outputs in `godot_cpp=external`. |
| `scons -c all` | Full selected clean: `cambang` clean plus selected `godot_cpp` clean in `godot_cpp=delegated`; preserves selected `godot-cpp` outputs in `godot_cpp=external`. |
| `scons -c build_all` | Compatibility clean matching `all`. |
| `scons -c cambang` | CamBANG-owned outputs only: maintainer tools, all safely identifiable CamBANG GDE outputs/object paths, platform runtime validation artifacts, and `COMPDB_PATH`; preserves `thirdparty/godot-cpp`. |
| `scons -c maintainer_tools` | Maintainer-tool executables and `out/maintainer_tools_obj` only. |
| `scons -c gde` | Selected GDE object tree and selected plugin artifact only; preserves maintainer tools, `thirdparty/godot-cpp`, and `COMPDB_PATH`. |
| `scons -c gde platform=<platform>` | Selected platform GDE clean; clean-safe for all declared platforms, including unimplemented provider platforms. |
| `scons -c gde_all` | All known CamBANG GDE object/output paths; does not clean maintainer tools, `godot_cpp`, platform runtime validation, or `COMPDB_PATH`. |
| `scons -c godot_cpp` | Selected root-modelled `godot-cpp` generated-header sentinel and selected static library only. |
| `scons -c platform_runtime_validate` | Selected platform runtime validation artifacts only. |
| `scons -c platform=<android|linux|macos|ios|web>` | Clean-safe even though normal builds for those platforms currently fail for missing provider implementation. |

---

## 6. Platform/provider mapping

`platform=<...>` selects the GDE target platform. It does **not** select the
host platform for maintainer tools.

Declared GDE platforms and provider families:

| Platform | Provider family | Source location | Current normal build support |
|---|---|---|---|
| `windows` | `windows_mediafoundation` | `src/imaging/platform/windows` | Implemented dev-accelerator path. |
| `android` | `android_camera2` | `src/imaging/platform/android` | Not yet implemented. |
| `linux` | `linux_v4l2` | `src/imaging/platform/linux` | Not yet implemented. |
| `macos` | `apple_avfoundation` | `src/imaging/platform/apple` | Not yet implemented. |
| `ios` | `apple_avfoundation` | `src/imaging/platform/apple` | Not yet implemented. |
| `web` | `web_getusermedia` | `src/imaging/platform/web` | Not yet implemented. |

Windows is currently the only implemented platform-backed GDE provider path.
For declared platforms whose provider family is not implemented yet, non-clean
GDE builds are still allowed to produce a synthetic-capable artifact. In those
artifacts no platform-backed provider sources are compiled, no `CAMBANG_PROVIDER_*`
macro is defined, and `platform_backed` mode is unavailable in the build. A
default `CamBANGServer.start()` or explicit platform-backed start must therefore
fail visibly at runtime through the provider capability path instead of silently
falling back. Clean mode remains safe for all declared platforms.

GDE builds always include `SyntheticProvider` support and define
`CAMBANG_ENABLE_SYNTHETIC=1`. Synthetic is an alternate runtime mode of the
single provider instance compiled into GDE builds, not a second simultaneous
provider instance or multi-provider arbitration model. `StubProvider` is built
where needed for host-native deterministic maintainer coverage; it is not a
public production GDE fallback provider.

---

## 7. `godot-cpp` delegation and external consumption

The repo-root `SConstruct` has two `godot-cpp` artifact modes. The default
`godot_cpp=delegated` is the one-command path: root SCons invokes
`python -m SCons -C thirdparty/godot-cpp ...` for the selected
`platform`/`target`/`arch`/`precision` tuple before compiling CamBANG's GDE
objects. `godot_cpp=external` is a narrower developer mode for target switching:
root SCons never invokes the delegated sub-build and instead consumes artifacts
that the developer prepared separately.

CamBANG's own GDE object directories and plugin binaries are separated by
`platform`, Godot target, `arch`, and `precision`, for example
`out/gde_obj/<platform>/<godot_target>/<arch>/<precision>`. The single
`thirdparty/godot-cpp` checkout still has selected-platform mutable generated and
SCons state, so alternating delegated Windows and Android builds can cause
`godot-cpp` regeneration/rebuild churn. External mode trades one-command
convenience for faster target switching and user-managed `godot-cpp` freshness.

The root model tracks two selected `godot-cpp` outputs:

- generated-header sentinel:
  `thirdparty/godot-cpp/gen/include/godot_cpp/core/ext_wrappers.gen.inc`
- selected static library, for example:
  - Windows MinGW:
    `thirdparty/godot-cpp/bin/libgodot-cpp.windows.template_debug.x86_64.a`
  - Windows MSVC/non-MinGW:
    `thirdparty/godot-cpp/bin/libgodot-cpp.windows.template_debug.x86_64.lib`

In delegated mode, GDE objects depend on the delegated `godot_cpp` build before
compiling Godot-facing sources that include generated `godot-cpp` headers, and
the GDE link depends on the delegated static library. In external mode, the root
build fails early if either selected artifact is missing and prints the matching
`python -m SCons -C thirdparty/godot-cpp ...` preparation command. External mode
links and compiles against the existing files without an AlwaysBuild action that
re-enters `thirdparty/godot-cpp`.

Root clean only removes the selected root-modelled `godot-cpp` outputs; it does
not deep-clean every `godot-cpp` internal object. In external mode, broad root
cleans preserve `thirdparty/godot-cpp` outputs; use the explicit selected
`godot_cpp` clean alias only when you intentionally want to remove the selected
root-modelled generated header and static library.

---

## 8. Compilation database / IDE integration

The repository contains the SCons-native compilation database tool at:

```text
site_scons/site_tools/compdb.py
```

`COMPDB_PATH` is a declared supported variable. Its default output is:

```text
compile_commands.json
```

Behaviour:

- compile actions are wrapped and recorded for C/C++ translation units
- the database is written when compile actions run
- no-op builds preserve the existing database
- `COMPDB_PATH=<path>` writes to a custom location

Clean behaviour:

- `scons -c cambang`, `scons -c all`, and plain `scons -c` clean `COMPDB_PATH`
- `scons -c gde`, `scons -c maintainer_tools`,
  `scons -c platform_runtime_validate`, `scons -c godot_cpp`, and
  `scons -c gde_all` do not clean `COMPDB_PATH`

This supports tools such as `clangd`, CLion, and other language servers using
`compile_commands.json`.

---

## 9. Windows / MinGW local workflow

Prerequisites:

- Godot 4.5.x Windows editor
- MSYS2 MinGW x64 environment or equivalent MinGW-w64 toolchain
- `scons`
- `thirdparty/godot-cpp` submodule initialised and pinned to the intended Godot
  minor version

Recommended command on the current Windows development machine:

```sh
scons use_mingw=yes mingw_prefix=/c/Compilers/mingw64
```

Shorter acceptable form if `use_mingw=auto` resolves correctly:

```sh
scons mingw_prefix=/c/Compilers/mingw64
```

GDE-only build:

```sh
scons gde use_mingw=yes mingw_prefix=/c/Compilers/mingw64
```

Maintainer-tools-only build:

```sh
scons gde=no
scons maintainer_tools
```

Skip maintainer tools while building the selected GDE family:

```sh
scons maintainer_tools=no use_mingw=yes mingw_prefix=/c/Compilers/mingw64
```

`mingw_prefix` is important because delegated `godot-cpp` MinGW discovery can
otherwise find Git for Windows' bundled MinGW path. Passing
`mingw_prefix=/c/Compilers/mingw64` steers `godot-cpp` to the intended compiler.

---

## 10. Windows / MinGW runtime linkage

Windows MinGW GDE builds default to static MinGW compiler/runtime linkage for
the CamBANG plugin DLL. The normal Windows MinGW GDE build therefore remains:

```sh
scons gde platform=windows use_mingw=yes mingw_prefix=/c/Compilers/mingw64
```

With the default `windows_mingw_static_runtime=auto`, that build links MinGW
compiler/runtime support into the CamBANG GDE DLL. This keeps the Godot project
payload to one Windows native library beside the `.gdextension` descriptor. It
does not attempt to statically link Windows system or media APIs; those remain
ordinary OS imports.

Validate the resulting DLL import table when changing toolchain, target, or
packaging logic. The expected result is that these MinGW runtime DLLs are absent
from the import table:

- `libstdc++-6.dll`
- `libgcc_s_seh-1.dll`
- `libwinpthread-1.dll`

Example inspection command from Git Bash / MinGW:

```sh
objdump -p tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll | grep "DLL Name"
```

If a local toolchain cannot link or load with static MinGW runtime support, use
the explicit fallback:

```sh
scons gde platform=windows use_mingw=yes mingw_prefix=/c/Compilers/mingw64 windows_mingw_static_runtime=no
```

That fallback may require the MinGW runtime DLLs listed above beside the
extension DLL or available on `PATH`.

Windows GDE builds intentionally suppress SCons' generated import-library
output (`libcambang.windows.<target>.<arch>.a` for MinGW). That file is a
link-time developer artifact for native consumers of a DLL; Godot loads the GDE
DLL through the `.gdextension` descriptor and does not need the import library
beside the plugin.

---

## 11. Platform runtime validation

Platform-backed runtime validation is separate from deterministic maintainer
tools. It is explicitly enabled with:

```sh
scons platform_runtime_validate=yes platform=windows
```

The current implemented validator is Windows Media Foundation runtime validation.
It is a real-OS/API visibility check and may depend on local hardware, drivers,
and negotiated formats. It is not a replacement for deterministic provider
contract verification.

---

## 12. Object-tree separation

Current root-modelled object/output locations include:

- maintainer tools object tree:
  `out/maintainer_tools_obj`
- GDE object tree:
  `out/gde_obj/<platform>/<godot_target>/<arch>/<precision>`
- Windows debug GDE artifact example:
  `tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll`
- selected root-modelled `godot-cpp` generated-header sentinel:
  `thirdparty/godot-cpp/gen/include/godot_cpp/core/ext_wrappers.gen.inc`
- selected Windows MinGW `godot-cpp` static library example:
  `thirdparty/godot-cpp/bin/libgodot-cpp.windows.template_debug.x86_64.a`
- selected Windows MSVC/non-MinGW `godot-cpp` static library example:
  `thirdparty/godot-cpp/bin/libgodot-cpp.windows.template_debug.x86_64.lib`

---

## 13. Existing Godot/GDE scaffolding notes

### `.gdextension` encoding requirement

Godot's `.gdextension` parser does not tolerate a UTF-8 BOM. Ensure the file is
UTF-8 without BOM and begins with `[configuration]` at byte 0.

### Entry-point signature and method visibility

For Godot 4.x + `godot-cpp`, the entry signature uses the current
`GDExtensionInterfaceGetProcAddress` / `GDExtensionInitialization` form. Do not
use legacy signatures.

`_enter_tree()` / `_exit_tree()` overrides in dev-node scaffolding must be
public if binding code needs to take their method pointers during registration.

### Windows Media Foundation dev accelerator

The Windows platform-backed path is currently a Media Foundation dev accelerator,
not the final release Windows provider strategy. It remains useful for platform
visibility and local hardware checks, but release-provider work must still decide
negotiation policy, conversion policy, callback drain guarantees, teardown
safety, GPU/native presentation paths, still capture, multi-stream, rig support,
and release-grade validation criteria.

### Windows macro collision: `OPAQUE`

Windows headers brought in via `windows.h` may define a macro named `OPAQUE`.
Avoid Windows-macro-prone identifiers such as `OPAQUE`, `ERROR`, `DELETE`, `IN`,
and `OUT` as unqualified enum members in shared headers. Prefer prefixed names
such as `DOMAIN_OPAQUE`.

### Development scenes and boundary checks

Typical scaffolding scenes include extension load / lifecycle smoke scenes and
deterministic boundary-abuse scenes in the Godot test project. Current
Godot-side boundary verification is documented in:

```text
docs/dev/maintainer_tools.md
```

---

## 14. Scope boundary

This document governs:

- build-system direction
- scaffolding and toolchain behaviour
- local developer workflow
- repeatable build-loop expectations

It does **not** govern:

- runtime architecture
- provider lifecycle contract
- core threading model
- resource ownership rules

Those remain defined in canonical architecture documents.
