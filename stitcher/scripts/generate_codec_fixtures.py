"""Generate the tiny, synthetic native-codec contract corpus."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import struct
import zlib
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

from pano_stitch.compositor import _ExrWriter


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF)
    )


def _write_png(path: Path, pixels: np.ndarray[Any, Any], cicp: bytes | None = None) -> None:
    height, width, channels = pixels.shape
    if channels != 3 or pixels.dtype not in {np.dtype(np.uint8), np.dtype(np.uint16)}:
        raise ValueError("fixture PNG must be uint8 or uint16 RGB")
    bit_depth = pixels.dtype.itemsize * 8
    rows = []
    for row in pixels:
        encoded = row.tobytes() if bit_depth == 8 else row.astype(">u2").tobytes()
        rows.append(b"\0" + encoded)
    data = b"\x89PNG\r\n\x1a\n"
    data += _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, 2, 0, 0, 0))
    if cicp is not None:
        data += _chunk(b"cICP", cicp)
    data += _chunk(b"IDAT", zlib.compress(b"".join(rows), level=9))
    data += _chunk(b"IEND", b"")
    path.write_bytes(data)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    png8 = np.array(
        [
            [[0, 64, 255], [10, 20, 30], [255, 128, 1]],
            [[3, 2, 1], [17, 34, 51], [250, 251, 252]],
        ],
        dtype=np.uint8,
    )
    png16 = np.array(
        [
            [[0, 32768, 65535], [1000, 2000, 3000], [65535, 50000, 40000]],
            [[1, 2, 3], [16384, 24576, 32768], [60000, 45000, 12345]],
        ],
        dtype=np.uint16,
    )
    jpeg = np.array(
        [[[x * 31, y * 67, (x * 19 + y * 43) % 256] for x in range(8)] for y in range(4)],
        dtype=np.uint8,
    )
    exr = np.array(
        [
            [[0.0, 0.25, 1.0], [2.0, 4.0, 8.0], [-0.5, 0.5, 1.5]],
            [[16.0, 0.125, 3.0], [0.75, 1.25, 2.5], [6.0, 7.0, 9.0]],
        ],
        dtype=np.float32,
    )

    png8_path = output / "rgb8-srgb.png"
    png16_path = output / "rgb16-rec2020-pq.png"
    jpeg_path = output / "rgb8-srgb.jpg"
    exr_path = output / "rgb32-rec2020-linear.exr"
    _write_png(png8_path, png8)
    _write_png(png16_path, png16, bytes((9, 16, 0, 1)))
    Image.fromarray(jpeg, "RGB").save(
        jpeg_path,
        format="JPEG",
        quality=95,
        subsampling=0,
        optimize=False,
        progressive=False,
    )
    with _ExrWriter(exr_path, exr.shape[1], exr.shape[0]) as writer:
        writer.write(exr)
    with Image.open(jpeg_path) as decoded_jpeg:
        jpeg_samples = np.asarray(decoded_jpeg.convert("RGB"), dtype=np.uint8).tolist()

    fixtures = {
        "rgb8-srgb.jpg": {
            "width": 8,
            "height": 4,
            "channels": 3,
            "sample_type": "uint8",
            "color_primaries": "srgb",
            "transfer_function": "srgb",
            "reference_white_nits": 100.0,
            "jpeg_subsampling": "4:4:4",
            "decoded_samples": jpeg_samples,
        },
        "rgb8-srgb.png": {
            "width": 3,
            "height": 2,
            "channels": 3,
            "sample_type": "uint8",
            "color_primaries": "srgb",
            "transfer_function": "srgb",
            "reference_white_nits": 100.0,
            "decoded_samples": png8.tolist(),
        },
        "rgb16-rec2020-pq.png": {
            "width": 3,
            "height": 2,
            "channels": 3,
            "sample_type": "uint16",
            "color_primaries": "rec2020",
            "transfer_function": "pq",
            "reference_white_nits": 203.0,
            "png_cicp": [9, 16, 0, 1],
            "decoded_samples": png16.tolist(),
        },
        "rgb32-rec2020-linear.exr": {
            "width": 3,
            "height": 2,
            "channels": 3,
            "sample_type": "float32",
            "color_primaries": "rec2020",
            "transfer_function": "linear",
            "reference_white_nits": 203.0,
            "compression": "PIZ",
            "decoded_samples": exr.tolist(),
        },
    }
    for name, expectation in fixtures.items():
        expectation["sha256"] = _sha256(output / name)
    (output / "expected.json").write_text(
        json.dumps({"schema_version": 1, "fixtures": fixtures}, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    generate(arguments.output)


if __name__ == "__main__":
    main()
