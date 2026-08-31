from __future__ import annotations

import ctypes
import os
from collections.abc import Callable
from pathlib import Path

import pytest

from pano_stitch.d3d12_adapter import (
    PANO_GPU_ABI_VERSION,
    D3D12Adapter,
    D3D12AdapterUnavailableError,
    D3D12OutputBandScheduler,
    D3D12PreparedSession,
    D3D12RenderCancelledError,
    _default_library_path,
    _ExposurePairRequest,
    _ExposureProxyRequest,
    _ExposureReport,
    _ExposureSolveResult,
    _MemoryPlan,
    _MemoryRequest,
    _OneFrameCompositeRequest,
    _OrderedCompositeRequest,
    _OutputCreateOptions,
    _OutputDownloadRequest,
    _PreviewCreateOptions,
    _PreviewOverlayRequest,
    _PreviewRenderRequest,
    _ProbeOptions,
    _SessionCreateOptions,
    _SessionDiagnostics,
    _SourceUpload,
    load_d3d12_adapter,
    run_d3d12_output_bands,
)


def test_frozen_adapter_falls_back_to_executable_sibling(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    executable = tmp_path / "PanoramaCaptureStitcher-Python.exe"
    monkeypatch.setattr(
        "pano_stitch.d3d12_adapter.__file__", str(tmp_path / "internal" / "d3d12_adapter.py")
    )
    monkeypatch.setattr("pano_stitch.d3d12_adapter.sys.frozen", True, raising=False)
    monkeypatch.setattr("pano_stitch.d3d12_adapter.sys.executable", str(executable))

    assert _default_library_path() == tmp_path / "pano_gpu.dll"


@pytest.mark.gpu_contract
def test_d3d12_adapter_imports_and_reports_a_controlled_non_windows_result() -> None:
    if os.name == "nt":
        pytest.skip("native DLL location is exercised by Windows integration tests")

    with pytest.raises(D3D12AdapterUnavailableError, match="only on Windows"):
        load_d3d12_adapter()


@pytest.mark.gpu_contract
def test_session_create_options_match_the_x64_native_abi_layout() -> None:
    assert ctypes.sizeof(_SessionCreateOptions) == 72
    assert _SessionCreateOptions.transfer_function.offset == 24
    assert _SessionCreateOptions.device_luid.offset == 32
    assert _SessionCreateOptions.rotations.offset == 40
    assert _SessionCreateOptions.encoding_metadata.offset == 56


class _FakeNativeFunction:
    def __init__(
        self,
        result: int = 0,
        calls: list[str] | None = None,
        name: str = "",
        action: Callable[..., None] | None = None,
    ) -> None:
        self.argtypes: list[object] | None = None
        self.restype: object | None = None
        self._result = result
        self._calls = calls
        self._name = name
        self._action = action
        self.arguments: list[tuple[object, ...]] = []

    def __call__(self, *args: object) -> int:
        if self._calls is not None:
            self._calls.append(self._name)
        self.arguments.append(args)
        if self._action is not None:
            self._action(*args)
        return self._result


class _FakeNativeLibrary:
    def __init__(self) -> None:
        self.calls: list[str] = []
        for name in (
            "pano_gpu_probe_adapter",
            "pano_gpu_dispatch_self_test",
            "pano_gpu_plan_memory",
            "pano_gpu_cancellation_token_create",
            "pano_gpu_cancellation_token_cancel",
            "pano_gpu_cancellation_token_is_cancelled",
            "pano_gpu_cancellation_token_destroy",
            "pano_gpu_device_create",
            "pano_gpu_session_create",
            "pano_gpu_session_allocate_source",
            "pano_gpu_session_allocate_rotations",
            "pano_gpu_session_upload_rotations",
            "pano_gpu_session_allocate_encoding_metadata",
            "pano_gpu_session_upload_encoding_metadata",
            "pano_gpu_session_allocate_upload_slot",
            "pano_gpu_session_allocate_second_upload_slot",
            "pano_gpu_session_upload_frame_zero",
            "pano_gpu_session_upload_frame_cancellable",
            "pano_gpu_session_finish_uploads",
            "pano_gpu_session_finish_uploads_cancellable",
            "pano_gpu_session_query_diagnostics",
            "pano_gpu_session_build_exposure_proxies",
            "pano_gpu_exposure_pair_count",
            "pano_gpu_session_prepare_exposure_graph",
            "pano_gpu_session_enumerate_exposure_pairs",
            "pano_gpu_session_reduce_exposure_graph",
            "pano_gpu_session_build_exposure_solve_graph",
            "pano_gpu_session_solve_exposure_graph",
            "pano_gpu_session_upload_exposure_gains",
            "pano_gpu_session_query_exposure_report",
            "pano_gpu_output_create_empty",
            "pano_gpu_output_allocate_linear",
            "pano_gpu_output_allocate_coverage",
            "pano_gpu_output_compose_hard_with_inputs",
            "pano_gpu_output_compose_feather_with_inputs",
            "pano_gpu_output_prepare_auto_contrast_histogram",
            "pano_gpu_output_accumulate_auto_contrast_histogram_srgb",
            "pano_gpu_output_accumulate_auto_contrast_histogram_converted_srgb",
            "pano_gpu_output_select_auto_contrast_levels",
            "pano_gpu_output_apply_auto_contrast_srgb",
            "pano_gpu_output_apply_auto_contrast_converted_srgb",
            "pano_gpu_output_quantize_normalized_srgb8",
            "pano_gpu_output_tone_map_rec2020",
            "pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb",
            "pano_gpu_output_copy_linear_float",
            "pano_gpu_output_download_srgb8",
            "pano_gpu_output_download_float",
            "pano_gpu_output_download_coverage",
            "pano_gpu_preview_create",
            "pano_gpu_preview_render_base",
            "pano_gpu_preview_set_generation",
            "pano_gpu_preview_render_overlay_generation",
            "pano_gpu_preview_destroy",
            "pano_gpu_output_destroy",
            "pano_gpu_session_destroy",
            "pano_gpu_device_destroy",
        ):
            setattr(self, name, _FakeNativeFunction(calls=self.calls, name=name))
        self.pano_gpu_abi_version = _FakeNativeFunction(10)
        self.pano_gpu_cancellation_token_create._action = lambda handle, *_args: setattr(
            handle._obj, "value", 303
        )
        self.pano_gpu_device_create._action = lambda _options, handle, *_args: setattr(
            handle._obj, "value", 101
        )
        self.pano_gpu_session_create._action = lambda _device, _options, handle, *_args: setattr(
            handle._obj, "value", 202
        )
        self.pano_gpu_output_create_empty._action = lambda _session, _options, handle, *_args: (
            setattr(handle._obj, "value", 404)
        )
        self.pano_gpu_preview_create._action = lambda _session, _options, handle, *_args: setattr(
            handle._obj, "value", 505
        )
        self.pano_gpu_exposure_pair_count._action = lambda _frames, count, *_args: setattr(
            count._obj, "value", 3
        )

        def populate_solve(_session: object, result: object, *_args: object) -> None:
            result._obj.anchor_frame_index = 0
            result._obj.edge_count = 3
            result._obj.frame_count = 3

        self.pano_gpu_session_solve_exposure_graph._action = populate_solve

        def populate_report(_session: object, report: object, *_args: object) -> None:
            report._obj.anchor_frame_index = 0
            report._obj.edge_count = 3
            report._obj.frame_count = 3
            report._obj.gains_uploaded = 1
            report._obj.solve_count = 1
            report._obj.gain_upload_count = 1

        self.pano_gpu_session_query_exposure_report._action = populate_report


@pytest.mark.gpu_contract
def test_d3d12_adapter_rejects_an_incompatible_native_abi() -> None:
    library = _FakeNativeLibrary()
    library.pano_gpu_abi_version = _FakeNativeFunction(PANO_GPU_ABI_VERSION + 1)

    with pytest.raises(D3D12AdapterUnavailableError, match="ABI version is incompatible"):
        D3D12Adapter(library)  # type: ignore[arg-type]


@pytest.mark.gpu_contract
def test_d3d12_runtime_verification_uses_product_hardware_and_preserves_failure() -> None:
    library = _FakeNativeLibrary()
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]

    adapter.verify_runtime()
    assert library.pano_gpu_dispatch_self_test.arguments[-1][0] == 0

    library.pano_gpu_dispatch_self_test._result = 1
    library.pano_gpu_dispatch_self_test._action = lambda _warp, error, *_args: setattr(
        error, "value", b"injected dispatch failure"
    )
    with pytest.raises(D3D12AdapterUnavailableError, match="injected dispatch failure"):
        adapter.verify_runtime()


