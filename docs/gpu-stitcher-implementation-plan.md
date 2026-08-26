# GPU stitcher implementation plan

## Goal and constraints

Add an optional NVIDIA CUDA compositor that accelerates projection, bilinear sampling, local
exposure correction, and hard/feather blending. Keep the current CPU renderer as the default and
fallback. Do not change capture/session schemas, projection conventions, color transforms, blend
definitions, output names, output dimensions, defaults, cancellation behavior, or staged output
cleanup.

The first implementation should target Windows x64, Python 3.12, NVIDIA GPUs, and CUDA 12. GPU
acceleration is an advanced option enabled by default. Before rendering, check whether the current
panorama's complete GPU working set fits in currently available VRAM. If it does not, report the
reason and automatically use the unchanged CPU renderer.

## Important correction to the initial assumption

“All data is loaded into VRAM” is not compatible with an unconditional 3 GiB minimum. Float32
storage alone is:

```text
source bytes       = frame_count * source_width * source_height * 3 * 4
accumulator bytes  = output_width * output_height * (3 color + 1 weight) * 4
exposure bytes     = ceil(output_width / 4) * ceil(output_height / 4) * (1 + 1) * 4
```

For example, a 16,384 x 8,192 accumulator is 2.00 GiB, before sources, exposure fields, CUDA
context, temporary arrays, or encoder staging. Twenty-four 3840 x 2160 float32 RGB sources add
about 2.22 GiB. A 3 GiB GPU therefore qualifies for GPU acceleration only on panoramas whose full
working set fits. Do not use tiling, source streaming, or CUDA unified-memory oversubscription in
the first implementation. Fall back to CPU before allocating GPU render buffers.

## Backend decision

Use **CuPy with one fused `RawKernel`**, not OpenCV CUDA, PyTorch, or a collection of individual
CuPy array operations.

