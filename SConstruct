#!/usr/bin/env python
# CamBANG - SCons entrypoint
#
# Repo-root build contract:
# - deterministic host maintainer tools are enabled by default (maintainer_tools=yes)
# - the GDExtension artifact is enabled by default (gde=yes)
# - platform=<...> selects the GDE target platform, not the host maintainer-tool platform
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
    Clean,
    Default,
    Environment,
    EnumVariable,
    Exit,
    GetOption,
    Glob,
    Help,
    Variables,
)

GDE_PLATFORMS = ["windows", "android", "linux", "macos", "ios", "web"]
GDE_TARGET_VALUES = ["debug", "release", "template_debug", "template_release"]
GDE_ARCH_VALUES = ["x86_64", "x86_32", "arm64", "arm32"]
GDE_PRECISION_VALUES = ["single", "double"]

ALLOWED_VARIABLES = {
    "gde",
    "maintainer_tools",
    "platform",
    "target",
    "arch",
    "precision",
    "platform_runtime_validate",
    "COMPDB_PATH",
    "use_mingw",
    "use_llvm",
    "mingw_prefix",
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


def _godot_cpp_lib_path(platform: str, target: str, arch: str, windows_uses_mingw: bool) -> str:
    # godot-cpp outputs: thirdparty/godot-cpp/bin/libgodot-cpp.<platform>.<target>.<arch>.(a|lib)
    ext = "a" if (platform != "windows" or windows_uses_mingw) else "lib"
    return os.path.join("thirdparty", "godot-cpp", "bin", f"libgodot-cpp.{platform}.{target}.{arch}.{ext}")


def _cpp_string_define_value(value: str) -> str:
    # SCons passes CPPDEFINES tuple values as macro text; escaped quotes ensure
    # the compiler sees a C++ string literal rather than a bare identifier.
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'\\"{escaped}\\"'


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
    "maintainer_tools",
    "Build deterministic host-native smoke/verification/benchmark tools.",
    True,
))
vars.Add(EnumVariable(
    "platform",
    "GDE target platform.",
    _detect_host_platform(),
    allowed_values=GDE_PLATFORMS,
))
vars.Add(EnumVariable(
    "target",
    "Build target. Accepts debug/release and Godot template_debug/template_release.",
    "debug",
    allowed_values=GDE_TARGET_VALUES,
))
vars.Add(EnumVariable(
    "arch",
    "Target architecture (convention-compatible knob).",
    "x86_64",
    allowed_values=GDE_ARCH_VALUES,
))
vars.Add(EnumVariable(
    "precision",
    "Floating-point precision knob (forwarded to godot-cpp).",
    "single",
    allowed_values=GDE_PRECISION_VALUES,
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
vars.Add(
    "mingw_prefix",
    "Optional MinGW installation prefix forwarded to godot-cpp for Windows MinGW GDE builds.",
    "",
)
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
build_maintainer_tools = bool(tmp_env["maintainer_tools"])
build_platform_runtime_validate = bool(tmp_env["platform_runtime_validate"])
selected_provider = GDE_PROVIDER_RESOLUTION[gde_platform]

# Toolchain selection is intentionally host-oriented. platform=<...> selects the GDE
# target platform and must not make host verifiers non-native.
resolved_use_mingw = tmp_env["use_mingw"]
if resolved_use_mingw == "auto":
    resolved_use_mingw = "yes" if (host_platform == "windows" and _looks_like_msys_or_bash()) else "no"

windows_uses_mingw = gde_platform == "windows" and resolved_use_mingw == "yes"

tools = None
if host_platform == "windows" and windows_uses_mingw:
    tools = ["mingw"]

command_process_env = _command_process_env(host_platform, tmp_env["arch"])

# Build-only validation. Cleaning remains first-class even when selected platform
# support, generated godot-cpp output, compdb output, SDKs, or validators are absent.
if not is_clean:
    if build_platform_runtime_validate and gde_platform not in RUNTIME_VALIDATORS:
        print(f"ERROR: platform_runtime_validate=yes has no validator for platform '{gde_platform}'.")
        print("  Currently implemented validator platform(s): " + ", ".join(sorted(RUNTIME_VALIDATORS)))
        Exit(1)
    if not (build_gde or build_maintainer_tools or build_platform_runtime_validate):
        print("ERROR: Nothing to build. Set gde=yes and/or maintainer_tools=yes and/or platform_runtime_validate=yes (or run with -c to clean).")
        Exit(1)

env = Environment(variables=vars, tools=tools)
env["ENV"] = command_process_env
env.Append(CPPPATH=["src"])

# IDE support: aggregate compile_commands.json for all active compile environments.
env.Tool("compdb", toolpath=["site_scons/site_tools"])

cxx = str(env.get("CXX", "")).lower()
is_msvc = ("cl" in cxx) or env.get("MSVC_VERSION")
core_target = _core_target_for_flags(env["target"])
godot_target = _godot_target(env["target"])

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
    print(f"  use_mingw={env['use_mingw']} use_llvm={env['use_llvm']}")
print(f"  gde={'yes' if build_gde else 'no'} maintainer_tools={'yes' if build_maintainer_tools else 'no'} platform_runtime_validate={'yes' if build_platform_runtime_validate else 'no'}")
print(f"  gde_provider={selected_provider['family']} ({selected_provider['location']})")
if build_gde and not selected_provider["implemented"]:
    print("  gde_provider_status=not_compiled")
    print("  platform_backed runtime mode unavailable")
    print("  synthetic=yes")
elif selected_provider.get("status"):
    print(f"  gde_provider_status={selected_provider['status']}")
print(f"  COMPDB_PATH={env['COMPDB_PATH']}")

out_dir = "out"
os.makedirs(out_dir, exist_ok=True)

maintainer_tools_obj_dir = os.path.join(out_dir, "maintainer_tools_obj")
platform_runtime_validate_obj_root = os.path.join(out_dir, "platform_runtime_validate_obj")
platform_runtime_validate_obj_dir = os.path.join(platform_runtime_validate_obj_root, gde_platform)
gde_obj_root = os.path.join(out_dir, "gde_obj")
gde_obj_dir = os.path.join(gde_obj_root, gde_platform, godot_target, env["arch"], env["precision"])


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


def _program_path(name):
    return os.path.join(out_dir, name + env.get("PROGSUFFIX", ""))


def _gde_obj_dir(platform, target, arch, precision):
    return os.path.join(gde_obj_root, platform, target, arch, precision)


def _gde_shared_library_suffix(platform):
    return {
        "windows": ".dll",
        "android": ".so",
        "linux": ".so",
        "macos": ".dylib",
        "ios": ".dylib",
        "web": ".so",
    }[platform]


def _gde_artifact_stem(platform, target, arch):
    if platform == "windows":
        return os.path.join("tests", "cambang_gde", "bin", f"cambang.windows.{target}.{arch}")
    return os.path.join("tests", "cambang_gde", "bin", f"libcambang.{platform}.{target}.{arch}")


def _gde_artifact_base(platform, target, arch):
    return _gde_artifact_stem(platform, target, arch) + _gde_shared_library_suffix(platform)


def _planned_gde_artifact_paths(platform, target, arch):
    artifact = _gde_artifact_base(platform, target, arch)
    if platform == "windows":
        return [artifact]
    legacy_stem = _gde_artifact_stem(platform, target, arch)
    return _unique_sources([artifact, legacy_stem, legacy_stem + ".so", legacy_stem + ".dylib", legacy_stem + ".dll"])


def _planned_selected_gde_clean_outputs(platform, target, arch, precision):
    return _unique_sources([_gde_obj_dir(platform, target, arch, precision)] + _planned_gde_artifact_paths(platform, target, arch))


def _all_planned_gde_clean_outputs():
    outputs = [gde_obj_root]
    for platform in GDE_PLATFORMS:
        for target_value in GDE_TARGET_VALUES:
            planned_target = _godot_target(target_value)
            for arch_value in GDE_ARCH_VALUES:
                outputs += _planned_gde_artifact_paths(platform, planned_target, arch_value)
                for precision_value in GDE_PRECISION_VALUES:
                    outputs.append(_gde_obj_dir(platform, planned_target, arch_value, precision_value))
    return _unique_sources(outputs)


def _selected_platform_runtime_validate_clean_outputs(platform):
    outputs = [os.path.join(platform_runtime_validate_obj_root, platform)]
    if platform == "windows":
        outputs.append(_program_path("windows_mf_runtime_validate"))
    return outputs


def _all_platform_runtime_validate_clean_outputs():
    outputs = [platform_runtime_validate_obj_root]
    for platform in GDE_PLATFORMS:
        outputs += _selected_platform_runtime_validate_clean_outputs(platform)
    return _unique_sources(outputs)


maintainer_tools_clean_outputs = [
    maintainer_tools_obj_dir,
    _program_path("core_spine_smoke"),
    _program_path("core_result_path_smoke"),
    _program_path("core_capture_assembly_registry_smoke"),
    _program_path("core_dispatcher_bracket_routing_smoke"),
    _program_path("pattern_render_bench"),
    _program_path("synthetic_timeline_verify"),
    _program_path("phase3_snapshot_verify"),
    _program_path("verify_case_runner"),
    _program_path("provider_compliance_verify"),
    _program_path("restart_boundary_verify"),
    _program_path("synthetic_only_provider_support_verify"),
]
platform_runtime_validate_clean_outputs = _selected_platform_runtime_validate_clean_outputs(gde_platform)
gde_clean_outputs = _planned_selected_gde_clean_outputs(gde_platform, godot_target, env["arch"], env["precision"])
gde_all_clean_outputs = _all_planned_gde_clean_outputs()
godot_cpp_lib = _godot_cpp_lib_path(gde_platform, godot_target, env["arch"], windows_uses_mingw)
godot_gen_header = os.path.join(
    "thirdparty", "godot-cpp", "gen", "include", "godot_cpp", "core", "ext_wrappers.gen.inc"
)
godot_cpp_clean_outputs = [godot_gen_header, godot_cpp_lib]
build_gde_graph = build_gde and not is_clean


# ---------------------------------------------------------------------------
# Host-native deterministic maintainer tools (alias: maintainer_tools)
# ---------------------------------------------------------------------------
if build_maintainer_tools:
    maintainer_tools_env = env.Clone()
    maintainer_tools_env.Append(CPPDEFINES=[
        "CAMBANG_INTERNAL_SMOKE=1",
        "CAMBANG_ENABLE_SYNTHETIC=1",
        "CAMBANG_PROVIDER_STUB=1",
        "CAMBANG_SMOKE_WITH_STUB_PROVIDER=1",
    ])

    if host_platform == "windows" and not is_msvc:
        maintainer_tools_env.Append(LINKFLAGS=["-mconsole"])

    maintainer_tools_env.VariantDir(maintainer_tools_obj_dir, "src", duplicate=0)

    maintainer_tools_core_runtime_sources = _host_core_runtime_sources(maintainer_tools_obj_dir)
    maintainer_tools_broker_sources = _glob_cpp(maintainer_tools_obj_dir, "imaging", "broker")
    maintainer_tools_broker_sources = [s for s in maintainer_tools_broker_sources if not str(s).endswith("banner_info.cpp")]
    maintainer_tools_stub_sources = _glob_cpp(maintainer_tools_obj_dir, "imaging", "stub")
    maintainer_tools_synthetic_sources = _glob_cpp(maintainer_tools_obj_dir, "imaging", "synthetic")

    runtime_maintainer_tools_sources = _unique_sources(maintainer_tools_core_runtime_sources + maintainer_tools_stub_sources)

    core_smoke_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "core_spine_smoke"),
        source=runtime_maintainer_tools_sources + ["src/smoke/core_spine_smoke.cpp"],
    )
    core_result_path_smoke_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "core_result_path_smoke"),
        source=maintainer_tools_core_runtime_sources + ["src/smoke/core_result_path_smoke.cpp"],
    )
    core_capture_assembly_registry_smoke_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "core_capture_assembly_registry_smoke"),
        source=maintainer_tools_core_runtime_sources + ["src/smoke/core_capture_assembly_registry_smoke.cpp"],
    )
    core_dispatcher_bracket_routing_smoke_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "core_dispatcher_bracket_routing_smoke"),
        source=maintainer_tools_core_runtime_sources + ["src/smoke/core_dispatcher_bracket_routing_smoke.cpp"],
    )

    pattern_bench_sources = _unique_sources(_glob_cpp(maintainer_tools_obj_dir, "pixels", "pattern") + ["src/smoke/pattern_render_bench.cpp"])
    pattern_bench_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "pattern_render_bench"),
        source=pattern_bench_sources,
    )

    synthetic_maintainer_tools_sources = _unique_sources(maintainer_tools_core_runtime_sources + maintainer_tools_synthetic_sources + ["src/smoke/synthetic_timeline_verify.cpp"])
    synthetic_maintainer_tools_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "synthetic_timeline_verify"),
        source=synthetic_maintainer_tools_sources,
    )

    phase3_maintainer_tools_sources = _unique_sources(maintainer_tools_core_runtime_sources + ["src/smoke/phase3_snapshot_verify.cpp"])
    phase3_maintainer_tools_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "phase3_snapshot_verify"),
        source=phase3_maintainer_tools_sources,
    )

    restart_boundary_maintainer_tools_sources = _unique_sources(
        maintainer_tools_core_runtime_sources + maintainer_tools_stub_sources + maintainer_tools_synthetic_sources + ["src/smoke/restart_boundary_verify.cpp"]
    )
    restart_boundary_maintainer_tools_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "restart_boundary_verify"),
        source=restart_boundary_maintainer_tools_sources,
    )

    verify_case_runner_sources = _unique_sources(
        maintainer_tools_core_runtime_sources
        + maintainer_tools_broker_sources
        + maintainer_tools_stub_sources
        + maintainer_tools_synthetic_sources
        + [
            "src/smoke/verify_case/verify_case_catalog.cpp",
            "src/smoke/verify_case_runner.cpp",
        ]
    )
    verify_case_runner_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "verify_case_runner"),
        source=verify_case_runner_sources,
    )

    provider_maintainer_tools_sources = _unique_sources(
        maintainer_tools_core_runtime_sources
        + maintainer_tools_broker_sources
        + maintainer_tools_stub_sources
        + maintainer_tools_synthetic_sources
        + ["src/smoke/provider_compliance_verify.cpp"]
    )
    provider_maintainer_tools_prog = maintainer_tools_env.Program(
        target=os.path.join(out_dir, "provider_compliance_verify"),
        source=provider_maintainer_tools_sources,
    )

    synthetic_only_provider_support_obj_dir = os.path.join(out_dir, "synthetic_only_provider_support_obj")
    maintainer_tools_clean_outputs.append(synthetic_only_provider_support_obj_dir)
    synthetic_only_provider_support_env = env.Clone()
    synthetic_only_provider_support_env.Append(CPPDEFINES=[
        "CAMBANG_INTERNAL_SMOKE=1",
        "CAMBANG_ENABLE_SYNTHETIC=1",
    ])
    if host_platform == "windows" and not is_msvc:
        synthetic_only_provider_support_env.Append(LINKFLAGS=["-mconsole"])
    synthetic_only_provider_support_env.VariantDir(synthetic_only_provider_support_obj_dir, "src", duplicate=0)
    synthetic_only_provider_support_broker_sources = _glob_cpp(synthetic_only_provider_support_obj_dir, "imaging", "broker")
    synthetic_only_provider_support_broker_sources = [
        s for s in synthetic_only_provider_support_broker_sources
        if not str(s).endswith("banner_info.cpp")
    ]
    synthetic_only_provider_support_sources = _unique_sources(
        _host_core_runtime_sources(synthetic_only_provider_support_obj_dir)
        + synthetic_only_provider_support_broker_sources
        + _glob_cpp(synthetic_only_provider_support_obj_dir, "imaging", "synthetic")
        + ["src/smoke/synthetic_only_provider_support_verify.cpp"]
    )
    synthetic_only_provider_support_prog = synthetic_only_provider_support_env.Program(
        target=os.path.join(out_dir, "synthetic_only_provider_support_verify"),
        source=synthetic_only_provider_support_sources,
    )

    maintainer_tools_alias = Alias(
        "maintainer_tools",
        [
            core_smoke_prog,
            core_result_path_smoke_prog,
            core_capture_assembly_registry_smoke_prog,
            core_dispatcher_bracket_routing_smoke_prog,
            pattern_bench_prog,
            synthetic_maintainer_tools_prog,
            phase3_maintainer_tools_prog,
            verify_case_runner_prog,
            provider_maintainer_tools_prog,
            restart_boundary_maintainer_tools_prog,
            synthetic_only_provider_support_prog,
        ],
    )
    AlwaysBuild(maintainer_tools_alias)
