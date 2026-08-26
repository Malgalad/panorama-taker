from __future__ import annotations

import cv2
import numpy as np
import pytest

from pano_stitch.gpu import (
    CudaFrameCompositor,
    GpuUnavailableError,
    cuda_device_info,
    cuda_remap_source,
)


def _cuda_available() -> bool:
    try:
        cuda_device_info()
    except GpuUnavailableError:
        return False
    return True


pytestmark = pytest.mark.skipif(not _cuda_available(), reason="CUDA device unavailable")


def test_cuda_remap_matches_opencv() -> None:
    rng = np.random.default_rng(7)
    source = rng.random((9, 11, 3), dtype=np.float32)
    map_x = rng.uniform(0, 10, (7, 13)).astype(np.float32)
    map_y = rng.uniform(0, 8, (7, 13)).astype(np.float32)
    expected = cv2.remap(source, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
    actual = cuda_remap_source(source, map_x, map_y)
    assert np.max(np.abs(expected - actual)) <= np.float32(2e-4)


@pytest.mark.parametrize("hard_blend", (True, False))
def test_cuda_frame_compositor_matches_cpu(hard_blend: bool) -> None:
    rng = np.random.default_rng(3)
    source = rng.random((9, 11, 3), dtype=np.float32)
    map_x = rng.uniform(0, 10, (7, 13)).astype(np.float32)
    map_y = rng.uniform(0, 8, (7, 13)).astype(np.float32)
    valid = np.ones(map_x.shape, dtype=bool)
    candidate = rng.random(map_x.shape, dtype=np.float32)
    correction = rng.uniform(0.5, 1.5, map_x.shape).astype(np.float32)
    color = np.zeros((*map_x.shape, 3), dtype=np.float32)
    weight = np.zeros(map_x.shape, dtype=np.float32)
    CudaFrameCompositor().composite(
        source,
        map_x,
        map_y,
        valid,
        candidate,
        correction,
        color,
        weight,
        11,
        9,
        hard_blend,
    )
    sampled = cv2.remap(source, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
    corrected = sampled * correction[..., np.newaxis]
    expected_color = corrected if hard_blend else corrected * candidate[..., np.newaxis]
    expected_weight = candidate
    np.testing.assert_allclose(color, expected_color, rtol=0.0, atol=2e-4)
    np.testing.assert_allclose(weight, expected_weight, rtol=0.0, atol=2e-4)
