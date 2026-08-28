from __future__ import annotations

import sys
from types import SimpleNamespace

import pytest

import pano_stitch.gpu as gpu
from pano_stitch.cuda_kernels import CUDA_KERNEL_NAMES, CUDA_MODULE_SOURCE
from pano_stitch.gpu import (
    CudaBandScheduler,
    GpuDeviceInfo,
    GpuUnavailableError,
    MiB,
    cuda_device_info,
    cuda_memory_plan,
    cuda_preview_display_bytes,
    native_source_bytes,
    select_cuda_backend,
)


def test_cuda_band_scheduler_starts_at_256_and_targets_watchdog_time() -> None:
    scheduler = CudaBandScheduler(2048)

    assert scheduler.rows == 256
    assert scheduler.next_rows(100) == 100

    scheduler.record_elapsed(0.5)
    assert scheduler.rows == 128
    scheduler.record_elapsed(0.125)
    assert scheduler.rows == 256


def test_cuda_band_scheduler_respects_workspace_and_row_bounds() -> None:
    constrained = CudaBandScheduler(32)
    assert constrained.rows == 32
    constrained.record_elapsed(10.0)
    assert constrained.rows == 32

    scheduler = CudaBandScheduler(4096)
    scheduler.record_elapsed(0.001)
    scheduler.record_elapsed(0.001)
    assert scheduler.rows == 1024
    scheduler.record_elapsed(10.0)
    scheduler.record_elapsed(10.0)
    scheduler.record_elapsed(10.0)
    scheduler.record_elapsed(10.0)
    assert scheduler.rows == 64


@pytest.mark.parametrize(
    ("sample_type", "expected"),
    (("uint8", 12), ("uint16", 24), ("float32", 48)),
)
def test_native_source_bytes_preserves_source_precision(sample_type: str, expected: int) -> None:
    assert native_source_bytes(1, 2, 2, sample_type) == expected


def test_cuda_memory_plan_chooses_banded_output_when_full_frame_does_not_fit() -> None:
    plan = cuda_memory_plan(
        frame_count=2,
        source_width=512,
        source_height=512,
        output_width=16384,
        output_height=8192,
        sample_type="uint8",
        output_sample_bytes=1,
        needs_sdr_conversion=True,
        free_bytes=1024 * MiB,
        total_bytes=2 * 1024 * MiB,
    )
    assert plan is not None
    assert plan.output_band_rows is not None
    assert plan.output_band_rows >= 32
    assert plan.required_bytes <= plan.available_bytes


def test_cuda_memory_plan_rejects_when_sources_and_minimum_band_do_not_fit() -> None:
    assert (
        cuda_memory_plan(
            frame_count=32,
            source_width=4096,
            source_height=2048,
            output_width=2048,
            output_height=1024,
            sample_type="float32",
            output_sample_bytes=1,
            needs_sdr_conversion=True,
            free_bytes=2 * 1024 * MiB,
            total_bytes=6 * 1024 * MiB,
        )
        is None
    )


def test_cuda_memory_plan_admits_a_large_capture_on_a_simulated_six_gib_card() -> None:
    plan = cuda_memory_plan(
        frame_count=60,
        source_width=3840,
        source_height=2160,
        output_width=24000,
        output_height=12000,
        sample_type="uint8",
        output_sample_bytes=1,
        needs_sdr_conversion=True,
        free_bytes=6 * 1024 * MiB,
        total_bytes=6 * 1024 * MiB,
    )

    assert plan is not None
    assert plan.output_band_rows is not None
    assert plan.required_bytes <= plan.available_bytes


def test_cuda_preview_display_bytes_includes_full_pose_masks_and_viewport_buffers() -> None:
    assert (
        cuda_preview_display_bytes(
            frame_count=16,
            preview_width=4000,
            preview_height=2000,
            viewport_width=1000,
            viewport_height=500,
        )
        == 163_000_016
    )


def test_cuda_memory_plan_reserves_retained_preview_cache() -> None:
    plan = cuda_memory_plan(
        frame_count=1,
        source_width=1,
        source_height=1,
        output_width=32,
        output_height=32,
        sample_type="uint8",
        output_sample_bytes=1,
        needs_sdr_conversion=True,
        free_bytes=512 * MiB,
        total_bytes=512 * MiB,
        preview_cache_bytes=129 * MiB,
    )

    assert plan is None


def test_select_cuda_backend_reports_pre_kernel_memory_fallback(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(gpu, "cuda_device_info", lambda: GpuDeviceInfo("test", 1024, 512))
    selection, plan = select_cuda_backend(
        frame_count=32,
        source_width=4096,
        source_height=2048,
        output_width=2048,
        output_height=1024,
        sample_type="float32",
        output_sample_bytes=1,
        needs_sdr_conversion=True,
    )
    assert selection.backend == "cpu"
    assert selection.memory_mode == "cpu"
    assert "insufficient CUDA memory" in selection.reason
    assert plan is None


def test_select_cuda_backend_strict_mode_raises_for_pre_kernel_memory_fallback(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(gpu, "cuda_device_info", lambda: GpuDeviceInfo("test", 1024, 512))
    with pytest.raises(GpuUnavailableError, match="insufficient CUDA memory"):
        select_cuda_backend(
            frame_count=32,
            source_width=4096,
            source_height=2048,
            output_width=2048,
            output_height=1024,
            sample_type="float32",
            output_sample_bytes=1,
            needs_sdr_conversion=True,
            strict=True,
        )


@pytest.mark.parametrize(
    "kernel_name",
    (
        "build_exposure_proxies",
        "sample_exposure_grid",
        "classify_exposure_samples",
        "compose_output",
        "expand_preview_masks",
        "compose_preview_display",
    ),
)
def test_cuda_module_contains_full_gpu_pipeline_kernels(kernel_name: str) -> None:
    assert kernel_name in CUDA_KERNEL_NAMES
    assert f'extern "C" __global__ void {kernel_name}' in CUDA_MODULE_SOURCE


def test_cuda_module_excludes_retired_map_input_kernels() -> None:
    for kernel_name in (
        "normalize_exposure",
        "accumulate_exposure",
        "composite_frame",
        "composite_projected",
    ):
        assert kernel_name not in CUDA_KERNEL_NAMES
        assert f'extern "C" __global__ void {kernel_name}' not in CUDA_MODULE_SOURCE


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
