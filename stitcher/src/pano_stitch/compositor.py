"""Memory-bounded rendering of validated capture sessions."""

from __future__ import annotations

import binascii
import logging
import mmap
import os
import struct
import tempfile
import time
import zlib
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager, nullcontext
from dataclasses import dataclass, replace
from importlib import import_module
from pathlib import Path
from threading import Event
from typing import Any

import cv2
import numpy as np
from numpy.typing import NDArray
from PIL import Image

from pano_stitch.gpu import (
    CudaBandScheduler,
    CudaMemoryPlan,
    CudaOutputJob,
    CudaPreflightError,
    CudaRenderDiagnostics,
    CudaSession,
    GpuUnavailableError,
    compile_cuda_module,
    cuda_device_info,
    select_cuda_backend,
)
from pano_stitch.metadata import FrameMetadata, ImageEncoding, SessionMetadata
from pano_stitch.projection import (
    _frame_rotation,
    camera_maps,
    equirectangular_directions,
    rectilinear_directions,
    remap_source,
)

FloatImage = NDArray[np.float32]
LOGGER = logging.getLogger(__name__)
DEFAULT_MEMORY_BUDGET_BYTES = 1024 * 1024 * 1024
MAX_MEMORY_BUDGET_BYTES = 8192 * 1024 * 1024
_RESERVED_RUNTIME_BYTES = 192 * 1024 * 1024
_TILE_BYTES_PER_PIXEL = 164
_MAX_AUTO_WORKERS = 8
_EXPOSURE_FIELD_SCALE = 4
_AUTO_CONTRAST_BINS = 4096
_AUTO_CONTRAST_CLIP = 0.005


@dataclass(frozen=True)
class SourceInfo:
    """Dimensions and linear-light interpretation of one source image."""

    width: int
    height: int
    encoding: ImageEncoding


@dataclass(frozen=True)
class RenderResources:
    """The bounded compositor allocations selected for one render."""

    output_width: int
    output_height: int
    strip_height: int
    worker_count: int
    scratch_bytes: int


@dataclass(frozen=True)
class ExposureReport:
    """Summary of the automatic linear-light exposure solve."""

    anchor_frame: int
    edge_count: int
    gains: tuple[float, ...]

    @property
    def log_gains(self) -> tuple[float, ...]:
        return tuple(float(np.log(max(gain, 1e-6))) for gain in self.gains)


@dataclass(frozen=True)
class PreviewResult:
    """Displayable SDR preview pixels and the exposure solve used to create them."""

    pixels: NDArray[np.uint8]
    exposure_report: ExposureReport


@dataclass
class PreparedCudaSession:
    """Reusable resident CUDA inputs and their one-time exposure solve."""

    cuda_session: CudaSession
    exposure_report: ExposureReport
    upload_seconds: float
    exposure_seconds: float

    def close(self) -> None:
        self.cuda_session.close()


@dataclass(frozen=True)
class CudaSessionCacheKey:
    """Every immutable input that permits safe resident-session reuse."""

    device_name: str
    compute_capability: tuple[int, int] | None
    session_path: str
    source_fingerprints: tuple[tuple[str, int, int], ...]
    encoding: ImageEncoding
    capture_mode: str
    horizontal_fov_deg: float
    vertical_fov_deg: float
    overlap_fraction: float
    frames: tuple[FrameMetadata, ...]
    gpu_memory_budget_bytes: int | None


def cuda_session_cache_key(
    *,
    device_name: str,
    compute_capability: tuple[int, int] | None,
    session_path: Path,
    session: SessionMetadata,
    image_root: Path,
    gpu_memory_budget_bytes: int | None,
) -> CudaSessionCacheKey:
    """Fingerprint a validated CUDA session without decoding its source pixels."""

    fingerprints = []
    for frame in session.frames:
        source_path = _image_path(image_root, frame.filename).resolve()
        stat = source_path.stat()
        fingerprints.append((str(source_path), stat.st_size, stat.st_mtime_ns))
    return CudaSessionCacheKey(
        device_name,
        compute_capability,
        str(session_path.resolve()),
        tuple(fingerprints),
        session.image_encoding,
        session.capture_mode.value,
        session.horizontal_fov_deg,
        session.vertical_fov_deg,
        session.overlap_fraction,
        session.frames,
        gpu_memory_budget_bytes,
    )


class CudaSessionCache:
    """Single-entry owner for GUI reuse of one resident CUDA session."""

    def __init__(self) -> None:
        self._key: CudaSessionCacheKey | None = None
        self._prepared: PreparedCudaSession | None = None

    def get(self, key: CudaSessionCacheKey) -> PreparedCudaSession | None:
        if self._key == key and self._prepared is not None:
            stats = self._prepared.cuda_session.transfer_stats
            LOGGER.info("CUDA session cache hit: %d resident bytes", stats.peak_device_bytes)
            return self._prepared
        LOGGER.info("CUDA session cache miss")
        return None

    def store(self, key: CudaSessionCacheKey, prepared: PreparedCudaSession) -> None:
        if self._key == key and self._prepared is prepared:
            return
        self.invalidate("replaced")
        self._key = key
        self._prepared = prepared
        stats = prepared.cuda_session.transfer_stats
        LOGGER.info("CUDA session cache stored: %d resident bytes", stats.peak_device_bytes)

    def invalidate(self, reason: str = "invalidated") -> None:
        prepared = self._prepared
        self._key = None
        self._prepared = None
        if prepared is not None:
            LOGGER.info("CUDA session cache cleanup: %s", reason)
            prepared.close()

    def close(self) -> None:
        self.invalidate("shutdown")


class RenderCancelledError(RuntimeError):
    """Raised when a cooperative render cancellation is requested."""


@contextmanager
def _limit_opencv_threads(worker_count: int) -> Any:
    """Avoid nested OpenCV and strip-worker pools competing for the same cores."""

    if worker_count <= 1:
        yield
        return
    previous_threads = cv2.getNumThreads()
    cv2.setNumThreads(1)
    try:
        yield
    finally:
        cv2.setNumThreads(previous_threads)


def _exposure_luminance(image: FloatImage) -> FloatImage:
    return np.asarray(
        image[..., 0] * np.float32(0.2126)
        + image[..., 1] * np.float32(0.7152)
        + image[..., 2] * np.float32(0.0722),
        dtype=np.float32,
    )


def _exposure_clipped(image: FloatImage, encoding: ImageEncoding) -> NDArray[np.bool_]:
    """Return saturated-code samples, excluding unbounded linear HDR sources."""

    if encoding.transfer_function == "linear":
        return np.zeros(image.shape[:2], dtype=bool)
    return np.any(image >= np.float32(0.995), axis=-1)


def _exposure_proxy(source: FloatImage, maximum_width: int = 256) -> FloatImage:
    """Downsample one decoded source for the exposure-only geometry pass."""

    height, width = source.shape[:2]
    proxy_width = min(maximum_width, width)
    proxy_height = max(1, int(round(height * proxy_width / width)))
    if (proxy_width, proxy_height) == (width, height):
        return source
    return np.asarray(
        cv2.resize(source, (proxy_width, proxy_height), interpolation=cv2.INTER_AREA),
        dtype=np.float32,
    )


def _estimate_exposure_gains(
    session: SessionMetadata,
    image_root: Path,
    source_info: SourceInfo,
    cancel_event: Event | None = None,
    progress_callback: Callable[[int], None] | None = None,
) -> ExposureReport:
    """Solve robust per-frame gains from low-resolution geometric overlaps."""

    frame_count = len(session.frames)
    if frame_count <= 1:
        return ExposureReport(0, 0, (1.0,) * frame_count)

    sample_width = 256
    sample_height = 128
    latitude_span = (
        180.0 if session.capture_mode.value == "full_sphere" else session.vertical_fov_deg
    )
    directions = equirectangular_directions(sample_width, sample_height, latitude_span)
    coverage_masks: list[NDArray[np.bool_]] = []
    valid_masks: list[NDArray[np.bool_]] = []
    luminances: list[FloatImage] = []
    for position, frame in enumerate(session.frames):
        if cancel_event is not None and cancel_event.is_set():
            raise RenderCancelledError("render cancelled")
        source = _read_source(_image_path(image_root, frame.filename), source_info.encoding)
        try:
            proxy = _exposure_proxy(source)
            map_x, map_y, valid, _ = camera_maps(
                directions,
                frame,
                proxy.shape[1],
                proxy.shape[0],
                session.horizontal_fov_deg,
                session.vertical_fov_deg,
            )
            sampled = remap_source(proxy, map_x, map_y)
        finally:
            del source
        luminance = _exposure_luminance(sampled)
        finite = np.isfinite(luminance) & (luminance > np.float32(1e-5))
        clipped = _exposure_clipped(sampled, source_info.encoding)
        log_luminance = np.log(np.maximum(luminance, np.float32(1e-5)))
        gradient_x = cv2.Sobel(log_luminance, cv2.CV_32F, 1, 0, ksize=3)
        gradient_y = cv2.Sobel(log_luminance, cv2.CV_32F, 0, 1, ksize=3)
        gradient = np.hypot(gradient_x, gradient_y)
        gradient_limit = float(np.quantile(gradient[np.isfinite(gradient)], 0.9))
        luminances.append(luminance)
        coverage_masks.append(valid)
        valid_masks.append(valid & finite & ~clipped & (gradient <= gradient_limit))
        if progress_callback is not None:
            progress_callback(position + 1)

    equations: list[tuple[int, int, float, float]] = []
    adjacency: list[set[int]] = [set() for _ in session.frames]
    geometric_overlaps: list[tuple[int, int]] = []
    for left in range(frame_count):
        for right in range(left + 1, frame_count):
            if int(np.count_nonzero(coverage_masks[left] & coverage_masks[right])) >= 24:
                geometric_overlaps.append((left, right))
            mask = valid_masks[left] & valid_masks[right]
            if int(np.count_nonzero(mask)) < 24:
                continue
            left_values = luminances[left][mask]
            right_values = luminances[right][mask]
            log_ratios = np.log(left_values) - np.log(right_values)
            finite_ratios = log_ratios[np.isfinite(log_ratios)]
            if finite_ratios.size < 24:
                continue
            low, high = np.quantile(finite_ratios, (0.1, 0.9))
            inliers = finite_ratios[(finite_ratios >= low) & (finite_ratios <= high)]
            if inliers.size < 12:
                continue
            median = float(np.median(inliers))
            mad = float(np.median(np.abs(inliers - median)))
            if mad > 0.5:
                continue
            equations.append((left, right, median, float(np.sqrt(inliers.size) / (1.0 + mad))))
            adjacency[left].add(right)
            adjacency[right].add(left)

    # A dark or clipped overlap cannot estimate a relative gain, but it must not
    # make an otherwise valid capture unrenderable. Link such components with a
    # neutral constraint so their unmeasurable relationship remains unchanged.
    def reachable() -> set[int]:
        seen = {0}
        pending = [0]
        while pending:
            current = pending.pop()
            for neighbor in adjacency[current]:
                if neighbor not in seen:
                    seen.add(neighbor)
                    pending.append(neighbor)
        return seen

    connected = reachable()
    while True:
        bridge = next(
            (
                (left, right)
                for left, right in geometric_overlaps
                if (left in connected) != (right in connected)
            ),
            None,
        )
        if bridge is None:
            break
        left, right = bridge
        equations.append((left, right, 0.0, 1.0))
        adjacency[left].add(right)
        adjacency[right].add(left)
        connected = reachable()

    matrix = np.zeros((len(equations) + 1, frame_count), dtype=np.float64)
    values = np.zeros(len(equations) + 1, dtype=np.float64)
    for row, (left, right, ratio, weight) in enumerate(equations):
        matrix[row, left] = -weight
        matrix[row, right] = weight
        values[row] = ratio * weight
    matrix[-1, 0] = 1.0
    solution, *_ = np.linalg.lstsq(matrix, values, rcond=None)
    solution -= np.median(solution)
    limit = np.log(2.0)
    gains = np.exp(np.clip(solution, -limit, limit)).astype(np.float32)
    return ExposureReport(
        anchor_frame=int(np.argmin(np.abs(solution - np.median(solution)))),
        edge_count=len(equations),
        gains=tuple(float(gain) for gain in gains),
    )


