"""Thin ctypes loader for the transitional native D3D12 core."""

from __future__ import annotations

import ctypes
import os
import sys
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path
from threading import Lock
from typing import Any, Final

import numpy as np

from pano_stitch.gpu import GpuBandScheduler

PANO_GPU_ABI_VERSION: Final = 10
PANO_GPU_UNAVAILABLE: Final = 1
PANO_GPU_CANCELLED: Final = 2


class _ProbeOptions(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("allow_warp", ctypes.c_uint32),
    ]


class _AdapterInfo(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("vendor_id", ctypes.c_uint32),
        ("device_id", ctypes.c_uint32),
        ("luid", ctypes.c_uint64),
        ("dedicated_bytes", ctypes.c_uint64),
        ("local_budget_bytes", ctypes.c_uint64),
        ("local_usage_bytes", ctypes.c_uint64),
        ("name", ctypes.c_char * 128),
    ]


class _SessionCreateOptions(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("source_width", ctypes.c_uint32),
        ("source_height", ctypes.c_uint32),
        ("source_sample_type", ctypes.c_uint32),
        ("transfer_function", ctypes.c_uint32),
        ("source_row_stride_bytes", ctypes.c_uint32),
        ("device_luid", ctypes.c_uint64),
        ("rotations", ctypes.c_void_p),
        ("rotations_bytes", ctypes.c_uint64),
        ("encoding_metadata", ctypes.c_void_p),
        ("encoding_metadata_bytes", ctypes.c_uint64),
    ]


class _SessionDiagnostics(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("descriptor_count", ctypes.c_uint32),
        ("planned_source_bytes", ctypes.c_uint64),
        ("source_bytes", ctypes.c_uint64),
        ("planned_rotation_bytes", ctypes.c_uint64),
        ("rotation_bytes", ctypes.c_uint64),
        ("planned_encoding_metadata_bytes", ctypes.c_uint64),
        ("encoding_metadata_bytes", ctypes.c_uint64),
        ("upload_count", ctypes.c_uint32),
        ("uploaded_bytes", ctypes.c_uint64),
        ("last_completed_upload_fence", ctypes.c_uint64),
    ]