- CuPy publishes prebuilt Windows wheels for CUDA 12 and can run with only a compatible NVIDIA
  driver when CUDA component wheels are bundled. See the
  [CuPy installation guide](https://docs.cupy.dev/en/stable/install.html).
- `RawKernel` accepts CUDA C++, compiles on first call with NVRTC, and caches the binary per device.
  See the [RawKernel API](https://docs.cupy.dev/en/stable/reference/generated/cupy.RawKernel.html).
- A fused kernel avoids materializing the current `directions`, `map_x`, `map_y`, `valid`,
  `edge_distance`, `sampled`, `correction`, and `candidate` arrays. That is the main VRAM and
  bandwidth win.
- `cupyx.scipy.ndimage.map_coordinates(order=1, mode="opencv")` is useful only for a parity
  prototype. Its interpolation API is documented
  [here](https://docs.cupy.dev/en/stable/reference/generated/cupyx.scipy.ndimage.map_coordinates.html),
  but separate coordinate and sampled arrays cost too much at full resolution.
- Do not use the normal `opencv-python` package as a CUDA backend. A CUDA-enabled OpenCV build is a
  separate native packaging burden and still leaves most NumPy projection/blending expressions on
  the CPU.
- Do not use PyTorch for this narrowly scoped numerical kernel; its package/runtime surface is much
  larger and `grid_sample` introduces a different normalized-coordinate contract.

Keep CuPy optional. Put it in a new dependency extra such as `gpu = ["cupy-cuda12x>=14,<15"]`; do
not add it to the base dependencies. Confirm the exact pinned CuPy/CUDA component versions in a
clean Windows build before committing the release lockfile.

## Architecture

Add a small backend boundary instead of adding `if gpu` branches throughout `compositor.py`.

```text
CLI / GUI
    -> GPU enabled? (default yes)
    -> device/VRAM admission check
    -> existing validation, decoding, exposure gain solve
    -> CPU compositor OR CUDA compositor
    -> existing streaming writers and staged-file replacement
```

The CUDA path owns only geometric exposure mapping and final compositing. Keep these on CPU for
the first version:

- PNG/JPEG/EXR probing and decoding;
- the 256 x 128 robust exposure solve and `numpy.linalg.lstsq`;
- SDR transfer functions, tone mapping, auto-contrast histogram, and file encoding;
- metadata/session validation and all output staging.

Those operations are either small, tied to current codecs, or occur once per output row. Moving
them in version one would expand scope without addressing the dominant repeated work.

## Files to add or change

### 1. `stitcher/src/pano_stitch/gpu.py` (new)

Keep CuPy imports inside functions so importing the CPU application never requires CUDA.

Add named types/dataclasses rather than ad-hoc dictionaries:

```python
@dataclass(frozen=True)
class GpuDeviceInfo:
    name: str
    total_bytes: int
    free_bytes: int

@dataclass(frozen=True)
class GpuRenderPlan:
    required_bytes: int
    available_bytes: int
    source_bytes: int
    output_bytes: int
```

If an existing project-wide option type has been added by the time this is implemented, extend it
instead of creating a duplicate union.

Functions and responsibilities:

- `cuda_device_info() -> GpuDeviceInfo`: lazily import CuPy, call the CUDA runtime, and return a
  concise actionable error for missing package, missing/incompatible driver, no CUDA device, or
  kernel compilation failure.
- `choose_gpu_render_plan(...) -> GpuRenderPlan | None`: use current free VRAM, not only total VRAM.
  Reserve the greater of 384 MiB or 15% of total VRAM for the CUDA context, allocator fragmentation,
  and desktop use. Return `None` with a structured reason when the resident working set does not
  fit. CuPy notes that context/library allocations outside its pool can consume one to
  several hundred MiB in its
  [memory guide](https://docs.cupy.dev/en/stable/user_guide/memory.html).
- `CudaCompositor`: allocate/reuse device buffers, launch kernels, synchronize at cancellation and
  copy boundaries, and release its memory pool on close.
- Convert every host upload with `np.ascontiguousarray(..., dtype=np.float32)`. `RawKernel` does not
  honor array views/strides automatically, as its API explicitly warns.

Do not globally replace CuPy's allocator or set process-wide environment variables. Keep ownership
inside the compositor and call `free_all_blocks()` during teardown after synchronization.

### 2. `stitcher/src/pano_stitch/cuda_kernels.py` (new)

Store one CUDA source string and construct one `cupy.RawModule`/`RawKernel` lazily. Provide these
kernels:

#### `accumulate_exposure`

One thread handles one output pixel in the quarter-resolution exposure field:

1. Derive longitude and latitude from the global pixel center using exactly the formulas in
   `equirectangular_directions()`.
2. Form the world direction.
3. Multiply by the supplied contiguous row-major world-to-camera 3 x 3 matrix.
4. Compute `map_x`, `map_y`, validity, and edge distance using exactly `camera_maps()` semantics,
   including `z > 0`, the half-pixel validity bounds, and clamping to source pixel centers.
5. Compute feather exposure weight and add `weight * log_gain` and `weight` to the two output
   arrays. No atomics are needed: each launch processes one frame and each thread owns one output
   pixel.

Launch once per frame. Then run a small `normalize_exposure` kernel that divides where weight is
positive.

#### `composite_frame`

One thread handles one output pixel for one frame:

1. Repeat the exact projection math above. Do not allocate direction or map arrays.
2. Bilinearly sample interleaved RGB float32 pixels. Clamp coordinates before sampling, calculate
   `x0=floor(x)`, `x1=min(x0+1,width-1)` (and the equivalent for y), and interpolate in float32.
3. Bilinearly sample the quarter-resolution local-exposure field with the same coordinate formula
   as `_local_exposure_rows()`.
4. Multiply RGB by `expf(log_gain - local_exposure)`.
5. For hard blend, replace RGB and weight only when `candidate > old_weight`; preserve the strict
   comparison so equal-weight frame ordering matches CPU behavior.
6. For feather blend, accumulate `sample * weight` and weight.

Launch frames sequentially on one CUDA stream. Do not launch frames concurrently: all frames write
the same accumulator and concurrency would require atomics, change deterministic ordering, and
complicate cancellation.

#### `finish_rows`

For each pixel, produce coverage, divide feather RGB by weight once, enforce the current magenta
uncovered-pixel behavior when incomplete captures are allowed, and count uncovered pixels with a
device reduction or a uint64 atomic counter. Copy finished RGB rows and coverage to host staging
buffers for the existing writers.

Compile without `--use_fast_math`; it changes division, trigonometry, and exponential accuracy.
The first version should use float32 throughout to match the CPU working representation.

### 3. `stitcher/src/pano_stitch/compositor.py`

Make the smallest extraction needed:

- Add a `use_gpu: bool = True` parameter to `estimate_render_resources()`, `render_session()`, and
  `render_preview()` only after adding one shared validator.
- Preserve the complete existing CPU path. Move it only if necessary to avoid a large diff.
- Dispatch geometric exposure mapping and compositing to `CudaCompositor` when selected.
- Continue using the existing `_read_source()`, writer classes, temporary paths, cancellation
  exception, progress phases, thumbnail behavior, and exposure report.
- Copy finished rows into a reusable pinned host staging array, then pass its NumPy view to the
  existing writer. A single stream is sufficient initially; add overlapped transfer/compute only
  after profiling proves the copy is material.
- Do not pass CPU `workers` into the CUDA path. Extend `RenderResources` with explicit selected
  backend, device, required VRAM, available VRAM, and fallback reason fields if the UI needs them.
  Avoid pretending CUDA threads are CPU workers.

The current `memory_budget_bytes` means host RAM. Add a separate optional
`gpu_memory_budget_bytes`; do not silently reinterpret the existing setting. When unset, use free
VRAM minus the safety reserve. Clamp a user-supplied GPU budget to available VRAM.

### 4. `stitcher/src/pano_stitch/cli.py`

Add:

```text
--gpu / --no-gpu            default: --gpu
--gpu-memory-budget-mib N   default: use available VRAM safely
```

With `--gpu`, try CUDA only when CuPy imports, a device is available, the kernel compiles, and the
planner says the current panorama's resident working set fits. Otherwise report the reason and use
CPU. `--no-gpu` always uses CPU. The internal API and benchmark script may have a strict CUDA mode
that fails instead of falling back, so tests cannot appear to benchmark CUDA while running CPU.

Keep `--workers` accepted for CPU mode. In CUDA mode, either ignore `0/Auto` or reject an explicit
nonzero value with a clear message; choose and test one behavior.

### 5. `stitcher/src/pano_stitch/gui.py`

Add a **Use GPU acceleration** checkbox to the advanced options and check it by default. Show the
selected device, free/total VRAM, required VRAM for the current panorama, and whether CPU fallback
will be used in the existing resource summary. Keep device probing and rendering on the GUI worker
thread and post results through the existing event queue.

On fallback, show a non-modal status message, not an error dialog. Do not add CUDA device probing
to Tk's main thread.

### 6. `stitcher/pyproject.toml`, lockfile, and Windows bundle configuration

- Add the optional `gpu` dependency extra.
- Update the lockfile in the normal environment.
- Build separate CPU and CUDA artifacts initially. Do not make every user download the CUDA
  runtime before bundle size and startup behavior are measured.
- Ensure PyInstaller collects CuPy binaries, NVRTC, required CUDA runtime/header components, and
  the kernel source module. Run the packaged executable on a clean Windows machine with only a
  supported NVIDIA driver installed.
- Set `CUPY_CACHE_DIR` to an application cache directory before the first kernel is constructed,
  not to the unpacked/temporary PyInstaller directory. The cache behavior and environment variable
  are documented in CuPy's
  [environment reference](https://docs.cupy.dev/en/stable/reference/environment.html).
- Warm/compile the kernels during explicit CUDA device initialization so compilation time appears
  as “initializing CUDA,” not as a frozen first render progress step.

## VRAM admission check

Use checked integer arithmetic and account for every live allocation. Start with this conservative
model and replace constants only after measuring CuPy's allocator:

```text
reserve          = max(384 MiB, total_vram * 0.15)
usable           = min(user_gpu_budget or free_vram, free_vram) - reserve
sources          = frame_count * source_pixels * 12       # all decoded RGB float32 sources
output           = output_pixels * 16                     # RGB + weight float32
exposure_fields  = exposure_pixels * 8                     # sum + weight
host_copy_device = writer_strip_pixels * 13                # RGB float32 + coverage uint8
overhead         = 64 MiB                                  # kernel args, allocator rounding, counters
```

The finish kernel may reuse accumulator storage; reduce `host_copy_device` only after a peak-memory
test demonstrates that buffers do not overlap. Admit GPU rendering only when `required <= usable`.
Allocate the complete plan before starting output work. If allocation still raises
`OutOfMemoryError` because free VRAM changed, release owned/cached blocks, report the fallback, and
restart the render on CPU. Do not partially continue a GPU render and do not retry allocations in a
loop.

Upload decoded sources one by one into a preallocated `(frames, h, w, 3)` array. If decoding or any
upload fails, release the incomplete GPU working set before CPU fallback.

## Semantic parity requirements

GPU output need not be byte-identical after transcendental functions, but it must preserve visible
and geometric behavior. Establish tolerances from real comparisons rather than loosening tests
until they pass.

Required invariants:

- pixel-center longitude/latitude convention;
- pitch sign and row-major camera-basis preference;
- half-pixel source validity bounds and clamped bilinear sampling;
- strict hard-blend tie behavior;
- feather width `max(1, min(source_width, source_height) * 0.08)`;
- exposure-field scale and bilinear expansion coordinates;
- float32 source decoding and linear-light blending;
- HDR/Rec.2020/PQ/EXR behavior remains in the unchanged CPU color/output path;
- uncovered-pixel count and magenta marking;
- cancellation, progress, temporary output cleanup, and thumbnail semantics.

## Tests

### CPU-only tests (always run)

Add planner and dispatch tests using a fake CUDA adapter; CI must not require a GPU.

- auto falls back on missing CuPy, no device, incompatible driver, compile error, and insufficient
  VRAM;
- default GPU mode falls back for missing CuPy/device, incompatible driver, compile failure,
  insufficient VRAM, and an allocation-race OOM;
- the internal strict-CUDA test/benchmark entry point reports each failure and never falls back;
- planner admits or rejects the complete resident working set at exact byte boundaries;
- safety reserve, user budget clamping, allocation-race OOM fallback, and overflow cases;
- cancellation and exceptions still remove partial output/coverage/thumbnail files;
- GUI worker communicates probe/render results only through its event queue;
- CLI and GUI pass identical GPU-enabled and GPU-budget options.

### CUDA tests (marked `gpu` and skipped without a device)

Add small direct kernel tests before end-to-end tests:

- cardinal directions, poles, seam columns, camera behind (`z <= 0`), and each half-pixel boundary;
- identity, yaw/pitch/roll, and recorded camera-basis matrices;
- bilinear samples at integer pixels, half pixels, and all four borders;
- hard blend tie and frame order;
- feather weights and local exposure interpolation;
- uncovered counter and magenta output.

Then compare CPU and CUDA renders for current synthetic fixtures across:

- hard and feather blend;
- full-sphere and horizontal capture;
- PNG 8/16-bit, JPEG, PQ/Rec.2020 PNG, and EXR inputs;
- complete/incomplete captures, auto contrast on/off, coverage output, preview, and thumbnail;
- admitted resident renders and rejected renders that fall back to CPU.

Suggested initial numerical gates before encoding:

```text
projection map max error:       <= 2e-4 source pixels
linear RGB absolute error:      <= 2e-5 for values in [0, 1]
linear RGB relative error:      <= 2e-4 for HDR values above 1e-3
coverage/uncovered pixels:      exact
8-bit encoded pixel difference: <= 1 code value, with >= 99.99% exact
```

If hard-blend selection differs because a candidate is within floating-point noise of the previous
weight, fix the kernel math/order. Do not mask seam changes with a broad image tolerance.

## Benchmark method and acceptance gates

Add `stitcher/scripts/benchmark_compositor.py`. It should accept one existing session, run a warm-up,
then time CPU and CUDA with identical options. CUDA calls are asynchronous, so synchronize before
starting/stopping host timers; CuPy exposes CUDA synchronization and events. Record:

- device and driver/runtime versions;
- input frame count and dimensions, output dimensions, blend, format, selected backend, required
  and available VRAM, and any fallback reason;
- kernel initialization time separately;
- exposure mapping, compositing, device-to-host transfer, encoding, and total wall time;
- peak VRAM from both the planner and allocator; peak host RAM if available;
- output comparison metrics and uncovered count.

Run at least three measured iterations and report median plus range. Use a release Windows bundle,
not only a source checkout.

Acceptance gates:

1. All existing CPU tests and formatting/type checks pass unchanged.
2. All CUDA parity tests meet the stated tolerances.
3. A render whose planned working set fits completes without OOM; an oversized render falls back
   to CPU before GPU rendering begins.
4. CUDA compositing (excluding decode/encode) is at least 3x faster than CPU on the chosen baseline.
5. Total warm-render wall time is at least 1.5x faster for a representative session. If encoding
   dominates and misses this gate, report the measured bottleneck before moving more scope to GPU.
6. Automatic fallback, cancellation, and cleanup work in the packaged application; strict CUDA
   failure works in the benchmark/test interface.

## Implementation order for a lower-cost coding model

Each step must be a separate small change with tests passing before continuing.

1. Add fake-adapter planner types, checked VRAM formulas, and CPU-only planner tests in `gpu.py`.
2. Add lazy device probing, default-on GPU selection, and automatic CPU fallback. Wire CLI only;
   keep CPU as the actual renderer.
3. Add direct CUDA test infrastructure and the fused projection/bilinear kernel for hard blend.
   Compare tiny float buffers directly against current CPU helpers.
4. Add feather accumulation, local exposure mapping/normalization, and finish/uncovered behavior.
5. Add `CudaCompositor` and end-to-end hard/feather tests.
6. Add resident-working-set admission and allocation-race fallback tests. Inject available VRAM in
   tests instead of relying on the developer GPU's free memory.
7. Integrate existing writers, auto contrast, debug coverage, cancellation, progress, preview, and
   thumbnail one feature at a time. After each feature, add CPU/CUDA parity coverage.
8. Add GUI selection/status on the worker thread and matching GUI/CLI option tests.
9. Add benchmark script, measure phase timings and peak VRAM, and tune only from those results.
10. Add the optional dependency, lockfile, and separate CUDA bundle last. Test on a clean Windows
    NVIDIA system with no developer CUDA Toolkit installed.
11. Run from `stitcher/`: `uv run ruff check .`, `uv run ruff format --check .`,
    `uv run mypy src`, and `uv run pytest`. Run marked GPU tests and the benchmark separately.
12. Review the diff for accidental schema/default/color/projection changes and document remaining
    manual Windows/GPU validation.

Do not begin with GUI or packaging, do not delete the CPU path, and do not optimize decode/encode
until profiling after the fused compositor is working. Those are later decisions, not prerequisites
for proving the GPU design.
