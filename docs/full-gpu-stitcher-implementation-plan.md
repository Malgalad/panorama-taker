# Full GPU stitcher implementation plan

## Goal

Build two independent renderers:

- the existing memory-bounded CPU renderer;
- a CUDA renderer that keeps source pixels and numerical image processing on the GPU.

The CUDA renderer must be practical on a normal Cyberpunk-capable NVIDIA card, using a 6 GiB card
as the baseline rather than assuming a 32 GiB development GPU. AMD and Intel devices require a
different backend and are outside this CUDA implementation.

CUDA mode may use the CPU only for validation, file decode, asynchronous upload, orchestration,
UI/logging, final result download, and file encoding. Projection, exposure analysis and solving,
local exposure, sampling, blending, coverage, auto contrast, tone/color conversion, and output
quantization execute on the GPU.

The GPU path must not call CPU projection/compositing helpers, create compositor memmaps, or write
disk scratch while numerical work is in progress.

## Feasibility

This is feasible with CuPy, NVRTC, custom CUDA kernels, pinned host buffers, streams, and events.
CUDA work is asynchronous; pinned memory and non-default streams permit decode/upload overlap.
CuPy supports resident device and pinned memory pools and custom raw CUDA modules.

The constraint "all data in VRAM" applies to all source pixels and active GPU workspaces. A complete
full-resolution output is also resident when it fits. On smaller cards, the output is computed in
GPU row bands and copied into one final host-RAM output array; sources remain resident and no image
math moves to CPU. This avoids disk backing and makes 6 GiB cards viable.

If even native-precision sources plus the minimum workspaces cannot fit, the requirement is
physically impossible for that capture. Report the required/available memory and select the CPU
renderer before source upload.

## Required behavior

- GPU enabled by default; explicit CPU mode remains available.
- Automatic fallback occurs only before the first numerical CUDA kernel.
- A CUDA error after computation starts fails the render and cleans up; it never resumes halfway
  on CPU.
- CPU mode never imports CuPy and retains its strips, workers, memmaps, and output behavior.
- CUDA mode never calls `_composite_strip()`, `_estimate_exposure_gains()`, CPU camera-map helpers,
  CPU auto contrast, or CPU color conversion.
- Preserve projection formulas, capture order, hard-blend ties, feather behavior, schema, filenames,
  defaults, HDR/Rec.2020/PQ/EXR behavior, and incomplete-output magenta.
- Do not enable CUDA fast math.
- Do not launch exposure/compositing once per frame from Python.
- Preview and full render use the same CUDA pipeline.

## Architecture

```text
render request
    |
    +-- validate/probe/decode metadata
    +-- select backend and memory mode
    |
    +-- CPU ---------------------------> existing CPU pipeline
    |
    `-- CUDA
          +-- allocate session buffers transactionally
          +-- decode sources into two pinned host slots
          +-- upload every source once
          +-- solve global exposure on GPU
          +-- build local exposure on GPU
          +-- compose full frame or output bands on GPU
          +-- auto contrast + color conversion on GPU
          +-- download completed output to host RAM
          +-- encode staged output file
          `-- atomic rename
```

Split orchestration explicitly:

```python
def render_session(...):
    request = _validate_request(...)
    selection = select_backend(request)
    if selection.backend == "cuda":
        return _render_cuda(request, selection)
    return _render_cpu(request)


def _render_cpu(request):
    # Existing implementation, mechanically moved and behavior-preserving.


def _render_cuda(request, selection):
    # CUDA session only; no CPU image-processing helpers or memmaps.
```

## Named types

Add immutable types instead of returning ambiguous tuples:

```python
@dataclass(frozen=True)
class BackendSelection:
    backend: Literal["cpu", "cuda"]
    device_name: str | None
    memory_mode: Literal["cpu", "resident", "banded"]
    required_bytes: int | None
    available_bytes: int | None
    reason: str


@dataclass(frozen=True)
class CudaMemoryPlan:
    source_bytes: int
    session_workspace_bytes: int
    output_workspace_bytes: int
    host_output_bytes: int
    reserve_bytes: int
    output_band_rows: int | None
    required_bytes: int
    available_bytes: int


@dataclass(frozen=True)
class CudaTransferStats:
    source_uploads: int
    host_to_device_bytes: int
    device_to_host_bytes: int
    kernel_launches: int
    synchronizations: int
    peak_device_bytes: int
    disk_scratch_bytes: int
```

