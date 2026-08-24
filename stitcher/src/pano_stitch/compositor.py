"""Memory-bounded rendering of validated capture sessions."""

from __future__ import annotations

import binascii
import mmap
import os
import struct
import tempfile
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

from pano_stitch.metadata import FrameMetadata, ImageEncoding, SessionMetadata
from pano_stitch.projection import camera_maps, equirectangular_directions, remap_source

FloatImage = NDArray[np.float32]
DEFAULT_MEMORY_BUDGET_BYTES = 768 * 1024 * 1024
MAX_MEMORY_BUDGET_BYTES = 8192 * 1024 * 1024
_RESERVED_RUNTIME_BYTES = 192 * 1024 * 1024
_TILE_BYTES_PER_PIXEL = 160
_MAX_AUTO_WORKERS = 8


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
        luminances.append(luminance)
        valid_masks.append(valid & finite)
        if progress_callback is not None:
            progress_callback(position + 1)

    equations: list[tuple[int, int, float, float]] = []
    adjacency: list[set[int]] = [set() for _ in session.frames]
    for left in range(frame_count):
        for right in range(left + 1, frame_count):
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
            equations.append((left, right, float(np.median(inliers)), float(np.sqrt(inliers.size))))
            adjacency[left].add(right)
            adjacency[right].add(left)

    if not equations:
        raise ValueError("exposure normalization found no reliable overlapping frame pairs")
    seen = {0}
    pending = [0]
    while pending:
        current = pending.pop()
        for neighbor in adjacency[current]:
            if neighbor not in seen:
                seen.add(neighbor)
                pending.append(neighbor)
    if len(seen) != frame_count:
        missing = ", ".join(str(index + 1) for index in range(frame_count) if index not in seen)
        raise ValueError(
            f"exposure normalization graph is disconnected; frames not linked: {missing}"
        )

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


class _PngWriter:
    """Streaming lossless SDR PNG writer with HDR tone mapping."""

    def __init__(self, path: Path, width: int, height: int, encoding: ImageEncoding) -> None:
        self._encoding = encoding
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
        linear = np.maximum(rows, 0.0)
        if self._encoding.transfer_function == "pq":
            linear = linear * np.float32(10000.0 / self._encoding.reference_white_nits)
            linear = linear / (1.0 + linear)
        mapped = linear
        rgb = np.round(np.clip(_linear_to_srgb(mapped), 0.0, 1.0) * 255.0).astype(np.uint8)
        for row in np.ascontiguousarray(rgb):
            compressed = self._compressor.compress(b"\0" + row.tobytes())
            if compressed:
                self._write_chunk(b"IDAT", compressed)


class _JpegWriter:
    """Disk-spooled SDR JPEG writer that avoids a second full RAM image."""

    def __init__(
        self, path: Path, width: int, height: int, encoding: ImageEncoding, quality: int
    ) -> None:
        self._path = path
        self._width = width
        self._height = height
        self._encoding = encoding
        self._quality = quality
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
        linear = np.maximum(rows, 0.0)
        if self._encoding.transfer_function == "pq":
            linear = linear * np.float32(10000.0 / self._encoding.reference_white_nits)
            linear = linear / (1.0 + linear)
        rgb = np.round(np.clip(_linear_to_srgb(linear), 0.0, 1.0) * 255.0).astype(np.uint8)
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