def _png_chunks(path: Path) -> dict[bytes, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG file: {path}")
    chunks: dict[bytes, bytes] = {}
    offset = 8
    while offset + 12 <= len(data):
        size = struct.unpack(">I", data[offset : offset + 4])[0]
        kind = data[offset + 4 : offset + 8]
        end = offset + 12 + size
        if end > len(data):
            raise ValueError(f"truncated PNG chunk in {path}")
        chunks.setdefault(kind, data[offset + 8 : offset + 8 + size])
        offset = end
        if kind == b"IEND":
            break
    return chunks


def _pq_to_linear(value: FloatImage) -> FloatImage:
    """Decode normalized BT.2100 PQ values to linear 0..1 at 10,000 nits."""

    m1 = np.float32(2610.0 / 16384.0)
    m2 = np.float32(2523.0 / 32.0)
    c1 = np.float32(3424.0 / 4096.0)
    c2 = np.float32(2413.0 / 128.0)
    c3 = np.float32(2392.0 / 128.0)
    powered = np.power(np.maximum(value, 0.0), 1.0 / m2)
    numerator = np.maximum(powered - c1, 0.0)
    denominator = np.maximum(c2 - c3 * powered, np.finfo(np.float32).tiny)
    return np.asarray(np.power(numerator / denominator, 1.0 / m1), dtype=np.float32)


def _srgb_to_linear(value: FloatImage) -> FloatImage:
    return np.where(
        value <= 0.04045,
        value / 12.92,
        np.power((value + 0.055) / 1.055, 2.4),
    ).astype(np.float32)


def _linear_to_srgb(value: FloatImage) -> FloatImage:
    positive = np.maximum(value, 0.0)
    return np.where(
        positive <= 0.0031308,
        positive * 12.92,
        1.055 * np.power(positive, 1.0 / 2.4) - 0.055,
    ).astype(np.float32)


def _rec2020_to_srgb_linear(value: FloatImage) -> FloatImage:
    matrix = np.array(
        (
            (1.660491, -0.587641, -0.072850),
            (-0.124550, 1.132900, -0.008349),
            (-0.018151, -0.100579, 1.118730),
        ),
        dtype=np.float32,
    )
    return np.asarray(value @ matrix.T, dtype=np.float32)


def _tone_map_rec2020_to_srgb(linear: FloatImage, reference_white_nits: float) -> FloatImage:
    relative = np.maximum(linear, 0.0) * np.float32(10000.0 / reference_white_nits)
    luminance = relative @ np.array((0.2627, 0.6780, 0.0593), dtype=np.float32)
    mapped_luminance = luminance / (1.0 + luminance)
    scale = np.zeros_like(luminance, dtype=np.float32)
    np.divide(mapped_luminance, luminance, out=scale, where=luminance > 0.0)
    mapped = relative * scale[..., np.newaxis]
    return _rec2020_to_srgb_linear(mapped)


def _png_encoding(path: Path) -> ImageEncoding:
    chunks = _png_chunks(path)
    header = chunks.get(b"IHDR")
    if header is None or len(header) != 13:
        raise ValueError(f"invalid PNG header: {path}")
    sample_type = "uint16" if header[8] == 16 else "uint8"
    cicp = chunks.get(b"cICP")
    if cicp == bytes((9, 16, 0, 1)):
        return ImageEncoding("uint16", "rec2020", "pq", 203.0)
    if cicp is not None:
        raise ValueError(f"unsupported PNG cICP signaling {tuple(cicp)}: {path}")
    return ImageEncoding(sample_type, "srgb", "srgb")


def _probe_source(path: Path) -> SourceInfo:
    suffix = path.suffix.lower()
    if suffix == ".png":
        with Image.open(path) as image:
            width, height = image.size
        return SourceInfo(width, height, _png_encoding(path))
    if suffix == ".exr":
        module: Any = import_module("OpenEXR")
        image = module.InputFile(str(path))
        try:
            data_window = image.header()["dataWindow"]
            width = data_window.max.x - data_window.min.x + 1
            height = data_window.max.y - data_window.min.y + 1
        finally:
            image.close()
        return SourceInfo(width, height, ImageEncoding("float32", "rec2020", "linear", 203.0))
    if suffix in {".jpg", ".jpeg"}:
        with Image.open(path) as image:
            width, height = image.size
        return SourceInfo(width, height, ImageEncoding("uint8", "srgb", "srgb"))
    raise ValueError(f"unsupported source image format: {path.suffix}")


def _read_png(path: Path, encoding: ImageEncoding) -> FloatImage:
    raw = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if raw is None or raw.ndim != 3 or raw.shape[2] < 3:
        raise ValueError(f"cannot decode RGB PNG: {path}")
    raw = raw[..., :3]
    if raw.dtype == np.uint8:
        divisor = np.float32(255.0)
    elif raw.dtype == np.uint16:
        divisor = np.float32(65535.0)
    else:
        raise ValueError(f"unsupported PNG sample type {raw.dtype}: {path}")

    result = np.empty(raw.shape, dtype=np.float32)
    conversion = _pq_to_linear if encoding.transfer_function == "pq" else _srgb_to_linear
    for start in range(0, raw.shape[0], 128):
        stop = min(start + 128, raw.shape[0])
        values = raw[start:stop, :, ::-1].astype(np.float32) / divisor
        result[start:stop] = conversion(values)
    return result


def _read_exr(path: Path) -> FloatImage:
    module: Any = import_module("OpenEXR")
    with module.File(str(path)) as image:
        channels = image.channels()
        if "RGB" not in channels:
            raise ValueError(f"EXR must contain RGB channels: {path}")
        values = np.asarray(channels["RGB"].pixels, dtype=np.float32)
    if values.ndim != 3 or values.shape[2] != 3:
        raise ValueError(f"EXR RGB channel has an invalid shape: {path}")
    return values


def _read_jpeg(path: Path) -> FloatImage:
    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.float32) / np.float32(255.0)
    return _srgb_to_linear(rgb)


def _read_source(path: Path, encoding: ImageEncoding) -> FloatImage:
    if path.suffix.lower() == ".png":
        return _read_png(path, encoding)
    if path.suffix.lower() == ".exr":
        return _read_exr(path)
    if path.suffix.lower() in {".jpg", ".jpeg"}:
        return _read_jpeg(path)
    raise ValueError(f"unsupported source image format: {path.suffix}")


def _read_native_source(path: Path, encoding: ImageEncoding) -> NDArray[Any]:
    """Decode RGB source samples without CPU-side transfer conversion for CUDA."""

    suffix = path.suffix.lower()
    if suffix == ".png":
        raw = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if raw is None or raw.ndim != 3 or raw.shape[2] < 3:
            raise ValueError(f"cannot decode RGB PNG: {path}")
        expected_dtype = np.uint16 if encoding.sample_type == "uint16" else np.uint8
        if raw.dtype != expected_dtype:
            raise ValueError(f"PNG sample type {raw.dtype} does not match session encoding: {path}")
        return np.ascontiguousarray(raw[..., :3][..., ::-1])
    if suffix in {".jpg", ".jpeg"}:
        with Image.open(path) as image:
            return np.asarray(image.convert("RGB"), dtype=np.uint8)
    if suffix == ".exr":
        return _read_exr(path)
    raise ValueError(f"unsupported source image format: {path.suffix}")


class _ExrWriter:
    """Incremental float EXR writer backed by OpenEXR's scanline API."""

    def __init__(self, path: Path, width: int, height: int) -> None:
        module: Any = import_module("OpenEXR")
        imath: Any = import_module("Imath")
        header = module.Header(width, height)
        pixel_type = imath.PixelType(imath.PixelType.FLOAT)
        header["channels"] = {
            "R": imath.Channel(pixel_type),
            "G": imath.Channel(pixel_type),
            "B": imath.Channel(pixel_type),
        }
        header["compression"] = imath.Compression(imath.Compression.PIZ_COMPRESSION)
        self._output: Any = module.OutputFile(str(path), header)

    def __enter__(self) -> _ExrWriter:
        return self

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        self._output.close()

    def write(self, rows: FloatImage) -> None:
        contiguous = np.ascontiguousarray(rows, dtype=np.float32)
        self._output.writePixels(
            {
                "R": contiguous[..., 0].tobytes(),
                "G": contiguous[..., 1].tobytes(),
                "B": contiguous[..., 2].tobytes(),
            },
            contiguous.shape[0],
        )


def _write_exr(path: Path, image: FloatImage, _encoding: ImageEncoding) -> None:
    """Write a test image through the same scanline path as production."""

    with _ExrWriter(path, image.shape[1], image.shape[0]) as output:
        output.write(image)


def _to_sdr_srgb(rows: FloatImage, encoding: ImageEncoding) -> FloatImage:
    linear = np.maximum(rows, 0.0)
    if encoding.transfer_function == "pq":
        linear = _tone_map_rec2020_to_srgb(linear, encoding.reference_white_nits)
    return np.clip(_linear_to_srgb(linear), 0.0, 1.0)


