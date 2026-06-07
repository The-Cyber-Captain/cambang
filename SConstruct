#!/usr/bin/env python
# CamBANG - SCons entrypoint
#
# Repo-root build contract:
# - deterministic host verification tools are enabled by default (verify=yes)
# - the GDExtension artifact is enabled by default (gde=yes)
# - platform=<...> selects the GDE target platform, not the host verifier platform
# - platform-backed runtime validation is explicit opt-in (platform_runtime_validate=yes)

import os
import sys

python_exe = sys.executable

from SCons.Script import (
    ARGUMENTS,
    AddPostAction,
    Alias,
    AlwaysBuild,
    BoolVariable,
    Default,
    Environment,
    EnumVariable,
    Exit,
    GetOption,
    Glob,
    Help,
    Variables,
)

ALLOWED_VARIABLES = {
    "gde",
    "verify",
    "platform",
    "target",
    "arch",
    "precision",
    "platform_runtime_validate",
    "COMPDB_PATH",
    "use_mingw",
    "use_llvm",
    "warnings_as_errors",
}

unknown_variables = sorted(set(ARGUMENTS) - ALLOWED_VARIABLES)
if unknown_variables:
    print("ERROR: Unknown CamBANG SCons variable(s): " + ", ".join(unknown_variables))
    print("Allowed variables: " + ", ".join(sorted(ALLOWED_VARIABLES)))
    Exit(1)


def _detect_host_platform():
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


def _processor_architecture_for_arch(arch: str) -> str:
    return {
        "x86_64": "AMD64",
        "arm64": "ARM64",
        "x86_32": "x86",
        "arm32": "ARM",
    }.get(arch, "AMD64")


def _command_process_env(host_platform: str, arch: str):
    process_env = os.environ.copy()
    if host_platform == "windows":
        process_env.setdefault("PROCESSOR_ARCHITECTURE", _processor_architecture_for_arch(arch))
    return process_env


def _core_target_for_flags(t: str) -> str:
    # Map Godot template targets to our debug/release flag logic.
    if t == "template_debug":
        return "debug"
    if t == "template_release":
        return "release"
    return t


def _godot_target(t: str) -> str:
    # godot-cpp expects template_debug/template_release.
    if t == "debug":
        return "template_debug"
    if t == "release":
        return "template_release"
    return t


def _godot_cpp_lib_path(platform: str, target: str, arch: str, windows_gde_toolchain) -> str:
    # godot-cpp outputs: thirdparty/godot-cpp/bin/libgodot-cpp.<platform>.<target>.<arch>.(a|lib)
    ext = "lib" if (platform == "windows" and windows_gde_toolchain["family"] == "msvc") else "a"
    return os.path.join("thirdparty", "godot-cpp", "bin", f"libgodot-cpp.{platform}.{target}.{arch}.{ext}")


def _tool_basename(value):
    return os.path.basename(str(value).replace("\\", "/")).lower()


def _toolchain_probe_text(env) -> str:
    return " ".join(str(env.get(name, "")) for name in ("CC", "CXX", "LINK", "SHLINK"))


def _looks_like_mingw_toolchain(env) -> bool:
    probe = _toolchain_probe_text(env).lower().replace("\\", "/")
    tool_basenames = {_tool_basename(env.get(name, "")) for name in ("CC", "CXX", "LINK", "SHLINK")}
    return (
        "mingw" in probe
        or "msys" in probe
        or "ucrt64" in probe
        or "clang64" in probe
        or bool(tool_basenames & {"gcc", "gcc.exe", "g++", "g++.exe", "ld", "ld.exe"})
    )


def _looks_like_mingw_llvm_toolchain(env) -> bool:
    probe = _toolchain_probe_text(env).lower().replace("\\", "/")
    tool_basenames = {_tool_basename(env.get(name, "")) for name in ("CC", "CXX", "LINK", "SHLINK")}
    return (
        "clang64" in probe
        or "mingw-clang" in probe
        or bool(tool_basenames & {"clang", "clang.exe", "clang++", "clang++.exe", "lld", "lld.exe", "ld.lld", "ld.lld.exe"})
    )


