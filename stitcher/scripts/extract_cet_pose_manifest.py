"""Bind indexed CET POSE_METADATA records to chronologically ordered images."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
from pathlib import Path
from typing import cast

POSE_RE = re.compile(
    r"POSE_METADATA index=(?P<index>\d+)/(?P<count>\d+) row=(?P<row>\d+) "
    r"column=(?P<column>\d+) commanded_yaw=(?P<yaw>[-+0-9.]+) "
    r"commanded_pitch=(?P<pitch>[-+0-9.]+) "
    r"observed_forward=\((?P<forward>[^)]+)\) "
    r"observed_right=\((?P<right>[^)]+)\) "
    r"observed_up=\((?P<up>[^)]+)\) hfov=(?P<hfov>[-+0-9.]+) "
    r"vfov=(?P<vfov>[-+0-9.]+) "
    r"(?:real_frames=\d+ )?"
    r"settle_seconds=(?P<seconds>[-+0-9.]+) "
    r"(?:real_fps=[-+0-9.]+ )?"
    r"basis_valid=(?P<valid>true|false)"
)
IMAGE_EXTENSIONS = {".exr", ".jpeg", ".jpg", ".png"}


def _vector(value: str) -> list[float]:
    return [float(part) for part in value.split(",")]


def _dot(left: list[float], right: list[float]) -> float:
    return sum(a * b for a, b in zip(left, right, strict=True))


def _canonical_basis(record: dict[str, str], base: dict[str, list[float]]) -> list[float]:
    forward = _vector(record["forward"])
    right = _vector(record["right"])
    up = _vector(record["up"])

    def canonical(vector: list[float]) -> list[float]:
        return [
            _dot(vector, base["right"]),
            _dot(vector, base["up"]),
            _dot(vector, base["forward"]),
        ]

    return canonical(right) + canonical(up) + canonical(forward)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("image_directory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start", required=True)
    parser.add_argument("--end", required=True)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--mod-version", required=True)
    parser.add_argument("--created-utc", required=True)
    return parser


def _discover_images(image_directory: Path, start: str, end: str) -> list[str]:
    if not image_directory.is_dir():
        raise ValueError(f"image directory does not exist: {image_directory}")
    images = sorted(
        path.name
        for path in image_directory.iterdir()
        if path.is_file()
        and path.stat().st_size > 0
        and path.suffix.lower() in IMAGE_EXTENSIONS
        and start <= path.name <= end
    )
    if not images:
        raise ValueError(f"no supported screenshots found between {start!r} and {end!r}")
    return images


def _latest_complete_session(lines: list[str]) -> list[dict[str, str]]:
    current: list[dict[str, str]] = []
    complete: list[dict[str, str]] = []
    expected_count = 0
    for line in lines:
        match = POSE_RE.search(line)
        if match is None:
            continue
        record = match.groupdict()
        index = int(record["index"])
        count = int(record["count"])
        if index == 1:
            current = []
            expected_count = count
        if expected_count == count and index == len(current) + 1:
            current.append(record)
            if len(current) == expected_count:
                complete = current
    return complete


def build_document(
    log: Path,
    image_directory: Path,
    start: str,
    end: str,
    session_id: str,
    mod_version: str,
    created_utc: str,
) -> dict[str, object]:
    records = _latest_complete_session(
        log.read_text(encoding="utf-8", errors="replace").splitlines()
    )
    images = _discover_images(image_directory, start, end)
    if len(records) != len(images):
        raise ValueError(
            f"record/image count mismatch: {len(records)} records, {len(images)} images"
        )
    if not records:
        raise ValueError("no complete POSE_METADATA session found")
    first = records[0]
    horizontal_fov = float(first["hfov"])
    vertical_fov = float(first["vfov"])
    first_right = _vector(first["right"])
    horizontal_norm = math.hypot(first_right[0], first_right[1])
    if horizontal_norm < 0.9:
        raise SystemExit("first observed right vector is not a horizontal camera basis")
    base = {
        "forward": [-first_right[1] / horizontal_norm, first_right[0] / horizontal_norm, 0.0],
        "right": [first_right[0] / horizontal_norm, first_right[1] / horizontal_norm, 0.0],
        "up": [0.0, 0.0, 1.0],
    }
    frames = []
    for record, filename in zip(records, images, strict=True):
        frame = {
                "index": int(record["index"]),
                "filename": filename,
                "row": int(record["row"]),
                "column": int(record["column"]),
                "commanded_yaw_deg": float(record["yaw"]),
                "commanded_pitch_deg": float(record["pitch"]),
                "observed_forward": _vector(record["forward"]),
                "observed_right": _vector(record["right"]),
                "observed_up": _vector(record["up"]),
                "camera_basis_row_major": _canonical_basis(record, base),
                "horizontal_fov_deg": float(record["hfov"]),
                "vertical_fov_deg": float(record["vfov"]),
                "settle_seconds": float(record["seconds"]),
                "basis_valid": record["valid"] == "true",
        }
        frames.append(frame)
    return {
        "schema_version": 1,
        "session_id": session_id,
        "game_version": "Cyberpunk 2077",
        "mod_version": mod_version,
        "created_utc": created_utc,
        "capture_mode": "full_sphere",
        "projection": "rectilinear",
        "viewport": {"width": 3840, "height": 2160},
        "image_encoding": {
            "sample_type": "uint16",
            "color_primaries": "rec2020",
            "transfer_function": "pq",
            "reference_white_nits": 203.0,
        },
        "fov": {
            "horizontal_deg": horizontal_fov,
            "vertical_deg": vertical_fov,
            "source": "configured_value",
        },
        "base_pose": {"position": [0.0, 0.0, 0.0], "orientation_xyzw": [0.0, 0.0, 0.0, 1.0]},
        "planner": {
            "overlap_fraction": 0.08,
            "yaw_step_deg": float(first["hfov"]) * 0.92,
            "pitch_step_deg": float(first["vfov"]) * 0.92,
        },
        "frames": [
            {
                "index": cast(int, frame["index"]) - 1,
                "filename": frame["filename"],
                "yaw_deg": frame["commanded_yaw_deg"],
                "pitch_deg": frame["commanded_pitch_deg"],
                "roll_deg": 0.0,
                "camera_basis_row_major": frame["camera_basis_row_major"],
                "observed_forward": frame["observed_forward"],
                "observed_right": frame["observed_right"],
                "observed_up": frame["observed_up"],
                "status": "captured",
            }
            for frame in frames
        ],
        "completed": True,
    }


def _write_atomic(output: Path, document: dict[str, object]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + ".partial")
    with partial.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(document, indent=2) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(partial, output)


def main() -> None:
    arguments = _parser().parse_args()
    try:
        document = build_document(
            arguments.log,
            arguments.image_directory,
            arguments.start,
            arguments.end,
            arguments.session_id,
            arguments.mod_version,
            arguments.created_utc,
        )
        _write_atomic(arguments.output, document)
    except ValueError as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
