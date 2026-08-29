from __future__ import annotations

import numpy as np
from PIL import Image

from pano_stitch.metadata import CaptureMode, FrameMetadata, SessionMetadata


def single_frame_session(
    *,
    session_id: str,
    capture_mode: CaptureMode = CaptureMode.FULL_SPHERE,
    horizontal_fov_deg: float = 90.0,
    vertical_fov_deg: float = 90.0,
) -> SessionMetadata:
    """Build the minimal deterministic captured session used by renderer tests."""

    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    return SessionMetadata(
        1,
        session_id,
        capture_mode,
        horizontal_fov_deg,
        vertical_fov_deg,
        0.08,
        (frame,),
        True,
    )


def write_gradient_source(path: object, *, width: int = 32, height: int = 16) -> None:
    """Write a small RGB source with distinct horizontal and vertical gradients."""

    destination = path
    x = np.linspace(0, 255, width, dtype=np.uint8)
    y = np.linspace(0, 255, height, dtype=np.uint8)[:, np.newaxis]
    pixels = np.dstack(
        (np.broadcast_to(x, (height, width)), np.broadcast_to(y, (height, width)), x ^ y)
    )
    Image.fromarray(pixels, mode="RGB").save(destination)