def _looks_like_msvc_toolchain(env) -> bool:
    probe = _toolchain_probe_text(env).lower().replace("\\", "/")
    tool_basenames = {_tool_basename(env.get(name, "")) for name in ("CC", "CXX", "LINK", "SHLINK")}
    return bool(env.get("MSVC_VERSION")) or "msvc" in probe or bool(tool_basenames & {"cl", "cl.exe", "link", "link.exe"})


def _windows_gde_toolchain_error(message, *, platform, env, artifact_ext, godot_cpp_toolchain_args):
    root_tools = _toolchain_probe_text(env) or "unknown"
    print("ERROR: " + message)
    print(f"  selected platform: {platform}")
    print(f"  root GDE compiler/linker/tool family: {root_tools}")
    print(f"  expected godot-cpp artifact extension: .{artifact_ext}")
    print("  delegated godot-cpp toolchain arguments: " + (" ".join(godot_cpp_toolchain_args) or "<none>"))
    print("  suggested action: use use_mingw=yes for MinGW-w64, use_mingw=yes use_llvm=yes for MinGW-LLVM, or use_mingw=no from an MSVC developer environment.")
    Exit(1)


def resolve_windows_gde_toolchain(env, *, platform, requested_use_mingw, requested_use_llvm, is_clean):
    actual_mingw = _looks_like_mingw_toolchain(env)
    actual_llvm = _looks_like_mingw_llvm_toolchain(env)
    actual_msvc = _looks_like_msvc_toolchain(env)

    if requested_use_mingw == "no":
        if requested_use_llvm == "yes" and not is_clean:
            _windows_gde_toolchain_error(
                "use_llvm=yes requires use_mingw=yes for Windows GDE builds.",
                platform=platform,
                env=env,
                artifact_ext="lib",
                godot_cpp_toolchain_args=["use_mingw=no"],
            )
        if not actual_msvc and not is_clean:
            _windows_gde_toolchain_error(
                "use_mingw=no selected MSVC ABI, but the root GDE toolchain is not confidently MSVC-family.",
                platform=platform,
                env=env,
                artifact_ext="lib",
                godot_cpp_toolchain_args=["use_mingw=no"],
            )
        return {
            "family": "msvc",
            "artifact_ext": "lib",
            "label": "MSVC",
            "godot_cpp_toolchain_args": ["use_mingw=no"],
        }

    if requested_use_mingw == "yes":
        family = "mingw-llvm" if (requested_use_llvm == "yes" or (requested_use_llvm == "auto" and actual_llvm)) else "mingw-gcc"
        args = ["use_mingw=yes"]
        if family == "mingw-llvm":
            args.append("use_llvm=yes")
        if not actual_mingw and not is_clean:
            _windows_gde_toolchain_error(
                "use_mingw=yes selected MinGW ABI, but the root GDE toolchain is not confidently MinGW-family.",
                platform=platform,
                env=env,
                artifact_ext="a",
                godot_cpp_toolchain_args=args,
            )
        if requested_use_llvm == "yes" and not actual_llvm and not is_clean:
            _windows_gde_toolchain_error(
                "use_llvm=yes selected MinGW-LLVM, but the root GDE toolchain is not confidently LLVM-family.",
                platform=platform,
                env=env,
                artifact_ext="a",
                godot_cpp_toolchain_args=args,
            )
        if requested_use_llvm == "no" and actual_llvm and not is_clean:
            _windows_gde_toolchain_error(
                "use_llvm=no selected MinGW GCC, but the root GDE toolchain appears to be LLVM-family.",
                platform=platform,
                env=env,
                artifact_ext="a",
                godot_cpp_toolchain_args=args,
            )
        return {
            "family": family,
            "artifact_ext": "a",
            "label": "MinGW LLVM" if family == "mingw-llvm" else "MinGW GCC",
            "godot_cpp_toolchain_args": args,
        }

    # use_mingw=auto: resolve from the actual root GDE tools, not only the shell.
    if actual_mingw:
        if requested_use_llvm == "yes" and not actual_llvm and not is_clean:
            _windows_gde_toolchain_error(
                "use_llvm=yes selected MinGW-LLVM, but the root GDE toolchain is not confidently LLVM-family.",
                platform=platform,
                env=env,
                artifact_ext="a",
                godot_cpp_toolchain_args=["use_mingw=yes", "use_llvm=yes"],
            )
        if requested_use_llvm == "no" and actual_llvm and not is_clean:
            _windows_gde_toolchain_error(
                "use_llvm=no selected MinGW GCC, but the root GDE toolchain appears to be LLVM-family.",
                platform=platform,
                env=env,
                artifact_ext="a",
                godot_cpp_toolchain_args=["use_mingw=yes"],
            )
        family = "mingw-llvm" if (requested_use_llvm == "yes" or (requested_use_llvm == "auto" and actual_llvm)) else "mingw-gcc"
        args = ["use_mingw=yes"]
        if family == "mingw-llvm":
            args.append("use_llvm=yes")
        return {
            "family": family,
            "artifact_ext": "a",
            "label": "MinGW LLVM" if family == "mingw-llvm" else "MinGW GCC",
            "godot_cpp_toolchain_args": args,
        }

    if actual_msvc:
        if requested_use_llvm == "yes" and not is_clean:
            _windows_gde_toolchain_error(
                "use_llvm=yes requires a MinGW-family root GDE toolchain, but the root GDE toolchain appears to be MSVC-family.",
                platform=platform,
                env=env,
                artifact_ext="lib",
                godot_cpp_toolchain_args=["use_mingw=no"],
            )
        return {
            "family": "msvc",
            "artifact_ext": "lib",
            "label": "MSVC",
            "godot_cpp_toolchain_args": ["use_mingw=no"],
        }

    if not is_clean:
        _windows_gde_toolchain_error(
            "use_mingw=auto could not confidently resolve the Windows GDE toolchain family.",
            platform=platform,
            env=env,
            artifact_ext="a|lib",
            godot_cpp_toolchain_args=[],
        )

    return {
        "family": "msvc",
        "artifact_ext": "lib",
        "label": "MSVC (clean fallback)",
        "godot_cpp_toolchain_args": ["use_mingw=no"],
    }