def _auto_contrast_levels(
    color_scratch: FloatImage,
    weight_scratch: FloatImage,
    output_height: int,
    strip_height: int,
    encoding: ImageEncoding,
    cancel_event: Event | None,
    progress_callback: Callable[[int], None] | None = None,
) -> tuple[float, float] | None:
    """Find shared SDR black/white points without retaining the panorama."""

    histogram = np.zeros(_AUTO_CONTRAST_BINS, dtype=np.int64)
    for row_start in range(0, output_height, strip_height):
        if cancel_event is not None and cancel_event.is_set():
            raise RenderCancelledError("render cancelled")
        rows = min(strip_height, output_height - row_start)
        color = color_scratch[row_start : row_start + rows]
        valid = (weight_scratch[row_start : row_start + rows] > 0.0) & np.isfinite(color).all(
            axis=-1
        )
        if np.any(valid):
            rgb = _to_sdr_srgb(color, encoding)
            luminance = rgb @ np.array((0.2126, 0.7152, 0.0722), dtype=np.float32)
            values = luminance[valid]
            histogram += np.histogram(values, bins=_AUTO_CONTRAST_BINS, range=(0.0, 1.0))[0]
        if progress_callback is not None:
            progress_callback(row_start // strip_height + 1)
    total = int(histogram.sum())
    if total < 2:
        return None
    cumulative = np.cumsum(histogram)

    def percentile(fraction: float) -> float:
        rank = fraction * (total - 1)
        index = min(
            int(np.searchsorted(cumulative, rank + 1, side="left")),
            _AUTO_CONTRAST_BINS - 1,
        )
        previous = int(cumulative[index - 1]) if index else 0
        count = int(histogram[index])
        position = (rank - previous) / count if count else 0.0
        return (index + position) / _AUTO_CONTRAST_BINS

    black = percentile(_AUTO_CONTRAST_CLIP)
    white = percentile(1.0 - _AUTO_CONTRAST_CLIP)
    if (
        not np.isfinite(black)
        or not np.isfinite(white)
        or white - black < 1.0 / _AUTO_CONTRAST_BINS
    ):
        return None
    return black, white


class _PngWriter:
    """Streaming lossless SDR PNG writer with HDR tone mapping."""

    def __init__(
        self,
        path: Path,
        width: int,
        height: int,
        encoding: ImageEncoding,
        levels: tuple[float, float] | None = None,
    ) -> None:
        self._encoding = encoding
        self._levels = levels
        self._stream = path.open("wb")
        self._compressor = zlib.compressobj(level=9)
        self._stream.write(b"\x89PNG\r\n\x1a\n")
        self._write_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))

    def __enter__(self) -> _PngWriter:
        return self

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        try:
            tail = self._compressor.flush()
            if tail:
                self._write_chunk(b"IDAT", tail)
            self._write_chunk(b"IEND", b"")
        finally:
            self._stream.close()

    def _write_chunk(self, kind: bytes, data: bytes) -> None:
        self._stream.write(struct.pack(">I", len(data)))
        self._stream.write(kind)
        self._stream.write(data)
        self._stream.write(struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF))

    def write(self, rows: FloatImage) -> None:
        rgb = _to_sdr_srgb(rows, self._encoding)
        if self._levels is not None:
            black, white = self._levels
            rgb = np.clip((rgb - np.float32(black)) / np.float32(white - black), 0.0, 1.0)
        rgb = np.round(rgb * 255.0).astype(np.uint8)
        for row in np.ascontiguousarray(rgb):
            compressed = self._compressor.compress(b"\0" + row.tobytes())
            if compressed:
                self._write_chunk(b"IDAT", compressed)


class _JpegWriter:
    """Disk-spooled SDR JPEG writer that avoids a second full RAM image."""

    def __init__(
        self,
        path: Path,
        width: int,
        height: int,
        encoding: ImageEncoding,
        quality: int,
        levels: tuple[float, float] | None = None,
    ) -> None:
        self._path = path
        self._width = width
        self._height = height
        self._encoding = encoding
        self._quality = quality
        self._levels = levels
        self._raw_path = path.with_suffix(".rgb")
        self._stream = self._raw_path.open("wb")

    def __enter__(self) -> _JpegWriter:
        return self

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        self._stream.close()
        try:
            if exception_type is not None:
                return
            with self._raw_path.open("rb") as raw_stream:
                with mmap.mmap(raw_stream.fileno(), 0, access=mmap.ACCESS_READ) as raw:
                    raw_buffer: Any = raw
                    image = Image.frombuffer(
                        "RGB", (self._width, self._height), raw_buffer, "raw", "RGB", 0, 1
                    )
                    try:
                        image.save(
                            self._path,
                            format="JPEG",
                            quality=self._quality,
                            subsampling=0,
                            optimize=False,
                            progressive=False,
                        )
                    finally:
                        image.close()
        finally:
            self._raw_path.unlink(missing_ok=True)

    def write(self, rows: FloatImage) -> None:
        rgb = _to_sdr_srgb(rows, self._encoding)
        if self._levels is not None:
            black, white = self._levels
            rgb = np.clip((rgb - np.float32(black)) / np.float32(white - black), 0.0, 1.0)
        rgb = np.round(rgb * 255.0).astype(np.uint8)
        self._stream.write(np.ascontiguousarray(rgb).tobytes())


class _CoverageWriter:
    """Streaming grayscale PNG writer for covered (white) output pixels."""

    def __init__(self, path: Path, width: int, height: int) -> None:
        self._stream = path.open("wb")
        self._compressor = zlib.compressobj(level=9)
        self._stream.write(b"\x89PNG\r\n\x1a\n")
        self._write_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))

    def __enter__(self) -> _CoverageWriter:
        return self

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        try:
            tail = self._compressor.flush()
            if tail:
                self._write_chunk(b"IDAT", tail)
            self._write_chunk(b"IEND", b"")
        finally:
            self._stream.close()

    def _write_chunk(self, kind: bytes, data: bytes) -> None:
        self._stream.write(struct.pack(">I", len(data)))
        self._stream.write(kind)
        self._stream.write(data)
        self._stream.write(struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF))

    def write(self, covered: NDArray[np.bool_]) -> None:
        rows = np.where(covered, np.uint8(255), np.uint8(0))
        for row in np.ascontiguousarray(rows):
            compressed = self._compressor.compress(b"\0" + row.tobytes())
            if compressed:
                self._write_chunk(b"IDAT", compressed)


def _available_tile_bytes(source: SourceInfo, output_width: int, memory_budget_bytes: int) -> int:
    if memory_budget_bytes <= 0 or memory_budget_bytes > MAX_MEMORY_BUDGET_BYTES:
        maximum_mib = MAX_MEMORY_BUDGET_BYTES // (1024 * 1024)
        raise ValueError(f"memory budget must be between 1 and {maximum_mib} MiB")
    source_working_set = source.width * source.height * 3 * np.dtype(np.float32).itemsize * 3
    available = memory_budget_bytes - source_working_set - _RESERVED_RUNTIME_BYTES
    if available < output_width * _TILE_BYTES_PER_PIXEL:
        raise ValueError("memory budget is too small for one output row and one decoded source")
    return available


