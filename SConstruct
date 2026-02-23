#!/usr/bin/env python
# CamBANG (core spine) - temporary SCons entrypoint.
#
# Goals:
# - Provide a deterministic build path for the current "core spine smoke" harness.
# - Keep output artifacts out of the repository root (everything goes under ./out/).
# - Mirror Godot/godot-cpp conventions where practical (platform/target/arch/precision).
# - Avoid hard-coding compiler flags that are toolchain-specific (MSVC vs GCC/Clang).
#
# This file is intentionally minimal and will evolve once the Godot-facing GDExtension build
# is introduced. Until then, we compile only the smoke harness and its core/stub dependencies.

import os
import sys

from SCons.Script import (
    AlwaysBuild,
    Alias,
    BoolVariable,
    Default,
    Environment,
    EnumVariable,
    Exit,
    Glob,
    Help,
    Variables,
)

def _detect_platform():
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"

def _looks_like_msys_or_bash():
    # Heuristic only; users can always override with use_mingw=...
    env = os.environ
    if env.get("MSYSTEM") or env.get("MINGW_PREFIX") or env.get("MSYS2_PATH_TYPE"):
        return True
    sh = env.get("SHELL", "")
    if "bash" in sh.lower():
        return True
    return False

vars = Variables()
vars.Add(EnumVariable(
    "platform",
    "Target platform. For now this controls host-toolchain selection and output naming only.",
    _detect_platform(),
    allowed_values=["windows", "linux", "macos", "android"],
))
vars.Add(EnumVariable(
    "target",
    "Build target.",
    "debug",
    allowed_values=["debug", "release"],
))
vars.Add(EnumVariable(
    "arch",
    "Target architecture (convention-compatible knob; host builds only for now).",
    "x86_64",
    allowed_values=["x86_64", "x86_32", "arm64", "arm32"],
))
vars.Add(EnumVariable(
    "precision",
    "Floating-point precision (convention-compatible knob; not used by the core spine yet).",
    "single",
    allowed_values=["single", "double"],
))
vars.Add(EnumVariable(
    "use_mingw",
    "Windows toolchain selection: auto/yes/no. Mirrors Godot SCons intent.",
    "auto",
    allowed_values=["auto", "yes", "no"],
))
vars.Add(EnumVariable(
    "use_llvm",
    "Windows MinGW-LLVM selection: auto/yes/no (meaningful only with use_mingw=yes).",
    "auto",
    allowed_values=["auto", "yes", "no"],
))
vars.Add(BoolVariable(
    "warnings_as_errors",
    "Treat warnings as errors.",
    False,
))

# Resolve variables (needed for Help output).
tmp_env = Environment(variables=vars)
Help(vars.GenerateHelpText(tmp_env))

if tmp_env["platform"] == "android":
    # We intentionally do not implement Android cross-compilation yet; this will be introduced
    # alongside the real GDExtension build and will align with godot-cpp's Android conventions.
    print("ERROR: platform=android is not yet supported for this temporary smoke build.")
    Exit(1)

# Toolchain selection (Windows only).
tools = None
if tmp_env["platform"] == "windows":
    use_mingw = tmp_env["use_mingw"]
    if use_mingw == "auto":
        use_mingw = "yes" if _looks_like_msys_or_bash() else "no"

    if use_mingw == "yes":
        # Prefer MinGW in MSYS/Git-Bash style environments.
        tools = ["mingw"]
    else:
        tools = None  # Let SCons pick (likely MSVC if available)

env = Environment(variables=vars, tools=tools)
env.Append(CPPPATH=["src"])

# Identify toolchain kind.
cxx = str(env.get("CXX", "")).lower()
is_msvc = ("cl" in cxx) or env.get("MSVC_VERSION")

if is_msvc:
    # MSVC flags (avoid GCC/Clang flags like -Wextra).
    env.Append(CXXFLAGS=["/std:c++20", "/W4"])
    if env["warnings_as_errors"]:
        env.Append(CXXFLAGS=["/WX"])
    if env["target"] == "debug":
        env.Append(CXXFLAGS=["/Zi", "/Od"])
        env.Append(LINKFLAGS=["/DEBUG"])
    else:
        env.Append(CXXFLAGS=["/O2"])
else:
    # GCC/Clang (MinGW, Linux, macOS).
    env.Append(CXXFLAGS=["-std=gnu++20", "-Wall", "-Wextra", "-Wpedantic"])
    if env["warnings_as_errors"]:
        env.Append(CXXFLAGS=["-Werror"])
    if env["target"] == "debug":
        env.Append(CXXFLAGS=["-g", "-O0"])
    else:
        env.Append(CXXFLAGS=["-O2"])

    # Threading (no-op for MSVC; required for GCC/Clang on many platforms).
    env.Append(CCFLAGS=["-pthread"])
    env.Append(LINKFLAGS=["-pthread"])

# IDE smoke harness uses a few intentionally test-only hooks guarded behind this define.
env.Append(CPPDEFINES=["CAMBANG_INTERNAL_IDE_SMOKE=1"])

print("CamBANG SCons (smoke) configuration:")
print(f"  platform={env['platform']} target={env['target']} arch={env['arch']} precision={env['precision']}")
print(f"  toolchain={'msvc' if is_msvc else 'gcc/clang'} CXX={env.get('CXX')}")
if env['platform'] == 'windows':
    print(f"  use_mingw={env['use_mingw']} use_llvm={env['use_llvm']}")

sources = []
sources += Glob("src/core/*.cpp")
sources += Glob("src/provider/*.cpp")
sources += Glob("src/provider/stub/*.cpp")
sources += ["ide/core_spine_smoke.cpp"]

out_dir = "out"
if not os.path.isdir(out_dir):
    try:
        os.makedirs(out_dir)
    except OSError:
        pass

prog = env.Program(target=os.path.join(out_dir, "core_spine_smoke"), source=sources)

Alias("smoke", prog)
AlwaysBuild("smoke")
Default("smoke")
