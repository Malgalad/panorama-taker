import logging
import math
from pathlib import Path
from threading import Event
from types import SimpleNamespace

import cv2
import numpy as np
import pytest
from PIL import Image

from pano_stitch import compositor
from pano_stitch.compositor import (
    DEFAULT_MEMORY_BUDGET_BYTES,
    MAX_MEMORY_BUDGET_BYTES,
    CudaSessionCache,
    CudaSessionCacheKey,
    ExposureReport,
    RenderCancelledError,
    SourceInfo,
    _auto_contrast_levels,
    _choose_strip_height,
    _exposure_clipped,
    _output_dimensions,
    _pq_to_linear,
    _probe_source,
    _read_native_source,
    _rec2020_to_srgb_linear,
    _to_sdr_srgb,
    _write_exr,
    cuda_session_cache_key,
    estimate_render_resources,
    estimate_target_exposure_gain,
    prepare_cuda_session,
    render_preview,
    render_session,
    validate_images,
)
from pano_stitch.gpu import (
    BackendSelection,
    CudaMemoryPlan,
    CudaPreflightError,
)
from pano_stitch.metadata import CaptureMode, FrameMetadata, ImageEncoding, SessionMetadata
from pano_stitch.planner import plan_shots
from pano_stitch.projection import (
    _frame_rotation,
    _rotation_matrix,
    camera_maps,
    equirectangular_directions,
    rectilinear_directions,
    remap_source,
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


def test_rectilinear_directions_center_forward_and_strip_matches() -> None:
    whole = rectilinear_directions(5, 3, 90.0, 60.0)
    strip = rectilinear_directions(5, 1, 90.0, 60.0, row_offset=1, full_height=3)

    np.testing.assert_allclose(strip, whole[1:2])
    np.testing.assert_allclose(whole[1, 2], (0.0, 0.0, 1.0), atol=1e-6)
    assert whole[1, 0, 0] < 0.0
    assert whole[1, -1, 0] > 0.0


def test_remap_source_splits_output_wider_than_opencv_limit() -> None:
    source = np.array([[[0.25, 0.5, 0.75]]], dtype=np.float32)
    width = np.iinfo(np.int16).max
    map_x = np.zeros((1, width), dtype=np.float32)
    map_y = np.zeros((1, width), dtype=np.float32)

    sampled = remap_source(source, map_x, map_y)

    assert sampled.shape == (1, width, 3)
    np.testing.assert_array_equal(sampled, np.broadcast_to(source, sampled.shape))


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
    render_session(
        session,
        tmp_path,
        output_path,
        width=64,
        blend="hard",
        auto_contrast=False,
        session_thumbnail=True,
    )

    with Image.open(output_path) as output_image:
        result = np.asarray(output_image.convert("RGB"))
    assert result.shape == (32, 64, 3)
    assert np.all(result > 0)
    with Image.open(tmp_path / "panorama-thumbnail.png") as thumbnail:
        assert thumbnail.size == (64, 64)

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
        [[0.0, 0.0, 1.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, -1.0, 0.0]],
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


def test_image_validation_honors_cancellation(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "cancel-validation", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (8, 8)).save(tmp_path / frame.filename)
    cancelled = Event()
    cancelled.set()

    with pytest.raises(RenderCancelledError, match="render cancelled"):
        validate_images(session, tmp_path, cancel_event=cancelled)


def test_render_preview_returns_sdr_pixels_and_exposure_report(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "preview", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)

    result = render_preview(
        session,
        tmp_path,
        24,
        ".exr",
        allow_incomplete=True,
        auto_contrast=True,
        use_gpu=False,
        cuda_width_multiplier=4,
    )

    assert result.pixels.shape == (4, 24, 3)
    assert result.pixels.dtype == np.uint8
    assert result.exposure_report.gains == (1.0,)
    assert not any(tmp_path.glob("pano-preview-*"))


def test_render_preview_applies_width_multiplier_only_to_selected_cuda(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "cuda-preview-size", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    plan = CudaMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    dimensions: list[tuple[int, int]] = []

    monkeypatch.setattr(
        compositor,
        "select_cuda_backend",
        lambda **_kwargs: (BackendSelection("cuda", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(compositor, "compile_cuda_module", lambda: None)

    def render_cuda(*args: object, **_kwargs: object) -> compositor.PreviewResult:
        width, height = int(args[4]), int(args[5])
        dimensions.append((width, height))
        return compositor.PreviewResult(
            np.zeros((height, width, 3), dtype=np.uint8), ExposureReport(0, 0, (1.0,))
        )

    monkeypatch.setattr(compositor, "_render_cuda", render_cuda)

    result = render_preview(
        session, tmp_path, 24, ".png", allow_incomplete=True, cuda_width_multiplier=4
    )

    assert dimensions == [(96, 16)]
    assert result.pixels.shape == (16, 96, 3)


def test_render_session_reports_selected_cpu_backend(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "backend", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    selected: list[tuple[str, str]] = []
    caplog.set_level(logging.INFO, logger=compositor.__name__)

    render_session(
        session,
        tmp_path,
        tmp_path / "cpu-backend.png",
        width=24,
        allow_incomplete=True,
        use_gpu=False,
        backend_callback=lambda backend, detail: selected.append((backend, detail)),
    )

    assert selected == [("cpu", "GPU acceleration disabled")]
    assert "render backend selected: CPU (GPU acceleration disabled)" in caplog.text


def test_render_session_dispatches_once_to_cpu_pipeline(monkeypatch: pytest.MonkeyPatch) -> None:
    expected = ExposureReport(0, 0, (1.0,))
    calls: list[tuple[object, ...]] = []

    def render_cpu(*args: object) -> ExposureReport:
        calls.append(args)
        return expected

    monkeypatch.setattr(compositor, "_render_cpu", render_cpu)

    actual = render_session(
        SessionMetadata(1, "dispatch", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (), True),
        Path("images"),
        Path("output.png"),
        use_gpu=False,
    )

    assert actual == expected
    assert len(calls) == 1
    assert calls[0][15] is False


def test_cuda_preflight_failure_restarts_once_on_cpu(monkeypatch: pytest.MonkeyPatch) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "preflight", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = CudaMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    expected = ExposureReport(0, 0, (1.0,))
    cpu_calls: list[tuple[object, ...]] = []

    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_cuda_backend",
        lambda **_kwargs: (BackendSelection("cuda", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(compositor, "compile_cuda_module", lambda: None)
    monkeypatch.setattr(
        compositor,
        "_render_cuda",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(CudaPreflightError("upload failed")),
    )
    monkeypatch.setattr(
        compositor,
        "_render_cpu",
        lambda *args: cpu_calls.append(args) or expected,
    )

    actual = render_session(session, Path("images"), Path("output.png"), width=1)

    assert actual == expected
    assert len(cpu_calls) == 1
    assert cpu_calls[0][15] is False


def test_cuda_preflight_failure_is_strict_when_requested(monkeypatch: pytest.MonkeyPatch) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "preflight-strict", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = CudaMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)

    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_cuda_backend",
        lambda **_kwargs: (BackendSelection("cuda", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(compositor, "compile_cuda_module", lambda: None)
    monkeypatch.setattr(
        compositor,
        "_render_cuda",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(CudaPreflightError("upload failed")),
    )
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )

    with pytest.raises(CudaPreflightError, match="upload failed"):
        render_session(session, Path("images"), Path("output.png"), width=1, strict_gpu=True)


def test_native_source_decoder_preserves_png_samples(tmp_path: Path) -> None:
    pixels = np.array([[[3, 257, 65535]]], dtype=np.uint16)
    path = tmp_path / "native.png"
    cv2.imwrite(str(path), pixels[..., ::-1])

    actual = _read_native_source(path, ImageEncoding("uint16", "rec2020", "pq", 203.0))

    assert actual.dtype == np.uint16
    np.testing.assert_array_equal(actual, pixels)


def test_cuda_session_cache_key_tracks_sources_geometry_and_gpu_options(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "cache-key", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    source_path = tmp_path / frame.filename
    Image.new("RGB", (16, 8), (96, 64, 32)).save(source_path)
    kwargs = {
        "device_name": "test GPU",
        "compute_capability": (9, 0),
        "session_path": tmp_path / "session.json",
        "session": session,
        "image_root": tmp_path,
        "gpu_memory_budget_bytes": 1024,
    }

    original = cuda_session_cache_key(**kwargs)
    assert original == cuda_session_cache_key(**kwargs)

    with source_path.open("ab") as source_file:
        source_file.write(b"\0")
    assert original != cuda_session_cache_key(**kwargs)

    changed_budget = {**kwargs, "gpu_memory_budget_bytes": 2048}
    assert original != cuda_session_cache_key(**changed_budget)
    changed_geometry = {
        **kwargs,
        "session": SessionMetadata(
            1, "cache-key", CaptureMode.HORIZONTAL, 91.0, 60.0, 0.08, (frame,), True
        ),
    }
    assert original != cuda_session_cache_key(**changed_geometry)


def test_cuda_session_cache_closes_replaced_and_invalidated_sessions_once() -> None:
    class FakePrepared:
        def __init__(self) -> None:
            self.cuda_session = SimpleNamespace(
                transfer_stats=SimpleNamespace(peak_device_bytes=1234)
            )
            self.close_calls = 0

        def close(self) -> None:
            self.close_calls += 1

    first_key = CudaSessionCacheKey(
        "GPU", (9, 0), "session", (), ImageEncoding(), "horizontal", 90.0, 60.0, 0.08, (), None
    )
    second_key = CudaSessionCacheKey(
        "GPU", (9, 0), "session", (), ImageEncoding(), "horizontal", 91.0, 60.0, 0.08, (), None
    )
    first = FakePrepared()
    second = FakePrepared()
    cache = CudaSessionCache()

    cache.store(first_key, first)  # type: ignore[arg-type]
    assert cache.get(first_key) is first
    assert cache.get(second_key) is None
    cache.store(second_key, second)  # type: ignore[arg-type]
    assert first.close_calls == 1
    cache.invalidate("input changed")
    cache.close()
    assert second.close_calls == 1


def test_render_progress_starts_with_compositing(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "cached-progress", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    preview = render_preview(session, tmp_path, 24, ".png", allow_incomplete=True)
    progress: list[tuple[int, int, str]] = []

    render_session(
        session,
        tmp_path,
        tmp_path / "cached.png",
        width=24,
        allow_incomplete=True,
        exposure_report=preview.exposure_report,
        use_gpu=False,
        progress_callback=lambda completed, total, phase: progress.append(
            (completed, total, phase)
        ),
    )

    assert progress[0][2] == "[1/3] compositing"


def test_prepare_cuda_session_reports_each_source_upload(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frames = tuple(
        FrameMetadata(index, f"frame-{index}.png", 0.0, 0.0, 0.0, "captured") for index in range(2)
    )
    session = SessionMetadata(
        1, "upload-progress", CaptureMode.FULL_SPHERE, 90.0, 90.0, 0.08, frames, True
    )
    for frame in frames:
        Image.new("RGB", (2, 2), (32, 64, 96)).save(tmp_path / frame.filename)

    class FakeCudaSession:
        def __init__(self, **_kwargs: object) -> None:
            self.log_gains = np.zeros(len(frames), dtype=np.float32)

        def pinned_slot(self, _index: int) -> np.ndarray:
            return np.empty(12, dtype=np.uint8)

        def upload_source(self, _frame: int, _slot: np.ndarray, _slot_index: int) -> None:
            pass

        def finish_uploads(self) -> None:
            pass

        def close(self) -> None:
            pass

    monkeypatch.setattr(compositor, "CudaSession", FakeCudaSession)
    progress: list[tuple[int, int, str]] = []
    prepared = prepare_cuda_session(
        session,
        tmp_path,
        SourceInfo(2, 2, ImageEncoding()),
        CudaMemoryPlan(1, 1, 1, 1, 1, None, 1, 1),
        progress_callback=lambda completed, total, phase: progress.append(
            (completed, total, phase)
        ),
    )
    prepared.close()

    assert progress == [
        (0, 2, "loading from disk"),
        (1, 2, "loading from disk"),
        (2, 2, "loading from disk"),
    ]


def test_jpeg_sources_are_supported(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.jpg", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(1, "jpeg", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True)
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename, quality=95)
    validate_images(session, tmp_path)
    assert _probe_source(tmp_path / frame.filename).encoding.transfer_function == "srgb"


def test_allow_incomplete_skips_missing_sources(tmp_path: Path) -> None:
    frames = (
        FrameMetadata(0, "present.png", 0.0, 0.0, 0.0, "captured"),
        FrameMetadata(1, "missing.png", 90.0, 0.0, 0.0, "planned"),
    )
    session = SessionMetadata(1, "partial", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, frames, False)
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / "present.png")
    validate_images(session, tmp_path, allow_incomplete=True)
    render_session(session, tmp_path, tmp_path / "partial.png", width=32, allow_incomplete=True)
    assert (tmp_path / "partial.png").is_file()


def test_pq_decoder_preserves_hdr_domain() -> None:
    encoded = np.array([0.0, 0.5, 1.0], dtype=np.float32)
    decoded = _pq_to_linear(encoded)
    assert decoded[0] == 0.0
    assert 0.0 < decoded[1] < 1.0
    assert decoded[2] == pytest.approx(1.0, abs=1e-6)


def test_rec2020_to_srgb_linear_preserves_neutral_axis() -> None:
    neutral = np.full((1, 1, 3), 0.25, dtype=np.float32)
    converted = _rec2020_to_srgb_linear(neutral)

    np.testing.assert_allclose(converted, neutral, atol=2e-6)


def test_hdr_sdr_conversion_preserves_saturated_highlight_chroma() -> None:
    linear_rec2020 = np.array([[[0.02, 0.005, 0.001]]], dtype=np.float32)
    converted = _to_sdr_srgb(linear_rec2020, ImageEncoding("uint16", "rec2020", "pq", 203.0))
    old_relative = linear_rec2020 * np.float32(10000.0 / 203.0)
    old_converted = compositor._linear_to_srgb(old_relative / (1.0 + old_relative))

    def saturation(rgb: np.ndarray) -> float:
        return float((rgb.max() - rgb.min()) / rgb.max())

    assert saturation(converted[0, 0]) > saturation(old_converted[0, 0])


def test_target_exposure_gain_uses_overlapping_selected_pose(tmp_path: Path) -> None:
    frames = (
        FrameMetadata(0, "bright.png", 0.0, 0.0, 0.0, "captured"),
        FrameMetadata(1, "dim.png", 30.0, 0.0, 0.0, "captured"),
    )
    session = SessionMetadata(
        1, "target-exposure", CaptureMode.FULL_SPHERE, 120.0, 90.0, 0.08, frames, True
    )
    Image.new("RGB", (64, 64), (160, 160, 160)).save(tmp_path / "bright.png")
    Image.new("RGB", (64, 64), (80, 80, 80)).save(tmp_path / "dim.png")

    progress: list[tuple[int, int, str]] = []
    gain = estimate_target_exposure_gain(
        session, tmp_path, 0, (1,), progress_callback=lambda *update: progress.append(update)
    )
    repeated_gain = estimate_target_exposure_gain(session, tmp_path, 0, (1,), (1.0, gain))

    assert gain == pytest.approx(4.38, rel=0.03)
    assert repeated_gain == pytest.approx(1.0, rel=0.03)
    assert (2, 2, "sampling poses") in progress
    assert progress[-1] == (1, 1, "comparing overlaps")

    cancelled = Event()
    cancelled.set()
    with pytest.raises(RenderCancelledError, match="render cancelled"):
        estimate_target_exposure_gain(session, tmp_path, 0, (1,), cancel_event=cancelled)


def test_linear_hdr_highlights_are_not_treated_as_clipped() -> None:
    image = np.full((2, 2, 3), 4.0, dtype=np.float32)
    assert not np.any(_exposure_clipped(image, ImageEncoding("float32", "rec2020", "linear")))
    assert np.all(_exposure_clipped(image, ImageEncoding("uint16", "rec2020", "pq")))


def test_auto_contrast_uses_shared_sdr_levels() -> None:
    values = np.linspace(0.15, 0.85, 1000, dtype=np.float32).reshape((10, 100))
    color = np.stack((values, values * 0.8, values * 0.6), axis=-1)
    weight = np.ones((10, 100), dtype=np.float32)
    levels = _auto_contrast_levels(
        color, weight, 10, 4, ImageEncoding("float32", "srgb", "srgb"), None
    )

    assert levels is not None
    black, white = levels
    assert 0.0 < black < white < 1.0
    neutral = np.full((1, 2, 3), 0.5, dtype=np.float32)
    neutral_result = np.clip((neutral - black) / (white - black), 0.0, 1.0)
    assert np.allclose(neutral_result[..., 0], neutral_result[..., 1])
    assert np.allclose(neutral_result[..., 1], neutral_result[..., 2])


def test_auto_contrast_skips_empty_or_flat_output() -> None:
    color = np.full((4, 8, 3), 0.25, dtype=np.float32)
    weight = np.ones((4, 8), dtype=np.float32)
    assert (
        _auto_contrast_levels(color, weight, 4, 2, ImageEncoding("float32", "srgb", "srgb"), None)
        is None
    )


def test_4k_source_uses_bounded_output_strips() -> None:
    source = SourceInfo(3840, 2160, ImageEncoding("uint16", "rec2020", "pq", 203.0))
    strip_height = _choose_strip_height(source, 21274, DEFAULT_MEMORY_BUDGET_BYTES)

    assert 1 <= strip_height < 256
    with pytest.raises(ValueError, match="memory budget"):
        _choose_strip_height(source, 21274, MAX_MEMORY_BUDGET_BYTES + 1)


def test_uniform_yaw_rows_avoid_low_fov_polar_coverage_gaps() -> None:
    horizontal = 70.599993
    vertical = 43.43203
    overlap = 0.08
    guard = 0.05
    yaw_step = horizontal * (1.0 - overlap) * (1.0 - guard)
    pitches = (-70.021266, -35.010633, 0.0, 35.010633, 70.021266)
    adaptive_columns = tuple(
        math.ceil(360.0 * math.cos(math.radians(abs(pitch))) / yaw_step) for pitch in pitches
    )
    assert adaptive_columns == (2, 5, 6, 5, 2)

    directions = equirectangular_directions(512, 256)
    adaptive_coverage = np.zeros((256, 512), dtype=bool)
    for row, (pitch, count) in enumerate(zip(pitches, adaptive_columns, strict=True)):
        for column in range(count):
            frame = FrameMetadata(row, "", 360.0 * column / count, pitch, 0.0, "captured")
            adaptive_coverage |= camera_maps(directions, frame, 3840, 2160, horizontal, vertical)[2]
    assert np.any(~adaptive_coverage)

    uniform_columns = math.ceil(360.0 / yaw_step)
    assert uniform_columns == 6
    covered = np.zeros((256, 512), dtype=bool)
    for row, pitch in enumerate(pitches):
        for column in range(uniform_columns):
            frame = FrameMetadata(row, "", 360.0 * column / uniform_columns, pitch, 0.0, "captured")
            covered |= camera_maps(directions, frame, 3840, 2160, horizontal, vertical)[2]
    assert np.all(covered)


@pytest.mark.parametrize("aspect", (4 / 3, 16 / 9, 16 / 10, 21 / 9, 32 / 9))
def test_full_sphere_projection_supports_common_display_aspects(aspect: float) -> None:
    vertical = 59.229667664
    horizontal = math.degrees(2.0 * math.atan(math.tan(math.radians(vertical) / 2.0) * aspect))
    base = _synthetic_session()
    session = SessionMetadata(
        schema_version=base.schema_version,
        session_id=f"aspect-{aspect}",
        capture_mode=base.capture_mode,
        horizontal_fov_deg=horizontal,
        vertical_fov_deg=vertical,
        overlap_fraction=base.overlap_fraction,
        frames=(),
        completed=True,
    )
    planned = plan_shots(session)
    directions = equirectangular_directions(256, 128)
    covered = np.zeros((128, 256), dtype=bool)
    source_height = 2160
    source_width = round(source_height * aspect)
    for shot in planned.shots:
        frame = FrameMetadata(
            shot.index, "", shot.yaw_deg, shot.pitch_deg, shot.roll_deg, "captured"
        )
        map_x, map_y, valid, _ = camera_maps(
            directions, frame, source_width, source_height, horizontal, vertical
        )
        assert np.all(np.isfinite(map_x))
        assert np.all(np.isfinite(map_y))
        covered |= valid
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


def test_full_sphere_output_dimensions_are_always_two_to_one() -> None:
    session = _synthetic_session()

    assert _output_dimensions(session, source_width=3840, width=1235) == (1234, 617)


def test_horizontal_output_dimensions_remain_cropped() -> None:
    session = SessionMetadata(
        schema_version=1,
        session_id="horizontal",
        capture_mode=CaptureMode.HORIZONTAL,
        horizontal_fov_deg=90.0,
        vertical_fov_deg=60.0,
        overlap_fraction=0.08,
        frames=(),
        completed=True,
    )

    assert _output_dimensions(session, source_width=3840, width=1235) == (1235, 206)


def test_parallel_strips_match_single_worker_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    base = _synthetic_session()
    frames = tuple(
        FrameMetadata(
            index=shot.index,
            filename=f"frame-{shot.index}.png",
            yaw_deg=shot.yaw_deg,
            pitch_deg=shot.pitch_deg,
            roll_deg=shot.roll_deg,
            status="captured",
        )
        for shot in plan_shots(base).shots
    )
    session = SessionMetadata(
        base.schema_version,
        base.session_id,
        base.capture_mode,
        base.horizontal_fov_deg,
        base.vertical_fov_deg,
        base.overlap_fraction,
        frames,
        base.completed,
    )
    for frame in frames:
        Image.fromarray(_source_image(frame, 64, 64, 90.0)).save(tmp_path / frame.filename)

    source_bytes = 64 * 64 * 3 * np.dtype(np.float32).itemsize * 3
    row_bytes = 64 * 164
    budget = 192 * 1024 * 1024 + source_bytes + 4 * row_bytes
    serial_resources = estimate_render_resources(
        session, tmp_path, width=64, memory_budget_bytes=budget, workers=1
    )
    parallel_resources = estimate_render_resources(
        session, tmp_path, width=64, memory_budget_bytes=budget, workers=2
    )
    assert serial_resources.strip_height == 4
    assert parallel_resources.worker_count == 2
    assert parallel_resources.strip_height == 2

    serial_path = tmp_path / "serial.png"
    parallel_path = tmp_path / "parallel.png"
    render_session(
        session,
        tmp_path,
        serial_path,
        width=64,
        workers=1,
        memory_budget_bytes=budget,
        use_gpu=False,
    )
    opencv_thread_changes: list[int] = []
    monkeypatch.setattr(compositor.cv2, "getNumThreads", lambda: 6)
    monkeypatch.setattr(compositor.cv2, "setNumThreads", opencv_thread_changes.append)
    render_session(
        session,
        tmp_path,
        parallel_path,
        width=64,
        workers=2,
        memory_budget_bytes=budget,
        use_gpu=False,
    )
    assert opencv_thread_changes == [1, 6]
    with Image.open(serial_path) as serial, Image.open(parallel_path) as parallel:
        np.testing.assert_array_equal(np.asarray(serial), np.asarray(parallel))
    gpu_path = tmp_path / "gpu.png"
    render_session(
        session,
        tmp_path,
        gpu_path,
        width=64,
        workers=2,
        memory_budget_bytes=budget,
        use_gpu=True,
    )
    with Image.open(serial_path) as serial, Image.open(gpu_path) as gpu:
        difference = np.abs(np.asarray(serial, dtype=np.int16) - np.asarray(gpu, dtype=np.int16))
        assert int(difference.max()) <= 1


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
