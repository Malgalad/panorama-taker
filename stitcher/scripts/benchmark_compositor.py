"""Compare CPU and CUDA stitcher wall time on an existing capture session."""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from pano_stitch.compositor import render_session, renderable_session, validate_images
from pano_stitch.gpu import GpuUnavailableError, cuda_device_info
from pano_stitch.metadata import load_session


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--image-dir", type=Path)
    parser.add_argument("--width", type=int)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-output"))
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    image_dir = (args.image_dir or args.session.parent).resolve()
    session = renderable_session(load_session(args.session, image_directory=image_dir), image_dir)
    validate_images(session, image_dir)
    try:
        device = cuda_device_info()
        print(f"CUDA device: {device.name}; free={device.free_bytes} total={device.total_bytes}")
    except GpuUnavailableError as error:
        raise SystemExit(f"CUDA unavailable: {error}") from error

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for use_gpu in (False, True):
        label = "cuda" if use_gpu else "cpu"
        output = args.output_dir / f"{label}.png"
        render_session(session, image_dir, output, width=args.width, use_gpu=use_gpu)
        samples: list[float] = []
        for run in range(args.runs):
            output = args.output_dir / f"{label}-{run}.png"
            started = time.perf_counter()
            render_session(session, image_dir, output, width=args.width, use_gpu=use_gpu)
            samples.append(time.perf_counter() - started)
        median = sorted(samples)[len(samples) // 2]
        print(f"{label}: median={median:.3f}s min={min(samples):.3f}s max={max(samples):.3f}s")


if __name__ == "__main__":
    main()
