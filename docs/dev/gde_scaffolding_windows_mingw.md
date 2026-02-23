# CamBANG – GDE Scaffolding (Windows / MinGW Notes)

Branch: `build-phase-gde-1`  
Target: Godot 4.5+

This document records non-obvious integration details discovered during initial GDExtension scaffolding on Windows using MinGW.

These are structural/tooling constraints, not temporary hacks.

---

## 1. SCons Object Collisions (VariantDir Required)

Both the smoke harness and GDE build compile the same `src/` sources with different flags.

Without separation, SCons attempts to emit identical object paths and fails with an error like:

> Two environments with different actions were specified for the same target

**Solution**
- Use `VariantDir` for smoke and GDE builds.
- Keep object trees separate (e.g. `out/smoke_obj`, `out/gde_obj`).

This is mandatory for multi-target builds sharing sources.

---

## 2. godot-cpp Delegation Strategy

The root `SConstruct` delegates to the `thirdparty/godot-cpp` submodule.

Important facts:
- godot-cpp must generate headers (`gen/include/*`) before extension compilation.
- The extension build must depend on header generation, not only the static library.
- Target a generated header (e.g. `global_constants.hpp`) as a dependency sentinel.

Do not rely solely on linking against the static library to imply header generation.

---

## 3. Windows / MinGW Runtime Behavior

MinGW-built DLLs commonly depend on runtime DLLs such as:
- `libstdc++-6.dll`
- `libgcc_s_seh-1.dll`
- `libwinpthread-1.dll`

If Godot is launched outside the MSYS2 MinGW environment, Windows loader may report:

> Error 126: The specified module could not be found

This typically means a dependent runtime DLL is missing (not the plugin DLL itself).

**Fix options**
- **A)** Copy required runtime DLLs next to the extension DLL (in the Godot project `bin/` directory).
- **B)** Launch Godot from an MSYS2 MinGW shell with `/mingw64/bin` on `PATH`.

This is expected Windows loader behavior, not a CamBANG issue.

---

## 4. `.gdextension` Encoding Requirement (No BOM)

Godot’s `.gdextension` parser does not tolerate a UTF-8 BOM.

If the file begins with:

```
EF BB BF
```

Godot may report:

> configuration/entry_symbol missing

**Ensure**
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

If overrides are declared `protected`, compilation can fail (binding code cannot take the method pointer).

Therefore, lifecycle overrides in `CamBANGDevNode` must be declared `public`.

---

## 7. Single-Instance Guard (Scaffolding Phase Only)

`CamBANGDevNode` enforces one live instance:
- First node starts `CoreRuntime`
- Second logs and `queue_free()`s itself

This prevents undefined multi-runtime state during early integration.

This is scaffolding behavior, not the final API design.

---

## 8. Integration Checkpoint Achieved

- Extension loads in Godot 4.5 on Windows (MinGW build)
- Runtime starts and stops correctly via `_enter_tree()` / `_exit_tree()`
- Single-instance guard functions as intended
- Deterministic SCons build (including godot-cpp delegation)
- Repo hygiene is maintained via `.gitignore` + per-project Godot `.gitignore`
