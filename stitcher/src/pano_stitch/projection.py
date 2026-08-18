"""Rectilinear-camera to equirectangular projection helpers."""

from __future__ import annotations

import math

import cv2
import numpy as np
from numpy.typing import NDArray

from pano_stitch.metadata import FrameMetadata

FloatArray = NDArray[np.float32]


def _rotation_matrix(yaw_deg: float, pitch_deg: float, roll_deg: float) -> FloatArray:
    """Return local-camera-to-world rotation in the canonical coordinate system."""

    yaw = math.radians(yaw_deg)
    pitch = math.radians(-pitch_deg)
    roll = math.radians(roll_deg)
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    yaw_matrix = np.array([[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]], dtype=np.float32)
    pitch_matrix = np.array([[1.0, 0.0, 0.0], [0.0, cp, -sp], [0.0, sp, cp]], dtype=np.float32)
    roll_matrix = np.array([[cr, -sr, 0.0], [sr, cr, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32)
    return (yaw_matrix @ pitch_matrix @ roll_matrix).astype(np.float32)


def equirectangular_directions(
    width: int,
    height: int,
    latitude_span_deg: float = 180.0,
) -> FloatArray:
    """Create one canonical world direction for every output pixel center."""

    if width < 1 or height < 1:
        raise ValueError("output dimensions must be positive")
    if not 0 < latitude_span_deg <= 180:
        raise ValueError("latitude span must be greater than 0 and at most 180 degrees")
    x = (np.arange(width, dtype=np.float32) + 0.5) / width
    y = (np.arange(height, dtype=np.float32) + 0.5) / height
    longitude = (x - 0.5) * (2.0 * math.pi)
    latitude = (0.5 - y) * math.radians(latitude_span_deg)
    longitude_grid, latitude_grid = np.meshgrid(longitude, latitude)
    cos_latitude = np.cos(latitude_grid)
    return np.stack(
        (
            cos_latitude * np.sin(longitude_grid),
            np.sin(latitude_grid),
            cos_latitude * np.cos(longitude_grid),
        ),
        axis=-1,
    ).astype(np.float32)


def camera_maps(
    directions: FloatArray,
    frame: FrameMetadata,
    source_width: int,
    source_height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[FloatArray, FloatArray, NDArray[np.bool_], FloatArray]:
    """Project world directions into one source frame."""

    rotation = _rotation_matrix(frame.yaw_deg, frame.pitch_deg, frame.roll_deg)
    local = directions @ rotation
    z = local[..., 2]
    safe_z = np.where(np.abs(z) > 1e-8, z, 1.0)
    focal_x = source_width / (2.0 * math.tan(math.radians(horizontal_fov_deg) / 2.0))
    focal_y = source_height / (2.0 * math.tan(math.radians(vertical_fov_deg) / 2.0))
    center_x = (source_width - 1) / 2.0
    center_y = (source_height - 1) / 2.0
    map_x = center_x + focal_x * local[..., 0] / safe_z
    map_y = center_y - focal_y * local[..., 1] / safe_z
    valid = (
        (z > 0)
        & (map_x >= 0)
        & (map_x <= source_width - 1)
        & (map_y >= 0)
        & (map_y <= source_height - 1)
    )
    edge_distance = np.minimum.reduce(
        (map_x, map_y, source_width - 1 - map_x, source_height - 1 - map_y)
    )
    return (
        map_x.astype(np.float32),
        map_y.astype(np.float32),
        valid,
        edge_distance.astype(np.float32),
    )


def remap_source(
    source: NDArray[np.uint8],
    map_x: FloatArray,
    map_y: FloatArray,
) -> NDArray[np.uint8]:
    """Sample a source image at floating-point coordinates."""

    return np.asarray(
        cv2.remap(source, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT),
        dtype=np.uint8,
    )
