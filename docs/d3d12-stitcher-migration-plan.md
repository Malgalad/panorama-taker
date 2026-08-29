# D3D12 stitcher migration plan

## Status and objective

This is the implementation plan for replacing the CUDA/CuPy renderer with a Windows x64 D3D12
compute renderer. The completed release has one stitcher archive, selects a hardware D3D12 adapter
from NVIDIA, AMD, or Intel, and falls back to the CPU renderer when preflight or memory admission
fails. It contains no CUDA, CuPy, NVRTC, OpenCL, vendor compute runtime, or vendor-specific build.

The migration also leaves a safe route from the Python/Tk application to a native Windows
application. The D3D12 core must not depend on Python or Tk. Python remains a transitional adapter
and test harness until the native CLI, GUI, codecs, and CPU fallback reach parity; it is removed
only at the final gate. Do not combine the D3D12 renderer and application rewrite in one change.

Current code and tests are authoritative. Preserve session schema/history, projection and pixel
center conventions, hard-blend ties, feather behavior, exposure behavior, incomplete-output
magenta, color transforms, HDR/Rec.2020/PQ/EXR behavior, names, defaults, cancellation, staged
publication, and cleanup.

## Decisions

1. Target Windows 10/11 x64 and the inbox D3D12/DXGI runtime. Start with feature level 11_0,
   root-signature version 1.0, and compute shader model 5.1. Do not require the Agility SDK.
2. Compile HLSL during the signed Windows build and embed the bytecode in the backend DLL. Do not
   ship a runtime shader compiler or compile shaders on the user's machine.
3. Put the implementation in a native core with a narrow C ABI. The transitional Python adapter
   and the later native application must call the same core; neither owns D3D12 resources.
4. Use committed HLSL source and explicit buffers. Manual bilinear sampling avoids texture format,
   texture-array, and maximum-dimension differences for native `uint8`, `uint16`, and `float32`
   sources.
5. Use a direct command queue, fences, default/upload/readback heaps, and placed or committed
   resources initially. Do not add a GPU allocator dependency until measurements justify it.
6. Keep sources resident and support resident and output-banded modes. Calculate D3D12 allocation
   alignment and staging bytes explicitly rather than reusing CUDA allocator assumptions.
7. Perform the tiny exposure graph solve in native CPU `double` arithmetic after downloading only
   the reduced equations. Projection, proxy generation, sample classification, local exposure,
   compositing, histograms, color conversion, and quantization remain on the GPU. This avoids
   optional shader `double` support and is not a return to CPU image processing.
8. Use 32-bit histogram counters after proving the admitted maximum output pixel count cannot
   overflow a bin. Shader model 5.1 does not provide portable 64-bit atomics.
9. Automatic CPU fallback is allowed only before the first numerical GPU dispatch. Device removal
   or another error after work starts fails and cleans up; it never restarts halfway on CPU.
10. Product adapter selection rejects software adapters. Tests may explicitly request WARP so
    shader and lifecycle tests can run on Windows CI without physical GPU access.
11. Keep the user-facing `Use GPU acceleration` setting and `--gpu`/`--no-gpu` behavior during the
    transition. Remove CUDA terminology from user-visible status, logs, archive names, and APIs.

If shader model 5.1 or feature level 11_0 cannot implement a required operation, stop and document
the exact capability before raising the minimum. Do not silently introduce a newer Windows,
driver, or shader-model requirement.

## Target structure

```text
stitcher/
  native/
    CMakeLists.txt
    include/pano_gpu.h             # versioned C ABI and plain data structures
    src/d3d12_*.cpp                # device, resources, session, jobs, diagnostics
    shaders/*.hlsl                 # shared helpers and one entry point per operation
    tests/                         # CTest executables; WARP and pure planning tests
  src/pano_stitch/
    gpu.py                         # backend-neutral orchestration during transition
    d3d12_adapter.py               # ctypes adapter only; no image algorithms
```

The C ABI uses opaque device/session/output/preview handles, fixed-width integer fields, explicit
buffer sizes and strides, caller-owned error buffers, and idempotent destroy functions. Every
creating call returns a result code and writes its handle through a caller-owned out-pointer that
is set to null before work begins and remains null on failure. A destroy call accepts a pointer to
that caller-owned handle, consumes the current value, and sets it to null, so destroying the same
handle variable repeatedly is harmless. Copying an opaque handle does not create shared ownership;
copied aliases become invalid when the owning handle is destroyed.

No exception may cross the ABI. Exported C++ definitions are `noexcept`, translate allocation
failure to an explicit result code, and catch unexpected internal exceptions at the outer boundary.
Do not expose COM pointers, C++ standard-library types, Python objects, exceptions, or HLSL details
across the boundary. Every exported structure starts with its size and ABI version.

Calls should be coarse grained: probe, create session, upload source, finish uploads, build
exposure report, create output job, dispatch/copy a band, render a preview viewport, query
diagnostics, cancel, and destroy. Do not make one foreign-function call per pixel or frame during
compositing.

## Test preservation before implementation

Do this before adding D3D12 production behavior.

### Classify the existing tests

Preserve and make backend-neutral every behavioral assertion currently in
`tests/test_gpu_runtime.py` and the applicable orchestration assertions in
`tests/test_gpu.py`, `tests/test_compositor.py`, and `tests/test_gui_cache.py`:

- final output, coverage, thumbnail, and no disk scratch;
- no calls into CPU projection/compositing helpers;
- adaptive band progress and resident/banded pixel parity;
- in-memory preview creation;
- default exposure behavior and multi-frame exposure parity with CPU;
- native-precision source upload and retained-session reuse;
- preview crop, hover, target, and boundary composition;
- ownership, cancellation, failure cleanup, and repeat-render cleanup;
- preflight fallback versus strict-GPU failure;
- diagnostic transfer counts and phase names;
- preview-to-full reuse without a second upload or exposure solve.

Rename tests and fixtures from `cuda` to `gpu` only when the production names they call are made
backend-neutral. Preserve file history where possible; do not delete and recreate the suite.

The following tests describe an implementation, not behavior, and cannot literally pass on D3D12:

- CUDA source contains named `__global__` kernels;
- CuPy import and CUDA driver error wording;
- CUDA package/NVRTC DLL collection and CUDA archive construction;
- CuPy device and pinned memory-pool counters.

Keep those tests until the corresponding cutover increment. Replace each one in the same commit
with its D3D12 equivalent: embedded shader entry points, adapter/probe failures, absence of runtime
shader compiler and vendor runtimes, one cross-vendor archive, and live native handle/allocation
counters. No behavioral test may disappear as part of that replacement.

### Add missing regression coverage

Add focused tests for behavior not currently exercised on the hardware GPU path:

1. hard and feather output parity for SDR PNG and JPEG;
2. PQ/Rec.2020 input through SDR preview/output within the existing one-code-value tolerance;
3. float/EXR output preserving values above one within the current float tolerance;
4. incomplete coverage, magenta pixels, and coverage output;
5. auto-contrast enabled and disabled;
6. cancellation during upload, compositing, conversion/download, and encoding;
7. injected failure after the first numerical dispatch does not fall back to CPU;
8. device/session/output/preview destruction is idempotent after partial construction;
9. device identity participates in the session-cache key;
10. a maximum admitted output cannot overflow a 32-bit histogram counter.

Use the CPU renderer as the pixel oracle at small dimensions. Keep a few deterministic source
fixtures that cover seams, half-pixel boundaries, poles, clipping, non-finite float rejection, and
hard-blend equal-weight ties. Avoid large binary golden files.

Split tests into three explicit groups:

- platform-independent Python contract and CPU-oracle tests;
- Windows WARP native/integration tests, suitable for CI;
- physical-hardware acceptance on the available Windows 11/NVIDIA system, with the broader vendor
  matrix retained as a deferred non-blocking checklist.

Record a baseline from the current branch before production changes. On Linux run the normal
Python checks excluding the hardware runtime module. On Windows run the full current suite on an
NVIDIA machine and save representative output hashes and diagnostics as migration evidence.

## Incremental implementation

Do not proceed to the next increment until the listed gate passes. Keep compatibility aliases for
one increment when a rename would otherwise mix mechanical churn with new behavior.

For a numbered section with lettered substeps, the **lettered substep is the implementation unit**.
Implement, test, run applicable checks, and record progress for one letter before starting the next.
Do not combine lettered substeps merely because they touch the same source file or shader. If a
substep proves to require a new ownership boundary, data format, algorithmic primitive, or product
route not named in that substep, split it again before implementation.

### Current rewind after native review

The 2026-08-28 native review invalidated the prior completion evidence for 3a, 3c, 3d, 4b, 4c,
and 6b. The earliest incomplete unit is **3a**, not 7a. Repair and pass 3a–3d in order, then 4a–4d,
then rerun step 5, then 6a–6c. Do not begin 7a until all affected Release, WARP, and planner gates
pass again. Historical successful commands remain useful diagnostics but are not completion
evidence for an invalidated gate.

### 1. Freeze backend behavior

- Add the missing regression tests above without changing production behavior.
- Extract shared test builders for sessions and deterministic source images.
- Add markers for `gpu_contract`, `windows_warp`, and `gpu_hardware`.
- Keep all existing CUDA runtime tests running on the current NVIDIA baseline.

Gate: all existing checks and new tests pass; generated outputs from old tests are unchanged.

### 2. Introduce backend-neutral Python contracts

- Rename generic concepts to `GpuDeviceInfo`, `GpuMemoryPlan`, `GpuSession`, `GpuOutputJob`,
  `GpuPreviewDisplay`, and `GpuDiagnostics` in a small compatibility layer.
- Keep temporary `Cuda*` aliases so compositor behavior does not change in the same increment.
- Replace backend literals such as `"cuda"` with a named backend identifier while accepting the
  old internal value through the compatibility adapter.
- Move cache ownership and selection interfaces out of CUDA-specific naming without moving render
  algorithms.

Gate: the CUDA implementation still renders through the neutral interfaces and every behavioral
test passes unchanged.

### 3. Add the native build skeleton

#### 3a. Add the warning-clean native build and Release test harness

- Add `stitcher/native` with MSVC `/W4 /WX /permissive-`, portable warning-as-error flags, CTest,
  and no Python dependency.
- Build and run native tests in both the locally available host configuration and Windows x64
  **Release** configuration.
- Do not use standard `assert` for contract checks. Use an always-evaluated test check helper that
  reports the failed expression/line and returns a nonzero process exit code under `NDEBUG`.

Gate: Debug and Release tests deliberately detect an injected failed check; Release compilation
has no unused-variable warning and CTest performs the same number of checks as Debug.

#### 3b. Define and load the versioned C ABI

- Define result codes, opaque handles, diagnostics, caller-owned error buffers, and structure
  size/ABI headers.
- Add a tiny Python `ctypes` loader with exact argument and return declarations.

Gate: Python loads the Windows DLL, validates the ABI version, and reports a controlled unavailable
result; Linux imports and skips the Windows adapter without failure.

#### 3c. Make creation failures ABI-safe

- Make cancellation-token and all later constructors return a result plus a null-initialized
  out-handle; no exported constructor returns an allocating pointer directly.
- Add an explicit out-of-memory result and translate `std::bad_alloc`/injected allocation failure.
- Mark exported C++ entry points `noexcept` and translate any unexpected exception at the boundary.

Gate: forced allocation failure returns the documented result, leaves the out-handle null, writes
an actionable error, and no C++ exception crosses a native or ctypes caller.

#### 3d. Make destruction idempotent by contract

- Accept a pointer to each caller-owned opaque handle, consume its value, and set it to null.
- Apply the same convention to cancellation, device, session, output, and preview handles.
- Document that copied raw aliases do not own or extend lifetime.

Gate: null pointer, null handle, repeated destruction of the same handle variable, partial
construction cleanup, and normal destruction are harmless in Release native tests.