def _choose_render_plan(
    source: SourceInfo,
    output_width: int,
    output_height: int,
    memory_budget_bytes: int,
    workers: int | None,
) -> tuple[int, int]:
    available = _available_tile_bytes(source, output_width, memory_budget_bytes)
    bytes_per_worker_row = output_width * _TILE_BYTES_PER_PIXEL
    maximum_workers = max(1, available // bytes_per_worker_row)
    if workers is not None and workers < 1:
        raise ValueError("workers must be at least 1")
    requested_workers = workers or min(os.cpu_count() or 1, _MAX_AUTO_WORKERS)
    worker_count = min(requested_workers, maximum_workers)
    strip_height = max(1, available // (worker_count * bytes_per_worker_row))
    return worker_count, min(strip_height, output_height)


def _choose_strip_height(source: SourceInfo, output_width: int, memory_budget_bytes: int) -> int:
    """Return the legacy single-worker strip height used by resource tests."""

    _, strip_height = _choose_render_plan(
        source, output_width, 2**31 - 1, memory_budget_bytes, workers=1
    )
    return strip_height


def _composite_strip(
    color_scratch: FloatImage,
    weight_scratch: FloatImage,
    source: FloatImage,
    frame: FrameMetadata,
    source_info: SourceInfo,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
    latitude_span: float,
    output_width: int,
    output_height: int,
    row_start: int,
    strip_height: int,
    blend: str,
    cancel_event: Event | None,
    local_exposure: FloatImage | None = None,
    log_gain: float = 0.0,
) -> None:
    """Composite one row-disjoint strip in disk-backed shared array views."""

    if cancel_event is not None and cancel_event.is_set():
        raise RenderCancelledError("render cancelled")
    rows = min(strip_height, output_height - row_start)
    directions = equirectangular_directions(
        output_width, rows, latitude_span, row_start, output_height
    )
    map_x, map_y, valid, edge_distance = camera_maps(
        directions,
        frame,
        source_info.width,
        source_info.height,
        horizontal_fov_deg,
        vertical_fov_deg,
    )
    color = color_scratch[row_start : row_start + rows]
    weight = weight_scratch[row_start : row_start + rows]
    sampled = remap_source(source, map_x, map_y)
    if local_exposure is not None:
        correction = _local_exposure_multiplier(
            log_gain,
            _local_exposure_rows(local_exposure, row_start, rows, output_width, output_height),
        )
        sampled *= correction[..., np.newaxis]
    if blend == "hard":
        candidate = np.where(valid, np.maximum(edge_distance, 1e-6), 0.0)
        selected = candidate > weight
        color[selected] = sampled[selected]
        weight[selected] = candidate[selected]
    else:
        candidate = _exposure_weight(valid, edge_distance, source_info)
        color += sampled * candidate[..., np.newaxis]
        weight += candidate


def _exposure_weight(
    valid: NDArray[np.bool_], edge_distance: FloatImage, source_info: SourceInfo
) -> FloatImage:
    feather_width = max(1.0, min(source_info.width, source_info.height) * 0.08)
    return np.where(valid, np.maximum(edge_distance / feather_width, 1e-6), 0.0).astype(np.float32)


def _local_exposure_multiplier(log_gain: float, local_exposure: FloatImage) -> FloatImage:
    """Return overlap-only compensation; a constant exposure gauge cancels."""

    return np.exp(np.float32(log_gain) - local_exposure).astype(np.float32)


def _exposure_field_dimensions(output_width: int, output_height: int) -> tuple[int, int]:
    return (
        max(1, (output_width + _EXPOSURE_FIELD_SCALE - 1) // _EXPOSURE_FIELD_SCALE),
        max(1, (output_height + _EXPOSURE_FIELD_SCALE - 1) // _EXPOSURE_FIELD_SCALE),
    )


def _local_exposure_rows(
    field: FloatImage,
    row_start: int,
    rows: int,
    output_width: int,
    output_height: int,
) -> FloatImage:
    """Bilinearly expand a quarter-resolution field for one output strip."""

    if field.shape == (output_height, output_width):
        return field[row_start : row_start + rows]
    field_height, field_width = field.shape
    x = (np.arange(output_width, dtype=np.float32) + 0.5) * field_width / output_width - 0.5
    y = (
        np.arange(row_start, row_start + rows, dtype=np.float32) + 0.5
    ) * field_height / output_height - 0.5
    x = np.clip(x, 0.0, field_width - 1)
    y = np.clip(y, 0.0, field_height - 1)
    map_x, map_y = np.meshgrid(x, y)
    sampled = remap_source(field[..., np.newaxis], map_x, map_y)
    return sampled if sampled.ndim == 2 else sampled[..., 0]


def _close_scratch_memmap(array: Any) -> None:
    """Flush and release a scratch mapping before Windows deletes its backing file."""

    array.flush()
    mapping = array._mmap
    if mapping is not None:
        mapping.close()


def _image_path(image_root: Path, filename: str) -> Path:
    path = Path(filename)
    return path if path.is_absolute() else image_root / path


def _output_dimensions(
    session: SessionMetadata, source_width: int, width: int | None
) -> tuple[int, int]:
    if width is None:
        focal_x = source_width / (2.0 * np.tan(np.radians(session.horizontal_fov_deg) / 2.0))
        width = max(2, int(round(2.0 * np.pi * focal_x)))
    if session.capture_mode.value == "full_sphere":
        width = max(2, width - width % 2)
        return width, width // 2
    latitude_span = (
        180.0 if session.capture_mode.value == "full_sphere" else session.vertical_fov_deg
    )
    return width, max(1, int(round(width * latitude_span / 360.0)))


def _temporary_output_path(output_path: Path) -> Path:
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{output_path.name}.", suffix=".partial", dir=output_path.parent
    )
    os.close(descriptor)
    path = Path(temporary)
    path.unlink()
    return path


def thumbnail_output_path(output_path: Path) -> Path:
    """Return the derived session-thumbnail path for a panorama output."""

    return output_path.with_name(f"{output_path.stem}-thumbnail{output_path.suffix}")


def _render_thumbnail(
    session: SessionMetadata,
    image_root: Path,
    output_path: Path,
    source: SourceInfo,
    blend: str,
    jpeg_quality: int,
    cancel_event: Event | None,
    log_gains: tuple[float, ...],
    allow_incomplete: bool,
    auto_contrast: bool,
    memory_budget_bytes: int,
    workers: int | None,
    progress_callback: Callable[[int, int, str], None] | None = None,
) -> Path:
    width, height = source.width, source.height
    vertical_fov = np.degrees(2.0 * np.arctan((height / width) * np.tan(np.pi / 4.0)))
    worker_count, strip_height = _choose_render_plan(
        source, width, height, memory_budget_bytes, workers
    )
    strip_count = (height + strip_height - 1) // strip_height
    with tempfile.TemporaryDirectory(prefix="pano-thumbnail-", dir=output_path.parent) as scratch:
        color = np.memmap(
            Path(scratch) / "color.f32", mode="w+", dtype=np.float32, shape=(height, width, 3)
        )
        weights = np.memmap(
            Path(scratch) / "weights.f32", mode="w+", dtype=np.float32, shape=(height, width)
        )
        exposure = np.memmap(
            Path(scratch) / "exposure.f32", mode="w+", dtype=np.float32, shape=(height, width)
        )
        exposure_weights = np.memmap(
            Path(scratch) / "exposure-weights.f32",
            mode="w+",
            dtype=np.float32,
            shape=(height, width),
        )
        color[:] = 0.0
        weights[:] = 0.0
        exposure[:] = 0.0
        exposure_weights[:] = 0.0
        temporary: Path | None = None
        try:
            exposure_completed = 0
            for frame_position, frame in enumerate(session.frames):

                def map_thumbnail_exposure(row_start: int) -> None:
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    rows = min(strip_height, height - row_start)
                    directions = rectilinear_directions(
                        width,
                        rows,
                        90.0,
                        float(vertical_fov),
                        row_offset=row_start,
                        full_height=height,
                    )
                    _, _, valid, edge_distance = camera_maps(
                        directions,
                        frame,
                        source.width,
                        source.height,
                        session.horizontal_fov_deg,
                        session.vertical_fov_deg,
                    )
                    exposure_weight = _exposure_weight(valid, edge_distance, source)
                    row_slice = slice(row_start, row_start + rows)
                    exposure[row_slice] += exposure_weight * np.float32(log_gains[frame_position])
                    exposure_weights[row_slice] += exposure_weight

                with _limit_opencv_threads(worker_count):
                    with ThreadPoolExecutor(max_workers=worker_count) as executor:
                        futures = [
                            executor.submit(map_thumbnail_exposure, row_start)
                            for row_start in range(0, height, strip_height)
                        ]
                        for future in as_completed(futures):
                            future.result()
                            exposure_completed += 1
                        if progress_callback is not None:
                            progress_callback(
                                exposure_completed,
                                len(session.frames) * strip_count,
                                "thumbnail exposure",
                            )
            for row_start in range(0, height, strip_height):
                rows = min(strip_height, height - row_start)
                row_slice = slice(row_start, row_start + rows)
                covered_exposure = exposure_weights[row_slice] > 0.0
                exposure_rows = exposure[row_slice]
                exposure_rows[covered_exposure] /= exposure_weights[row_slice][covered_exposure]

            compositing_completed = 0
            for frame_position, frame in enumerate(session.frames):
                decoded = _read_source(_image_path(image_root, frame.filename), source.encoding)

                def composite_thumbnail_strip(row_start: int) -> None:
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    rows = min(strip_height, height - row_start)
                    row_slice = slice(row_start, row_start + rows)
                    directions = rectilinear_directions(
                        width,
                        rows,
                        90.0,
                        float(vertical_fov),
                        row_offset=row_start,
                        full_height=height,
                    )
                    map_x, map_y, valid, edge_distance = camera_maps(
                        directions,
                        frame,
                        source.width,
                        source.height,
                        session.horizontal_fov_deg,
                        session.vertical_fov_deg,
                    )
                    sampled = remap_source(decoded, map_x, map_y)
                    sampled *= np.exp(np.float32(log_gains[frame_position]) - exposure[row_slice])[
                        ..., np.newaxis
                    ]
                    candidate = (
                        np.where(valid, np.maximum(edge_distance, 1e-6), 0.0)
                        if blend == "hard"
                        else _exposure_weight(valid, edge_distance, source)
                    )
                    color_rows = color[row_slice]
                    weight_rows = weights[row_slice]
                    if blend == "hard":
                        selected = candidate > weight_rows
                        color_rows[selected] = sampled[selected]
                        weight_rows[selected] = candidate[selected]
                    else:
                        color_rows += sampled * candidate[..., np.newaxis]
                        weight_rows += candidate

                with _limit_opencv_threads(worker_count):
                    with ThreadPoolExecutor(max_workers=worker_count) as executor:
                        futures = [
                            executor.submit(composite_thumbnail_strip, row_start)
                            for row_start in range(0, height, strip_height)
                        ]
                        for future in as_completed(futures):
                            future.result()
                            compositing_completed += 1
                        if progress_callback is not None:
                            progress_callback(
                                compositing_completed,
                                len(session.frames) * strip_count,
                                "thumbnail compositing",
                            )

            for row_start in range(0, height, strip_height):
                rows = min(strip_height, height - row_start)
                row_slice = slice(row_start, row_start + rows)
                color_rows = color[row_slice]
                weight_rows = weights[row_slice]
                covered = weight_rows > 0.0
                uncovered = int(np.count_nonzero(~covered))
                if uncovered and not allow_incomplete:
                    raise ValueError(f"capture does not cover {uncovered} thumbnail pixels")
                if uncovered:
                    color_rows[~covered] = np.array((1.0, 0.0, 1.0), dtype=np.float32)
                if blend == "feather":
                    color_rows[covered] /= weight_rows[covered, np.newaxis]

            levels = (
                _auto_contrast_levels(
                    color,
                    weights,
                    height,
                    strip_height,
                    source.encoding,
                    cancel_event,
                )
                if auto_contrast and output_path.suffix.lower() in {".png", ".jpg", ".jpeg"}
                else None
            )
            temporary = _temporary_output_path(output_path)
            suffix = output_path.suffix.lower()
            if suffix == ".exr":
                writer: Any = _ExrWriter(temporary, width, height)
            elif suffix == ".png":
                writer = _PngWriter(temporary, width, height, source.encoding, levels)
            else:
                writer = _JpegWriter(
                    temporary, width, height, source.encoding, jpeg_quality, levels
                )
            with writer:
                for row_start in range(0, height, strip_height):
                    rows = min(strip_height, height - row_start)
                    writer.write(color[row_start : row_start + rows])
            return temporary
        except Exception:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            raise
        finally:
            for mapping in (color, weights, exposure, exposure_weights):
                _close_scratch_memmap(mapping)


def _source_info_for_session(session: SessionMetadata, image_root: Path) -> SourceInfo:
    first_source = _probe_source(_image_path(image_root, session.frames[0].filename))
    for frame in session.frames[1:]:
        source_info = _probe_source(_image_path(image_root, frame.filename))
        if source_info != first_source:
            raise ValueError("all source images must use identical dimensions and color encoding")
    if (
        session.image_encoding != ImageEncoding()
        and session.image_encoding != first_source.encoding
    ):
        raise ValueError("session image_encoding does not match source images")
    return first_source


def renderable_session(
    session: SessionMetadata, image_root: Path, allow_incomplete: bool = False
) -> SessionMetadata:
    """Return only captured, available frames when rendering an incomplete session."""

    if not allow_incomplete:
        return session
    frames = tuple(
        frame
        for frame in session.frames
        if frame.status == "captured" and _image_path(image_root, frame.filename).is_file()
    )
    if not frames:
        raise ValueError("incomplete session contains no captured source images")
    return replace(session, frames=frames)


def estimate_render_resources(
    session: SessionMetadata,
    image_root: Path,
    width: int | None = None,
    memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    workers: int | None = None,
) -> RenderResources:
    """Return the output and scratch requirements without decoding source pixels."""

    if not session.frames:
        raise ValueError("session contains no frames")
    source = _source_info_for_session(session, image_root)
    output_width, output_height = _output_dimensions(session, source.width, width)
    exposure_width, exposure_height = _exposure_field_dimensions(output_width, output_height)
    worker_count, strip_height = _choose_render_plan(
        source, output_width, output_height, memory_budget_bytes, workers
    )
    scratch_bytes = (
        output_height * output_width * 4 + exposure_height * exposure_width
    ) * np.dtype(np.float32).itemsize
    return RenderResources(output_width, output_height, strip_height, worker_count, scratch_bytes)


def _render_cpu(
    session: SessionMetadata,
    image_root: Path,
    output_path: Path,
    width: int | None = None,
    blend: str = "hard",
    allow_incomplete: bool = False,
    memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    progress_callback: Callable[[int, int, str], None] | None = None,
    debug_coverage_path: Path | None = None,
    cancel_event: Event | None = None,
    jpeg_quality: int = 95,
    workers: int | None = None,
    auto_contrast: bool = True,
    session_thumbnail: bool = False,
    exposure_report: ExposureReport | None = None,
    use_gpu: bool | None = True,
    gpu_memory_budget_bytes: int | None = None,
    backend_callback: Callable[[str, str], None] | None = None,
) -> ExposureReport:
    """Render one session with the legacy bounded-RAM CPU pipeline."""

    if not session.frames:
        raise ValueError("session contains no frames")
    if blend not in {"hard", "feather"}:
        raise ValueError("blend must be 'hard' or 'feather'")
    if not 1 <= jpeg_quality <= 100:
        raise ValueError("jpeg quality must be between 1 and 100")
    if not allow_incomplete and (
        not session.completed or any(frame.status != "captured" for frame in session.frames)
    ):
        raise ValueError("session contains frames that are not captured")

    session = renderable_session(session, image_root, allow_incomplete)

    first_source = _source_info_for_session(session, image_root)
    output_suffix = output_path.suffix.lower()
    if output_suffix not in {".exr", ".jpg", ".jpeg", ".png"}:
        raise ValueError("output extension must be .exr, .jpg, .jpeg, or .png")
    resources = estimate_render_resources(session, image_root, width, memory_budget_bytes, workers)
    output_width = resources.output_width
    output_height = resources.output_height
    strip_height = resources.strip_height
    composite_strips = (output_height + strip_height - 1) // strip_height
    auto_contrast_active = auto_contrast and output_suffix in {".png", ".jpg", ".jpeg"}
    phase_count = 5 if auto_contrast_active else 4
    phase_labels = {
        "exposure": f"[1/{phase_count}] exposure",
        "exposure mapping": f"[2/{phase_count}] exposure mapping",
        "compositing": f"[3/{phase_count}] compositing",
        "auto contrast": f"[4/{phase_count}] auto contrast",
        "writing": f"[{5 if auto_contrast_active else 4}/{phase_count}] writing",
    }
    exposure_width, exposure_height = _exposure_field_dimensions(output_width, output_height)
    exposure_strip_height = max(
        1, (strip_height + _EXPOSURE_FIELD_SCALE - 1) // _EXPOSURE_FIELD_SCALE
    )
    exposure_strips = (exposure_height + exposure_strip_height - 1) // exposure_strip_height
    exposure_work = len(session.frames)
    local_exposure_work = (len(session.frames) + 1) * exposure_strips
    compositing_work = len(session.frames) * composite_strips
    latitude_span = (
        180.0 if session.capture_mode.value == "full_sphere" else session.vertical_fov_deg
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if debug_coverage_path is not None:
        debug_coverage_path.parent.mkdir(parents=True, exist_ok=True)
        if debug_coverage_path.resolve() == output_path.resolve():
            raise ValueError("debug coverage path must differ from output path")

    with tempfile.TemporaryDirectory(
        prefix="pano-stitch-", dir=output_path.parent
    ) as scratch_directory:
        scratch_root = Path(scratch_directory)
        backend_detail = "GPU acceleration disabled"
        LOGGER.info("render backend selected: CPU (%s)", backend_detail)
        if backend_callback is not None:
            backend_callback("cpu", backend_detail)
        if exposure_report is None:
            exposure_report = _estimate_exposure_gains(
                session,
                image_root,
                first_source,
                cancel_event,
                (
                    (
                        lambda completed: progress_callback(
                            completed, exposure_work, phase_labels["exposure"]
                        )
                    )
                    if progress_callback is not None
                    else None
                ),
            )
        elif len(exposure_report.gains) != len(session.frames):
            raise ValueError("cached exposure report does not match the renderable session")
        elif progress_callback is not None:
            progress_callback(exposure_work, exposure_work, f"{phase_labels['exposure']} (cached)")
        local_exposure_path = scratch_root / "local-exposure.f32"
        local_weight_path = scratch_root / "local-exposure-weight.f32"
        local_exposure: Any | None = None
        local_weight: Any | None = None
        try:
            local_exposure = np.memmap(
                local_exposure_path,
                mode="w+",
                dtype=np.float32,
                shape=(exposure_height, exposure_width),
            )
            local_weight = np.memmap(
                local_weight_path,
                mode="w+",
                dtype=np.float32,
                shape=(exposure_height, exposure_width),
            )
            local_exposure[:] = 0.0
            local_weight[:] = 0.0
            mapping_completed = 0
            for frame_position, frame in enumerate(session.frames):
                for row_start in range(0, exposure_height, exposure_strip_height):
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    rows = min(exposure_strip_height, exposure_height - row_start)
                    directions = equirectangular_directions(
                        exposure_width, rows, latitude_span, row_start, exposure_height
                    )
                    _, _, valid, edge_distance = camera_maps(
                        directions,
                        frame,
                        first_source.width,
                        first_source.height,
                        session.horizontal_fov_deg,
                        session.vertical_fov_deg,
                    )
                    weights = _exposure_weight(valid, edge_distance, first_source)
                    local_exposure[row_start : row_start + rows] += weights * np.float32(
                        exposure_report.log_gains[frame_position]
                    )
                    local_weight[row_start : row_start + rows] += weights
                    mapping_completed += 1
                    if progress_callback is not None:
                        progress_callback(
                            mapping_completed, local_exposure_work, phase_labels["exposure mapping"]
                        )
            for row_start in range(0, exposure_height, exposure_strip_height):
                if cancel_event is not None and cancel_event.is_set():
                    raise RenderCancelledError("render cancelled")
                rows = min(exposure_strip_height, exposure_height - row_start)
                exposure_rows = local_exposure[row_start : row_start + rows]
                weight_rows = local_weight[row_start : row_start + rows]
                covered_exposure = weight_rows > 0.0
                exposure_rows[covered_exposure] /= weight_rows[covered_exposure]
                mapping_completed += 1
                if progress_callback is not None:
                    progress_callback(
                        mapping_completed,
                        local_exposure_work,
                        phase_labels["exposure mapping"],
                    )
            local_exposure.flush()
        except Exception:
            if local_weight is not None:
                _close_scratch_memmap(local_weight)
            if local_exposure is not None:
                _close_scratch_memmap(local_exposure)
            raise
        _close_scratch_memmap(local_weight)
        del local_weight
        assert local_exposure is not None
        color_path = scratch_root / "color.f32"
        weight_path = scratch_root / "weight.f32"
        color_scratch: Any | None = None
        weight_scratch: Any | None = None
        try:
            color_scratch = np.memmap(
                color_path,
                mode="w+",
                dtype=np.float32,
                shape=(output_height, output_width, 3),
            )
            weight_scratch = np.memmap(
                weight_path,
                mode="w+",
                dtype=np.float32,
                shape=(output_height, output_width),
            )
        except Exception:
            if color_scratch is not None:
                _close_scratch_memmap(color_scratch)
            if weight_scratch is not None:
                _close_scratch_memmap(weight_scratch)
            _close_scratch_memmap(local_exposure)
            raise
        assert color_scratch is not None
        assert weight_scratch is not None
        try:
            compositor_workers = resources.worker_count
            with _limit_opencv_threads(compositor_workers):
                compositing_completed = 0
                for frame_position, frame in enumerate(session.frames):
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    source = _read_source(
                        _image_path(image_root, frame.filename), first_source.encoding
                    )
                    try:
                        with ThreadPoolExecutor(max_workers=compositor_workers) as executor:
                            futures = [
                                executor.submit(
                                    _composite_strip,
                                    color_scratch,
                                    weight_scratch,
                                    source,
                                    frame,
                                    first_source,
                                    session.horizontal_fov_deg,
                                    session.vertical_fov_deg,
                                    latitude_span,
                                    output_width,
                                    output_height,
                                    row_start,
                                    strip_height,
                                    blend,
                                    cancel_event,
                                    local_exposure,
                                    exposure_report.log_gains[frame_position],
                                )
                                for row_start in range(0, output_height, strip_height)
                            ]
                            for future in as_completed(futures):
                                future.result()
                                compositing_completed += 1
                                if progress_callback is not None:
                                    progress_callback(
                                        compositing_completed,
                                        compositing_work,
                                        phase_labels["compositing"],
                                    )
                    finally:
                        del source

            auto_levels: tuple[float, float] | None = None
            if auto_contrast_active:
                auto_completed = 0
                for row_start in range(0, output_height, strip_height):
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    rows = min(strip_height, output_height - row_start)
                    color_rows = color_scratch[row_start : row_start + rows]
                    weight_rows = weight_scratch[row_start : row_start + rows]
                    covered = weight_rows > 0.0
                    if blend == "feather":
                        color_rows[covered] /= weight_rows[covered, np.newaxis]
                    uncovered = int(np.count_nonzero(~covered))
                    if uncovered and not allow_incomplete:
                        raise ValueError(f"capture does not cover {uncovered} output pixels")
                    if uncovered and allow_incomplete:
                        color_rows[~covered] = np.array((1.0, 0.0, 1.0), dtype=np.float32)
                    auto_completed += 1
                    if progress_callback is not None:
                        progress_callback(
                            auto_completed,
                            2 * composite_strips,
                            phase_labels["auto contrast"],
                        )
                auto_levels = _auto_contrast_levels(
                    color_scratch,
                    weight_scratch,
                    output_height,
                    strip_height,
                    first_source.encoding,
                    cancel_event,
                    (
                        lambda completed: (
                            progress_callback(
                                auto_completed + completed,
                                2 * composite_strips,
                                phase_labels["auto contrast"],
                            )
                            if progress_callback is not None
                            else None
                        )
                    ),
                )
                auto_completed += composite_strips

            temporary_path = _temporary_output_path(output_path)
            temporary_coverage_path = (
                _temporary_output_path(debug_coverage_path)
                if debug_coverage_path is not None
                else None
            )
            temporary_thumbnail_path: Path | None = None
            try:
                if output_suffix == ".exr":
                    writer: _ExrWriter | _JpegWriter | _PngWriter = _ExrWriter(
                        temporary_path, output_width, output_height
                    )
                elif output_suffix == ".png":
                    writer = _PngWriter(
                        temporary_path,
                        output_width,
                        output_height,
                        first_source.encoding,
                        auto_levels,
                    )
                else:
                    writer = _JpegWriter(
                        temporary_path,
                        output_width,
                        output_height,
                        first_source.encoding,
                        jpeg_quality,
                        auto_levels,
                    )
                coverage_writer = (
                    _CoverageWriter(temporary_coverage_path, output_width, output_height)
                    if temporary_coverage_path is not None
                    else None
                )
                coverage_context = coverage_writer if coverage_writer is not None else nullcontext()
                with writer, coverage_context:
                    writing_completed = 0
                    for row_start in range(0, output_height, strip_height):
                        if cancel_event is not None and cancel_event.is_set():
                            raise RenderCancelledError("render cancelled")
                        rows = min(strip_height, output_height - row_start)
                        color_rows = color_scratch[row_start : row_start + rows]
                        weight_rows = weight_scratch[row_start : row_start + rows]
                        try:
                            covered = weight_rows > 0.0
                            if coverage_writer is not None:
                                coverage_writer.write(covered)
                            if blend == "feather" and not auto_contrast_active:
                                color_rows[covered] /= weight_rows[covered, np.newaxis]
                            uncovered = int(np.count_nonzero(~covered))
                            if uncovered and not allow_incomplete:
                                raise ValueError(
                                    f"capture does not cover {uncovered} output pixels"
                                )
                            if uncovered and allow_incomplete:
                                color_rows[~covered] = np.array((1.0, 0.0, 1.0), dtype=np.float32)
                            writer.write(color_rows)
                            writing_completed += 1
                            if progress_callback is not None:
                                progress_callback(
                                    writing_completed, composite_strips, phase_labels["writing"]
                                )
                        finally:
                            del color_rows
                            del weight_rows
                if session_thumbnail:
                    temporary_thumbnail_path = _render_thumbnail(
                        session,
                        image_root,
                        thumbnail_output_path(output_path),
                        first_source,
                        blend,
                        jpeg_quality,
                        cancel_event,
                        exposure_report.log_gains,
                        allow_incomplete,
                        auto_contrast,
                        memory_budget_bytes,
                        workers,
                        (
                            lambda completed, total, phase: (
                                progress_callback(completed, total, f"[thumbnail] {phase}")
                                if progress_callback is not None
                                else None
                            )
                        ),
                    )
                os.replace(temporary_path, output_path)
                if temporary_coverage_path is not None and debug_coverage_path is not None:
                    os.replace(temporary_coverage_path, debug_coverage_path)
                if temporary_thumbnail_path is not None:
                    os.replace(temporary_thumbnail_path, thumbnail_output_path(output_path))
            except Exception:
                temporary_path.unlink(missing_ok=True)
                if temporary_coverage_path is not None:
                    temporary_coverage_path.unlink(missing_ok=True)
                if temporary_thumbnail_path is not None:
                    temporary_thumbnail_path.unlink(missing_ok=True)
                raise
            return exposure_report
        finally:
            _close_scratch_memmap(color_scratch)
            _close_scratch_memmap(weight_scratch)
            _close_scratch_memmap(local_exposure)
            del color_scratch
            del weight_scratch
            del local_exposure


def _write_cuda_sdr_output(
    path: Path, pixels: NDArray[np.uint8], suffix: str, jpeg_quality: int
) -> None:
    """Encode final CUDA RGB bytes directly, without an intermediate image spool."""

    image = Image.fromarray(pixels, mode="RGB")
    if suffix == ".png":
        image.save(path, format="PNG")
    else:
        image.save(path, format="JPEG", quality=jpeg_quality, subsampling=0)


def _render_cuda_thumbnail(
    cuda_session: CudaSession,
    session: SessionMetadata,
    source: SourceInfo,
    output_path: Path,
    blend: str,
    allow_incomplete: bool,
    output_suffix: str,
    jpeg_quality: int,
    auto_contrast: bool,
) -> None:
    """Encode the rectilinear thumbnail from already resident CUDA session data."""

    width, height = source.width, source.height
    vertical_fov = float(np.degrees(2.0 * np.arctan((height / width) * np.tan(np.pi / 4.0))))
    needs_sdr = output_suffix in {".png", ".jpg", ".jpeg"}
    with CudaOutputJob(
        cuda_session,
        output_width=width,
        output_height=height,
        output_sample_bytes=1 if needs_sdr else np.dtype(np.float32).itemsize,
        needs_sdr_conversion=needs_sdr,
        rectilinear_output=True,
        output_vertical_fov=vertical_fov,
        plan=cuda_session._plan,
    ) as job:
        job.build_local_exposure(
            latitude_span=180.0,
            horizontal_fov=session.horizontal_fov_deg,
            vertical_fov=session.vertical_fov_deg,
            log_gains=cuda_session.log_gains,
        )
        output_dtype = np.dtype(np.uint8 if needs_sdr else np.float32)
        host_output = cuda_session.pinned_array((height, width, 3), output_dtype)
        auto_contrast_active = auto_contrast and needs_sdr
        starts = range(0, height, job.band_rows)
        if auto_contrast_active:
            job.reset_auto_contrast_histogram()
            for row_start in starts:
                rows = min(job.band_rows, height - row_start)
                job.compose_band(
                    row_start=row_start,
                    rows=rows,
                    latitude_span=180.0,
                    horizontal_fov=session.horizontal_fov_deg,
                    vertical_fov=session.vertical_fov_deg,
                    transfer_function=source.encoding.transfer_function,
                    hard_blend=blend == "hard",
                    incomplete_magenta=allow_incomplete,
                )
                if not allow_incomplete and job.uncovered_count(rows):
                    raise ValueError("capture does not cover every thumbnail pixel")
                job.build_auto_contrast_histogram(
                    rows=rows,
                    transfer_function=source.encoding.transfer_function,
                    reference_white_nits=source.encoding.reference_white_nits,
                )
            job.select_auto_contrast_levels()
        for row_start in starts:
            rows = min(job.band_rows, height - row_start)
            if not auto_contrast_active or job.is_banded:
                job.compose_band(
                    row_start=row_start,
                    rows=rows,
                    latitude_span=180.0,
                    horizontal_fov=session.horizontal_fov_deg,
                    vertical_fov=session.vertical_fov_deg,
                    transfer_function=source.encoding.transfer_function,
                    hard_blend=blend == "hard",
                    incomplete_magenta=allow_incomplete,
                )
                if not allow_incomplete and job.uncovered_count(rows):
                    raise ValueError("capture does not cover every thumbnail pixel")
            if needs_sdr:
                job.convert_band(
                    rows=rows,
                    transfer_function=source.encoding.transfer_function,
                    reference_white_nits=source.encoding.reference_white_nits,
                    apply_auto_contrast=auto_contrast_active,
                )
            job.download_band(host_output[row_start : row_start + rows], rows, converted=needs_sdr)
    temporary_path = _temporary_output_path(output_path)
    try:
        if needs_sdr:
            _write_cuda_sdr_output(temporary_path, host_output, output_suffix, jpeg_quality)
        else:
            _write_exr(temporary_path, host_output, source.encoding)
        os.replace(temporary_path, output_path)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise


def prepare_cuda_session(
    session: SessionMetadata,
    image_root: Path,
    source: SourceInfo,
    gpu_plan: CudaMemoryPlan,
    cancel_event: Event | None = None,
) -> PreparedCudaSession:
    """Upload one session and solve its exposure once for reusable CUDA output jobs."""

    latitude_span = (
        180.0 if session.capture_mode.value == "full_sphere" else session.vertical_fov_deg
    )
    rotations = np.stack([_frame_rotation(frame) for frame in session.frames], dtype=np.float32)
    cuda_session = CudaSession(
        frame_count=len(session.frames),
        source_width=source.width,
        source_height=source.height,
        sample_type=source.encoding.sample_type,
        rotations=rotations,
        plan=gpu_plan,
    )
    try:
        upload_started = time.perf_counter()
        for frame_position, frame in enumerate(session.frames):
            if cancel_event is not None and cancel_event.is_set():
                raise RenderCancelledError("render cancelled")
            slot_index = frame_position % 2
            slot = cuda_session.pinned_slot(slot_index).reshape((source.height, source.width, 3))
            decoded = _read_native_source(_image_path(image_root, frame.filename), source.encoding)
            try:
                np.copyto(slot, decoded)
            finally:
                del decoded
            cuda_session.upload_source(frame_position, slot, slot_index)
        cuda_session.finish_uploads()
        upload_seconds = time.perf_counter() - upload_started
        exposure_started = time.perf_counter()
        exposure = cuda_session.solve_exposure_gains(
            latitude_span=latitude_span,
            horizontal_fov=session.horizontal_fov_deg,
            vertical_fov=session.vertical_fov_deg,
            transfer_function=source.encoding.transfer_function,
        )
        anchor, edge_count, gains = cuda_session.download_exposure_report(exposure)
        exposure_seconds = time.perf_counter() - exposure_started
        return PreparedCudaSession(
            cuda_session,
            ExposureReport(anchor, edge_count, tuple(float(np.exp(gain)) for gain in gains)),
            upload_seconds,
            exposure_seconds,
        )
    except Exception:
        cuda_session.close()
        raise


def _cached_or_prepared_cuda_session(
    *,
    cache: CudaSessionCache | None,
    session_path: Path | None,
    session: SessionMetadata,
    image_root: Path,
    source: SourceInfo,
    gpu_plan: CudaMemoryPlan,
    gpu_memory_budget_bytes: int | None,
    cancel_event: Event | None,
) -> tuple[PreparedCudaSession, CudaSessionCache | None]:
    """Return a matching cached session or prepare one for this render."""

    if cache is None or session_path is None:
        return prepare_cuda_session(session, image_root, source, gpu_plan, cancel_event), None
    device = cuda_device_info()
    key = cuda_session_cache_key(
        device_name=device.name,
        compute_capability=device.compute_capability,
        session_path=session_path,
        session=session,
        image_root=image_root,
        gpu_memory_budget_bytes=gpu_memory_budget_bytes,
    )
    prepared = cache.get(key)
    if prepared is not None:
        return prepared, cache
    try:
        prepared = prepare_cuda_session(session, image_root, source, gpu_plan, cancel_event)
    except Exception:
        cache.invalidate("preparation failed")
        raise
    cache.store(key, prepared)
    return prepared, cache


def _run_adaptive_cuda_bands(
    cuda_session: CudaSession,
    output_job: CudaOutputJob,
    output_height: int,
    render_band: Callable[[int, int], None],
) -> tuple[tuple[tuple[int, int], ...], float]:
    """Run watchdog-safe CUDA bands, adapting only at completed GPU checkpoints."""

    scheduler = CudaBandScheduler(output_job.band_rows)
    completed_rows = 0
    elapsed_seconds = 0.0
    bands: list[tuple[int, int]] = []
    while completed_rows < output_height:
        rows = scheduler.next_rows(output_height - completed_rows)
        started = cuda_session.begin_compute_timing()
        render_band(completed_rows, rows)
        elapsed = cuda_session.end_compute_timing(started)
        scheduler.record_elapsed(elapsed)
        bands.append((completed_rows, rows))
        completed_rows += rows
        elapsed_seconds += elapsed
    return tuple(bands), elapsed_seconds


def _render_prepared_cuda(
    session: SessionMetadata,
    image_root: Path,
    output_path: Path,
    source: SourceInfo,
    output_width: int,
    output_height: int,
    blend: str,
    allow_incomplete: bool,
    output_suffix: str,
    jpeg_quality: int,
    auto_contrast: bool,
    debug_coverage_path: Path | None,
    cancel_event: Event | None,
    progress_callback: Callable[[int, int, str], None] | None,
    gpu_plan: CudaMemoryPlan,
    prepared: PreparedCudaSession,
    session_thumbnail: bool = False,
    return_preview: bool = False,
    diagnostics_callback: Callable[[CudaRenderDiagnostics], None] | None = None,
) -> ExposureReport | PreviewResult:
    """Render a session through the CUDA-only numerical pipeline."""

    latitude_span = (
        180.0 if session.capture_mode.value == "full_sphere" else session.vertical_fov_deg
    )
    needs_sdr_conversion = output_suffix in {".png", ".jpg", ".jpeg"}
    output_sample_bytes = 1 if needs_sdr_conversion else np.dtype(np.float32).itemsize
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if debug_coverage_path is not None:
        debug_coverage_path.parent.mkdir(parents=True, exist_ok=True)
    phase_count = 5 if auto_contrast and needs_sdr_conversion else 4
    phases = {
        "upload": f"[1/{phase_count}] CUDA upload",
        "exposure": f"[2/{phase_count}] CUDA exposure",
        "compositing": f"[3/{phase_count}] CUDA compositing",
        "conversion": f"[4/{phase_count}] CUDA conversion",
        "writing": f"[{phase_count}/{phase_count}] writing",
    }
    if auto_contrast and needs_sdr_conversion:
        phases["conversion"] = f"[4/{phase_count}] CUDA auto contrast"
        phases["writing"] = f"[5/{phase_count}] writing"

    phase_seconds: dict[str, float] = {
        "upload": prepared.upload_seconds,
        "exposure": prepared.exposure_seconds,
    }
    with nullcontext(prepared.cuda_session) as cuda_session:
        report = prepared.exposure_report
        if progress_callback is not None:
            progress_callback(len(session.frames), len(session.frames), phases["upload"])
            progress_callback(1, 1, phases["exposure"])

        with CudaOutputJob(
            cuda_session,
            output_width=output_width,
            output_height=output_height,
            output_sample_bytes=output_sample_bytes,
            needs_sdr_conversion=needs_sdr_conversion,
            plan=gpu_plan,
        ) as output_job:
            phase_started = time.perf_counter()
            output_job.build_local_exposure(
                latitude_span=latitude_span,
                horizontal_fov=session.horizontal_fov_deg,
                vertical_fov=session.vertical_fov_deg,
                log_gains=cuda_session.log_gains,
            )
            phase_seconds["local_exposure"] = time.perf_counter() - phase_started
            output_dtype = np.dtype(np.uint8 if needs_sdr_conversion else np.float32)
            host_output = cuda_session.pinned_array((output_height, output_width, 3), output_dtype)
            host_coverage = (
                cuda_session.pinned_array((output_height, output_width), np.dtype(np.uint8))
                if debug_coverage_path is not None
                else None
            )
            auto_contrast_active = auto_contrast and needs_sdr_conversion
            if auto_contrast_active:
                output_job.reset_auto_contrast_histogram()

                def build_histogram_band(row_start: int, rows: int) -> None:
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    output_job.compose_band(
                        row_start=row_start,
                        rows=rows,
                        latitude_span=latitude_span,
                        horizontal_fov=session.horizontal_fov_deg,
                        vertical_fov=session.vertical_fov_deg,
                        transfer_function=source.encoding.transfer_function,
                        hard_blend=blend == "hard",
                        incomplete_magenta=allow_incomplete,
                    )
                    if not allow_incomplete and output_job.uncovered_count(rows):
                        raise ValueError("capture does not cover every output pixel")
                    output_job.build_auto_contrast_histogram(
                        rows=rows,
                        transfer_function=source.encoding.transfer_function,
                        reference_white_nits=source.encoding.reference_white_nits,
                    )
                    if progress_callback is not None:
                        progress_callback(row_start + rows, output_height, phases["compositing"])

                _histogram_bands, compositing_seconds = _run_adaptive_cuda_bands(
                    cuda_session, output_job, output_height, build_histogram_band
                )
                output_job.select_auto_contrast_levels()
                phase_seconds["compositing"] = compositing_seconds
            else:
                phase_seconds["compositing"] = 0.0

            phase_started = time.perf_counter()

            def convert_output_band(row_start: int, rows: int) -> None:
                if cancel_event is not None and cancel_event.is_set():
                    raise RenderCancelledError("render cancelled")
                output_job.compose_band(
                    row_start=row_start,
                    rows=rows,
                    latitude_span=latitude_span,
                    horizontal_fov=session.horizontal_fov_deg,
                    vertical_fov=session.vertical_fov_deg,
                    transfer_function=source.encoding.transfer_function,
                    hard_blend=blend == "hard",
                    incomplete_magenta=allow_incomplete,
                )
                if not allow_incomplete and output_job.uncovered_count(rows):
                    raise ValueError("capture does not cover every output pixel")
                if needs_sdr_conversion:
                    output_job.convert_band(
                        rows=rows,
                        transfer_function=source.encoding.transfer_function,
                        reference_white_nits=source.encoding.reference_white_nits,
                        apply_auto_contrast=auto_contrast_active,
                    )
                output_job.download_band(
                    host_output[row_start : row_start + rows], rows, converted=needs_sdr_conversion
                )
                if host_coverage is not None:
                    output_job.download_coverage(host_coverage[row_start : row_start + rows], rows)
                if progress_callback is not None:
                    progress_callback(row_start + rows, output_height, phases["conversion"])

            _output_bands, output_seconds = _run_adaptive_cuda_bands(
                cuda_session, output_job, output_height, convert_output_band
            )
            if not auto_contrast_active:
                phase_seconds["compositing"] = output_seconds
            phase_seconds["conversion_download"] = time.perf_counter() - phase_started

            if return_preview:
                if not needs_sdr_conversion:
                    raise RuntimeError("CUDA previews require an SDR conversion buffer")
                if diagnostics_callback is not None:
                    diagnostics_callback(
                        CudaRenderDiagnostics(
                            cuda_session.transfer_stats, tuple(phase_seconds.items())
                        )
                    )
                return PreviewResult(host_output, report)
            temporary_path = _temporary_output_path(output_path)
            temporary_coverage_path = (
                _temporary_output_path(debug_coverage_path)
                if debug_coverage_path is not None
                else None
            )
            phase_started = time.perf_counter()
            try:
                if needs_sdr_conversion:
                    _write_cuda_sdr_output(temporary_path, host_output, output_suffix, jpeg_quality)
                else:
                    _write_exr(temporary_path, host_output, source.encoding)
                if temporary_coverage_path is not None and host_coverage is not None:
                    Image.fromarray(host_coverage, mode="L").save(
                        temporary_coverage_path, format="PNG"
                    )
                os.replace(temporary_path, output_path)
                if temporary_coverage_path is not None and debug_coverage_path is not None:
                    os.replace(temporary_coverage_path, debug_coverage_path)
                if progress_callback is not None:
                    progress_callback(1, 1, phases["writing"])
            except Exception:
                temporary_path.unlink(missing_ok=True)
                if temporary_coverage_path is not None:
                    temporary_coverage_path.unlink(missing_ok=True)
                raise
            phase_seconds["encode"] = time.perf_counter() - phase_started
            if session_thumbnail:
                _render_cuda_thumbnail(
                    cuda_session,
                    session,
                    source,
                    thumbnail_output_path(output_path),
                    blend,
                    allow_incomplete,
                    output_suffix,
                    jpeg_quality,
                    auto_contrast,
                )
            if diagnostics_callback is not None:
                diagnostics_callback(
                    CudaRenderDiagnostics(cuda_session.transfer_stats, tuple(phase_seconds.items()))
                )
        return report


def _render_cuda(
    session: SessionMetadata,
    image_root: Path,
    output_path: Path,
    source: SourceInfo,
    output_width: int,
    output_height: int,
    blend: str,
    allow_incomplete: bool,
    output_suffix: str,
    jpeg_quality: int,
    auto_contrast: bool,
    debug_coverage_path: Path | None,
    cancel_event: Event | None,
    progress_callback: Callable[[int, int, str], None] | None,
    gpu_plan: CudaMemoryPlan,
    session_thumbnail: bool = False,
    return_preview: bool = False,
    diagnostics_callback: Callable[[CudaRenderDiagnostics], None] | None = None,
) -> ExposureReport | PreviewResult:
    """Prepare one CUDA session, render it once, and release its device ownership."""

    prepared = prepare_cuda_session(session, image_root, source, gpu_plan, cancel_event)
    try:
        return _render_prepared_cuda(
            session,
            image_root,
            output_path,
            source,
            output_width,
            output_height,
            blend,
            allow_incomplete,
            output_suffix,
            jpeg_quality,
            auto_contrast,
            debug_coverage_path,
            cancel_event,
            progress_callback,
            gpu_plan,
            prepared,
            session_thumbnail,
            return_preview,
            diagnostics_callback,
        )
    finally:
        prepared.close()


def render_session(
    session: SessionMetadata,
    image_root: Path,
    output_path: Path,
    width: int | None = None,
    blend: str = "hard",
    allow_incomplete: bool = False,
    memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    progress_callback: Callable[[int, int, str], None] | None = None,
    debug_coverage_path: Path | None = None,
    cancel_event: Event | None = None,
    jpeg_quality: int = 95,
    workers: int | None = None,
    auto_contrast: bool = True,
    session_thumbnail: bool = False,
    exposure_report: ExposureReport | None = None,
    use_gpu: bool | None = True,
    gpu_memory_budget_bytes: int | None = None,
    backend_callback: Callable[[str, str], None] | None = None,
    strict_gpu: bool = False,
    gpu_diagnostics_callback: Callable[[CudaRenderDiagnostics], None] | None = None,
    cuda_session_cache: CudaSessionCache | None = None,
    cuda_session_path: Path | None = None,
) -> ExposureReport:
    """Dispatch one render to the CUDA-only or bounded CPU pipeline."""

    cpu_arguments = (
        session,
        image_root,
        output_path,
        width,
        blend,
        allow_incomplete,
        memory_budget_bytes,
        progress_callback,
        debug_coverage_path,
        cancel_event,
        jpeg_quality,
        workers,
        auto_contrast,
        session_thumbnail,
        exposure_report,
        False,
        gpu_memory_budget_bytes,
    )
    if use_gpu is False:
        return _render_cpu(*cpu_arguments, backend_callback)
    if not session.frames:
        return _render_cpu(*cpu_arguments, backend_callback)
    if not allow_incomplete and (
        not session.completed or any(frame.status != "captured" for frame in session.frames)
    ):
        return _render_cpu(*cpu_arguments, backend_callback)
    renderable = renderable_session(session, image_root, allow_incomplete)
    try:
        source = _source_info_for_session(renderable, image_root)
        output_suffix = output_path.suffix.lower()
        if output_suffix not in {".exr", ".jpg", ".jpeg", ".png"}:
            raise ValueError("output extension must be .exr, .jpg, .jpeg, or .png")
        output_width, output_height = _output_dimensions(renderable, source.width, width)
        plan_width = max(output_width, source.width) if session_thumbnail else output_width
        plan_height = max(output_height, source.height) if session_thumbnail else output_height
        selection, gpu_plan = select_cuda_backend(
            frame_count=len(renderable.frames),
            source_width=source.width,
            source_height=source.height,
            output_width=plan_width,
            output_height=plan_height,
            sample_type=source.encoding.sample_type,
            output_sample_bytes=1 if output_suffix != ".exr" else np.dtype(np.float32).itemsize,
            needs_sdr_conversion=output_suffix != ".exr",
            gpu_budget_bytes=gpu_memory_budget_bytes,
            strict=strict_gpu,
        )
        if selection.backend == "cuda":
            compile_cuda_module()
    except GpuUnavailableError as error:
        if strict_gpu:
            raise
        selection = None
        gpu_plan = None
        detail = str(error)
    else:
        assert selection is not None
        detail = selection.reason
    if selection is None or selection.backend == "cpu" or gpu_plan is None:
        if selection is not None:
            detail = selection.reason
        LOGGER.info("render backend selected: CPU (%s)", detail)
        if backend_callback is not None:
            backend_callback("cpu", detail)
        return _render_cpu(*cpu_arguments, None)
    LOGGER.info("render backend selected: CUDA %s (%s)", selection.memory_mode, detail)
    if backend_callback is not None:
        backend_callback(f"cuda {selection.memory_mode}", detail)
    prepared: PreparedCudaSession | None = None
    cache_owner: CudaSessionCache | None = None
    try:
        if cuda_session_cache is None or cuda_session_path is None:
            cuda_result = _render_cuda(
                renderable,
                image_root,
                output_path,
                source,
                output_width,
                output_height,
                blend,
                allow_incomplete,
                output_suffix,
                jpeg_quality,
                auto_contrast,
                debug_coverage_path,
                cancel_event,
                progress_callback,
                gpu_plan,
                session_thumbnail=session_thumbnail,
                diagnostics_callback=gpu_diagnostics_callback,
            )
        else:
            prepared, cache_owner = _cached_or_prepared_cuda_session(
                cache=cuda_session_cache,
                session_path=cuda_session_path,
                session=renderable,
                image_root=image_root,
                source=source,
                gpu_plan=gpu_plan,
                gpu_memory_budget_bytes=gpu_memory_budget_bytes,
                cancel_event=cancel_event,
            )
            cuda_result = _render_prepared_cuda(
                renderable,
                image_root,
                output_path,
                source,
                output_width,
                output_height,
                blend,
                allow_incomplete,
                output_suffix,
                jpeg_quality,
                auto_contrast,
                debug_coverage_path,
                cancel_event,
                progress_callback,
                gpu_plan,
                prepared,
                session_thumbnail=session_thumbnail,
                diagnostics_callback=gpu_diagnostics_callback,
            )
    except CudaPreflightError as error:
        if strict_gpu:
            raise
        detail = str(error)
        LOGGER.info("render backend selected: CPU (%s)", detail)
        if backend_callback is not None:
            backend_callback("cpu", detail)
        return _render_cpu(*cpu_arguments, None)
    except Exception:
        if cache_owner is not None:
            cache_owner.invalidate("render failed")
        raise
    finally:
        if prepared is not None and cache_owner is None:
            prepared.close()
    assert isinstance(cuda_result, ExposureReport)
    return cuda_result


def validate_images(
    session: SessionMetadata,
    image_root: Path,
    allow_incomplete: bool = False,
) -> None:
    """Validate image references without retaining decoded sources."""

    if not session.frames:
        return
    if not allow_incomplete and (
        not session.completed or any(frame.status != "captured" for frame in session.frames)
    ):
        raise ValueError("session contains frames that are not captured")
    filenames = [Path(frame.filename) for frame in session.frames]
    if any(not path.is_absolute() and ".." in path.parts for path in filenames):
        raise ValueError("frame filenames must be relative and stay inside the session directory")
    if len(set(filenames)) != len(filenames):
        raise ValueError("frame filenames must be unique")
    for filename in filenames:
        path = filename if filename.is_absolute() else image_root / filename
        if not path.is_file():
            if allow_incomplete:
                continue
            raise ValueError(f"missing source image: {filename}")
        try:
            _probe_source(path)
        except Exception as error:
            raise ValueError(f"cannot decode source image {filename}: {error}") from error


def render_preview(
    session: SessionMetadata,
    image_root: Path,
    width: int,
    output_suffix: str,
    blend: str = "hard",
    allow_incomplete: bool = False,
    memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    progress_callback: Callable[[int, int, str], None] | None = None,
    cancel_event: Event | None = None,
    workers: int | None = None,
    auto_contrast: bool = True,
    use_gpu: bool | None = True,
    backend_callback: Callable[[str, str], None] | None = None,
    strict_gpu: bool = False,
    gpu_diagnostics_callback: Callable[[CudaRenderDiagnostics], None] | None = None,
    cuda_session_cache: CudaSessionCache | None = None,
    cuda_session_path: Path | None = None,
    gpu_memory_budget_bytes: int | None = None,
) -> PreviewResult:
    """Render an ephemeral, displayable SDR preview and return its exposure solve."""

    if width < 1:
        raise ValueError("preview width must be positive")
    if output_suffix.lower() not in {".exr", ".jpg", ".jpeg", ".png"}:
        raise ValueError("output extension must be .exr, .jpg, .jpeg, or .png")
    image_root = image_root.resolve()
    if (
        use_gpu is not False
        and session.frames
        and (
            allow_incomplete
            or (session.completed and all(frame.status == "captured" for frame in session.frames))
        )
    ):
        renderable = renderable_session(session, image_root, allow_incomplete)
        try:
            source = _source_info_for_session(renderable, image_root)
            output_width, output_height = _output_dimensions(renderable, source.width, width)
            selection, gpu_plan = select_cuda_backend(
                frame_count=len(renderable.frames),
                source_width=source.width,
                source_height=source.height,
                output_width=output_width,
                output_height=output_height,
                sample_type=source.encoding.sample_type,
                output_sample_bytes=1,
                needs_sdr_conversion=True,
                gpu_budget_bytes=gpu_memory_budget_bytes,
                strict=strict_gpu,
            )
            if selection.backend == "cuda":
                compile_cuda_module()
        except GpuUnavailableError as error:
            if strict_gpu:
                raise
            selection = None
            gpu_plan = None
            detail = str(error)
        else:
            assert selection is not None
            detail = selection.reason
        if selection is not None and selection.backend == "cuda" and gpu_plan is not None:
            LOGGER.info("preview backend selected: CUDA %s (%s)", selection.memory_mode, detail)
            if backend_callback is not None:
                backend_callback(f"cuda {selection.memory_mode}", detail)
            try:
                if cuda_session_cache is None or cuda_session_path is None:
                    preview = _render_cuda(
                        renderable,
                        image_root,
                        image_root / "preview.png",
                        source,
                        output_width,
                        output_height,
                        blend,
                        allow_incomplete,
                        ".png",
                        95,
                        auto_contrast and output_suffix.lower() != ".exr",
                        None,
                        cancel_event,
                        progress_callback,
                        gpu_plan,
                        return_preview=True,
                        diagnostics_callback=gpu_diagnostics_callback,
                    )
                else:
                    prepared, cache_owner = _cached_or_prepared_cuda_session(
                        cache=cuda_session_cache,
                        session_path=cuda_session_path,
                        session=renderable,
                        image_root=image_root,
                        source=source,
                        gpu_plan=gpu_plan,
                        gpu_memory_budget_bytes=gpu_memory_budget_bytes,
                        cancel_event=cancel_event,
                    )
                    assert cache_owner is not None
                    try:
                        preview = _render_prepared_cuda(
                            renderable,
                            image_root,
                            image_root / "preview.png",
                            source,
                            output_width,
                            output_height,
                            blend,
                            allow_incomplete,
                            ".png",
                            95,
                            auto_contrast and output_suffix.lower() != ".exr",
                            None,
                            cancel_event,
                            progress_callback,
                            gpu_plan,
                            prepared,
                            return_preview=True,
                            diagnostics_callback=gpu_diagnostics_callback,
                        )
                    except Exception:
                        cache_owner.invalidate("preview failed")
                        raise
            except CudaPreflightError:
                if strict_gpu:
                    raise
                use_gpu = False
            else:
                assert isinstance(preview, PreviewResult)
                return preview
        if backend_callback is not None:
            backend_callback("cpu", detail if selection is None else selection.reason)
    with tempfile.TemporaryDirectory(prefix="pano-preview-") as directory:
        preview_path = Path(directory) / "preview.png"
        report = render_session(
            session,
            image_root,
            preview_path,
            width=width,
            blend=blend,
            allow_incomplete=allow_incomplete,
            memory_budget_bytes=memory_budget_bytes,
            progress_callback=progress_callback,
            cancel_event=cancel_event,
            workers=workers,
            auto_contrast=auto_contrast and output_suffix.lower() != ".exr",
            use_gpu=use_gpu,
            strict_gpu=strict_gpu,
            backend_callback=backend_callback,
        )
        with Image.open(preview_path) as image:
            pixels = np.array(image.convert("RGB"), dtype=np.uint8, copy=True)
        return PreviewResult(pixels, report)
