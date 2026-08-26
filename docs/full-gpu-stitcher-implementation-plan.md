# Full-GPU stitcher corrective implementation plan

## Objective

Maintain two complete rendering paths: the existing CPU renderer and a genuinely GPU-resident CUDA
renderer. CUDA is preferred when enabled, available, and large enough for the current panorama;
otherwise the application uses the CPU renderer. During CUDA geometry and compositing, decoded
source pixels, exposure fields, color accumulators, and weight accumulators must stay in VRAM.
Projection, validity, edge weighting, bilinear sampling, local exposure correction, and blending
must execute inside CUDA kernels.

The CPU renderer remains supported, tested, and independently selectable. Do not replace it with
CuPy operations, make CuPy a base dependency, or weaken its memory-bounded strip/memmap behavior.
Users without a supported NVIDIA GPU must retain all existing stitcher functionality.

CPU work may remain for image decode, the small robust exposure solve, metadata validation,
auto-contrast, color/output conversion, and file encoding. Those operations must not run once per
source/output strip except for the final bounded row download used by the existing writers.

GPU mode is successful only if profiling shows no full-size `directions`, `map_x`, `map_y`,
`valid`, `edge_distance`, `candidate`, or `correction` NumPy arrays during compositing and no
source/output round trip per strip.

## Current state and code to remove

The current implementation is a parity prototype, not acceleration:

- `compositor._composite_strip()` computes directions, camera maps, validity, weights, and local
  correction on CPU.
- `CudaFrameCompositor.composite()` calls `cp.asarray()` for the source and every per-pixel input on
  every strip.
- It calls `deviceSynchronize()` and `cp.asnumpy()` for color and weights after every strip.
- Color and weight remain disk-backed NumPy memmaps, so every frame repeatedly crosses PCIe.
- The current CUDA kernel receives precomputed maps and therefore cannot fuse projection.
- The VRAM planner reserves space for resident sources/accumulators, but those allocations are not
  actually made. Admission success does not prove the intended execution mode.
- CUDA failure and VRAM rejection silently select CPU, so tests and benchmarks can accidentally
  claim GPU success.

After the resident CUDA path passes parity tests, delete only the obsolete hybrid CUDA pieces:

- `cuda_remap_source()` except as a test-only reference if still useful;
- the current map-input `_COMPOSITE_KERNEL`;
- `CudaFrameCompositor.composite()` and its per-strip upload/download contract;
- the `cuda_compositor` parameter and branch in `_composite_strip()`.

Do not delete `_composite_strip()` or modify its CPU algorithm beyond removing the hybrid CUDA
branch. It remains the production CPU fallback.

## Required architecture

```text
validated render request
        |
        +--> GPU disabled/unavailable/too small/allocation failure
        |        -> existing memory-bounded CPU renderer
        |
        `--> CUDA admitted
                 -> decode each source on CPU
                 -> one upload per source into device_sources[frame, y, x, rgb]
                 -> device exposure mapping and normalization
                 -> device projection, sampling, correction, and blending
                 -> finish kernel
                 -> bounded D2H row copies
                 -> existing CPU writer
```

All frame launches use one CUDA stream and retain frame order. No atomics are required because one
thread owns one output pixel and frames are processed sequentially.

## Backend selection and fallback contract

Expose one user-facing preference with GPU enabled by default. Internally distinguish automatic,
forced CPU, and strict CUDA behavior:

| Mode | Selection behavior | Failure behavior |
| --- | --- | --- |
| Automatic/default | Try CUDA admission first | Report reason and use CPU |
| GPU disabled | Select CPU without probing CUDA | Render on CPU |
| Strict CUDA (tests/benchmark only) | Require CUDA | Raise; never run CPU |

Automatic fallback is allowed only before CUDA output writing begins. If CUDA import, device probe,
kernel compilation, VRAM admission, or initial allocation fails, release CUDA resources and invoke
the CPU renderer from the beginning. A CUDA failure after rendering or staged output writing starts
is a render failure; do not silently mix CPU and CUDA output or resume halfway on CPU.

Both renderers must accept the same session geometry, output format, width, blend, incomplete-mode,
auto-contrast, coverage, cancellation, preview, thumbnail, and progress options. Backend selection
must not change schemas, filenames, defaults unrelated to acceleration, or color behavior.

## Phase 1: make backend selection observable

Before changing numerical code, prevent silent false positives.

1. Add these immutable types to `gpu.py`:

   ```python
   @dataclass(frozen=True)
   class GpuSelection:
       requested: bool
       selected_backend: Literal["cpu", "cuda"]
       device_name: str | None
       required_bytes: int | None
       available_bytes: int | None
       fallback_reason: str | None
   ```

2. Extend `RenderResources` with `backend`, `device_name`, `gpu_required_bytes`,
   `gpu_available_bytes`, and `fallback_reason`. Prefer an existing named type if the code already
   has gained one.
3. Make one shared selector perform import, device probe, VRAM calculation, and kernel warm-up.
4. Automatic mode may fall back. Add an internal `strict_gpu=True` option for tests and the
   benchmark; it must raise instead of falling back.
5. CLI and GUI must report `CUDA: <device>` or `CPU fallback: <reason>` before rendering.
6. The benchmark must assert that `selected_backend == "cuda"` before starting its GPU timing.

Tests:

- fake missing CuPy, driver error, no device, compile error, insufficient VRAM, and allocation OOM;
- automatic mode returns a structured CPU selection for each;
- strict mode raises for each;
- a CUDA benchmark cannot complete while using CPU.

## Phase 2: separate CUDA source and backend ownership

Create `stitcher/src/pano_stitch/cuda_kernels.py` containing only the CUDA source string and a lazy
`RawModule` factory. Keep device ownership in `gpu.py`.

Create `CudaCompositor` as a context manager with this lifecycle:

```python
with CudaCompositor(plan, geometry, blend) as compositor:
    compositor.upload_sources(decoded_sources)
    compositor.build_exposure(log_gains)
    compositor.composite(log_gains)
    compositor.download_rows(writer, coverage_writer)