def _available_tile_bytes(
    source: SourceInfo, output_width: int, memory_budget_bytes: int
) -> int:
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
    sampled = remap_source(source, map_x, map_y)
    color = color_scratch[row_start : row_start + rows]
    weight = weight_scratch[row_start : row_start + rows]
    if blend == "hard":
        candidate = np.where(valid, np.maximum(edge_distance, 1e-6), 0.0)
        selected = candidate > weight
        color[selected] = sampled[selected]
        weight[selected] = candidate[selected]
    else:
        feather_width = max(1.0, min(source_info.width, source_info.height) * 0.08)
        candidate = np.where(
            valid,
            np.maximum(edge_distance / feather_width, 1e-6),
            0.0,
        )
        color += sampled * candidate[..., np.newaxis]
        weight += candidate


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
    worker_count, strip_height = _choose_render_plan(
        source, output_width, output_height, memory_budget_bytes, workers
    )
    scratch_bytes = output_height * output_width * 4 * np.dtype(np.float32).itemsize
    return RenderResources(output_width, output_height, strip_height, worker_count, scratch_bytes)


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
) -> ExposureReport:
    """Render one session with bounded RAM and disk-backed strip accumulators."""

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
    resources = estimate_render_resources(
        session, image_root, width, memory_budget_bytes, workers
    )
    output_width = resources.output_width
    output_height = resources.output_height
    strip_height = resources.strip_height
    composite_strips = (output_height + strip_height - 1) // strip_height
    exposure_work = len(session.frames)
    total_work = exposure_work + len(session.frames) * composite_strips + composite_strips
    completed_work = exposure_work
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
        exposure_report = _estimate_exposure_gains(
            session,
            image_root,
            first_source,
            cancel_event,
            (
                (lambda completed: progress_callback(completed, total_work, "exposure"))
                if progress_callback is not None
                else None
            ),
        )
        scratch_root = Path(scratch_directory)
        color_path = scratch_root / "color.f32"
        weight_path = scratch_root / "weight.f32"
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
        try:
            with _limit_opencv_threads(resources.worker_count):
                for frame_position, frame in enumerate(session.frames):
                    if cancel_event is not None and cancel_event.is_set():
                        raise RenderCancelledError("render cancelled")
                    source = _read_source(
                        _image_path(image_root, frame.filename), first_source.encoding
                    )
                    try:
                        source *= np.float32(exposure_report.gains[frame_position])
                        with ThreadPoolExecutor(max_workers=resources.worker_count) as executor:
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
                                )
                                for row_start in range(0, output_height, strip_height)
                            ]
                            for future in as_completed(futures):
                                future.result()
                                completed_work += 1
                                if progress_callback is not None:
                                    progress_callback(completed_work, total_work, "compositing")
                    finally:
                        del source

            temporary_path = _temporary_output_path(output_path)
            temporary_coverage_path = (
                _temporary_output_path(debug_coverage_path)
                if debug_coverage_path is not None
                else None
            )
            try:
                if output_suffix == ".exr":
                    writer: _ExrWriter | _JpegWriter | _PngWriter = _ExrWriter(
                        temporary_path, output_width, output_height
                    )
                elif output_suffix == ".png":
                    writer = _PngWriter(
                        temporary_path, output_width, output_height, first_source.encoding
                    )
                else:
                    writer = _JpegWriter(
                        temporary_path,
                        output_width,
                        output_height,
                        first_source.encoding,
                        jpeg_quality,
                    )
                coverage_writer = (
                    _CoverageWriter(temporary_coverage_path, output_width, output_height)
                    if temporary_coverage_path is not None
                    else None
                )
                coverage_context = coverage_writer if coverage_writer is not None else nullcontext()
                with writer, coverage_context:
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
                            if blend == "feather":
                                color_rows[covered] /= weight_rows[covered, np.newaxis]
                            uncovered = int(np.count_nonzero(~covered))
                            if uncovered and not allow_incomplete:
                                raise ValueError(
                                    f"capture does not cover {uncovered} output pixels"
                                )
                            if uncovered and allow_incomplete:
                                color_rows[~covered] = np.array((1.0, 0.0, 1.0), dtype=np.float32)
                            writer.write(color_rows)
                            completed_work += 1
                            if progress_callback is not None:
                                progress_callback(completed_work, total_work, "writing")
                        finally:
                            del color_rows
                            del weight_rows
                os.replace(temporary_path, output_path)
                if temporary_coverage_path is not None and debug_coverage_path is not None:
                    os.replace(temporary_coverage_path, debug_coverage_path)
            except Exception:
                temporary_path.unlink(missing_ok=True)
                if temporary_coverage_path is not None:
                    temporary_coverage_path.unlink(missing_ok=True)
                raise
            return exposure_report
        finally:
            _close_scratch_memmap(color_scratch)
            _close_scratch_memmap(weight_scratch)
            del color_scratch
            del weight_scratch


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