GDE_PROVIDER_RESOLUTION = {
    "windows": {
        "family": "windows_mediafoundation",
        "location": os.path.join("src", "imaging", "platform", "windows"),
        "implemented": True,
        "defines": ["CAMBANG_PROVIDER_WINDOWS_MF=1"],
        "libs": ["mf", "mfplat", "mfreadwrite", "mfuuid", "ole32", "uuid"],
        "status": "windows_mediafoundation(dev accelerator)",
    },
    "android": {
        "family": "android_camera2",
        "location": os.path.join("src", "imaging", "platform", "android"),
        "implemented": False,
    },
    "linux": {
        "family": "linux_v4l2",
        "location": os.path.join("src", "imaging", "platform", "linux"),
        "implemented": False,
    },
    "macos": {
        "family": "apple_avfoundation",
        "location": os.path.join("src", "imaging", "platform", "apple"),
        "implemented": False,
    },
    "ios": {
        "family": "apple_avfoundation",
        "location": os.path.join("src", "imaging", "platform", "apple"),
        "implemented": False,
    },
    "web": {
        "family": "web_getusermedia",
        "location": os.path.join("src", "imaging", "platform", "web"),
        "implemented": False,
    },
}

RUNTIME_VALIDATORS = {
    "windows": {
        "name": "windows_mediafoundation_runtime_validator",
        "target": os.path.join("out", "windows_mf_runtime_validate"),
    },
}


