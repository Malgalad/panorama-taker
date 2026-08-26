"""Optional CUDA capability probing and resident working-set admission."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

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


_COMPOSITE_KERNEL = r"""
extern "C" __global__ void composite_frame(
    const float* source, const float* map_x, const float* map_y,
    const unsigned char* valid, const float* candidate, const float* correction,
    float* color, float* weight, int source_width, int source_height,
    int pixels, int hard_blend) {
  int index = blockDim.x * blockIdx.x + threadIdx.x;
  if (index >= pixels || !valid[index]) return;
  float x = map_x[index], y = map_y[index];
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  int x1 = min(x0 + 1, source_width - 1), y1 = min(y0 + 1, source_height - 1);
  float wx = x - (float)x0, wy = y - (float)y0;
  int p00 = (y0 * source_width + x0) * 3;
  int p10 = (y0 * source_width + x1) * 3;
  int p01 = (y1 * source_width + x0) * 3;
  int p11 = (y1 * source_width + x1) * 3;
  float w00 = (1.0f - wx) * (1.0f - wy), w10 = wx * (1.0f - wy);
  float w01 = (1.0f - wx) * wy, w11 = wx * wy;
  float c = candidate[index];
  if (hard_blend) {
    if (c <= weight[index]) return;
    for (int channel = 0; channel < 3; ++channel)
      color[index * 3 + channel] = (source[p00 + channel] * w00 + source[p10 + channel] * w10 +
        source[p01 + channel] * w01 + source[p11 + channel] * w11) * correction[index];
    weight[index] = c;
  } else {
    for (int channel = 0; channel < 3; ++channel)
      color[index * 3 + channel] += (source[p00 + channel] * w00 + source[p10 + channel] * w10 +
        source[p01 + channel] * w01 + source[p11 + channel] * w11) * correction[index] * c;
    weight[index] += c;
  }
}
"""


def cuda_device_info() -> GpuDeviceInfo:
    """Return device capacity without importing CuPy for CPU-only installations."""

    try:
        import cupy as cp  # type: ignore[import-untyped]
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
        import cupy as cp
        from cupyx.scipy import ndimage  # type: ignore[import-untyped]
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
            import cupy as cp
        except ImportError as exc:
            raise GpuUnavailableError("CuPy is not installed") from exc
        self._cp = cp
        try:
            self._kernel = cp.RawKernel(_COMPOSITE_KERNEL, "composite_frame")
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
