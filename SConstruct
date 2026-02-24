#!/usr/bin/env python
# CamBANG - SCons entrypoint (smoke + GDE scaffolding)
#
# Goals (current branch: build-phase-windows-provider-min-1):
# - Keep core smoke executable deterministic and fast (opt-in; stub-provider-only).
# - Build GDExtension:
#     - delegate godot-cpp build to its SCons
#     - build a shared lib that links core + selected provider + godot glue
#     - drop output into tests/cambang_gde/bin/
# - Avoid SCons object collisions between smoke and gde builds.
#
# NOTE on terminology:
# - "smoke" here means an internal executable built from repo sources, intended to
#   validate core invariants quickly. It is not part of the GDExtension artifact.

import os
import sys

scons_exe = sys.argv[0]
python_exe = sys.executable

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

def _core_target_for_flags(t: str) -> str:
    # Map Godot template targets to our debug/release flag logic.
    if t == "template_debug":
        return "debug"
    if t == "template_release":
        return "release"
    return t

def _godot_target(t: str) -> str:
    # godot-cpp expects template_debug/template_release
    if t == "debug":
        return "template_debug"
    if t == "release":
        return "template_release"
    return t

def _godot_cpp_lib_path(platform: str, target: str, arch: str, is_msvc: bool) -> str:
    # godot-cpp outputs: thirdparty/godot-cpp/bin/libgodot-cpp.<platform>.<target>.<arch>.(a|lib)
    ext = "lib" if (platform == "windows" and is_msvc) else "a"
    return os.path.join("thirdparty", "godot-cpp", "bin", f"libgodot-cpp.{platform}.{target}.{arch}.{ext}")

vars = Variables()
vars.Add(EnumVariable(
    "platform",
    "Target platform. Controls host-toolchain selection and output naming.",
    _detect_platform(),
    allowed_values=["windows", "linux", "macos", "android"],
))

# Provider selection applies to GDE build only.
vars.Add(EnumVariable(
    "provider",
    "Provider backend to build into the GDE (dev accelerator selection).",
    "stub",
    allowed_values=["stub", "windows_mediafoundation"],
))

