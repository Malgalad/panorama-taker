import importlib.util
import json
from pathlib import Path
from types import ModuleType

import pytest


def _extractor() -> ModuleType:
    path = Path(__file__).resolve().parents[1] / "scripts" / "extract_cet_pose_manifest.py"
    spec = importlib.util.spec_from_file_location("extract_cet_pose_manifest", path)
    if spec is None or spec.loader is None:
        raise AssertionError("could not load CET manifest extractor")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _pose_line(index: int = 1, count: int = 1) -> str:
    return (
        f"[PanoramaCaptureProbe] POSE_METADATA index={index}/{count} row=0 column=0 "
        "commanded_yaw=0.000000 commanded_pitch=0.000000 "
        "observed_forward=(0.0,1.0,0.0) observed_right=(1.0,0.0,0.0) "
        "observed_up=(0.0,0.0,1.0) hfov=90.0 vfov=60.0 "
        "settle_seconds=1.5 basis_valid=true"
    )


def test_build_document_binds_supported_images_in_filename_order(tmp_path: Path) -> None:
    extractor = _extractor()
    log = tmp_path / "cet.log"
    log.write_text(_pose_line() + "\n", encoding="utf-8")
    image_directory = tmp_path / "images"
    image_directory.mkdir()
    (image_directory / "Cyberpunk2077 2026-08-21 01-00-00_001.png").write_bytes(b"png")
    (image_directory / "ignored.txt").touch()

    document = extractor.build_document(
        log,
        image_directory,
        "Cyberpunk2077 2026-08-21 01-00-00_001.png",
        "Cyberpunk2077 2026-08-21 01-00-00_001.png",
        "session-1",
        "0.1.13",
        "2026-08-21T01:00:00Z",
    )

    assert document["session_id"] == "session-1"
    assert document["frames"][0]["filename"] == "Cyberpunk2077 2026-08-21 01-00-00_001.png"


def test_build_document_rejects_record_image_count_mismatch(tmp_path: Path) -> None:
    extractor = _extractor()
    log = tmp_path / "cet.log"
    log.write_text(_pose_line() + "\n", encoding="utf-8")
    image_directory = tmp_path / "images"
    image_directory.mkdir()
    (image_directory / "one.png").write_bytes(b"png")
    (image_directory / "two.png").write_bytes(b"png")

    with pytest.raises(ValueError, match="record/image count mismatch: 1 records, 2 images"):
        extractor.build_document(
            log,
            image_directory,
            "one.png",
            "two.png",
            "session-1",
            "0.1.13",
            "2026-08-21T01:00:00Z",
        )


def test_write_atomic_replaces_partial_and_leaves_no_partial(tmp_path: Path) -> None:
    extractor = _extractor()
    output = tmp_path / "session.json"

    extractor._write_atomic(output, {"session_id": "session-1"})

    assert json.loads(output.read_text(encoding="utf-8"))["session_id"] == "session-1"
    assert not output.with_name("session.json.partial").exists()
