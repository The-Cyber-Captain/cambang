# Current tranche

## Bounded provider/Core transport

### Goal

Make provider-to-Core transport genuinely bounded and implement the documented
fatal transport-failure behaviour without weakening registry, snapshot,
ownership, or restart contracts.

### Architectural authority

Implement the provider transport-failure and Core truth-loss contracts documented
in:

* `docs/provider_architecture.md`
* `docs/core_runtime_model.md`

If the required contracts are not present and internally consistent in those
documents, stop before implementation and report the missing documentation.

### Scope

* Hard-bound `CBProviderStrand` storage.
* Hard-bound `CoreThread::essential_tasks_`.
* Preserve repeating-stream-frame pressure dropping and exact release.
* Treat failure to admit non-lossy provider truth as fatal to the active
  generation.
* Propagate strand failure to Core through a non-allocating out-of-band path.
* Quarantine registry-derived snapshot publication at the documented truth-loss
  boundary.
* Distinguish provider-derived tasks from teardown-critical Core work where
  required.
* Contain provider-strand callback and allocation failures.
* Preserve deterministic stop and restart.

### Required implementation

* Queue storage never exceeds its configured or named capacity.
* Non-lossy strand traffic may reclaim queued repeating frames, but is never
  silently discarded.
* Unrecoverable non-lossy admission failure latches one fatal reason and closes
  further admission.
* Fatal notification does not traverse a failed queue.
* Essential `QueueFull` and `AllocFail` latch Core transport failure.
* Already-queued and already-local work obeys the documented truth-loss rules.
* Rejected or discarded payload-owning work releases ownership exactly once.
* `flush()`, stop, teardown, and restart remain deterministic.
* Exceptions do not escape provider or Core worker-thread boundaries.

### Verification

Add deterministic checks for:

* hard queue bounds;
* repeating-frame drop and reclamation;
* non-lossy saturation;
* one-shot fatal propagation;
* callback exception containment;
* exact lease/release and ingress-depth accounting;
* no registry or snapshot mutation beyond the truth-loss boundary;
* last coherent snapshot and completed-stop `NIL` behaviour;
* teardown-critical work after fault;
* stop and restart with a fresh generation.

Preserve existing provider-compliance, restart, capture, result, and snapshot
verification.

### Exclusions

* No Godot public API change.
* No immediate-on-fault `NIL` change.
* No ProviderBroker invocation-lock redesign.
* No authoritative-fact coalescing.
* No bounded-marshalling changes.
* No diagnostic or environment-variable cleanup.
* No unrelated scheduler framework or refactoring.
* No weakening of tests.

### Completion criteria

* Both transport queues are genuinely bounded.
* Admission failure of authoritative provider truth is visible and fatal.
* Registry and snapshot behaviour matches the permanent truth-loss contract.
* Payload ownership remains exact.
* Fatal teardown and subsequent restart are deterministic.
* Required verifier suites pass.
