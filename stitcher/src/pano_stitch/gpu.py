"""Optional CUDA capability probing and resident working-set admission."""

from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
from typing import Any, Literal

import numpy as np

from pano_stitch.cuda_kernels import CUDA_KERNEL_NAMES, CUDA_MODULE_SOURCE

MiB = 1024 * 1024
GPU_RESERVE_BYTES = 384 * MiB
GPU_RESERVE_FRACTION = 0.15
GPU_OVERHEAD_BYTES = 64 * MiB
_MINIMUM_BAND_ROWS = 32
_LOCAL_EXPOSURE_SCALE = 4
_HISTOGRAM_BYTES = 4096 * 8
_WATCHDOG_TARGET_SECONDS = 0.25
_WATCHDOG_INITIAL_ROWS = 256
_WATCHDOG_MINIMUM_ROWS = 64
_WATCHDOG_MAXIMUM_ROWS = 1024


@dataclass(frozen=True)
class GpuDeviceInfo:
    """CUDA device capacity reported at probe time."""

    name: str
    total_bytes: int
    free_bytes: int
    compute_capability: tuple[int, int] | None = None


@dataclass(frozen=True)
class BackendSelection:
    """The render backend selected before any source pixels are uploaded."""

    backend: Literal["cpu", "cuda"]
    device_name: str | None
    memory_mode: Literal["cpu", "resident", "banded"]
    required_bytes: int | None
    available_bytes: int | None
    reason: str


@dataclass(frozen=True)
class CudaMemoryPlan:
    """Exact CUDA allocation budget for one output job."""

    source_bytes: int
    session_workspace_bytes: int
    output_workspace_bytes: int
    host_output_bytes: int
    reserve_bytes: int
    output_band_rows: int | None
    required_bytes: int
    available_bytes: int


@dataclass
class CudaBandScheduler:
    """Adapt CUDA output bands to stay below the Windows watchdog target."""

    workspace_rows: int
    target_seconds: float = _WATCHDOG_TARGET_SECONDS

    def __post_init__(self) -> None:
        if self.workspace_rows < 1:
            raise ValueError("CUDA band workspace must contain at least one row")
        if self.target_seconds <= 0.0:
            raise ValueError("CUDA band target time must be positive")
        self._minimum_rows = min(_WATCHDOG_MINIMUM_ROWS, self.workspace_rows)
        self._maximum_rows = min(_WATCHDOG_MAXIMUM_ROWS, self.workspace_rows)
        self._rows = min(_WATCHDOG_INITIAL_ROWS, self._maximum_rows)

    @property
    def rows(self) -> int:
        return self._rows

    def next_rows(self, remaining_rows: int) -> int:
        """Return the next band size without exceeding the output workspace."""

        if remaining_rows < 1:
            raise ValueError("CUDA band scheduler needs remaining output rows")
        return min(self._rows, remaining_rows)

    def record_elapsed(self, seconds: float) -> None:
        """Use measured CUDA event time to size the following band conservatively."""

        if seconds < 0.0:
            raise ValueError("CUDA band elapsed time cannot be negative")
        if seconds == 0.0:
            scale = 2.0
        else:
            scale = max(0.5, min(2.0, self.target_seconds / seconds))
        self._rows = max(
            self._minimum_rows,
            min(self._maximum_rows, round(self._rows * scale)),
        )


@dataclass(frozen=True)
class CudaTransferStats:
    """Transfers and CUDA synchronization performed by one render."""

    source_uploads: int = 0
    host_to_device_bytes: int = 0
    device_to_host_bytes: int = 0
    kernel_launches: int = 0
    synchronizations: int = 0
    peak_device_bytes: int = 0
    disk_scratch_bytes: int = 0


@dataclass(frozen=True)
class CudaRenderDiagnostics:
    """Observable CUDA render counters and wall-clock phase timings."""

    transfer_stats: CudaTransferStats
    phase_seconds: tuple[tuple[str, float], ...]


@dataclass(frozen=True)
class CudaExposureResult:
    """Device-resident exposure gains with final scalar report values."""

    log_gains: Any
    anchor_index: Any
    edge_count: Any


class GpuUnavailableError(RuntimeError):
    """Raised when CUDA cannot be used by an explicitly requested probe."""


class CudaPreflightError(GpuUnavailableError):
    """CUDA setup failed before the first numerical kernel can start."""


class CudaKernelModule:
    """Eagerly compiled CUDA module shared by one session's device owners."""

    def __init__(self, cp: Any) -> None:
        try:
            module = cp.RawModule(
                code=CUDA_MODULE_SOURCE,
                options=("--std=c++11",),
                name_expressions=CUDA_KERNEL_NAMES,
            )
            module.compile()
            self._kernels = {name: module.get_function(name) for name in CUDA_KERNEL_NAMES}
        except Exception as exc:
            raise GpuUnavailableError(f"CUDA kernel compilation failed: {exc}") from exc

    def __getitem__(self, name: str) -> Any:
        return self._kernels[name]


def compile_cuda_module() -> None:
    """Eagerly compile every CUDA entry point before source decode or allocation."""

    try:
        cp: Any = import_module("cupy")
    except ImportError as exc:
        raise GpuUnavailableError("CuPy is not installed") from exc
    CudaKernelModule(cp)


