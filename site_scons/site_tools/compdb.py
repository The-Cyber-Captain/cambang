import os
import json
import shlex
from SCons.Action import Action

def _as_str_list(cmd):
    if isinstance(cmd, str):
        return shlex.split(cmd, posix=False)
    if isinstance(cmd, (list, tuple)):
        return [str(x) for x in cmd]
    return [str(cmd)]

def _is_cpp_source(path):
    p = str(path).lower()
    return p.endswith((".c", ".cc", ".cpp", ".cxx", ".c++", ".mm", ".m"))

def _is_object_file(path):
    p = str(path).lower()
    return p.endswith((".o", ".obj"))

def generate(env):
    entries = []
    out_path = os.path.abspath(env.get("COMPDB_PATH", "compile_commands.json"))

    def record_command(target, source, env):
        if not target or not source:
            return None

        tgt = str(target[0])
        src = str(source[0])

        if not _is_object_file(tgt):
            return None
        if not _is_cpp_source(src):
            return None

        # Shared-object compiles are the important ones for your GDExtension build.
        cmd = (
            env.subst("$SHCXXCOM", target=target, source=source) or
            env.subst("$CXXCOM",   target=target, source=source) or
            env.subst("$SHCCCOM",  target=target, source=source) or
            env.subst("$CCCOM",    target=target, source=source)
        )
        if not cmd or cmd.strip() == "":
            return None

        entries.append({
            "directory": os.path.abspath(str(env.Dir("#"))),
            "file": os.path.abspath(src),
            "command": cmd,
            "arguments": _as_str_list(cmd),
        })
        return None

    def write_compdb(target=None, source=None, env=None):
        seen = set()
        unique = []
        for e in entries:
            key = (e["file"], e["command"])
            if key in seen:
                continue
            seen.add(key)
            unique.append(e)

        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(unique, f, indent=2)

        print(f"[compdb] wrote {out_path} ({len(unique)} entries)")

    def wrap(varname):
        act = env.get(varname)
        if not act:
            return
        base = act if isinstance(act, list) else [act]
        env[varname] = base + [Action(record_command, cmdstr="")]

    # Wrap both normal + shared compile actions
    wrap("CCCOM");   wrap("CXXCOM")
    wrap("SHCCCOM"); wrap("SHCXXCOM")

    # Expose the write action so SConstruct can attach it to a target/alias
    env["COMPDB_WRITE_ACTION"] = Action(write_compdb, cmdstr="")

def exists(env):
    return True