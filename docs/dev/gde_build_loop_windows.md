# CamBANG – GDE Build Loop (Windows / MinGW)

Status: Working Note (dev-only)
Target: Godot 4.5.x (Windows editor), MinGW-w64 toolchain

This document describes the repeatable local loop for building and running the
temporary GDExtension scaffolding (CamBANGDevNode) on Windows.

---

## Prerequisites

- Godot 4.5.x Windows editor installed
- MSYS2 MinGW x64 environment installed
- scons available in the MSYS2 MinGW shell
- thirdparty/godot-cpp submodule initialised and pinned

---

## 1. Build the extension

From an MSYS2 MinGW x64 shell at repo root:

scons -c
scons gde platform=windows target=template_debug arch=x86_64 use_mingw=yes

Expected outputs:
- tests/cambang_gde/bin/cambang.windows.template_debug.x86_64.dll
- tests/cambang_gde/bin/libcambang.windows.template_debug.x86_64.a

---

## 2. Ensure runtime DLLs are resolvable

If Godot reports Error 126, MinGW runtime DLLs are not on PATH.

Option A (recommended for this test project):

cp /mingw64/bin/libstdc++-6.dll tests/cambang_gde/bin/
cp /mingw64/bin/libgcc_s_seh-1.dll tests/cambang_gde/bin/
cp /mingw64/bin/libwinpthread-1.dll tests/cambang_gde/bin/

Option B: Launch Godot from MSYS2 shell with /mingw64/bin on PATH.

---

## 3. Open the Godot test project

Open:
tests/cambang_gde/project.godot

Confirm:
tests/cambang_gde/bin/cambang_dev.gdextension exists.

---

## 4. .gdextension must be UTF-8 without BOM

If you see:
configuration/entry_symbol missing

Check that the file does not begin with EF BB BF.

---

## 5. Run scenes

00_extension_load.tscn:
Should log runtime start/stop.

10_lifecycle_smoke.tscn:
Second instance should log and free itself.
