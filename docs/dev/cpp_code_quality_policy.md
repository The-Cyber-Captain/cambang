# CamBANG C++ Code Quality Policy

This document defines the C++ code-quality policy used when reviewing, auditing, or changing CamBANG source code.

The goal is not to create a cosmetic style guide. The goal is to keep CamBANG C++ code explicit, resource-safe, reviewable, deterministic at lifecycle boundaries, and consistent with the architecture of the project.

## Authority order

When guidance conflicts, use this order of authority:

1. CamBANG architectural contracts and runtime invariants.
2. Locked Godot/public API decisions for the current tranche.
3. Project language/toolchain target: C++20 as built by the supported project toolchains.
4. ISO C++ legality: the language standard defines what is valid C++; it does not, by itself, define sufficient project quality.
5. C++ Core Guidelines as the default public good-practice baseline.
6. Local file/module consistency for purely stylistic choices.
7. Static-analysis output as advisory evidence, not automatic truth.

A generic C++ style preference should not override CamBANG ownership, lifecycle, threading, provider-boundary, snapshot-truth, or Godot API constraints.

## Public references

The public baseline for this policy is:

- ISO/IEC 14882, Programming languages — C++, as the formal C++ language specification.
- C++ Core Guidelines, as the default modern C++ good-practice reference.
- LLVM clang-tidy documentation, for mechanical/static-analysis checks.
- Include-What-You-Use documentation, for include hygiene where practical.

These references guide the project, but CamBANG architecture decides how they are applied.

## Audit posture

A C++ audit finding should identify a real risk:

- correctness;
- ownership or lifetime;
- thread safety;
- render-thread safety;
- failure visibility;
- provider/core separation;
- snapshot truth;
- public API contract preservation;
- portability;
- maintainability.

Do not raise findings merely because code differs from a personal formatting preference.

Use this severity model:

| Severity | Meaning |
| --- | --- |
| Blocker | Undefined behaviour, data race, lifetime hazard, leak, double release, wrong-thread release, public API contract break, render-thread hazard, or silent failure in a critical path. |
| Major | Design or implementation likely to fail under plausible provider, restart, teardown, concurrency, or platform-backed conditions. |
| Minor | Local maintainability issue, unclear ownership, weak naming, avoidable complexity, missing validation, or excessive coupling. |
| Note | Style-only or future cleanup; should not block unless repeated widely or masking a real issue. |

Each finding should include:

```text
Finding:
Evidence:
Applicable guideline or CamBANG policy:
Risk:
Suggested correction path:
Scope:
Severity:
```

## Language and portability baseline

CamBANG currently targets C++20. Do not introduce C++23/C++26 language/library features unless the project explicitly raises the target.

Expected practice:

- Prefer standard C++ facilities where practical.
- Localize platform-specific code behind provider or platform seams.
- Encapsulate compiler, OS, Godot, platform API, Android Camera2, and renderer-specific details.
- Avoid relying on undefined, unspecified, or surprising implementation-defined behaviour.
- Treat compiler extensions as boundary-only tools, not general project style.

Audit red flags:

- Platform-specific APIs leaking into core abstractions.
- Core code learning provider-specific details.
- One platform-backed implementation's local assumptions becoming permanent architecture.
- Compiler extensions used outside narrow boundary code.

## Headers, includes, and source layout

Expected practice:

- Headers should be self-contained.
- A `.cpp` should include its corresponding header first where applicable.
- Include what is used directly; do not rely on transitive includes.
- Avoid heavy platform/Godot/provider includes in core/public headers when a forward declaration or abstraction is appropriate.
- Avoid defining non-trivial functions in headers unless needed for templates, `constexpr`, or very small inline accessors.
- Do not use `using namespace ...` in headers.
- Keep anonymous namespaces and internal linkage in `.cpp` implementation files.

CamBANG-specific expectations:

- Core headers should not casually include Godot, Windows, Android, or renderer-specific headers.
- Provider-specific headers should not become implicit architectural authority.
- Godot-facing headers may expose Godot types where that is the public boundary, but should not drag provider internals with them.

