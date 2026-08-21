from pathlib import Path

import pytest

from pano_stitch.screenshot_watch import image_snapshot, wait_for_new_screenshot


def test_wait_for_new_screenshot_ignores_existing_and_unsupported_files(tmp_path: Path) -> None:
    existing = tmp_path / "old.png"
    existing.write_bytes(b"old")
    before = image_snapshot(tmp_path)
    (tmp_path / "notes.txt").write_text("not an image", encoding="utf-8")
    new_image = tmp_path / "new.PNG"
    new_image.write_bytes(b"pixels")

    detected = wait_for_new_screenshot(tmp_path, before, timeout_seconds=0.1, poll_seconds=0)

    assert detected == new_image


def test_wait_for_new_screenshot_rejects_multiple_new_files(tmp_path: Path) -> None:
    before = image_snapshot(tmp_path)
    (tmp_path / "one.png").write_bytes(b"one")
    (tmp_path / "two.png").write_bytes(b"two")

    with pytest.raises(ValueError, match="multiple new screenshots"):
        wait_for_new_screenshot(tmp_path, before, timeout_seconds=0.1, poll_seconds=0)


def test_wait_for_new_screenshot_waits_for_non_empty_file(tmp_path: Path) -> None:
    before = image_snapshot(tmp_path)
    (tmp_path / "empty.png").touch()

    with pytest.raises(TimeoutError, match="did not become stable"):
        wait_for_new_screenshot(tmp_path, before, timeout_seconds=0.01, poll_seconds=0)
