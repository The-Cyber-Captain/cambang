# Current tranche

## Alternate payload support — stage 3 remainder: planar GPU display path

Planar payload transport is built and proven on hardware. What remains of this
workstream is the RenderingDevice half of the display path, which is where the
bandwidth win actually lands and which nothing has yet exercised.

### What is already true (scope boundary, not a record)

Stated so this tranche's scope is unambiguous; the git history is the record.

- Planar payloads are retained as `CPU_PLANAR` and converted to packed RGBA in
  Core (`planar_payload_to_rgba8`), on demand, at the point a caller asks for
  display bytes or a Godot `Image`. Providers no longer convert for delivery.
- `display_view` and `to_image` both report `EXPENSIVE` for planar and both
  work. The stage-4 split in the previous draft of this file no longer applies.
- SyntheticProvider emits NV12/NV21/I420/YV12 on streams and captures. WinRT
  emits NV12 on both. Camera2 emits the device-resolved `YUV_420_888` family
  member on both. The previous draft listed platform-native YUV as a later
  stage; it is done.
- Synthetic's GPU backing is RGBA8-only and planar formats are excluded from it
  on both the stream and capture sides, so a planar stream currently takes the
  CPU path everywhere.

### Where the RD path is actually reachable

The Windows project default is `gl_compatibility`, which has no
`RenderingDevice`, so a default Windows run exercises only the CPU path. The RD
path needs an explicit `--rendering-method mobile` or `forward_plus`.

Android now matches: the harness previously substituted `"mobile"` on every
Android export regardless of the project setting, so every Android run to date
used Vulkan and had an RD. That default is fixed and an unqualified Android run
is Compatibility, verified on an S20+ (`usesVulkan(): false`, `opengl3`).

Both targets therefore need a deliberate renderer argument to exercise this
tranche, and neither gets it by accident. Do not infer backing form from the
renderer setting either way -- read `payload_kind` off the retained result.

### Scope

1. **Two-plane upload**: luma to `R8_UNORM` (`w x h`), chroma to `R8G8_UNORM`
   (`w/2 x h/2`), from the retained payload's per-plane offsets and strides.
   Deliberately not relying on Vulkan sampler-ycbcr conversion or a multi-plane
   `DATA_FORMAT_*_2PLANE_*` being exposed and driver-supported; two
   single-plane textures work wherever RD does.

   Fully planar formats (I420/YV12) carry U and V in separate planes, so this
   path needs either a third `R8_UNORM` texture or an interleaving step. Decide
   explicitly and state which; do not silently support only the semi-planar
   half of the family Synthetic and Camera2 can emit.

2. **GPU conversion pass** into `R8G8B8A8_UNORM`, driven by the payload's
   declared `PayloadColorimetry` rather than hardcoded coefficients. Range and
   matrix must both be honoured, `UNSPECIFIED` must select the documented
   fallback explicitly, and `chroma_v_first` must be respected — NV21 and YV12
   are emitted by real hardware here, and getting their order wrong produces a
   plausible image rather than a failure.

3. **Route selection and classification.** `get_display_view()` keeps its
   signature and meaning; the RD path is chosen by runtime capability
   (`global_rd_available()`), never by renderer name. Access classification
   must reflect the route actually taken — the RD path is not `READY`, since
   there is a conversion pass.

4. **RID lifetime**, RD path only. Two or three RIDs per stream instead of one.
   Creation *and* release marshal to the render thread through the existing
   pending-queue/drain helpers. Teardown stays deterministic at
   stop/destroy/reconfigure boundaries.

### Out of scope

- Encoded and RAW payload kinds.
- Any Godot-facing public API change.
- Platform providers exposing GPU-backed payloads. Both currently report
  `{cpu=true, gpu=false, both=false}`; Camera2 `PRIVATE` is not implemented.

### Acceptance criteria

- A planar stream displays correctly on the RD path, verified **by eye** in a
  windowed run. A wrong matrix or swapped chroma produces a plausible image, so
  no verdict establishes this.
- CPU and GPU paths agree on the same frame within a stated tolerance. Judge
  flat colour regions: 4:2:0 chroma is point-sampled by Synthetic, so
  high-frequency content differs legitimately by far more than a colour fault
  would.
- Packed RGBA display is unchanged — byte counts and access classification
  identical to current `main`.
- No `free_rid()` from a non-render thread; teardown stress gates clean.

### Validation expectations

```sh
out/core_spine_smoke.exe
out/provider_compliance_verify.exe
out/restart_boundary_verify.exe
out/verify_case_runner.exe --run-all
out/core_result_path_smoke.exe
```

```powershell
# The RD path needs an explicit renderer; a default run proves nothing about it.
.\run_godot.ps1 -Scene res://scenes/570_planar_display_visual_check.tscn -Windowed -CaptureLogs -TimeoutSec 60 -RunLabel scene570_rd
.\run_godot.ps1 -Scene res://scenes/70_result_retrieval_verification.tscn -Windowed -CaptureLogs -TimeoutSec 90 -RunLabel scene70
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -Windowed -CaptureLogs -TimeoutSec 120 -RunLabel scene568
.\run_cpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
.\run_gpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
```

Scene 570 takes `--cambang-planar-format=nv12|nv21|i420|yv12`; cover at least
one semi-planar and one fully planar member. Scene 70 is **attended** — it
waits on button presses, including to exit.

The GPU teardown gate is required this tranche, not optional: RID count per
stream changes, which is what that gate exists to police.

### Two traps this workstream has already sprung

- `ResultCapability` is `READY=0, CHEAP=1, EXPENSIVE=2, UNSUPPORTED=3`. Zero is
  the best answer. Compare against the bound `CAPABILITY_UNSUPPORTED` constant,
  never a literal.
- A reported capability must imply structural admissibility. Deriving one truth
  from two sources is what repeatedly let capability and behaviour disagree
  here; the unified predicates in `core_result_store.h` exist for that reason.