Create two ownership classes in `gpu.py`:

- `CudaSession`: device, streams/events, source pixels, rotations, exposure-solve buffers, gains;
- `CudaOutputJob`: preview/panorama/thumbnail geometry, local exposure, output buffers, conversion.

Both are context managers. Allocation is transactional and `close()` is idempotent after success,
cancellation, or failure. CUDA C source remains in `cuda_kernels.py`.

## Memory model for ordinary GPUs

### Store sources in native precision

Do not blindly expand every source to float32 RGB in VRAM:

- JPEG and 8-bit PNG: interleaved uint8;
- 16-bit PNG: interleaved uint16;
- float/EXR: float32 unless the file is explicitly half and half precision is proven lossless for
  the current contract;
- keep one source encoding descriptor used by device sampling/conversion.

Convert to working float32 inside sampling kernels. This reduces ordinary SDR source VRAM by 4x
without changing source values.

### Two output modes

`resident` mode allocates the full linear output, coverage, local exposure, histogram, and optional
uint8 output in VRAM.

`banded` mode keeps all sources and session data resident but allocates only:

- full quarter-resolution local exposure;
- one linear output band;
- one coverage band;
- one converted uint8 band when needed;
- one pinned host band and one complete final host output array.

Each completed band is copied into its final location in host RAM. It is never used for additional
CPU image math and is not written to disk until all bands finish. Auto-contrast histogram is built
on the first GPU pass; conversion/download is a second GPU band pass only when auto contrast is
enabled.

Choose the largest band that fits after reserving `max(384 MiB, 15% of total VRAM)`. The minimum
band is 32 rows. If sources plus session buffers plus a 32-row band do not fit, use CPU.

The plan must use exact array sizes and verify planned versus actual CuPy pool usage in tests.

## Decode and upload

1. Allocate the complete device source tensor/buffer and session metadata first.
2. Allocate two reusable pinned host source slots.
3. Decode with at most two producer workers into a free slot.
4. Enqueue `cudaMemcpyAsync` into a non-default upload stream.
5. Protect slot reuse with a CUDA event.
6. Release ordinary decoded arrays as soon as their pinned copy is queued.
7. Synchronize once after all source uploads.
8. Do not start numerical kernels until every source is resident.

Upload rotations, frame encoding metadata, and indices in one small transfer. Assert exactly one
pixel upload per frame and zero source reuploads for one render.

## CUDA module

Replace separate `RawKernel` construction with one eagerly compiled `cupy.RawModule`. Compile and
resolve every function during selection so NVRTC failures can fall back before decode/upload.

Shared device helpers must implement:

- equirectangular and rectilinear pixel-center rays;
- row-major world-to-camera rotation;
- exact focal lengths, centers, `z > 0`, half-pixel validity, and clamping;
- native-source conversion and bilinear RGB sampling;
- bilinear scalar-field sampling;
- luminance, clipping, PQ/Rec.2020/sRGB, and quantization formulas.

Initial required kernels:

1. `build_exposure_proxies`
2. `sample_exposure_grid`
3. `classify_exposure_samples`
4. `reduce_overlap_pairs`
5. `solve_exposure_graph`
6. `build_local_exposure`
7. `compose_output`
8. `build_auto_contrast_histogram`
9. `select_auto_contrast_levels`
10. `convert_output`

## Global exposure solve on GPU

The CUDA branch must replace `_estimate_exposure_gains()` while preserving its current algorithm:

1. Downsample every resident source to its maximum-256-pixel-wide exposure proxy with area
   filtering matching OpenCV `INTER_AREA`.
2. Launch over `(frame, y, x)` for all frames on the 256x128 exposure grid. Generate projection and
   sample proxy RGB in one kernel.
3. Compute coverage, luminance, clipping, finite/positive validity, log luminance, Sobel gradients,
   and each frame's 90th gradient quantile on-device.
4. Generate all frame pairs on-device. Launch over `(pair, sample)` and compute geometric overlap
   and valid log ratios.
5. Use segmented partition/reduction for the 10th/90th trimmed range, median, MAD, inlier count,
   and equation weight. Do not loop pairs in Python.
