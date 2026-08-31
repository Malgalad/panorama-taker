from __future__ import annotations

import shutil
from pathlib import Path

import pytest

from pano_stitch.metadata import CaptureMode, load_session
from pano_stitch.sessions import (
    deletion_targets,
    discover_sessions,
    history_key,
    mark_stitched,
    mod_directory,
)

FIXTURES = Path(__file__).parent / "fixtures" / "application_contracts"
SCHEMA = Path(__file__).parents[2] / "contracts" / "session.schema.json"


def test_shared_metadata_contract() -> None:
    session = load_session(FIXTURES / "shared-valid.json", SCHEMA)

    assert session.session_id == "shared-valid"
    assert session.capture_mode is CaptureMode.FULL_SPHERE
    assert session.completed is True
    assert session.frames[0].filename == "shared.png"
    assert session.image_encoding.transfer_function == "srgb"


def test_invalid_shared_metadata_contract() -> None:
    with pytest.raises(ValueError, match=r"viewport\.width"):
        load_session(FIXTURES / "shared-invalid-viewport.json", SCHEMA)


def test_shared_metadata_default_encoding_contract() -> None:
    session = load_session(FIXTURES / "shared-default-encoding.json", SCHEMA)

    assert session.session_id == "shared-default-encoding"
    assert session.capture_mode is CaptureMode.HORIZONTAL
    assert session.horizontal_fov_deg == 80.0
    assert session.vertical_fov_deg == 50.0
    assert session.overlap_fraction == 0.1
    assert session.completed is False
    assert session.image_encoding.sample_type == "uint8"
    assert session.image_encoding.color_primaries == "srgb"
    assert session.image_encoding.transfer_function == "srgb"
    assert session.image_encoding.reference_white_nits == 100.0
    assert session.frames[0].camera_basis_row_major == (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


@pytest.mark.parametrize("state, completed", [("complete", True), ("incomplete", False)])
def test_cet_state_and_moved_path_contract(tmp_path: Path, state: str, completed: bool) -> None:
    image_root = tmp_path / "moved images"
    image_root.mkdir()
    image = image_root / f"{state}.png"
    image.write_bytes(b"fixture")

    session = load_session(FIXTURES / f"cet-{state}.json", image_directory=image_root)

    assert session.completed is completed
    assert session.frames[0].filename == str(image)
    assert session.frames[0].camera_basis_row_major == pytest.approx(
        (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    )
    assert session.image_encoding.transfer_function == "pq"


def test_cet_invalid_state_contract() -> None:
    with pytest.raises(ValueError, match="state must be"):
        load_session(FIXTURES / "cet-invalid.json")


def test_discovery_history_error_and_deletion_contract(tmp_path: Path) -> None:
    game = tmp_path / "Cyberpunk 2077"
    directory = mod_directory(game)
    directory.mkdir(parents=True)
    complete = directory / "PanoramaCaptureBridge.pano-1700000000-1.json"
    incomplete = directory / "PanoramaCaptureBridge.pano-1700000100-1.json"
    invalid = directory / "PanoramaCaptureBridge.pano-1700000200-1.json"
    shutil.copyfile(FIXTURES / "cet-complete.json", complete)
    shutil.copyfile(FIXTURES / "cet-incomplete.json", incomplete)
    shutil.copyfile(FIXTURES / "cet-invalid.json", invalid)
    (directory / "complete.png").write_bytes(b"complete")
    history: dict[str, object] = {}
    mark_stitched(history, game, "1700000000-1", "stitched.jpg")

    records = discover_sessions(game, history)

    assert [record.path for record in records] == [invalid, incomplete, complete]
    assert records[0].error is not None
    assert records[1].complete is False
    assert records[2].stitched_name == "stitched.jpg"
    assert history_key(game, "1700000000-1") in history
    assert deletion_targets(records[2], False) == (complete,)
    assert deletion_targets(records[2], True) == (complete, directory / "complete.png")