def _cuda_sample_dtype(cp: Any, sample_type: str) -> Any:
    dtypes = {"uint8": cp.uint8, "uint16": cp.uint16, "float32": cp.float32}
    try:
        return dtypes[sample_type]
    except KeyError as exc:
        raise ValueError(f"unsupported CUDA source sample type: {sample_type}") from exc


class CudaSession:
    """Transactional owner for one CUDA device session and resident source pixels."""

    def __init__(
        self,
        *,
        frame_count: int,
        source_width: int,
        source_height: int,
        sample_type: str,
        rotations: np.ndarray[Any, Any],
        plan: CudaMemoryPlan,
    ) -> None:
        if min(frame_count, source_width, source_height) < 1:
            raise ValueError("CUDA session dimensions must be positive")
        try:
            cp: Any = import_module("cupy")
        except ImportError as exc:
            raise GpuUnavailableError("CuPy is not installed") from exc

        self._cp = cp
        self._closed = False
        self._source_uploads = 0
        self._host_to_device_bytes = 0
        self._device_to_host_bytes = 0
        self._kernel_launches = 0
        self._synchronizations = 0
        self._plan = plan
        self.sample_type = sample_type
        self.sources: Any = None
        self.rotations: Any = None
        self.log_gains: Any = None
        self.exposure_proxies: Any = None
        self.upload_stream: Any = None
        self.compute_stream: Any = None
        self._upload_events: tuple[Any, Any] | None = None
        self._pinned_slots: tuple[np.ndarray[Any, Any], np.ndarray[Any, Any]] | None = None
        try:
            self.module = CudaKernelModule(cp)
            self.upload_stream = cp.cuda.Stream(non_blocking=True)
            self.compute_stream = cp.cuda.Stream(non_blocking=True)
            self._upload_events = (cp.cuda.Event(), cp.cuda.Event())
            dtype = _cuda_sample_dtype(cp, sample_type)
            self.sources = cp.empty((frame_count, source_height, source_width, 3), dtype=dtype)
            self.rotations = cp.asarray(rotations, dtype=cp.float32).reshape((frame_count, 9))
            source_slot_bytes = source_width * source_height * 3 * np.dtype(sample_type).itemsize
            self._pinned_slots = self._allocate_pinned_slots(source_slot_bytes, sample_type)
            self._refresh_peak_bytes()
        except Exception as exc:
            self.close()
            if isinstance(exc, ValueError):
                raise
            if isinstance(exc, GpuUnavailableError):
                raise CudaPreflightError(str(exc)) from exc
            raise CudaPreflightError(f"CUDA session initialization failed: {exc}") from exc

    def __enter__(self) -> CudaSession:
        return self

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        self.close()

    @property
    def transfer_stats(self) -> CudaTransferStats:
        return CudaTransferStats(
            source_uploads=self._source_uploads,
            host_to_device_bytes=self._host_to_device_bytes,
            device_to_host_bytes=self._device_to_host_bytes,
            kernel_launches=self._kernel_launches,
            synchronizations=self._synchronizations,
            peak_device_bytes=self._peak_device_bytes,
        )

    def _allocate_pinned_slots(
        self, byte_count: int, sample_type: str
    ) -> tuple[np.ndarray[Any, Any], np.ndarray[Any, Any]]:
        dtype = np.dtype(sample_type)
        shape = (byte_count // dtype.itemsize,)
        slots = []
        for _ in range(2):
            memory = self._cp.cuda.alloc_pinned_memory(byte_count)
            slots.append(np.frombuffer(memory, dtype=dtype, count=shape[0]))
        return slots[0], slots[1]

    def pinned_slot(self, slot_index: int) -> np.ndarray[Any, Any]:
        """Return one reusable pinned source slot after its upload event completed."""

        if self._pinned_slots is None:
            raise RuntimeError("CUDA session is closed")
        events = self._upload_events
        if events is None:
            raise RuntimeError("CUDA session upload events are unavailable")
        if slot_index not in {0, 1}:
            raise ValueError("pinned source slot must be 0 or 1")
        events[slot_index].synchronize()
        self._synchronizations += 1
        return self._pinned_slots[slot_index]

    def pinned_array(self, shape: tuple[int, ...], dtype: np.dtype[Any]) -> np.ndarray[Any, Any]:
        """Allocate a final host result in pinned memory for asynchronous CUDA copies."""

        if self._closed:
            raise RuntimeError("CUDA session is closed")
        if not shape or any(dimension < 1 for dimension in shape):
            raise ValueError("pinned CUDA output shape must be positive")
        byte_count = int(np.prod(shape, dtype=np.int64)) * dtype.itemsize
        memory = self._cp.cuda.alloc_pinned_memory(byte_count)
        return np.frombuffer(memory, dtype=dtype, count=byte_count // dtype.itemsize).reshape(shape)

    def upload_source(
        self, frame_index: int, source: np.ndarray[Any, Any], pinned_slot_index: int = 0
    ) -> None:
        """Queue exactly one asynchronous native-pixel upload for a source frame."""

        if self._closed:
            raise RuntimeError("CUDA session is closed")
        events = self._upload_events
        if events is None:
            raise RuntimeError("CUDA session upload events are unavailable")
        if pinned_slot_index not in {0, 1}:
            raise ValueError("pinned source slot must be 0 or 1")
        if not 0 <= frame_index < int(self.sources.shape[0]):
            raise ValueError("source frame index is outside the resident source tensor")
        expected = tuple(int(value) for value in self.sources.shape[1:])
        if source.shape != expected:
            raise ValueError(f"source shape {source.shape} does not match CUDA session {expected}")
        if source.nbytes != self.sources[frame_index].nbytes:
            raise ValueError("source byte count does not match the resident CUDA slot")
        try:
            with self.upload_stream:
                self.sources[frame_index].set(source, stream=self.upload_stream)
                events[pinned_slot_index].record(self.upload_stream)
        except Exception as exc:
            raise CudaPreflightError(f"CUDA source upload failed: {exc}") from exc
        self._source_uploads += 1
        self._host_to_device_bytes += int(source.nbytes)

    def finish_uploads(self) -> None:
        """Make all source uploads visible before numerical CUDA work starts."""

        if self._closed:
            raise RuntimeError("CUDA session is closed")
        events = self._upload_events
        if events is None:
            raise RuntimeError("CUDA session upload events are unavailable")
        try:
            for event in events:
                event.synchronize()
                self._synchronizations += 1
        except Exception as exc:
            raise CudaPreflightError(f"CUDA source upload synchronization failed: {exc}") from exc
        self._refresh_peak_bytes()

    def solve_exposure_gains(
        self,
        *,
        latitude_span: float,
        horizontal_fov: float,
        vertical_fov: float,
        transfer_function: str,
    ) -> CudaExposureResult:
        """Solve global gains on CUDA without downloading image statistics or equations."""

        if self._closed:
            raise RuntimeError("CUDA session is closed")
        frame_count = int(self.sources.shape[0])
        if frame_count == 1:
            self.log_gains = self._cp.zeros(1, dtype=self._cp.float32)
            return CudaExposureResult(self.log_gains, self._cp.int32(0), self._cp.int32(0))

        transfer_code = {"srgb": 0, "pq": 1, "linear": 2}.get(transfer_function)
        if transfer_code is None:
            raise ValueError(f"unsupported CUDA transfer function: {transfer_function}")
        source_code = {"uint8": 0, "uint16": 1, "float32": 2}[self.sample_type]
        source_height, source_width = map(int, self.sources.shape[1:3])
        proxy_width = min(256, source_width)
        proxy_height = max(1, round(source_height * proxy_width / source_width))
        sample_width, sample_height = 256, 128
        proxy_pixels = frame_count * proxy_width * proxy_height
        sample_pixels = frame_count * sample_width * sample_height
        with self.compute_stream:
            self.exposure_proxies = self._cp.empty(
                (frame_count, proxy_height, proxy_width, 3), dtype=self._cp.float32
            )
            self.module["build_exposure_proxies"](
                ((proxy_pixels + 255) // 256,),
                (256,),
                (
                    self.sources,
                    self._cp.int32(source_code),
                    self._cp.int32(transfer_code),
                    self.exposure_proxies,
                    self._cp.int32(frame_count),
                    self._cp.int32(source_width),
                    self._cp.int32(source_height),
                    self._cp.int32(proxy_width),
                    self._cp.int32(proxy_height),
                ),
            )
            sample_shape = (frame_count, sample_height, sample_width)
            luminance = self._cp.empty(sample_shape, dtype=self._cp.float32)
            coverage = self._cp.empty(sample_shape, dtype=self._cp.uint8)
            clipped = self._cp.empty(sample_shape, dtype=self._cp.uint8)
            self.module["sample_exposure_grid"](
                ((sample_pixels + 255) // 256,),
                (256,),
                (
                    self.exposure_proxies,
                    luminance,
                    coverage,
                    clipped,
                    self.rotations,
                    self._cp.int32(frame_count),
                    self._cp.int32(proxy_width),
                    self._cp.int32(proxy_height),
                    self._cp.int32(sample_width),
                    self._cp.int32(sample_height),
                    self._cp.float32(latitude_span),
                    self._cp.float32(horizontal_fov),
                    self._cp.float32(vertical_fov),
                ),
            )
            gradients = self._cp.empty_like(luminance)
            valid = self._cp.empty_like(coverage)
            gradient_limits = self._cp.zeros(frame_count, dtype=self._cp.float32)
            self.module["classify_exposure_samples"](
                ((sample_pixels + 255) // 256,),
                (256,),
                (
                    luminance,
                    coverage,
                    clipped,
                    gradient_limits,
                    gradients,
                    valid,
                    self._cp.int32(frame_count),
                    self._cp.int32(sample_width),
                    self._cp.int32(sample_height),
                    self._cp.int32(0),
                ),
            )
            base_valid = (coverage != 0) & (clipped == 0) & self._cp.isfinite(luminance)
            gradient_limits = self._masked_quantile(
                gradients.reshape((frame_count, -1)), base_valid.reshape((frame_count, -1)), 0.9
            )
            self.module["classify_exposure_samples"](
                ((sample_pixels + 255) // 256,),
                (256,),
                (
                    luminance,
                    coverage,
                    clipped,
                    gradient_limits,
                    gradients,
                    valid,
                    self._cp.int32(frame_count),
                    self._cp.int32(sample_width),
                    self._cp.int32(sample_height),
                    self._cp.int32(1),
                ),
            )
            result = self._solve_exposure_equations(luminance, coverage, valid)
        self.record_kernel_launch()
        self.record_kernel_launch()
        self.record_kernel_launch()
        self.record_kernel_launch()
        self._refresh_peak_bytes()
        self.log_gains = result.log_gains
        return result

    def _solve_exposure_equations(
        self, luminance: Any, coverage: Any, valid: Any
    ) -> CudaExposureResult:
        """Reduce every frame pair and solve the anchored weighted system on-device."""

        cp = self._cp
        frame_count = int(luminance.shape[0])
        left, right = cp.triu_indices(frame_count, k=1)
        valid_flat = valid.reshape((frame_count, -1)) != 0
        coverage_flat = coverage.reshape((frame_count, -1)) != 0
        log_luminance = cp.log(cp.maximum(luminance.reshape((frame_count, -1)), cp.float32(1e-5)))
        pair_valid = valid_flat[left] & valid_flat[right]
        pair_coverage = coverage_flat[left] & coverage_flat[right]
        valid_counts = cp.sum(pair_valid, axis=1)
        geometric_counts = cp.sum(pair_coverage, axis=1)
        ratios = log_luminance[left] - log_luminance[right]
        low = self._masked_quantile(ratios, pair_valid, 0.1)
        high = self._masked_quantile(ratios, pair_valid, 0.9)
        inliers = pair_valid & (ratios >= low[:, None]) & (ratios <= high[:, None])
        median = self._masked_quantile(ratios, inliers, 0.5)
        mad = self._masked_quantile(cp.abs(ratios - median[:, None]), inliers, 0.5)
        inlier_counts = cp.sum(inliers, axis=1)
        measured = (
            (valid_counts >= 24)
            & (inlier_counts >= 12)
            & cp.isfinite(median)
            & cp.isfinite(mad)
            & (mad <= cp.float32(0.5))
        )
        weights = cp.where(measured, cp.sqrt(inlier_counts) / (1.0 + mad), 0.0).astype(cp.float64)
        ratios64 = cp.where(measured, median, 0.0).astype(cp.float64)
        geometric = geometric_counts >= 24
        edge_weights = cp.zeros((frame_count, frame_count), dtype=cp.float64)
        edge_ratios = cp.zeros((frame_count, frame_count), dtype=cp.float64)
        edge_weights[left, right] = weights
        edge_weights[right, left] = weights
        edge_ratios[left, right] = ratios64
        edge_ratios[right, left] = -ratios64
        bridge_weights = self._neutral_bridge_weights(edge_weights != 0.0, left, right, geometric)
        edge_weights += bridge_weights
        diagonal = cp.sum(edge_weights, axis=1)
        system = cp.diag(diagonal) - edge_weights
        values = -cp.sum(edge_weights * edge_ratios, axis=1)
        system[0, 0] += 1.0
        solution = cp.linalg.lstsq(system, values, rcond=None)[0]
        centered = solution - cp.median(solution)
        clipped = cp.clip(centered, -cp.log(2.0), cp.log(2.0)).astype(cp.float32)
        gains = clipped
        anchor = cp.argmin(cp.abs(centered - cp.median(centered))).astype(cp.int32)
        edge_count = cp.sum((weights != 0.0) | (bridge_weights[left, right] != 0.0)).astype(
            cp.int32
        )
        return CudaExposureResult(gains, anchor, edge_count)

    def _masked_quantile(self, values: Any, valid: Any, quantile: float) -> Any:
        """Compute one quantile per row entirely on CUDA, excluding invalid samples."""

        cp = self._cp
        if values.ndim != 2 or valid.shape != values.shape:
            raise ValueError("CUDA masked quantiles require matching two-dimensional arrays")
        sorted_values = cp.sort(cp.where(valid, values, cp.inf), axis=1)
        counts = cp.sum(valid, axis=1)
        positions = cp.maximum(counts - 1, 0).astype(cp.float64) * quantile
        lower = cp.floor(positions).astype(cp.int64)
        upper = cp.ceil(positions).astype(cp.int64)
        fractions = (positions - lower).astype(values.dtype)
        rows = cp.arange(values.shape[0])
        interpolated = (
            sorted_values[rows, lower] * (1.0 - fractions) + sorted_values[rows, upper] * fractions
        )
        return cp.where(counts > 0, interpolated, cp.nan).astype(cp.float32)

    def _neutral_bridge_weights(self, measured: Any, left: Any, right: Any, geometric: Any) -> Any:
        """Add neutral constraints only where geometric overlaps connect components."""

        cp = self._cp
        frame_count = int(measured.shape[0])
        adjacency = measured.copy()
        cp.fill_diagonal(adjacency, True)
        geometric_matrix = cp.zeros_like(adjacency)
        geometric_matrix[left, right] = geometric
        geometric_matrix[right, left] = geometric
        bridges = cp.zeros_like(adjacency, dtype=cp.float64)
        for _ in range(frame_count):
            reachability = adjacency
            for _ in range(frame_count):
                reachability = reachability | (
                    (reachability.astype(cp.int16) @ reachability.astype(cp.int16)) != 0
                )
            candidates = geometric_matrix & ~reachability
            rows = cp.arange(frame_count)
            first = cp.argmax(candidates, axis=1)
            has_candidate = cp.any(candidates, axis=1)
            selected = candidates[rows, first] & has_candidate
            additions = cp.zeros_like(adjacency)
            additions[rows[selected], first[selected]] = True
            additions[first[selected], rows[selected]] = True
            bridges = cp.where(additions, 1.0, bridges)
            adjacency = adjacency | additions
        return bridges

    def record_kernel_launch(self) -> None:
        self._kernel_launches += 1

    def record_download(self, byte_count: int) -> None:
        self._device_to_host_bytes += byte_count

    def begin_compute_timing(self) -> Any:
        """Record a CUDA event immediately before a watchdog-sized band."""

        if self._closed:
            raise RuntimeError("CUDA session is closed")
        event = self._cp.cuda.Event()
        with self.compute_stream:
            event.record()
        return event

    def end_compute_timing(self, started: Any) -> float:
        """Synchronize one band checkpoint and return its elapsed CUDA time."""

        if self._closed:
            raise RuntimeError("CUDA session is closed")
        completed = self._cp.cuda.Event()
        with self.compute_stream:
            completed.record()
        completed.synchronize()
        self._synchronizations += 1
        return float(self._cp.cuda.get_elapsed_time(started, completed)) / 1000.0

    def download_exposure_report(
        self, result: CudaExposureResult
    ) -> tuple[int, int, tuple[float, ...]]:
        """Download only final exposure report scalars after the device solve completes."""

        self.compute_stream.synchronize()
        self._synchronizations += 1
        gains = self._cp.asnumpy(result.log_gains)
        anchor = int(self._cp.asnumpy(result.anchor_index))
        edge_count = int(self._cp.asnumpy(result.edge_count))
        self.record_download(int(gains.nbytes) + np.dtype(np.int32).itemsize * 2)
        return anchor, edge_count, tuple(float(value) for value in gains)

    def _refresh_peak_bytes(self) -> None:
        try:
            used_bytes = int(self._cp.get_default_memory_pool().used_bytes())
        except Exception:
            used_bytes = self._plan.required_bytes
        self._peak_device_bytes = max(getattr(self, "_peak_device_bytes", 0), used_bytes)

    def close(self) -> None:
        """Release all session-owned buffers exactly once after any failure or cancellation."""

        if self._closed:
            return
        self._closed = True
        try:
            if self.compute_stream is not None:
                self.compute_stream.synchronize()
                self._synchronizations += 1
        except Exception:
            pass
        finally:
            self.sources = None
            self.rotations = None
            self.log_gains = None
            self.exposure_proxies = None
            self._pinned_slots = None
            self._upload_events = None
            self.upload_stream = None
            self.compute_stream = None
            self._cp.get_default_memory_pool().free_all_blocks()
            self._cp.get_default_pinned_memory_pool().free_all_blocks()


class CudaOutputJob:
    """Transactional owner for a resident or banded CUDA output workspace."""

    def __init__(
        self,
        session: CudaSession,
        *,
        output_width: int,
        output_height: int,
        output_sample_bytes: int,
        needs_sdr_conversion: bool,
        rectilinear_output: bool = False,
        output_vertical_fov: float = 0.0,
        plan: CudaMemoryPlan | None = None,
    ) -> None:
        if min(output_width, output_height, output_sample_bytes) < 1:
            raise ValueError("CUDA output dimensions must be positive")
        self._session = session
        self._cp = session._cp
        self._plan = plan or session._plan
        self._closed = False
        self.output_width = output_width
        self.output_height = output_height
        self.rectilinear_output = rectilinear_output
        self.output_vertical_fov = output_vertical_fov
        self.band_rows = self._plan.output_band_rows or output_height
        self.is_banded = self._plan.output_band_rows is not None
        self.local_exposure: Any = None
        self.color: Any = None
        self.coverage: Any = None
        self.converted: Any = None
        self.histogram: Any = None
        self.levels: Any = None
        try:
            local_width = max(
                1, (output_width + _LOCAL_EXPOSURE_SCALE - 1) // _LOCAL_EXPOSURE_SCALE
            )
            local_height = max(
                1, (output_height + _LOCAL_EXPOSURE_SCALE - 1) // _LOCAL_EXPOSURE_SCALE
            )
            self.local_exposure = self._cp.zeros(
                (local_height, local_width), dtype=self._cp.float32
            )
            self.color = self._cp.empty((self.band_rows, output_width, 3), dtype=self._cp.float32)
            self.coverage = self._cp.empty((self.band_rows, output_width), dtype=self._cp.uint8)
            if needs_sdr_conversion:
                dtype = self._cp.uint8 if output_sample_bytes == 1 else self._cp.uint16
                self.converted = self._cp.empty((self.band_rows, output_width, 3), dtype=dtype)
            self.histogram = self._cp.zeros(4096, dtype=self._cp.uint64)
            self.levels = self._cp.empty(2, dtype=self._cp.float32)
            session._refresh_peak_bytes()
        except Exception as exc:
            self.close()
            raise GpuUnavailableError(f"CUDA output allocation failed: {exc}") from exc

    def __enter__(self) -> CudaOutputJob:
        return self

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        self.close()

    def build_local_exposure(
        self,
        *,
        latitude_span: float,
        horizontal_fov: float,
        vertical_fov: float,
        log_gains: Any,
    ) -> None:
        """Build the quarter-resolution local exposure in one all-frame kernel."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        self._session.log_gains = self._cp.asarray(log_gains, dtype=self._cp.float32)
        field_height, field_width = self.local_exposure.shape
        block = (16, 16)
        grid = ((int(field_width) + 15) // 16, (int(field_height) + 15) // 16)
        with self._session.compute_stream:
            self._session.module["build_local_exposure"](
                grid,
                block,
                (
                    self.local_exposure,
                    self._cp.int32(field_width),
                    self._cp.int32(field_height),
                    self._cp.int32(self.output_width),
                    self._cp.int32(self.output_height),
                    self._cp.float32(latitude_span),
                    self._session.rotations,
                    self._session.log_gains,
                    self._cp.int32(self._session.sources.shape[0]),
                    self._cp.int32(self._session.sources.shape[2]),
                    self._cp.int32(self._session.sources.shape[1]),
                    self._cp.float32(horizontal_fov),
                    self._cp.float32(vertical_fov),
                    self._cp.int32(self.rectilinear_output),
                    self._cp.float32(self.output_vertical_fov),
                ),
            )
        self._session.record_kernel_launch()

    def build_auto_contrast_histogram(
        self,
        *,
        rows: int,
        transfer_function: str,
        reference_white_nits: float,
    ) -> None:
        """Accumulate this completed band into the global device histogram."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        if rows < 1 or rows > self.band_rows:
            raise ValueError("CUDA histogram rows exceed the output workspace")
        transfer_code = {"srgb": 0, "pq": 1, "linear": 2}.get(transfer_function)
        if transfer_code is None:
            raise ValueError(f"unsupported CUDA transfer function: {transfer_function}")
        pixels = rows * self.output_width
        with self._session.compute_stream:
            self._session.module["build_auto_contrast_histogram"](
                ((pixels + 255) // 256,),
                (256,),
                (
                    self.color,
                    self.coverage,
                    self.histogram,
                    self._cp.int32(pixels),
                    self._cp.int32(transfer_code),
                    self._cp.float32(reference_white_nits),
                ),
            )
        self._session.record_kernel_launch()

    def reset_auto_contrast_histogram(self) -> None:
        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        self.histogram.fill(0)

    def select_auto_contrast_levels(self) -> None:
        """Derive the shared 0.5% black and white points on-device."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        with self._session.compute_stream:
            self._session.module["select_auto_contrast_levels"](
                (1,), (1,), (self.histogram, self.levels)
            )
        self._session.record_kernel_launch()

    def convert_band(
        self,
        *,
        rows: int,
        transfer_function: str,
        reference_white_nits: float,
        apply_auto_contrast: bool,
    ) -> None:
        """Convert one composed band to its final SDR uint8 device representation."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        if self.converted is None:
            raise RuntimeError("CUDA output job has no SDR conversion buffer")
        if rows < 1 or rows > self.band_rows:
            raise ValueError("CUDA conversion rows exceed the output workspace")
        transfer_code = {"srgb": 0, "pq": 1, "linear": 2}.get(transfer_function)
        if transfer_code is None:
            raise ValueError(f"unsupported CUDA transfer function: {transfer_function}")
        pixels = rows * self.output_width
        with self._session.compute_stream:
            self._session.module["convert_output"](
                ((pixels + 255) // 256,),
                (256,),
                (
                    self.color,
                    self.converted,
                    self._cp.int32(pixels),
                    self._cp.int32(transfer_code),
                    self._cp.float32(reference_white_nits),
                    self.levels,
                    self._cp.int32(apply_auto_contrast),
                ),
            )
        self._session.record_kernel_launch()

    def download_band(self, target: np.ndarray[Any, Any], rows: int, *, converted: bool) -> None:
        """Copy exactly one completed band into its final pinned host location."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        source = self.converted if converted else self.color
        if source is None:
            raise RuntimeError("CUDA output job has no requested download buffer")
        expected_shape = (rows, self.output_width, 3)
        if target.shape != expected_shape:
            raise ValueError(f"CUDA download target {target.shape} does not match {expected_shape}")
        self._session.compute_stream.synchronize()
        self._session._synchronizations += 1
        self._cp.asnumpy(source[:rows], out=target)
        self._session.record_download(target.nbytes)

    def download_coverage(self, target: np.ndarray[Any, Any], rows: int) -> None:
        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        expected_shape = (rows, self.output_width)
        if target.shape != expected_shape:
            raise ValueError(f"CUDA coverage target {target.shape} does not match {expected_shape}")
        self._session.compute_stream.synchronize()
        self._session._synchronizations += 1
        self._cp.asnumpy(self.coverage[:rows], out=target)
        self._session.record_download(target.nbytes)

    def uncovered_count(self, rows: int) -> int:
        """Return the completed band's coverage failure count as one device scalar."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        if rows < 1 or rows > self.band_rows:
            raise ValueError("CUDA coverage rows exceed the output workspace")
        self._session.compute_stream.synchronize()
        self._session._synchronizations += 1
        count = self._cp.count_nonzero(self.coverage[:rows] == 0)
        host_count = self._cp.asnumpy(count)
        self._session.record_download(int(host_count.nbytes))
        return int(host_count)

    def compose_band(
        self,
        *,
        row_start: int,
        rows: int,
        latitude_span: float,
        horizontal_fov: float,
        vertical_fov: float,
        transfer_function: str,
        hard_blend: bool,
        incomplete_magenta: bool,
    ) -> None:
        """Compose one output band with a device-side loop over capture frames."""

        if self._closed:
            raise RuntimeError("CUDA output job is closed")
        if row_start < 0 or rows < 1 or row_start + rows > self.output_height:
            raise ValueError("CUDA output band lies outside the panorama")
        if rows > self.band_rows:
            raise ValueError("CUDA output band exceeds its allocated workspace")
        transfer_code = {"srgb": 0, "pq": 1, "linear": 2}.get(transfer_function)
        if transfer_code is None:
            raise ValueError(f"unsupported CUDA transfer function: {transfer_function}")
        source_code = {"uint8": 0, "uint16": 1, "float32": 2}[self._session.sample_type]
        field_height, field_width = self.local_exposure.shape
        block = (16, 16)
        grid = ((self.output_width + 15) // 16, (rows + 15) // 16)
        with self._session.compute_stream:
            self._session.module["compose_output"](
                grid,
                block,
                (
                    self._session.sources,
                    self._cp.int32(source_code),
                    self._cp.int32(transfer_code),
                    self._session.rotations,
                    self._session.log_gains,
                    self.local_exposure,
                    self._cp.int32(field_width),
                    self._cp.int32(field_height),
                    self.color,
                    self.coverage,
                    self._cp.int32(self._session.sources.shape[2]),
                    self._cp.int32(self._session.sources.shape[1]),
                    self._cp.int32(self.output_width),
                    self._cp.int32(self.output_height),
                    self._cp.int32(row_start),
                    self._cp.int32(rows),
                    self._cp.float32(latitude_span),
                    self._cp.float32(horizontal_fov),
                    self._cp.float32(vertical_fov),
                    self._cp.int32(self._session.sources.shape[0]),
                    self._cp.int32(hard_blend),
                    self._cp.int32(incomplete_magenta),
                    self._cp.int32(self.rectilinear_output),
                    self._cp.float32(self.output_vertical_fov),
                ),
            )
        self._session.record_kernel_launch()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self.local_exposure = None
        self.color = None
        self.coverage = None
        self.converted = None
        self.histogram = None
        self.levels = None


def cuda_device_info() -> GpuDeviceInfo:
    """Return device capacity without importing CuPy for CPU-only installations."""

    try:
        cp: Any = import_module("cupy")
    except ImportError as exc:
        raise GpuUnavailableError("CuPy is not installed") from exc
    try:
        device_count = int(cp.cuda.runtime.getDeviceCount())
        if device_count < 1:
            raise GpuUnavailableError("no CUDA device is available")
        device = cp.cuda.Device()
        free_bytes, total_bytes = cp.cuda.runtime.memGetInfo()
        properties: Any = cp.cuda.runtime.getDeviceProperties(device.id)
        raw_name = properties.get("name", b"CUDA device")
        name = raw_name.decode(errors="replace") if isinstance(raw_name, bytes) else str(raw_name)
        capability_raw = properties.get("major"), properties.get("minor")
        capability = (
            (int(capability_raw[0]), int(capability_raw[1]))
            if all(value is not None for value in capability_raw)
            else None
        )
        return GpuDeviceInfo(name, int(total_bytes), int(free_bytes), capability)
    except GpuUnavailableError:
        raise
    except Exception as exc:
        raise GpuUnavailableError(f"CUDA device probe failed: {exc}") from exc


def native_source_bytes(
    frame_count: int,
    source_width: int,
    source_height: int,
    sample_type: str,
) -> int:
    """Return resident RGB bytes without expanding native source samples."""

    bytes_per_sample = {"uint8": 1, "uint16": 2, "float32": 4}.get(sample_type)
    if bytes_per_sample is None:
        raise ValueError(f"unsupported CUDA source sample type: {sample_type}")
    if min(frame_count, source_width, source_height) < 1:
        raise ValueError("GPU render dimensions must be positive")
    return frame_count * source_width * source_height * 3 * bytes_per_sample


def cuda_memory_plan(
    *,
    frame_count: int,
    source_width: int,
    source_height: int,
    output_width: int,
    output_height: int,
    sample_type: str,
    output_sample_bytes: int,
    needs_sdr_conversion: bool,
    free_bytes: int,
    total_bytes: int,
    gpu_budget_bytes: int | None = None,
) -> CudaMemoryPlan | None:
    """Choose a full-frame or row-banded CUDA allocation plan before upload.

    Sources and all numerical scratch stay resident. Banded output is the only
    reduced-memory mode, and its completed pixels are copied exactly once into
    the final host array.
    """

    if min(output_width, output_height, output_sample_bytes) < 1:
        raise ValueError("GPU render dimensions must be positive")
    if free_bytes < 0 or total_bytes < 1 or free_bytes > total_bytes:
        raise ValueError("GPU memory values are invalid")
    if gpu_budget_bytes is not None and gpu_budget_bytes < 1:
        raise ValueError("GPU memory budget must be positive")

    reserve_bytes = max(GPU_RESERVE_BYTES, int(total_bytes * GPU_RESERVE_FRACTION))
    available_bytes = max(0, min(free_bytes, gpu_budget_bytes or free_bytes) - reserve_bytes)
    source_bytes = native_source_bytes(frame_count, source_width, source_height, sample_type)
    proxy_width = min(256, source_width)
    proxy_height = max(1, round(source_height * proxy_width / source_width))
    proxy_bytes = frame_count * proxy_width * proxy_height * 3 * 4
    exposure_sample_bytes = frame_count * 256 * 128 * (4 + 4 + 1 + 1)
    rotation_bytes = frame_count * 9 * 4
    session_workspace_bytes = (
        proxy_bytes + exposure_sample_bytes + rotation_bytes + _HISTOGRAM_BYTES
    )
    local_width = max(1, (output_width + _LOCAL_EXPOSURE_SCALE - 1) // _LOCAL_EXPOSURE_SCALE)
    local_height = max(1, (output_height + _LOCAL_EXPOSURE_SCALE - 1) // _LOCAL_EXPOSURE_SCALE)
    local_exposure_bytes = local_width * local_height * 4
    host_output_bytes = output_width * output_height * 3 * output_sample_bytes

    def output_workspace(rows: int) -> int:
        linear_bytes = rows * output_width * 3 * 4
        coverage_bytes = rows * output_width
        converted_bytes = (
            rows * output_width * 3 * output_sample_bytes if needs_sdr_conversion else 0
        )
        return (
            local_exposure_bytes
            + linear_bytes
            + coverage_bytes
            + converted_bytes
            + _HISTOGRAM_BYTES
        )

    resident_workspace = output_workspace(output_height)
    resident_required = source_bytes + session_workspace_bytes + resident_workspace
    if resident_required <= available_bytes:
        return CudaMemoryPlan(
            source_bytes,
            session_workspace_bytes,
            resident_workspace,
            host_output_bytes,
            reserve_bytes,
            None,
            resident_required,
            available_bytes,
        )

    fixed_bytes = source_bytes + session_workspace_bytes + local_exposure_bytes + _HISTOGRAM_BYTES
    bytes_per_row = output_width * (
        3 * 4 + 1 + (3 * output_sample_bytes if needs_sdr_conversion else 0)
    )
    maximum_rows = min(output_height, (available_bytes - fixed_bytes) // bytes_per_row)
    if maximum_rows < _MINIMUM_BAND_ROWS:
        return None
    band_rows = max(_MINIMUM_BAND_ROWS, (maximum_rows // _MINIMUM_BAND_ROWS) * _MINIMUM_BAND_ROWS)
    workspace = output_workspace(band_rows)
    required_bytes = source_bytes + session_workspace_bytes + workspace
    return CudaMemoryPlan(
        source_bytes,
        session_workspace_bytes,
        workspace,
        host_output_bytes,
        reserve_bytes,
        band_rows,
        required_bytes,
        available_bytes,
    )


def select_cuda_backend(
    *,
    frame_count: int,
    source_width: int,
    source_height: int,
    output_width: int,
    output_height: int,
    sample_type: str,
    output_sample_bytes: int,
    needs_sdr_conversion: bool,
    gpu_budget_bytes: int | None = None,
    strict: bool = False,
) -> tuple[BackendSelection, CudaMemoryPlan | None]:
    """Select CUDA before image decode, returning CPU only for pre-kernel failures."""

    try:
        device = cuda_device_info()
        plan = cuda_memory_plan(
            frame_count=frame_count,
            source_width=source_width,
            source_height=source_height,
            output_width=output_width,
            output_height=output_height,
            sample_type=sample_type,
            output_sample_bytes=output_sample_bytes,
            needs_sdr_conversion=needs_sdr_conversion,
            free_bytes=device.free_bytes,
            total_bytes=device.total_bytes,
            gpu_budget_bytes=gpu_budget_bytes,
        )
    except (GpuUnavailableError, ValueError) as error:
        if strict:
            raise GpuUnavailableError(str(error)) from error
        return BackendSelection("cpu", None, "cpu", None, None, str(error)), None
    if plan is None:
        reason = "insufficient CUDA memory for sources and a 32-row output band"
        if strict:
            raise GpuUnavailableError(reason)
        return (
            BackendSelection("cpu", device.name, "cpu", None, max(0, device.free_bytes), reason),
            None,
        )
    mode: Literal["resident", "banded"] = "resident" if plan.output_band_rows is None else "banded"
    return (
        BackendSelection(
            "cuda",
            device.name,
            mode,
            plan.required_bytes,
            plan.available_bytes,
            f"CUDA {mode}; reserve={plan.reserve_bytes} bytes",
        ),
        plan,
    )