### 4. Add D3D12 adapter preflight

#### 4a. Enumerate adapter candidates in preference order

- Create the DXGI factory and enumerate high-performance candidates, with `EnumAdapters1` fallback.
- Reject software adapters in product mode and expose WARP only through an explicit test option.
- Separate fatal factory/enumeration errors from candidate-specific incompatibility.

Gate: pure enumeration tests cover preferred order, fallback order, software rejection, WARP
opt-in, empty lists, and fatal enumeration errors.

#### 4b. Select the first fully compatible candidate

- For each non-software candidate inside the enumeration loop, attempt feature-level 11_0 device
  creation and check root-signature 1.0 and shader-model 5.1 support.
- Continue to the next candidate after device/capability incompatibility; do not stop merely because
  the first hardware adapter is unusable.

Gate: an incompatible first adapter followed by a compatible second adapter selects the second;
all-incompatible and device-creation-error cases return distinct actionable results.

#### 4c. Require complete adapter identity and memory-budget data

- Query description, vendor/device IDs, LUID, dedicated memory, and `IDXGIAdapter3` local budget and
  current usage before accepting a candidate.
- Treat interface conversion or `QueryVideoMemoryInfo` failure as candidate incompatibility and
  continue enumeration. Never report success without usable budget/usage values.
- After validating structure headers, initialize every output field deterministically before any
  fallible work. Use budget minus usage for admission with checked subtraction.

Gate: poisoned output-memory tests prove every failure returns deterministic fields; budget-query
failure skips to a later compatible adapter or reports an actionable unavailable result.

#### 4d. Verify real WARP and physical-adapter preflight

- Run WARP preflight in Windows CI and query real physical adapters without changing product
  selection rules.

Gate: WARP passes and the available NVIDIA acceptance record contains expected identity,
dedicated-memory, budget, usage, and usable-memory values. AMD and Intel records remain on the
deferred cross-device checklist.

### 5. Establish the shader build and dispatch path

- Add shared HLSL helpers and a minimal buffer fill/copy-check compute shader.
- Compile to shader model 5.1 during CMake and embed bytecode in the DLL.
- Create root signatures, pipeline states, descriptor heaps, command allocators/lists, queue, and
  fence lifecycle.
- Dispatch on WARP and read back a deterministic buffer.
- Report `GetDeviceRemovedReason` and live object/allocation counters in test diagnostics.

Gate: WARP produces the expected bytes, the release DLL has no dependency on DXC, FXC,
`d3dcompiler_*.dll`, CUDA, or vendor libraries, and all native handles return to zero.

### 6. Port the memory planner

#### 6a. Add checked D3D12 allocation accounting

- Make source, session workspace, output workspace, preview cache, upload, and readback sizes
  backend-neutral.
- Add checked arithmetic, D3D12 buffer/resource alignment, descriptor counts, and the existing
  reserve policy.
- Prove admitted histogram population fits `uint32_t`; reject dimensions that do not.

Gate: overflow/alignment/reserve/descriptor tests pass and plan totals match instrumented WARP
allocations for resident cases.

#### 6b. Select resident or aligned minimum-band mode

- Preserve resident admission when the whole output fits.
- For banded admission, start at `floor(min(output_height, maximum_scheduler_rows) / 32) * 32` and
  test descending positive multiples of 32 through the required 32-row minimum.
- Never decrement an unsigned candidate below 32; an output shorter than 32 rows may be resident
  but cannot use banded mode.

Gate: heights 31, 32, 33, 40, 63, 64, 65, and 1025 cover resident, only-32-fits, larger aligned
bands, insufficient memory, and loop termination without underflow.

#### 6c. Connect adaptive band scheduling

- Preserve the adaptive 64–1024-row scheduler within the admitted aligned workspace capacity.

Gate: existing simulated-memory cases choose the same mode where assumptions are shared; scheduler
feedback never exceeds planned rows and progress terminates exactly at output height.

### 7. Implement session allocation and native source upload

This increment is deliberately split so no substep combines a new ownership layer, transfer
mechanism, and format matrix. Complete and verify each substep before starting the next. No
numerical image shader starts anywhere in 7a-7f.

#### 7a. Make the tested D3D12 device persistent

##### 7a.1. Define device diagnostics and opaque ownership boundaries

- Add the versioned caller-sized diagnostics structure and a query that reports live device, queue,
  and fence counts.
- Define the opaque device handle's ownership boundary without changing creation or dispatch.

Gate: portable structure validation tests reject bad headers; zero diagnostics report no live
objects before any device work.

##### 7a.2. Create and destroy an explicit WARP test device

- Add an exception-safe create out-handle and pointer-to-handle destroy call for a WARP device,
  direct queue, and fence, enabled only by the explicit test option.
- Count each successfully owned object and prove partial creation leaves all counts at zero.

Gate: WARP create/destroy and repeated destroy pass, and device/queue/fence diagnostics return to
zero afterward.

##### 7a.3. Route the fill self-test through the device handle

- Move the existing fill dispatch/readback operation to accept the persistent handle, retaining the
  compatibility self-test entry point as a create/dispatch/destroy wrapper.

Gate: the existing deterministic WARP bytes are produced through the handle and diagnostics remain
zero after the wrapper and direct-handle paths.

##### 7a.4. Admit product hardware devices through the persistent path

###### 7a.4.1. Reuse compatible-candidate admission for device creation

- Extract the already-proven adapter enumeration/admission loop into an internal factory shared by
  preflight and persistent-device creation.
- Preserve the explicit WARP path for tests and retain product-mode software rejection.

Gate: existing WARP preflight and handle-create tests remain green; no product selection behavior
changes while the factory is shared.

###### 7a.4.2. Enable product hardware device creation

- Use the shared factory to create the first fully compatible product hardware device, continuing
  past incompatible candidates exactly as preflight does.

Gate: product creation rejects software adapters and all-incompatible systems; physical-adapter
acceptance records match preflight identity and usable-memory data.

###### 7a.4.3. Expose selected identity through device diagnostics

- Extend the versioned diagnostic query with the selected device identity and usable-memory values
  while preserving deterministic output on invalid and destroyed handles.

Gate: a created device reports identity consistent with preflight; zero live handles report empty
identity and all diagnostics tests pass.

#### 7b. Add an empty transactional session handle

##### 7b.1. Define the versioned empty-session ABI

- Add fixed-width create fields for frame/source dimensions, native sample type, device identity,
  and caller-sized rotation/encoding metadata buffers; do not allocate or parse them yet.

Gate: C and ctypes layout tests cover headers, field widths, sample-type values, and null optional
metadata buffers.

##### 7b.2. Validate empty-session inputs without allocation

- Validate sizes, strides, frame counts, device identity, and metadata-buffer length/overflow
  relationships in a pure validation path.
- Leave every output handle null on every rejection.

Gate: pure contract tests cover every invalid field, size/stride relationship, and arithmetic
overflow without creating a device or session.

##### 7b.3. Create and destroy the empty parent-retaining session

###### 7b.3.1. Introduce ref-counted native device core ownership

- Move adapter/device/queue/fence ownership behind an internal reference-counted device core while
  preserving the public opaque device handle and diagnostics.
- Prove a normal create/dispatch/destroy cycle retains the same live-count behavior.

Gate: existing WARP handle and wrapper dispatch tests pass unchanged; no core object survives after
the last device handle is destroyed.

###### 7b.3.2. Create and destroy an empty session retaining the device core

- Add exception-safe empty-session create and pointer-to-handle destroy calls; the session retains
  the internal parent core but owns no source resources.
- Add a live-session diagnostic counter.

Gate: WARP creates and destroys an empty session with zero source allocations and no leaked native
objects.

###### 7b.3.3. Verify parent/child ordering and partial construction cleanup

- Exercise session-before-device and device-before-session destruction; native objects remain alive
  until the last owner is released.
- Inject session-handle allocation failure and prove every output handle remains null.

Gate: both orders and injected partial construction end with zero live sessions, devices, queues,
and fences.

#### 7c. Allocate resident source and metadata resources

##### 7c.1. Allocate one native-precision resident source buffer

- Allocate one default-heap source buffer using already-validated session dimensions and native
  sample bytes, without uploading data or allocating metadata.
- Prove checked size/alignment calculations and cleanup after allocation failure.

Gate: `uint8`, `uint16`, and `float32` single-frame WARP allocations report expected bytes and
leave zero resources after destroy.

##### 7c.2. Allocate resident storage for all frames

- Extend the proven source buffer to the accepted frame count without changing sample precision or
  allocating an upload slot.
- Reject overflow and plan mismatches before resource creation.

Gate: multi-frame WARP allocation sizes match the accepted plan for every source type; injected
allocation failure leaves zero resources.

##### 7c.3. Allocate and upload rotation metadata

###### 7c.3.1. Allocate immutable rotation storage

- Allocate default-heap rotation storage for the already-validated matrix byte count without
  uploading caller data.

Gate: all valid frame counts allocate the expected aligned rotation buffer; allocation failure
leaves no rotation storage.

###### 7c.3.2. Upload rotations through one temporary staging resource

- Copy exactly one caller-provided validated rotation buffer into immutable storage, then release
  the temporary staging resource after fence completion.

Gate: repeated upload is rejected and no staging resource persists after a successful upload.

###### 7c.3.3. Add test-only rotation readback

- Read immutable rotation storage through a temporary readback resource solely for native tests.

Gate: deterministic matrices round-trip byte-for-byte on WARP; no readback resource persists.

##### 7c.4. Allocate and upload encoding metadata

###### 7c.4.1. Allocate optional immutable encoding-metadata storage

- Allocate default-heap storage only when caller-sized encoding metadata is present; null/zero
  metadata creates no resource.

Gate: absent metadata performs no allocation; present metadata allocates the expected aligned bytes
and failure leaves no storage.

###### 7c.4.2. Upload optional encoding metadata

- Upload one caller-provided metadata buffer through a temporary staging resource and persistent
  fence; reject repeated or mismatched uploads.

Gate: staging does not persist after completion and invalid metadata upload does not submit work.

###### 7c.4.3. Add test-only encoding-metadata readback

- Read metadata through a temporary native readback resource solely for byte-exact native tests.

Gate: present metadata round-trips exactly and absent metadata has no resource or bytes to read.

##### 7c.5. Report source and metadata allocation accounting

- Record planned versus actual source/metadata allocation bytes and descriptor counts in session
  diagnostics.

Gate: WARP diagnostics match allocation instrumentation for all source types and failure paths.

#### 7d. Upload one source through one reusable slot

##### 7d.1. Define single-slot source-upload ABI and pure validation

- Add a caller-buffer upload structure and validate frame index, byte count, stride, and native
  sample type without allocating a staging resource or recording commands.

Gate: pure contract tests reject every invalid relationship and leave upload diagnostics unchanged.

##### 7d.2. Allocate one persistent mapped upload slot

- Allocate one upload-heap staging slot sized for one validated native source frame and persistently
  map it for the session lifetime.

Gate: all three sample types allocate expected aligned slot bytes; repeated allocation is rejected
and session destruction unmaps/releases the slot.

##### 7d.3. Upload the first frame through the slot

###### 7d.3.1. Submit one frame-zero copy

- Copy one validated caller buffer into the mapped slot and record a copy into frame zero of the
  resident source buffer, then complete the slot fence.

Gate: valid frame-zero input submits exactly one bounded copy; invalid input submits none.

###### 7d.3.2. Add test-only frame-zero readback

- Read frame zero through a temporary native readback resource solely for byte-exact native tests.

Gate: deterministic `uint8`, `uint16`, and `float32` frame-zero bytes round-trip exactly on WARP.

##### 7d.4. Reuse the completed slot for another frame

###### 7d.4.1. Generalize copy/readback internals without changing frame-zero ABI

