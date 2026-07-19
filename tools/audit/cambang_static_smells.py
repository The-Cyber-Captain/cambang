#!/usr/bin/env python3
"""CamBANG-specific advisory smell scanner (docs/dev/static_analysis.md).

Scans src/ (never thirdparty/, never build output) for the project-specific
smell patterns that generic tooling misses, emitting the documented
file:line / smell / reason / audit-lens shape. Output is audit *leads* for
human triage -- this script proves nothing and gates nothing. Exit status
reflects execution health only, never finding counts.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_ROOT = os.path.join(REPO_ROOT, "src")

# (smell, compiled regex, reason, audit lens). Heuristic by design; the
# baseline report (docs/dev/audit_baselines/) records which of these are
# noisy in practice so later audits do not rediscover the noise.
LINE_SMELLS = [
    ("thread-detach", re.compile(r"\.detach\s*\("),
     "detached threads defeat deterministic teardown; CamBANG requires joinable ownership.",
     "threading/teardown"),
    ("raw-new", re.compile(r"(^|[^_\w>])new\s+[A-Za-z_]"),
     "raw ownership outside approved wrappers risks leaks on early-return/exception paths "
     "(cf. the FrameReleaseLease manual-release contract).",
     "ownership/lifetime"),
    ("raw-delete", re.compile(r"(^|[^_\w])delete\s"),
     "manual delete implies a manual ownership contract; verify every path releases exactly once.",
     "ownership/lifetime"),
    ("getenv", re.compile(r"\bgetenv\s*\("),
     "persistent environment diagnostic knobs are discouraged unless documented as retained "
     "maintainer tooling (docs/dev/maintainer_tools.md).",
     "diagnostics/temp-code"),
    ("todo-marker", re.compile(r"\b(TODO|FIXME|TEMP)\b"),
     "temporary markers must not persist in production code (cpp_code_quality_policy.md).",
     "diagnostics/temp-code"),
    ("std-endl", re.compile(r"std::endl"),
     "std::endl flushes; in hot paths prefer '\\n'.",
     "performance/hot-path"),
    ("reinterpret-cast", re.compile(r"\breinterpret_cast\s*<"),
     "verify the ABI/byte-layout justification is written at the site (waiver posture).",
     "casts/abi-boundary"),
    ("const-cast", re.compile(r"\bconst_cast\s*<"),
     "logical-vs-physical constness must be deliberate and commented.",
     "casts/constness"),
    ("ignored-post-result", re.compile(r"^\s*(?:try_post|core_thread_\.try_post)\w*\s*\("),
     "an unconsumed PostResult silently drops retained-truth work; consume or (void) with rationale.",
     "mailbox/admission"),
    ("free-rid", re.compile(r"\bfree_rid\s*\("),
     "RID release must go through the approved render-thread drain paths, never an arbitrary thread.",
     "render-thread/ownership"),
    ("binding-latest", re.compile(r"bind_method\([^)]*latest", re.IGNORECASE),
     "public API names containing 'latest' leak internal freshness semantics.",
     "public-api/naming"),
    ("godot-binding-surface", re.compile(r"ClassDB::bind_method|ADD_SIGNAL|BIND_ENUM_CONSTANT|BIND_CONSTANT"),
     "inventory line: the Godot public binding surface is locked; diff this inventory against "
     "the baseline to spot unauthorized additions/removals.",
     "public-api/lock"),
]

# Window (lines) for the lock-then-callback heuristic.
LOCK_CALLBACK_WINDOW = 6
LOCK_RE = re.compile(r"\b(lock_guard|unique_lock|scoped_lock)\s*<")
CALLBACK_RE = re.compile(r"(->on_[a-z_]+\(|callbacks_->|emit_signal|hook\(|\.call\()")


def scan_file(path: str) -> list[str]:
    rel = os.path.relpath(path, REPO_ROOT).replace("\\", "/")
    findings: list[str] = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError as exc:
        print(f"ERROR: cannot read {rel}: {exc}", file=sys.stderr)
        raise

    lock_lines: list[int] = []
    for lineno, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("//"):
            # Comments still matter for todo-marker; skip other smells there.
            if LINE_SMELLS[4][1].search(line):
                smell, _, reason, lens = LINE_SMELLS[4]
                findings.append(_emit(rel, lineno, smell, reason, lens))
            continue
        for smell, pattern, reason, lens in LINE_SMELLS:
            if pattern.search(line):
                findings.append(_emit(rel, lineno, smell, reason, lens))
        if LOCK_RE.search(line):
            lock_lines.append(lineno)
        elif CALLBACK_RE.search(line):
            for lock_line in lock_lines:
                if 0 < lineno - lock_line <= LOCK_CALLBACK_WINDOW:
                    findings.append(_emit(
                        rel, lineno, "lock-then-callback",
                        f"call shape within {LOCK_CALLBACK_WINDOW} lines of a lock acquisition "
                        f"(line {lock_line}); callbacks under locks invite re-entry deadlocks. Heuristic.",
                        "locking/re-entry"))
                    break
    return findings


def _emit(rel: str, lineno: int, smell: str, reason: str, lens: str) -> str:
    return f"{rel}:{lineno}\nSmell: {smell}\nReason: {reason}\nAudit lens: {lens}\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smell", default="",
                        help="restrict output to one smell name")
    parser.add_argument("--summary", action="store_true",
                        help="print per-smell counts only")
    args = parser.parse_args()

    all_findings: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(SRC_ROOT):
        for name in sorted(filenames):
            if name.endswith((".cpp", ".cc", ".cxx", ".h", ".hpp")):
                all_findings.extend(scan_file(os.path.join(dirpath, name)))

    if args.smell:
        all_findings = [f for f in all_findings if f"\nSmell: {args.smell}\n" in f]

    if args.summary:
        counts: dict[str, int] = {}
        for finding in all_findings:
            smell = finding.split("\nSmell: ")[1].split("\n")[0]
            counts[smell] = counts.get(smell, 0) + 1
        for smell in sorted(counts, key=counts.get, reverse=True):
            print(f"{counts[smell]:5d}  {smell}")
        print(f"{len(all_findings):5d}  TOTAL")
    else:
        print("\n".join(all_findings))
    return 0


if __name__ == "__main__":
    sys.exit(main())
