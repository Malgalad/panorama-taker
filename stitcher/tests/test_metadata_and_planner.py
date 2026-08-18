import json
from pathlib import Path

import pytest

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