- Factor the proven frame-zero copy/readback implementation into internal frame-indexed helpers
  while retaining the existing public frame-zero validation behavior.

Gate: existing frame-zero byte-exact tests pass unchanged.

###### 7d.4.2. Submit a completed-slot copy for frame one

- Add the public indexed upload operation, wait for the prior slot fence before overwriting mapped
  staging memory, and copy into frame one without new persistent allocation.

Gate: two distinct frames round-trip in order through the one slot; persistent slot bytes do not
grow.

##### 7d.5. Report single-slot upload accounting

- Expose submitted upload count, byte count, and last completed slot fence through session
  diagnostics.

Gate: invalid uploads submit nothing; valid uploads report exact counts/bytes and all session
diagnostics return to zero after destruction.

#### 7e. Add the second upload slot and cancellation

##### 7e.1. Allocate a second independently tracked upload slot

- Add a second persistent mapped slot with its own allocation, mapping, and last-fence state;
  retain the proven first-slot behavior.

Gate: both slots allocate expected bounded bytes and session destruction unmaps/releases both.

##### 7e.2. Introduce selected-slot state without changing submission timing

- Represent the chosen upload resource, mapped pointer, and prior fence as one local selection;
  retain the first-slot-only upload route and synchronous completion behavior.

Gate: the existing one-slot upload/readback contract remains unchanged and no upload allocation is
introduced after setup.

##### 7e.3. Select slots round-robin for validated uploads

- Select the first or second persistent slot from the upload sequence, but continue waiting for each
  submitted copy while this routing change is proved.

Gate: two frames use distinct resident slots and preserve their byte order through readback.

##### 7e.4. Extract source-upload completion waiting

- Move the existing source-upload fence event/wait into a helper while retaining the post-submit
  wait and current `last_completed_upload_fence` semantics.

Gate: the one- and two-slot upload/readback contract remains unchanged.

##### 7e.5. Defer completion until the selected slot is reused

- Replace the post-submit wait with a wait only for the selected slot's prior fence before its mapped
  bytes are overwritten.

Gate: two-slot uploads return after submission and diagnostics report only actually observed fence
completion.

##### 7e.6. Prove first-slot reuse after deferred completion

- Exercise three source uploads so the third upload waits for and safely reuses the first slot.

Gate: more than two frames preserve byte order with no new staging allocation after setup.

##### 7e.7. Add an ABI-compatible cancellable upload entry point

- Add a distinct source-upload entry point accepting an optional cancellation token while retaining
  the existing upload entry point as an uncancelled compatibility wrapper.

Gate: an active token follows the existing upload/readback path unchanged.

##### 7e.8. Add cancellation before wait and submission

- Accept a cancellation token and check it before slot wait and before recording/submitting copy
  commands.

Gate: pre-cancelled uploads submit no work, preserve prior bytes, and leave the session destroyable.

##### 7e.9. Add cancellation after a completed slot wait

- Check cancellation after the selected slot's fence completes and before overwriting mapped bytes.

Gate: forced slot-wait cancellation leaves prior slot/source bytes intact and counts bounded.

##### 7e.10. Finish the first submitted upload slot

- Add an explicit finish entry point that waits for a submitted first-slot fence, is a no-op for an
  idle session, and submits no work itself.

Gate: idle and first-slot sessions finish deterministically with accurate completion diagnostics.

##### 7e.11. Finish the second submitted upload slot

- Extend finish to wait for the second slot only when it has a distinct submitted fence.

Gate: alternating uploads finish both slots without a staging allocation or additional submission.

##### 7e.12. Add cancellation to upload finishing

- Accept an optional cancellation token, checking it before and after each finish wait; cancellation
  and wait errors remain destroyable and submit no further work.

Gate: finish handles idle, alternating, cancelled, and failed uploads with zero live resources after
session destruction.

#### 7f. Integrate retained-session reuse and cache identity

##### 7f.1. State retained-session ownership

- Document the native rule that successfully finished source and metadata uploads remain resident
  until session destruction, while output and preview jobs hold a shared session reference.

Gate: native session destruction remains the sole release point for resident sources and metadata.

##### 7f.2. Add backend-neutral cache identity fields

- Introduce a cache-key representation containing backend kind, adapter identity, and backend ABI,
  without changing the existing CUDA call sites or cache eviction behavior.

Gate: keys differing only in backend, adapter LUID, or ABI do not compare equal.

##### 7f.3. Adapt the temporary CUDA key

- Route the existing CUDA-specific key builder through the backend-neutral key representation for
  one compatibility increment, preserving its current source/geometry/budget fingerprint.

Gate: existing CUDA cache-key and replacement/invalidation tests remain unchanged in behavior.

##### 7f.4. Bind native retained-session lifecycle calls

- Add ctypes declarations for native device/session creation, upload finishing, diagnostics, and
  pointer-to-handle destruction without changing compositor backend selection.

Gate: mock-library contract tests prove every declaration has the expected argument and result type.

##### 7f.5. Add a closeable retained D3D12 prepared-session owner

- Wrap a finished native session in the same closeable prepared-session ownership contract used by
  the cache, without routing application rendering through D3D12 yet.

Gate: owner destruction releases session before device and is idempotent.

##### 7f.6. Add retained D3D12 child-job ownership

- Make the output-job wrapper retain its parent prepared session and release before it once the
  Step 9 native output API exists.
- Defer the preview-job half until Step 12 provides native preview creation/destruction. That
  deferred half does not block Steps 8-10e.

Gate now: closing/replacing a native prepared session destroys output jobs before session and device
and returns native live counters to zero. Step 12 repeats the gate with preview children included.

##### 7f.7. Reuse a prepared D3D12 session from preview to full output

- Select the D3D12 cache key when preflight admits the native backend and reuse a matching prepared
  session across preview and final setup.
- Dependency: completed native output and preview job adapters.

Scheduling: execute this substep immediately after the Step 12 preview-job adapter; it is not an
implementation gate for Steps 8-10e because its prerequisite does not yet exist.

Gate: preview-to-full reuse performs one source upload set and no second metadata upload; changing
device identity misses the cache.

##### 7f.8. Complete retained-session failure injection

- Add focused injection for session creation, metadata upload, both source slots, fence signaling,
  and finish-upload waits; verify cancellation/failure/replacement tears down child jobs before the
  session and device.

Gate: every injected failure leaves all live counters at zero.

### 8. Port one-frame projection and hard compositing

#### 8a. Add an output-job lifecycle without image work

##### 8a.1. Define and validate output-job creation options

- Add a versioned output-job creation structure and validation ABI call; validate parent session and
  selected resident-versus-banded planning inputs without allocating a job or GPU resource.

Gate: invalid structure, parent, dimensions, and plan combinations create no job or resource.

##### 8a.2. Create an empty resident output job

###### 8a.2.1. Introduce shared session state behind the opaque handle

- Move existing session fields into a refcounted internal state owned by the opaque session handle,
  without changing any exported function signature or resource behavior.

Gate: all existing session creation, upload, diagnostic, and destruction tests pass unchanged.

###### 8a.2.2. Retain shared session state from an empty output handle

- Add an output handle that retains the internal session state but allocates no GPU output resource.

Gate: destroying the caller session handle before the output leaves device/session diagnostics valid;
destroying output then releases the final state exactly once.

###### 8a.2.3. Allocate resident linear RGB storage

- Allocate only the accepted full linear RGB resource and report planned/actual bytes.

Gate: a resident WARP job reports exact bounded linear bytes.

###### 8a.2.4. Allocate resident coverage storage

- Add the accepted full coverage resource and include it in diagnostics and teardown.

Gate: a resident WARP job reports exact bounded RGB and coverage bytes.

##### 8a.3. Create an empty one-band output job

###### 8a.3.1. Retain validated band configuration

- Store whether the accepted output is resident or banded, without changing resource allocation.

Gate: resident and banded output handles report their selected mode and no resource is allocated.

###### 8a.3.2. Plan the initial full-width band

- For a banded handle, calculate the initial row range and RGB/coverage byte plans from one aligned band,
  rather than the full output height.

Gate: a large forced-banded WARP job reports row range `[0, band_rows)` and bounded band byte plans.

###### 8a.3.3. Allocate the initial band resources

- Reuse the output resource allocation paths for the planned initial band and preserve exact diagnostics.

Gate: the forced-banded WARP job allocates only its first RGB and coverage band.

##### 8a.4. Harden output-job ownership and teardown

###### 8a.4.1. Report live output-job handles

- Add a native live-output diagnostic counter registered only after successful handle construction.

Gate: empty resident and banded handle creation/destruction changes the counter exactly once.

###### 8a.4.2. Reject partial output construction safely

- Add focused output-handle allocation failure injection; preserve a null out-handle and all live counters.

Gate: injected output-handle creation failure leaves device, session, and output diagnostics unchanged.

###### 8a.4.3. Tear down resource-owning output handles safely

- Verify resident and banded output resources release before their retained session; repeated destroy and
  job-before-session destruction remain harmless.

Gate: resident and forced-banded jobs return all live allocation counts to zero. No projection shader is
present yet.

#### 8b. Port equirectangular ray generation and camera projection

##### 8b.1. Define test-only projection request and result contracts

- Add versioned camera and row-range inputs plus explicit projected-coordinate/validity result layout.
- Reject malformed matrices, dimensions, and ranges before any output resource is allocated.

Gate: malformed request and buffer contracts fail without D3D12 allocation.

##### 8b.2. Generate equirectangular pixel-center rays for a row range

###### 8b.2.1. Compile the ray-only compute shader

- Add a separate embedded shader header whose sole output is one float3 world ray per requested pixel.

Gate: MSVC builds the ray-only shader and retains the existing self-test shader unchanged.

###### 8b.2.2. Dispatch one ray-only row range

- Create the root signature, UAV, temporary output resource, and constants for the validated request.

Gate: a WARP dispatch succeeds for a requested nonzero row range without source sampling or projection.

###### 8b.2.3. Read back ray-only results

- Copy the temporary world-ray output to test-owned memory after its fence completes.

Gate: seam, pole, center, and edge fixtures match CPU ray coordinates within the existing float tolerance.

##### 8b.3. Apply recorded world-to-camera rotation

###### 8b.3.1. Extend the ray shader contract with row-major rotation output

- Add the validated nine-float world-to-camera matrix and a camera-ray UAV, without source projection.

Gate: identity rotation dispatches successfully for a requested row range.

###### 8b.3.2. Read back and compare rotated camera rays

- Copy camera rays to test-owned memory and compare identity and axis-rotation fixtures to CPU multiplication.

Gate: identity and axis-rotation fixtures match CPU camera-space ray coordinates on WARP.

##### 8b.4. Project camera-space rays and record validity

###### 8b.4.1. Extend the test shader contract with source-camera constants

- Bind source dimensions and focal lengths while preserving camera-ray generation and readback.

Gate: identity camera constants dispatch successfully without a projection result buffer.

###### 8b.4.2. Write and read back unclamped projected coordinates

- Write one float2 projected coordinate per camera ray using the source center and focal lengths.

Gate: center and edge fixtures match CPU coordinates before validity masking or clamping.

###### 8b.4.3.1. Add packed validity output

- Apply `z > 0` and half-pixel bounds, then write one packed validity byte per pixel without changing coordinates.

Gate: centers, edges, invalid-behind-camera, and half-pixel fixtures match CPU validity masks on WARP.

###### 8b.4.3.2. Clamp valid and invalid coordinates to source centers

- Clamp projected coordinates to source centers while preserving the validity bytes.

Gate: centers, edges, invalid-behind-camera, and half-pixel fixtures match CPU coordinates and masks on WARP.

#### 8c. Add manual `uint8` bilinear sampling

##### 8c.1. Define a test-only `uint8` sampling contract

