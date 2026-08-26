"""Optional CUDA capability probing and resident working-set admission."""

from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
from typing import Any

from pano_stitch.cuda_kernels import (
    ACCUMULATE_EXPOSURE,
    COMPOSITE_FRAME,
    COMPOSITE_PROJECTED,
    NORMALIZE_EXPOSURE,
)

MiB = 1024 * 1024
GPU_RESERVE_BYTES = 384 * MiB
GPU_RESERVE_FRACTION = 0.15
GPU_OVERHEAD_BYTES = 64 * MiB


@dataclass(frozen=True)
class GpuDeviceInfo:
    """CUDA device capacity reported at probe time."""

    name: str
    total_bytes: int
    free_bytes: int


@dataclass(frozen=True)
class GpuRenderPlan:
    """Resident CUDA allocation requirements for one panorama."""

    required_bytes: int
    available_bytes: int
    source_bytes: int
    output_bytes: int
    exposure_bytes: int
    writer_bytes: int


class GpuUnavailableError(RuntimeError):
    """Raised when CUDA cannot be used by an explicitly requested probe."""


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
        return GpuDeviceInfo(name, int(total_bytes), int(free_bytes))
    except GpuUnavailableError:
        raise
    except Exception as exc:
        raise GpuUnavailableError(f"CUDA device probe failed: {exc}") from exc


def resident_gpu_plan(
    *,
    frame_count: int,
    source_width: int,
    source_height: int,
    output_width: int,
    output_height: int,
    exposure_width: int,
    exposure_height: int,
    writer_strip_pixels: int,
    free_bytes: int,
    total_bytes: int,
    gpu_budget_bytes: int | None = None,
) -> GpuRenderPlan | None:
    """Return a resident plan, or ``None`` when current VRAM cannot admit it."""

    dimensions = (
        frame_count,
        source_width,
        source_height,
        output_width,
        output_height,
        exposure_width,
        exposure_height,
        writer_strip_pixels,
    )
    if any(value < 1 for value in dimensions):
        raise ValueError("GPU render dimensions must be positive")
    if free_bytes < 0 or total_bytes < 1 or free_bytes > total_bytes:
        raise ValueError("GPU memory values are invalid")
    if gpu_budget_bytes is not None and gpu_budget_bytes < 1:
        raise ValueError("GPU memory budget must be positive")

    source_bytes = frame_count * source_width * source_height * 3 * 4
    output_bytes = output_width * output_height * 4 * 4
    exposure_bytes = exposure_width * exposure_height * 2 * 4
    writer_bytes = writer_strip_pixels * 13
    required_bytes = (
        source_bytes + output_bytes + exposure_bytes + writer_bytes + GPU_OVERHEAD_BYTES
    )
    reserve = max(GPU_RESERVE_BYTES, int(total_bytes * GPU_RESERVE_FRACTION))
    available_bytes = min(free_bytes, gpu_budget_bytes or free_bytes) - reserve
    plan = GpuRenderPlan(
        required_bytes,
        max(0, available_bytes),
        source_bytes,
        output_bytes,
        exposure_bytes,
        writer_bytes,
    )
    return plan if required_bytes <= available_bytes else None


def cuda_remap_source(source: Any, map_x: Any, map_y: Any) -> Any:
    """Remap one source on CUDA, returning a NumPy float32 array at the host boundary.

    The import is intentionally lazy: CPU-only installs must be able to import the stitcher.
    Callers should keep the returned array on-device in a future fused compositor; this primitive
    exists as the incremental migration seam and parity reference.
    """

    try:
        cp: Any = import_module("cupy")
        ndimage: Any = import_module("cupyx.scipy.ndimage")
    except ImportError as exc:
        raise GpuUnavailableError("CuPy is not installed") from exc
    device_source = cp.asarray(source, dtype=cp.float32)
    coordinates = cp.stack(
        (cp.asarray(map_y, dtype=cp.float32), cp.asarray(map_x, dtype=cp.float32))
    )
    channels = [
        ndimage.map_coordinates(
            device_source[..., channel], coordinates, order=1, mode="opencv", prefilter=False
        )
        for channel in range(int(device_source.shape[2]))
    ]
    return cp.asnumpy(cp.stack(channels, axis=-1)).astype("float32", copy=False)