6. Build connectivity and neutral bridges on-device.
7. Solve the anchored weighted system in float64 on-device, center by median, clamp to one stop, and
   exponentiate to float32 gains.
8. Download only final `ExposureReport` scalars; all sample and equation arrays remain on-device.

The graph is tiny. A single-block control/solve kernel is acceptable for connectivity and the
small linear system; transferring it to CPU is not. Large exposure sampling and pair statistics
must be parallel.

## Local exposure in one phase

`build_local_exposure` launches one thread per quarter-resolution output pixel. Each thread creates
its ray once, loops through every frame in capture order, computes projection/edge weight, and
accumulates weighted log gain in registers. It writes one normalized value or zero.

This removes the current Python loop that launches `accumulate_exposure` once per frame and removes
the separate sum/weight resident arrays.

## Compositing in one phase

`compose_output` launches one thread per output pixel in the full frame or current output band.
Each thread:

1. creates its ray once;
2. samples local exposure once;
3. loops all frames in capture order;
4. projects and skips invalid frames;
5. samples native resident source RGB as float32;
6. applies `expf(log_gain - local_exposure)`;
7. updates register-local hard or feather state;
8. normalizes feather once;
9. writes final linear RGB and coverage once.

Hard blend updates only for `candidate > best_weight`, preserving first-frame ties. Feather sums in
frame order. Do not use frame atomics, per-frame contribution tensors, or Python frame launches.
The output-pixel grid supplies abundant parallelism without nondeterministic reductions.

Start with `(16, 16)` blocks. Profile before adding textures or changing launch geometry.

## Bands, responsiveness, and Windows watchdog

Even resident mode should accept `row_start`/`row_count`. On Windows, choose bands targeting less
than 250 ms per kernel to avoid WDDM timeout and provide cancellation/progress checkpoints. Start at
256 rows and adapt from measured event timing between 64 and 1024 rows.

Bands are not CPU strips: they create no maps/memmaps, perform no CPU image math, and do not split
the frame dimension. All pixels within a band execute concurrently on GPU.

Synchronize only after all uploads, between long band checkpoints, for final scalar/result copies,
and during teardown. CUDA Graph capture is optional after profiling proves launch overhead matters.

## Auto contrast and output conversion

For SDR output:

1. Run GPU linear/PQ/Rec.2020-to-sRGB conversion for histogram input.
2. Build the existing 4096-bin luminance histogram with block-local histograms and a global merge.
3. Select the same 0.5% black/white percentiles on-device.
4. Apply levels, clamp, round, and write uint8 RGB on-device.

For banded auto contrast, the first band pass builds one global histogram without downloads. The
second pass converts and downloads bands. With auto contrast disabled, compose, convert, and copy
each band once.

EXR skips SDR conversion and downloads float32 linear RGB, preserving above-one and permitted
negative finite values.

## Preview, full render, and thumbnail

CUDA preview must render directly into a device uint8 result and copy it into `PreviewResult`.
Delete its temporary-PNG write/read round trip.

Add `CudaSessionCache` to the GUI after one-shot rendering is correct:

- key by device, canonical session path, source sizes/mtimes, encoding, and frame geometry;
- retain sources, rotations, exposure proxies, and solved gains after preview;
- full render allocates only its output job and reuses the resident session;
- thumbnail reuses the same session and gains;
- invalidate on input/session/source/GPU-option change, failure, discard, or GUI shutdown;
- log cache hits, misses, bytes, and cleanup.

Thus pressing full render after preview must not decode, upload, or solve exposure again.

## Final download and file output

Do not create output files before all GPU computation succeeds.

- Resident PNG/JPEG: one full pinned uint8 D2H copy.
- Banded PNG/JPEG: copy each completed band into a complete host uint8 array, then encode once.
- Resident EXR: one full pinned float32 D2H copy.
- Banded EXR: copy bands into a complete host float32 array, then encode once.
- Coverage: copy the completed uint8 mask only when requested.

Encode directly from host arrays. CUDA mode must not create `color.f32`, `weight.f32`, exposure
memmaps, preview PNG scratch, or JPEG `.rgb` spool files. Only after computation completes may it
create the normal staged final output and atomically rename it.

