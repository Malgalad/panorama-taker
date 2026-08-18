"""Render validated capture sessions into PNG panoramas."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from numpy.typing import NDArray
from PIL import Image

from pano_stitch.metadata import SessionMetadata
from pano_stitch.projection import camera_maps, equirectangular_directions, remap_source


def _read_rgb(path: Path) -> NDArray[np.uint8]:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"))


def render_session(
    session: SessionMetadata,
    image_root: Path,
    output_path: Path,
    width: int | None = None,
    blend: str = "hard",
    allow_incomplete: bool = False,
) -> None:
    """Render a session using its frame angles and effective FoV."""

    if not session.frames:
        raise ValueError("session contains no frames")
    if blend not in {"hard", "feather"}:
        raise ValueError("blend must be 'hard' or 'feather'")
    sources = [(frame, _read_rgb(image_root / frame.filename)) for frame in session.frames]
    source_height, source_width = sources[0][1].shape[:2]
    if any(image.shape[:2] != (source_height, source_width) for _, image in sources):
        raise ValueError("all source images must have identical dimensions")
    if width is None:
        focal_x = source_width / (2.0 * np.tan(np.radians(session.horizontal_fov_deg) / 2.0))
        width = max(2, int(round(2.0 * np.pi * focal_x)))
    latitude_span = (
        180.0 if session.capture_mode.value == "full_sphere" else session.vertical_fov_deg
    )
    height = max(1, int(round(width * latitude_span / 360.0)))
    directions = equirectangular_directions(width, height, latitude_span)
    panorama = np.zeros((height, width, 3), dtype=np.float32)
    best_weight = np.full((height, width), -np.inf, dtype=np.float32)
    accumulated_weight = np.zeros((height, width, 1), dtype=np.float32)

    for frame, source in sources:
        map_x, map_y, valid, edge_distance = camera_maps(
            directions,
            frame,
            source_width,
            source_height,
            session.horizontal_fov_deg,
            session.vertical_fov_deg,
        )
        sampled = remap_source(source, map_x, map_y).astype(np.float32)
        if blend == "hard":
            selected = valid & (edge_distance > best_weight)
            panorama[selected] = sampled[selected]
            best_weight[selected] = edge_distance[selected]
        else:
            feather_width = max(1.0, min(source_width, source_height) * 0.08)
            weight = np.clip(edge_distance / feather_width, 0.0, 1.0)
            weight *= valid
            weight = weight[..., np.newaxis]
            panorama += sampled * weight
            accumulated_weight += weight

    if blend == "feather":
        covered = accumulated_weight[..., 0] > 0
        panorama[covered] /= accumulated_weight[covered]
    else:
        covered = np.isfinite(best_weight)
    uncovered = int(np.count_nonzero(~covered))
    if uncovered and not allow_incomplete:
        raise ValueError(f"capture does not cover {uncovered} output pixels")
    output = np.clip(panorama, 0, 255).astype(np.uint8)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(output, mode="RGB").save(output_path, format="PNG", compress_level=6)


def validate_images(
    session: SessionMetadata,
    image_root: Path,
    allow_incomplete: bool = False,
) -> None:
    """Validate image references before rendering."""

    if not session.frames:
        return
    if not allow_incomplete and any(frame.status != "captured" for frame in session.frames):
        raise ValueError("session contains frames that are not captured")
    filenames = [Path(frame.filename) for frame in session.frames]
    if any(path.is_absolute() or ".." in path.parts for path in filenames):
        raise ValueError("frame filenames must be relative and stay inside the session directory")
    if len(set(filenames)) != len(filenames):
        raise ValueError("frame filenames must be unique")
    for filename in filenames:
        path = image_root / filename
        if not path.is_file():
            raise ValueError(f"missing source image: {filename}")
        try:
            with Image.open(path) as image:
                image.verify()
        except Exception as error:
            raise ValueError(f"cannot decode source image {filename}: {error}") from error
