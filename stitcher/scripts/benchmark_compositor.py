"""Compare strict CUDA and CPU stitcher timing on one capture session."""

from __future__ import annotations

import argparse
import time
from importlib import import_module
from pathlib import Path
from typing import Any

from pano_stitch.compositor import render_session, renderable_session, validate_images
from pano_stitch.gpu import (
    CudaRenderDiagnostics,
    GpuUnavailableError,
    compile_cuda_module,
    cuda_device_info,
)
from pano_stitch.metadata import load_session


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--image-dir", type=Path)
    parser.add_argument("--width", type=int)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-output"))
    parser.add_argument("--blend", choices=("hard", "feather"), default="hard")
    parser.add_argument("--disable-auto-contrast", action="store_true")
    args = parser.parse_args()
    if args.runs < 3:
        parser.error("--runs must be at least 3")
    image_dir = (args.image_dir or args.session.parent).resolve()
    session = renderable_session(load_session(args.session, image_directory=image_dir), image_dir)
    validate_images(session, image_dir)
    try:
        device = cuda_device_info()
        print(f"CUDA device: {device.name}; free={device.free_bytes} total={device.total_bytes}")
    except GpuUnavailableError as error:
        raise SystemExit(f"CUDA unavailable: {error}") from error

    args.output_dir.mkdir(parents=True, exist_ok=True)
    cp: Any = import_module("cupy")

    def synchronize_cuda() -> None:
        """Use an event checkpoint so every CUDA timing starts from completed work."""

        completed = cp.cuda.Event()
        completed.record()
        completed.synchronize()
        cp.cuda.runtime.deviceSynchronize()

    compile_cuda_module()
    synchronize_cuda()
    print("CUDA kernels: warmed separately")

    def render(
        label: str, run: str, *, use_gpu: bool
    ) -> tuple[float, CudaRenderDiagnostics | None]:
        diagnostics: list[CudaRenderDiagnostics] = []
        output = args.output_dir / f"{label}-{run}.png"
        synchronize_cuda()
        started = time.perf_counter()
        render_session(
            session,
            image_dir,
            output,
            width=args.width,
            blend=args.blend,
            auto_contrast=not args.disable_auto_contrast,
            use_gpu=use_gpu,
            strict_gpu=use_gpu,
            gpu_diagnostics_callback=diagnostics.append if use_gpu else None,
        )
        synchronize_cuda()
        return time.perf_counter() - started, diagnostics[0] if diagnostics else None

    def audit_cuda(diagnostics: CudaRenderDiagnostics) -> None:
        stats = diagnostics.transfer_stats
        if stats.source_uploads != len(session.frames):
            raise RuntimeError(
                f"CUDA transfer audit failed: expected {len(session.frames)} source uploads, "
                f"got {stats.source_uploads}"
            )
        if stats.disk_scratch_bytes != 0:
            raise RuntimeError(
                "CUDA transfer audit failed: expected no disk scratch, "
                f"got {stats.disk_scratch_bytes}"
            )

    for use_gpu in (False, True):
        label = "cuda" if use_gpu else "cpu"
        warm_seconds, warm_diagnostics = render(label, "warmup", use_gpu=use_gpu)
        print(f"{label}: warm-up={warm_seconds:.3f}s")
        samples: list[float] = []
        diagnostics: CudaRenderDiagnostics | None = warm_diagnostics
        for run in range(args.runs):
            elapsed, diagnostics = render(label, str(run), use_gpu=use_gpu)
            samples.append(elapsed)
        median = sorted(samples)[len(samples) // 2]
        print(f"{label}: median={median:.3f}s min={min(samples):.3f}s max={max(samples):.3f}s")
        if diagnostics is not None:
            audit_cuda(diagnostics)
            phases = ", ".join(
                f"{name}={seconds:.3f}s" for name, seconds in diagnostics.phase_seconds
            )
            print(f"{label}: {phases}")
            print(f"{label}: transfers={diagnostics.transfer_stats}")


if __name__ == "__main__":
    main()
