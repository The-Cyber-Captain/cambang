#!/usr/bin/env python3
"""Advisory changed-file clang-tidy runner (docs/dev/static_analysis.md).

Runs clang-tidy with the repo compile database over the changed src/ C++
translation units (default), or over every compiled src/ TU (--all).

Posture: advisory. The exit status reflects tool execution health only --
nonzero means clang-tidy itself failed to run or a TU failed to parse, never
that findings exist. Findings are audit leads for human triage
(docs/dev/static_analysis.md, "Triage categories").

Changed headers are not analyzed directly (clang-tidy needs a TU); header
findings surface through HeaderFilterRegex when an including TU is analyzed.
If only headers changed, run with --all or name an including TU explicitly.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COMPDB_PATH = os.path.join(REPO_ROOT, "compile_commands.json")


def repo_relative(path: str) -> str:
    return os.path.relpath(os.path.abspath(path), REPO_ROOT).replace("\\", "/")


def compdb_src_files() -> list[str]:
    with open(COMPDB_PATH, "r", encoding="utf-8") as f:
        entries = json.load(f)
    files = []
    for entry in entries:
        rel = repo_relative(entry["file"])
        if rel.startswith("src/") and rel.endswith((".cpp", ".cc", ".cxx")):
            files.append(rel)
    return sorted(set(files))


def changed_src_files(base: str) -> list[str]:
    def git_lines(*args: str) -> list[str]:
        out = subprocess.run(
            ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True, check=True
        ).stdout
        return [line.strip() for line in out.splitlines() if line.strip()]

    names = set(git_lines("diff", "--name-only", f"{base}...HEAD"))
    # Include uncommitted work so pre-commit runs see the actual working tree.
    names.update(git_lines("diff", "--name-only"))
    names.update(git_lines("diff", "--name-only", "--cached"))
    analyzable = set(compdb_src_files())
    return sorted(n.replace("\\", "/") for n in names if n.replace("\\", "/") in analyzable)


def run_one(rel_path: str) -> tuple[str, int, str]:
    proc = subprocess.run(
        ["clang-tidy", rel_path, "-p", "."],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    # clang-tidy exits nonzero on real errors (missing compdb entry, parse
    # failure), not on ordinary warnings.
    return rel_path, proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/main",
                        help="git base ref for changed-file resolution (default origin/main)")
    parser.add_argument("--all", action="store_true",
                        help="analyze every compiled src/ TU instead of changed files")
    parser.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                        help="parallel clang-tidy processes")
    parser.add_argument("-o", "--output", default="",
                        help="also write the combined report to this file")
    parser.add_argument("files", nargs="*",
                        help="explicit TUs to analyze (overrides changed-file resolution)")
    args = parser.parse_args()

    if not os.path.exists(COMPDB_PATH):
        print("ERROR: compile_commands.json missing; run the SCons build first "
              "(docs/dev/build_and_scaffolding.md).", file=sys.stderr)
        return 1

    if args.files:
        targets = [repo_relative(f) for f in args.files]
    elif args.all:
        targets = compdb_src_files()
    else:
        targets = changed_src_files(args.base)

    if not targets:
        print("No analyzable changed src/ TUs. (Headers-only change? See --help.)")
        return 0

    print(f"clang-tidy over {len(targets)} TU(s), jobs={args.jobs}")
    failures: list[str] = []
    chunks: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for rel_path, code, text in pool.map(run_one, targets):
            header = f"\n===== {rel_path} (clang-tidy exit {code}) =====\n"
            sys.stdout.write(header + text)
            chunks.append(header + text)
            if code != 0:
                failures.append(rel_path)

    if args.output:
        with open(os.path.join(REPO_ROOT, args.output), "w", encoding="utf-8") as f:
            f.writelines(chunks)
        print(f"\nreport written to {args.output}")

    if failures:
        print(f"\nTOOL EXECUTION FAILURES ({len(failures)}): " + ", ".join(failures),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
