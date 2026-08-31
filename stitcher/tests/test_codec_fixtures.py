from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import numpy as np
import OpenEXR
from PIL import Image, JpegImagePlugin

from pano_stitch.compositor import _png_chunks, _probe_source, _read_native_source
from pano_stitch.metadata import ImageEncoding

FIXTURES = Path(__file__).parent / "fixtures" / "codecs"


def _expectations() -> dict[str, dict[str, Any]]:
    document = json.loads((FIXTURES / "expected.json").read_text(encoding="utf-8"))
    assert document["schema_version"] == 1
    return document["fixtures"]


def test_codec_fixture_hashes_and_metadata_are_frozen() -> None:
    for name, expected in _expectations().items():
        path = FIXTURES / name
        assert hashlib.sha256(path.read_bytes()).hexdigest() == expected["sha256"]
        source = _probe_source(path)
        assert (source.width, source.height) == (expected["width"], expected["height"])
        assert source.encoding == ImageEncoding(
            expected["sample_type"],
            expected["color_primaries"],
            expected["transfer_function"],
            expected["reference_white_nits"],
        )


def test_lossless_png_samples_and_pq_signaling_are_frozen() -> None:
    expected = _expectations()
    srgb = _read_native_source(FIXTURES / "rgb8-srgb.png", ImageEncoding())
    pq_encoding = ImageEncoding("uint16", "rec2020", "pq", 203.0)
    pq = _read_native_source(FIXTURES / "rgb16-rec2020-pq.png", pq_encoding)

    np.testing.assert_array_equal(srgb, expected["rgb8-srgb.png"]["decoded_samples"])
    np.testing.assert_array_equal(pq, expected["rgb16-rec2020-pq.png"]["decoded_samples"])
    assert (
        list(_png_chunks(FIXTURES / "rgb16-rec2020-pq.png")[b"cICP"])
        == expected["rgb16-rec2020-pq.png"]["png_cicp"]
    )


def test_jpeg_decoded_samples_and_sampling_are_frozen() -> None:
    expected = _expectations()["rgb8-srgb.jpg"]
    path = FIXTURES / "rgb8-srgb.jpg"
    decoded = _read_native_source(path, ImageEncoding())

    np.testing.assert_array_equal(decoded, expected["decoded_samples"])
    with Image.open(path) as image:
        assert JpegImagePlugin.get_sampling(image) == 0


def test_float_exr_samples_compression_and_range_are_frozen() -> None:
    expected = _expectations()["rgb32-rec2020-linear.exr"]
    path = FIXTURES / "rgb32-rec2020-linear.exr"
    decoded = _read_native_source(path, ImageEncoding("float32", "rec2020", "linear", 203.0))

    np.testing.assert_allclose(decoded, expected["decoded_samples"], rtol=0.0, atol=0.0)
    assert np.isfinite(decoded).all()
    assert float(decoded.min()) < 0.0
    assert float(decoded.max()) > 1.0
    with OpenEXR.File(str(path)) as image:
        assert str(image.header()["compression"]) == "Compression.PIZ_COMPRESSION"
