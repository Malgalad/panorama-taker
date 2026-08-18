"""Generate deterministic camera poses for a capture session."""

from __future__ import annotations

import math
from dataclasses import dataclass

from pano_stitch.metadata import CaptureMode, SessionMetadata


@dataclass(frozen=True)
class Shot:
    """One absolute pose relative to the session's panorama origin."""

    index: int
    yaw_deg: float
    pitch_deg: float
    roll_deg: float = 0.0


@dataclass(frozen=True)
class ShotPlan:
    """A complete, deterministic set of camera poses."""

    shots: tuple[Shot, ...]
    yaw_step_deg: float
    pitch_step_deg: float


def _validate_inputs(session: SessionMetadata) -> None:
    if not 0 < session.horizontal_fov_deg < 180:
        raise ValueError("horizontal FoV must be greater than 0 and less than 180 degrees")
    if not 0 < session.vertical_fov_deg < 180:
        raise ValueError("vertical FoV must be greater than 0 and less than 180 degrees")
    if not 0 <= session.overlap_fraction < 1:
        raise ValueError("overlap fraction must be at least 0 and less than 1")


def _axis_count(axis_range_deg: float, fov_deg: float, overlap_fraction: float) -> int:
    desired_step = fov_deg * (1 - overlap_fraction)
    return max(1, math.ceil(axis_range_deg / desired_step))


def plan_shots(session: SessionMetadata) -> ShotPlan:
    """Create absolute yaw/pitch poses with exact angular closure."""

    _validate_inputs(session)
    yaw_count = _axis_count(360.0, session.horizontal_fov_deg, session.overlap_fraction)
    yaw_step = 360.0 / yaw_count

    if session.capture_mode is CaptureMode.HORIZONTAL:
        shots = tuple(
            Shot(index=index, yaw_deg=index * yaw_step, pitch_deg=0.0) for index in range(yaw_count)
        )
        return ShotPlan(shots=shots, yaw_step_deg=yaw_step, pitch_step_deg=0.0)

    pitch_count = _axis_count(180.0, session.vertical_fov_deg, session.overlap_fraction)
    pitch_step = 180.0 / pitch_count
    shots = tuple(
        Shot(
            index=row * yaw_count + column,
            yaw_deg=column * yaw_step,
            pitch_deg=-90.0 + pitch_step / 2.0 + row * pitch_step,
        )
        for row in range(pitch_count)
        for column in range(yaw_count)
    )
    return ShotPlan(shots=shots, yaw_step_deg=yaw_step, pitch_step_deg=pitch_step)
