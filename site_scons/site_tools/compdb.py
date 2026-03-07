import json
import os
import shlex
from SCons.Action import Action

_CPP_EXTS = (".c", ".cc", ".cpp", ".cxx", ".c++", ".mm", ".m")
_OBJ_EXTS = (".o", ".obj")
_COMPILE_VARS = ("SHCXXCOM", "CXXCOM", "SHCCCOM", "CCCOM")


def _norm_path(path):
    return os.path.abspath(str(path)).replace("\\", "/")


def _quote_arg(value):
    value = str(value)
    if not value:
        return '""'
    if any(ch in value for ch in ' \t"'):
        return '"' + value.replace('"', '\\"') + '"'
    return value


def _is_cpp_source(path):
    return str(path).lower().endswith(_CPP_EXTS)


def _is_object_file(path):
    return str(path).lower().endswith(_OBJ_EXTS)


def _render_action(env, action, target, source):
    """Render the first real compile command from an SCons action/list."""
    if not action:
        return ""

    actions = action if isinstance(action, (list, tuple)) else [action]
    for item in actions:
        text = env.subst(str(item), target=target, source=source).strip()
        if not text:
            continue
        # Skip Python-function style actions or placeholders.
        if "function_action" in text.lower():
            continue
        if text == "None":
            continue
        return text
    return ""


def _ensure_source_in_command(cmd, src, src_abs):
    if src in cmd or src_abs in cmd:
        return cmd
    return f"{cmd} {_quote_arg(src_abs)}"


def _ensure_build_dir_include(cmd, directory):
    include_unquoted = f"-I{directory}"
    include_quoted = f'-I"{directory}"'
    if include_unquoted in cmd or include_quoted in cmd:
        return cmd
    include_flag = include_quoted if any(ch in directory for ch in ' \t"') else include_unquoted
    return f"{cmd} {include_flag}"


def generate(env):
    entries = []
    out_path = _norm_path(env.get("COMPDB_PATH", "compile_commands.json"))
    directory = _norm_path(env.Dir("#"))

    # Snapshot original compile actions before wrapping them.
    original_actions = {var: env.get(var) for var in _COMPILE_VARS}

    def record_command(target, source, env):
        if not target or not source:
            return None

        tgt = str(target[0])
        src = str(source[0])
        if not _is_object_file(tgt) or not _is_cpp_source(src):
            return None

        cmd = ""
        for var in _COMPILE_VARS:
            cmd = _render_action(env, original_actions.get(var), target, source)
            if cmd:
                break
        if not cmd:
            return None

        src_abs = _norm_path(src)
        cmd = _ensure_source_in_command(cmd, src, src_abs)
        cmd = _ensure_build_dir_include(cmd, directory)

        entries.append({
            "directory": directory,
            "file": src_abs,
            "command": cmd,
        })
        return None

    def write_compdb(target=None, source=None, env=None):
        seen = set()
        unique = []
        for entry in entries:
            key = (entry["file"], entry["command"])
            if key in seen:
                continue
            seen.add(key)
            unique.append(entry)

        unique.sort(key=lambda e: (e["file"], e["command"]))

        if not unique:
            if os.path.exists(out_path):
                print(f"[compdb] no entries captured in this run; preserving existing {out_path}")
            else:
                print(f"[compdb] no entries captured in this run; not writing empty database {out_path}")
            return None

        out_dir = os.path.dirname(out_path) or "."
        os.makedirs(out_dir, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as fh:
            json.dump(unique, fh, indent=2)
            fh.write("\n")

        print(f"[compdb] wrote {out_path} ({len(unique)} entries)")
        return None

    def wrap(varname):
        action = env.get(varname)
        if not action:
            return
        base = action if isinstance(action, list) else [action]
        env[varname] = base + [Action(record_command, cmdstr="")]

    for var in _COMPILE_VARS:
        wrap(var)

    env["COMPDB_WRITE_ACTION"] = Action(write_compdb, cmdstr="")


def exists(env):
    return True
