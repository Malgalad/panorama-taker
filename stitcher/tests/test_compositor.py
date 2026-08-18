from pathlib import Path

import numpy as np
from PIL import Image

from pano_stitch.compositor import render_session, validate_images
from pano_stitch.metadata import CaptureMode, FrameMetadata, SessionMetadata
from pano_stitch.planner import plan_shots
from pano_stitch.projection import _rotation_matrix


def _synthetic_session() -> SessionMetadata:
    return SessionMetadata(
        schema_version=1,
        session_id="synthetic",
        capture_mode=CaptureMode.FULL_SPHERE,
        horizontal_fov_deg=90.0,
        vertical_fov_deg=90.0,
        overlap_fraction=0.08,
        frames=(),
        completed=True,
    )


def _source_image(frame: FrameMetadata, width: int, height: int, fov_deg: float) -> np.ndarray:
    x = (np.arange(width, dtype=np.float32) - (width - 1) / 2.0) / (width / 2.0)
    y = ((height - 1) / 2.0 - np.arange(height, dtype=np.float32)) / (height / 2.0)
    local_x, local_y = np.meshgrid(x, y)
    focal = 1.0 / np.tan(np.radians(fov_deg) / 2.0)
    local = np.stack((local_x / focal, local_y / focal, np.ones_like(local_x)), axis=-1)
    local /= np.linalg.norm(local, axis=-1, keepdims=True)
    world = local @ _rotation_matrix(frame.yaw_deg, frame.pitch_deg, frame.roll_deg).T
    return np.clip((world + 1.0) * 110.0 + 20.0, 0, 255).astype(np.uint8)


def test_full_sphere_render_has_coverage_and_expected_directions(tmp_path: Path) -> None:
    base = _synthetic_session()
    planned = plan_shots(base)
    frames = tuple(
        FrameMetadata(
            index=shot.index,
            filename=f"frame-{shot.index}.png",
            yaw_deg=shot.yaw_deg,
            pitch_deg=shot.pitch_deg,
            roll_deg=shot.roll_deg,
            status="captured",
        )
        for shot in planned.shots
    )
    session = SessionMetadata(
        schema_version=base.schema_version,
        session_id=base.session_id,
        capture_mode=base.capture_mode,
        horizontal_fov_deg=base.horizontal_fov_deg,
        vertical_fov_deg=base.vertical_fov_deg,
        overlap_fraction=base.overlap_fraction,
        frames=frames,
        completed=base.completed,
    )
    for frame in frames:
        Image.fromarray(_source_image(frame, 64, 64, 90.0)).save(tmp_path / frame.filename)

    validate_images(session, tmp_path)
    output_path = tmp_path / "panorama.png"
    render_session(session, tmp_path, output_path, width=64, blend="hard")

    with Image.open(output_path) as output_image:
        result = np.asarray(output_image.convert("RGB"))
    assert result.shape == (32, 64, 3)
    assert np.all(result > 0)

    directions = np.array(
        [
            [0.0, 0.0, 1.0],
            [1.0, 0.0, 0.0],
            [0.0, 1.0, 0.0],
            [0.0, -1.0, 0.0],
        ],
        dtype=np.float32,
    )
    expected = np.clip((directions + 1.0) * 110.0 + 20.0, 0, 255)
    pixels = result[[16, 16, 0, 31], [32, 48, 32, 32]].astype(np.float32)
    assert np.max(np.abs(pixels - expected)) < 12.0
