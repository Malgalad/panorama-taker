"""PyInstaller entry point for the desktop panorama stitcher."""

import sys
import traceback
from collections.abc import Callable
from pathlib import Path

from pano_stitch.gui import main


def _run_probe(result_path: Path, probe: Callable[[], str]) -> None:
    try:
        result = probe()
    except Exception:  # noqa: BLE001 - preserve frozen-runtime diagnostics
        result_path.write_text(traceback.format_exc(), encoding="utf-8")
        raise SystemExit(1) from None
    result_path.write_text(f"{result}\n", encoding="utf-8")


def _verify_cupy_import() -> str:
    import cupy

    return f"CuPy {cupy.__version__} imported successfully"


def _verify_cuda_runtime() -> str:
    from pano_stitch.gpu import compile_cuda_module, cuda_device_info

    device = cuda_device_info()
    compile_cuda_module()
    return (
        f"CUDA kernels compiled on {device.name}; "
        f"free={device.free_bytes} total={device.total_bytes}"
    )


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] in {
        "--verify-cupy-import",
        "--verify-cuda-runtime",
    }:
        probe = (
            _verify_cupy_import if sys.argv[1] == "--verify-cupy-import" else _verify_cuda_runtime
        )
        _run_probe(Path(sys.argv[2]), probe)
    else:
        main()
