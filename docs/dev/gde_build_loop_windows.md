# CamBANG – GDE Build Loop (Windows / MinGW)

Status: Working Note (dev-only)  
Target: Godot 4.5.x (Windows editor), MinGW-w64 toolchain

This document describes the repeatable local loop for building and running the
temporary GDExtension scaffolding (CamBANGDevNode) on Windows.

---

## Prerequisites

- Godot 4.5.x Windows editor installed
- MSYS2 MinGW x64 environment installed
- `scons` available in the MSYS2 MinGW shell
- `thirdparty/godot-cpp` submodule initialised and pinned

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
