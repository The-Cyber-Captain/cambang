# AGENTS.md

## Default workflow

Treat the repository source as authoritative.

For repo-specific architecture and review guidance, read `docs/dev/agent_context.md`.

Before changing code:

* Inspect the relevant source first.
* Prefer minimal, high-confidence changes.
* State the files expected to change when the task is non-trivial.
* Do not broaden scope without explaining why.

Do not:

* Weaken tests, smoke tools, or Godot verification scenes merely to get PASS.
* Change locked public API surfaces unless explicitly instructed.
* Add persistent environment variables or diagnostic knobs for temporary investigation.
* Modify generated build outputs.
* Commit unless explicitly asked.

After changes:

* Report changed files.
* Explain the design rationale.
* List validation commands.
* Say plainly which validation was not run.

## Project priorities

* Snapshot truth over cosmetic simplicity.
* Thread safety and deterministic teardown over speculative performance.
* Avoid new abstractions unless they clearly earn their keep.
* Preserve provider/platform seams.
* Keep normal user-facing APIs simple; keep advanced/debug tooling separate.

## Testing expectations

Manual local validation remains authoritative.

Do not assume cloud or sandbox validation proves Windows, Godot, hardware, GPU, or platform-provider behaviour unless those paths were actually exercised.