else:
    maintainer_tools_alias = Alias("maintainer_tools", [])


# ---------------------------------------------------------------------------
# GDExtension artifact (alias: gde)
# ---------------------------------------------------------------------------
if build_gde_graph:
    gde_env = env.Clone()
    gde_platform_provider_status = "compiled" if selected_provider["implemented"] else "not_compiled"
    gde_env.Append(CPPDEFINES=[
        "CAMBANG_GDE_BUILD=1",
        "CAMBANG_ENABLE_SYNTHETIC=1",
        ("CAMBANG_GDE_TARGET_PLATFORM", _cpp_string_define_value(gde_platform)),
        ("CAMBANG_GDE_PLATFORM_PROVIDER_FAMILY", _cpp_string_define_value(selected_provider["family"])),
        ("CAMBANG_GDE_PLATFORM_BACKED_COMPILED", "1" if selected_provider["implemented"] else "0"),
        ("CAMBANG_GDE_PLATFORM_PROVIDER_STATUS", _cpp_string_define_value(gde_platform_provider_status)),
    ])
    gde_env.VariantDir(gde_obj_dir, "src", duplicate=0)

    gde_out_dir = os.path.join("tests", "cambang_gde", "bin")
    os.makedirs(gde_out_dir, exist_ok=True)

    godot_cpp_libdir = os.path.join("thirdparty", "godot-cpp", "bin")
    godot_cpp_libname = f"godot-cpp.{gde_platform}.{godot_target}.{env['arch']}"

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
    if gde_platform == "windows" and windows_uses_mingw:
        godot_cpp_args.append("use_mingw=yes")
        if env["mingw_prefix"]:
            godot_cpp_args.append(f"mingw_prefix={env['mingw_prefix']}")
        if env["use_llvm"] == "yes":
            godot_cpp_args.append("use_llvm=yes")
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

    if selected_provider["implemented"]:
        provider_source_parts = selected_provider["location"].split(os.sep)[1:]
        gde_sources += _glob_cpp(gde_obj_dir, *provider_source_parts)
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

    gde_target = _gde_artifact_base(gde_platform, godot_target, env["arch"])
    gde_env["SHLIBSUFFIX"] = _gde_shared_library_suffix(gde_platform)

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


