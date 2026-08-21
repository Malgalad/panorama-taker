"""Memory-bounded rendering of validated capture sessions."""

from __future__ import annotations

import binascii
import mmap
import os
import struct
import tempfile
import zlib
from collections.abc import Callable
from contextlib import nullcontext
from dataclasses import dataclass
from importlib import import_module
from pathlib import Path
from threading import Event
from typing import Any, BinaryIO

import cv2
import numpy as np
from numpy.typing import NDArray
from PIL import Image

from pano_stitch.metadata import ImageEncoding, SessionMetadata
from pano_stitch.projection import camera_maps, equirectangular_directions, remap_source

FloatImage = NDArray[np.float32]
DEFAULT_MEMORY_BUDGET_BYTES = 768 * 1024 * 1024
MAX_MEMORY_BUDGET_BYTES = 8192 * 1024 * 1024
_RESERVED_RUNTIME_BYTES = 192 * 1024 * 1024
_TILE_BYTES_PER_PIXEL = 160


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
    scratch_bytes: int


class RenderCancelledError(RuntimeError):
    """Raised when a cooperative render cancellation is requested."""


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


def _read_source(path: Path, encoding: ImageEncoding) -> FloatImage:
    if path.suffix.lower() == ".png":
        return _read_png(path, encoding)
    if path.suffix.lower() == ".exr":
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