- Accept a finished resident `uint8` source frame, caller-owned float2 coordinates, and an exact float3 result layout.
- Reject unfinished sessions, non-`uint8` sources, invalid frame/coordinate ranges, and mismatched buffers before D3D12 allocation.

Gate: sampling requests have exact source/coordinate/result byte contracts and all malformed calls fail without a dispatch.

##### 8c.2.1. Transition finished resident sources to shader-readable state

- Track the resident source buffer state through the existing upload and finish-upload commands.
- Transition to `NON_PIXEL_SHADER_RESOURCE` only after the completed upload set, and transition back before a later upload.

Gate: repeated upload/finish cycles preserve source readback and leave the finished source valid for SRV binding on WARP.

##### 8c.2.2. Bind resident `uint8` sources and read exact-center samples

- Compile a dedicated sampling shader and bind the resident interleaved-RGB source buffer, caller coordinates, and a float3 output.
- Return exact integer-center samples through a test-only readback, without interpolation or blending.

Gate: a deterministic resident source's corners and interior integer centers match direct CPU `uint8` loads.

##### 8c.3. Add manual interior bilinear interpolation

- Replace exact-center loads with four interleaved-RGB `uint8` loads and float interpolation for coordinates strictly inside the source bounds.

Gate: deterministic interior half-pixel fixtures match the CPU `uint8` sampler.

##### 8c.4. Apply source-coordinate boundary semantics

- Clamp sampling coordinates to source centers before manual bilinear loads, without blending or quantization.

Gate: deterministic corner and clipped-coordinate fixtures match the CPU `uint8` sampler.

#### 8d. Add manual `uint16` bilinear sampling

##### 8d.1. Define a test-only `uint16` sampling contract

- Reuse the proven coordinate/result layout while admitting only finished resident `uint16` source frames.

Gate: malformed, unready, wrong-type, and mismatched-buffer `uint16` calls fail without a dispatch.

##### 8d.2.1. Compile the dedicated `uint16` sampling shader

- Embed a `cs_5_1` shader that declares the typed `R16_UINT` source-load contract without changing dispatch code.

Gate: the Windows build embeds the `uint16` shader independently of the proven `uint8` sampler.

##### 8d.2.2. Bind resident `uint16` sources and read exact-center samples

- Compile a dedicated shader that reads the resident interleaved-RGB `R16_UINT` source buffer and normalizes native values.

Gate: deterministic corner and interior integer centers match direct CPU `uint16` loads without source expansion.

##### 8d.3. Add manual `uint16` bilinear interpolation

- Reuse the proven four-tap coordinate path with native `uint16` loads and float interpolation.

Gate: interior half-pixel fixtures match the CPU `uint16` sampler.

##### 8d.4. Apply `uint16` source-coordinate boundary semantics

- Clamp finite coordinates to source centers before `uint16` loads.

Gate: the same corner and clipping fixtures match CPU `uint16` results without source expansion.

#### 8e. Add manual `float32` bilinear sampling

##### 8e.1. Define float32 sampling and non-finite admission semantics

- Reuse the proven coordinate/result layout for finished resident `float32` frames.
- Preserve the CPU sampler boundary: finite coordinates are rejected at admission, while resident float pixels
  (including NaN and infinities from EXR) proceed to sampling and are filtered only by later compositing/exposure stages.

Gate: malformed, unready, wrong-type, and non-finite-coordinate fixture contracts fail before dispatch; a resident
NaN/±∞ source fixture is explicitly admitted for CPU-compatible sampling.

##### 8e.2. Compile and bind native float32 exact-center sampling

- Embed a dedicated `cs_5_1` shader with typed `R32_FLOAT` loads and bind it through the proven temporary readback path.

Gate: finite direct corner/interior samples match CPU float values without source expansion.

##### 8e.3.1. Add float32 interior bilinear interpolation

- Reuse the proven four-tap coordinate path for finite coordinates strictly inside the source bounds.

Gate: finite interior half-pixel fixtures match CPU float sampling within the established tolerance.

##### 8e.3.2. Apply float32 source-center clipping

- Clamp finite float32 coordinates to source centers before the four-tap loads.

Gate: finite clipped-coordinate fixtures match CPU float sampling within the established tolerance.

##### 8e.4. Apply the established non-finite source result rule

- Implement the explicitly tested CPU-compatible result for NaN and infinity source taps.

Gate: finite and non-finite fixtures match CPU float sampling within the established tolerance.

#### 8f. Add one-frame hard compositing and coverage

##### 8f.1. Define a one-frame composite band contract

- Add a test-only request that identifies a finished source frame, output band geometry, projection
  parameters, and caller-owned linear RGB/coverage result buffers.
- Validate exact byte layouts, finite geometry, output bounds, source type, and resident-frame readiness
  before recording work.

Gate: malformed, unready, wrong-type, out-of-band, and non-finite geometry calls fail without dispatch.

##### 8f.2.1. Dispatch one-frame projection and validity into temporary band candidates

- Reuse the established embedded projection shader through the one-frame band contract to generate world/camera rays,
  source coordinates, and source-validity state for one output band and one source frame.

Gate: finite one-frame interior/corner coordinates and source-validity results match the CPU oracle.

##### 8f.2.2.1. Embed a uint8 one-frame candidate shader

- Compile an embedded `cs_5_1` shader that combines the established one-frame projection, validity, source-center
  clipping, and `R8_UINT` bilinear candidate-load contracts.

Gate: the Windows build embeds the uint8 one-frame candidate shader independently of the projection primitive.

##### 8f.2.2.2. Bind a resident uint8 source and read candidate RGB/validity

- Bind a resident `R8_UINT` source and read back clipped bilinear float RGB candidates plus source-validity bits.

Gate: `uint8` one-frame interior/corner candidate RGB and validity match the CPU oracle.

##### 8f.2.3.1. Embed a uint16 one-frame candidate shader

- Compile an embedded `cs_5_1` shader that combines the established one-frame projection, validity, source-center
  clipping, and `R16_UINT` `/65535` bilinear candidate-load contracts.

Gate: the Windows build embeds the uint16 one-frame candidate shader independently of the uint8 path.

##### 8f.2.3.2. Bind a resident uint16 source and read candidate RGB/validity

- Bind a resident `R16_UINT` source and read back clipped bilinear float RGB candidates plus source-validity bits.

Gate: `uint16` one-frame interior/corner candidate RGB and validity match the CPU oracle.

##### 8f.2.4.1. Embed a float32 one-frame candidate shader

- Compile an embedded `cs_5_1` shader that combines the established one-frame projection, validity, source-center
  clipping, and `R32_FLOAT` bilinear candidate-load contracts.

Gate: the Windows build embeds the float32 one-frame candidate shader independently of the integer paths.

##### 8f.2.4.2. Bind a resident float32 source and read candidate RGB/validity

- Bind a resident `R32_FLOAT` source and read back clipped bilinear float RGB candidates plus source-validity bits,
  preserving the established finite and non-finite source-value semantics.

Gate: `float32` one-frame interior/corner candidate RGB and validity match the CPU oracle.

##### 8f.2.5. Surface one-frame candidate edge distance

- Extend each typed candidate shader and test-only readback contract with the clipped source-edge distance used by
  the CPU hard-blend candidate weight.

Gate: uint8, uint16, and float32 candidate edge distances match the CPU oracle for interior, edge, and invalid pixels.

##### 8f.3.1. Define hard-selection inputs and accumulator layout

- Add a test-only hard-selection request for one candidate RGB/validity band and caller-owned prior linear RGB,
  prior weight, and result RGB/coverage buffers.
- Validate exact layouts and finite nonnegative weights before dispatch.

Gate: malformed layouts, invalid validity masks, and non-finite/negative candidate or prior weights fail before dispatch.

##### 8f.3.2. Embed the hard-selection and coverage shader

- Compile an embedded `cs_5_1` shader that converts valid source edge distance into the established hard candidate
  weight and conditionally replaces RGB/weight/coverage.

Gate: the Windows build embeds the hard-selection shader independently of candidate generation.

##### 8f.3.3. Bind hard-selection accumulators and read one selected band

- Bind candidate/prior buffers, write selected linear RGB and coverage, and preserve strict
  `candidate > previous_weight` replacement semantics so equal weights retain the earlier value.

Gate: seams, poles, edge coverage, and equal-weight ties match the CPU hard-blend oracle.

##### 8f.3.4.1. Chain uint8 candidate generation and hard selection on the GPU

- Feed uint8 candidate RGB, packed validity, and edge-distance output directly into hard selection in one command sequence,
  with UAV ordering between passes and no host candidate readback.

Gate: a one-frame uint8 band reaches selected GPU RGB/weight/coverage without intermediate host copies.

##### 8f.3.4.2. Chain uint16 candidate generation and hard selection on the GPU

- Extend the same command sequence with typed R16 candidate sources and element-based source offsets.

Gate: a one-frame uint16 band reaches selected GPU RGB/weight/coverage without intermediate host copies.

##### 8f.3.4.3. Chain float32 candidate generation and hard selection on the GPU

- Extend the same command sequence with typed R32_FLOAT candidates, preserving established IEEE source semantics.

Gate: a one-frame float32 band reaches selected GPU RGB/weight/coverage without intermediate host copies.

##### 8f.4. Read back one linear RGB and coverage band

- Copy only the requested band into caller-owned RGB and coverage buffers, without encoding or allocating a
  full output image.

Gate: a full one-frame WARP band round-trips with CPU-oracle RGB/coverage parity for every source type.
SDR conversion remains out of scope.

### 9. Port multi-frame feather and local exposure

#### 9a. Extend hard composition to multiple frames

##### 9a.1. Define a test-only ordered hard-composition band contract

- Add a fixed-layout request for an ordered list of finished resident frame requests that share one output band,
  plus caller-owned selected RGB, weight, and coverage buffers.
- Validate list count, exact common band geometry, strictly increasing capture order, source-type agreement, and
  final result layouts before any GPU work.

Gate: empty, malformed, mixed-band, unordered, unready, and wrong-type frame lists fail without dispatch.

##### 9a.2.1. Define the two-frame uint8 hard-composition dispatch contract

- Expose a test-only dispatch entry point using the validated ordered request and caller-owned final RGB, weight,
  and coverage buffers.
- Reject non-uint8 lists and incorrect final buffer layouts before allocating GPU command resources.

Gate: malformed output buffers and non-uint8 ordered requests fail without dispatch.

##### 9a.2.2. Allocate two uint8 candidate workspaces and a ping-pong hard accumulator

- Allocate per-pass candidate RGB/validity/edge buffers plus two selected RGB/weight pairs with explicit initial
  resource states.
- Create the descriptor ranges required to read one selected pair as the next pass's prior input.

Gate: WARP records the two-pass resource setup and releases every temporary resource on success or failure.

##### 9a.2.3. Dispatch the first uint8 candidate into the zeroed hard accumulator

- Submit the established uint8 candidate pass followed by hard selection against a zeroed GPU-resident accumulator.

Gate: the first pass matches the proven one-frame uint8 result without reading it back before the second pass.

##### 9a.2.4. Dispatch the second uint8 candidate against the first selected result

- Transition the first selected pair to non-pixel SRV and write the second selection result to the alternate pair.
- Preserve strict `candidate > prior_weight` ties and retain final coverage only.

Gate: an overlapping two-frame uint8 fixture, including a strict equal-weight tie, matches the CPU hard-blend oracle
without host intermediate copies.

##### 9a.2.5. Read back the final two-frame uint8 hard-composition band

- Copy only the second selected RGB, weight, and coverage buffers after the second pass completes.

Gate: the two-frame result uses one final band readback set and no candidate or prior host copies.

##### 9a.2. Chain two uint8 candidates through one device-resident hard accumulator

- This heading is complete only after 9a.2.1–9a.2.5: reuse the existing uint8 candidate and hard-selection passes
  twice in one command sequence, retaining selected RGB/weight on the GPU between passes.

