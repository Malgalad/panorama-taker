"""Command-line interface for metadata validation and panorama rendering."""

from __future__ import annotations

import argparse
import sys
from fractions import Fraction
from pathlib import Path

from pano_stitch.compositor import (
    estimate_render_resources,
    render_session,
    renderable_session,
    thumbnail_output_path,
    validate_images,
)
from pano_stitch.metadata import load_session


def _progress(completed: int, total: int, phase: str) -> None:
    width = 30
    ratio = completed / total if total else 1.0
    filled = min(width, int(ratio * width))
    bar = "=" * filled + ">" + " " * max(0, width - filled - 1)
    print(
        f"\r{phase}: [{bar}] {ratio * 100:5.1f}% ({completed}/{total})",
        end="",
        file=sys.stderr,
        flush=True,
    )


def _resolution_scale(value: str) -> Fraction:
    try:
        scale = Fraction(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "resolution must be a fraction such as 1/4 or 2/3"
        ) from error
    if scale <= 0 or scale > 1:
        raise argparse.ArgumentTypeError("resolution fraction must be greater than 0 and at most 1")
    return scale


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pano-stitch")
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="validate a capture manifest and its images")
    validate.add_argument("session", type=Path)
    validate.add_argument(
        "--image-dir", type=Path, help="directory containing screenshots relocated from the game"
    )
    validate.add_argument("--allow-incomplete", action="store_true")
    render = commands.add_parser(
        "render", help="render a capture manifest to PNG, JPEG, or OpenEXR"
    )
    render.add_argument("session", type=Path)
    render.add_argument(
        "--image-dir", type=Path, help="directory containing screenshots relocated from the game"
    )
    render.add_argument("--output", required=True, type=Path)
    render.add_argument(
        "--debug-coverage",
        type=Path,
        help="write a white/black PNG showing covered/uncovered output pixels",
    )
    render.add_argument("--width", type=int)
    render.add_argument(
        "--resolution",
        type=_resolution_scale,
        default=Fraction(1, 1),
        help="fraction of normal linear dimensions, for example 1/4 or 2/3",
    )
    render.add_argument("--blend", choices=("hard", "feather"), default="hard")
    render.add_argument(
        "--auto-contrast",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="apply Photoshop-style final SDR auto contrast (default: enabled)",
    )
    render.add_argument(
        "--session-thumbnail",
        action="store_true",
        help="also write a 90-degree thumbnail at the session center",
    )
    render.add_argument(
        "--jpeg-quality",
        type=int,
        default=95,
        help="JPEG quality from 1 to 100; default is 95",
    )
    render.add_argument(
        "--memory-budget-mib",
        type=int,
        default=1024,
        help="maximum compositor working budget; production default is 1024 MiB, cap is 8192 MiB",
    )
    render.add_argument(
        "--workers",
        type=int,
        default=0,
        help="parallel strip workers; 0 selects Auto (default)",
    )
    render.add_argument(
        "--gpu",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="use CUDA when available and the panorama fits VRAM (default: enabled)",
    )
    render.add_argument(
        "--gpu-memory-budget-mib",
        type=int,
        help="maximum VRAM budget; default is available VRAM minus safety reserve",
    )
    render.add_argument("--allow-incomplete", action="store_true")
    return parser


def main() -> None:
    """Validate or render one capture session."""
    arguments = _parser().parse_args()
    try:
        session_path = arguments.session.resolve()
        image_root = (
            arguments.image_dir.resolve()
            if arguments.image_dir is not None
            else session_path.parent
        )
        session = load_session(session_path, image_directory=image_root)
        validate_images(session, image_root, arguments.allow_incomplete)
        if arguments.command == "render":
            session = renderable_session(session, image_root, arguments.allow_incomplete)
            memory_budget_bytes = arguments.memory_budget_mib * 1024 * 1024
            render_width = arguments.width
            if arguments.resolution != 1:
                if render_width is not None:
                    raise ValueError("--width and --resolution cannot be combined")
                full_resources = estimate_render_resources(
                    session, image_root, None, memory_budget_bytes
                )
                render_width = max(1, int(full_resources.output_width * arguments.resolution))
            resources = estimate_render_resources(
                session,
                image_root,
                render_width,
                memory_budget_bytes,
                arguments.workers or None,
            )
            scratch_gib = resources.scratch_bytes / (1024**3)
            print(
                f"render plan: {resources.output_width}x{resources.output_height}, "
                f"{resources.worker_count} workers, {resources.strip_height}-row strips, "
                f"{scratch_gib:.2f} GiB scratch"
            )
            exposure_report = render_session(
                session,
                image_root,
                arguments.output,
                render_width,
                arguments.blend,
                arguments.allow_incomplete,
                memory_budget_bytes,
                _progress,
                arguments.debug_coverage,
                jpeg_quality=arguments.jpeg_quality,
                workers=arguments.workers or None,
                auto_contrast=arguments.auto_contrast,
                session_thumbnail=arguments.session_thumbnail,
                use_gpu=arguments.gpu,
                gpu_memory_budget_bytes=(
                    arguments.gpu_memory_budget_mib * 1024 * 1024
                    if arguments.gpu_memory_budget_mib is not None
                    else None
                ),
            )
            print(file=sys.stderr)
            gains = ", ".join(f"{gain:.3f}" for gain in exposure_report.gains)
            print(
                f"exposure normalization: anchor={exposure_report.anchor_frame + 1}, "
                f"overlap edges={exposure_report.edge_count}, relative exposure estimates=[{gains}]"
            )
            state = "enabled" if arguments.auto_contrast else "disabled"
            if arguments.output.suffix.lower() == ".exr":
                state = "skipped for EXR"
            print(f"auto contrast: {state}")
            print(f"wrote {arguments.output}")
            if arguments.session_thumbnail:
                print(f"wrote {thumbnail_output_path(arguments.output)}")
        else:
            print(f"valid session: {session.session_id}")
    except (OSError, ValueError) as error:
        raise SystemExit(f"error: {error}") from error
