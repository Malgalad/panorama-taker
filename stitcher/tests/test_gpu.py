from __future__ import annotations

import sys
from types import SimpleNamespace

import pytest

from pano_stitch.gpu import (
    GPU_OVERHEAD_BYTES,
    GpuUnavailableError,
    MiB,
    cuda_device_info,
    resident_gpu_plan,
)


def _kwargs() -> dict[str, int]:
    return {
        "frame_count": 1,
        "source_width": 2,
        "source_height": 2,
        "output_width": 2,
        "output_height": 1,
        "exposure_width": 1,
        "exposure_height": 1,
        "writer_strip_pixels": 2,
    }


def test_resident_plan_accepts_exact_available_capacity() -> None:
    base = resident_gpu_plan(**_kwargs(), free_bytes=2 * 1024 * MiB, total_bytes=4 * 1024 * MiB)
    assert base is not None
    plan = resident_gpu_plan(
        **_kwargs(),
        free_bytes=base.required_bytes + 2 * 1024 * MiB - base.available_bytes,
        total_bytes=4 * 1024 * MiB,
    )
    assert plan is not None
    assert plan.required_bytes == plan.available_bytes


def test_resident_plan_rejects_when_budget_is_too_small() -> None:
    plan = resident_gpu_plan(
        **_kwargs(),
        free_bytes=2 * 1024 * MiB,
        total_bytes=4 * 1024 * MiB,
        gpu_budget_bytes=GPU_OVERHEAD_BYTES,
    )
    assert plan is None


def test_resident_plan_rejects_when_free_memory_is_insufficient() -> None:
    assert resident_gpu_plan(**_kwargs(), free_bytes=1, total_bytes=4 * 1024 * MiB) is None


@pytest.mark.parametrize("field", ("frame_count", "source_width", "output_height"))
def test_resident_plan_rejects_nonpositive_dimensions(field: str) -> None:
    kwargs = _kwargs()
    kwargs[field] = 0
    with pytest.raises(ValueError, match="dimensions"):
        resident_gpu_plan(**kwargs, free_bytes=2 * 1024 * MiB, total_bytes=4 * 1024 * MiB)


def test_cuda_device_info_rejects_zero_devices(monkeypatch: pytest.MonkeyPatch) -> None:
    runtime = SimpleNamespace(getDeviceCount=lambda: 0)
    monkeypatch.setitem(sys.modules, "cupy", SimpleNamespace(cuda=SimpleNamespace(runtime=runtime)))
    with pytest.raises(GpuUnavailableError, match="no CUDA device"):
        cuda_device_info()


def test_cuda_device_info_wraps_driver_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    def fail() -> int:
        raise RuntimeError("insufficient driver")

    runtime = SimpleNamespace(getDeviceCount=fail)
    monkeypatch.setitem(sys.modules, "cupy", SimpleNamespace(cuda=SimpleNamespace(runtime=runtime)))
    with pytest.raises(GpuUnavailableError, match="probe failed: insufficient driver"):
        cuda_device_info()