selected_build_nodes = []
if build_maintainer_tools:
    selected_build_nodes.append(maintainer_tools_alias)
if build_gde:
    selected_build_nodes.append(gde_alias)
if build_platform_runtime_validate:
    selected_build_nodes.append(platform_runtime_validate_alias)

cambang_alias = Alias("cambang", selected_build_nodes)
AlwaysBuild(cambang_alias)

gde_all_alias = Alias("gde_all", [])
godot_cpp_alias = Alias("godot_cpp", godot_cpp_build if build_gde_graph else [])
all_build_nodes = list(selected_build_nodes)
if build_gde_graph:
    all_build_nodes.append(godot_cpp_alias)
if is_clean:
    all_alias = Alias("all", [])
    build_all_alias = Alias("build_all", [])
else:
    all_alias = Alias("all", all_build_nodes)
    build_all_alias = Alias("build_all", all_alias)
AlwaysBuild(all_alias)
AlwaysBuild(build_all_alias)

cambang_clean_outputs = _unique_sources(
    maintainer_tools_clean_outputs
    + gde_all_clean_outputs
    + _all_platform_runtime_validate_clean_outputs()
    + [env["COMPDB_PATH"]]
)
Clean(maintainer_tools_alias, maintainer_tools_clean_outputs)
Clean(gde_alias, gde_clean_outputs)
Clean(gde_all_alias, gde_all_clean_outputs)
Clean(platform_runtime_validate_alias, platform_runtime_validate_clean_outputs)
Clean(cambang_alias, cambang_clean_outputs)
Clean(godot_cpp_alias, godot_cpp_clean_outputs)
Clean(all_alias, cambang_clean_outputs)
Clean(all_alias, godot_cpp_clean_outputs)
Clean(build_all_alias, cambang_clean_outputs)
Clean(build_all_alias, godot_cpp_clean_outputs)

if not is_clean:
    AddPostAction(all_alias, env["COMPDB_WRITE_ACTION"])

Default(all_alias)