Gate: an overlapping two-frame uint8 fixture, including a strict equal-weight tie, matches the CPU hard-blend oracle
without host intermediate copies.

##### 9a.3. Add three-frame and capture-order hard-selection fixtures

- Extend the same uint8 sequence to a third frame without changing its buffer ownership or readback shape.
- Exercise lower-weight retention and an equal-weight tie after an earlier replacement.

Gate: three-frame uint8 output and coverage match the CPU oracle in capture order.

##### 9a.4.1. Define the two-frame uint16 hard-composition dispatch contract

- Expose a test-only two-frame uint16 dispatch entry point using the existing ordered request and final-band layout.
- Reject non-uint16 lists and incorrect final buffers before allocating GPU command resources.

Gate: malformed output buffers and non-uint16 ordered requests fail without dispatch.

##### 9a.4.2. Reuse the resident two-frame hard chain with uint16 candidates

- Select the existing uint16 candidate pipeline and use element-based source stride/frame offsets.
- Retain the established two selected RGB/weight pairs and one final readback set.

Gate: an overlapping two-frame uint16 fixture matches CPU hard blending without source expansion.

##### 9a.4.3. Extend the uint16 chain to three capture-ordered frames

- Reuse the uint16 candidate path and ping-pong accumulator without changing resource ownership.

Gate: three-frame uint16 output and coverage match the CPU oracle, including lower-weight and equal-weight retention.

##### 9a.5.1. Define the two-frame float32 hard-composition dispatch contract

- Expose a test-only two-frame float32 dispatch entry point with the existing ordered request/final-band layout.
- Reject non-float32 lists and malformed final buffers before GPU allocation.

Gate: malformed output buffers and non-float32 ordered requests fail without dispatch.

##### 9a.5.2. Reuse the resident two-frame hard chain with float32 candidates

- Select the existing float32 candidate pipeline with element-based source stride/frame offsets.
- Preserve established IEEE source-value behavior and one final readback set.

Gate: an overlapping two-frame float32 fixture matches CPU hard blending without source expansion.

##### 9a.5.3. Extend the float32 chain to three capture-ordered frames

- Reuse the float32 candidate path and ping-pong accumulator without changing resource ownership.

Gate: three-frame float32 output and coverage match the CPU oracle, including lower-weight and equal-weight retention.

##### 9a.6.1. Define one-output-band ordered-hard dispatch admission

- Add a test-only dispatch contract that accepts an allocated output handle and an ordered hard-composition request.
- Require exact output width, current band row range, and resident linear/coverage storage before recording commands.

Gate: missing output storage, mismatched geometry, and incompatible ordered requests fail before dispatch.

##### 9a.6.2. Copy final selected GPU buffers into one output-job band

- Let the ordered hard-composition recording path copy its final selected RGB and coverage buffers into the already
  allocated output band, with explicit resource transitions and no host candidate/prior intermediates.
- Do not add band advancement, scheduling, feathering, exposure gains, encoding, or Python production routing.

Gate: WARP records one selected multi-frame uint8 band into the output handle with bounded workspace.

##### 9a.6.3. Read back and verify one stored output-job band

- Add a test-only output-band readback helper that copies only the stored output band after the compositor fence.
- Verify linear RGB and coverage against the multi-frame hard-selection oracle.

Gate: a test-only output job reads one selected multi-frame linear band and coverage with one final bounded readback.

#### 9b. Add feather accumulation and normalization

##### 9b.1. Define feather candidate-weight inputs and accumulator layout

- Add a test-only contract for finite source dimensions, candidate validity/edge distance, and linear RGB/weight
  accumulation buffers. Keep hard selection unchanged.

Gate: malformed layouts, invalid source dimensions, and non-finite values fail before dispatch.

##### 9b.2. Embed and verify one-frame feather weight generation

- Compile an SM 5.1 shader that applies the existing `max(1, min(width, height) * 0.08)` feather width and
  `max(edge_distance / feather_width, 1e-6)` valid-candidate rule.

Gate: interior, edge, invalid, and minimum-dimension weights match the CPU oracle.

##### 9b.3. Accumulate one frame's weighted RGB and weight on the GPU

- Bind a candidate RGB/weight pair and zeroed accumulators; add weighted RGB and scalar weight without host
  intermediates.

Gate: one-frame RGB/weight accumulators match CPU float32 results.

##### 9b.4.1. Chain two frames into the feather accumulator

- Reuse resident weighted RGB and scalar-weight accumulators across a second ordered candidate pass.
- Preserve the existing float32 accumulation order without host candidate/prior copies.

Gate: a two-frame overlapping seam fixture matches CPU accumulated RGB and weights.

##### 9b.4.2. Extend feather accumulation to a third ordered frame

- Ping-pong the two resident feather accumulator pairs through a third candidate pass.

Gate: a three-frame pole fixture matches CPU accumulated RGB and weights.

##### 9b.4.3. Verify deterministic ordered feather arithmetic

- Exercise distinct ordered frame values where reassociation would change float32 results.
- Keep the caller-requested capture order; do not sort or parallel-reduce frames.

Gate: ordered three-frame GPU accumulation bitwise matches the CPU frame-order fixture where the documented
arithmetic permits it, or otherwise matches the established float32 tolerance.

##### 9b.5. Normalize covered pixels in a separate final pass

- Divide accumulated RGB only where the final weight is positive; leave uncovered pixels identifiable for the
  incomplete-output stage.

Gate: normalized covered pixels and unnormalized uncovered state match CPU fixtures.

#### 9c. Add supplied global and local exposure gains

##### 9c.1. Define supplied-gain and local-field metadata contracts

- Add fixed layouts for finite per-frame global gains and the existing quarter-resolution local-field parameters.
- Validate frame counts and field dimensions without performing exposure analysis or changing hard blending.

Gate: mismatched counts, non-finite gains, and malformed local-field layouts fail before dispatch.

##### 9c.2. Apply one supplied global gain to one candidate band

- Multiply finite candidate RGB by one caller-supplied frame gain before hard or feather composition.

Gate: identity and non-identity one-frame fixtures match CPU linear RGB.

##### 9c.3. Apply supplied global gains across an ordered hard-composition chain

- Bind each frame's own supplied gain while retaining strict hard-selection tie behavior.

Gate: overlapping differently gained frames match CPU hard output and coverage.

##### 9c.4.1. Represent output projection mode for local exposure

- Extend the native output geometry contract to distinguish the existing equirectangular panorama and rectilinear
  thumbnail modes, including the rectilinear vertical FOV. Preserve all existing equirectangular callers unchanged.

Gate: malformed projection-mode metadata fails before dispatch, while existing equirectangular requests retain their
current layout and behavior.

##### 9c.4.2. Build an equirectangular quarter-resolution local-exposure field

- Port the existing equirectangular field construction inputs and storage for a deterministic one-frame fixture.

Gate: equirectangular local-field sample values and ceil-quarter dimensions match CPU output.

##### 9c.4.3. Build the rectilinear thumbnail local-exposure field

- Apply the same field construction with the existing rectilinear direction and vertical-FOV conventions.

Gate: rectilinear thumbnail local-field values and dimensions match the CPU output.

##### 9c.5. Sample the local field with existing interpolation and boundary conventions

- Apply the established interpolation to one candidate band after global gain.

Gate: interior and clipped local-field samples match CPU linear RGB.

##### 9c.6. Apply local exposure across ordered composition

- Bind per-frame local fields in the established candidate path without implementing automatic exposure analysis.

Gate: identity, global-only, and spatial local-exposure fixtures match the CPU oracle. Exposure
analysis remains out of scope.

#### 9d. Add incomplete-output marking

##### 9d.1. Mark uncovered hard-composition pixels after selection

- Write linear magenta only where the final hard-selection weight is zero; preserve coverage.

Gate: hard-mode uncovered pixels and coverage match CPU fixtures.

##### 9d.2. Mark uncovered feather-composition pixels after normalization

- Reuse the final feather weight after normalization; do not alter covered RGB.

Gate: uncovered pixels and the coverage buffer match CPU fixtures for hard and feather modes.

#### 9e. Add forced-banded execution

##### 9e.1. Bind a nonzero output-band row offset through one compositing request

- Reuse the one-frame band path with a later row range and bounded temporary resources.

Gate: the later-band projection/candidate result matches the CPU oracle without allocating full output height.

##### 9e.2. Execute ordered hard composition in two forced bands

- Run the existing multi-frame hard primitive independently for adjacent bands.

Gate: concatenated forced-band hard pixels and coverage match the resident result across the boundary.

##### 9e.3. Execute feather composition in two forced bands

- Reuse the feather accumulator and normalization independently for adjacent bands.

Gate: resident and forced-banded linear pixels and coverage are identical across band boundaries.

#### 9f. Connect adaptive band scheduling

##### 9f.1. Adapt the existing scheduler input/output contract to native output jobs

- Feed measured completed-band elapsed time to the existing 64–1024 row policy without changing its calculation.

Gate: deterministic scheduler fixtures return the existing next-band sizes.

##### 9f.2. Report one completed-row progress event per completed native band

- Publish progress only after the corresponding GPU fence and band download complete.

Gate: progress values and phase ordering match existing orchestration tests.

##### 9f.3. Check cancellation between completed bands and destroy the output job

- Do not submit a subsequent band after cancellation; release its bounded resources and preserve staged output cleanup.

Gate: elapsed-time feedback and completed-row progress match existing behavior; cancellation
between bands destroys the output job.

### 10. Port exposure analysis

#### 10a. Generate native-precision exposure proxies

##### 10a.1. Define retained proxy dimensions and byte layouts

- Calculate quarter-resolution dimensions, per-frame offsets, and bounded resident storage from the existing proxy policy.

Gate: odd/even source dimensions produce the expected proxy layouts without allocation.

##### 10a.2. Downsample one uint8 source with the existing area footprint

- Generate one native-precision uint8 proxy without luminance decode or pair projection.

Gate: deterministic odd/even uint8 proxy pixels match CPU tolerance.

##### 10a.3. Extend proxy generation to uint16 and float32 sources

- Reuse the footprint and edge semantics with native uint16 and float32 values.

Gate: uint16 and float32 proxy pixels, including permitted non-finite source values, match CPU tolerance.

##### 10a.4. Retain completed proxies with the session

- Associate proxy resources with one finished session and release them with session destruction.

Gate: odd/even dimensions and all source encodings match CPU proxy pixels within current tolerance.

#### 10b. Project one exposure pair

##### 10b.1. Define one-pair exposure-grid projection contract

- Validate two distinct finished frame indices, grid geometry, and caller-owned paired-coordinate/overlap layouts.

Gate: malformed and unready pair requests fail before dispatch.

##### 10b.2. Project pair coordinates and geometric overlap

- Generate source coordinates and overlap only, without proxy sampling or photometric classification.

Gate: shared, disjoint, seam, pole, and boundary coordinates/overlap match CPU.

##### 10b.3. Sample retained proxy pairs

- Read the two proxy values at the established coordinates and retain geometric overlap separately.

Gate: shared, disjoint, seam, pole, and boundary fixtures match CPU sample coordinates and overlap.

#### 10c. Classify exposure samples

##### 10c.1. Classify finite values and luminance

- Produce finite/luminance categories without SDR clipping or gradient filtering.

Gate: non-finite and low/high luminance fixtures match CPU categories.

##### 10c.2. Add SDR clipping classification

- Apply existing SDR clipping rules while preserving independent geometric overlap state.

Gate: clipped SDR fixtures match CPU categories.

##### 10c.3. Add linear-HDR pair categories

- Apply established HDR handling without changing SDR behavior.

Gate: clipped SDR, non-finite, low/high luminance, and linear HDR fixtures match CPU categories.

#### 10d. Add gradient filtering

##### 10d.1. Compute per-proxy gradients

