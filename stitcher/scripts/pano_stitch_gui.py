"""PyInstaller entry point for the desktop panorama stitcher."""

import sys
import traceback
from collections.abc import Callable
from pathlib import Path
from typing import Any

from pano_stitch.gui import main

GPU_RUNTIME_UNAVAILABLE_EXIT = 2
GPU_RUNTIME_FAILURE_EXIT = 3


class _GpuRuntimeUnavailable(RuntimeError):
    pass


def _verify_gpu_runtime(loader: Callable[[], Any] | None = None) -> str:
    from pano_stitch.d3d12_adapter import (
        PANO_GPU_ABI_VERSION,
        D3D12AdapterUnavailableError,
        load_d3d12_adapter,
    )

    adapter = (loader or load_d3d12_adapter)()
    try:
        device = adapter.probe()
    except D3D12AdapterUnavailableError as error:
        raise _GpuRuntimeUnavailable(str(error)) from error
    adapter.verify_runtime()
    name = bytes(device.name).split(b"\0", 1)[0].decode("utf-8", errors="replace")
    return (
        f"D3D12 runtime verified; ABI={PANO_GPU_ABI_VERSION}; adapter={name}; "
        f"vendor=0x{int(device.vendor_id):04x}; device=0x{int(device.device_id):04x}; "
        f"luid=0x{int(device.luid):016x}"
    )


def _run_gpu_runtime_probe(result_path: Path) -> None:
    try:
        result = _verify_gpu_runtime()
    except _GpuRuntimeUnavailable as error:
        result_path.write_text(f"GPU runtime unavailable: {error}\n", encoding="utf-8")
        raise SystemExit(GPU_RUNTIME_UNAVAILABLE_EXIT) from None
    except Exception:  # noqa: BLE001 - preserve frozen-runtime diagnostics
        result_path.write_text(traceback.format_exc(), encoding="utf-8")
        raise SystemExit(GPU_RUNTIME_FAILURE_EXIT) from None
    result_path.write_text(f"{result}\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--verify-gpu-runtime":
        _run_gpu_runtime_probe(Path(sys.argv[2]))
    else:
        main()