@pytest.mark.gpu_contract
def test_d3d12_adapter_declares_retained_session_lifecycle_calls() -> None:
    library = _FakeNativeLibrary()
    D3D12Adapter(library)  # type: ignore[arg-type]

    assert library.pano_gpu_device_create.argtypes == [
        ctypes.POINTER(_ProbeOptions),
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_plan_memory.argtypes == [
        ctypes.POINTER(_MemoryRequest),
        ctypes.POINTER(_MemoryPlan),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_create.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_SessionCreateOptions),
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_query_diagnostics.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_SessionDiagnostics),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_upload_frame_cancellable.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_SourceUpload),
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_finish_uploads_cancellable.argtypes == [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_build_exposure_proxies.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_ExposureProxyRequest),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_reduce_exposure_graph.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_ExposurePairRequest),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_solve_exposure_graph.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_ExposureSolveResult),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_query_exposure_report.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_ExposureReport),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_session_destroy.argtypes == [ctypes.POINTER(ctypes.c_void_p)]
    assert library.pano_gpu_output_create_empty.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_OutputCreateOptions),
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_output_compose_hard_with_inputs.argtypes[1] == ctypes.POINTER(
        _OrderedCompositeRequest
    )
    assert library.pano_gpu_output_download_srgb8.argtypes == [
        ctypes.c_void_p,
        ctypes.POINTER(_OutputDownloadRequest),
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_preview_create.argtypes[1:] == [
        ctypes.POINTER(_PreviewCreateOptions),
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_uint32,
    ]
    assert library.pano_gpu_preview_render_base.argtypes[1] == ctypes.POINTER(_PreviewRenderRequest)
    assert library.pano_gpu_preview_render_overlay_generation.argtypes[1:4] == [
        ctypes.POINTER(_PreviewOverlayRequest),
        ctypes.c_uint64,
        ctypes.c_void_p,
    ]
    assert library.pano_gpu_output_destroy.argtypes == [ctypes.POINTER(ctypes.c_void_p)]
    assert library.pano_gpu_device_destroy.argtypes == [ctypes.POINTER(ctypes.c_void_p)]


@pytest.mark.gpu_contract
def test_d3d12_prepared_session_closes_session_before_device_once() -> None:
    library = _FakeNativeLibrary()
    owner = D3D12PreparedSession(D3D12Adapter(library), 101, 202)  # type: ignore[arg-type]

    owner.close()
    owner.close()

    assert library.calls[-2:] == ["pano_gpu_session_destroy", "pano_gpu_device_destroy"]
    assert library.calls.count("pano_gpu_session_destroy") == 1
    assert library.calls.count("pano_gpu_device_destroy") == 1


@pytest.mark.gpu_contract
def test_d3d12_prepared_session_closes_outputs_before_parent_once() -> None:
    library = _FakeNativeLibrary()
    owner = D3D12PreparedSession(D3D12Adapter(library), 101, 202)  # type: ignore[arg-type]
    first = owner.retain_output(301)
    owner.retain_output(302)

    first.close()
    owner.close()
    owner.close()

    assert library.calls[-4:] == [
        "pano_gpu_output_destroy",
        "pano_gpu_output_destroy",
        "pano_gpu_session_destroy",
        "pano_gpu_device_destroy",
    ]
    assert library.calls.count("pano_gpu_output_destroy") == 2


@pytest.mark.gpu_contract
def test_d3d12_prepared_session_solves_and_reuses_retained_exposure() -> None:
    library = _FakeNativeLibrary()
    owner = D3D12PreparedSession(
        D3D12Adapter(library), 101, 202, frame_count=3, source_width=8, source_height=4
    )  # type: ignore[arg-type]

    first = owner.solve_exposure(
        sample_width=32,
        sample_height=16,
        latitude_span_degrees=90.0,
        horizontal_fov_degrees=80.0,
        vertical_fov_degrees=50.0,
    )
    second = owner.solve_exposure(
        sample_width=1,
        sample_height=1,
        latitude_span_degrees=1.0,
        horizontal_fov_degrees=1.0,
        vertical_fov_degrees=1.0,
    )

    assert library.calls[-9:] == [
        "pano_gpu_session_build_exposure_proxies",
        "pano_gpu_exposure_pair_count",
        "pano_gpu_session_prepare_exposure_graph",
        "pano_gpu_session_enumerate_exposure_pairs",
        "pano_gpu_session_reduce_exposure_graph",
        "pano_gpu_session_build_exposure_solve_graph",
        "pano_gpu_session_solve_exposure_graph",
        "pano_gpu_session_upload_exposure_gains",
        "pano_gpu_session_query_exposure_report",
    ]
    proxy = library.pano_gpu_session_build_exposure_proxies.arguments[0][1]._obj
    pair = library.pano_gpu_session_reduce_exposure_graph.arguments[0][1]._obj
    assert (proxy.frame_count, proxy.source_width, proxy.source_height) == (3, 8, 4)
    assert (pair.sample_width, pair.sample_height) == (32, 16)
    assert pair.latitude_span_degrees == pytest.approx(90.0)
    assert pair.horizontal_fov_degrees == pytest.approx(80.0)
    assert pair.vertical_fov_degrees == pytest.approx(50.0)
    assert first.gains_uploaded == second.gains_uploaded == 1
    assert first.solve_count == second.solve_count == 1
    assert first.gain_upload_count == second.gain_upload_count == 1
    assert first is not second


@pytest.mark.gpu_contract
def test_d3d12_exposure_failure_does_not_upload_gains() -> None:
    library = _FakeNativeLibrary()
    library.pano_gpu_session_reduce_exposure_graph._result = 1
    owner = D3D12PreparedSession(
        D3D12Adapter(library), 101, 202, frame_count=1, source_width=2, source_height=2
    )  # type: ignore[arg-type]

    with pytest.raises(D3D12AdapterUnavailableError, match="cannot reduce"):
        owner.solve_exposure(
            sample_width=8,
            sample_height=4,
            latitude_span_degrees=90.0,
            horizontal_fov_degrees=80.0,
            vertical_fov_degrees=50.0,
        )

    assert "pano_gpu_session_upload_exposure_gains" not in library.calls
    owner.close()
    assert library.calls[-2:] == ["pano_gpu_session_destroy", "pano_gpu_device_destroy"]


def _composite_frame(frame_index: int, row_start: int = 0) -> _OneFrameCompositeRequest:
    return _OneFrameCompositeRequest(
        size=ctypes.sizeof(_OneFrameCompositeRequest),
        abi_version=PANO_GPU_ABI_VERSION,
        frame_index=frame_index,
        source_sample_type=1,
        output_width=4,
        output_height=2,
        row_start=row_start,
        row_count=2,
        latitude_span_degrees=90.0,
        horizontal_fov_degrees=80.0,
        vertical_fov_degrees=50.0,
        world_to_camera=(ctypes.c_float * 9)(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
    )


@pytest.mark.gpu_contract
def test_d3d12_output_job_allocates_composes_converts_and_downloads() -> None:
    library = _FakeNativeLibrary()
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]
    owner = D3D12PreparedSession(adapter, 101, 202, frame_count=2, source_width=2, source_height=2)
    output = owner.create_output(
        _OutputCreateOptions(
            size=ctypes.sizeof(_OutputCreateOptions),
            abi_version=PANO_GPU_ABI_VERSION,
            output_width=4,
            output_height=2,
            output_sample_bytes=1,
            descriptor_count=7,
            output_workspace_bytes=104,
        )
    )
    token = adapter.create_cancellation_token()

    output.compose(
        (_composite_frame(0), _composite_frame(1)),
        feather=False,
        global_gains=(0.5, 2.0),
        local_fields=(0.0, 1.0),
        mark_incomplete=True,
    )
    output.compose(
        (_composite_frame(0), _composite_frame(1)),
        feather=True,
        use_session_exposure_gains=True,
    )
    output.prepare_auto_contrast()
    output.accumulate_auto_contrast_srgb()
    output.accumulate_auto_contrast_srgb(converted=True)
    output.select_auto_contrast_levels()
    output.apply_auto_contrast_srgb(apply_levels=True)
    output.quantize_srgb8()
    srgb = bytearray(24)
    output.download(
        srgb,
        output_width=4,
        row_start=0,
        row_count=2,
        floating_point=False,
        cancellation=token,
    )
    output.tone_map_rec2020(203.0)
    output.convert_tone_mapped_rec2020_to_linear_srgb()
    output.apply_auto_contrast_srgb(apply_levels=False, converted=True)
    output.copy_linear_float()
    linear = bytearray(96)
    output.download(
        linear,
        output_width=4,
        row_start=0,
        row_count=2,
        floating_point=True,
    )
    coverage = bytearray(8)
    output.download_coverage(
        coverage,
        output_width=4,
        row_start=0,
        row_count=2,
        cancellation=token,
    )

    assert library.calls[:3] == [
        "pano_gpu_output_create_empty",
        "pano_gpu_output_allocate_linear",
        "pano_gpu_output_allocate_coverage",
    ]
    hard_arguments = library.pano_gpu_output_compose_hard_with_inputs.arguments[0]
    ordered = hard_arguments[1]._obj
    inputs = hard_arguments[2]._obj
    assert ordered.frame_request_count == 2
    assert [ordered.frame_requests[index].frame_index for index in range(2)] == [0, 1]
    assert inputs.mark_incomplete == 1
    assert inputs.global_gain_bytes == 8
    assert inputs.local_field_bytes == 8
    feather_inputs = library.pano_gpu_output_compose_feather_with_inputs.arguments[0][2]._obj
    assert feather_inputs.use_session_exposure_gains == 1
    srgb_request = library.pano_gpu_output_download_srgb8.arguments[0][1]._obj
    float_request = library.pano_gpu_output_download_float.arguments[0][1]._obj
    coverage_request = library.pano_gpu_output_download_coverage.arguments[0][1]._obj
    assert (srgb_request.output_width, srgb_request.row_start, srgb_request.row_count) == (4, 0, 2)
    assert srgb_request.data_bytes == 24
    assert library.pano_gpu_output_download_srgb8.arguments[0][2].value == 303
    assert float_request.data_bytes == 96
    assert library.pano_gpu_output_download_float.arguments[0][2] is None
    assert coverage_request.data_bytes == 8
    assert library.pano_gpu_output_download_coverage.arguments[0][2].value == 303

    token.close()
    owner.close()


@pytest.mark.gpu_contract
def test_d3d12_output_rejects_bad_download_before_native_submission() -> None:
    library = _FakeNativeLibrary()
    owner = D3D12PreparedSession(D3D12Adapter(library), 101, 202)  # type: ignore[arg-type]
    output = owner.retain_output(404)

    with pytest.raises(ValueError, match="exactly 24"):
        output.download(
            bytearray(23),
            output_width=4,
            row_start=0,
            row_count=2,
            floating_point=False,
        )
    with pytest.raises(ValueError, match="writable"):
        output.download(
            memoryview(bytes(24)),
            output_width=4,
            row_start=0,
            row_count=2,
            floating_point=False,
        )
    with pytest.raises(ValueError, match="exactly 8"):
        output.download_coverage(bytearray(7), output_width=4, row_start=0, row_count=2)

    assert "pano_gpu_output_download_srgb8" not in library.calls


@pytest.mark.gpu_contract
def test_d3d12_output_allocation_failure_destroys_unpublished_output() -> None:
    library = _FakeNativeLibrary()
    library.pano_gpu_output_allocate_coverage._result = 1
    owner = D3D12PreparedSession(D3D12Adapter(library), 101, 202)  # type: ignore[arg-type]

    with pytest.raises(D3D12AdapterUnavailableError, match="allocate_coverage"):
        owner.create_output(
            _OutputCreateOptions(
                size=ctypes.sizeof(_OutputCreateOptions),
                abi_version=PANO_GPU_ABI_VERSION,
                output_width=4,
                output_height=2,
                output_sample_bytes=1,
                descriptor_count=7,
                output_workspace_bytes=104,
            )
        )

    assert library.calls[-1] == "pano_gpu_output_destroy"


@pytest.mark.gpu_contract
def test_d3d12_preview_retains_bytes_renders_base_and_closes_before_parent() -> None:
    library = _FakeNativeLibrary()
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]
    owner = D3D12PreparedSession(adapter, 101, 202)
    token = adapter.create_cancellation_token()
    preview = owner.create_preview(
        frame_count=2,
        preview_width=4,
        preview_height=2,
        preview_rgb8=bytearray(24),
        overview_width=2,
        overview_height=1,
        overview_rgb8=bytearray(6),
        mask_width=2,
        mask_height=1,
        compact_masks=bytearray(4),
    )
    destination = bytearray(6)

    preview.render_base(
        destination,
        crop_left=1,
        crop_top=0,
        crop_width=2,
        crop_height=1,
        use_overview=False,
        cancellation=token,
    )
    preview.set_generation(7)
    preview.render_overlay(
        destination,
        crop_left=1,
        crop_top=0,
        crop_width=2,
        crop_height=1,
        use_overview=False,
        hovered_frames=b"\x00\x01",
        target_pose=1,
        target_mode=True,
        show_boundaries=True,
        generation=7,
        cancellation=token,
    )

    create = library.pano_gpu_preview_create.arguments[0][1]._obj
    render = library.pano_gpu_preview_render_base.arguments[0][1]._obj
    assert (create.preview_rgb8_bytes, create.overview_rgb8_bytes, create.compact_mask_bytes) == (
        24,
        6,
        4,
    )
    assert (render.crop_left, render.crop_width, render.crop_height, render.output_rgb8_bytes) == (
        1,
        2,
        1,
        6,
    )
    assert library.pano_gpu_preview_render_base.arguments[0][2].value == 303
    overlay_arguments = library.pano_gpu_preview_render_overlay_generation.arguments[0]
    overlay = overlay_arguments[1]._obj
    assert (overlay.hovered_frame_bytes, overlay.target_pose, overlay.target_mode) == (2, 1, 1)
    assert overlay.show_boundaries == 1
    assert overlay_arguments[2] == 7
    assert overlay_arguments[3].value == 303
    token.close()
    owner.close()
    assert library.calls[-3:] == [
        "pano_gpu_preview_destroy",
        "pano_gpu_session_destroy",
        "pano_gpu_device_destroy",
    ]


@pytest.mark.gpu_contract
def test_d3d12_closed_prepared_session_rejects_new_output_owner() -> None:
    library = _FakeNativeLibrary()
    owner = D3D12PreparedSession(D3D12Adapter(library), 101, 202)  # type: ignore[arg-type]
    owner.close()

    with pytest.raises(RuntimeError, match="prepared session is closed"):
        owner.retain_output(301)


@pytest.mark.gpu_contract
def test_d3d12_prepare_session_uploads_in_native_order_and_publishes_owner() -> None:
    library = _FakeNativeLibrary()
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]
    frames = (bytes(range(12)), bytes(range(12, 24)), bytes(range(24, 36)))

    owner = adapter.prepare_session(
        adapter_luid=44,
        rotations=tuple(float(index) for index in range(27)),
        frames=frames,
        source_width=2,
        source_height=2,
        source_sample_type=1,
        transfer_function=1,
        source_row_stride_bytes=6,
        encoding_metadata=b"meta",
    )

    expected_order = [
        "pano_gpu_cancellation_token_create",
        "pano_gpu_device_create",
        "pano_gpu_session_create",
        "pano_gpu_session_allocate_source",
        "pano_gpu_session_allocate_rotations",
        "pano_gpu_session_upload_rotations",
        "pano_gpu_session_allocate_encoding_metadata",
        "pano_gpu_session_upload_encoding_metadata",
        "pano_gpu_session_allocate_upload_slot",
        "pano_gpu_session_allocate_second_upload_slot",
        "pano_gpu_session_upload_frame_zero",
        "pano_gpu_session_upload_frame_cancellable",
        "pano_gpu_session_upload_frame_cancellable",
        "pano_gpu_session_finish_uploads_cancellable",
        "pano_gpu_cancellation_token_destroy",
    ]
    assert library.calls == expected_order
    first_upload = library.pano_gpu_session_upload_frame_zero.arguments[0][1]._obj
    later_uploads = [
        arguments[1]._obj
        for arguments in library.pano_gpu_session_upload_frame_cancellable.arguments
    ]
    assert [first_upload.frame_index, *(upload.frame_index for upload in later_uploads)] == [
        0,
        1,
        2,
    ]
    assert [first_upload.data_bytes, *(upload.data_bytes for upload in later_uploads)] == [
        12,
        12,
        12,
    ]
    assert all(
        arguments[2].value == 303
        for arguments in library.pano_gpu_session_upload_frame_cancellable.arguments
    )
    assert library.pano_gpu_session_finish_uploads_cancellable.arguments[0][1].value == 303

    owner.close()

    assert library.calls[-2:] == ["pano_gpu_session_destroy", "pano_gpu_device_destroy"]


@pytest.mark.gpu_contract
def test_d3d12_prepare_session_failure_cleans_up_in_reverse_order() -> None:
    library = _FakeNativeLibrary()
    library.pano_gpu_session_upload_frame_cancellable._result = 1
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]

    with pytest.raises(D3D12AdapterUnavailableError, match="frame 1"):
        adapter.prepare_session(
            adapter_luid=44,
            rotations=tuple(float(index) for index in range(18)),
            frames=(bytes(12), bytes(12)),
            source_width=2,
            source_height=2,
            source_sample_type=1,
            transfer_function=1,
            source_row_stride_bytes=6,
        )

    assert library.calls[-3:] == [
        "pano_gpu_session_destroy",
        "pano_gpu_device_destroy",
        "pano_gpu_cancellation_token_destroy",
    ]
    assert "pano_gpu_session_finish_uploads_cancellable" not in library.calls


@pytest.mark.gpu_contract
def test_d3d12_prepare_session_translates_native_upload_cancellation() -> None:
    library = _FakeNativeLibrary()
    library.pano_gpu_session_upload_frame_cancellable._result = 2
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]

    with pytest.raises(D3D12RenderCancelledError, match="frame 1"):
        adapter.prepare_session(
            adapter_luid=44,
            rotations=tuple(float(index) for index in range(18)),
            frames=(bytes(12), bytes(12)),
            source_width=2,
            source_height=2,
            source_sample_type=1,
            transfer_function=1,
            source_row_stride_bytes=6,
        )

    assert library.calls[-3:] == [
        "pano_gpu_session_destroy",
        "pano_gpu_device_destroy",
        "pano_gpu_cancellation_token_destroy",
    ]


@pytest.mark.gpu_contract
@pytest.mark.parametrize(
    ("frames", "message"),
    [((bytes(12),), "ended before"), ((bytes(12), bytes(12), bytes(12)), "more frames")],
)
def test_d3d12_prepare_session_rejects_mismatched_source_stream(
    frames: tuple[bytes, ...], message: str
) -> None:
    library = _FakeNativeLibrary()
    adapter = D3D12Adapter(library)  # type: ignore[arg-type]

    with pytest.raises(ValueError, match=message):
        adapter.prepare_session(
            adapter_luid=44,
            rotations=tuple(float(index) for index in range(18)),
            frames=iter(frames),
            source_width=2,
            source_height=2,
            source_sample_type=1,
            transfer_function=1,
            source_row_stride_bytes=6,
        )

    assert library.calls[-3:] == [
        "pano_gpu_session_destroy",
        "pano_gpu_device_destroy",
        "pano_gpu_cancellation_token_destroy",
    ]
    assert "pano_gpu_session_finish_uploads_cancellable" not in library.calls


@pytest.mark.gpu_contract
def test_d3d12_output_band_scheduler_reuses_the_shared_watchdog_policy() -> None:
    scheduler = D3D12OutputBandScheduler(2048)

    assert scheduler.rows == 256
    assert scheduler.next_rows(100) == 100
    scheduler.record_completed_band(0.5)
    assert scheduler.rows == 128
    scheduler.record_completed_band(0.125)
    assert scheduler.rows == 256


