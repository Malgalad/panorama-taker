from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from pano_stitch import compositor
from pano_stitch.compositor import (
    CudaSessionCache,
    SourceInfo,
    _render_cuda,
    _render_prepared_cuda,
    prepare_cuda_session,
    render_preview,
    render_session,
)
from pano_stitch.gpu import (
    CudaMemoryPlan,
    CudaOutputJob,
    CudaSession,
    GpuUnavailableError,
    cuda_device_info,
)
from pano_stitch.metadata import CaptureMode, FrameMetadata, ImageEncoding, SessionMetadata
from pano_stitch.planner import plan_shots
from pano_stitch.projection import rectilinear_directions


def _cuda_available() -> bool:
    try:
        cuda_device_info()
    except GpuUnavailableError:
        return False
    return True


pytestmark = pytest.mark.skipif(not _cuda_available(), reason="CUDA device unavailable")


def test_cuda_render_session_writes_final_output_without_scratch(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1,
        "cuda-runtime",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (frame,),
        True,
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    selected: list[tuple[str, str]] = []
    diagnostics = []

    report = render_session(
        session,
        tmp_path,
        tmp_path / "output.png",
        width=16,
        allow_incomplete=True,
        debug_coverage_path=tmp_path / "coverage.png",
        session_thumbnail=True,
        use_gpu=True,
        backend_callback=lambda backend, detail: selected.append((backend, detail)),
        gpu_diagnostics_callback=diagnostics.append,
    )

    assert selected and selected[0][0] == "cuda resident"
    assert report.gains == (1.0,)
    assert (tmp_path / "output.png").is_file()
    assert (tmp_path / "coverage.png").is_file()
    with Image.open(tmp_path / "output-thumbnail.png") as thumbnail:
        assert thumbnail.size == (32, 16)
    assert not list(tmp_path.glob("pano-stitch-*"))
    assert len(diagnostics) == 1
    assert diagnostics[0].transfer_stats.source_uploads == 1
    assert diagnostics[0].transfer_stats.host_to_device_bytes == 32 * 16 * 3
    assert diagnostics[0].transfer_stats.device_to_host_bytes > 0
    assert diagnostics[0].transfer_stats.disk_scratch_bytes == 0
    assert {name for name, _seconds in diagnostics[0].phase_seconds} == {
        "upload",
        "exposure",
        "local_exposure",
        "compositing",
        "conversion_download",
        "encode",
    }


def test_cuda_render_avoids_cpu_image_helpers(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1,
        "cuda-no-cpu-image-helpers",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (frame,),
        True,
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)

    def forbidden(*_args: object, **_kwargs: object) -> None:
        pytest.fail("CUDA renderer invoked a CPU image helper")

    for name in (
        "_estimate_exposure_gains",
        "_composite_strip",
        "_auto_contrast_levels",
        "camera_maps",
        "remap_source",
    ):
        monkeypatch.setattr(compositor, name, forbidden)
    monkeypatch.setattr(compositor.np, "memmap", forbidden)

    render_session(
        session,
        tmp_path,
        tmp_path / "output.png",
        width=16,
        allow_incomplete=True,
        strict_gpu=True,
    )


def test_cuda_adaptive_bands_report_completed_rows(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1,
        "cuda-adaptive-bands",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (frame,),
        True,
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    progress: list[tuple[int, int, str]] = []

    render_session(
        session,
        tmp_path,
        tmp_path / "adaptive.png",
        width=1024,
        allow_incomplete=True,
        use_gpu=True,
        strict_gpu=True,
        progress_callback=lambda completed, total, phase: progress.append(
            (completed, total, phase)
        ),
    )

    conversion_progress = [
        (completed, total)
        for completed, total, phase in progress
        if phase.endswith("CUDA auto contrast")
    ]
    assert conversion_progress == [(256, 512), (512, 512)]


def test_cuda_forced_banded_output_matches_resident_output(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1,
        "cuda-banded-parity",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (frame,),
        True,
    )
    x = np.linspace(0, 255, 32, dtype=np.uint8)
    y = np.linspace(0, 255, 16, dtype=np.uint8)[:, np.newaxis]
    pixels = np.dstack((np.broadcast_to(x, (16, 32)), np.broadcast_to(y, (16, 32)), x ^ y))
    Image.fromarray(pixels, mode="RGB").save(tmp_path / frame.filename)
    source = SourceInfo(32, 16, ImageEncoding("uint8", "srgb", "srgb"))
    cpu_path = tmp_path / "cpu.png"
    resident_path = tmp_path / "resident.png"
    selected: list[tuple[str, str]] = []

    render_session(
        session,
        tmp_path,
        cpu_path,
        width=64,
        blend="feather",
        allow_incomplete=True,
        use_gpu=False,
    )
    render_session(
        session,
        tmp_path,
        resident_path,
        width=64,
        blend="feather",
        allow_incomplete=True,
        use_gpu=True,
        strict_gpu=True,
        backend_callback=lambda backend, detail: selected.append((backend, detail)),
    )
    forced_banded_plan = CudaMemoryPlan(1536, 1024, 4096, 6144, 0, 8, 6656, 10_000_000)
    banded_path = tmp_path / "banded.png"
    _render_cuda(
        session,
        tmp_path,
        banded_path,
        source,
        64,
        32,
        "feather",
        True,
        ".png",
        95,
        True,
        None,
        None,
        None,
        forced_banded_plan,
    )

    assert selected and selected[0][0] == "cuda resident"
    with (
        Image.open(cpu_path) as cpu,
        Image.open(resident_path) as resident,
        Image.open(banded_path) as banded,
    ):
        np.testing.assert_array_equal(np.asarray(resident), np.asarray(cpu))
        np.testing.assert_array_equal(np.asarray(banded), np.asarray(resident))


def test_cuda_preview_returns_pixels_without_a_temporary_image(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1,
        "cuda-preview",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (frame,),
        True,
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    selected: list[tuple[str, str]] = []

    preview = render_preview(
        session,
        tmp_path,
        16,
        ".png",
        allow_incomplete=True,
        use_gpu=True,
        backend_callback=lambda backend, detail: selected.append((backend, detail)),
    )

    assert selected and selected[0][0] == "cuda resident"
    assert preview.pixels.shape == (8, 16, 3)
    assert list(tmp_path.iterdir()) == [tmp_path / frame.filename]


def test_cuda_banded_output_job_copies_multiple_bands() -> None:
    plan = CudaMemoryPlan(12, 1024, 4096, 24, 0, 2, 5132, 10_000_000)
    rotations = np.eye(3, dtype=np.float32).reshape((1, 9))
    with CudaSession(
        frame_count=1,
        source_width=2,
        source_height=2,
        sample_type="uint8",
        rotations=rotations,
        plan=plan,
    ) as session:
        source = np.full((2, 2, 3), 128, dtype=np.uint8)
        session.upload_source(0, source)
        session.finish_uploads()
        exposure = session.solve_exposure_gains(
            latitude_span=180.0,
            horizontal_fov=90.0,
            vertical_fov=90.0,
            transfer_function="srgb",
        )
        with CudaOutputJob(
            session,
            output_width=8,
            output_height=4,
            output_sample_bytes=1,
            needs_sdr_conversion=True,
        ) as job:
            assert job.is_banded
            host = session.pinned_array((4, 8, 3), np.dtype(np.uint8))
            job.build_local_exposure(
                latitude_span=180.0,
                horizontal_fov=90.0,
                vertical_fov=90.0,
                log_gains=exposure.log_gains,
            )
            for row_start in (0, 2):
                job.compose_band(
                    row_start=row_start,
                    rows=2,
                    latitude_span=180.0,
                    horizontal_fov=90.0,
                    vertical_fov=90.0,
                    transfer_function="srgb",
                    hard_blend=True,
                    incomplete_magenta=True,
                )
                job.convert_band(
                    rows=2,
                    transfer_function="srgb",
                    reference_white_nits=100.0,
                    apply_auto_contrast=False,
                )
                job.download_band(host[row_start : row_start + 2], 2, converted=True)

    assert host.shape == (4, 8, 3)
    assert int(host.max()) == 255


def test_cuda_global_exposure_solves_a_two_frame_overlap(tmp_path: Path) -> None:
    frames = (
        FrameMetadata(0, "dark.png", 0.0, 0.0, 0.0, "captured"),
        FrameMetadata(1, "bright.png", 0.0, 0.0, 0.0, "captured"),
    )
    session = SessionMetadata(
        1,
        "cuda-exposure",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        frames,
        True,
    )
    Image.new("RGB", (64, 32), (64, 64, 64)).save(tmp_path / frames[0].filename)
    Image.new("RGB", (64, 32), (128, 128, 128)).save(tmp_path / frames[1].filename)

    report = render_session(
        session,
        tmp_path,
        tmp_path / "output.png",
        width=16,
        allow_incomplete=True,
        use_gpu=True,
    )

    assert report.edge_count == 1
    assert report.gains == pytest.approx((2.0, 0.5), abs=1e-5)


def test_cuda_multi_frame_exposure_matches_cpu_for_shared_scene(tmp_path: Path) -> None:
    planning_session = SessionMetadata(
        1,
        "cuda-shared-scene",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (),
        True,
    )
    frames = tuple(
        FrameMetadata(
            shot.index, f"frame-{shot.index}.png", shot.yaw_deg, shot.pitch_deg, 0.0, "captured"
        )
        for shot in plan_shots(planning_session).shots
    )
    session = SessionMetadata(
        1,
        "cuda-shared-scene",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        frames,
        True,
    )
    for frame in frames:
        directions = rectilinear_directions(
            256, 128, 90.0, 90.0, frame.yaw_deg, frame.pitch_deg, frame.roll_deg
        )
        x, y, z = (directions[..., index] for index in range(3))
        pixels = np.clip(
            np.stack(
                (
                    0.5 + 0.22 * x + 0.11 * np.sin(5.0 * z) + 0.06 * np.cos(7.0 * y),
                    0.5 + 0.20 * y + 0.10 * np.sin(6.0 * x - 2.0 * z),
                    0.5 + 0.18 * z + 0.09 * np.cos(4.0 * x + 3.0 * y),
                ),
                axis=-1,
            ),
            0.04,
            0.96,
        )
        Image.fromarray(np.rint(pixels * 255.0).astype(np.uint8), mode="RGB").save(
            tmp_path / frame.filename
        )

    cpu_report = render_session(
        session, tmp_path, tmp_path / "cpu.png", width=128, auto_contrast=False, use_gpu=False
    )
    selected: list[tuple[str, str]] = []
    cuda_report = render_session(
        session,
        tmp_path,
        tmp_path / "cuda.png",
        width=128,
        auto_contrast=False,
        use_gpu=True,
        strict_gpu=True,
        backend_callback=lambda backend, detail: selected.append((backend, detail)),
    )

    assert selected and selected[0][0] == "cuda resident"
    assert cuda_report.edge_count == cpu_report.edge_count
    assert cuda_report.gains == pytest.approx(cpu_report.gains, abs=5e-5)
    with Image.open(tmp_path / "cpu.png") as cpu, Image.open(tmp_path / "cuda.png") as cuda:
        difference = np.abs(np.asarray(cpu, dtype=np.int16) - np.asarray(cuda, dtype=np.int16))
    assert int(difference.max()) <= 1


def test_prepare_cuda_session_retains_uploaded_sources_for_reuse(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    metadata = SessionMetadata(
        1, "prepared", CaptureMode.FULL_SPHERE, 90.0, 90.0, 0.08, (frame,), True
    )
    Image.new("RGB", (16, 8), (96, 64, 32)).save(tmp_path / frame.filename)
    prepared = prepare_cuda_session(
        metadata,
        tmp_path,
        SourceInfo(16, 8, ImageEncoding("uint8", "srgb", "srgb")),
        CudaMemoryPlan(384, 1024, 4096, 384, 0, None, 5504, 10_000_000),
    )
    try:
        assert prepared.exposure_report.gains == (1.0,)
        assert prepared.cuda_session.transfer_stats.source_uploads == 1
        assert prepared.cuda_session.sources is not None
    finally:
        prepared.close()
    assert prepared.cuda_session.sources is None


def test_prepared_cuda_output_does_not_take_session_ownership(tmp_path: Path) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    metadata = SessionMetadata(
        1, "prepared-output", CaptureMode.FULL_SPHERE, 90.0, 90.0, 0.08, (frame,), True
    )
    Image.new("RGB", (16, 8), (96, 64, 32)).save(tmp_path / frame.filename)
    source = SourceInfo(16, 8, ImageEncoding("uint8", "srgb", "srgb"))
    plan = CudaMemoryPlan(384, 1024, 4096, 384, 0, None, 5504, 10_000_000)
    prepared = prepare_cuda_session(metadata, tmp_path, source, plan)
    try:
        preview = _render_prepared_cuda(
            metadata,
            tmp_path,
            tmp_path / "unused.png",
            source,
            8,
            4,
            "hard",
            True,
            ".png",
            95,
            False,
            None,
            None,
            None,
            plan,
            prepared,
            return_preview=True,
        )
        assert preview.pixels.shape == (4, 8, 3)
        assert prepared.cuda_session.sources is not None
        assert prepared.cuda_session.transfer_stats.source_uploads == 1
    finally:
        prepared.close()


def test_cuda_preview_to_full_render_reuses_prepared_session(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    metadata = SessionMetadata(
        1, "cache-reuse", CaptureMode.FULL_SPHERE, 90.0, 90.0, 0.08, (frame,), True
    )
    Image.new("RGB", (32, 16), (96, 64, 32)).save(tmp_path / frame.filename)
    cache = CudaSessionCache()
    original_prepare = prepare_cuda_session
    preparations = 0

    def counted_prepare(*args: object, **kwargs: object) -> object:
        nonlocal preparations
        preparations += 1
        return original_prepare(*args, **kwargs)

    monkeypatch.setattr(compositor, "prepare_cuda_session", counted_prepare)
    try:
        preview = render_preview(
            metadata,
            tmp_path,
            8,
            ".png",
            allow_incomplete=True,
            cuda_session_cache=cache,
            cuda_session_path=tmp_path / "session.json",
        )
        diagnostics = []
        report = render_session(
            metadata,
            tmp_path,
            tmp_path / "full.png",
            width=16,
            allow_incomplete=True,
            cuda_session_cache=cache,
            cuda_session_path=tmp_path / "session.json",
            gpu_diagnostics_callback=diagnostics.append,
        )
    finally:
        cache.close()

    assert preview.pixels.shape == (4, 8, 3)
    assert report.gains == (1.0,)
    assert preparations == 1
    assert diagnostics[0].transfer_stats.source_uploads == 1


def test_repeated_cuda_jobs_release_device_and_pinned_pools(tmp_path: Path) -> None:
    import cupy as cp

    frame = FrameMetadata(0, "frame.png", 0.0, 0.0, 0.0, "captured")
    session = SessionMetadata(
        1,
        "cuda-pool-cleanup",
        CaptureMode.FULL_SPHERE,
        90.0,
        90.0,
        0.08,
        (frame,),
        True,
    )
    Image.new("RGB", (32, 16), (128, 64, 32)).save(tmp_path / frame.filename)
    device_pool = cp.get_default_memory_pool()
    pinned_pool = cp.get_default_pinned_memory_pool()

    for index in range(3):
        render_session(
            session,
            tmp_path,
            tmp_path / f"output-{index}.png",
            width=16,
            allow_incomplete=True,
            strict_gpu=True,
        )

    assert device_pool.used_bytes() == 0
    assert pinned_pool.n_free_blocks() == 0
