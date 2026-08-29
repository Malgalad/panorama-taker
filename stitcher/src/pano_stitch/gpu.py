"""Backend-neutral GPU scheduling and D3D12 admission."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from typing import Literal

GPU_BACKEND_ID: Literal["gpu"] = "gpu"
MiB = 1024 * 1024
_WATCHDOG_TARGET_SECONDS = 0.25
_WATCHDOG_INITIAL_ROWS = 256
_WATCHDOG_MINIMUM_ROWS = 64
_WATCHDOG_MAXIMUM_ROWS = 1024


@dataclass(frozen=True)
class BackendSelection:
    backend: Literal["cpu", "gpu"]
    device_name: str | None
    memory_mode: Literal["cpu", "resident", "banded"]
    required_bytes: int | None
    available_bytes: int | None
    reason: str


@dataclass(frozen=True)
class GpuMemoryPlan:
    source_bytes: int
    session_workspace_bytes: int
    output_workspace_bytes: int
    host_output_bytes: int
    reserve_bytes: int
    output_band_rows: int | None
    required_bytes: int
    available_bytes: int
    preview_cache_bytes: int = 0


@dataclass
class GpuBandScheduler:
    workspace_rows: int
    target_seconds: float = _WATCHDOG_TARGET_SECONDS

    def __post_init__(self) -> None:
        if self.workspace_rows < 1:
            raise ValueError("GPU band workspace must contain at least one row")
        if self.target_seconds <= 0.0:
            raise ValueError("GPU band target time must be positive")
        self._minimum_rows = min(_WATCHDOG_MINIMUM_ROWS, self.workspace_rows)
        self._maximum_rows = min(_WATCHDOG_MAXIMUM_ROWS, self.workspace_rows)
        self._rows = min(_WATCHDOG_INITIAL_ROWS, self._maximum_rows)

    @property
    def rows(self) -> int:
        return self._rows

    def next_rows(self, remaining_rows: int) -> int:
        if remaining_rows < 1:
            raise ValueError("GPU band scheduler needs remaining output rows")
        return min(self._rows, remaining_rows)

    def record_elapsed(self, seconds: float) -> None:
        if seconds < 0.0:
            raise ValueError("GPU band elapsed time cannot be negative")
        scale = 2.0 if seconds == 0.0 else max(0.5, min(2.0, self.target_seconds / seconds))
        self._rows = max(self._minimum_rows, min(self._maximum_rows, round(self._rows * scale)))


class GpuUnavailableError(RuntimeError):
    """The requested GPU backend is unavailable."""


class GpuPreflightError(GpuUnavailableError):
    """GPU setup failed before numerical dispatch."""


def gpu_preview_display_bytes(
    *,
    frame_count: int,
    preview_width: int,
    preview_height: int,
    viewport_width: int,
    viewport_height: int,
) -> int:
    if min(frame_count, preview_width, preview_height, viewport_width, viewport_height) < 1:
        raise ValueError("GPU preview display dimensions must be positive")
    preview_pixels = preview_width * preview_height
    viewport_pixels = viewport_width * viewport_height
    return preview_pixels * (3 + frame_count) + viewport_pixels * (6 + frame_count) + frame_count


def native_source_bytes(
    frame_count: int, source_width: int, source_height: int, sample_type: str
) -> int:
    bytes_per_sample = {"uint8": 1, "uint16": 2, "float32": 4}.get(sample_type)
    if bytes_per_sample is None:
        raise ValueError(f"unsupported GPU source sample type: {sample_type}")
    if min(frame_count, source_width, source_height) < 1:
        raise ValueError("GPU render dimensions must be positive")
    return frame_count * source_width * source_height * 3 * bytes_per_sample


def select_gpu_backend(
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
    preview_cache_bytes: int = 0,
    strict: bool = False,
) -> tuple[BackendSelection, GpuMemoryPlan | None]:
    """Select D3D12 or pre-dispatch CPU fallback."""

    from pano_stitch.d3d12_adapter import (
        PANO_GPU_ABI_VERSION,
        D3D12AdapterUnavailableError,
        _MemoryRequest,
        load_d3d12_adapter,
    )

    sample_bytes = {"uint8": 1, "uint16": 2, "float32": 4}.get(sample_type)
    try:
        if sample_bytes is None:
            raise ValueError(f"unsupported GPU source sample type: {sample_type}")
        if min(frame_count, source_width, source_height, output_width, output_height) < 1:
            raise ValueError("GPU render dimensions must be positive")
        if output_sample_bytes < 1 or preview_cache_bytes < 0:
            raise ValueError("GPU output allocation values are invalid")
        adapter = load_d3d12_adapter()
        device = adapter.probe()
        free_bytes = max(0, int(device.local_budget_bytes) - int(device.local_usage_bytes))
        total_bytes = max(int(device.dedicated_bytes), int(device.local_budget_bytes), 1)
        source_frame_bytes = source_width * source_height * 3 * sample_bytes
        aligned_source_frame_bytes = (source_frame_bytes + 65535) & ~65535
        session_workspace_bytes = (frame_count * 9 * 4 + 65535) & ~65535
        output_bytes_per_pixel = max(62 + 21 * frame_count, 52 if needs_sdr_conversion else 25)
        request = _MemoryRequest(
            size=ctypes.sizeof(_MemoryRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            frame_count=frame_count,
            source_width=source_width,
            source_height=source_height,
            source_sample_bytes=sample_bytes,
            output_width=output_width,
            output_height=output_height,
            output_sample_bytes=output_sample_bytes,
            needs_sdr_conversion=int(needs_sdr_conversion),
            free_bytes=free_bytes,
            total_bytes=total_bytes,
            requested_budget_bytes=gpu_budget_bytes or 0,
            preview_cache_bytes=preview_cache_bytes,
            session_workspace_bytes=session_workspace_bytes,
            output_workspace_bytes_per_pixel=output_bytes_per_pixel,
            output_workspace_fixed_bytes=4096 * 4 + (4 * frame_count + 16) * 65536,
            upload_bytes=aligned_source_frame_bytes * 2,
            readback_bytes_per_pixel=12,
            readback_fixed_bytes=0,
            descriptor_count=frame_count + 4,
            reserved=0,
        )
        native_plan = adapter.plan_memory(request)
    except (D3D12AdapterUnavailableError, ValueError) as error:
        if strict:
            raise GpuUnavailableError(str(error)) from error
        return BackendSelection("cpu", None, "cpu", None, None, str(error)), None
    mode: Literal["resident", "banded"] = (
        "resident" if native_plan.output_band_rows == 0 else "banded"
    )
    name = bytes(device.name).split(b"\0", 1)[0].decode("utf-8", errors="replace")
    plan = GpuMemoryPlan(
        int(native_plan.source_bytes),
        int(native_plan.session_workspace_bytes),
        int(native_plan.output_workspace_bytes),
        output_width * output_height * 3 * output_sample_bytes,
        int(native_plan.reserve_bytes),
        None if native_plan.output_band_rows == 0 else int(native_plan.output_band_rows),
        int(native_plan.required_bytes),
        int(native_plan.available_bytes),
        preview_cache_bytes,
    )
    return BackendSelection(
        GPU_BACKEND_ID,
        name,
        mode,
        plan.required_bytes,
        plan.available_bytes,
        f"D3D12 {mode}; reserve={plan.reserve_bytes} bytes",
    ), plan
