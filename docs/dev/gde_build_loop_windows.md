# CamBANG – GDE Build Loop (Windows / MinGW)

Status: Working Note (dev-only)  
Target: Godot 4.5.x (Windows editor), MinGW-w64 toolchain

This document describes the repeatable local loop for building and running the
temporary GDExtension scaffolding (CamBANGDevNode) on Windows.

This document describes temporary scaffolding workflow and does not define
release build procedures.

---

## Prerequisites

- Godot 4.5.x Windows editor installed
- MSYS2 MinGW x64 environment installed
- `scons` available in the MSYS2 MinGW shell
- `thirdparty/godot-cpp` submodule initialised and pinned. The submodule must match the intended Godot minor version (4.5.x).

---

## Note on IDE indexing (CLion / clangd)

This repo uses **SCons-only**. IDEs should invoke SCons for builds.

The `gde` build also generates a compilation database for indexing:

- `compile_commands.json` at repo root

The file is produced by the repo tool:

- `site_scons/site_tools/compdb.py`

Because the database is recorded from real compile actions, it is written at the
end of a build that performs compilation.

---

## 1. Build the extension

From an **MSYS2 MinGW x64** shell at repo root:

```sh
scons -c
scons -j 8 gde platform=windows target=template_debug arch=x86_64 use_mingw=yes
```

(Adjust `-j` to match your logical CPU core count.)

Expected outputs:

- `tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll`
- `tests/cambang_gde/bin/libcambang.windows.template_debug.x86_64.a`

Also generated (IDE support):

- `compile_commands.json` at repo root

### Refreshing the compilation database

Re-run the same `scons ... gde ...` command after changing build flags, include paths,
or adding/removing source files. The database is written at the end of the build.

### Optional: write compile_commands.json elsewhere

To place the file somewhere other than repo root:

```sh
scons -j 8 gde platform=windows target=template_debug arch=x86_64 use_mingw=yes COMPDB_PATH=out/compile_commands.json
```

---

## 2. Ensure runtime DLLs are resolvable (Windows loader)

If Godot reports:

```
Error 126: The specified module could not be found
```

This typically means MinGW runtime DLLs are not on the PATH for the Godot process.

### Option A (recommended for this test project)

Copy the required runtime DLLs next to the extension DLL:

```sh
cp /mingw64/bin/libstdc++-6.dll tests/cambang_gde/bin/
cp /mingw64/bin/libgcc_s_seh-1.dll tests/cambang_gde/bin/
cp /mingw64/bin/libwinpthread-1.dll tests/cambang_gde/bin/
```

### Option B

Launch Godot from the MSYS2 shell so `/mingw64/bin` is on PATH:

```sh
export PATH=/mingw64/bin:$PATH
"/c/path/to/Godot_v4.5.1-stable_win64.exe"
```

---

## 3. Open the Godot test project

Open:

```
tests/cambang_gde/project.godot
```

Confirm:

```
tests/cambang_gde/bin/cambang_dev.gdextension
```

exists and is UTF-8 without BOM.

---

## 4. Run the scenes

### 00_extension_load.tscn

Should log:

```
[CamBANGDevNode] Starting CoreRuntime...
[CamBANGDevNode] Stopping CoreRuntime...
```

### 10_lifecycle_smoke.tscn

Second instance should log and free itself:

```
Another instance already live; freeing this node.
```

This validates the single-instance guard during scaffolding.


------------------------------------------------------------------------

## Running Godot Boundary Abuse Scenes

The Godot development project includes deterministic boundary
verification scenes.

These scenes verify the behaviour of the CamBANG runtime from the
Godot boundary.

Typical scenes:

- `60_restart_boundary_abuse`
- `61_tick_bounded_coalescing_abuse`
- `62_snapshot_polling_immutability_abuse`
- `63_snapshot_observer_minimal`

Expected final output from each scene:
