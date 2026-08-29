import logging
import math
import re
from pathlib import Path
from threading import Event

import cv2
import numpy as np
import pytest
from conftest import single_frame_session, write_gradient_source
from PIL import Image

from pano_stitch import compositor
from pano_stitch.compositor import (
    DEFAULT_MEMORY_BUDGET_BYTES,
    MAX_MEMORY_BUDGET_BYTES,
    AutomaticExposureAmbiguousError,
    ExposureReport,
    GpuSessionCache,
    RenderCancelledError,
    ResidentSessionBackendIdentity,
    SourceInfo,
    _auto_contrast_levels,
    _choose_strip_height,
    _exposure_clipped,
    _output_dimensions,
    _pq_to_linear,
    _probe_source,
    _read_native_source,
    _rec2020_to_srgb_linear,
    _solve_automatic_exposure,
    _to_sdr_srgb,
    _write_exr,
    d3d12_session_cache_key,
    estimate_automatic_exposure_gains,
    estimate_render_resources,
    estimate_target_exposure_gain,
    gpu_session_cache_key,
    render_preview,
    render_session,
    validate_images,
)
from pano_stitch.gpu import (
    BackendSelection,
    GpuMemoryPlan,
    GpuPreflightError,
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
        gpu_width_multiplier=4,
    )

    assert result.pixels.shape == (4, 24, 3)
    assert result.pixels.dtype == np.uint8
    assert result.exposure_report.gains == (1.0,)
    assert not any(tmp_path.glob("pano-preview-*"))


def test_render_preview_applies_width_multiplier_only_to_selected_gpu(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "gpu-preview-size", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    dimensions: list[tuple[int, int]] = []

    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )

    def render_gpu(*args: object, **_kwargs: object) -> compositor.PreviewResult:
        width, height = int(args[4]), int(args[5])
        dimensions.append((width, height))
        return compositor.PreviewResult(
            np.zeros((height, width, 3), dtype=np.uint8), ExposureReport(0, 0, (1.0,))
        )

    monkeypatch.setattr(compositor, "_render_d3d12_resident", render_gpu)

    result = render_preview(
        session, tmp_path, 24, ".png", allow_incomplete=True, gpu_width_multiplier=4
    )

    assert dimensions == [(96, 16)]
    assert result.pixels.shape == (16, 96, 3)


def test_render_preview_routes_selected_d3d12_to_in_memory_pixels(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-preview", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    expected = compositor.PreviewResult(
        np.zeros((8, 24, 3), dtype=np.uint8), ExposureReport(0, 0, (1.0,))
    )
    calls: list[bool] = []
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **kwargs: calls.append(bool(kwargs["return_preview"])) or expected,
    )
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU preview is forbidden")
    )

    result = render_preview(
        session,
        tmp_path,
        24,
        ".png",
        allow_incomplete=True,
        auto_contrast=False,
        strict_gpu=True,
    )

    assert result is expected
    assert calls == [True]
    assert not list(tmp_path.glob("pano-preview-*"))


def test_d3d12_cache_creates_generation_aware_preview_display(tmp_path: Path) -> None:
    session = single_frame_session(session_id="d3d12-cache")
    Image.new("RGB", (2, 1), (0, 0, 0)).save(tmp_path / "frame.png")
    key = d3d12_session_cache_key(
        device_name="adapter",
        adapter_luid=44,
        session_path=tmp_path / "session.json",
        session=session,
        image_root=tmp_path,
        gpu_memory_budget_bytes=None,
    )
    calls: list[object] = []

    class Token:
        def close(self) -> None:
            calls.append("token_close")

    class Preview:
        def set_generation(self, generation: int) -> None:
            calls.append(("generation", generation))

        def render_base(self, destination: bytearray, **_kwargs: object) -> None:
            destination[:] = bytes(len(destination))
            calls.append("base")

        def render_overlay(self, destination: bytearray, **kwargs: object) -> None:
            destination[:] = bytes(len(destination))
            calls.append(("overlay", kwargs["target_pose"], bytes(kwargs["hovered_frames"])))

        def close(self) -> None:
            calls.append("preview_close")

    class Prepared:
        def create_preview(self, **_kwargs: object) -> Preview:
            calls.append("create_preview")
            return Preview()

        def create_cancellation_token(self) -> Token:
            return Token()

        def close(self) -> None:
            calls.append("prepared_close")

    cache = GpuSessionCache()
    prepared = Prepared()
    cache.store_d3d12(key, prepared)
    assert cache.get_d3d12(key) is prepared
    display = cache.create_preview_display(
        np.zeros((1, 2, 3), dtype=np.uint8),
        np.zeros((1, 2, 3), dtype=np.uint8),
        (np.ones((1, 2), dtype=np.bool_),),
    )

    assert display.render(None, frozenset(), None, False, False).shape == (1, 2, 3)
    assert display.render(None, frozenset({0}), 0, True, True).shape == (1, 2, 3)
    display.close()
    cache.close()

    assert ("generation", 1) in calls and ("generation", 2) in calls
    assert ("overlay", 0, b"\x01") in calls
    assert calls[-3:] == ["preview_close", "token_close", "prepared_close"]


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


def test_gpu_preflight_failure_restarts_once_on_cpu(monkeypatch: pytest.MonkeyPatch) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "preflight", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
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
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(GpuPreflightError("upload failed")),
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


def test_gpu_preflight_failure_is_strict_when_requested(monkeypatch: pytest.MonkeyPatch) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "preflight-strict", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)

    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(GpuPreflightError("upload failed")),
    )
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )

    with pytest.raises(GpuPreflightError, match="upload failed"):
        render_session(session, Path("images"), Path("output.png"), width=1, strict_gpu=True)


def test_gpu_post_dispatch_failure_never_restarts_on_cpu(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "post-dispatch", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)

    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    from pano_stitch.d3d12_adapter import D3D12AdapterUnavailableError

    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            D3D12AdapterUnavailableError("dispatch failed")
        ),
    )
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )

    with pytest.raises(D3D12AdapterUnavailableError, match="dispatch failed"):
        render_session(session, Path("images"), Path("output.png"), width=1)


@pytest.mark.parametrize("strict", (False, True))
def test_d3d12_preflight_failure_falls_back_only_when_not_strict(
    monkeypatch: pytest.MonkeyPatch, strict: bool
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-preflight", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    expected = ExposureReport(0, 0, (1.0,))
    cpu_calls: list[bool] = []
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(GpuPreflightError("device failed")),
    )
    monkeypatch.setattr(
        compositor,
        "_render_cpu",
        lambda *_args: cpu_calls.append(True) or expected,
    )

    if strict:
        with pytest.raises(GpuPreflightError, match="device failed"):
            render_session(
                session,
                Path("images"),
                Path("output.png"),
                width=1,
                auto_contrast=False,
                strict_gpu=True,
            )
        assert not cpu_calls
    else:
        assert (
            render_session(
                session,
                Path("images"),
                Path("output.png"),
                width=1,
                auto_contrast=False,
            )
            == expected
        )
        assert cpu_calls == [True]


def test_d3d12_post_dispatch_failure_never_restarts_on_cpu(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from pano_stitch.d3d12_adapter import D3D12AdapterUnavailableError

    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-dispatch", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            D3D12AdapterUnavailableError("dispatch failed")
        ),
    )
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )

    with pytest.raises(D3D12AdapterUnavailableError, match="dispatch failed"):
        render_session(
            session,
            Path("images"),
            Path("output.png"),
            width=1,
            auto_contrast=False,
        )


@pytest.mark.parametrize("after_dispatch", (False, True))
@pytest.mark.parametrize(
    "message",
    (
        "cannot create D3D12 hard-composite frame buffers",
        "D3D12 device removed (HRESULT 0x887a0005; device reason 0x887a0007)",
    ),
)
def test_d3d12_allocation_failure_fallback_depends_only_on_dispatch_boundary(
    monkeypatch: pytest.MonkeyPatch, after_dispatch: bool, message: str
) -> None:
    from pano_stitch.d3d12_adapter import D3D12AdapterUnavailableError

    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-allocation-boundary", CaptureMode.HORIZONTAL, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    cpu_calls: list[bool] = []
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(1, 1, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (1, 1))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )
    error: Exception = (
        D3D12AdapterUnavailableError(message) if after_dispatch else GpuPreflightError(message)
    )
    monkeypatch.setattr(
        compositor,
        "_render_d3d12_resident",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(error),
    )
    expected = ExposureReport(0, 0, (1.0,))
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: cpu_calls.append(True) or expected
    )

    if after_dispatch:
        with pytest.raises(D3D12AdapterUnavailableError, match=re.escape(message)):
            render_session(session, Path("images"), Path("output.png"), width=1)
        assert cpu_calls == []
    else:
        assert render_session(session, Path("images"), Path("output.png"), width=1) == expected
        assert cpu_calls == [True]


def test_d3d12_thumbnail_uses_retained_rectilinear_render_and_staged_publication(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-thumbnail", CaptureMode.FULL_SPHERE, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    calls: list[tuple[Path, dict[str, object]]] = []
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(4, 2, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (8, 4))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )

    def render(*args: object, **kwargs: object) -> ExposureReport:
        path = args[2]
        assert isinstance(path, Path)
        calls.append((path, kwargs))
        path.write_bytes(b"thumbnail" if kwargs.get("rectilinear_output") else b"panorama")
        return ExposureReport(0, 0, (1.0,))

    monkeypatch.setattr(compositor, "_render_d3d12_resident", render)
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )
    output_path = tmp_path / "output.png"

    render_session(
        session,
        tmp_path,
        output_path,
        width=8,
        auto_contrast=False,
        session_thumbnail=True,
    )

    assert output_path.read_bytes() == b"panorama"
    assert compositor.thumbnail_output_path(output_path).read_bytes() == b"thumbnail"
    assert len(calls) == 2
    assert calls[1][1]["rectilinear_output"] is True
    assert calls[1][1]["output_vertical_fov_degrees"] == pytest.approx(53.130102)


def test_d3d12_thumbnail_failure_preserves_existing_outputs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from pano_stitch.d3d12_adapter import D3D12AdapterUnavailableError

    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-thumbnail-failure", CaptureMode.FULL_SPHERE, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(4, 2, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (8, 4))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )

    def render(*args: object, **kwargs: object) -> ExposureReport:
        path = args[2]
        assert isinstance(path, Path)
        if kwargs.get("rectilinear_output"):
            raise D3D12AdapterUnavailableError("thumbnail failed")
        path.write_bytes(b"new panorama")
        return ExposureReport(0, 0, (1.0,))

    monkeypatch.setattr(compositor, "_render_d3d12_resident", render)
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )
    output_path = tmp_path / "output.png"
    thumbnail_path = compositor.thumbnail_output_path(output_path)
    output_path.write_bytes(b"old panorama")
    thumbnail_path.write_bytes(b"old thumbnail")

    with pytest.raises(D3D12AdapterUnavailableError, match="thumbnail failed"):
        render_session(
            session,
            tmp_path,
            output_path,
            width=8,
            auto_contrast=False,
            session_thumbnail=True,
        )

    assert output_path.read_bytes() == b"old panorama"
    assert thumbnail_path.read_bytes() == b"old thumbnail"
    assert sorted(path.name for path in tmp_path.iterdir()) == sorted(
        (output_path.name, thumbnail_path.name)
    )


@pytest.mark.parametrize("failed_output", ("coverage", "thumbnail", "panorama"))
def test_d3d12_multi_output_publication_rolls_back_every_existing_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failed_output: str
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-publication-failure", CaptureMode.FULL_SPHERE, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(4, 2, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (8, 4))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )

    def render(*args: object, **kwargs: object) -> ExposureReport:
        path = args[2]
        assert isinstance(path, Path)
        if kwargs.get("rectilinear_output"):
            path.write_bytes(b"new thumbnail")
        else:
            path.write_bytes(b"new panorama")
            coverage = kwargs.get("debug_coverage_path")
            assert isinstance(coverage, Path)
            coverage.write_bytes(b"new coverage")
        return ExposureReport(0, 0, (1.0,))

    monkeypatch.setattr(compositor, "_render_d3d12_resident", render)
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )
    output_path = tmp_path / "output.png"
    destinations = {
        "panorama": output_path,
        "thumbnail": compositor.thumbnail_output_path(output_path),
        "coverage": tmp_path / "coverage.png",
    }
    old_contents = {name: f"old {name}".encode() for name in destinations}
    for name, path in destinations.items():
        path.write_bytes(old_contents[name])
    real_replace = compositor.os.replace
    failed = False

    def fail_one_publication(source: object, destination: object) -> None:
        nonlocal failed
        source_path = Path(source)  # type: ignore[arg-type]
        destination_path = Path(destination)  # type: ignore[arg-type]
        if (
            not failed
            and destination_path == destinations[failed_output]
            and source_path.read_bytes().startswith(b"new ")
        ):
            failed = True
            raise OSError("injected publication failure")
        real_replace(source, destination)

    monkeypatch.setattr(compositor.os, "replace", fail_one_publication)

    class Cache:
        invalidations: list[str] = []

        def invalidate(self, reason: str) -> None:
            self.invalidations.append(reason)

    cache = Cache()

    with pytest.raises(OSError, match="injected publication failure"):
        render_session(
            session,
            tmp_path,
            output_path,
            width=8,
            auto_contrast=False,
            session_thumbnail=True,
            debug_coverage_path=destinations["coverage"],
            gpu_session_cache=cache,
        )

    assert failed is True
    assert cache.invalidations == ["render failed"]
    for name, path in destinations.items():
        assert path.read_bytes() == old_contents[name]
    assert sorted(path.name for path in tmp_path.iterdir()) == sorted(
        path.name for path in destinations.values()
    )


def test_d3d12_multi_output_cancellation_removes_stages_and_preserves_outputs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-publication-cancel", CaptureMode.FULL_SPHERE, 90.0, 60.0, 0.08, (frame,), True
    )
    plan = GpuMemoryPlan(1, 1, 1, 1, 1, None, 1, 1)
    cancelled = Event()
    monkeypatch.setattr(
        compositor,
        "_source_info_for_session",
        lambda *_args: SourceInfo(4, 2, ImageEncoding("uint8", "srgb", "srgb")),
    )
    monkeypatch.setattr(compositor, "_output_dimensions", lambda *_args: (8, 4))
    monkeypatch.setattr(
        compositor,
        "select_gpu_backend",
        lambda **_kwargs: (BackendSelection("gpu", "test", "resident", 1, 1, "test"), plan),
    )

    def render(*args: object, **kwargs: object) -> ExposureReport:
        path = args[2]
        assert isinstance(path, Path)
        if kwargs.get("rectilinear_output"):
            path.write_bytes(b"new thumbnail")
            cancelled.set()
        else:
            path.write_bytes(b"new panorama")
            coverage = kwargs.get("debug_coverage_path")
            assert isinstance(coverage, Path)
            coverage.write_bytes(b"new coverage")
        return ExposureReport(0, 0, (1.0,))

    monkeypatch.setattr(compositor, "_render_d3d12_resident", render)
    monkeypatch.setattr(
        compositor, "_render_cpu", lambda *_args: pytest.fail("CPU fallback is forbidden")
    )
    output_path = tmp_path / "output.png"
    destinations = (
        output_path,
        compositor.thumbnail_output_path(output_path),
        tmp_path / "coverage.png",
    )
    for path in destinations:
        path.write_bytes(f"old {path.name}".encode())

    class Cache:
        invalidations: list[str] = []

        def invalidate(self, reason: str) -> None:
            self.invalidations.append(reason)

    cache = Cache()

    with pytest.raises(RenderCancelledError, match="render cancelled"):
        render_session(
            session,
            tmp_path,
            output_path,
            width=8,
            auto_contrast=False,
            session_thumbnail=True,
            debug_coverage_path=destinations[2],
            cancel_event=cancelled,
            gpu_session_cache=cache,
        )

    assert cache.invalidations == ["render cancelled"]
    for path in destinations:
        assert path.read_bytes() == f"old {path.name}".encode()
    assert sorted(path.name for path in tmp_path.iterdir()) == sorted(
        path.name for path in destinations
    )


@pytest.mark.parametrize(
    (
        "suffix",
        "encoding",
        "expected_conversion",
        "floating_point",
        "auto_contrast",
        "band_rows",
        "compose_count",
    ),
    [
        (".png", ImageEncoding("uint8", "srgb", "srgb"), "apply", False, False, None, 1),
        (
            ".jpg",
            ImageEncoding("uint16", "rec2020", "pq", 203.0),
            "tone_map",
            False,
            False,
            None,
            1,
        ),
        (
            ".exr",
            ImageEncoding("float32", "rec2020", "linear", 203.0),
            "copy",
            True,
            False,
            None,
            1,
        ),
        (".png", ImageEncoding("uint8", "srgb", "srgb"), "apply", False, True, 1, 4),
        (".png", ImageEncoding("uint8", "srgb", "srgb"), "apply", False, False, 1024, 3),
        (
            ".png",
            ImageEncoding("uint16", "rec2020", "pq", 203.0),
            "tone_map",
            False,
            True,
            None,
            2,
        ),
    ],
)
def test_d3d12_resident_final_output_routes_conversion_and_staged_write(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    suffix: str,
    encoding: ImageEncoding,
    expected_conversion: str,
    floating_point: bool,
    auto_contrast: bool,
    band_rows: int | None,
    compose_count: int,
) -> None:
    frame = FrameMetadata(0, "frame.source", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1, "d3d12-resident", CaptureMode.FULL_SPHERE, 90.0, 90.0, 0.08, (frame,), True
    )
    calls: list[str] = []
    row_ranges: list[tuple[int, int]] = []
    output_height = 2500 if band_rows == 1024 else (2 if band_rows else 2048)

    class Token:
        handle = None

        def cancel(self) -> None:
            calls.append("cancel")

        def close(self) -> None:
            calls.append("token_close")

    class Output:
        def compose(self, frame_requests: object, **_kwargs: object) -> None:
            calls.append("compose")
            request = tuple(frame_requests)[0]  # type: ignore[arg-type]
            row_ranges.append((request.row_start, request.row_count))

        def prepare_auto_contrast(self) -> None:
            calls.append("prepare_histogram")

        def accumulate_auto_contrast_srgb(self, *, converted: bool = False) -> None:
            calls.append("accumulate_histogram_converted" if converted else "accumulate_histogram")

        def select_auto_contrast_levels(self) -> object:
            return type("Levels", (), {"processed_pixels": 4 * output_height})()

        def apply_auto_contrast_srgb(self, **_kwargs: object) -> None:
            calls.append("apply")

        def quantize_srgb8(self) -> None:
            calls.append("quantize")

        def tone_map_rec2020(self, _nits: float) -> None:
            calls.append("tone_map")

        def convert_tone_mapped_rec2020_to_linear_srgb(self) -> None:
            calls.append("convert_rec2020")

        def copy_linear_float(self) -> None:
            calls.append("copy")

        def download(self, destination: memoryview, **kwargs: object) -> None:
            calls.append("download_float" if kwargs["floating_point"] else "download_srgb8")
            destination.cast("B")[:] = bytes(destination.nbytes)

        def download_coverage(self, destination: memoryview, **_kwargs: object) -> None:
            calls.append("download_coverage")
            destination.cast("B")[:] = bytes([1]) * destination.nbytes

        def close(self) -> None:
            calls.append("output_close")

    class Prepared:
        def create_cancellation_token(self) -> Token:
            return Token()

        def solve_exposure(self, **_kwargs: object) -> None:
            calls.append("solve_exposure")

        def create_output(self, _options: object) -> Output:
            calls.append("create_output")
            return Output()

        def close(self) -> None:
            calls.append("prepared_close")

    class Adapter:
        def probe(self) -> object:
            return type("Device", (), {"luid": 44})()

        def create_cancellation_token(self) -> Token:
            return Token()

        def prepare_session(self, **kwargs: object) -> Prepared:
            assert len(tuple(kwargs["frames"])) == 1  # type: ignore[arg-type]
            calls.append("prepare_session")
            return Prepared()

    monkeypatch.setattr("pano_stitch.d3d12_adapter.load_d3d12_adapter", lambda: Adapter())
    monkeypatch.setattr(
        compositor,
        "_read_native_source",
        lambda *_args: np.zeros((2, 4, 3), dtype=np.dtype(encoding.sample_type)),
    )
    monkeypatch.setattr(
        compositor,
        "_write_exr",
        lambda path, _pixels, _encoding: path.write_bytes(b"exr"),
    )
    output_path = tmp_path / f"output{suffix}"
    coverage_path = (
        tmp_path / "coverage.png"
        if suffix == ".png" and not auto_contrast and band_rows is None
        else None
    )

    report = compositor._render_d3d12_resident(
        session,
        tmp_path,
        output_path,
        SourceInfo(4, 2, encoding),
        4,
        output_height,
        "hard",
        False,
        suffix,
        95,
        auto_contrast,
        None,
        None,
        GpuMemoryPlan(1, 1, 104, 1, 1, band_rows, 1, 1),
        None,
        debug_coverage_path=coverage_path,
    )

    assert output_path.is_file()
    assert report.gains == (1.0,)
    assert calls.count("solve_exposure") == 1
    assert calls.index("compose") < calls.index(expected_conversion)
    assert calls.count("compose") == compose_count
    if band_rows is None:
        expected_ranges = [(0, output_height)]
    else:
        expected_ranges = [
            (row_start, min(band_rows, output_height - row_start))
            for row_start in range(0, output_height, band_rows)
        ]
    if auto_contrast:
        expected_ranges *= 2
    assert row_ranges == expected_ranges
    expected_downloads = len(expected_ranges) // (2 if auto_contrast else 1)
    assert calls.count("download_float" if floating_point else "download_srgb8") == (
        expected_downloads
    )
    if auto_contrast and encoding.transfer_function == "pq":
        assert "accumulate_histogram_converted" in calls
    assert ("download_float" in calls) is floating_point
    assert calls[-3:] == ["output_close", "prepared_close", "token_close"]
    if coverage_path is not None:
        with Image.open(coverage_path) as coverage:
            assert np.all(np.asarray(coverage) == 255)


def test_native_source_decoder_preserves_png_samples(tmp_path: Path) -> None:
    pixels = np.array([[[3, 257, 65535]]], dtype=np.uint16)
    path = tmp_path / "native.png"
    cv2.imwrite(str(path), pixels[..., ::-1])

    actual = _read_native_source(path, ImageEncoding("uint16", "rec2020", "pq", 203.0))

    assert actual.dtype == np.uint16
    np.testing.assert_array_equal(actual, pixels)


def test_gpu_session_cache_key_tracks_sources_geometry_and_gpu_options(tmp_path: Path) -> None:
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

    original = gpu_session_cache_key(**kwargs)
    assert original == gpu_session_cache_key(**kwargs)
    assert original.backend_identity == ResidentSessionBackendIdentity("gpu", 0, 0)

    with source_path.open("ab") as source_file:
        source_file.write(b"\0")
    assert original != gpu_session_cache_key(**kwargs)

    changed_budget = {**kwargs, "gpu_memory_budget_bytes": 2048}
    assert original != gpu_session_cache_key(**changed_budget)
    changed_geometry = {
        **kwargs,
        "session": SessionMetadata(
            1, "cache-key", CaptureMode.HORIZONTAL, 91.0, 60.0, 0.08, (frame,), True
        ),
    }
    assert original != gpu_session_cache_key(**changed_geometry)


def test_resident_session_backend_identity_tracks_backend_adapter_and_abi() -> None:
    original = ResidentSessionBackendIdentity("d3d12", 42, 3)

    assert original != ResidentSessionBackendIdentity("gpu", 42, 3)
    assert original != ResidentSessionBackendIdentity("d3d12", 43, 3)
    assert original != ResidentSessionBackendIdentity("d3d12", 42, 4)


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


def test_automatic_exposure_propagates_from_target_without_clamping(tmp_path: Path) -> None:
    frames = tuple(
        FrameMetadata(position, f"pose-{position}.png", 0.0, 0.0, 0.0, "captured")
        for position in range(5)
    )
    session = SessionMetadata(
        1, "automatic-exposure", CaptureMode.FULL_SPHERE, 120.0, 90.0, 0.08, frames, True
    )
    for position in range(4):
        Image.new("RGB", (64, 64), (160, 160, 160)).save(tmp_path / f"pose-{position}.png")
    Image.new("RGB", (64, 64), (40, 40, 40)).save(tmp_path / "pose-4.png")

    progress: list[tuple[int, int, str]] = []
    result = estimate_automatic_exposure_gains(
        session, tmp_path, 0, progress_callback=lambda *update: progress.append(update)
    )
    repeated = estimate_automatic_exposure_gains(session, tmp_path, 0, result.gains)

    assert result.baseline_positions == (0,)
    assert result.corrected_positions == (4,)
    assert result.gains[4] > 2.0
    assert repeated.corrected_positions == ()
    assert repeated.gains == pytest.approx((1.0,) * 5)
    assert progress[-1] == (4, 4, "propagating exposure")


def test_automatic_exposure_uses_median_from_corrected_neighbors() -> None:
    equations = [
        (0, 1, np.log(2.0), 1.0),
        (0, 2, np.log(4.0), 1.0),
        (1, 3, np.log(3.0), 1.0),
        (2, 3, np.log(1.5), 1.0),
    ]

    result = _solve_automatic_exposure(4, equations, 0)

    assert result.gains == pytest.approx((1.0, 2.0, 4.0, 6.0))


def test_automatic_exposure_accepts_pairwise_aligned_chain() -> None:
    equations = [(position, position + 1, 0.0, 1.0) for position in range(5)]

    result = _solve_automatic_exposure(6, equations, 2)

    assert result.baseline_positions == (2,)
    assert result.corrected_positions == ()
    assert result.gains == (1.0,) * 6


def test_automatic_exposure_rejects_poses_disconnected_from_target() -> None:
    equations = [(0, 1, 0.0, 1.0), (2, 3, 0.0, 1.0)]

    with pytest.raises(AutomaticExposureAmbiguousError, match="disconnected"):
        _solve_automatic_exposure(4, equations, 0)


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


@pytest.mark.gpu_contract
def test_cpu_hard_and_feather_png_jpeg_outputs_are_deterministic(tmp_path: Path) -> None:
    session = single_frame_session(session_id="cpu-output-parity")
    write_gradient_source(tmp_path / session.frames[0].filename)

    for blend in ("hard", "feather"):
        png_path = tmp_path / f"{blend}.png"
        jpeg_path = tmp_path / f"{blend}.jpg"
        render_session(session, tmp_path, png_path, width=32, blend=blend, allow_incomplete=True)
        render_session(session, tmp_path, jpeg_path, width=32, blend=blend, allow_incomplete=True)

        with Image.open(png_path) as png, Image.open(jpeg_path) as jpeg:
            assert png.size == jpeg.size == (32, 16)
            assert png.mode == "RGB"
            assert jpeg.mode == "RGB"
            difference = np.abs(np.asarray(png, dtype=np.int16) - np.asarray(jpeg, dtype=np.int16))
        assert int(difference.max()) <= 20


@pytest.mark.gpu_contract
def test_cpu_pq_rec2020_source_converts_to_sdr_within_one_code_value(tmp_path: Path) -> None:
    source = np.array([[[0, 32768, 65535]]], dtype=np.uint16)
    source_path = tmp_path / "pq.png"
    cv2.imwrite(str(source_path), source[..., ::-1])
    encoding = ImageEncoding("uint16", "rec2020", "pq", 203.0)

    decoded = _read_native_source(source_path, encoding)
    actual = _to_sdr_srgb(
        _pq_to_linear(decoded.astype(np.float32) / 65535.0),
        encoding,
    )
    expected = np.rint(
        _to_sdr_srgb(_pq_to_linear(source.astype(np.float32) / 65535.0), encoding) * 255.0
    ).astype(np.uint8)
    result = np.rint(actual * 255.0).astype(np.uint8)
    difference = result.astype(np.int16) - expected.astype(np.int16)
    assert int(np.abs(difference).max()) <= 1


@pytest.mark.gpu_contract
def test_cpu_incomplete_output_marks_uncovered_pixels_magenta_and_writes_coverage(
    tmp_path: Path,
) -> None:
    session = single_frame_session(session_id="incomplete", capture_mode=CaptureMode.HORIZONTAL)
    Image.new("RGB", (4, 2), (32, 64, 96)).save(tmp_path / session.frames[0].filename)
    output_path = tmp_path / "incomplete.png"
    coverage_path = tmp_path / "coverage.png"

    render_session(
        session,
        tmp_path,
        output_path,
        width=32,
        allow_incomplete=True,
        debug_coverage_path=coverage_path,
        use_gpu=False,
    )

    with Image.open(output_path) as output, Image.open(coverage_path) as coverage:
        output_pixels = np.asarray(output)
        coverage_pixels = np.asarray(coverage)
    uncovered = coverage_pixels == 0
    assert np.any(uncovered)
    assert np.all(output_pixels[uncovered] == np.array((255, 0, 255), dtype=np.uint8))
