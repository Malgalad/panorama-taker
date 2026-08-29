from __future__ import annotations

from types import SimpleNamespace

import pytest

import pano_stitch.d3d12_adapter as d3d12_adapter
from pano_stitch.d3d12_adapter import D3D12AdapterUnavailableError
from pano_stitch.gpu import (
    GpuBandScheduler,
    GpuMemoryPlan,
    GpuUnavailableError,
    MiB,
    gpu_preview_display_bytes,
    native_source_bytes,
    select_gpu_backend,
)


@pytest.mark.gpu_contract
def test_gpu_band_scheduler_targets_watchdog_time_and_workspace() -> None:
    scheduler = GpuBandScheduler(2048)
    assert scheduler.rows == 256
    assert scheduler.next_rows(100) == 100
    scheduler.record_elapsed(0.5)
    assert scheduler.rows == 128
    scheduler.record_elapsed(0.125)
    assert scheduler.rows == 256

    constrained = GpuBandScheduler(32)
    constrained.record_elapsed(10.0)
    assert constrained.rows == 32


@pytest.mark.parametrize(
    ("sample_type", "expected"),
    (("uint8", 12), ("uint16", 24), ("float32", 48)),
)
def test_native_source_bytes_preserves_source_precision(sample_type: str, expected: int) -> None:
    assert native_source_bytes(1, 2, 2, sample_type) == expected


def test_gpu_preview_display_bytes_includes_masks_and_viewport_buffers() -> None:
    assert (
        gpu_preview_display_bytes(
            frame_count=16,
            preview_width=4000,
            preview_height=2000,
            viewport_width=1000,
            viewport_height=500,
        )
        == 163_000_016
    )


@pytest.mark.gpu_contract
def test_neutral_gpu_contracts_are_real_definitions() -> None:
    assert GpuMemoryPlan.__name__ == "GpuMemoryPlan"
    assert GpuBandScheduler.__name__ == "GpuBandScheduler"


@pytest.mark.gpu_contract
def test_neutral_selector_uses_d3d12(monkeypatch: pytest.MonkeyPatch) -> None:
    captured: list[object] = []

    class Adapter:
        def probe(self) -> object:
            return SimpleNamespace(
                name=b"Test D3D12\0",
                dedicated_bytes=1024 * MiB,
                local_budget_bytes=1024 * MiB,
                local_usage_bytes=0,
            )

        def plan_memory(self, request: object) -> object:
            captured.append(request)
            return SimpleNamespace(
                output_band_rows=0,
                source_bytes=65536,
                session_workspace_bytes=65536,
                output_workspace_bytes=65536,
                reserve_bytes=384 * MiB,
                required_bytes=4 * 65536,
                available_bytes=640 * MiB,
            )

    monkeypatch.setattr(d3d12_adapter, "load_d3d12_adapter", lambda: Adapter())

    selection, plan = select_gpu_backend(
        frame_count=30,
        source_width=3840,
        source_height=2160,
        output_width=15360,
        output_height=7680,
        sample_type="uint16",
        output_sample_bytes=1,
        needs_sdr_conversion=True,
    )

    assert selection.backend == "gpu"
    assert selection.device_name == "Test D3D12"
    assert selection.reason.startswith("D3D12 resident")
    assert plan is not None
    request = captured[0]
    assert getattr(request, "output_workspace_bytes_per_pixel") == 692
    assert getattr(request, "output_workspace_fixed_bytes") == 4096 * 4 + 136 * 65536
    assert getattr(request, "descriptor_count") == 34


@pytest.mark.gpu_contract
def test_neutral_selector_falls_back_or_raises_on_d3d12_preflight(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        d3d12_adapter,
        "load_d3d12_adapter",
        lambda: (_ for _ in ()).throw(D3D12AdapterUnavailableError("no D3D12 adapter")),
    )
    arguments = dict(
        frame_count=1,
        source_width=1,
        source_height=1,
        output_width=1,
        output_height=1,
        sample_type="uint8",
        output_sample_bytes=1,
        needs_sdr_conversion=True,
    )

    selection, plan = select_gpu_backend(**arguments)

    assert selection.backend == "cpu"
    assert selection.reason == "no D3D12 adapter"
    assert plan is None
    with pytest.raises(GpuUnavailableError, match="no D3D12 adapter"):
        select_gpu_backend(**arguments, strict=True)
