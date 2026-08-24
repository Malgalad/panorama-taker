"""Rectilinear-camera to equirectangular projection helpers."""

from __future__ import annotations

import math

import cv2
import numpy as np
from numpy.typing import NDArray

from pano_stitch.metadata import FrameMetadata

FloatArray = NDArray[np.float32]
_MAX_REMAP_DIMENSION = np.iinfo(np.int16).max - 1


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


def _frame_rotation(frame: FrameMetadata) -> FloatArray:
    """Return world-to-camera rotation, preferring observed camera geometry."""

    if frame.camera_basis_row_major is not None:
        if len(frame.camera_basis_row_major) != 9:
            raise ValueError("camera basis must contain exactly nine values")
        camera_to_world = np.asarray(frame.camera_basis_row_major, dtype=np.float32).reshape((3, 3))
        return camera_to_world.T
    return _rotation_matrix(frame.yaw_deg, frame.pitch_deg, frame.roll_deg)


def equirectangular_directions(
    width: int,
    height: int,
    latitude_span_deg: float = 180.0,
    row_offset: int = 0,
    full_height: int | None = None,
) -> FloatArray:
    """Create canonical world directions for one full-width output strip."""

    if width < 1 or height < 1:
        raise ValueError("output dimensions must be positive")
    if not 0 < latitude_span_deg <= 180:
        raise ValueError("latitude span must be greater than 0 and at most 180 degrees")
    if full_height is None:
        full_height = height
    if full_height < 1 or row_offset < 0 or row_offset + height > full_height:
        raise ValueError("strip rows must lie inside the output image")
    x = (np.arange(width, dtype=np.float32) + 0.5) / width
    y = (np.arange(row_offset, row_offset + height, dtype=np.float32) + 0.5) / full_height
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

    rotation = _frame_rotation(frame)
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
        & (map_x >= -0.5)
        & (map_x <= source_width - 0.5)
        & (map_y >= -0.5)
        & (map_y <= source_height - 0.5)
    )
    map_x = np.clip(map_x, 0.0, source_width - 1)
    map_y = np.clip(map_y, 0.0, source_height - 1)
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
    source: FloatArray,
    map_x: FloatArray,
    map_y: FloatArray,
) -> FloatArray:
    """Sample a source image at floating-point coordinates within OpenCV's size limit."""

    if source.shape[0] > _MAX_REMAP_DIMENSION or source.shape[1] > _MAX_REMAP_DIMENSION:
        raise ValueError("source image exceeds OpenCV remap's 32,766-pixel dimension limit")
    height, width = map_x.shape
    if map_y.shape != (height, width):
        raise ValueError("remap coordinate arrays must have matching dimensions")
    if height <= _MAX_REMAP_DIMENSION and width <= _MAX_REMAP_DIMENSION:
        return np.asarray(
            cv2.remap(source, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT),
            dtype=np.float32,
        )

    sampled = np.empty((height, width, source.shape[2]), dtype=np.float32)
    for row_start in range(0, height, _MAX_REMAP_DIMENSION):
        row_end = min(row_start + _MAX_REMAP_DIMENSION, height)
        for column_start in range(0, width, _MAX_REMAP_DIMENSION):
            column_end = min(column_start + _MAX_REMAP_DIMENSION, width)
            sampled[row_start:row_end, column_start:column_end] = cv2.remap(
                source,
                np.ascontiguousarray(map_x[row_start:row_end, column_start:column_end]),
                np.ascontiguousarray(map_y[row_start:row_end, column_start:column_end]),
                cv2.INTER_LINEAR,
                borderMode=cv2.BORDER_CONSTANT,
            )
    return sampled