@pytest.mark.gpu_contract
def test_d3d12_output_bands_publish_progress_after_each_completed_band() -> None:
    events: list[tuple[str, int]] = []

    def render_completed_band(row_start: int, rows: int) -> float:
        events.append(("render", row_start + rows))
        return 0.25

    def report_progress(completed_rows: int, _total_rows: int) -> None:
        events.append(("progress", completed_rows))

    bands = run_d3d12_output_bands(
        300,
        D3D12OutputBandScheduler(256),
        render_completed_band,
        report_progress,
    )

    assert bands == ((0, 256), (256, 44))
    assert events == [("render", 256), ("progress", 256), ("render", 300), ("progress", 300)]


@pytest.mark.gpu_contract
def test_d3d12_output_bands_cancel_before_the_next_submission_and_close() -> None:
    rendered: list[tuple[int, int]] = []
    closed: list[bool] = []

    with pytest.raises(D3D12RenderCancelledError, match="between output bands"):
        run_d3d12_output_bands(
            300,
            D3D12OutputBandScheduler(256),
            lambda row_start, rows: rendered.append((row_start, rows)) or 0.25,
            is_cancelled=lambda: len(rendered) == 1,
            close_output_job=lambda: closed.append(True),
        )

    assert rendered == [(0, 256)]
    assert closed == [True]