class _SourceUpload(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_index", ctypes.c_uint32),
        ("source_sample_type", ctypes.c_uint32),
        ("source_row_stride_bytes", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("data_bytes", ctypes.c_uint64),
    ]


class _ExposureProxyRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("source_width", ctypes.c_uint32),
        ("source_height", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class _ExposurePairRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("first_frame_index", ctypes.c_uint32),
        ("second_frame_index", ctypes.c_uint32),
        ("sample_width", ctypes.c_uint32),
        ("sample_height", ctypes.c_uint32),
        ("latitude_span_degrees", ctypes.c_float),
        ("horizontal_fov_degrees", ctypes.c_float),
        ("vertical_fov_degrees", ctypes.c_float),
        ("reserved", ctypes.c_uint32),
    ]


class _ExposureSolveResult(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("anchor_frame_index", ctypes.c_uint32),
        ("edge_count", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class _ExposureReport(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("anchor_frame_index", ctypes.c_uint32),
        ("edge_count", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("gains_uploaded", ctypes.c_uint32),
        ("solve_count", ctypes.c_uint32),
        ("gain_upload_count", ctypes.c_uint32),
    ]


class _OutputCreateOptions(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("output_width", ctypes.c_uint32),
        ("output_height", ctypes.c_uint32),
        ("output_sample_bytes", ctypes.c_uint32),
        ("output_band_rows", ctypes.c_uint32),
        ("descriptor_count", ctypes.c_uint32),
        ("output_workspace_bytes", ctypes.c_uint64),
    ]


class _OneFrameCompositeRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_index", ctypes.c_uint32),
        ("source_sample_type", ctypes.c_uint32),
        ("output_width", ctypes.c_uint32),
        ("output_height", ctypes.c_uint32),
        ("row_start", ctypes.c_uint32),
        ("row_count", ctypes.c_uint32),
        ("latitude_span_degrees", ctypes.c_float),
        ("horizontal_fov_degrees", ctypes.c_float),
        ("vertical_fov_degrees", ctypes.c_float),
        ("world_to_camera", ctypes.c_float * 9),
        ("rectilinear_output", ctypes.c_uint32),
        ("output_vertical_fov_degrees", ctypes.c_float),
    ]


class _OrderedCompositeRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_request_count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("frame_requests", ctypes.POINTER(_OneFrameCompositeRequest)),
    ]


class _CompositeInputs(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("use_session_exposure_gains", ctypes.c_uint32),
        ("mark_incomplete", ctypes.c_uint32),
        ("global_gains", ctypes.POINTER(ctypes.c_float)),
        ("global_gain_bytes", ctypes.c_uint64),
        ("local_fields", ctypes.POINTER(ctypes.c_float)),
        ("local_field_bytes", ctypes.c_uint64),
        ("reserved", ctypes.c_uint32),
    ]


class _AutoContrastLevels(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("black", ctypes.c_float),
        ("white", ctypes.c_float),
        ("processed_pixels", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class _OutputDownloadRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("output_width", ctypes.c_uint32),
        ("row_start", ctypes.c_uint32),
        ("row_count", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("data_bytes", ctypes.c_uint64),
    ]


class _PreviewCreateOptions(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("preview_width", ctypes.c_uint32),
        ("preview_height", ctypes.c_uint32),
        ("overview_width", ctypes.c_uint32),
        ("overview_height", ctypes.c_uint32),
        ("mask_width", ctypes.c_uint32),
        ("mask_height", ctypes.c_uint32),
        ("preview_rgb8", ctypes.c_void_p),
        ("preview_rgb8_bytes", ctypes.c_uint64),
        ("overview_rgb8", ctypes.c_void_p),
        ("overview_rgb8_bytes", ctypes.c_uint64),
        ("compact_masks", ctypes.c_void_p),
        ("compact_mask_bytes", ctypes.c_uint64),
    ]


class _PreviewRenderRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("crop_left", ctypes.c_uint32),
        ("crop_top", ctypes.c_uint32),
        ("crop_width", ctypes.c_uint32),
        ("crop_height", ctypes.c_uint32),
        ("use_overview", ctypes.c_uint32),
        ("output_rgb8", ctypes.c_void_p),
        ("output_rgb8_bytes", ctypes.c_uint64),
    ]


class _PreviewOverlayRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("crop_left", ctypes.c_uint32),
        ("crop_top", ctypes.c_uint32),
        ("crop_width", ctypes.c_uint32),
        ("crop_height", ctypes.c_uint32),
        ("use_overview", ctypes.c_uint32),
        ("hovered_frames", ctypes.c_void_p),
        ("hovered_frame_bytes", ctypes.c_uint64),
        ("target_pose", ctypes.c_int32),
        ("target_mode", ctypes.c_uint32),
        ("show_boundaries", ctypes.c_uint32),
        ("output_rgb8", ctypes.c_void_p),
        ("output_rgb8_bytes", ctypes.c_uint64),
    ]


class _MemoryRequest(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("source_width", ctypes.c_uint32),
        ("source_height", ctypes.c_uint32),
        ("source_sample_bytes", ctypes.c_uint32),
        ("output_width", ctypes.c_uint32),
        ("output_height", ctypes.c_uint32),
        ("output_sample_bytes", ctypes.c_uint32),
        ("needs_sdr_conversion", ctypes.c_uint32),
        ("free_bytes", ctypes.c_uint64),
        ("total_bytes", ctypes.c_uint64),
        ("requested_budget_bytes", ctypes.c_uint64),
        ("preview_cache_bytes", ctypes.c_uint64),
        ("session_workspace_bytes", ctypes.c_uint64),
        ("output_workspace_bytes_per_pixel", ctypes.c_uint64),
        ("output_workspace_fixed_bytes", ctypes.c_uint64),
        ("upload_bytes", ctypes.c_uint64),
        ("readback_bytes_per_pixel", ctypes.c_uint64),
        ("readback_fixed_bytes", ctypes.c_uint64),
        ("descriptor_count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class _MemoryPlan(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("output_band_rows", ctypes.c_uint32),
        ("descriptor_count", ctypes.c_uint32),
        ("source_bytes", ctypes.c_uint64),
        ("session_workspace_bytes", ctypes.c_uint64),
        ("output_workspace_bytes", ctypes.c_uint64),
        ("upload_bytes", ctypes.c_uint64),
        ("readback_bytes", ctypes.c_uint64),
        ("reserve_bytes", ctypes.c_uint64),
        ("required_bytes", ctypes.c_uint64),
        ("available_bytes", ctypes.c_uint64),
    ]


class D3D12AdapterUnavailableError(RuntimeError):
    """Raised when the native D3D12 core cannot be loaded or used."""


class D3D12RenderCancelledError(RuntimeError):
    """Raised after cancellation prevents submission of another native band."""


class D3D12CancellationToken:
    """Closeable native token that may be cancelled from another Python thread."""

    def __init__(self, adapter: D3D12Adapter, handle: int) -> None:
        self._adapter = adapter
        self._handle = ctypes.c_void_p(handle)

    @property
    def handle(self) -> ctypes.c_void_p:
        return self._handle

    def cancel(self) -> None:
        if self._handle.value is not None:
            self._adapter._library.pano_gpu_cancellation_token_cancel(self._handle)

    def close(self) -> None:
        if self._handle.value is not None:
            self._adapter._library.pano_gpu_cancellation_token_destroy(ctypes.byref(self._handle))
            self._handle = ctypes.c_void_p()


class D3D12PreparedSession:
    """Owner for a finished native session and the device that backs it."""

    def __init__(
        self,
        adapter: D3D12Adapter,
        device_handle: int,
        session_handle: int,
        *,
        frame_count: int = 0,
        source_width: int = 0,
        source_height: int = 0,
        adapter_luid: int = 0,
    ) -> None:
        self._adapter = adapter
        self._device_handle = ctypes.c_void_p(device_handle)
        self._session_handle = ctypes.c_void_p(session_handle)
        self._children: set[D3D12OutputJob | D3D12PreviewJob] = set()
        self._frame_count = frame_count
        self._source_width = source_width
        self._source_height = source_height
        self._adapter_luid = adapter_luid
        self._exposure_report: _ExposureReport | None = None

    def solve_exposure(
        self,
        *,
        sample_width: int,
        sample_height: int,
        latitude_span_degrees: float,
        horizontal_fov_degrees: float,
        vertical_fov_degrees: float,
    ) -> _ExposureReport:
        """Build, solve, and upload the retained native exposure graph exactly once."""

        if self._session_handle.value is None:
            raise RuntimeError("D3D12 prepared session is closed")
        if self._exposure_report is not None:
            return _ExposureReport.from_buffer_copy(self._exposure_report)
        if self._frame_count < 1 or self._source_width < 1 or self._source_height < 1:
            raise RuntimeError("D3D12 prepared session has no retained source geometry")
        error = ctypes.create_string_buffer(512)
        proxy_request = _ExposureProxyRequest(
            size=ctypes.sizeof(_ExposureProxyRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            frame_count=self._frame_count,
            source_width=self._source_width,
            source_height=self._source_height,
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_build_exposure_proxies(
                self._session_handle, ctypes.byref(proxy_request), error, len(error)
            ),
            error,
            "cannot build retained D3D12 exposure proxies",
        )
        pair_count = ctypes.c_uint32()
        self._adapter._checked(
            self._adapter._library.pano_gpu_exposure_pair_count(
                self._frame_count, ctypes.byref(pair_count), error, len(error)
            ),
            error,
            "cannot count D3D12 exposure pairs",
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_prepare_exposure_graph(
                self._session_handle, pair_count.value, error, len(error)
            ),
            error,
            "cannot prepare retained D3D12 exposure graph",
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_enumerate_exposure_pairs(
                self._session_handle, error, len(error)
            ),
            error,
            "cannot enumerate retained D3D12 exposure pairs",
        )
        pair_request = _ExposurePairRequest(
            size=ctypes.sizeof(_ExposurePairRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            first_frame_index=0,
            second_frame_index=min(1, self._frame_count - 1),
            sample_width=sample_width,
            sample_height=sample_height,
            latitude_span_degrees=latitude_span_degrees,
            horizontal_fov_degrees=horizontal_fov_degrees,
            vertical_fov_degrees=vertical_fov_degrees,
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_reduce_exposure_graph(
                self._session_handle, ctypes.byref(pair_request), error, len(error)
            ),
            error,
            "cannot reduce retained D3D12 exposure graph",
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_build_exposure_solve_graph(
                self._session_handle, error, len(error)
            ),
            error,
            "cannot build retained D3D12 exposure solve graph",
        )
        solve_result = _ExposureSolveResult(
            size=ctypes.sizeof(_ExposureSolveResult), abi_version=PANO_GPU_ABI_VERSION
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_solve_exposure_graph(
                self._session_handle, ctypes.byref(solve_result), error, len(error)
            ),
            error,
            "cannot solve retained D3D12 exposure graph",
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_upload_exposure_gains(
                self._session_handle, error, len(error)
            ),
            error,
            "cannot upload retained D3D12 exposure gains",
        )
        report = _ExposureReport(
            size=ctypes.sizeof(_ExposureReport), abi_version=PANO_GPU_ABI_VERSION
        )
        self._adapter._checked(
            self._adapter._library.pano_gpu_session_query_exposure_report(
                self._session_handle, ctypes.byref(report), error, len(error)
            ),
            error,
            "cannot query retained D3D12 exposure report",
        )
        self._exposure_report = _ExposureReport.from_buffer_copy(report)
        return report

    def retain_output(self, output_handle: int) -> D3D12OutputJob:
        if self._session_handle.value is None:
            raise RuntimeError("D3D12 prepared session is closed")
        child = D3D12OutputJob(self, output_handle)
        self._children.add(child)
        return child

    def create_cancellation_token(self) -> D3D12CancellationToken:
        if self._session_handle.value is None:
            raise RuntimeError("D3D12 prepared session is closed")
        return self._adapter.create_cancellation_token()

    @property
    def adapter_luid(self) -> int:
        return self._adapter_luid

    def create_output(self, options: _OutputCreateOptions) -> D3D12OutputJob:
        """Create and fully allocate a native output before publishing its owner."""

        if self._session_handle.value is None:
            raise RuntimeError("D3D12 prepared session is closed")
        error = ctypes.create_string_buffer(512)
        output = ctypes.c_void_p()
        try:
            self._adapter._checked(
                self._adapter._library.pano_gpu_output_create_empty(
                    self._session_handle,
                    ctypes.byref(options),
                    ctypes.byref(output),
                    error,
                    len(error),
                ),
                error,
                "cannot create native D3D12 output",
            )
            for function_name in (
                "pano_gpu_output_allocate_linear",
                "pano_gpu_output_allocate_coverage",
            ):
                self._adapter._checked(
                    getattr(self._adapter._library, function_name)(output, error, len(error)),
                    error,
                    f"{function_name} failed",
                )
            child = self.retain_output(output.value or 0)
            output = ctypes.c_void_p()
            return child
        finally:
            if output.value is not None:
                self._adapter._library.pano_gpu_output_destroy(ctypes.byref(output))

    def _release_output(self, child: D3D12OutputJob) -> None:
        self._children.discard(child)

    def create_preview(
        self,
        *,
        frame_count: int,
        preview_width: int,
        preview_height: int,
        preview_rgb8: bytearray | memoryview,
        overview_width: int,
        overview_height: int,
        overview_rgb8: bytearray | memoryview,
        mask_width: int,
        mask_height: int,
        compact_masks: bytearray | memoryview,
    ) -> D3D12PreviewJob:
        """Copy bounded preview inputs into one session-retaining native owner."""

        if self._session_handle.value is None:
            raise RuntimeError("D3D12 prepared session is closed")

        def borrowed(value: bytearray | memoryview) -> tuple[memoryview, ctypes.Array[Any]]:
            view = memoryview(value)
            if not view.c_contiguous:
                raise ValueError("D3D12 preview inputs must be C-contiguous")
            owner = (
                (ctypes.c_ubyte * view.nbytes).from_buffer_copy(view)
                if view.readonly
                else (ctypes.c_ubyte * view.nbytes).from_buffer(view)
            )
            return view, owner

        preview_view, preview_owner = borrowed(preview_rgb8)
        overview_view, overview_owner = borrowed(overview_rgb8)
        mask_view, mask_owner = borrowed(compact_masks)
        options = _PreviewCreateOptions(
            size=ctypes.sizeof(_PreviewCreateOptions),
            abi_version=PANO_GPU_ABI_VERSION,
            frame_count=frame_count,
            preview_width=preview_width,
            preview_height=preview_height,
            overview_width=overview_width,
            overview_height=overview_height,
            mask_width=mask_width,
            mask_height=mask_height,
            preview_rgb8=ctypes.cast(preview_owner, ctypes.c_void_p),
            preview_rgb8_bytes=preview_view.nbytes,
            overview_rgb8=ctypes.cast(overview_owner, ctypes.c_void_p),
            overview_rgb8_bytes=overview_view.nbytes,
            compact_masks=ctypes.cast(mask_owner, ctypes.c_void_p),
            compact_mask_bytes=mask_view.nbytes,
        )
        error = ctypes.create_string_buffer(512)
        preview = ctypes.c_void_p()
        self._adapter._checked(
            self._adapter._library.pano_gpu_preview_create(
                self._session_handle,
                ctypes.byref(options),
                ctypes.byref(preview),
                error,
                len(error),
            ),
            error,
            "cannot create retained D3D12 preview",
        )
        child = D3D12PreviewJob(self, preview.value or 0)
        self._children.add(child)
        return child

    def close(self) -> None:
        for child in tuple(self._children):
            child.close()
        if self._session_handle.value is not None:
            self._adapter._library.pano_gpu_session_destroy(ctypes.byref(self._session_handle))
            self._session_handle = ctypes.c_void_p()
        if self._device_handle.value is not None:
            self._adapter._library.pano_gpu_device_destroy(ctypes.byref(self._device_handle))
            self._device_handle = ctypes.c_void_p()


class D3D12OutputJob:
    """Closeable native output that keeps its prepared parent alive."""

    def __init__(self, parent: D3D12PreparedSession, output_handle: int) -> None:
        self._parent: D3D12PreparedSession | None = parent
        self._output_handle = ctypes.c_void_p(output_handle)

    def _call(self, name: str, *arguments: object) -> None:
        parent = self._parent
        if parent is None or self._output_handle.value is None:
            raise RuntimeError("D3D12 output job is closed")
        error = ctypes.create_string_buffer(512)
        parent._adapter._checked(
            getattr(parent._adapter._library, name)(
                self._output_handle, *arguments, error, len(error)
            ),
            error,
            f"{name} failed",
        )

    def compose(
        self,
        frame_requests: Sequence[_OneFrameCompositeRequest],
        *,
        feather: bool,
        use_session_exposure_gains: bool = False,
        mark_incomplete: bool = False,
        global_gains: Sequence[float] = (),
        local_fields: Sequence[float] = (),
    ) -> None:
        """Compose one complete resident image or one output band."""

        if not frame_requests:
            raise ValueError("D3D12 composition requires at least one frame request")
        if use_session_exposure_gains and global_gains:
            raise ValueError("retained and explicit D3D12 exposure gains are mutually exclusive")
        request_array = (_OneFrameCompositeRequest * len(frame_requests))(*frame_requests)
        request = _OrderedCompositeRequest(
            size=ctypes.sizeof(_OrderedCompositeRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            frame_request_count=len(request_array),
            frame_requests=request_array,
        )
        gain_array = (ctypes.c_float * len(global_gains))(*global_gains)
        local_array = (ctypes.c_float * len(local_fields))(*local_fields)
        inputs = _CompositeInputs(
            size=ctypes.sizeof(_CompositeInputs),
            abi_version=PANO_GPU_ABI_VERSION,
            use_session_exposure_gains=int(use_session_exposure_gains),
            mark_incomplete=int(mark_incomplete),
            global_gains=gain_array if gain_array else None,
            global_gain_bytes=ctypes.sizeof(gain_array),
            local_fields=local_array if local_array else None,
            local_field_bytes=ctypes.sizeof(local_array),
        )
        mode = "feather" if feather else "hard"
        self._call(
            f"pano_gpu_output_compose_{mode}_with_inputs",
            ctypes.byref(request),
            ctypes.byref(inputs),
        )

    def prepare_auto_contrast(self) -> None:
        self._call("pano_gpu_output_prepare_auto_contrast_histogram")

    def accumulate_auto_contrast_srgb(self, *, converted: bool = False) -> None:
        suffix = "converted_srgb" if converted else "srgb"
        self._call(f"pano_gpu_output_accumulate_auto_contrast_histogram_{suffix}")

    def select_auto_contrast_levels(self) -> _AutoContrastLevels:
        levels = _AutoContrastLevels(
            size=ctypes.sizeof(_AutoContrastLevels), abi_version=PANO_GPU_ABI_VERSION
        )
        self._call("pano_gpu_output_select_auto_contrast_levels", ctypes.byref(levels))
        return levels

    def apply_auto_contrast_srgb(self, *, apply_levels: bool, converted: bool = False) -> None:
        suffix = "converted_srgb" if converted else "srgb"
        self._call(f"pano_gpu_output_apply_auto_contrast_{suffix}", int(apply_levels))

    def quantize_srgb8(self) -> None:
        self._call("pano_gpu_output_quantize_normalized_srgb8")

    def tone_map_rec2020(self, reference_white_nits: float) -> None:
        self._call("pano_gpu_output_tone_map_rec2020", ctypes.c_float(reference_white_nits))

    def convert_tone_mapped_rec2020_to_linear_srgb(self) -> None:
        self._call("pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb")

    def copy_linear_float(self) -> None:
        self._call("pano_gpu_output_copy_linear_float")

    def download(
        self,
        destination: bytearray | memoryview,
        *,
        output_width: int,
        row_start: int,
        row_count: int,
        floating_point: bool,
        cancellation: D3D12CancellationToken | None = None,
    ) -> None:
        """Download a completed band into one exact caller-owned writable buffer."""

        view = memoryview(destination)
        if view.readonly or not view.c_contiguous:
            raise ValueError("D3D12 download destination must be writable and C-contiguous")
        expected_bytes = output_width * row_count * 3 * (4 if floating_point else 1)
        if view.nbytes != expected_bytes:
            raise ValueError(f"D3D12 download requires exactly {expected_bytes} destination bytes")
        owner = (ctypes.c_ubyte * view.nbytes).from_buffer(view)
        request = _OutputDownloadRequest(
            size=ctypes.sizeof(_OutputDownloadRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            output_width=output_width,
            row_start=row_start,
            row_count=row_count,
            data=ctypes.cast(owner, ctypes.c_void_p),
            data_bytes=view.nbytes,
        )
        self._call(
            "pano_gpu_output_download_float"
            if floating_point
            else "pano_gpu_output_download_srgb8",
            ctypes.byref(request),
            cancellation.handle if cancellation is not None else None,
        )

    def download_coverage(
        self,
        destination: bytearray | memoryview,
        *,
        output_width: int,
        row_start: int,
        row_count: int,
        cancellation: D3D12CancellationToken | None = None,
    ) -> None:
        """Download one exact completed coverage band into caller-owned storage."""

        view = memoryview(destination)
        if view.readonly or not view.c_contiguous:
            raise ValueError("D3D12 coverage destination must be writable and C-contiguous")
        expected_bytes = output_width * row_count
        if view.nbytes != expected_bytes:
            raise ValueError(
                f"D3D12 coverage download requires exactly {expected_bytes} destination bytes"
            )
        owner = (ctypes.c_ubyte * view.nbytes).from_buffer(view)
        request = _OutputDownloadRequest(
            size=ctypes.sizeof(_OutputDownloadRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            output_width=output_width,
            row_start=row_start,
            row_count=row_count,
            data=ctypes.cast(owner, ctypes.c_void_p),
            data_bytes=view.nbytes,
        )
        self._call(
            "pano_gpu_output_download_coverage",
            ctypes.byref(request),
            cancellation.handle if cancellation is not None else None,
        )

    def close(self) -> None:
        parent = self._parent
        if parent is None:
            return
        if self._output_handle.value is not None:
            parent._adapter._library.pano_gpu_output_destroy(ctypes.byref(self._output_handle))
            self._output_handle = ctypes.c_void_p()
        self._parent = None
        parent._release_output(self)


class D3D12PreviewJob:
    """Closeable native preview that retains its prepared parent."""

    def __init__(self, parent: D3D12PreparedSession, preview_handle: int) -> None:
        self._parent: D3D12PreparedSession | None = parent
        self._preview_handle = ctypes.c_void_p(preview_handle)

    def render_base(
        self,
        destination: bytearray | memoryview,
        *,
        crop_left: int,
        crop_top: int,
        crop_width: int,
        crop_height: int,
        use_overview: bool,
        cancellation: D3D12CancellationToken | None = None,
    ) -> None:
        parent = self._parent
        if parent is None or self._preview_handle.value is None:
            raise RuntimeError("D3D12 preview is closed")
        view = memoryview(destination)
        expected_bytes = crop_width * crop_height * 3
        if view.readonly or not view.c_contiguous or view.nbytes != expected_bytes:
            raise ValueError(
                "D3D12 preview destination must be writable, contiguous, and "
                f"{expected_bytes} bytes"
            )
        owner = (ctypes.c_ubyte * view.nbytes).from_buffer(view)
        request = _PreviewRenderRequest(
            size=ctypes.sizeof(_PreviewRenderRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            crop_left=crop_left,
            crop_top=crop_top,
            crop_width=crop_width,
            crop_height=crop_height,
            use_overview=int(use_overview),
            output_rgb8=ctypes.cast(owner, ctypes.c_void_p),
            output_rgb8_bytes=view.nbytes,
        )
        error = ctypes.create_string_buffer(512)
        parent._adapter._checked(
            parent._adapter._library.pano_gpu_preview_render_base(
                self._preview_handle,
                ctypes.byref(request),
                cancellation.handle if cancellation is not None else None,
                error,
                len(error),
            ),
            error,
            "cannot render retained D3D12 preview",
        )

    def set_generation(self, generation: int) -> None:
        parent = self._parent
        if parent is None or self._preview_handle.value is None:
            raise RuntimeError("D3D12 preview is closed")
        error = ctypes.create_string_buffer(512)
        parent._adapter._checked(
            parent._adapter._library.pano_gpu_preview_set_generation(
                self._preview_handle, generation, error, len(error)
            ),
            error,
            "cannot set retained D3D12 preview generation",
        )

    def render_overlay(
        self,
        destination: bytearray | memoryview,
        *,
        crop_left: int,
        crop_top: int,
        crop_width: int,
        crop_height: int,
        use_overview: bool,
        hovered_frames: bytes | bytearray | memoryview,
        target_pose: int,
        target_mode: bool,
        show_boundaries: bool,
        generation: int,
        cancellation: D3D12CancellationToken | None = None,
    ) -> None:
        parent = self._parent
        if parent is None or self._preview_handle.value is None:
            raise RuntimeError("D3D12 preview is closed")
        view = memoryview(destination)
        expected_bytes = crop_width * crop_height * 3
        hovered_view = memoryview(hovered_frames)
        if view.readonly or not view.c_contiguous or view.nbytes != expected_bytes:
            raise ValueError("invalid D3D12 preview overlay destination")
        if not hovered_view.c_contiguous:
            raise ValueError("D3D12 hovered-frame flags must be contiguous")
        output_owner = (ctypes.c_ubyte * view.nbytes).from_buffer(view)
        hovered_owner = (ctypes.c_ubyte * hovered_view.nbytes).from_buffer_copy(hovered_view)
        request = _PreviewOverlayRequest(
            size=ctypes.sizeof(_PreviewOverlayRequest),
            abi_version=PANO_GPU_ABI_VERSION,
            crop_left=crop_left,
            crop_top=crop_top,
            crop_width=crop_width,
            crop_height=crop_height,
            use_overview=int(use_overview),
            hovered_frames=ctypes.cast(hovered_owner, ctypes.c_void_p),
            hovered_frame_bytes=hovered_view.nbytes,
            target_pose=target_pose,
            target_mode=int(target_mode),
            show_boundaries=int(show_boundaries),
            output_rgb8=ctypes.cast(output_owner, ctypes.c_void_p),
            output_rgb8_bytes=view.nbytes,
        )
        error = ctypes.create_string_buffer(512)
        parent._adapter._checked(
            parent._adapter._library.pano_gpu_preview_render_overlay_generation(
                self._preview_handle,
                ctypes.byref(request),
                generation,
                cancellation.handle if cancellation is not None else None,
                error,
                len(error),
            ),
            error,
            "cannot render retained D3D12 preview overlay",
        )

    def close(self) -> None:
        parent = self._parent
        if parent is None:
            return
        if self._preview_handle.value is not None:
            parent._adapter._library.pano_gpu_preview_destroy(ctypes.byref(self._preview_handle))
            self._preview_handle = ctypes.c_void_p()
        self._parent = None
        parent._children.discard(self)


class D3D12PreviewDisplay:
    """Generation-aware retained D3D12 viewport compositor for the GUI worker."""

    def __init__(
        self,
        preview: D3D12PreviewJob,
        token: D3D12CancellationToken,
        *,
        frame_count: int,
        preview_width: int,
        preview_height: int,
        output_width: int,
        output_height: int,
    ) -> None:
        self._preview: D3D12PreviewJob | None = preview
        self._token: D3D12CancellationToken | None = token
        self._lock = Lock()
        self._generation = 0
        self.frame_count = frame_count
        self.preview_width = preview_width
        self.preview_height = preview_height
        self.output_width = output_width
        self.output_height = output_height

    def render(
        self,
        crop_box: tuple[int, int, int, int] | None,
        hovered_poses: frozenset[int],
        target_pose: int | None,
        target_mode: bool,
        overlay: bool,
    ) -> np.ndarray[Any, Any]:
        with self._lock:
            if self._preview is None:
                raise RuntimeError("D3D12 preview display is closed")
            crop = crop_box or (0, 0, self.output_width, self.output_height)
            left, top, right, bottom = crop
            if right - left != self.output_width or bottom - top != self.output_height:
                raise ValueError("D3D12 preview crop does not match the display viewport")
            destination = bytearray(self.output_width * self.output_height * 3)
            self._generation += 1
            self._preview.set_generation(self._generation)
            if overlay or hovered_poses:
                hovered = bytearray(self.frame_count)
                for position in hovered_poses:
                    if 0 <= position < self.frame_count:
                        hovered[position] = 1
                self._preview.render_overlay(
                    destination,
                    crop_left=left,
                    crop_top=top,
                    crop_width=self.output_width,
                    crop_height=self.output_height,
                    use_overview=crop_box is None,
                    hovered_frames=hovered,
                    target_pose=target_pose if target_pose is not None else -1,
                    target_mode=target_mode,
                    show_boundaries=overlay,
                    generation=self._generation,
                    cancellation=self._token,
                )
            else:
                self._preview.render_base(
                    destination,
                    crop_left=left,
                    crop_top=top,
                    crop_width=self.output_width,
                    crop_height=self.output_height,
                    use_overview=crop_box is None,
                    cancellation=self._token,
                )
            return np.frombuffer(destination, dtype=np.uint8).reshape(
                self.output_height, self.output_width, 3
            )

    def close(self) -> None:
        with self._lock:
            preview = self._preview
            token = self._token
            self._preview = None
            self._token = None
            if preview is not None:
                preview.close()
            if token is not None:
                token.close()


class D3D12OutputBandScheduler:
    """Apply the shared watchdog policy at completed native output-band boundaries."""

    def __init__(self, workspace_rows: int) -> None:
        self._scheduler = GpuBandScheduler(workspace_rows)

    @property
    def rows(self) -> int:
        return self._scheduler.rows

    def next_rows(self, remaining_rows: int) -> int:
        return self._scheduler.next_rows(remaining_rows)

    def record_completed_band(self, elapsed_seconds: float) -> None:
        self._scheduler.record_elapsed(elapsed_seconds)


def run_d3d12_output_bands(
    output_height: int,
    scheduler: D3D12OutputBandScheduler,
    render_completed_band: Callable[[int, int], float],
    progress_callback: Callable[[int, int], None] | None = None,
    is_cancelled: Callable[[], bool] | None = None,
    close_output_job: Callable[[], None] | None = None,
) -> tuple[tuple[int, int], ...]:
    """Render native bands and publish rows only after each callback completes."""

    if output_height < 1:
        raise ValueError("D3D12 output height must be positive")
    completed_rows = 0
    bands: list[tuple[int, int]] = []
    while completed_rows < output_height:
        if is_cancelled is not None and is_cancelled():
            if close_output_job is not None:
                close_output_job()
            raise D3D12RenderCancelledError("D3D12 render cancelled between output bands")
        rows = scheduler.next_rows(output_height - completed_rows)
        elapsed_seconds = render_completed_band(completed_rows, rows)
        scheduler.record_completed_band(elapsed_seconds)
        completed_rows += rows
        bands.append((completed_rows - rows, rows))
        if progress_callback is not None:
            progress_callback(completed_rows, output_height)
    return tuple(bands)


class D3D12Adapter:
    """Version-checked declarations for the native C ABI; no image logic lives here."""

    def __init__(self, library: ctypes.CDLL) -> None:
        self._library = library
        library.pano_gpu_abi_version.argtypes = []
        library.pano_gpu_abi_version.restype = ctypes.c_uint32
        library.pano_gpu_probe_adapter.argtypes = [
            ctypes.POINTER(_ProbeOptions),
            ctypes.POINTER(_AdapterInfo),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_probe_adapter.restype = ctypes.c_int
        library.pano_gpu_dispatch_self_test.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_dispatch_self_test.restype = ctypes.c_int
        library.pano_gpu_plan_memory.argtypes = [
            ctypes.POINTER(_MemoryRequest),
            ctypes.POINTER(_MemoryPlan),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_plan_memory.restype = ctypes.c_int
        library.pano_gpu_cancellation_token_create.argtypes = [
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_cancellation_token_create.restype = ctypes.c_int
        library.pano_gpu_cancellation_token_cancel.argtypes = [ctypes.c_void_p]
        library.pano_gpu_cancellation_token_cancel.restype = None
        library.pano_gpu_cancellation_token_is_cancelled.argtypes = [ctypes.c_void_p]
        library.pano_gpu_cancellation_token_is_cancelled.restype = ctypes.c_int32
        library.pano_gpu_cancellation_token_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        library.pano_gpu_cancellation_token_destroy.restype = None
        library.pano_gpu_device_create.argtypes = [
            ctypes.POINTER(_ProbeOptions),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_device_create.restype = ctypes.c_int
        library.pano_gpu_session_create.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_SessionCreateOptions),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_create.restype = ctypes.c_int
        library.pano_gpu_session_allocate_source.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_allocate_source.restype = ctypes.c_int
        for name in (
            "pano_gpu_session_allocate_rotations",
            "pano_gpu_session_allocate_encoding_metadata",
            "pano_gpu_session_allocate_upload_slot",
            "pano_gpu_session_allocate_second_upload_slot",
        ):
            function = getattr(library, name)
            function.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_char),
                ctypes.c_uint32,
            ]
            function.restype = ctypes.c_int
        library.pano_gpu_session_upload_rotations.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_upload_rotations.restype = ctypes.c_int
        library.pano_gpu_session_upload_encoding_metadata.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_upload_encoding_metadata.restype = ctypes.c_int
        library.pano_gpu_session_upload_frame_zero.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_SourceUpload),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_upload_frame_zero.restype = ctypes.c_int
        library.pano_gpu_session_upload_frame_cancellable.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_SourceUpload),
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_upload_frame_cancellable.restype = ctypes.c_int
        library.pano_gpu_session_finish_uploads.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_finish_uploads.restype = ctypes.c_int
        library.pano_gpu_session_finish_uploads_cancellable.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_finish_uploads_cancellable.restype = ctypes.c_int
        library.pano_gpu_session_query_diagnostics.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_SessionDiagnostics),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_query_diagnostics.restype = ctypes.c_int
        library.pano_gpu_session_build_exposure_proxies.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_ExposureProxyRequest),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_build_exposure_proxies.restype = ctypes.c_int
        library.pano_gpu_exposure_pair_count.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_exposure_pair_count.restype = ctypes.c_int
        library.pano_gpu_session_prepare_exposure_graph.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_prepare_exposure_graph.restype = ctypes.c_int
        for name in (
            "pano_gpu_session_enumerate_exposure_pairs",
            "pano_gpu_session_build_exposure_solve_graph",
            "pano_gpu_session_upload_exposure_gains",
        ):
            function = getattr(library, name)
            function.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_char),
                ctypes.c_uint32,
            ]
            function.restype = ctypes.c_int
        library.pano_gpu_session_reduce_exposure_graph.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_ExposurePairRequest),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_reduce_exposure_graph.restype = ctypes.c_int
        library.pano_gpu_session_solve_exposure_graph.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_ExposureSolveResult),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_solve_exposure_graph.restype = ctypes.c_int
        library.pano_gpu_session_query_exposure_report.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_ExposureReport),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_session_query_exposure_report.restype = ctypes.c_int
        library.pano_gpu_output_create_empty.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_OutputCreateOptions),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_output_create_empty.restype = ctypes.c_int
        for name in (
            "pano_gpu_output_allocate_linear",
            "pano_gpu_output_allocate_coverage",
            "pano_gpu_output_prepare_auto_contrast_histogram",
            "pano_gpu_output_accumulate_auto_contrast_histogram_srgb",
            "pano_gpu_output_accumulate_auto_contrast_histogram_converted_srgb",
            "pano_gpu_output_quantize_normalized_srgb8",
            "pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb",
            "pano_gpu_output_copy_linear_float",
        ):
            function = getattr(library, name)
            function.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_char),
                ctypes.c_uint32,
            ]
            function.restype = ctypes.c_int
        for name in (
            "pano_gpu_output_compose_hard_with_inputs",
            "pano_gpu_output_compose_feather_with_inputs",
        ):
            function = getattr(library, name)
            function.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(_OrderedCompositeRequest),
                ctypes.POINTER(_CompositeInputs),
                ctypes.POINTER(ctypes.c_char),
                ctypes.c_uint32,
            ]
            function.restype = ctypes.c_int
        library.pano_gpu_output_select_auto_contrast_levels.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_AutoContrastLevels),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_output_select_auto_contrast_levels.restype = ctypes.c_int
        for name in (
            "pano_gpu_output_apply_auto_contrast_srgb",
            "pano_gpu_output_apply_auto_contrast_converted_srgb",
        ):
            function = getattr(library, name)
            function.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint32,
                ctypes.POINTER(ctypes.c_char),
                ctypes.c_uint32,
            ]
            function.restype = ctypes.c_int
        library.pano_gpu_output_tone_map_rec2020.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_output_tone_map_rec2020.restype = ctypes.c_int
        for name in (
            "pano_gpu_output_download_srgb8",
            "pano_gpu_output_download_float",
            "pano_gpu_output_download_coverage",
        ):
            function = getattr(library, name)
            function.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(_OutputDownloadRequest),
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_char),
                ctypes.c_uint32,
            ]
            function.restype = ctypes.c_int
        library.pano_gpu_preview_create.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_PreviewCreateOptions),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_preview_create.restype = ctypes.c_int
        library.pano_gpu_preview_render_base.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_PreviewRenderRequest),
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_preview_render_base.restype = ctypes.c_int
        library.pano_gpu_preview_set_generation.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_preview_set_generation.restype = ctypes.c_int
        library.pano_gpu_preview_render_overlay_generation.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_PreviewOverlayRequest),
            ctypes.c_uint64,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char),
            ctypes.c_uint32,
        ]
        library.pano_gpu_preview_render_overlay_generation.restype = ctypes.c_int
        library.pano_gpu_preview_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        library.pano_gpu_preview_destroy.restype = None
        library.pano_gpu_output_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        library.pano_gpu_output_destroy.restype = None
        library.pano_gpu_session_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        library.pano_gpu_session_destroy.restype = None
        library.pano_gpu_device_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        library.pano_gpu_device_destroy.restype = None
        if library.pano_gpu_abi_version() != PANO_GPU_ABI_VERSION:
            raise D3D12AdapterUnavailableError("native D3D12 ABI version is incompatible")

    def probe(self, *, allow_warp: bool = False) -> _AdapterInfo:
        """Return one admitted adapter; WARP is reserved for native integration tests."""

        error = ctypes.create_string_buffer(512)
        options = _ProbeOptions(
            size=ctypes.sizeof(_ProbeOptions),
            abi_version=PANO_GPU_ABI_VERSION,
            allow_warp=int(allow_warp),
        )
        adapter = _AdapterInfo(
            size=ctypes.sizeof(_AdapterInfo),
            abi_version=PANO_GPU_ABI_VERSION,
        )
        result = self._library.pano_gpu_probe_adapter(
            ctypes.byref(options), ctypes.byref(adapter), error, len(error)
        )
        if result != 0:
            detail = error.value.decode("utf-8", errors="replace")
            raise D3D12AdapterUnavailableError(detail or "native D3D12 probe failed")
        return adapter

    def verify_runtime(self, *, allow_warp: bool = False) -> None:
        """Create a pipeline, dispatch its embedded shader, and release all test resources."""

        error = ctypes.create_string_buffer(512)
        result = self._library.pano_gpu_dispatch_self_test(int(allow_warp), error, len(error))
        if result != 0:
            detail = error.value.decode("utf-8", errors="replace")
            raise D3D12AdapterUnavailableError(detail or "native D3D12 runtime verification failed")

    def plan_memory(self, request: _MemoryRequest) -> _MemoryPlan:
        """Return the native checked memory plan without submitting numerical work."""

        error = ctypes.create_string_buffer(512)
        plan = _MemoryPlan(size=ctypes.sizeof(_MemoryPlan), abi_version=PANO_GPU_ABI_VERSION)
        result = self._library.pano_gpu_plan_memory(
            ctypes.byref(request), ctypes.byref(plan), error, len(error)
        )
        if result != 0:
            detail = error.value.decode("utf-8", errors="replace")
            raise D3D12AdapterUnavailableError(detail or "native D3D12 memory admission failed")
        return plan

    def _checked(self, result: int, error: ctypes.Array[ctypes.c_char], fallback: str) -> None:
        if result == 0:
            return
        detail = error.value.decode("utf-8", errors="replace") or fallback
        if result == PANO_GPU_CANCELLED:
            raise D3D12RenderCancelledError(detail)
        raise D3D12AdapterUnavailableError(detail)

    def create_cancellation_token(self) -> D3D12CancellationToken:
        """Create a native token suitable for cross-thread cancellation."""

        error = ctypes.create_string_buffer(512)
        handle = ctypes.c_void_p()
        self._checked(
            self._library.pano_gpu_cancellation_token_create(
                ctypes.byref(handle), error, len(error)
            ),
            error,
            "cannot create native D3D12 cancellation token",
        )
        if handle.value is None:
            raise D3D12AdapterUnavailableError("native D3D12 cancellation token is null")
        return D3D12CancellationToken(self, handle.value)

    def prepare_session(
        self,
        *,
        adapter_luid: int,
        rotations: Sequence[float],
        frames: Iterable[bytes | bytearray | memoryview],
        source_width: int,
        source_height: int,
        source_sample_type: int,
        transfer_function: int,
        source_row_stride_bytes: int,
        encoding_metadata: bytes = b"",
        allow_warp: bool = False,
        cancellation: D3D12CancellationToken | None = None,
    ) -> D3D12PreparedSession:
        """Upload one retained source set transactionally through two bounded native slots."""

        if not rotations or len(rotations) % 9 != 0:
            raise ValueError("D3D12 session rotations must contain complete 3x3 matrices")
        frame_count = len(rotations) // 9
        frame_iterator = iter(frames)
        rotation_values = (ctypes.c_float * len(rotations))(*rotations)
        metadata_owner = (
            ctypes.create_string_buffer(encoding_metadata) if encoding_metadata else None
        )
        error = ctypes.create_string_buffer(512)
        probe_options = _ProbeOptions(
            size=ctypes.sizeof(_ProbeOptions),
            abi_version=PANO_GPU_ABI_VERSION,
            allow_warp=int(allow_warp),
        )
        device = ctypes.c_void_p()
        session = ctypes.c_void_p()
        owned_token = cancellation is None
        token = cancellation or self.create_cancellation_token()
        try:
            self._checked(
                self._library.pano_gpu_device_create(
                    ctypes.byref(probe_options), ctypes.byref(device), error, len(error)
                ),
                error,
                "cannot create native D3D12 device",
            )
            options = _SessionCreateOptions(
                size=ctypes.sizeof(_SessionCreateOptions),
                abi_version=PANO_GPU_ABI_VERSION,
                frame_count=frame_count,
                source_width=source_width,
                source_height=source_height,
                source_sample_type=source_sample_type,
                transfer_function=transfer_function,
                source_row_stride_bytes=source_row_stride_bytes,
                device_luid=adapter_luid,
                rotations=ctypes.cast(rotation_values, ctypes.c_void_p),
                rotations_bytes=ctypes.sizeof(rotation_values),
                encoding_metadata=(
                    ctypes.cast(metadata_owner, ctypes.c_void_p)
                    if metadata_owner is not None
                    else None
                ),
                encoding_metadata_bytes=len(encoding_metadata),
            )
            self._checked(
                self._library.pano_gpu_session_create(
                    device, ctypes.byref(options), ctypes.byref(session), error, len(error)
                ),
                error,
                "cannot create native D3D12 session",
            )
            for function_name in (
                "pano_gpu_session_allocate_source",
                "pano_gpu_session_allocate_rotations",
            ):
                self._checked(
                    getattr(self._library, function_name)(session, error, len(error)),
                    error,
                    f"{function_name} failed",
                )
            self._checked(
                self._library.pano_gpu_session_upload_rotations(
                    session,
                    ctypes.cast(rotation_values, ctypes.c_void_p),
                    ctypes.sizeof(rotation_values),
                    error,
                    len(error),
                ),
                error,
                "cannot upload native D3D12 rotations",
            )
            if metadata_owner is not None:
                self._checked(
                    self._library.pano_gpu_session_allocate_encoding_metadata(
                        session, error, len(error)
                    ),
                    error,
                    "cannot allocate native D3D12 encoding metadata",
                )
                self._checked(
                    self._library.pano_gpu_session_upload_encoding_metadata(
                        session,
                        ctypes.cast(metadata_owner, ctypes.c_void_p),
                        len(encoding_metadata),
                        error,
                        len(error),
                    ),
                    error,
                    "cannot upload native D3D12 encoding metadata",
                )
            for function_name in (
                "pano_gpu_session_allocate_upload_slot",
                "pano_gpu_session_allocate_second_upload_slot",
            ):
                self._checked(
                    getattr(self._library, function_name)(session, error, len(error)),
                    error,
                    f"{function_name} failed",
                )
            for frame_index in range(frame_count):
                try:
                    frame = next(frame_iterator)
                except StopIteration as exc:
                    raise ValueError("D3D12 source stream ended before every rotation") from exc
                frame_view = memoryview(frame)
                if not frame_view.c_contiguous:
                    raise ValueError("D3D12 source frames must be C-contiguous")
                frame_owner = (
                    (ctypes.c_ubyte * frame_view.nbytes).from_buffer_copy(frame_view)
                    if frame_view.readonly
                    else (ctypes.c_ubyte * frame_view.nbytes).from_buffer(frame_view)
                )
                upload = _SourceUpload(
                    size=ctypes.sizeof(_SourceUpload),
                    abi_version=PANO_GPU_ABI_VERSION,
                    frame_index=frame_index,
                    source_sample_type=source_sample_type,
                    source_row_stride_bytes=source_row_stride_bytes,
                    data=ctypes.cast(frame_owner, ctypes.c_void_p),
                    data_bytes=frame_view.nbytes,
                )
                if frame_index == 0:
                    result = self._library.pano_gpu_session_upload_frame_zero(
                        session, ctypes.byref(upload), error, len(error)
                    )
                else:
                    result = self._library.pano_gpu_session_upload_frame_cancellable(
                        session, ctypes.byref(upload), token.handle, error, len(error)
                    )
                self._checked(result, error, f"cannot upload native D3D12 frame {frame_index}")
            try:
                next(frame_iterator)
            except StopIteration:
                pass
            else:
                raise ValueError("D3D12 source stream contains more frames than rotations")
            self._checked(
                self._library.pano_gpu_session_finish_uploads_cancellable(
                    session, token.handle, error, len(error)
                ),
                error,
                "cannot finish native D3D12 uploads",
            )
            prepared = D3D12PreparedSession(
                self,
                device.value or 0,
                session.value or 0,
                frame_count=frame_count,
                source_width=source_width,
                source_height=source_height,
                adapter_luid=adapter_luid,
            )
            device = ctypes.c_void_p()
            session = ctypes.c_void_p()
            return prepared
        finally:
            if session.value is not None:
                self._library.pano_gpu_session_destroy(ctypes.byref(session))
            if device.value is not None:
                self._library.pano_gpu_device_destroy(ctypes.byref(device))
            if owned_token:
                token.close()


