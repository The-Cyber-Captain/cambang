# Retired GPU-import feasibility spike results (November 2026)

**Status:** Historical development evidence only. The `gpu-import-spike` branch
and its probe implementation have been retired. This note is not canonical
design, product support policy, or a performance claim. For the current retained
capture contract, see `../retained_gpu_capture_sampling.md` and the architecture
index.

The temporary `gpu-import-spike` work explored native camera-buffer import and
cross-device sharing. It is not production code and does not define the public
CamBANG API. These results were recovered from local GDE run records (run labels
below); the source probes and XR wrapper scenes are not part of the supported
product path.

- **Quest G5, `spike990_quest3_xr_g5` (17 November):** Godot 4.5.2 Mobile on
  Quest 3 / Adreno 740, Camera2 camera 50, under OpenXR. The probe imported the
  1280x1280 YUV `AHardwareBuffer` into Godot's Vulkan device, sampled it through
  an external-format YCbCr conversion in a compute pass, and compared the R8 +
  R8G8 outputs with Camera2 CPU planes. All 20 captures matched exactly (zero
  Y/Cb/Cr mismatches; mean measured GPU time 946 us). The harness reported
  `status=ok`, then Godot aborted during shutdown on a `VkThread` destroyed-mutex
  check. This proves feasibility for that probe/configuration, not safe product
  ownership, non-blocking use, or production support.
- **Quest G6, `spike990_quest3_xr_g6` (18 November):** 30 stills in each of
  `CPU_READ_OFTEN` and `GPU_SAMPLED_IMAGE|CPU_READ_OFTEN` modes had similar CPU
  copy medians (0.352 ms and 0.338 ms) and callback medians (0.501 ms and
  0.493 ms). Switching the reader mode rebuilt the Camera2 session: five arm
  and five disarm trials took about 80-103 ms each. This is evidence about that
  experimental reconfiguration path; it does not by itself attribute a capture
  stall or establish that production capture must switch modes that way.
  Blocking, verified G5 passes also produced large frame intervals in this
  probe, while its asynchronous variant had lower measured main-loop cost; those
  measurements include the probe's own synchronization and verification work.
- **Quest G7, `spike990_quest3_xr_g7_tm1_validation_fixed` (18 November):** a
  second Vulkan device created from Godot's instance exported R8/R8G8 image
  allocations as OPAQUE_FD; Godot imported them, wrapped them in RenderingDevice,
  and copied/read back a generated pattern. The `rd_tracked_copy` route passed
  30 route-only repetitions with zero mismatches and zero Vulkan errors. This
  validates a synthetic cross-device Vulkan interop experiment on that Quest;
  it does not validate Camera2 source import, capture-buffer lifetime, or the
  production `CamBANGGpuBacking` contract. Its measured queue work is not a
  product performance guarantee.
- **Windows W3, `spike990_laptop_d3d12_w3` (17 November):** on an RTX 5070 Ti
  laptop, the synthetic 64x64 BGRA shared-texture paths copied/read back with
  zero mismatches. NV12 support was partial: the full two-plane wrap was not
  copied, only the R8 plane was copied and checked, and the R8G8 plane was
  wrapped but not copied. This was not a WinRT camera test or complete NV12
  qualification.

The local run records also show that G5/G6's `status=ok` harness verdicts were
followed by the shutdown crash; the later fixed G7 wrapper run completed with an
`OK` process verdict. These observations are separate from the current retained
capture feature and from the capture-stall fix. Keep the probe implementation
and wrapper code retired; revisit a specific interop route only if a future
requirement needs it.
