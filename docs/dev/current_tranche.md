# Current tranche

## Godot render-resource deferred release

### Goal

Eliminate arbitrary-thread and under-lock destruction of Godot render resources
and render-resource-owning wrappers across CPU and GPU display paths.

### Architectural authority

Implement the ownership and thread-affinity contract in:

- `docs/architecture/pixel_payload_and_result_contract.md`, especially §7.7;
- `docs/architecture/godot_boundary_contract.md`;
- the repository C++ quality policy and audit checklist.

If those authorities conflict with current source, follow the documented
contract and report the source conflict.

### Scope

- CPU live-display texture RID replacement and final-owner release.
- CPU live-display registry erase, clear, and teardown.
- GPU/RD RID deferred release.
- `Texture2DRD` and equivalent wrapper deferred destruction.
- Pending-release admission, drain, failure, and teardown paths.
- Destructor exception containment.
- Thread-affinity verification for creation, update, replacement, and release.

### Required implementation

- No arbitrary-thread destructor calls `RenderingServer::free_rid()`,
  `RenderingDevice::free_rid()`, or equivalent render-resource operations.
- No arbitrary-thread path destroys the final Godot wrapper reference when that
  destruction can release a render resource.
- CPU and GPU release use an approved Godot/render-resource drain.
- Registry/cache/release-queue locks are not held during RID free or final
  wrapper destruction.
- Destructor-originating handoff is non-throwing.
- Allocation failure, saturation, late teardown, and unavailable-drain paths do
  not fall back to unsafe current-thread destruction.
- Teardown drains accepted work before release-service uninstall.
- Work that cannot safely drain during terminal teardown is explicitly
  quarantined rather than unsafely destroyed.
- Repeated start/stop/restart remains deterministic.
- Existing display semantics and performance policy remain unchanged.

### Verification

Add deterministic checks for:

- CPU RID final owner destroyed from a worker thread;
- CPU registry erase/clear with and without surviving wrappers;
- CPU RID replacement;
- GPU/RD RID final owner destroyed from a worker thread;
- deferred `Texture2DRD` final-reference destruction;
- enqueue failure and queue saturation;
- teardown with pending RID and wrapper releases;
- no release beneath registry or release-queue locks;
- no direct `free_rid()` outside the approved drain;
- no destructor exception escape;
- restart after teardown.

Run the required native, Windows GDE, Android GDE, and affected Godot display
verification, including representative Mobile and Compatibility paths.

### Exclusions

- No Godot public API or Dictionary-shape change.
- No provider/Core transport change.
- No ProviderBroker locking work.
- No Backing Plan, Backing State, Operation Support, or access-cost redesign.
- No display freshness or materialization-policy change.
- No unrelated performance optimisation or cleanup.
- No environment-variable controls.
- No weakening of tests.

### Completion criteria

- Every RID and render-wrapper destruction path has an identified safe context.
- No arbitrary-thread or under-lock render-resource destruction remains.
- Failure and terminal-teardown paths are safe and non-throwing.
- Required verification passes on the actual supported Godot/platform paths.
- Source, tests, and permanent documentation agree.