def _default_library_path() -> Path:
    package_path = Path(__file__).with_name("pano_gpu.dll")
    if package_path.is_file() or not getattr(sys, "frozen", False):
        return package_path
    return Path(sys.executable).with_name("pano_gpu.dll")


def _load_windows_library(path: Path) -> ctypes.CDLL:
    windows_library: Any = getattr(ctypes, "WinDLL")
    kernel32 = windows_library("kernel32", use_last_error=True)
    set_thread_error_mode = kernel32.SetThreadErrorMode
    set_thread_error_mode.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    set_thread_error_mode.restype = ctypes.c_int
    previous_mode = ctypes.c_uint32()
    changed = bool(set_thread_error_mode(0x0001 | 0x8000, ctypes.byref(previous_mode)))
    try:
        return ctypes.CDLL(str(path))
    finally:
        if changed:
            set_thread_error_mode(previous_mode.value, None)


def load_d3d12_adapter(library_path: Path | None = None) -> D3D12Adapter:
    """Load the Windows DLL without making import on other platforms fail."""

    if os.name != "nt":
        raise D3D12AdapterUnavailableError("D3D12 is available only on Windows")
    path = library_path or _default_library_path()
    try:
        return D3D12Adapter(_load_windows_library(path))
    except OSError as error:
        raise D3D12AdapterUnavailableError(f"cannot load native D3D12 core: {error}") from error