def _choose_strip_height(source: SourceInfo, output_width: int, memory_budget_bytes: int) -> int:
    if memory_budget_bytes <= 0 or memory_budget_bytes > MAX_MEMORY_BUDGET_BYTES:
        maximum_mib = MAX_MEMORY_BUDGET_BYTES // (1024 * 1024)
        raise ValueError(f"memory budget must be between 1 and {maximum_mib} MiB")
    source_working_set = source.width * source.height * 3 * np.dtype(np.float32).itemsize * 3
    available = memory_budget_bytes - source_working_set - _RESERVED_RUNTIME_BYTES
    if available < output_width * _TILE_BYTES_PER_PIXEL:
        raise ValueError("memory budget is too small for one output row and one decoded source")
    return max(1, available // (_TILE_BYTES_PER_PIXEL * output_width))


def _read_tile(stream: BinaryIO, offset: int, shape: tuple[int, ...]) -> FloatImage:
    count = int(np.prod(shape))
    stream.seek(offset)
    data = stream.read(count * np.dtype(np.float32).itemsize)
    if len(data) != count * np.dtype(np.float32).itemsize:
        raise OSError("truncated compositor scratch file")
    return np.frombuffer(data, dtype=np.float32).copy().reshape(shape)


def _write_tile(stream: BinaryIO, offset: int, tile: FloatImage) -> None:
    stream.seek(offset)
    stream.write(np.ascontiguousarray(tile, dtype=np.float32).tobytes())


def _scratch_offset(row: int, width: int, channels: int) -> int:
    return row * width * channels * np.dtype(np.float32).itemsize


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


def estimate_render_resources(
    session: SessionMetadata,
    image_root: Path,
    width: int | None = None,
    memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
) -> RenderResources:
    """Return the output and scratch requirements without decoding source pixels."""

    if not session.frames:
        raise ValueError("session contains no frames")
    source = _source_info_for_session(session, image_root)
    output_width, output_height = _output_dimensions(session, source.width, width)
    strip_height = _choose_strip_height(source, output_width, memory_budget_bytes)
    scratch_bytes = output_height * output_width * 4 * np.dtype(np.float32).itemsize
    return RenderResources(output_width, output_height, strip_height, scratch_bytes)


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
) -> None:
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

    first_source = _source_info_for_session(session, image_root)
    output_suffix = output_path.suffix.lower()
    if output_suffix not in {".exr", ".jpg", ".jpeg", ".png"}:
        raise ValueError("output extension must be .exr, .jpg, .jpeg, or .png")
    resources = estimate_render_resources(session, image_root, width, memory_budget_bytes)
    output_width = resources.output_width
    output_height = resources.output_height
    strip_height = resources.strip_height
    composite_strips = (output_height + strip_height - 1) // strip_height
    total_work = len(session.frames) * composite_strips + composite_strips
    completed_work = 0
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
        color_path = scratch_root / "color.f32"
        weight_path = scratch_root / "weight.f32"
        with (
            color_path.open("wb") as color_initializer,
            weight_path.open("wb") as weight_initializer,
        ):
            color_initializer.truncate(
                output_height * output_width * 3 * np.dtype(np.float32).itemsize
            )
            weight_initializer.truncate(
                output_height * output_width * np.dtype(np.float32).itemsize
            )

        with color_path.open("r+b") as color_scratch, weight_path.open("r+b") as weight_scratch:
            for frame in session.frames:
                if cancel_event is not None and cancel_event.is_set():
                    raise RenderCancelledError("render cancelled")
                source = _read_source(
                    _image_path(image_root, frame.filename), first_source.encoding
                )
                try:
                    for row_start in range(0, output_height, strip_height):
                        if cancel_event is not None and cancel_event.is_set():
                            raise RenderCancelledError("render cancelled")
                        rows = min(strip_height, output_height - row_start)
                        directions = equirectangular_directions(
                            output_width, rows, latitude_span, row_start, output_height
                        )
                        map_x, map_y, valid, edge_distance = camera_maps(
                            directions,
                            frame,
                            first_source.width,
                            first_source.height,
                            session.horizontal_fov_deg,
                            session.vertical_fov_deg,
                        )
                        sampled = remap_source(source, map_x, map_y)
                        color_offset = _scratch_offset(row_start, output_width, 3)
                        weight_offset = _scratch_offset(row_start, output_width, 1)
                        color = _read_tile(color_scratch, color_offset, (rows, output_width, 3))
                        weight = _read_tile(weight_scratch, weight_offset, (rows, output_width))
                        if blend == "hard":
                            candidate = np.where(valid, np.maximum(edge_distance, 1e-6), 0.0)
                            selected = candidate > weight
                            color[selected] = sampled[selected]
                            weight[selected] = candidate[selected]
                        else:
                            feather_width = max(
                                1.0, min(first_source.width, first_source.height) * 0.08
                            )
                            candidate = np.where(
                                valid,
                                np.maximum(edge_distance / feather_width, 1e-6),
                                0.0,
                            )
                            color += sampled * candidate[..., np.newaxis]
                            weight += candidate
                        _write_tile(color_scratch, color_offset, color)
                        _write_tile(weight_scratch, weight_offset, weight)
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
                        color = _read_tile(
                            color_scratch,
                            _scratch_offset(row_start, output_width, 3),
                            (rows, output_width, 3),
                        )
                        weight = _read_tile(
                            weight_scratch,
                            _scratch_offset(row_start, output_width, 1),
                            (rows, output_width),
                        )
                        covered = weight > 0.0
                        if coverage_writer is not None:
                            coverage_writer.write(covered)
                        if blend == "feather":
                            color[covered] /= weight[covered, np.newaxis]
                        uncovered = int(np.count_nonzero(~covered))
                        if uncovered and not allow_incomplete:
                            raise ValueError(f"capture does not cover {uncovered} output pixels")
                        if uncovered and allow_incomplete:
                            color[~covered] = np.array((1.0, 0.0, 1.0), dtype=np.float32)
                        writer.write(color)
                        completed_work += 1
                        if progress_callback is not None:
                            progress_callback(completed_work, total_work, "writing")
                os.replace(temporary_path, output_path)
                if temporary_coverage_path is not None and debug_coverage_path is not None:
                    os.replace(temporary_coverage_path, debug_coverage_path)
            except Exception:
                temporary_path.unlink(missing_ok=True)
                if temporary_coverage_path is not None:
                    temporary_coverage_path.unlink(missing_ok=True)
                raise


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
            raise ValueError(f"missing source image: {filename}")
        try:
            _probe_source(path)
        except Exception as error:
            raise ValueError(f"cannot decode source image {filename}: {error}") from error
