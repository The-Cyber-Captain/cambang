# Current tranche

## Bounded synchronous Core marshalling

### Goal

Remove indefinite caller waits from production-facing synchronous CoreRuntime
wrappers while preserving successful-path behaviour, Core-thread ownership,
queue-lane selection, and the locked Godot public API.

### Required behaviour

- Calls made from the Core thread continue to execute directly.
- Cross-thread calls confirm successful Core task admission before waiting.
- Every caller wait is finite and uses one named internal timeout policy.
- Enqueue rejection, timeout, shutdown, task exception, and completion failure
  return the method's existing conservative failure or unavailable result.
- Exceptions do not escape Core-thread, Godot, or public boundaries.
- A caller timeout does not imply that admitted Core work was cancelled.
- Posted work remains lifetime-safe if it executes after the caller has timed out.
- Successful calls retain their current observable semantics.

### Scope

Primary implementation:

- `src/core/core_runtime.cpp`

Supporting implementation or declarations may be added only where needed to
avoid duplicated unsafe marshalling logic.

Add or adapt focused verification for:

- successful cross-thread completion;
- queue admission rejection;
- caller timeout;
- stop racing an admitted synchronous request;
- task-body exception containment;
- Core-thread direct execution;
- late execution after caller timeout without dangling captures.

### Exclusions

- No Godot public API changes.
- No provider ordering changes.
- No provider-strand or Core essential-queue capacity changes.
- No ProviderBroker locking redesign.
- No timing-diagnostic cleanup.
- No environment-variable cleanup.
- No broad unrelated refactoring.
- No weakening of existing tests or verifiers.

### Completion criteria

- No production-facing CoreRuntime wrapper in scope performs an unbounded
  `future::get()` or `future::wait()`.
- All new failure paths are conservative and visible through existing return
  contracts.
- Existing success-path verification remains green.
- New bounded-wait and stop-race verification is deterministic and green.
- Documentation accurately describes timeout as caller-side bounded waiting,
  not cancellation.