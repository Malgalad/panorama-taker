import math
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from pano_stitch.compositor import (
    DEFAULT_MEMORY_BUDGET_BYTES,
    MAX_MEMORY_BUDGET_BYTES,
    SourceInfo,
    _choose_strip_height,
    _pq_to_linear,
    _probe_source,
    _write_exr,
    estimate_render_resources,
    render_session,
    validate_images,
)
from pano_stitch.metadata import CaptureMode, FrameMetadata, ImageEncoding, SessionMetadata
from pano_stitch.planner import plan_shots
from pano_stitch.projection import (
    _frame_rotation,
    _rotation_matrix,
    camera_maps,
    equirectangular_directions,
)


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


def test_observed_basis_rows_are_transposed_for_world_to_camera_rotation() -> None:
    frame = FrameMetadata(
        0,
        "frame.png",
        0.0,
        0.0,
        0.0,
        "captured",
        camera_basis_row_major=(0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0),
    )

    local_forward = np.array((1.0, 0.0, 0.0), dtype=np.float32) @ _frame_rotation(frame)

    assert local_forward == pytest.approx((0.0, 0.0, 1.0))


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

    jpeg_path = tmp_path / "panorama.jpg"
    render_session(session, tmp_path, jpeg_path, width=64, blend="hard", jpeg_quality=95)
    with Image.open(jpeg_path) as jpeg_image:
        assert jpeg_image.format == "JPEG"
        assert jpeg_image.size == (64, 32)

    coverage_path = tmp_path / "coverage.png"
    render_session(
        session,
        tmp_path,
        tmp_path / "coverage-render.png",
        width=64,
        blend="hard",
        debug_coverage_path=coverage_path,
    )
    with Image.open(coverage_path) as coverage:
        assert coverage.mode == "L"
        assert np.all(np.asarray(coverage) == 255)

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

    OpenEXR = pytest.importorskip("OpenEXR")
    exr_path = tmp_path / "panorama.exr"
    render_session(session, tmp_path, exr_path, width=64, blend="feather")
    with OpenEXR.File(str(exr_path)) as image:
        exr_result = np.asarray(image.channels()["RGB"].pixels, dtype=np.float32)
    assert exr_result.shape == (32, 64, 3)
    assert np.all(exr_result > 0.0)


def test_pq_decoder_preserves_hdr_domain() -> None:
    encoded = np.array([0.0, 0.5, 1.0], dtype=np.float32)
    decoded = _pq_to_linear(encoded)
    assert decoded[0] == 0.0
    assert 0.0 < decoded[1] < 1.0
    assert decoded[2] == pytest.approx(1.0, abs=1e-6)


def test_4k_source_uses_bounded_output_strips() -> None:
    source = SourceInfo(3840, 2160, ImageEncoding("uint16", "rec2020", "pq", 203.0))
    strip_height = _choose_strip_height(source, 21274, DEFAULT_MEMORY_BUDGET_BYTES)

    assert 1 <= strip_height < 128
    with pytest.raises(ValueError, match="memory budget"):
        _choose_strip_height(source, 21274, MAX_MEMORY_BUDGET_BYTES + 1)


def test_guarded_adaptive_rows_cover_full_sphere() -> None:
    horizontal = 59.229668
    vertical = 35.462
    overlap = 0.08
    guard = 0.05
    yaw_step = horizontal * (1.0 - overlap) * (1.0 - guard)
    pitches = (-72.269, -43.361, -14.454, 14.454, 43.361, 72.269)
    columns = tuple(
        math.ceil(360.0 * math.cos(math.radians(abs(pitch))) / yaw_step) for pitch in pitches
    )

    assert columns == (3, 6, 7, 7, 6, 3)
    directions = equirectangular_directions(512, 256)
    covered = np.zeros((256, 512), dtype=bool)
    for row, (pitch, count) in enumerate(zip(pitches, columns, strict=True)):
        for column in range(count):
            frame = FrameMetadata(row, "", 360.0 * column / count, pitch, 0.0, "captured")
            covered |= camera_maps(directions, frame, 3840, 2160, horizontal, vertical)[2]
    assert np.all(covered)


def test_render_resource_estimate_uses_color_and_weight_scratch(tmp_path: Path) -> None:
    session = _synthetic_session()
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        schema_version=session.schema_version,
        session_id=session.session_id,
        capture_mode=session.capture_mode,
        horizontal_fov_deg=session.horizontal_fov_deg,
        vertical_fov_deg=session.vertical_fov_deg,
        overlap_fraction=session.overlap_fraction,
        frames=(frame,),
        completed=session.completed,
    )
    Image.new("RGB", (64, 64)).save(tmp_path / frame.filename)

    resources = estimate_render_resources(session, tmp_path, width=64)

    assert resources.output_height == 32
    assert resources.scratch_bytes == 64 * 32 * 4 * np.dtype(np.float32).itemsize


def test_exr_round_trip_preserves_values_above_one(tmp_path: Path) -> None:
    OpenEXR = pytest.importorskip("OpenEXR")
    source = np.array([[[0.25, 1.0, 4.0], [8.0, 0.5, 2.0]]], dtype=np.float32)
    output_path = tmp_path / "hdr.exr"
    _write_exr(output_path, source, ImageEncoding("float32", "rec2020", "linear", 203.0))

    assert _probe_source(output_path).width == 2

    with OpenEXR.File(str(output_path)) as image:
        assert str(image.header()["compression"]) == "Compression.PIZ_COMPRESSION"
        result = np.asarray(image.channels()["RGB"].pixels, dtype=np.float32)
    np.testing.assert_allclose(result, source, rtol=1e-6, atol=1e-6)