- Generate deterministic gradients from already classified proxy samples.

Gate: flat, textured, and edge gradient values match CPU.

##### 10d.2. Apply pair-quality acceptance filtering

- Combine categories and gradients into the existing accepted mask.

Gate: deterministic flat, textured, and edge fixtures match CPU accepted masks and gradients.

#### 10e. Add trimming and robust pair reduction

##### 10e.1. Build one-pair valid log-ratio scratch

- Keep classified accepted samples and log-ratios device-resident.

Gate: accepted samples and ratios match CPU before trimming.

##### 10e.2. Apply the exact lower/upper trimming rule

- This requires a new device ordering primitive, so implement it as the following narrow units:

###### 10e.2.1. Initialize a padded sortable pair scratch

- Allocate session-owned power-of-two float scratch and initialize accepted ratios in it, using
  positive infinity for rejected and padded entries. Do not download samples.

Gate: deterministic accepted/rejected fixtures produce the CUDA-compatible sortable sequence.

###### 10e.2.2. Order one padded pair scratch on-device

- Apply a deterministic SM 5.1 bitonic ordering pass over the initialized scratch.

Gate: sorted accepted values and trailing sentinels match the CPU ordering oracle.

###### 10e.2.3. Extract interpolated lower and upper bounds

- Select the established 0.1 and 0.9 ranks from device scratch using CUDA-compatible linear
  interpolation and the retained accepted count.

Gate: deterministic odd/even-count bounds match the CPU quantile oracle.

###### 10e.2.4. Apply retained bounds to the accepted mask

- Keep only accepted ratios in the inclusive lower/upper interval without downloading samples.

Gate: deterministic outlier boundaries match CPU retained samples.

##### 10e.3. Reduce one trimmed pair to equation/report scalars

- Download only final reduced scalars, never image-sized pair samples.

Gate: outlier and low-sample fixtures match CPU reduced equations/rejection reasons; diagnostics
show only reduced data downloaded.

#### 10f. Build the graph and solve it in native CPU double precision

##### 10f.1. Enumerate and reduce required overlap edges

###### 10f.1.1. Define retained equation and pair-report storage

- Add versioned scalar equation/report layouts and session-owned storage without dispatching pairs.

Gate: structure/layout validation, empty storage, replacement, and destruction are deterministic.

###### 10f.1.2. Enumerate deterministic frame pairs

- Enumerate each `left < right` pair exactly once with checked pair-count arithmetic.

Gate: zero, one, two, and many-frame fixtures match the CPU upper-triangle order and overflow is rejected.

###### 10f.1.3. Chain one pair without host-sized intermediates

###### 10f.1.3.1. Retain one-pair device scratch

- Plan and allocate session-owned device buffers for every intermediate in one pair reduction.

Gate: checked layout arithmetic matches the retained resources, replacement is deterministic, and
the scratch allocation contains no upload or readback heap.

###### 10f.1.3.2. Chain projection and sampling

- Dispatch projection and both proxy samples into the retained scratch without CPU-sized transfers.

Gate: one-pair coordinates, overlap, and samples match the existing stage oracles through test-only
readback.

###### 10f.1.3.3. Chain classification through ratio construction

- 10f.1.3.3.1: classify the two retained sample buffers into luminance and candidate masks.
- 10f.1.3.3.2: compute gradients, finite-gradient p90, and device-resident limits.
- 10f.1.3.3.3: filter candidates with the device limits and construct log ratios.

Gate: each substep matches its existing stage oracle through test-only readback; production paths
perform no intermediate CPU transfer.

###### 10f.1.3.4. Trim and reduce to a scalar packet

- 10f.1.3.4.1: sort ratios, obtain trim bounds, and produce the retained inlier mask.
- 10f.1.3.4.2: compute the median/MAD reduction and download only its final scalar packet.

Gate: trim intermediates match the existing stage oracles through test-only readback; accepted and
rejected reductions match the existing oracle and transfer diagnostics show only the final scalar
packet downloaded.

###### 10f.1.4. Reduce and retain every requested edge

- Run the proven resident one-pair chain for every enumerated pair and retain accepted equations
  plus scalar rejection reports only.

Gate: pair counts and equations match CPU fixtures.

##### 10f.2. Build the CUDA-equivalent weighted solve graph

- 10f.2.1: retain geometric-overlap counts alongside reduced measured equations.
- 10f.2.2: reproduce CUDA's measured-edge reachability and deterministic weight-1 neutral bridges
  for geometrically overlapping disconnected components.

Gate: single-frame, measured chain, bridgeable components, and geometrically disconnected fixtures
match CUDA adjacency, bridge weights, and edge counts.

##### 10f.3. Solve the established graph in native CPU double precision

- Build CUDA's weighted Laplacian and right-hand side, add the frame-0 numerical anchor, solve in
  native CPU `double`, median-center, clamp log gains to `[-ln(2), ln(2)]`, and select the frame
  nearest the centered median as the report anchor.

Gate: single-frame, chain, shared-scene, bridgeable, geometrically disconnected, clipped SDR, and
HDR log gains, anchor, and edge-count scalars match CUDA tolerances.

#### 10g. Upload gains and retain the exposure report

##### 10g.1. Upload solved gains once to the resident session

- Bind final per-frame gains to future composition without re-uploading source data.

Gate: one composition fixture observes solved gains.

##### 10g.2. Retain the reduced exposure report and reuse it

- Reuse one completed proxy/solve result across preview and final jobs.

##### 10g.3. Invalidate retained exposure inputs precisely

- Invalidate only on manual gain or exposure-input changes.

Gate: preview-to-full performs one proxy build and solve, manual changes invalidate correctly, and
no image-sized exposure data crosses the ABI.

### 11. Port auto contrast and output conversion

#### 11a. Build the covered-pixel histogram

##### 11a.1. Define histogram limits, counter layout, and overflow admission check

- Bind the proven maximum-output population limit to `uint32` counters and reject overflow-risk jobs before dispatch.

Gate: boundary planner fixtures prove no admitted bin can overflow.

##### 11a.2. Clear and accumulate one covered finite linear band

- Exclude uncovered and non-finite pixels using existing luminance conventions.

Gate: empty, flat, sparse, and full deterministic one-band counts match CPU.

##### 11a.3. Accumulate and reuse histogram storage across bands

- Retain the histogram for one output job and clear it exactly once per job.

Gate: empty, flat, sparse, and full deterministic histograms match CPU bin counts in both modes.

#### 11b. Select and apply auto-contrast levels

##### 11b.1. Select levels from a completed histogram

- Implement existing percentiles and flat/empty rules, returning only scalar levels.

Gate: enabled, empty, and flat fixtures match CPU-selected levels.

##### 11b.2. Apply supplied selected levels to one linear band

- Apply shared levels with auto contrast enabled only.

Gate: enabled/disabled linear outputs match CPU behavior.

Gate: enabled, disabled, empty, and flat fixtures match CPU-selected levels and linear output.

#### 11c. Convert linear sRGB output

##### 11c.1. Port linear clamp and sRGB transfer

- Convert deterministic finite linear RGB to normalized SDR without quantization.

Gate: transfer breakpoints and endpoints match CPU tolerance.

##### 11c.2. Quantize converted sRGB to 8-bit

- Preserve established rounding order and no-fast-math behavior.

Gate: PNG-oriented buffers match the CPU oracle within one code value in resident and banded jobs.

#### 11d. Convert PQ/Rec.2020 output to SDR

##### 11d.1. Apply reference-white scaling and tone mapping

- Retain linear intermediate output for deterministic neutral/highlight fixtures.

Gate: scaled/tone-mapped values match CPU tolerance.

##### 11d.2. Convert Rec.2020 primaries to linear sRGB

- Preserve the existing matrix and operation order.

##### 11d.3. Reuse sRGB transfer and 8-bit quantization

- Feed the converted SDR linear result through the proven 11c primitives.

Gate: neutral, saturated-highlight, and PQ fixtures match existing preview/output tolerances.

#### 11e. Add float output conversion

##### 11e.1. Copy finite linear RGB to float output without SDR conversion

- Preserve values above one and existing channel order.

##### 11e.2. Define non-finite float-output behavior

- Match the current EXR policy explicitly before downloads.

Gate: finite and above-one EXR buffers match current float tolerance in both memory modes.

#### 11f. Add caller-owned band downloads

##### 11f.1. Validate caller-owned integer and float band layouts

- Reject wrong dimensions/byte counts before copy submission.

##### 11f.2. Read back one converted integer band

- Check cancellation before submission and after fence completion.

##### 11f.3. Read back one converted float band

- Reuse the same bounded staging lifetime and cancellation semantics.

Gate: integer downloads remain within one code value, float downloads remain within tolerance, and
transfer counts show one readback per completed band and no disk scratch.

### 12. Port retained interactive preview rendering

#### 12a. Retain base preview pixels and compact pose masks

- Create a preview handle owned by the session and retain its base pixels, overview, and compact
  per-pose masks.
- Add lifecycle, byte-count, and repeat-create/destroy diagnostics.

Gate: retained buffers match CPU fixtures and all live counts return to zero after partial and
normal destruction.

#### 12b. Add crop and scale rendering

- Render either the overview or one clamped magnified crop into a fixed viewport.
- Return only that viewport through the Python adapter.

Gate: center, edge, out-of-range, and exact-boundary crop/scale tests match current pixels on WARP.

#### 12c. Add hover overlay

- Add hover tint and outline using compact masks.

Gate: hovered/unhovered and overlay-disabled fixtures match current pixels.

#### 12d. Add target overlay and target mode

- Add target tint and target-selection mode while preserving hover priority.

Gate: hover/target/target-mode combinations match current pixels and click candidates.

#### 12e. Add pose boundaries

- Add boundary composition using compact masks and preserve established overlay priority.

Gate: boundary-only and combined hover/target/boundary fixtures match current pixels.

#### 12f. Add cancellation, coalescing, and latency evidence

- Add generation cancellation for in-flight viewport work.
- Coalesce stale pointer requests exactly as the current display worker does.
- Measure request-to-display latency without changing Tk thread ownership.

Gate: stale generations never publish; close/cancel leaks nothing; physical-hardware median and
95th-percentile latency are recorded.

### 13. Integrate D3D12 behind the existing application

#### 13a. Select D3D12 without routing renders

- Add D3D12/CPU selection and admission behind the neutral selector.
- Keep CUDA callable only by explicit tests; product selection never returns it.
- Preserve strict-GPU and pre-dispatch CPU fallback semantics.

Gate: mocked and real preflight cases select D3D12 or CPU correctly; no render path changes yet.

#### 13b. Route final rendering through D3D12

##### 13b.1. Promote arbitrary-count ordered hard composition

- Generalize the proven two/three-frame output-job hard chain to the session frame count and expose
  one production entry point without changing candidate or strict-tie arithmetic.

Gate: one-, two-, three-, and four-frame resident/banded output jobs match the established hard
fixtures for every source type.

##### 13b.2a. Generalize feather accumulation count

- Generalize the proven feather accumulation chain to arbitrary session frame counts while
  preserving ordered addition and ping-pong resource states.

Gate: one- through four-frame accumulators match the established ordered feather arithmetic.

##### 13b.2b. Retain and normalize feather output

- Write accumulated RGB/weight into the output job, normalize there, and derive coverage without
  downloading image-sized intermediates.

Gate: resident and repeated-band output storage matches the standalone accumulation/normalization
fixtures.

##### 13b.2c. Promote ordered feather composition

- Connect retained source candidate generation and feather weighting to the output-owned chain
  behind one production entry point.

Gate: one- through four-frame resident/banded output jobs match ordered feather fixtures for every
source type.

##### 13b.3. Bind production exposure and incomplete-output inputs

- Apply retained/manual global gains, established local fields, and incomplete-magenta behavior in
  both production blend modes.