This intentionally uses host RAM for the completed result on smaller GPUs. If host allocation
fails, report it; do not silently introduce disk backing into CUDA mode.

## Selection, fallback, and diagnostics

Automatic mode may fall back for missing CuPy, missing/incompatible driver, no device, compilation
failure, insufficient minimum plan, initial allocation OOM, or upload failure before computation.
Close the CUDA session and clear owned pool blocks before invoking CPU once from the beginning.

Strict mode for tests/benchmarks raises on all fallback. Backend selection must report:

- device and compute capability;
- native source format and bytes;
- resident or banded mode and band rows;
- planned/actual/free/total VRAM;
- decode, upload, exposure solve, local exposure, compositing, conversion, download, and encode
  times;
- H2D/D2H bytes, launches, synchronizations, and disk scratch bytes;
- session-cache hit/miss.

The GUI indicator must show `CUDA resident`, `CUDA banded`, or `CPU`, plus the reason/memory plan.

## Tests

### CPU-only CI

- fake all selection/fallback causes;
- strict mode never returns CPU;
- CPU mode never imports CuPy;
- CPU fallback invokes the complete CPU renderer exactly once;
- CPU extraction remains byte-identical;
- CUDA dispatcher never calls CPU geometry, exposure, compositing, auto-contrast, or memmap helpers;
- GUI/CLI propagate all options and display/log structured selection;
- failure/cancellation cleanup closes mocks and removes staged outputs.

### Real CUDA

- native uint8/uint16/float source conversion;
- area proxy resize parity;
- projection at seams, poles, cardinal rays, half-pixel bounds, behind-camera rays, Euler rotations,
  and recorded basis matrices;
- RGB/scalar bilinear sampling;
- exposure RGB, masks, Sobel, quantiles, pair ratios, median/MAD, graph bridges, solve, and gains;
- local exposure parity;
- hard/first-frame ties, feather order, coverage, and incomplete magenta;
- SDR, PQ, Rec.2020, histogram/levels/quantization, and EXR values;
- resident and forced-banded outputs are equivalent;
- preview/full/thumbnail/coverage end-to-end parity;
- cache reuse performs no second decode/upload/exposure solve;
- 6 GiB simulated admission selects a viable mode for representative SDR captures;
- insufficient source-only memory falls back before upload;
- repeated jobs do not leak device or pinned memory.

### Transfer and scratch audit

For `N` sources:

```text
source uploads                  == N
map/mask/correction uploads     == 0
intermediate image D2H copies   == 0
resident final image D2H copies == 1
banded final image D2H bytes    == final image bytes
per-frame exposure launches     == 0
per-frame compositor launches   == 0
CUDA memmaps                    == 0
CUDA disk scratch before encode == 0
```

Every CUDA parity test must assert `backend == "cuda"`; matching CPU output alone may be fallback.

## Benchmark gates

Rewrite `scripts/benchmark_compositor.py` to require strict CUDA, warm kernels separately,
synchronize with CUDA events, use identical options, run one warm-up plus at least three measured
iterations, and report every phase and transfer counter.

Profile with Nsight Systems/NVTX. Acceptance requires:

1. no Python per-frame exposure/compositing loop;
2. no CPU image math in the CUDA branch;
3. no intermediate D2H or disk scratch;
4. GPU numerical work at least 5x faster than CPU numerical work;
5. warm preview at least 3x faster end-to-end;
6. warm full render at least 2x faster end-to-end;
7. preview-to-full cache reuse skips source load and exposure solve;
8. resident and banded modes meet parity and cleanup requirements;
9. a representative capture succeeds under a simulated 6 GiB budget;
10. the packaged Windows application works on a clean NVIDIA driver-only machine.

Do not call implementation complete when a gate is unmeasured or failing.

## Ordered implementation checklist for a lower-cost model

Complete one item and its tests before starting the next.

1. Add `BackendSelection`, `CudaMemoryPlan`, `CudaTransferStats`, automatic/strict selection, and
   fake fallback tests.