class CudaFrameCompositor:
    """CUDA bilinear sampler and accumulator for one output strip."""

    def __init__(self) -> None:
        try:
            cp: Any = import_module("cupy")
        except ImportError as exc:
            raise GpuUnavailableError("CuPy is not installed") from exc
        self._cp = cp
        try:
            self._kernel = cp.RawKernel(COMPOSITE_FRAME, "composite_frame")
            self._kernel.compile()
        except Exception as exc:
            raise GpuUnavailableError(f"CUDA kernel compilation failed: {exc}") from exc

    def composite(
        self,
        source: Any,
        map_x: Any,
        map_y: Any,
        valid: Any,
        candidate: Any,
        correction: Any,
        color: Any,
        weight: Any,
        source_width: int,
        source_height: int,
        hard_blend: bool,
    ) -> None:
        """Accumulate one strip; all arrays may be NumPy or CuPy and are float32."""

        cp = self._cp
        device_source = cp.asarray(source, dtype=cp.float32)
        device_map_x = cp.asarray(map_x, dtype=cp.float32).ravel()
        device_map_y = cp.asarray(map_y, dtype=cp.float32).ravel()
        device_valid = cp.asarray(valid, dtype=cp.uint8).ravel()
        device_candidate = cp.asarray(candidate, dtype=cp.float32).ravel()
        device_correction = cp.asarray(correction, dtype=cp.float32).ravel()
        device_color = cp.asarray(color, dtype=cp.float32).reshape((-1, 3))
        device_weight = cp.asarray(weight, dtype=cp.float32).ravel()
        pixels = int(device_map_x.size)
        self._kernel(
            ((pixels + 255) // 256,),
            (256,),
            (
                device_source,
                device_map_x,
                device_map_y,
                device_valid,
                device_candidate,
                device_correction,
                device_color,
                device_weight,
                source_width,
                source_height,
                pixels,
                int(hard_blend),
            ),
        )
        cp.cuda.runtime.deviceSynchronize()
        if not isinstance(color, cp.ndarray):
            np_color = cp.asnumpy(device_color).reshape(color.shape)
            np_weight = cp.asnumpy(device_weight).reshape(weight.shape)
            color[...] = np_color
            weight[...] = np_weight


class CudaResidentCompositor:
    """Resident source/accumulator storage for full-frame CUDA rendering."""

    def __init__(
        self,
        frame_count: int,
        source_height: int,
        source_width: int,
        output_height: int,
        output_width: int,
    ) -> None:
        try:
            self._cp: Any = import_module("cupy")
        except ImportError as exc:
            raise GpuUnavailableError("CuPy is not installed") from exc
        cp = self._cp
        self.sources: Any = None
        self.color: Any = None
        self.weight: Any = None
        self.exposure_sum: Any = None
        self.exposure_weight: Any = None
        self._output_exposure: Any = None
        self.source_uploads = 0
        self.row_downloads = 0
        self.source_upload_bytes = 0
        self.exposure_upload_bytes = 0
        self.row_download_bytes = 0
        try:
            self._normalize_kernel = cp.RawKernel(NORMALIZE_EXPOSURE, "normalize_exposure")
            self._accumulate_kernel = cp.RawKernel(ACCUMULATE_EXPOSURE, "accumulate_exposure")
            self._composite_kernel = cp.RawKernel(COMPOSITE_FRAME, "composite_frame")
            self._projected_kernel = cp.RawKernel(COMPOSITE_PROJECTED, "composite_projected")
            for kernel in (
                self._normalize_kernel,
                self._accumulate_kernel,
                self._composite_kernel,
                self._projected_kernel,
            ):
                kernel.compile()
            self.sources = cp.empty((frame_count, source_height, source_width, 3), dtype=cp.float32)
            self.color = cp.zeros((output_height, output_width, 3), dtype=cp.float32)
            self.weight = cp.zeros((output_height, output_width), dtype=cp.float32)
        except Exception as exc:
            self.sources = None
            self.color = None
            self.weight = None
            cp.get_default_memory_pool().free_all_blocks()
            message = f"CUDA resident backend initialization failed: {exc}"
            raise GpuUnavailableError(message) from exc

    def allocate_exposure(self, height: int, width: int) -> None:
        """Allocate quarter-resolution exposure accumulators on the device."""

        if height < 1 or width < 1:
            raise ValueError("exposure dimensions must be positive")
        cp = self._cp
        self.exposure_sum = cp.zeros((height, width), dtype=cp.float32)
        self.exposure_weight = cp.zeros((height, width), dtype=cp.float32)

    def normalize_exposure(self) -> None:
        """Normalize the resident exposure field without a host round trip."""

        if self.exposure_sum is None or self.exposure_weight is None:
            raise RuntimeError("exposure buffers have not been allocated")
        pixels = int(self.exposure_sum.size)
        self._normalize_kernel(
            ((pixels + 255) // 256,),
            (256,),
            (self.exposure_sum, self.exposure_weight, self._cp.int32(pixels)),
        )

    def accumulate_exposure(
        self,
        rotation: Any,
        latitude_span: float,
        horizontal_fov: float,
        vertical_fov: float,
        log_gain: float,
    ) -> None:
        """Accumulate one frame's exposure field entirely on the device."""

        if self.exposure_sum is None or self.exposure_weight is None:
            raise RuntimeError("exposure buffers have not been allocated")
        cp = self._cp
        height, width = self.exposure_sum.shape
        pixels = int(width * height)
        device_rotation = cp.asarray(rotation, dtype=cp.float32).reshape(9)
        self._accumulate_kernel(
            ((pixels + 255) // 256,),
            (256,),
            (
                self.exposure_sum,
                self.exposure_weight,
                self._cp.int32(width),
                self._cp.int32(height),
                self._cp.float32(latitude_span),
                device_rotation,
                self._cp.int32(self.sources.shape[2]),
                self._cp.int32(self.sources.shape[1]),
                self._cp.float32(horizontal_fov),
                self._cp.float32(vertical_fov),
                self._cp.float32(log_gain),
            ),
        )

    def upload_source(self, frame_position: int, source: Any) -> None:
        """Upload one decoded source into its permanent device slot."""

        self.sources[frame_position] = self._cp.asarray(source, dtype=self._cp.float32)
        self.source_uploads += 1
        self.source_upload_bytes += int(source.nbytes)

    def composite_maps(
        self,
        frame_position: int,
        map_x: Any,
        map_y: Any,
        valid: Any,
        candidate: Any,
        correction: Any,
        source_width: int,
        source_height: int,
        hard_blend: bool,
    ) -> None:
        """Accumulate one frame into resident output buffers without downloading them."""

        cp = self._cp
        device_map_x = cp.asarray(map_x, dtype=cp.float32).ravel()
        device_map_y = cp.asarray(map_y, dtype=cp.float32).ravel()
        device_valid = cp.asarray(valid, dtype=cp.uint8).ravel()
        device_candidate = cp.asarray(candidate, dtype=cp.float32).ravel()
        device_correction = cp.asarray(correction, dtype=cp.float32).ravel()
        pixels = int(device_map_x.size)
        if pixels != int(self.color.shape[0] * self.color.shape[1]):
            raise ValueError("resident composite maps must cover the complete output")
        self._composite_kernel(
            ((pixels + 255) // 256,),
            (256,),
            (
                self.sources[frame_position],
                device_map_x,
                device_map_y,
                device_valid,
                device_candidate,
                device_correction,
                self.color,
                self.weight,
                self._cp.int32(source_width),
                self._cp.int32(source_height),
                self._cp.int32(pixels),
                self._cp.int32(hard_blend),
            ),
        )

    def composite_projected(
        self,
        frame_position: int,
        exposure: Any,
        rotation: Any,
        latitude_span: float,
        horizontal_fov: float,
        vertical_fov: float,
        hard_blend: bool,
        log_gain: float = 0.0,
    ) -> None:
        """Project and blend a resident source without host-side map arrays."""

        cp = self._cp
        pixels = int(self.color.shape[0] * self.color.shape[1])
        device_exposure = cp.asarray(exposure, dtype=cp.float32)
        if device_exposure.ndim != 2:
            raise ValueError("resident exposure must be a two-dimensional field")
        device_rotation = cp.asarray(rotation, dtype=cp.float32).reshape(9)
        exposure_height, exposure_width = map(int, device_exposure.shape)
        self._projected_kernel(
            ((pixels + 255) // 256,),
            (256,),
            (
                self.sources[frame_position],
                device_exposure,
                self.color,
                self.weight,
                self._cp.int32(self.sources.shape[2]),
                self._cp.int32(self.sources.shape[1]),
                self._cp.int32(self.color.shape[1]),
                self._cp.int32(self.color.shape[0]),
                self._cp.float32(latitude_span),
                self._cp.float32(horizontal_fov),
                self._cp.float32(vertical_fov),
                device_rotation,
                self._cp.int32(hard_blend),
                self._cp.int32(exposure_width),
                self._cp.int32(exposure_height),
                self._cp.float32(log_gain),
            ),
        )

    def set_output_exposure(self, exposure: Any) -> None:
        """Keep the exposure field resident without expanding it on the host."""

        device_exposure = self._cp.asarray(exposure, dtype=self._cp.float32)
        if device_exposure.ndim != 2:
            raise ValueError("resident exposure must be a two-dimensional field")
        self.exposure_upload_bytes += int(exposure.nbytes)
        self._output_exposure = device_exposure

    def composite_projected_with_gain(
        self,
        frame_position: int,
        log_gain: float,
        rotation: Any,
        latitude_span: float,
        horizontal_fov: float,
        vertical_fov: float,
        hard_blend: bool,
    ) -> None:
        """Composite using a resident exposure field and device-side gain correction."""

        if self._output_exposure is None:
            raise RuntimeError("output exposure has not been uploaded")
        self.composite_projected(
            frame_position,
            self._output_exposure,
            rotation,
            latitude_span,
            horizontal_fov,
            vertical_fov,
            hard_blend,
            log_gain,
        )

    def download_rows(self, row_start: int, row_end: int) -> tuple[Any, Any]:
        """Copy only finished rows to host memory."""

        self._cp.cuda.runtime.deviceSynchronize()
        self.row_downloads += 1
        color = self._cp.asnumpy(self.color[row_start:row_end])
        weight = self._cp.asnumpy(self.weight[row_start:row_end])
        self.row_download_bytes += int(color.nbytes + weight.nbytes)
        return color, weight

    def close(self) -> None:
        """Release resident allocations after stream synchronization."""

        try:
            self._cp.cuda.runtime.deviceSynchronize()
        except Exception:
            pass
        finally:
            self.sources = None
            self.color = None
            self.weight = None
            self.exposure_sum = None
            self.exposure_weight = None
            self._output_exposure = None
            self._cp.get_default_memory_pool().free_all_blocks()
