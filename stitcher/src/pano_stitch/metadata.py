"""Load and validate capture-session metadata."""

from __future__ import annotations

import json
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


class CaptureMode(StrEnum):
    """Supported camera coverage modes."""

    HORIZONTAL = "horizontal"
    FULL_SPHERE = "full_sphere"


@dataclass(frozen=True)
class FrameMetadata:
    """The pose and file identity recorded for one planned frame."""

    index: int
    filename: str
    yaw_deg: float
    pitch_deg: float
    roll_deg: float
    status: str


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


def _reject_non_standard_number(value: str) -> None:
    raise ValueError(f"non-standard JSON number is not allowed: {value}")


def _default_schema_path() -> Path:
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


def load_session(path: Path, schema_path: Path | None = None) -> SessionMetadata:
    """Load a JSON session and validate it against the shared contract."""

    document = _read_json(path)
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
    )
