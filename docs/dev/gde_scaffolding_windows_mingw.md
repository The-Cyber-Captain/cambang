# CamBANG – GDE Scaffolding (Windows / MinGW Notes)

Branch: `build-phase-gde-1`  
Target: Godot 4.5+

This document records non-obvious integration details discovered during
initial GDExtension scaffolding on Windows using MinGW.

These are structural/tooling constraints, not temporary hacks.

---

## 1. SCons Object Collisions (VariantDir Required)

Both the smoke harness and GDE build compile the same `src/` sources
with different flags.

Without separation, SCons attempts to emit identical object paths and fails
with an error similar to:

```
Two environments with different actions were specified for the same target
```

### Solution

- Use `VariantDir` for smoke and GDE builds.
- Keep object trees separate (e.g. `out/smoke_obj`, `out/gde_obj`).

This is mandatory for multi-target builds sharing sources.

---

## 2. godot-cpp Delegation Strategy

The root `SConstruct` delegates to the `thirdparty/godot-cpp` submodule.

Important facts:

- godot-cpp must generate headers (`gen/include/*`) before extension compilation.
- The extension build must depend on header generation, not only the static library.
- A generated header (e.g. `global_constants.hpp`) is used as a dependency sentinel.

Do not rely solely on linking against the static library to imply header generation.

---

## 3. Windows / MinGW Runtime Behavior

MinGW-built DLLs commonly depend on runtime DLLs such as:

- `libstdc++-6.dll`
- `libgcc_s_seh-1.dll`
- `libwinpthread-1.dll`

If Godot is launched outside the MSYS2 MinGW environment, Windows loader may report:

```
Error 126: The specified module could not be found
```

This typically means a dependent runtime DLL is missing (not the plugin DLL itself).

### Fix Options

**A) Copy runtime DLLs next to the extension DLL**

```sh
cp /mingw64/bin/libstdc++-6.dll tests/cambang_gde/bin/
cp /mingw64/bin/libgcc_s_seh-1.dll tests/cambang_gde/bin/
cp /mingw64/bin/libwinpthread-1.dll tests/cambang_gde/bin/
```

**B) Launch Godot from MSYS2 with PATH set**

```sh
export PATH=/mingw64/bin:$PATH
"/c/path/to/Godot_v4.5.1-stable_win64.exe"
```
### Media Foundation provider (MinGW) – link + visibility notes

When building with:

- `provider=windows_mediafoundation`

under MinGW, additional system libraries are required at link time:

- `mf` (required for `MFEnumDeviceSources`)
- `mfplat`
- `mfreadwrite`
- `mfuuid`
- `ole32`
- `uuid` (required for `GUID_NULL` resolution on MinGW)

If these are omitted, link-time failures will occur.

#### Visibility-phase constraints

During the initial Windows visibility phase:

- `MF_READWRITE_DISABLE_CONVERTERS` is enabled.
- Only native camera output types are selectable.
- Many consumer cameras expose only YUV-native formats (e.g. NV12, YUY2, MJPG).

If no RGB32-like subtype (`MFVideoFormat_RGB32`, `MFVideoFormat_ARGB32`, etc.)
is advertised natively, visible pixels will not appear. This is expected behaviour
and does not indicate a failure of the provider or build configuration.

For the development-phase record and acceptance/drop counter interpretation, see:

- `docs/dev/windows_mf_visibility_phase.md`

#### Windows macro collision: OPAQUE

Windows headers included via windows.h (often wingdi.h) define a macro named OPAQUE (commonly 2).
If CamBANG headers define enum members named OPAQUE and are included after windows.h, compilation fails with errors like “expected identifier before numeric constant”.

Policy: avoid Windows-macro-prone identifiers (OPAQUE, ERROR, DELETE, IN, OUT, etc.) as unqualified enum members in shared/provider-contract headers. Prefer prefixed names like DOMAIN_OPAQUE.

---

## 4. `.gdextension` Encoding Requirement (No BOM)

Godot’s `.gdextension` parser does not tolerate a UTF-8 BOM.

If the file begins with:

```
EF BB BF
```

Godot may report:

```
configuration/entry_symbol missing
```

### Ensure

- Encoding is UTF-8
- **No BOM**
- File begins with `[configuration]` at byte 0

---

## 5. Entry Point Signature (Godot 4.x)

For Godot 4.x + godot-cpp, the entry signature is:

```cpp
extern "C" GDExtensionBool GDE_EXPORT cambang_gdextension_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization
);
```

Do not use legacy `GDExtensionInterface*` signatures.

---

## 6. `_enter_tree()` / `_exit_tree()` Must Be Public

godot-cpp binds virtual methods using pointer access during registration.

If overrides are declared `protected`, compilation can fail because the
binding code cannot take the method pointer.

Lifecycle overrides in `CamBANGDevNode` must be declared `public`.

---

## 7. Single-Instance Guard (Scaffolding Phase Only)

`CamBANGDevNode` enforces one live instance:

- First node starts `CoreRuntime`
- Second logs and `queue_free()`s itself

This prevents undefined multi-runtime state during early integration.

This is scaffolding behavior, not final API design.

---

## 8. Integration Checkpoint Achieved

- Extension loads in Godot 4.5 on Windows (MinGW build)
- Runtime starts and stops correctly via lifecycle hooks
- Single-instance guard functions as intended
- Deterministic SCons build (including godot-cpp delegation)
- Repo hygiene maintained via `.gitignore` + per-project Godot `.gitignore`