## Interfaces and function signatures

Expected practice:

- Make interfaces explicit and strongly typed.
- Prefer return values over output parameters when returning one logical result.
- Pass cheap scalar/value types by value.
- Pass larger read-only objects by `const&`.
- Pass optional non-owning nullable objects as raw pointers only where null is meaningful.
- Pass mandatory non-owning objects as references.
- Avoid adjacent primitive parameters that are easy to swap.
- Avoid public boolean-flag soup.
- Avoid clever overload sets or forwarding references unless they solve a measured problem.

CamBANG-specific expectations:

- Do not churn the locked Godot public API for style-only reasons.
- Keep normal public APIs object-level and friendly.
- Avoid exposing capture IDs to normal user workflows.
- Keep explicit ID-based access on server/dev/scenario tooling.
- Readiness/admission methods must describe the actual state they test.

## Ownership and lifetime

Expected practice:

- Use RAII for resources with paired acquire/release behaviour.
- Prefer values and scoped objects over heap allocation.
- Use `std::unique_ptr` for exclusive ownership.
- Use `std::shared_ptr` only when shared ownership is intentional.
- Use `std::weak_ptr` to break cycles where shared ownership is genuinely required.
- Treat raw pointers and raw references as non-owning by default.
- Do not transfer ownership through raw pointer/reference APIs.
- Avoid naked `new` and `delete` outside low-level wrappers.
- Avoid storing non-owning raw pointers unless lifetime is enforced by invariant or explicit invalidation.

CamBANG-specific audit focus:

- retained snapshots;
- retained stream results;
- Godot `Ref<>` lifetimes;
- GPU backing handles;
- RD texture RIDs;
- render-thread release queues;
- provider-owned frame buffers;
- cross-thread callback captures;
- teardown and restart boundaries.

A resource release path is suspicious if it:

- may run on the wrong thread;
- may block the render thread;
- releases under a broad lock;
- relies on caller ordering rather than an explicit drain path;
- silently drops a release request.

## Initialization, invariants, and object design

Expected practice:

- Initialize all variables.
- Prefer member default initializers for simple state.
- Constructors should establish invariants.
- Use `explicit` for single-argument constructors unless implicit conversion is intentional.
- Prefer the Rule of Zero.
- If a class owns a non-trivial resource, define or delete copy/move operations deliberately.
- Destructors must not throw.
- Polymorphic base classes should have virtual destructors or protected non-virtual destructors, depending on ownership model.
- Use `struct` for passive data aggregates and `class` where invariants or behaviour matter.

CamBANG-specific expectations:

- Snapshot/state structs may be aggregate-like where they represent published truth.
- Runtime/provider objects should make ownership and lifecycle explicit.
- Any object with a phase, generation, native id, thread affinity, or render-resource handle should make that invariant visible in construction and teardown.
- Destroyed or tearing-down state must not be represented by objects that remain mostly usable by accident.

## Error handling and failure visibility

Expected practice:

- Do not silently ignore failures from enqueue, admission, provider start/stop, frame ingress, resource release, or result materialization paths.
- Use explicit status/result/error values for expected failures.
- Reserve `bool` return values for predicates or trivially obvious success/failure cases.
- Include enough diagnostic context to distinguish invalid caller state, provider refusal, queue full/backpressure, unsupported operation, stale generation/session, malformed payload, and teardown races.
- Mark functions `noexcept` only when throwing is impossible or unacceptable and the implementation honours that contract.

CamBANG-specific expectations:

- Godot-facing APIs should use Godot-style `Error`, `Variant`, `Dictionary`, nullable object refs, or project-established result objects rather than introducing exception-driven public failure.
- Do not throw across Godot/public/thread boundaries.
- Internal exception use should not be introduced casually. If used at all, it must be caught at the boundary and converted into project error reporting.
- Admission failure should be visible to the caller or diagnostics. “Request accepted but nothing happens” should be treated as a bug unless explicitly documented as best-effort telemetry.

## Concurrency and threading

Expected practice:

- Avoid data races.
- Minimize shared mutable state.
- Prefer immutable snapshots, message passing, owned queues, and clear ownership over shared writable objects.
- Use RAII lock guards.
- Do not call arbitrary callbacks while holding locks.
- Do not hold locks across blocking operations.
- Use atomics only where the memory-ordering story is clear.
- Do not use `volatile` for synchronization.
- Make thread affinity explicit where it matters.

CamBANG-specific audit focus:

- core-thread ownership;
- Godot main-thread entry;
- render-thread/RD release paths;
- display bridge advancement;
- provider callback ingress;
- shutdown/restart boundaries;
- synthetic provider timing;
- retained result destruction.

Suspicious patterns include:

```cpp
lock();
callback();
unlock();
```

```cpp
std::atomic<bool> ready;
// No documented relationship to protected state or memory ordering.
```

```cpp
render_resource.free(); // from arbitrary thread
```

```cpp
queue.push(frame); // return value ignored
```

Expected shape:

```cpp
{
    std::lock_guard<std::mutex> guard(mutex_);
    // Mutate protected state only.
}
// Callbacks, release work, and heavy work happen outside the lock.
```

## Provider callback ingress

Provider-to-core frame ingress must validate enough information to avoid corrupting core truth:

- provider/session/generation identity;
- device/session/stream routing;
- dimensions;
- format/FourCC;
- stride;
- payload size;
- timestamp behaviour;
- CPU/GPU payload kind;
- ownership/lease lifetime;
- target stream/session liveness;
- teardown state;
- queue acceptance.

Dropping malformed, stale, or teardown-racing frames can be correct. Silent dropping is not good enough for auditability.

## Type safety and data modelling

Expected practice:

- Prefer `enum class` for internal enums.
- Use fixed-width integer types for serialized/schema/platform-sized values where size matters.
- Use `std::chrono` types for durations/timestamps where practical.
- Avoid magic numbers; use named constants.
- Avoid unscoped global constants unless internal linkage is clear.
- Avoid C arrays for owned storage; use `std::array`, `std::vector`, `std::span`, or project buffer types.
- Avoid narrowing conversions.
- Minimize casts.
- Treat `reinterpret_cast` as a major audit smell unless isolated in low-level ABI/buffer code.
- Use `std::optional` where absence is part of the model.
- Use tagged structs or `std::variant` where a value genuinely has several shapes.

CamBANG-specific candidates for explicit modelling:

- generation/version/topology counters;
- native ids;
- internal capture ids;
- hardware ids;
- stream ids;
- timestamps;
- frame dimensions;
- stride and byte size;
- format FourCC;
- capture member indices;
- render policy values.

## Performance and allocation discipline

Expected practice:

- Prefer clear code until profiling shows a problem.
- Avoid unnecessary per-frame allocation.
- Avoid unnecessary image/frame copies.
- Use move semantics for large owned buffers.
- Use spans/views for non-owning access.
- Reserve vector capacity when size is known.
- Avoid unbounded queues.
- Make backpressure explicit.
- Do not use `std::endl` for ordinary logging; it flushes.
- Do not use `shared_ptr` as a shortcut for unclear lifetime.

CamBANG performance-sensitive areas:

- synthetic frame generation;
- multi-stream publication;
- retained result materialization;
- GPU display-view creation;
- CPU fallback/readback;
- snapshot dictionary conversion;
- status panel update paths;
- provider frame ingress queues.

A performance fix is not acceptable if it weakens ownership clarity, hides failure, or breaks teardown determinism.

## Macros, build switches, and diagnostics

Expected practice:

- Prefer `constexpr`, `consteval`, `enum class`, templates, or inline functions over macros.
- Macros are acceptable for include guards, export/import declarations, platform/compiler gates, and third-party/API-required glue.
- Avoid persistent environment-variable diagnostic knobs unless explicitly accepted as maintainer tooling.
- Temporary diagnostics must be clearly temporary and removed after investigation.

CamBANG audits should flag:

- temporary checkpoint logs left in source;
- persistent hidden diagnostic environment variables;
- retired helper-era hooks without active callers;
- dormant services without documented activation value;
- code that only exists to support retired test scaffolding.

