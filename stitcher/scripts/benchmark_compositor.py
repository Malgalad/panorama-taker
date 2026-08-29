"""Compare strict D3D12 and CPU stitcher timing on one capture session."""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from pano_stitch.compositor import (
    GpuSessionCache,
    render_preview,
    render_session,
    renderable_session,
    validate_images,
)
from pano_stitch.metadata import load_session


def _median(samples: list[float]) -> float:
    return sorted(samples)[len(samples) // 2]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--image-dir", type=Path)
    parser.add_argument("--width", type=int)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-output"))
    parser.add_argument("--preview-width", type=int)
    parser.add_argument("--blend", choices=("hard", "feather"), default="hard")
    parser.add_argument("--disable-auto-contrast", action="store_true")
    args = parser.parse_args()
    if args.runs < 3:
        parser.error("--runs must be at least 3")
    if args.preview_width is not None and args.preview_width < 1:
        parser.error("--preview-width must be positive")

    image_dir = (args.image_dir or args.session.parent).resolve()
    session = renderable_session(load_session(args.session, image_directory=image_dir), image_dir)
    validate_images(session, image_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for label, use_gpu in (("cpu", False), ("d3d12", True)):
        samples = []
        for run in range(args.runs + 1):
            started = time.perf_counter()
            render_session(
                session,
                image_dir,
                args.output_dir / f"{label}-{run}.png",
                width=args.width,
                blend=args.blend,
                auto_contrast=not args.disable_auto_contrast,
                use_gpu=use_gpu,
                strict_gpu=use_gpu,
            )
            elapsed = time.perf_counter() - started
            if run:
                samples.append(elapsed)
        print(f"{label}: median={_median(samples):.3f}s min={min(samples):.3f}s")

    if args.preview_width is None:
        return
    for label, use_gpu in (("cpu", False), ("d3d12", True)):
        cache = GpuSessionCache() if use_gpu else None
        try:
            samples = []
            for _run in range(args.runs + 1):
                started = time.perf_counter()
                render_preview(
                    session,
                    image_dir,
                    args.preview_width,
                    ".png",
                    blend=args.blend,
                    auto_contrast=not args.disable_auto_contrast,
                    use_gpu=use_gpu,
                    strict_gpu=use_gpu,
                    gpu_session_cache=cache,
                    gpu_session_path=args.session if cache is not None else None,
                )
                samples.append(time.perf_counter() - started)
        finally:
            if cache is not None:
                cache.close()
        print(f"preview {label}: warm-up={samples[0]:.3f}s median={_median(samples[1:]):.3f}s")


if __name__ == "__main__":
    main()