```

The class owns and preallocates:

- `sources`: `(frame_count, source_height, source_width, 3)`, float32;
- `rotations`: `(frame_count, 9)`, float32;
- `log_gains`: `(frame_count,)`, float32;
- `exposure_sum`, `exposure_weight`: quarter-resolution float32;
- `local_exposure`: quarter-resolution float32; reuse `exposure_sum` after normalization;
- `color`: `(output_height, output_width, 3)`, float32;
- `weight`: `(output_height, output_width)`, float32;
- one device output staging region only if the finish kernel cannot reuse accumulator rows;
- one pinned host row staging buffer sized from the existing CPU writer strip height;
- coverage staging buffer when requested;
- an uncovered counter and cancellation/error state.

Allocate every buffer in `__enter__` before rendering begins. If allocation fails, close the CUDA
context state and restart the untouched CPU renderer from phase one. Never fall back after a staged
output has begun.

`close()` must synchronize, drop owned arrays, and release free blocks held by CuPy's pool. It must
run after success, cancellation, or error.

## Phase 3: fused geometry helper in CUDA

Write one inline CUDA device function used by both exposure and compositing kernels:

```text
project_pixel(global_x, global_y, output_width, output_height, latitude_span,
              rotation[9], source_width, source_height)
    -> valid, map_x, map_y, edge_distance
