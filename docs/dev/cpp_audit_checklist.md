# CamBANG C++ Audit Checklist

Use this checklist for read-only source audits, Codex review prompts, and human review of changed C++ code.

This checklist is intentionally practical. It is not a formatting guide. The goal is to find correctness, ownership, lifetime, threading, failure-visibility, provider-boundary, public API, and test-integrity risks.

## Required finding format

For each finding, report:

```text
Finding:
Severity:
File/function:
Evidence:
Applicable guideline or CamBANG policy:
Risk:
Suggested correction path:
Scope:
Can tooling catch this class of issue?:
```

Severity:

| Severity | Meaning |
| --- | --- |
| Blocker | Undefined behaviour, data race, lifetime hazard, leak, double release, wrong-thread release, public API contract break, render-thread hazard, or silent failure in a critical path. |
| Major | Design or implementation likely to fail under plausible provider, restart, teardown, concurrency, or platform-backed conditions. |
| Minor | Local maintainability issue, unclear ownership, weak naming, avoidable complexity, missing validation, or excessive coupling. |
| Note | Style-only or future cleanup; should not block unless repeated widely or masking a real issue. |

## A. Scope and authority

- Is the audit read-only unless explicitly authorized otherwise?
- Is the supplied source tree treated as the source authority?
- Are conclusions grounded in actual files, not memory of previous repo state?
- Is the Godot public API lock respected for this tranche?
- Are tests/verifiers preserved as meaningful checks rather than weakened to pass?
- Is the finding a real project risk rather than a personal style preference?

## B. Ownership and lifetime

- Is ownership represented by value, RAII wrapper, `std::unique_ptr`, intentional `std::shared_ptr`, Godot `Ref<>`, or an explicit project handle?
- Are raw pointers and raw references clearly non-owning?
- Is any raw pointer stored? If so, is owner lifetime/invalidation explicit?
- Are copy/move operations deliberately defaulted, deleted, or implemented for resource-owning types?
- Can a resource be leaked, double-released, or released after owner destruction?
- Can destruction happen while holding a broad lock?
- Does a posted lambda capture anything that may be destroyed before execution?
- Are `shared_ptr` cycles possible?
- Is a `weak_ptr` lock result checked before use?
- Are Godot `Ref<>` lifetimes separated from internal provider/core lifetimes?

High-risk CamBANG areas:

- retained snapshots;
- retained stream results;
- `CamBANGCaptureResult` and result members;
- display-view textures;
- GPU backing objects;
- RD texture RIDs;
- frame-buffer leases;
- provider callbacks;
- stop/restart/teardown.

## C. Threading, locks, and callbacks

- Are shared mutable fields protected by a clear lock or atomic protocol?
- Is the lock guarding exactly the state it is assumed to guard?
- Are callbacks invoked outside locks?
- Are blocking operations performed outside locks?
- Are lock acquisition orders consistent?
- Are atomics used with a clear memory-ordering story?
- Is `volatile` avoided for synchronization?
- Is thread affinity documented where it matters?
- Could a teardown path race with a callback or posted task?
- Could a public call observe partially torn-down state?

Flag immediately:

```cpp
lock();
callback();
unlock();
```

```cpp
std::atomic<bool> ready;
// No documented relationship to protected state.
```

```cpp
resource.release(); // thread affinity unclear
```

## D. Render-thread and GPU resource safety

- Are render-server/RD resources released through the approved drain path?
- Is RID release deferred where required?
- Are display delegates released on the correct thread/path?
- Does any Godot object destructor perform unsafe core/render cleanup directly?
- Are GPU-only and CPU-staging result paths kept truthful?
- Is CPU readback/fallback bounded and failure-visible?
- Is display demand retained/released with a clear lease lifetime?
- Can teardown occur while a display view still exists?
- Are retained GPU resources separated from user-facing proxy objects?

Flag immediately:

- direct RID release outside the approved drain path;
- blocking render-thread cleanup;
- resource release under broad core locks;
- code that assumes TextureRect/Godot UI release order is deterministic.

## E. Provider callback ingress

For every provider-to-core frame/command ingress path, check:

- provider/session/generation identity;
- device id/hardware id routing;
- acquisition session routing;
- stream routing;
- stream/session liveness;
- teardown state;
- frame width/height;
- format/FourCC;
- stride;
- payload byte size;
- timestamp behaviour;
- CPU/GPU payload kind;
- frame-buffer lease ownership;
- queue/post result;
- accounting for queue full/closed/allocation failure;
- release-on-drop consistency.

Required posture:

- Dropped frames must be classified.
- Queue/post failure must be accounted.
- Frame-buffer leases must be released exactly once.
- Ingress depth/counters must remain consistent across success and failure paths.
- Null-core/closed-core defensive paths must use the same accounting discipline as normal post failure.

## F. Error handling and failure visibility

- Is every provider/core operation result checked?
- Are enqueue/post/admission failures handled?
- Are unsupported operations visible as such?
- Does a public API return success only after the intended work is accepted?
- Are stale-generation/session errors distinguishable from invalid caller state?
- Is “best effort” documented where it is intentional?
- Are logs/counters/snapshots sufficient to diagnose failure?
- Are exceptions avoided across Godot/public/thread boundaries?
- Are `noexcept` functions genuinely non-throwing?