Gate: identity/manual/automatic/local gains and complete/incomplete fixtures match current output.

##### 13b.4. Bind native session creation and uploads in ctypes

- Create the device/session, upload rotations and native source frames through two bounded slots,
  finish uploads, and own cancellation/destruction transactionally.

Gate: mock and WARP fixtures prove exact declarations, upload order/bytes, cancellation, and cleanup.

##### 13b.5. Bind retained exposure analysis in ctypes

- Build proxies, reduce the pair graph, solve/upload gains, and expose the scalar report without
  downloading image-sized intermediates.

Gate: single- and multi-frame reports match current tolerances and reuse one solved upload.

##### 13b.6. Bind output jobs, conversion, and downloads in ctypes

- Create resident/banded output jobs, dispatch one completed band, apply the required conversion,
  and download through caller-owned arrays.

Gate: hard/feather, SDR/PQ/float, auto-contrast, and cancellation fixtures match native contracts.

##### 13b.7. Route resident final renders through D3D12

- Use the native adapter for final renders while keeping existing writers, staged publication,
  history, and public options unchanged.

Gate: backend-neutral resident final-output tests pass on WARP for PNG/JPEG/EXR.

##### 13b.8. Route banded and auto-contrast final renders through D3D12

- Run the existing adaptive scheduler, repeated histogram pass when required, completed-band
  downloads, progress, and cancellation cleanup.

Gate: resident/banded parity, progress, transfer counts, and cancellation tests pass on WARP.

##### 13b.9. Enforce the fallback boundary

- Allow CPU fallback only for D3D12 failure before the first numerical dispatch; later failures
  clean up and propagate without restarting on CPU.

Gate: preflight falls back to CPU, strict mode fails, and injected post-dispatch failures never
invoke CPU rendering.

#### 13c. Route preview creation through D3D12

- Route base preview creation and retained preview ownership through the native adapter.
- Keep Tk image creation and event-queue delivery unchanged.

Gate: non-interactive preview pixels and cleanup tests pass on WARP and CPU fallback.

#### 13d. Route interactive preview and cache reuse

- Connect crop/overlay viewport requests and generation cancellation.
- Connect device-aware retained-session cache reuse from preview to final output.

Gate: interaction, coalescing, cache-hit/miss, one-upload, and one-exposure-solve tests pass.

#### 13e. Finish product-facing backend cutover

- Rename progress phases, logs, status text, and internal callback values from CUDA to GPU/D3D12.
- Keep `Use GPU acceleration`, `--gpu`, and `--no-gpu` behavior unchanged.
- Run CPU-fallback and hardware-vendor acceptance without removing CUDA test-oracle code yet.

Gate: the full neutral suite passes on WARP and NVIDIA/AMD/Intel; a no-hardware configuration starts
and renders on CPU; no product path selects CUDA.

### 13.5. Package a manually testable D3D12 Python GUI

- Build and run the Windows x64 native Release/WARP gate from the release workflow.
- Add a transitional `gpu` PyInstaller archive containing `pano_gpu.dll` beside the frozen
  `pano_stitch.d3d12_adapter` module, while keeping the CPU archive free of the native DLL.
- Run a non-GUI frozen probe that loads the DLL and verifies the synchronized ABI before archiving.
- Make the packaged GUI select the D3D12 product backend by default through the existing `Use GPU
  acceleration` control; keep CUDA reachable only through explicit test-oracle injection until Step
  15 removes it.

Gate: the GPU archive builds, its frozen ABI probe passes, the CPU archive contains no
`pano_gpu.dll`, and the packaged GUI launches and reaches preview with D3D12 selected. Final-output
rendering remains a useful manual smoke test but is not required to pass this early usability gate.

### 14. Harden failures and lifetime behavior

#### 14a. Harden construction and allocation failures

- **14a.1 — Charge the native composite peak before dispatch. (complete)** Include per-frame candidate,
  validity, edge/feather, accumulator, zero-initialization, output, and committed-resource alignment
  costs in final-render admission. Prove the 30-frame `1787897185-2` geometry selects a bounded band
  instead of reaching `cannot create D3D12 hard-composite frame buffers` after dispatch.
- **14a.2 — Verify allocation fallback boundaries. (complete)** Inject the same frame-buffer allocation failure
  before and after the first numerical dispatch; permit CPU fallback only in the former case and
  propagate the latter without double-rendering.
- **14a.3 — Complete construction failure injection. (complete)** Inject failures at device, pipeline,
  descriptor, resource, session, output, and preview creation one boundary at a time.
- **14a.4 — Verify reverse cleanup. (complete)** Check reverse-order cleanup and zero live native counts after
  every successfully completed construction boundary.

Gate: every construction case returns the expected error and zero live native counts.

#### 14b. Harden upload, dispatch, fence, and readback failures

- **14b.1 — Verify upload and upload-fence failures. (complete)** Cover upload allocation,
  metadata-upload rejection, slot reuse, fence signal, and finish-wait failures without corrupting
  completed frames. (`ExecuteCommandLists` has no return value; the following fence signal is the
  first reportable queue-submission failure.)
- **14b.2 — Inject numerical dispatch failures. (complete)** Fail command submission immediately before and
  after the first numerical dispatch and preserve an actionable phase error.
- **14b.3 — Inject readback failures. (complete)** Cover readback allocation, submission, fence wait, and map
  failures without publishing partial output.
- **14b.4 — Verify device removal and fallback boundaries. (complete)** Preserve the device-removal HRESULT in
  native errors and enforce CPU fallback only before the first numerical dispatch.

Gate: the failure matrix reports actionable phase/device-removal details, never double-renders, and
leaks no command or resource object.

#### 14c. Harden cancellation, timeout, and application shutdown

- **14c.1 — Verify cancellation checkpoints. (complete)** Cover cancellation before upload, after upload-slot
  wait, after upload-finish wait, before download, after download-fence wait, and before each next
  output band without publishing stale buffers.
- **14c.2 — Inject bounded fence timeouts. (complete)** Deterministically fail a native fence wait without a
  ten-second test delay, report the waiting phase, and leave the owning session/output reusable.
- **14c.3 — Verify abandoned and repeated preview work. (complete)** Reject stale generations, serialize
  concurrent preview calls, and prove repeated jobs/close do not publish abandoned pixels.
- **14c.4 — Verify active-work shutdown and parent lifetime. (complete)** Request cancellation when the GUI is
  closed with a live worker, finish closing exactly once after it exits, and destroy output/preview
  children before session and device parents.

Gate: cancellation latency is bounded, stale work never publishes, and repeated close is harmless.

#### 14d. Verify staged-file and cache cleanup

- **14d.1 — Connect staged D3D12 coverage output. (complete)** Download each completed native coverage band to
  its existing staged encoder path and publish it only with the successful final output.
- **14d.2 — Connect the D3D12 session thumbnail. (complete)** Render the existing rectilinear session thumbnail
  through the retained prepared session, preserving its projection, exposure, conversion, and
  filename contracts. The manually observed `D3D12 thumbnail and coverage routing is not yet
  connected` fallback becomes a regression failure.
- **14d.3 — Harden multi-output publication. (complete)** Inject final, coverage, and thumbnail failures through
  existing encoders, atomic publication, and cache invalidation; preserve every existing output and
  remove every staged file on failure.
- **14d.4 — Verify retained-session cleanup. (complete)** Exercise success, cancellation, and failure with
  final output plus coverage/thumbnail and prove child-before-parent cache cleanup.

Gate: no `.partial`, scratch directory, retained cache, or changed existing output remains after
the complete matrix.

### 15. Cut over tests and remove CUDA completely

#### 15a. Rename behavioral tests and remove compatibility aliases (complete)

- Rename remaining CUDA behavioral fixtures/tests to neutral GPU names without changing assertions.
- Remove the temporary `Cuda*` aliases and legacy backend literal from orchestration contracts.

Gate: the same behavioral test count and assertions pass through D3D12/CPU; a focused search finds
no legacy name in product orchestration.

#### 15b. Replace implementation-specific tests (complete)

- Replace CUDA kernel-source assertions with embedded-HLSL entry-point/hash assertions.
- Replace CuPy driver/pool tests with adapter-error and live native handle/allocation tests.
- Replace CUDA archive tests with one cross-vendor archive contract.

Gate: every deleted implementation assertion has a named D3D12 replacement; no behavioral test is
deleted or weakened.

#### 15c. Remove CUDA dependencies and build hooks (complete)

- Remove CuPy/CUDA packages and lock entries, probes, PyInstaller collection, archive flavoring,
  CI inputs, and environment switches.
- Regenerate lock/build metadata using the normal project tooling.

Gate: clean CPU-only Python installation and Windows D3D12 build both pass; packaging resolves no
CUDA or vendor-compute package.

#### 15d. Remove CUDA source and current-support documentation (complete)

- Delete `cuda_kernels.py` and remaining CuPy-specific implementation code.
- Remove current user-facing CUDA install/archive claims while retaining clearly historical notes.
- Do not leave a dormant backend or compatibility archive.

Gate: `rg -i 'cuda|cupy|nvrtc|cudart'` finds only historical documents or explicit migration
history; the full D3D12/CPU suite passes.

### 16. Produce the single transitional release archive

#### 16a. Add the single archive build (complete)

- Build `PanoramaCapture-Stitcher-<version>-win-x64.zip` with the transitional Python application,
  native DLL, and embedded shader bytecode.
- Remove CPU/CUDA flavor branching from filenames and release scripts.

Gate: the archive builds reproducibly and starts from an extracted path with spaces.

#### 16b. Add runtime verification CLI behavior (complete)

- Wire `--verify-gpu-runtime` to ABI validation, product adapter probe, pipeline creation, tiny
  dispatch, readback verification, and cleanup without opening session images.
- Return stable success/unavailable/failure exit categories and actionable diagnostics.

Gate: hardware, no-adapter, corrupted/mismatched DLL, and injected-dispatch cases behave as tested.

#### 16c. Audit the release artifact (complete)

- Inspect archive contents and PE dependencies for vendor runtime, shader compiler, CUDA, and
  unembedded shader files.
- Record compressed/extracted size, hashes, shader hashes, DLL dependencies, and adapter details.

Gate: the artifact audit is automated and passes on the candidate archive.

#### 16d. Run available physical-hardware acceptance (complete)

- Test the exact candidate archive on the available Windows 11/NVIDIA system with a known session,
  including preview interaction, cancellation, resident/banded modes, output routing, and cleanup.
- Retain automated coverage for CPU fallback and unavailable-adapter behavior.

Gate: recorded outputs/tolerances and diagnostics pass on the available Windows 11/NVIDIA target;
the archive needs no compute SDK or vendor package beyond its graphics driver. This is the D3D12
migration completion gate. Broader hardware and input-format validation is tracked separately and
does not block the migration or native-application work.

#### Deferred cross-device acceptance checklist (non-blocking)

Complete these checks when suitable hardware and sessions become available. Failures still require
investigation before claiming support for the affected target, but unchecked items do not reopen
Step 16 or block later migration increments.

- [ ] Run the exact candidate on Windows 10.
- [ ] Run the physical-hardware suite on a supported AMD adapter.
- [ ] Run the physical-hardware suite on Intel Arc or a supported integrated adapter.
- [ ] Test startup and CPU rendering on a clean machine with no compatible hardware adapter.
- [ ] Test known JPEG-input, 16-bit Rec.2020/PQ PNG-input, and float EXR-input sessions.
- [ ] Inspect GUI preview interaction, cancellation, output publication, and cleanup on each target.

## Native application migration

The D3D12 backend removes image computation from Python, but Tk still requires full viewport
readback, PIL/ImageTk object creation, and event-queue delivery for interactive preview updates.
A native application is therefore beneficial for preview latency and can ultimately remove the
bundled Python, Tk, NumPy, OpenCV, Pillow, and PyInstaller payload. It is not allowed to delay or
destabilize the D3D12 cutover.