vars = Variables()
vars.Add(BoolVariable(
    "gde",
    "Build the GDExtension artifact (godot glue + SyntheticProvider + selected platform provider).",
    True,
))
vars.Add(BoolVariable(
    "verify",
    "Build deterministic host-native smoke/verification/benchmark tools.",
    True,
))
vars.Add(EnumVariable(
    "platform",
    "GDE target platform.",
    _detect_host_platform(),
    allowed_values=["windows", "android", "linux", "macos", "ios", "web"],
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
vars.Add(BoolVariable(
    "platform_runtime_validate",
    "Build platform-backed runtime validation tools (explicit opt-in; may access real hardware).",
    False,
))
vars.Add(
    "COMPDB_PATH",
    "Path for aggregate compile_commands.json generation.",
    "compile_commands.json",
)
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

# Initial environment exists only to parse declared variables and render help.
tmp_env = Environment(variables=vars)
Help(vars.GenerateHelpText(tmp_env))

host_platform = _detect_host_platform()
gde_platform = tmp_env["platform"]
is_clean = GetOption("clean")
build_gde = bool(tmp_env["gde"])
build_verify = bool(tmp_env["verify"])
build_platform_runtime_validate = bool(tmp_env["platform_runtime_validate"])
selected_provider = GDE_PROVIDER_RESOLUTION[gde_platform]

# Toolchain selection is intentionally host-oriented. platform=<...> selects the GDE
# target platform and must not make host verifiers non-native. Explicit Windows
# toolchain choices are kept authoritative for the root GDE build environment.
requested_use_mingw = tmp_env["use_mingw"]
requested_use_llvm = tmp_env["use_llvm"]
resolved_use_mingw = requested_use_mingw
if resolved_use_mingw == "auto":
    resolved_use_mingw = "yes" if (host_platform == "windows" and (requested_use_llvm == "yes" or _looks_like_msys_or_bash())) else "no"

tools = None
if host_platform == "windows" and resolved_use_mingw == "yes":
    tools = ["mingw"]

command_process_env = _command_process_env(host_platform, tmp_env["arch"])

# Build-only validation. Cleaning remains first-class even when selected platform
# support, generated godot-cpp output, compdb output, SDKs, or validators are absent.
if not is_clean:
    if build_platform_runtime_validate and gde_platform not in RUNTIME_VALIDATORS:
        print(f"ERROR: platform_runtime_validate=yes has no validator for platform '{gde_platform}'.")
        print("  Currently implemented validator platform(s): " + ", ".join(sorted(RUNTIME_VALIDATORS)))
        Exit(1)
    if build_gde and not selected_provider["implemented"]:
        print(f"ERROR: GDE platform '{gde_platform}' is not currently buildable.")
        print(f"  Expected provider family: {selected_provider['family']}")
        print(f"  Expected provider implementation location: {selected_provider['location']}")
        Exit(1)
    if not (build_gde or build_verify or build_platform_runtime_validate):
        print("ERROR: Nothing to build. Set gde=yes and/or verify=yes and/or platform_runtime_validate=yes (or run with -c to clean).")
        Exit(1)

env = Environment(variables=vars, tools=tools)
env["ENV"] = command_process_env
if host_platform == "windows" and resolved_use_mingw == "yes" and requested_use_llvm == "yes":
    env.Replace(CC="clang", CXX="clang++", LINK="clang++", SHCC="clang", SHCXX="clang++", SHLINK="clang++")
env.Append(CPPPATH=["src"])

# IDE support: aggregate compile_commands.json for all active compile environments.
env.Tool("compdb", toolpath=["site_scons/site_tools"])

cxx = str(env.get("CXX", "")).lower()
is_msvc = ("cl" in cxx) or env.get("MSVC_VERSION")
core_target = _core_target_for_flags(env["target"])
godot_target = _godot_target(env["target"])
windows_gde_toolchain = None
if build_gde and gde_platform == "windows":
    windows_gde_toolchain = resolve_windows_gde_toolchain(
        env,
        platform=gde_platform,
        requested_use_mingw=requested_use_mingw,
        requested_use_llvm=requested_use_llvm,
        is_clean=is_clean,
    )
    is_msvc = windows_gde_toolchain["family"] == "msvc"

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
    env.Append(CCFLAGS=["-pthread"])
    env.Append(LINKFLAGS=["-pthread"])

print("CamBANG SCons configuration:")
print(f"  host_platform={host_platform} gde_platform={gde_platform} target={env['target']} (core_flags={core_target}, godot={godot_target}) arch={env['arch']} precision={env['precision']}")
print(f"  toolchain={'msvc' if is_msvc else 'gcc/clang'} CXX={env.get('CXX')}")
if host_platform == "windows":
    print(f"  use_mingw={env['use_mingw']} use_llvm={env['use_llvm']} resolved_use_mingw={resolved_use_mingw}")
if windows_gde_toolchain:
    print(f"  windows_gde_toolchain={windows_gde_toolchain['label']} godot_cpp_ext=.{windows_gde_toolchain['artifact_ext']}")
    print("  godot_cpp_toolchain_args=" + (" ".join(windows_gde_toolchain["godot_cpp_toolchain_args"]) or "<none>"))
print(f"  gde={'yes' if build_gde else 'no'} verify={'yes' if build_verify else 'no'} platform_runtime_validate={'yes' if build_platform_runtime_validate else 'no'}")
print(f"  gde_provider={selected_provider['family']} ({selected_provider['location']})")
if selected_provider.get("status"):
    print(f"  gde_provider_status={selected_provider['status']}")
print(f"  COMPDB_PATH={env['COMPDB_PATH']}")

out_dir = "out"
os.makedirs(out_dir, exist_ok=True)

verify_obj_dir = os.path.join(out_dir, "verify_obj")
platform_runtime_validate_obj_dir = os.path.join(out_dir, "platform_runtime_validate_obj")
gde_obj_dir = os.path.join(out_dir, "gde_obj")


def _glob_cpp(obj_dir, *parts):
    return Glob(os.path.join(obj_dir, *parts, "*.cpp"))


def _unique_sources(seq):
    seen = set()
    out = []
    for item in seq:
        key = str(item)
        if key not in seen:
            seen.add(key)
            out.append(item)
    return out


def _host_core_runtime_sources(obj_dir):
    sources = []
    sources += _glob_cpp(obj_dir, "core")
    sources += _glob_cpp(obj_dir, "core", "snapshot")
    sources += _glob_cpp(obj_dir, "imaging", "api")
    sources += [os.path.join(obj_dir, "imaging", "broker", "banner_info.cpp")]
    sources += _glob_cpp(obj_dir, "pixels", "pattern")
    return _unique_sources(sources)


# ---------------------------------------------------------------------------
# Host-native deterministic verification tools (alias: verify)
# ---------------------------------------------------------------------------
if build_verify:
    verify_env = env.Clone()
    verify_env.Append(CPPDEFINES=[
        "CAMBANG_INTERNAL_SMOKE=1",
        "CAMBANG_ENABLE_SYNTHETIC=1",
        "CAMBANG_PROVIDER_STUB=1",
        "CAMBANG_SMOKE_WITH_STUB_PROVIDER=1",
    ])

    if host_platform == "windows" and not is_msvc:
        verify_env.Append(LINKFLAGS=["-mconsole"])

    verify_env.VariantDir(verify_obj_dir, "src", duplicate=0)

    verify_core_runtime_sources = _host_core_runtime_sources(verify_obj_dir)
    verify_broker_sources = _glob_cpp(verify_obj_dir, "imaging", "broker")
    verify_broker_sources = [s for s in verify_broker_sources if not str(s).endswith("banner_info.cpp")]
    verify_stub_sources = _glob_cpp(verify_obj_dir, "imaging", "stub")
    verify_synthetic_sources = _glob_cpp(verify_obj_dir, "imaging", "synthetic")

    runtime_verify_sources = _unique_sources(verify_core_runtime_sources + verify_stub_sources)

    core_smoke_prog = verify_env.Program(
        target=os.path.join(out_dir, "core_spine_smoke"),
        source=runtime_verify_sources + ["src/smoke/core_spine_smoke.cpp"],
    )
    core_result_path_smoke_prog = verify_env.Program(
        target=os.path.join(out_dir, "core_result_path_smoke"),
        source=verify_core_runtime_sources + ["src/smoke/core_result_path_smoke.cpp"],
    )
    core_capture_assembly_registry_smoke_prog = verify_env.Program(
        target=os.path.join(out_dir, "core_capture_assembly_registry_smoke"),
        source=verify_core_runtime_sources + ["src/smoke/core_capture_assembly_registry_smoke.cpp"],
    )
    core_dispatcher_bracket_routing_smoke_prog = verify_env.Program(
        target=os.path.join(out_dir, "core_dispatcher_bracket_routing_smoke"),
        source=verify_core_runtime_sources + ["src/smoke/core_dispatcher_bracket_routing_smoke.cpp"],
    )

    pattern_bench_sources = _unique_sources(_glob_cpp(verify_obj_dir, "pixels", "pattern") + ["src/smoke/pattern_render_bench.cpp"])
    pattern_bench_prog = verify_env.Program(
        target=os.path.join(out_dir, "pattern_render_bench"),
        source=pattern_bench_sources,
    )

    synthetic_verify_sources = _unique_sources(verify_core_runtime_sources + verify_synthetic_sources + ["src/smoke/synthetic_timeline_verify.cpp"])
    synthetic_verify_prog = verify_env.Program(
        target=os.path.join(out_dir, "synthetic_timeline_verify"),
        source=synthetic_verify_sources,
    )

    phase3_verify_sources = _unique_sources(verify_core_runtime_sources + ["src/smoke/phase3_snapshot_verify.cpp"])
    phase3_verify_prog = verify_env.Program(
        target=os.path.join(out_dir, "phase3_snapshot_verify"),
        source=phase3_verify_sources,
    )

    restart_boundary_verify_sources = _unique_sources(
        verify_core_runtime_sources + verify_stub_sources + verify_synthetic_sources + ["src/smoke/restart_boundary_verify.cpp"]
    )
    restart_boundary_verify_prog = verify_env.Program(
        target=os.path.join(out_dir, "restart_boundary_verify"),
        source=restart_boundary_verify_sources,
    )

    verify_case_runner_sources = _unique_sources(
        verify_core_runtime_sources
        + verify_broker_sources
        + verify_stub_sources
        + verify_synthetic_sources
        + [
            "src/smoke/verify_case/verify_case_catalog.cpp",
            "src/smoke/verify_case_runner.cpp",
        ]
    )
    verify_case_runner_prog = verify_env.Program(
        target=os.path.join(out_dir, "verify_case_runner"),
        source=verify_case_runner_sources,
    )

    provider_verify_sources = _unique_sources(
        verify_core_runtime_sources
        + verify_broker_sources
        + verify_stub_sources
        + verify_synthetic_sources
        + ["src/smoke/provider_compliance_verify.cpp"]
    )
    provider_verify_prog = verify_env.Program(
        target=os.path.join(out_dir, "provider_compliance_verify"),
        source=provider_verify_sources,
    )

    verify_alias = Alias(
        "verify",
        [
            core_smoke_prog,
            core_result_path_smoke_prog,
            core_capture_assembly_registry_smoke_prog,
            core_dispatcher_bracket_routing_smoke_prog,
            pattern_bench_prog,
            synthetic_verify_prog,
            phase3_verify_prog,
            verify_case_runner_prog,
            provider_verify_prog,
            restart_boundary_verify_prog,
        ],
    )
    AlwaysBuild(verify_alias)
else:
    verify_alias = Alias("verify", [])


# ---------------------------------------------------------------------------
# GDExtension artifact (alias: gde)
# ---------------------------------------------------------------------------
if build_gde:
    gde_env = env.Clone()
    gde_env.Append(CPPDEFINES=["CAMBANG_GDE_BUILD=1", "CAMBANG_ENABLE_SYNTHETIC=1"])
    gde_env.VariantDir(gde_obj_dir, "src", duplicate=0)

    gde_out_dir = os.path.join("tests", "cambang_gde", "bin")
    os.makedirs(gde_out_dir, exist_ok=True)

    godot_cpp_lib = _godot_cpp_lib_path(gde_platform, godot_target, env["arch"], windows_gde_toolchain)
    godot_cpp_libdir = os.path.join("thirdparty", "godot-cpp", "bin")
    godot_cpp_libname = f"godot-cpp.{gde_platform}.{godot_target}.{env['arch']}"

    godot_gen_header = os.path.join(
        "thirdparty", "godot-cpp", "gen", "include", "godot_cpp", "classes", "global_constants.hpp"
    )
    godot_cpp_args = [
        f'"{python_exe}"',
        "-m",
        "SCons",
        "-C",
        "thirdparty/godot-cpp",
        f"platform={gde_platform}",
        f"target={godot_target}",
        f"arch={env['arch']}",
        f"precision={env['precision']}",
    ]
    if windows_gde_toolchain:
        godot_cpp_args += windows_gde_toolchain["godot_cpp_toolchain_args"]
    godot_cpp_cmd = " ".join(godot_cpp_args)
    godot_cpp_build = env.Command(target=[godot_gen_header, godot_cpp_lib], source=[], action=godot_cpp_cmd)

    Alias("godot_cpp", godot_cpp_build)
    AlwaysBuild("godot_cpp")

    gde_env.Append(CPPPATH=[
        os.path.join("thirdparty", "godot-cpp"),
        os.path.join("thirdparty", "godot-cpp", "include"),
        os.path.join("thirdparty", "godot-cpp", "gen"),
        os.path.join("thirdparty", "godot-cpp", "gen", "include"),
        os.path.join("thirdparty", "godot-cpp", "gdextension"),
    ])

    if not is_msvc:
        gde_env.Append(CXXFLAGS=["-fPIC", "-fvisibility=hidden"])

    gde_sources = []
    gde_sources += _glob_cpp(gde_obj_dir, "core")
    gde_sources += _glob_cpp(gde_obj_dir, "core", "snapshot")
    gde_sources += _glob_cpp(gde_obj_dir, "imaging", "api")
    gde_sources += _glob_cpp(gde_obj_dir, "imaging", "broker")
    gde_sources += _glob_cpp(gde_obj_dir, "pixels", "pattern")
    gde_sources += _glob_cpp(gde_obj_dir, "imaging", "synthetic")

    if gde_platform == "windows":
        gde_sources += _glob_cpp(gde_obj_dir, "imaging", "platform", "windows")
        gde_env.Append(CPPDEFINES=selected_provider["defines"])
        # MinGW link set typically needs mf + uuid in addition to the usual MF libs.
        gde_env.Append(LIBS=selected_provider["libs"])

    gde_sources += [
        os.path.join(gde_obj_dir, "godot", "module_init.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_server.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_device.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_rig.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_stream.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_stream_result.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_stream_result_internal.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_capture_result.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_capture_result_set.cpp"),
        os.path.join(gde_obj_dir, "godot", "cambang_result_convert.cpp"),
        os.path.join(gde_obj_dir, "godot", "state_snapshot_export.cpp"),
        os.path.join(gde_obj_dir, "godot", "godot_gpu_display_service.cpp"),
        os.path.join(gde_obj_dir, "godot", "synthetic_gpu_backing_bridge.cpp"),
    ]
    gde_sources = _unique_sources(gde_sources)

    if gde_platform == "windows":
        gde_filename = f"cambang.windows.{godot_target}.{env['arch']}.dll"
    else:
        gde_filename = f"libcambang.{gde_platform}.{godot_target}.{env['arch']}"
    gde_target = os.path.join(gde_out_dir, gde_filename)

    gde_env.Append(LIBPATH=[godot_cpp_libdir])
    gde_env.Append(LIBS=[godot_cpp_libname])

    gde_objects = gde_env.SharedObject(gde_sources)
    gde_env.Depends(gde_objects, godot_cpp_build)

    gde_lib = gde_env.SharedLibrary(target=gde_target, source=gde_objects)
    gde_env.Depends(gde_lib, godot_cpp_build)

    gde_alias = Alias("gde", gde_lib)
    AlwaysBuild(gde_alias)
else:
    gde_alias = Alias("gde", [])


# ---------------------------------------------------------------------------
# Platform-backed runtime validation (alias: platform_runtime_validate)
# Windows validation currently targets windows_mediafoundation(dev accelerator)
# only; it is not release-provider conformance evidence.
# ---------------------------------------------------------------------------
if build_platform_runtime_validate:
    validate_env = env.Clone()
    validate_env.Append(CPPDEFINES=["CAMBANG_INTERNAL_SMOKE=1"])

    if host_platform == "windows" and not is_msvc:
        validate_env.Append(LINKFLAGS=["-mconsole"])

    validate_env.VariantDir(platform_runtime_validate_obj_dir, "src", duplicate=0)

    runtime_validate_progs = []
    if gde_platform == "windows":
        windows_validate_sources = []
        windows_validate_sources += _glob_cpp(platform_runtime_validate_obj_dir, "core")
        windows_validate_sources += _glob_cpp(platform_runtime_validate_obj_dir, "core", "snapshot")
        windows_validate_sources += _glob_cpp(platform_runtime_validate_obj_dir, "imaging", "api")
        windows_validate_sources += [os.path.join(platform_runtime_validate_obj_dir, "imaging", "broker", "banner_info.cpp")]
        windows_validate_sources += _glob_cpp(platform_runtime_validate_obj_dir, "imaging", "platform", "windows")
        windows_validate_sources += _glob_cpp(platform_runtime_validate_obj_dir, "pixels", "pattern")
        windows_validate_sources += ["src/smoke/windows_mf_runtime_validate.cpp"]
        windows_validate_sources = _unique_sources(windows_validate_sources)
        validate_env.Append(LIBS=GDE_PROVIDER_RESOLUTION["windows"]["libs"])
        runtime_validate_progs.append(validate_env.Program(
            target=RUNTIME_VALIDATORS["windows"]["target"],
            source=windows_validate_sources,
        ))

    platform_runtime_validate_alias = Alias("platform_runtime_validate", runtime_validate_progs)
    AlwaysBuild(platform_runtime_validate_alias)
else:
    platform_runtime_validate_alias = Alias("platform_runtime_validate", [])


selected_default_nodes = []
if build_verify:
    selected_default_nodes.append(verify_alias)
if build_gde:
    selected_default_nodes.append(gde_alias)
if build_platform_runtime_validate:
    selected_default_nodes.append(platform_runtime_validate_alias)

if selected_default_nodes:
    build_all_alias = Alias("build_all", selected_default_nodes)
    AlwaysBuild(build_all_alias)
    if not is_clean:
        AddPostAction(build_all_alias, env["COMPDB_WRITE_ACTION"])
    Default(build_all_alias)
else:
    Default(Alias("build_all", []))
