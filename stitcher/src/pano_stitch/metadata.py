"""Load and validate capture-session metadata."""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path, PureWindowsPath
from typing import Any

from jsonschema import Draft202012Validator


class CaptureMode(StrEnum):
    """Supported camera coverage modes."""

    HORIZONTAL = "horizontal"
    FULL_SPHERE = "full_sphere"


@dataclass(frozen=True)
class ImageEncoding:
    """Color encoding shared by every source image in a session."""

    sample_type: str = "uint8"
    color_primaries: str = "srgb"
    transfer_function: str = "srgb"
    reference_white_nits: float = 100.0


@dataclass(frozen=True)
class FrameMetadata:
    """The pose and file identity recorded for one planned frame."""

    index: int
    filename: str
    yaw_deg: float
    pitch_deg: float
    roll_deg: float
    status: str
    camera_basis_row_major: tuple[float, ...] | None = None


@dataclass(frozen=True)
class SessionMetadata:
    """Validated metadata needed by the planner and future stitcher."""

    schema_version: int
    session_id: str
    capture_mode: CaptureMode
    horizontal_fov_deg: float
    vertical_fov_deg: float
    overlap_fraction: float
    frames: tuple[FrameMetadata, ...]
    completed: bool
    image_encoding: ImageEncoding = ImageEncoding()


def _reject_non_standard_number(value: str) -> None:
    raise ValueError(f"non-standard JSON number is not allowed: {value}")


def _default_schema_path() -> Path:
    bundled_root = getattr(sys, "_MEIPASS", None)
    if bundled_root is not None:
        return Path(bundled_root) / "contracts" / "session.schema.json"
    package_root = Path(__file__).resolve().parents[3]
    return package_root / "contracts" / "session.schema.json"


def _read_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream, parse_constant=_reject_non_standard_number)
    if not isinstance(value, dict):
        raise ValueError(f"metadata root must be an object: {path}")
    return value


def _validate_document(document: dict[str, Any], schema_path: Path) -> None:
    schema = _read_json(schema_path)
    Draft202012Validator.check_schema(schema)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(document),
        key=lambda error: list(error.path),
    )
    if errors:
        location = ".".join(str(part) for part in errors[0].path) or "root"
        raise ValueError(f"invalid session metadata at {location}: {errors[0].message}")


def _portable_filename(raw_path: str, metadata_path: Path, image_directory: Path | None) -> str:
    """Resolve CET's absolute Windows path, falling back to a moved capture."""

    native_path = Path(raw_path)
    if native_path.is_file():
        return str(native_path)
    filename = PureWindowsPath(raw_path).name
    candidates = []
    if image_directory is not None:
        candidates.append(image_directory / filename)
    candidates.append(metadata_path.parent / filename)
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return filename


def _cet_vector_to_canonical(vector: list[float]) -> tuple[float, float, float]:
    """Convert Cyberpunk's Z-up vectors to the stitcher's Y-up world."""

    if len(vector) != 3:
        raise ValueError("CET camera vector must contain exactly three values")
    return vector[0], vector[2], vector[1]


def _load_cet_session(
    document: dict[str, Any], path: Path, image_directory: Path | None
) -> SessionMetadata:
    state = document.get("state")
    if state not in {"active", "completed", "aborted", "failed"}:
        raise ValueError("CET metadata state must be active, completed, aborted, or failed")
    frames = tuple(
        FrameMetadata(
            index=pose["index"],
            filename=_portable_filename(pose["screenshot_path"], path, image_directory),
            yaw_deg=pose["commanded_yaw_deg"],
            pitch_deg=pose["commanded_pitch_deg"],
            roll_deg=0.0,
            status="captured",
            camera_basis_row_major=tuple(
                (
                    *_cet_vector_to_canonical(pose["right"]),
                    *_cet_vector_to_canonical(pose["up"]),
                    *_cet_vector_to_canonical(pose["forward"]),
                )
            ),
        )
        for pose in document.get("poses", [])
    )
    return SessionMetadata(
        schema_version=document["schema_version"],
        session_id=document["session_id"],
        capture_mode=CaptureMode.FULL_SPHERE,
        horizontal_fov_deg=document["horizontal_fov_deg"],
        vertical_fov_deg=document["vertical_fov_deg"],
        overlap_fraction=0.08,
        frames=frames,
        completed=state == "completed",
        image_encoding=ImageEncoding("uint16", "rec2020", "pq", 203.0),
    )


def load_session(
    path: Path,
    schema_path: Path | None = None,
    image_directory: Path | None = None,
) -> SessionMetadata:
    """Load either the shared contract or a CET capture-session JSON file."""

    document = _read_json(path)
    if "poses" in document and "state" in document:
        return _load_cet_session(document, path, image_directory)
    resolved_schema_path = schema_path or _default_schema_path()
    _validate_document(document, resolved_schema_path)

    frames = tuple(
        FrameMetadata(
            index=frame["index"],
            filename=frame["filename"],
            yaw_deg=frame["yaw_deg"],
            pitch_deg=frame["pitch_deg"],
            roll_deg=frame["roll_deg"],
            status=frame["status"],
            camera_basis_row_major=(
                tuple(frame["camera_basis_row_major"])
                if "camera_basis_row_major" in frame
                else None
            ),
        )
        for frame in document["frames"]
    )
    return SessionMetadata(
        schema_version=document["schema_version"],
        session_id=document["session_id"],
        capture_mode=CaptureMode(document["capture_mode"]),
        horizontal_fov_deg=document["fov"]["horizontal_deg"],
        vertical_fov_deg=document["fov"]["vertical_deg"],
        overlap_fraction=document["planner"]["overlap_fraction"],
        frames=frames,
        completed=document["completed"],
        image_encoding=ImageEncoding(
            sample_type=document.get("image_encoding", {}).get("sample_type", "uint8"),
            color_primaries=document.get("image_encoding", {}).get("color_primaries", "srgb"),
            transfer_function=document.get("image_encoding", {}).get("transfer_function", "srgb"),
            reference_white_nits=document.get("image_encoding", {}).get(
                "reference_white_nits", 100.0
            ),
        ),
    )
