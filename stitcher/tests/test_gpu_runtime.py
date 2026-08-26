from __future__ import annotations

import cv2
import numpy as np
import pytest

from pano_stitch.compositor import SourceInfo, _exposure_weight
from pano_stitch.gpu import (
    CudaFrameCompositor,
    CudaResidentCompositor,
    GpuUnavailableError,
    cuda_device_info,
    cuda_remap_source,
)
from pano_stitch.metadata import FrameMetadata, ImageEncoding
from pano_stitch.projection import camera_maps, equirectangular_directions


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


def test_cuda_resident_buffers_upload_once_and_download_rows() -> None:
    compositor = CudaResidentCompositor(2, 4, 5, 6, 7)
    try:
        first = np.full((4, 5, 3), 0.25, dtype=np.float32)
        second = np.full((4, 5, 3), 0.75, dtype=np.float32)
        compositor.upload_source(0, first)
        compositor.upload_source(1, second)
        color, weight = compositor.download_rows(2, 4)
        assert compositor.source_uploads == 2
        assert compositor.row_downloads == 1
        assert compositor.source_upload_bytes == first.nbytes + second.nbytes
        assert compositor.row_download_bytes == color.nbytes + weight.nbytes
        assert color.shape == (2, 7, 3)
        assert weight.shape == (2, 7)
        np.testing.assert_array_equal(color, 0.0)
        np.testing.assert_array_equal(weight, 0.0)
    finally:
        compositor.close()


def test_cuda_resident_exposure_normalization() -> None:
    import cupy as cp

    compositor = CudaResidentCompositor(1, 1, 1, 1, 1)
    try:
        compositor.allocate_exposure(2, 2)
        compositor.exposure_sum[...] = cp.asarray([[2.0, 9.0], [0.0, 4.0]], dtype=cp.float32)
        compositor.exposure_weight[...] = cp.asarray([[2.0, 3.0], [0.0, 2.0]], dtype=cp.float32)
        compositor.normalize_exposure()
        np.testing.assert_allclose(
            cp.asnumpy(compositor.exposure_sum), [[1.0, 3.0], [0.0, 2.0]], atol=1e-6
        )
    finally:
        compositor.close()


def test_cuda_exposure_geometry_matches_cpu_identity() -> None:
    compositor = CudaResidentCompositor(1, 4, 5, 1, 1)
    try:
        compositor.allocate_exposure(3, 4)
        compositor.accumulate_exposure(np.eye(3, dtype=np.float32), 180.0, 90.0, 90.0, 1.0)
        directions = equirectangular_directions(4, 3, 180.0)
        _, _, valid, edge = camera_maps(
            directions,
            FrameMetadata(0, "x", 0.0, 0.0, 0.0, "captured"),
            5,
            4,
            90.0,
            90.0,
        )
        expected = _exposure_weight(valid, edge, SourceInfo(5, 4, ImageEncoding()))
        np.testing.assert_allclose(
            compositor._cp.asnumpy(compositor.exposure_weight), expected, atol=2e-4
        )
        compositor.normalize_exposure()
        normalized = compositor._cp.asnumpy(compositor.exposure_sum)
        np.testing.assert_allclose(normalized[expected > 0.0], 1.0, atol=2e-4)
        np.testing.assert_array_equal(normalized[expected == 0.0], 0.0)
    finally:
        compositor.close()
