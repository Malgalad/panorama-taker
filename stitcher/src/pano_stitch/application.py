"""Tk-independent application contracts for the transitional and native frontends."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from typing import Any, cast

from pano_stitch.compositor import renderable_session, thumbnail_output_path, validate_images
from pano_stitch.metadata import SessionMetadata, load_session


class ApplicationErrorCategory(StrEnum):
    INVALID_INPUT = "invalid_input"
    CANCELLED = "cancelled"
    GPU_UNAVAILABLE = "gpu_unavailable"
    RENDER_FAILED = "render_failed"


class ApplicationOperation(StrEnum):
    VALIDATE = "validate"
    PREVIEW = "preview"
    RENDER = "render"
    MATCH_EXPOSURE = "match_exposure"
    AUTOMATIC_EXPOSURE = "automatic_exposure"


@dataclass(frozen=True)
class BusyTransition:
    previous_state: str
    active_operation: ApplicationOperation


def begin_operation(state: str, operation: str) -> BusyTransition | None:
    if state in {"busy", "closing"}:
        return None
    return BusyTransition(state, ApplicationOperation(operation))


def finish_operation(state: str, previous_state: str) -> str:
    return previous_state if state == "busy" else state


def cancellation_requested(cancel_owner_exists: bool) -> bool:
    return cancel_owner_exists


@dataclass(frozen=True)
class ApplicationEvent:
    kind: str
    payload: object = ""


@dataclass(frozen=True)
class ApplicationSettings:
    game_dir: str = ""
    image_dir: str = ""
    output_dir: str = ""
    stitched_sessions: Mapping[str, object] = field(default_factory=dict)
    auto_contrast: bool = True

    @classmethod
    def from_mapping(cls, value: object) -> ApplicationSettings:
        if not isinstance(value, dict):
            return cls()
        history = value.get("stitched_sessions", {})
        return cls(
            game_dir=str(value.get("game_dir", "")),
            image_dir=str(value.get("image_dir", "")),
            output_dir=str(value.get("output_dir", "")),
            stitched_sessions=history if isinstance(history, dict) else {},
            auto_contrast=cast(bool, value.get("auto_contrast", True)),
        )

    def to_mapping(self) -> dict[str, object]:
        return {
            "game_dir": self.game_dir,
            "image_dir": self.image_dir,
            "output_dir": self.output_dir,
            "stitched_sessions": dict(self.stitched_sessions),
            "auto_contrast": self.auto_contrast,
        }


@dataclass(frozen=True)
class OutputPlan:
    panorama: Path
    coverage: Path | None
    thumbnail: Path | None

    def existing(self) -> tuple[Path, ...]:
        return tuple(path for path in self.paths() if path.exists())

    def paths(self) -> tuple[Path, ...]:
        return (self.panorama,) + tuple(
            path for path in (self.coverage, self.thumbnail) if path is not None
        )


@dataclass(frozen=True)
class ExposureEdits:
    gains: tuple[float, ...]
    target: int | None = None
    selected: frozenset[int] = frozenset()

    def apply_match(self, gain: float) -> ExposureEdits:
        updated = list(self.gains)
        for position in self.selected:
            updated[position] *= gain
        return ExposureEdits(tuple(updated), self.target, self.selected)

    def discard(self) -> ExposureEdits:
        return ExposureEdits((1.0,) * len(self.gains), self.target, self.selected)


def default_output_name(current: str, session_id: str, suffix: str) -> str:
    stripped = current.strip()
    return stripped if stripped and stripped != "panorama.png" else f"panorama-{session_id}{suffix}"


def plan_outputs(
    output_dir: Path,
    output_name: str,
    suffix: str,
    *,
    coverage: bool,
    thumbnail: bool,
) -> OutputPlan:
    panorama = (output_dir / output_name).with_suffix(suffix)
    return OutputPlan(
        panorama,
        panorama.with_name(f"{panorama.stem}-coverage.png") if coverage else None,
        thumbnail_output_path(panorama) if thumbnail else None,
    )


def requested_render_operation(state: str) -> ApplicationOperation | None:
    if state == "ready":
        return ApplicationOperation.PREVIEW
    if state == "preview":
        return ApplicationOperation.RENDER
    return None


def validate_session_request(
    session_path: Path,
    image_dir: Path,
    allow_incomplete: bool,
    cancel_event: Any = None,
) -> SessionMetadata:
    session = load_session(session_path, image_directory=image_dir)
    validate_images(session, image_dir, allow_incomplete, cancel_event)
    return renderable_session(session, image_dir, allow_incomplete)