Flag immediately:

```cpp
action_that_returns_status(); // result ignored
```

```cpp
return OK; // work was not accepted, only requested locally
```

## G. Snapshot truth and publication

- Does snapshot state reflect retained core truth rather than UI assumptions?
- Does publication preserve baseline and version/topology semantics?
- Is `get_state_snapshot()` passive where required?
- Does signal emission advance only on actual consumption/publication?
- Are destroyed/tearing-down objects represented truthfully?
- Are native object registry phases consistent with lifecycle events?
- Are derived counters labelled and scoped correctly?
- Does code avoid fabricating parent/ancestor relationships?
- Does status panel interpretation remain subordinate to snapshot truth?

## H. Godot public boundary

- Is the locked Godot API unchanged unless a separately approved correctness issue requires change?
- Are object-level APIs preferred for normal users?
- Are internal ids avoided in normal public workflows?
- Are advanced id-based APIs kept on server/dev/scenario tooling?
- Are public wrappers truthful about readiness and failure?
- Are pre-baseline and post-baseline behaviours explicit?
- Are startup pending intents deterministic and bounded where applicable?
- Is public state reset truthfully after stop?
- Are Godot `Dictionary`/`Variant` shapes stable where documented?

Flag immediately:

- public method rename for style only;
- reintroduction of “latest” into public names where current/retained/object-level terminology is intended;
- public API returning stale objects after stop;
- calls that require an undocumented tick ordering.

## I. Provider/core separation

- Does core own orchestration and truth?
- Do providers own platform interaction?
- Are provider quirks normalized at the provider boundary?
- Does core avoid Windows MF, Android Camera2, SyntheticProvider-only, or renderer-specific details?
- Does public API avoid being shaped around SyntheticProvider convenience?
- Are provider capability seams explicit?
- Are provider start/stop failures visible?
- Are callbacks after teardown rejected as stale rather than accepted accidentally?
- Are scenario/synthetic tools kept separate from normal public UX automation?

## J. Type safety and schema values

- Are enums scoped internally where practical?
- Are serialized/schema/platform-sized values using appropriate fixed-width types?
- Are generation/version/topology counters explicitly modelled?
- Are native ids, stream ids, hardware ids, and capture ids hard to confuse?
- Are frame dimensions, stride, byte size, and FourCC validated before use?
- Are capture member indices contiguous and role-aware where required?
- Are optional values represented explicitly?
- Are narrowing conversions avoided?
- Are casts minimized?
- Is every `reinterpret_cast` isolated and justified?
- Is every `const_cast` isolated and justified?

## K. Initialization and invariants

- Are all fields initialized?
- Do constructors establish meaningful invariants?
- Are invalid states impossible where practical?
- Are phase transitions explicit?
- Are destroyed/tearing-down objects prevented from behaving as live?
- Are default still/stream profiles seeded deliberately?
- Are reset paths symmetric with construction/start paths?
- Are repeated start/stop/restart cases accounted for?

## L. Performance and allocation discipline

- Is there avoidable per-frame allocation?
- Is there avoidable per-frame copying?
- Are large buffers moved rather than copied?
- Are views/spans used for non-owning access where suitable?
- Are queues bounded?
- Is backpressure visible?
- Are hot-path logs avoided or gated appropriately?
- Is `std::endl` avoided in ordinary logging?
- Are snapshot dictionary conversions bounded and purposeful?
- Is performance improved without weakening ownership, failure visibility, or teardown safety?

## M. Headers and includes

- Is the header self-contained?
- Does the `.cpp` include its corresponding header first where applicable?
- Are directly used declarations directly included?
- Are transitive include dependencies avoided?
- Are forward declarations used where helpful and safe?
- Are platform/Godot/provider-heavy includes kept out of core headers where possible?
- Are implementation-only helpers kept in `.cpp` files?
- Is `using namespace ...` absent from headers?

## N. Macros, build switches, and diagnostics

- Could a macro be a `constexpr`, inline function, template, or enum instead?
- Is the macro required for platform/API/export glue?
- Are diagnostic environment variables avoided unless accepted as maintainer tooling?
- Are temporary logs/checkpoints removed after investigation?
- Are debug-only paths prevented from becoming runtime architecture?
- Are dormant services documented with purpose, non-purpose, and activation conditions?
- Are TODO/TEMP/FIXME comments either current and actionable or removed?

Search-sensitive terms:

```text
TODO
FIXME
TEMP
DEBUG
checkpoint
phase trace
getenv
std::endl
reinterpret_cast
const_cast
detach()
latest
```

## O. Tests and verification

- Are Godot scenes preserved as boundary checks?
- Are CLI smoke/case verifiers adapted only to reflect real intended semantics?
- Are tests not weakened merely to pass?
- Are renderer-specific expectations explicit?
- Are expected ERROR logs deliberate and bounded?
- Are known anomalies documented rather than hidden?
- Are synthetic tests prevented from encoding provider-universal assumptions unless intended?
- Does the audit recommend verification commands appropriate to the changed area?

## P. Review conclusion template

End each audit with:

```text
Conclusion:
- Blockers:
- Major findings:
- Minor findings:
- Notes:
- Recommended narrow action:
- Tooling that would have helped:
- Verification recommended after any fix:
```

If no concrete issue is found, say so explicitly and identify what was inspected.
