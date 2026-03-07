import json
import os
from SCons.Action import Action


def _is_cpp_source(path):
    p = str(path).lower()
    return p.endswith((".c", ".cc", ".cpp", ".cxx", ".c++", ".m", ".mm"))


def _is_object_file(path):
    p = str(path).lower()
    return p.endswith((".o", ".obj"))


def _flatten_actions(action):
    if action is None:
        return []
    if isinstance(action, (list, tuple)):
        out = []
        for item in action:
            out.extend(_flatten_actions(item))
        return out
    return [action]


def _render_action(env, action, target, source):
    """Return the first real command-line action as a string.

    We must ignore Python/function actions such as our own record hook.
    """
    for item in _flatten_actions(action):
        text = ""

        # SCons command actions usually support genstring(); function actions
        # either stringify to placeholders or names that must not enter compdb.
        if hasattr(item, "genstring"):
            try:
                text = item.genstring(target, source, env)
            except TypeError:
                try:
                    text = item.genstring(target, source, env, False)
                except Exception:
                    text = ""
            except Exception:
                text = ""
        else:
            text = str(item)

        if not text:
            continue

        lowered = text.lower()
        if "record_command" in lowered or "functionaction" in lowered:
            continue
        if text.startswith("<") and text.endswith(">"):
            continue

        rendered = env.subst(text, target=target, source=source).strip()
        if rendered:
            return rendered

    return ""


def generate(env):
    entries = []
    out_path = os.path.abspath(env.get("COMPDB_PATH", "compile_commands.json"))
    root_dir = os.path.abspath(str(env.Dir("#")))

    # Snapshot original compile actions *before* wrapping them, otherwise our
    # own record hook can leak into the rendered command line.
    original_actions = {
        name: env.get(name)
        for name in ("SHCXXCOM", "CXXCOM", "SHCCCOM", "CCCOM")
    }

    def record_command(target, source, env):
        if not target or not source:
            return None

        tgt = str(target[0])
        src = str(source[0])

        if not _is_object_file(tgt):
            return None
        if not _is_cpp_source(src):
            return None

        cmd = (
            _render_action(env, original_actions["SHCXXCOM"], target, source)
            or _render_action(env, original_actions["CXXCOM"], target, source)
            or _render_action(env, original_actions["SHCCCOM"], target, source)
            or _render_action(env, original_actions["CCCOM"], target, source)
        )
        if not cmd:
            return None

        try:
            file_path = source[0].srcnode().abspath
        except Exception:
            file_path = os.path.abspath(src)

        entries.append(
            {
                "directory": root_dir,
                "file": os.path.abspath(file_path),
                "command": cmd,
            }
        )
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

        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as handle:
            json.dump(unique, handle, indent=2)
            handle.write("\n")

        print(f"[compdb] wrote {out_path} ({len(unique)} entries)")
        return None

    def wrap(varname):
        action = env.get(varname)
        if not action:
            return
        base = list(action) if isinstance(action, list) else [action]
        env[varname] = base + [Action(record_command, cmdstr="")]

    wrap("CCCOM")
    wrap("CXXCOM")
    wrap("SHCCCOM")
    wrap("SHCXXCOM")

    env["COMPDB_WRITE_ACTION"] = Action(write_compdb, cmdstr="")


def exists(env):
    return True