vars.Add(EnumVariable(
    "target",
    "Build target. Accepts debug/release and Godot template_debug/template_release.",
    "debug",
    allowed_values=["debug", "release", "template_debug", "template_release"],
))
vars.Add(EnumVariable(
    "arch",
    "Target architecture (convention-compatible knob).",
    "x86_64",
    allowed_values=["x86_64", "x86_32", "arm64", "arm32"],
))
vars.Add(EnumVariable(
    "precision",
    "Floating-point precision knob (forwarded to godot-cpp).",
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
vars.Add(BoolVariable(
    "smoke",
    "Build core smoke executable (stub-provider-only). Not part of the GDExtension artifact.",
    False,
))

tmp_env = Environment(variables=vars)
Help(vars.GenerateHelpText(tmp_env))

if tmp_env["platform"] == "android":
    print("ERROR: platform=android is not yet supported by this temporary build entrypoint.")
    Exit(1)

# Toolchain selection (Windows only).
tools = None
if tmp_env["platform"] == "windows":
    use_mingw = tmp_env["use_mingw"]
    if use_mingw == "auto":
        use_mingw = "yes" if _looks_like_msys_or_bash() else "no"
    if use_mingw == "yes":
        tools = ["mingw"]
    else:
        tools = None  # Let SCons pick (likely MSVC if available)

env = Environment(variables=vars, tools=tools)
env.Append(CPPPATH=["src"])

# ---------------------------------------------------------------------------
# IDE support: generate compile_commands.json for CLion/clangd
# ---------------------------------------------------------------------------
env.Tool("compdb", toolpath=["site_scons/site_tools"])

# Identify toolchain kind.
cxx = str(env.get("CXX", "")).lower()
is_msvc = ("cl" in cxx) or env.get("MSVC_VERSION")

core_target = _core_target_for_flags(env["target"])
godot_target = _godot_target(env["target"])

# Base compiler/link flags
if is_msvc:
    env.Append(CXXFLAGS=["/std:c++20", "/W4"])
    if env["warnings_as_errors"]:
        env.Append(CXXFLAGS=["/WX"])
    if core_target == "debug":
        env.Append(CXXFLAGS=["/Zi", "/Od"])
        env.Append(LINKFLAGS=["/DEBUG"])
    else:
        env.Append(CXXFLAGS=["/O2"])
else:
    env.Append(CXXFLAGS=["-std=gnu++20", "-Wall", "-Wextra", "-Wpedantic"])
    if env["warnings_as_errors"]:
        env.Append(CXXFLAGS=["-Werror"])
    if core_target == "debug":
        env.Append(CXXFLAGS=["-g", "-O0"])
    else:
        env.Append(CXXFLAGS=["-O2"])

    # Threading (no-op for MSVC; required for GCC/Clang on many platforms).
    env.Append(CCFLAGS=["-pthread"])
    env.Append(LINKFLAGS=["-pthread"])

print("CamBANG SCons configuration:")
print(f"  platform={env['platform']} target={env['target']} (core_flags={core_target}, godot={godot_target}) arch={env['arch']} precision={env['precision']}")
print(f"  toolchain={'msvc' if is_msvc else 'gcc/clang'} CXX={env.get('CXX')}")
if env["platform"] == "windows":
    print(f"  use_mingw={env['use_mingw']} use_llvm={env['use_llvm']}")
print(f"  provider={env['provider']} smoke={'yes' if env['smoke'] else 'no'}")

# Output dirs
out_dir = "out"
if not os.path.isdir(out_dir):
    try:
        os.makedirs(out_dir)
    except OSError:
        pass

# Separate object trees to avoid action collisions between smoke and gde builds.
smoke_obj_dir = os.path.join(out_dir, "smoke_obj")
gde_obj_dir = os.path.join(out_dir, "gde_obj")

# ---------------------------------------------------------------------------
# Core smoke executable (opt-in; stub-provider-only)
# ---------------------------------------------------------------------------
if env["smoke"]:
    smoke_env = env.Clone()
    smoke_env.Append(CPPDEFINES=["CAMBANG_INTERNAL_SMOKE=1"])

    # Compile sources via a variant dir so smoke objects don't collide with gde objects.
    smoke_env.VariantDir(smoke_obj_dir, "src", duplicate=0)

    smoke_sources = []
    smoke_sources += Glob(os.path.join(smoke_obj_dir, "core", "*.cpp"))
    smoke_sources += Glob(os.path.join(smoke_obj_dir, "provider", "*.cpp"))
    smoke_sources += Glob(os.path.join(smoke_obj_dir, "provider", "stub", "*.cpp"))
    smoke_sources += ["src/smoke/core_spine_smoke.cpp"]

    smoke_prog = smoke_env.Program(
        target=os.path.join(out_dir, "core_spine_smoke"),
        source=smoke_sources,
    )

    smoke_alias = Alias("smoke", smoke_prog)
    AlwaysBuild(smoke_alias)
else:
    Alias("smoke", [])

# ---------------------------------------------------------------------------
# GDExtension scaffolding (alias: gde)
# ---------------------------------------------------------------------------

gde_env = env.Clone()

# Compile sources via a separate variant dir to avoid collisions with smoke.
gde_env.VariantDir(gde_obj_dir, "src", duplicate=0)

# Output location expected by the Godot test project.
gde_out_dir = os.path.join("tests", "cambang_gde", "bin")
if not os.path.isdir(gde_out_dir):
    try:
        os.makedirs(gde_out_dir)
    except OSError:
        pass

# Build godot-cpp (delegated to its own SCons).
godot_cpp_lib = _godot_cpp_lib_path(env["platform"], godot_target, env["arch"], is_msvc)
godot_cpp_libdir = os.path.join("thirdparty", "godot-cpp", "bin")
godot_cpp_libname = f"godot-cpp.{env['platform']}.{godot_target}.{env['arch']}"

godot_gen_header = os.path.join(
    "thirdparty", "godot-cpp", "gen", "include", "godot_cpp", "classes", "global_constants.hpp"
)

godot_cpp_build = env.Command(
    target=godot_gen_header,
    source=[],
    action=(
        f'cmd /c "set PROCESSOR_ARCHITECTURE=AMD64&& '
        f'"{python_exe}" -m SCons -C thirdparty/godot-cpp '
        f'platform={env["platform"]} target={godot_target} arch={env["arch"]} precision={env["precision"]}"'
    )
)

Alias("godot_cpp", godot_cpp_build)
AlwaysBuild("godot_cpp")

# Include dirs required by godot-cpp.
gde_env.Append(CPPPATH=[
    os.path.join("thirdparty", "godot-cpp"),
    os.path.join("thirdparty", "godot-cpp", "include"),
    os.path.join("thirdparty", "godot-cpp", "gen"),
    os.path.join("thirdparty", "godot-cpp", "gen", "include"),
    os.path.join("thirdparty", "godot-cpp", "gdextension"),
])

# Shared lib build needs PIC on non-MSVC toolchains.
if not is_msvc:
    gde_env.Append(CXXFLAGS=["-fPIC", "-fvisibility=hidden"])

# Sources: core + providers + Godot glue.
gde_sources = []
gde_sources += Glob(os.path.join(gde_obj_dir, "core", "*.cpp"))
gde_sources += Glob(os.path.join(gde_obj_dir, "provider", "*.cpp"))

# Provider backend selection (dev accelerator).
if env["provider"] == "stub":
    gde_sources += Glob(os.path.join(gde_obj_dir, "provider", "stub", "*.cpp"))
elif env["provider"] == "windows_mediafoundation":
    gde_sources += Glob(os.path.join(gde_obj_dir, "provider", "windows_mediafoundation", "*.cpp"))
    gde_env.Append(CPPDEFINES=["CAMBANG_PROVIDER_WINDOWS_MF=1"])
    if env["platform"] == "windows":
        # MinGW link set typically needs mf + uuid in addition to the usual MF libs.
        gde_env.Append(LIBS=["mf", "mfplat", "mfreadwrite", "mfuuid", "ole32", "uuid"])
else:
    print("Unknown provider:", env["provider"])
    Exit(1)

gde_sources += [
    os.path.join(gde_obj_dir, "godot", "module_init.cpp"),
    os.path.join(gde_obj_dir, "godot", "dev", "cambang_dev_node.cpp"),
    os.path.join(gde_obj_dir, "godot", "dev", "cambang_dev_frameview_node.cpp"),
]

# Output base name (SCons appends .dll/.so/.dylib automatically).
# These names are designed to match your cambang_dev.gdextension mapping.
if env["platform"] == "windows":
    # On Windows, SCons wants the target to include the .dll suffix explicitly.
    gde_filename = f"cambang.windows.{godot_target}.{env['arch']}.dll"
else:
    # On POSIX, keep a suffix-less base; SCons will add .so/.dylib appropriately.
    gde_filename = f"libcambang.{env['platform']}.{godot_target}.{env['arch']}"

gde_target = os.path.join(gde_out_dir, gde_filename)

# Link against godot-cpp static library.
# (We pass the full path; SCons will treat it as a library file.)
gde_env.Append(LIBPATH=[godot_cpp_libdir])
gde_env.Append(LIBS=[godot_cpp_libname])

gde_lib = gde_env.SharedLibrary(target=gde_target, source=gde_sources)
# Ensure all compilation steps wait for godot-cpp generation, not just the final link.
for n in gde_lib:
    gde_env.Depends(n, godot_cpp_build)

gde_alias = Alias("gde", gde_lib)
AlwaysBuild(gde_alias)

# Write compile_commands.json after building the 'gde' alias
from SCons.Script import AddPostAction
AddPostAction(gde_alias, env["COMPDB_WRITE_ACTION"])

# Default targets:
# - always build GDE scaffolding
# - additionally build smoke when smoke=1
if env["smoke"]:
    Default(["gde", "smoke"])
else:
    Default("gde")