```

It must reproduce these CPU formulas exactly:

1. Pixel centers use `x + 0.5` and `y + 0.5`.
2. Longitude is `(x / width - 0.5) * 2*pi`.
3. Latitude is `(0.5 - y / height) * radians(latitude_span)`.
4. World direction is `(cos(lat)*sin(lon), sin(lat), cos(lat)*cos(lon))`.
5. Multiply by the row-major world-to-camera rotation produced by `_frame_rotation()` on CPU.
6. Use source-width/source-height focal lengths and centers exactly as `camera_maps()` does.
7. Validity uses `z > 0` and half-pixel source bounds.
8. Clamp maps to `[0, dimension - 1]` before sampling/edge distance.
9. Edge distance is the minimum of all four clamped edge distances.

Do not pass direction/map/mask arrays to CUDA. Pass only scalar geometry and the 3x3 rotation.
Do not enable CUDA fast math.

Direct tests must cover seam columns, poles, cardinal directions, behind-camera directions,
half-pixel bounds, Euler rotations, and recorded basis matrices. Compare kernel diagnostics against
`equirectangular_directions()` and `camera_maps()` with maximum map error `<= 2e-4` source pixels.

## Phase 4: GPU exposure field

Implement these kernels:

### `clear_buffers`

Zero the exposure and compositor buffers. `cp.zeros()` is acceptable only if it allocates the final
resident arrays once; do not create per-frame arrays.

### `accumulate_exposure`

One thread owns one quarter-resolution exposure pixel. It calls `project_pixel`, computes:

```text
feather_width = max(1, min(source_width, source_height) * 0.08)
weight = valid ? max(edge_distance / feather_width, 1e-6) : 0
exposure_sum[p] += weight * log_gain[frame]
exposure_weight[p] += weight
```

Launch once per frame in capture order.

### `normalize_exposure`

Divide the sum by weight where weight is positive. Leave uncovered values zero, matching the CPU
memmap initialization.

After this phase there must be no host copy of the full local exposure field. For a debug test only,
allow an explicit download and compare against the CPU field.

## Phase 5: fully fused frame compositing

Replace the existing map-input kernel with a kernel whose thread inputs are only the output pixel,
frame index, scalar geometry, resident sources/rotations/log gains/exposure, and resident
accumulators.

For each pixel:

1. Call `project_pixel`; return immediately when invalid.
2. Bilinearly sample interleaved resident source RGB using float32 and clamped neighbors.
3. Bilinearly sample the quarter-resolution local-exposure field using exactly
   `_local_exposure_rows()` pixel-center mapping and clamping.
4. Apply `expf(log_gain[frame] - local_exposure)`.
5. Compute hard or feather candidate exactly like CPU.
6. Hard: replace only when `candidate > old_weight`; preserve strict frame-order ties.
7. Feather: add corrected RGB times candidate and add candidate to weight.

Launch once per frame, sequentially on one stream. Check cancellation between launches. Do not call
`deviceSynchronize()` inside the frame loop; stream ordering is sufficient. Synchronize only for a
host-visible progress/cancellation checkpoint, finish, download, error reporting, or teardown.

Tests:

- integer, half-pixel, and four-border bilinear samples;
- hard selection ties and frame order;
- feather accumulation;
- local-exposure interpolation and correction;
- two-frame and complete synthetic panorama parity before encoding;
- HDR values above one and negative finite values where the CPU preserves them.

## Phase 6: finish and bounded download

Implement `finish_rows` over a requested row range:

- compute coverage from `weight > 0`;
- divide feather color by weight once;
- leave hard color unchanged;
- write magenta for uncovered pixels only when incomplete output is allowed;
- count uncovered pixels in a uint64 counter;
- optionally write coverage bytes;
- do not perform SDR transfer functions or tone mapping in CUDA in this change.

For each writer strip:

1. Launch `finish_rows` for that row range.
2. Copy RGB and optional coverage into reusable pinned host staging buffers.
3. Synchronize the copy.
4. Call the existing PNG/JPEG/EXR and coverage writers.

This is the only full-resolution device-to-host traffic in normal rendering. No color or weight
array is copied back after an individual frame.

If auto contrast needs two passes, finish/normalize once on device, stream rows to the existing
histogram pass, then stream the same finished device rows again to the writer. Do not download a
full panorama merely for auto contrast.

## Phase 7: compositor integration

In `compositor.py`, split orchestration cleanly:

```python
def _render_cpu(...): ...       # existing implementation
def _render_cuda(...): ...      # new resident backend
def render_session(...):        # validation, selection, staging, dispatch
```

Do not send `CudaCompositor` through `_composite_strip()` or `ThreadPoolExecutor`. CUDA mode must
not allocate CPU color/weight/local-exposure memmaps. It may use the existing temporary directory
only for staged output formats and writers.

`render_session()` owns common validation and backend selection, then calls exactly one renderer.
The CPU renderer must not import CuPy or probe CUDA. The CUDA renderer must not call
`_composite_strip()` or allocate the CPU accumulator memmaps. Keep their resource lifetimes and
cleanup independent so fallback cannot leave mixed state.

Keep these shared:

- source probing and decode functions;
- robust 256x128 exposure gain solve;
- metadata/session validation;
- output dimension calculation;
- temporary output replacement and failure cleanup;
- writer classes and color/output conversions;
- progress callback vocabulary;
- cancellation exception;
- thumbnail and preview public behavior.

Implement preview and thumbnail through the same CUDA backend with their own output geometry. Do
not retain the current separate CPU-only thumbnail compositor when GPU is selected.

## Phase 8: accurate VRAM admission

Update the planner to match the actual allocations, including alignment and both temporary staging
buffers. Add a test that compares the plan with the sum of `array.nbytes` for a tiny real CUDA
allocation.

Admission sequence:

1. Probe current free/total VRAM.
2. Reserve `max(384 MiB, 15% of total)`.
3. Apply the optional user GPU budget.
4. Calculate the exact resident plan.
5. Reject before decoding all sources if it cannot fit.
6. Allocate all device buffers.
7. On the first allocation OOM, free owned/cached blocks and fall back to CPU from the beginning.
8. Do not retry with the current hybrid/per-strip CUDA path.

The 3 GiB minimum is not a promise that every panorama uses CUDA. If the complete resident working
set does not fit, report the required and available MiB and use CPU.

## Phase 9: tests and proof that work is on GPU

CPU-only CI tests:

- all selection/fallback cases with a fake adapter;
- no import of CuPy at application import time;
- no device allocation after admission rejection;
- CLI/GUI propagate GPU preference and budget;
- cancellation and staged-file cleanup for CUDA adapter failures;
- CPU output/default behavior remains unchanged.
- explicit GPU-disabled mode never imports/probes CuPy;
- automatic fallback invokes the complete CPU renderer from its beginning exactly once;
- every public render option is passed identically to both backend entry points.

Real CUDA tests, marked and skipped without a device:

- all direct geometry/exposure/composite/finish kernel tests;
- CPU/CUDA end-to-end parity for hard and feather;
- full-sphere and horizontal output;
- PNG 8/16, JPEG, PQ/Rec.2020, and EXR sources;
- auto contrast on/off, preview, thumbnail, incomplete output, and coverage diagnostic;
- an intentionally insufficient GPU budget uses CPU in automatic mode and raises in strict mode.

Add a transfer-audit adapter used by tests. For a render with `N` sources and `S` writer strips,
assert:

```text
host-to-device source uploads == N
host-to-device full-size map/mask/correction uploads == 0
device-to-host color downloads == S (or 2*S with auto contrast)
device-to-host weight downloads == 0
device synchronizations inside frame loop == 0
```

The CUDA end-to-end test must also assert `selected_backend == "cuda"`; image parity alone is not
proof because automatic fallback can produce the same image.

## Phase 10: benchmark and acceptance gates

Correct `scripts/benchmark_compositor.py` before using it:

- strict CUDA selection; abort on fallback;
- synchronize before and after each timed CUDA region;
- report kernel warm-up separately;
- report decode, exposure solve, exposure mapping, compositing, download, encoding, and total time;
- report planned and actual peak VRAM;
- report H2D/D2H byte totals and launch/synchronization counts;
- compare output metrics after every measured pair;
- use at least three measured iterations after warm-up.

Acceptance gates:

1. Ruff, format, mypy, and all CPU tests pass.
2. All real CUDA parity tests pass on the Windows target and WSL development GPU.
3. Transfer-audit counts match the resident contract above.
4. Profiler evidence shows projection, exposure mapping, sampling, correction, and blend kernels on
   GPU, with no per-frame output downloads.
5. CUDA geometry/compositing is at least 3x faster than CPU.
6. Total warm render is at least 1.5x faster on a representative capture.
7. An admitted render completes without OOM; a rejected render reports the reason and uses CPU.
8. Cancellation and failure leave no partial output.
9. The packaged Windows executable works on a clean NVIDIA-driver-only machine.

## Ordered implementation checklist for a lower-cost model

Complete one numbered item and its named tests before starting the next. Do not skip ahead to UI or
packaging, and do not declare success from the current map-input kernel.

1. Add structured backend selection and strict mode; test all fake fallback causes.
2. Add `cuda_kernels.py` and move kernel source out of `gpu.py`; keep behavior unchanged.
3. Implement/test `project_pixel` diagnostics against CPU geometry.
4. Implement persistent buffer allocation and exact `nbytes` accounting in `CudaCompositor`.
5. Upload each decoded source once; add transfer counters and assert exactly `frame_count` uploads.
6. Implement/test exposure accumulation and normalization entirely on device.
7. Implement/test fused projection, source sampling, exposure sampling/correction, and hard blend.
8. Add/test feather blend and strict frame ordering.
9. Implement/test finish, uncovered count, magenta marking, and bounded pinned downloads.
10. Extract `_render_cpu()` without changing it, then add `_render_cuda()` with no strip executor or
    CPU accumulator memmaps.
11. Integrate writers, auto contrast, coverage, cancellation, and cleanup; add tests after each.
12. Route preview and thumbnail through the resident backend and add parity tests.
13. Make VRAM admission use actual allocations and test admission/OOM fallback.
14. Add CLI/GUI backend reporting and ensure strict benchmarks cannot fall back.
15. Fix the benchmark, run transfer audit, profile, and meet both speed gates.
16. Validate the PyInstaller release on clean Windows and record the tested driver/GPU/runtime.
17. Delete the hybrid kernel path and any tests that only prove map-input remap parity.
18. Run the complete verification commands and inspect generated artifacts before handoff.

Never delete the CPU renderer, its resource planner, strip worker controls, memmap cleanup, or CPU
tests. “Delete the hybrid path” means delete only the CUDA-inside-`_composite_strip()` prototype.

At handoff, report changed files, exact checks run, profiler/benchmark results, selected backend,
peak VRAM, transfer counts, and any remaining manual Windows validation. If any acceptance gate is
unmet, call the implementation incomplete.