Use the following continuation only after increment 16 is accepted. Keep the transitional Python
release usable until the final native gate.

### 17. Freeze application-level contracts

#### 17a. Freeze metadata and session-management behavior

- Add fixtures for metadata validation, path inference, discovery/history, and moved/incomplete
  sessions.

Gate: the existing Python loaders pass valid/invalid fixture expectations unchanged.

#### 17b. Freeze render-option and output behavior

- Add fixtures for defaults, output naming, overwrite decisions, exposure edits, settings, CLI help,
  and representative semantic error categories.

Gate: CLI and application helpers pass without byte-for-byte message coupling.

#### 17c. Extract a headless application-core interface

- Move one state transition at a time behind a Tk-independent interface, starting with validation,
  then planning, then render/cancel/history updates.
- Keep Tk as the only caller until each transition has contract coverage.

Gate: Tk tests and new headless tests pass after every moved transition; no widget dependency enters
the application core.

### 18. Add a native command-line host

#### 18a. Add an executable with version/help only

- Build a native executable over the core with stable exit-code categories and no render command.

Gate: CTest covers help, version, unknown options, Unicode paths, and clean startup/shutdown.

#### 18b. Port render-option parsing

- Add one option group at a time and translate it into a plain native render-plan request.

Gate: defaults, conflicts, ranges, and semantic errors match the frozen Python fixtures.

#### 18c. Port session JSON parsing and validation

- Parse schema fields, then paths, then frames/encodings while preserving the current schema and
  compatibility decisions.

Gate: every valid/invalid metadata fixture matches Python validation results and inferred paths.

#### 18d. Produce native validation and render plans

- Connect parsed sessions/options to the native core without decoding or encoding images yet.

Gate: native and Python validation/render plans match fixture-by-fixture. The executable is still
not a shipped renderer.

### 19. Port codecs and staged output ownership

#### 19a. Evaluate and select codec dependencies

- Test candidate libraries against tiny committed JPEG, 8/16-bit PNG, PQ metadata, and float EXR
  fixtures before adding a production dependency.
- Record license, archive-size, color-metadata, streaming, and Windows-build results.

Gate: one explicit dependency decision covers every required format; no codec code is integrated
before this gate.

#### 19b. Port JPEG and 8-bit PNG source decode

- Decode one SDR source at a time into the two-slot upload pipeline.
- Preserve JPEG and PNG color metadata and native 8-bit samples.

Gate: JPEG and 8-bit PNG fixtures match frozen samples, metadata, bounded memory, and cancellation.

#### 19c. Port 16-bit PQ PNG source decode

- Preserve native `uint16` samples plus Rec.2020/PQ/full-range metadata through upload.

Gate: PQ PNG fixtures match frozen samples/metadata and never expand to float on the host.

#### 19d. Port float EXR source decode

- Stream native float scanlines into the upload slots and preserve linear Rec.2020 metadata.

Gate: finite/above-one EXR fixtures match frozen samples and bounded-memory behavior.

#### 19e. Port PNG output

- Add streaming 8-bit SDR PNG output and required metadata through caller-owned bands.

Gate: PNG samples, metadata, options, cancellation, and encode failures match fixtures.

#### 19f. Port JPEG output

- Add JPEG 4:4:4 quality behavior through caller-owned bands.

Gate: JPEG tolerances, subsampling, quality, cancellation, and encode failures match fixtures.

#### 19g. Port EXR output

- Add streaming float scanline EXR with the current compression, color metadata, and above-one
  behavior.

Gate: EXR round-trip and bounded-memory tests match current tolerances.

#### 19h. Own staged publication and cleanup natively

- Write same-directory staged files, flush/close, and atomically publish only on complete success.
- Add narrowly scoped stale-stage recovery consistent with current behavior.

Gate: every cancel/failure boundary preserves existing output and leaves no partial artifact.

### 20. Port the CPU fallback

#### 20a. Add CPU planning and strip ownership

- Implement the same memory admission, strip sizing, worker limit, scratch ownership, and
  cancellation contract without image math.

Gate: plans and peak allocations match frozen Python cases; WARP is never selected as fallback.

#### 20b. Port one-frame CPU projection and hard blend

- Port the already-frozen projection/sampling primitives for one strip and one frame.

Gate: deterministic hard-blend fixtures match Python pixels and coverage.

#### 20c. Add multi-frame hard composition

- Loop frames in capture order and preserve strict hard-blend ties.

Gate: multi-frame hard fixtures match the shared CPU/GPU oracle.

#### 20d. Add CPU feather composition

- Add feather weights, accumulation, normalization, coverage, and incomplete magenta.

Gate: feather and incomplete-coverage fixtures match the shared oracle with bounded strips.

#### 20e. Add supplied exposure gains

- Apply global and local supplied gains without exposure analysis.

Gate: identity/global/local-gain fixtures match the shared oracle.

#### 20f. Add CPU exposure proxies and pair sampling

- Port area-filter proxy generation and geometric pair sampling.

Gate: proxy pixels and sampled overlap fixtures match the frozen Python oracle.

#### 20g. Add CPU exposure classification and reduction

- Port photometric classification, gradients, trimming, and robust pair reduction.

Gate: reduced equations and rejection reasons match frozen fixtures.

#### 20h. Add CPU exposure solve and caching

- Solve the anchored graph in native `double`, apply manual inputs, and retain the report/gains.

Gate: exposure reports match current tolerances and preview-to-full solves once.

#### 20i. Add conversion and streaming output bands

- Add auto contrast, SDR/PQ conversion, quantization, and float bands in the same order already
  proven for D3D12.

Gate: PNG/JPEG/EXR output tolerances and bounded memory match Python.

#### 20j. Complete CPU failure and concurrency behavior

- Add worker-pool limits, cancellation at every phase, staged cleanup, and repeated renders.

Gate: the native CPU suite passes on a clean system without a compatible GPU or graphics SDK.

### 21. Add the native GUI shell

- Use Win32 common controls for the form, dialogs, DPI/accessibility behavior, and keyboard
  navigation. Avoid a redistributable UI runtime.
- Present the retained preview directly through a D3D12 swap chain in a child window, so pointer
  crop/overlay changes stay on the GPU and do not round-trip through host images.
- Drive validation/render work from background threads and marshal only state/progress to the UI
  thread.

#### 21a. Add the empty native shell

- Add Win32 common controls, DPI/accessibility behavior, keyboard navigation, and close handling.

Gate: native-DPI, screen-reader labels, tab order, keyboard activation, and repeated close pass.

#### 21b. Add session selection

- Add discovery, selection, refresh, and path editing without render options.

Gate: the frozen discovery/path contracts and native file-dialog smoke tests pass.

#### 21c. Add options and validation

- Add render options, background validation, output naming, and overwrite confirmation.

Gate: option/validation contracts pass and only state/progress crosses to the UI thread.

#### 21d. Add the base preview swap chain

- Present the retained preview through a child-window D3D12 swap chain.
- Handle resize, occlusion, and device loss without interactive overlays.

Gate: base preview pixels, resize, close, and device-loss recovery pass.

#### 21e. Add preview crop and selection interaction

- Add crop/scale, hover, target selection, and pose boundaries using the proven retained renderer.

Gate: pointer/crop/overlay contracts pass; 95th-percentile latency is below one 60 Hz frame or the
measured limitation is documented.

#### 21f. Add exposure interaction

- Add target exposure, automatic correction, manual matching, discard, and warning states.

Gate: frozen exposure-edit contracts and cancellation/close during recompute pass.

#### 21g. Add full render lifecycle

- Add background full render, progress, cancellation, errors, and shutdown during work.

Gate: render/cancel/failure contracts pass and the UI thread never performs image work.

#### 21h. Add history, settings, and deletion

- Add history updates, settings persistence, and deletion confirmations/actions.

Gate: persistence and deletion contracts plus manual keyboard/accessibility checks pass.

### 22. Switch the release and remove Python

#### 22a. Run dual-frontend parity

- Run Python and native frontends against the same fixture corpus, CPU/GPU matrix, failures, and
  accessibility checklist.

Gate: feature, output, error-category, cleanup, and interaction parity are recorded with no open
release-blocking difference.

#### 22b. Switch the archive entry point

- Make the native executable the candidate entry point while retaining the Python frontend in a
  non-default comparison archive for one acceptance cycle.

Gate: clean-machine install/upgrade, file association, settings migration, and rollback tests pass.

#### 22c. Remove Python runtime and build inputs

- Remove Python/Tk first, then NumPy/OpenCV/Pillow/OpenEXR bindings, then PyInstaller and lock/build
  configuration, verifying the archive after each dependency group.
- Retain cheap fixture generators/specifications only when they are not shipped.

Gate: the native archive builds and passes tests after each removal; no shipped file imports or
collects the removed runtime.

#### 22d. Accept the native-only release

- Repeat the clean-machine, hardware, CPU-fallback, format, failure, and accessibility matrix on
  the exact signed archive.
- Record size and dependency differences from the transitional archive.

Gate: the archive is materially smaller, contains no Python or CUDA payload, and passes the full
matrix. Only then retire the transitional release.

## Verification matrix

Every implementation increment runs the checks relevant to files it touches and reports only
checks actually run.

### Platform-independent

From `stitcher/`:

```text
uv run ruff check .
uv run ruff format --check .
uv run mypy src
uv run pytest --ignore=tests/test_gpu_runtime.py
```

Use `.venv/bin/<tool>` when `uv` cannot access its cache.

### Portable native Release

From `stitcher/` on a non-Windows development host:

```text
cmake -S native -B build/native-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-release
ctest --test-dir build/native-release --output-on-failure
```

This is a required gate for native changes, not a substitute for Windows WARP. Native contract
tests must execute their checks under `NDEBUG`; a Release test binary that compiles but evaluates
no checks is a failure.

### Windows WARP

```text
cmake -S stitcher/native -B build/stitcher-native -A x64
cmake --build build/stitcher-native --config Release
ctest --test-dir build/stitcher-native -C Release --output-on-failure
uv run pytest -m windows_warp
```

### Physical hardware acceptance

Physical adapter enumeration, driver access, native builds, and hardware runtime tests may require
sandbox elevation. If an in-sandbox probe or test fails because the adapter, driver, Windows build
tools, or required runtime resources are inaccessible, rerun the exact scoped command with
elevation before diagnosing a D3D12 failure. Record whether elevation was required; never treat a
sandbox access failure as evidence that the adapter is unsupported.

The Windows 11/NVIDIA run required by Step 16 is recorded in
`docs/d3d12-stitcher-acceptance.md`. When the remaining systems become available, run identical
known sessions on the unchecked targets in the deferred checklist:

- one supported NVIDIA adapter;
- one supported AMD adapter;
- one Intel Arc or supported integrated adapter;
- one CPU-fallback-only configuration.

For each, record Windows version, adapter, driver, dedicated/budget/used VRAM, selected memory
mode, archive hash and size, output hashes/tolerances, phase timings, preview latency, cancellation
results, and live allocation counts after shutdown. Test PNG, JPEG, PQ/Rec.2020 input, EXR output,
resident and forced-banded modes, preview overlays, preview-to-full cache reuse, and device/failure
cleanup.

## Completion criteria

The D3D12 migration is complete at increment 16 when one transitional archive passes the automated
gates, accelerates the full existing GPU contract on the available Windows 11/NVIDIA system,
safely falls back to CPU under the tested admission contracts, and contains no CUDA implementation
or dependencies. The explicitly deferred cross-device checklist expands confidence when hardware
becomes available but is not part of this completion gate. The application migration is separately
complete at increment 22 when the native executable replaces Python without losing any tested
behavior.

WARP and Linux tests alone are not physical-hardware acceptance; the recorded Windows 11/NVIDIA
run supplies the physical validation for the current milestone.