2. Mechanically extract `_render_cpu()` and prove byte-identical CPU output.
3. Add `_render_cuda()` as an empty dispatcher boundary and tests forbidding CPU image helpers.
4. Implement native-precision source size planning plus resident/banded plans for a 6 GiB budget.
5. Replace kernels with one eagerly compiled `RawModule` and transactional session/output owners.
6. Implement two-slot pinned decode/upload and exact upload/byte counters.
7. Implement/test shared projection, native-source conversion, and bilinear helpers.
8. Implement/test GPU proxy construction and all-frame 256x128 exposure sampling.
9. Implement/test GPU luminance, clipping, Sobel, quantiles, and valid masks.
10. Implement/test all-pair robust statistics without Python pair loops.
11. Implement/test device connectivity, bridges, weighted solve, centering, clipping, and gains.
12. Implement/test local exposure with the frame loop inside each pixel thread.
13. Implement/test hard compositing with the frame loop inside each pixel thread.
14. Add deterministic feather, coverage, incomplete output, and cancellation.
15. Add GPU auto contrast and SDR/PQ/Rec.2020/EXR output conversion.
16. Implement resident output download and direct host-array encoding without scratch files.
17. Implement banded output and prove equality with resident output.
18. Route preview directly through CUDA without temporary PNG.
19. Reuse the session for thumbnail.
20. Add adaptive watchdog-safe bands and progress timings.
21. Refactor session reuse through these independently testable checkpoints; keep one-shot rendering
    working after every checkpoint:
    1. Add `PreparedCudaSession` and a transactional preparation function that performs source
       decode/upload and the global exposure solve exactly once. Test retained sources, counters,
       report values, and idempotent cleanup on real CUDA.
    2. Extract output-job orchestration from `_render_cuda()` into a function accepting a caller-owned
       `PreparedCudaSession`. Route the existing one-shot renderer through prepare → render → close
       without changing output, progress, fallback, or cleanup behavior.
    3. Add an immutable `CudaSessionCacheKey` containing device identity, canonical session path,
       source size/mtime tuples, encoding, frame geometry, and GPU-affecting options. Add CPU-only
       key equality/invalidation tests.
    4. Add a single-entry `CudaSessionCache` owner with `get`, `store`, `invalidate`, and idempotent
       `close`. Replacement and failed preparation must close the prior/partial session exactly once;
       log hits, misses, resident bytes, and cleanup.
    5. Add optional prepared-session/cache parameters to preview and full-render orchestration.
       A cache miss prepares and stores once; a hit allocates only `CudaOutputJob`. One-shot CLI
       callers retain the current prepare/render/close lifecycle.
    6. Give `StitcherApp` one cache instance. Invalidate it on session/image/source/GPU-affecting
       option changes, render failure, cancellation, discard, and shutdown, but not when moving from
       a successful preview to its matching full render.
    7. Add a real-CUDA preview → full → optional-thumbnail test asserting the second operation adds
       zero source decodes/uploads and zero exposure solves. Add GUI lifecycle tests with fake owners
       proving every invalidation path closes exactly once.
22. Delete `CudaFrameCompositor`, per-frame resident launch methods, map-input kernels, and hybrid
    tests only after replacements pass.
23. Rewrite benchmark and add automated transfer/scratch audits. Record WSL profiling and benchmark
    results before moving to packaging.
24. Package only required CUDA libraries. Treat clean-Windows driver-only validation as a named
    external release gate: prepare the artifact and exact validation procedure locally, then record
    the result when the Windows machine is available. Do not mark the implementation complete while
    this evidence is absent, but continue all work that does not depend on that machine.
25. Run Ruff, format, Mypy, CPU tests, CUDA tests, leak tests, benchmarks, profiler gates, and
    packaged-artifact verification. Classify a failure as blocked only when the next concrete action
    requires unavailable hardware, credentials, or a user decision; otherwise fix it and continue.

At handoff report changed files, exact checks, selected mode, per-phase timings, plan/peak VRAM,
transfer and launch counts, disk scratch bytes, cache behavior, parity metrics, profiler bottleneck,
and Windows validation. Any missing acceptance item means the CUDA renderer is incomplete.

## Primary references

- [CuPy custom kernels](https://docs.cupy.dev/en/stable/reference/kernel.html)
- [CuPy device and pinned memory pools](https://docs.cupy.dev/en/stable/user_guide/memory.html)
- [NVIDIA CUDA asynchronous execution](https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/asynchronous-execution.html)
- [NVIDIA CUDA Best Practices Guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)