## Comments and documentation

Expected practice:

- Comments should explain why, not restate obvious code.
- Document invariants near the code that relies on them.
- Document thread affinity.
- Document ownership transfer or non-ownership where it is not obvious.
- Document intentional failure/drop behaviour.
- Delete obsolete comments when code changes.

CamBANG-specific comment targets:

- snapshot publication rules;
- baseline/live/startup boundaries;
- provider encapsulation;
- render-thread release discipline;
- retained GPU/CPU result ownership;
- synthetic-only behaviour;
- temporary Windows MF provider assumptions;
- dormant/future activation seams.

A dormant service is acceptable only if its purpose, non-purpose, and activation conditions are clear.

## Naming and formatting

For purely stylistic questions, local consistency wins.

Suggested audit posture:

| Item | Preferred style |
| --- | --- |
| Types/classes | Existing CamBANG style, generally `PascalCase` / `CamBANG...` where public-facing. |
| Functions/methods | Existing lower snake case, especially Godot-facing APIs. |
| Variables | Existing local lower snake case. |
| Private members | Trailing underscore where already used locally. |
| Constants | Existing `kDescriptiveName` style where present. |
| Enum values exposed to Godot | Preserve established public constants. |
| Files | Existing module naming convention; avoid rename churn. |

Do not raise findings for brace placement, whitespace, or naming alone unless it conflicts with surrounding code, hides meaning, creates API inconsistency, blocks tooling, or causes real maintenance cost.

## Godot and public boundary

Expected practice:

- Preserve the locked Godot public API unless a separately approved correctness issue requires change.
- Do not expose implementation lifetimes through public objects.
- Do not make normal callers handle internal ids where object-level APIs are intended.
- Public wrappers should be truthful about readiness and failure.
- Public synchronous wrappers must not busy-wait indefinitely.
- Public calls should not depend on undocumented tick ordering.

Audit red flags:

- Wrapper returns success before the core accepted work.
- Wrapper hides provider/core failure.
- Wrapper assumes baseline snapshot exists when only `STARTING` is true.
- Public object caches stale server/core pointers without invalidation.
- Godot object destruction triggers unsafe core/render-thread teardown directly.

## Provider/core separation

Expected practice:

- Core owns orchestration and truth.
- Providers own platform interaction.
- Provider-specific quirks should be normalized at the provider boundary.
- Core should not learn Windows MF, Android Camera2, SyntheticProvider-only, or renderer-specific details unnecessarily.
- SyntheticProvider can be richer for deterministic testing, but synthetic-only assumptions must not leak into platform-backed provider design.

Audit red flags:

- Core branching on provider-specific implementation details.
- Public API shaped around SyntheticProvider conveniences.
- Windows MF temporary limitations treated as permanent architecture.
- Provider start/stop errors ignored or normalized to success.
- Provider frame callbacks accepted after teardown without stale-generation rejection.

## Waivers

A guideline may be waived when there is a clear reason:

- ABI/API compatibility;
- Godot binding requirements;
- platform API shape;
- measured performance need;
- narrow low-level resource wrapper;
- temporary investigation code;
- legacy code not touched by the current task.

But the exception should be explicit. Examples:

```cpp
// Non-owning. Owned by CoreRuntime and cleared before provider teardown.
ProviderBroker* broker_ = nullptr;
```

```cpp
// Platform API requires paired acquire/release.
// Wrapped here so callers never manage the raw handle directly.
class PlatformApiHandle {
    ...
};
```

Unexplained exceptions are audit findings.

## Compliance definition

Compliance does not mean every file is perfectly generic-modern C++ or every static-analysis warning is fixed.

Compliance means:

1. No untriaged blocker or major C++ quality risks.
2. No architecture-breaking style cleanups.
3. No new silent failure paths.
4. No new unsafe ownership, lifetime, or thread-affinity ambiguity.
5. No weakening of tests or verifiers merely to pass.
6. Static-analysis findings are fixed, waived, deferred with rationale, or marked not applicable.
