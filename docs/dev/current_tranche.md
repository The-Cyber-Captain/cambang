# Current tranche

## Alternate payload support — stage 3: NV12 GPU display path

Follows `2ae081e`. An NV12 stream retains as `CPU_PLANAR` and correctly
reports no access path. This tranche gives it a display path, which is where
the latency and bandwidth win of the whole workstream actually lands.

### Follow the capability fork that already exists

`get_display_view()` returns a single texture object and that surface is
locked, so the consumer must never be asked to apply a `ShaderMaterial`.

CamBANG already forks on GPU availability, and it does so by runtime
capability rather than by renderer name: `global_rd_available()` asks whether
`RenderingServer::get_rendering_device()` exists, which feeds
`gpu_backed_available` through `stream_backing_capabilities()` and on into
output-form resolution, retained payload kind, and access classification.

Godot documents exactly when that returns nothing: "When using the OpenGL
rendering driver or when running in headless mode, this function always
returns `null`." The Compatibility renderer is the OpenGL driver, so the flag
is false there and everything is CPU-backed. Compatibility is a required
target — Web and older Android devices cannot use anything else — while
Quest 3 and newer handsets can run either. The project default is
`gl_compatibility` for both desktop and mobile.

The headless half of that note matters for validation, not just design: a
headless run has no `RenderingDevice` regardless of renderer, so it can never
exercise a GPU path. This is why `run_gpu_display_teardown_race_stress.ps1`
runs `-Windowed` with `--rendering-method=mobile`, and why a default
`run_godot.ps1` invocation proves nothing about the RD path.

NV12 display therefore needs no new mechanism decision. It needs the same
fork, with a planar-aware branch on each side:

| Path | Mechanism |
|---|---|
| No RD (Compatibility, Web, older Android) | CPU convert NV12 -> RGBA into the existing live `Image`/`texture_2d_create` display path |
| RD present (Mobile, Forward+) | Two-plane upload plus a GPU conversion pass |

Both land behind an unchanged `get_display_view()`. No `SubViewport`, no
scene-tree objects in the resource path.

State the wins honestly, because they differ per path.

RD path: upload drops from `w*h*4` to `w*h*3/2`, ~62% less bus traffic per
frame, and the CPU never touches the frame on the display path. Not free —
there is a GPU pass per displayed frame, to be measured rather than assumed
cheap.

No-RD path: the YUV->RGB conversion does not disappear, it *moves* from the
provider's acquisition thread to the Godot display path. Retention halves, and
frames Core drops are never converted at all, where today the provider
converts every frame before Core has decided whether it wants it. A smaller
win than the RD path, and a graceful degradation rather than a cliff.

### Scope

1. **CPU planar display conversion** for the no-RD path, in the existing live
   CPU display view path alongside the current RGBA/BGRA writers.

2. **Two-plane upload** for the RD path: luma to `R8_UNORM` (`w x h`), chroma
   to `R8G8_UNORM` (`w/2 x h/2`), from the retained payload's per-plane
   offsets and strides. Deliberately not relying on Vulkan sampler-ycbcr
   conversion or a multi-plane `DATA_FORMAT_*_2PLANE_*` being exposed and
   driver-supported; two single-plane textures work wherever RD does.

3. **GPU conversion pass** into `R8G8B8A8_UNORM`, driven by the payload's
   declared `PayloadColorimetry` rather than hardcoded coefficients. Both
   range and matrix must be honoured, and `UNSPECIFIED` must select an
   explicit documented fallback rather than a silent one. The same
   colorimetry must drive the CPU path, so the two agree.

4. **Display-view wiring.** `get_display_view()` returns a usable texture for
   a `CPU_PLANAR` stream result on both paths, and
   `build_stream_retained_access_truth()` stops reporting `UNSUPPORTED` for
   `display_view` on planar. `to_image()` stays `UNSUPPORTED` for planar until
   stage 4. Access classification must reflect the route actually taken: the
   RD path is not `READY` (there is a conversion pass), and the CPU path is
   not free.

5. **RID lifetime**, RD path only. Three RIDs per stream instead of one.
   Creation *and* release both marshal to the render thread through the
   existing pending-queue/drain helpers, which are already per-RID and need no
   new infrastructure. Teardown stays deterministic at
   stop/destroy/reconfigure boundaries.

### Out of scope

- `to_image()` for planar (stage 4).
- Platform providers advertising native YUV (stage 5).
- Encoded and RAW payload kinds.
- Any Godot-facing public API change. `get_display_view()` keeps its
  signature and its meaning.

### Acceptance criteria

- An NV12 stream displays correctly **on both paths**, verified by eye in a
  windowed scene, not only by verdict. A wrong colour matrix produces a
  plausible image, so a green verdict alone does not establish correctness
  here. The project default is `gl_compatibility`, so the no-RD path is what a
  default run exercises; the RD path needs an explicit Forward+/Mobile run and
  must not be assumed covered.
- Both paths agree: the same frame converted on CPU and on GPU must produce
  the same pixels within a stated tolerance.
- Colour is correct against a known-value check: SyntheticProvider's
  RGBA -> NV12 conversion is deterministic and point-sampled, so a round trip
  through upload and GPU conversion has a predictable expected result.
- Packed RGBA display is unchanged, byte counts and access classification
  identical to `2ae081e`.
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
.\run_godot.ps1 -Scene res://scenes/70_result_retrieval_verification.tscn -Windowed -CaptureLogs -TimeoutSec 90 -RunLabel scene70
.\run_godot.ps1 -Scene res://scenes/568_backing_plan_evaluation_verify.tscn -Windowed -CaptureLogs -TimeoutSec 120 -RunLabel scene568
.\run_cpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
.\run_gpu_display_teardown_race_stress.ps1 -Iterations 25 -TimeoutSec 30
```

The GPU teardown gate is required this tranche, not optional: RID count per
stream changes, which is exactly what that gate exists to police.
