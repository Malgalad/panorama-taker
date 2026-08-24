import json
from pathlib import Path

import pytest

import pano_stitch.metadata as metadata_module
from pano_stitch.metadata import CaptureMode, load_session
from pano_stitch.planner import plan_shots

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT.parent / "contracts" / "example-session.json"
SCHEMA = ROOT.parent / "contracts" / "session.schema.json"


def test_example_session_loads_against_shared_schema() -> None:
    session = load_session(EXAMPLE, SCHEMA)

    assert session.capture_mode is CaptureMode.HORIZONTAL
    assert session.session_id == "example"
    assert session.frames == ()


def test_invalid_session_reports_schema_location(tmp_path: Path) -> None:
    document = json.loads(EXAMPLE.read_text(encoding="utf-8"))
    document["viewport"]["width"] = 0
    invalid = tmp_path / "invalid.json"
    invalid.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(ValueError, match="viewport.width"):
        load_session(invalid, SCHEMA)


def test_version_one_timing_metadata_remains_accepted(tmp_path: Path) -> None:
    document = json.loads(EXAMPLE.read_text(encoding="utf-8"))
    document["render_timing"] = {
        "required_real_settle_frames": 8,
        "real_fps_at_start": 60.0,
        "presented_fps_at_start": 120.0,
        "frame_generation": {"enabled": True, "active_backend": "DLSS"},
    }
    document["frames"] = [
        {
            "index": 0,
            "filename": "legacy.png",
            "yaw_deg": 0.0,
            "pitch_deg": 0.0,
            "roll_deg": 0.0,
            "settled_real_frames": 8,
            "real_fps_before_capture": 60.0,
            "status": "captured",
        }
    ]
    legacy = tmp_path / "legacy-v1.json"
    legacy.write_text(json.dumps(document), encoding="utf-8")

    session = load_session(legacy, SCHEMA)

    assert session.frames[0].filename == "legacy.png"


def test_location_metadata_is_accepted(tmp_path: Path) -> None:
    document = json.loads(EXAMPLE.read_text(encoding="utf-8"))
    document["location"] = {"position": [1.0, 2.0, 3.0], "yaw_deg": 45.0}
    path = tmp_path / "located.json"
    path.write_text(json.dumps(document), encoding="utf-8")

    assert load_session(path, SCHEMA).session_id == "example"


def test_frozen_schema_path_uses_pyinstaller_bundle(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(metadata_module.sys, "_MEIPASS", str(tmp_path), raising=False)

    assert metadata_module._default_schema_path() == tmp_path / "contracts" / "session.schema.json"


def test_horizontal_plan_closes_without_duplicate_360_degree_pose() -> None:
    session = load_session(EXAMPLE, SCHEMA)
    plan = plan_shots(session)

    assert plan.shots
    assert plan.shots[0].yaw_deg == 0.0
    assert all(0.0 <= shot.yaw_deg < 360.0 for shot in plan.shots)
    assert plan.shots[-1].yaw_deg + plan.yaw_step_deg == pytest.approx(360.0)
    assert all(shot.pitch_deg == 0.0 for shot in plan.shots)


def test_full_sphere_plan_reaches_both_poles_within_vertical_fov(tmp_path: Path) -> None:
    document = json.loads(EXAMPLE.read_text(encoding="utf-8"))
    document["capture_mode"] = "full_sphere"
    full_sphere = tmp_path / "full-sphere.json"
    full_sphere.write_text(json.dumps(document), encoding="utf-8")
    session = load_session(full_sphere, SCHEMA)
    plan = plan_shots(session)

    pitches = [shot.pitch_deg for shot in plan.shots]
    assert min(pitches) == pytest.approx(-90.0 + plan.pitch_step_deg / 2.0)
    assert max(pitches) == pytest.approx(90.0 - plan.pitch_step_deg / 2.0)
    assert min(pitches) - plan.pitch_step_deg / 2.0 <= -90.0
    assert max(pitches) + plan.pitch_step_deg / 2.0 >= 90.0


def test_cet_metadata_loads_and_resolves_moved_windows_screenshot(tmp_path: Path) -> None:
    screenshot = tmp_path / "pose-001.png"
    screenshot.write_bytes(b"not a real png")
    document = {
        "schema_version": 1,
        "session_id": "cet-test",
        "horizontal_fov_deg": 90.6,
        "vertical_fov_deg": 59.23,
        "state": "completed",
        "poses": [
            {
                "index": 1,
                "screenshot_path": r"E:\Pictures\Cyberpunk 2077\pose-001.png",
                "commanded_yaw_deg": 0.0,
                "commanded_pitch_deg": -62.7,
                "forward": [0.0, 0.45, -0.89],
                "right": [1.0, 0.0, 0.0],
                "up": [0.0, 0.89, 0.45],
            }
        ],
    }
    metadata = tmp_path / "PanoramaCaptureBridge.pano-test.json"
    metadata.write_text(json.dumps(document), encoding="utf-8")

    session = load_session(metadata, image_directory=tmp_path)

    assert session.capture_mode is CaptureMode.FULL_SPHERE
    assert session.completed
    assert session.frames[0].filename == str(screenshot)
    assert session.frames[0].camera_basis_row_major == (
        1.0,
        0.0,
        0.0,
        0.0,
        0.45,
        0.89,
        0.0,
        -0.89,
        0.45,
    )


def test_cet_metadata_centers_the_yaw_zero_camera_direction(tmp_path: Path) -> None:
    document = {
        "schema_version": 1,
        "session_id": "cet-rotated",
        "horizontal_fov_deg": 90.6,
        "vertical_fov_deg": 59.23,
        "state": "completed",
        "poses": [
            {
                "index": 1,
                "screenshot_path": "pose-001.png",
                "commanded_yaw_deg": 0.0,
                "commanded_pitch_deg": 0.0,
                "forward": [-1.0, 0.0, 0.0],
                "right": [0.0, 1.0, 0.0],
                "up": [0.0, 0.0, 1.0],
            }
        ],
    }
    metadata = tmp_path / "PanoramaCaptureBridge.pano-rotated.json"
    metadata.write_text(json.dumps(document), encoding="utf-8")

    session = load_session(metadata)

    assert session.frames[0].camera_basis_row_major == pytest.approx(
        (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    )
