import json
from pathlib import Path

from pano_stitch.sessions import (
    delete_files,
    deletion_targets,
    discover_sessions,
    history_key,
    mark_stitched,
    mod_directory,
)


def _write_session(path: Path, session_id: str, state: str = "completed") -> None:
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "session_id": session_id,
                "horizontal_fov_deg": 100,
                "vertical_fov_deg": 60,
                "state": state,
                "poses": [
                    {
                        "index": 0,
                        "commanded_yaw_deg": 0,
                        "commanded_pitch_deg": 0,
                        "observed_pitch_deg": 0,
                        "forward": [0, 0, 1],
                        "right": [1, 0, 0],
                        "up": [0, 1, 0],
                        "screenshot_path": str(path.with_suffix(".png")),
                    }
                ],
            }
        ),
        encoding="utf-8",
    )


def test_discovery_and_stitch_history(tmp_path: Path) -> None:
    game = tmp_path / "game"
    directory = mod_directory(game)
    directory.mkdir(parents=True)
    first = directory / "PanoramaCaptureBridge.pano-1700000000-1.json"
    second = directory / "PanoramaCaptureBridge.pano-1700000100-1.json"
    _write_session(first, "1700000000-1")
    _write_session(second, "1700000100-1", "active")
    history: dict[str, object] = {}
    mark_stitched(history, game, "1700000000-1", "my-panorama.jpg")

    records = discover_sessions(game, history)

    assert [record.path for record in records] == [second, first]
    assert records[0].local_date != "Unknown date"
    assert records[0].complete is False
    assert records[1].stitched_name == "my-panorama.jpg"
    assert history_key(game, "1700000000-1") in history


def test_deletion_targets_and_missing_files(tmp_path: Path) -> None:
    game = tmp_path / "game"
    directory = mod_directory(game)
    directory.mkdir(parents=True)
    session_path = directory / "PanoramaCaptureBridge.pano-1700000000-1.json"
    image_path = session_path.with_suffix(".png")
    _write_session(session_path, "1700000000-1")
    image_path.write_bytes(b"image")
    record = discover_sessions(game, {})[0]

    assert deletion_targets(record, False) == (session_path,)
    deleted, missing = delete_files(deletion_targets(record, True))

    assert deleted == 2
    assert missing == 0
    assert not session_path.exists()
    assert not image_path.exists()
