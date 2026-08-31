from __future__ import annotations

import sys
from pathlib import Path

import pytest

from pano_stitch.application import (
    ApplicationErrorCategory,
    ApplicationEvent,
    ApplicationOperation,
    ApplicationSettings,
    ExposureEdits,
    begin_operation,
    cancellation_requested,
    default_output_name,
    finish_operation,
    plan_outputs,
    requested_render_operation,
)
from pano_stitch.cli import _parser


def test_headless_application_module_does_not_import_tkinter() -> None:
    assert "tkinter" not in sys.modules or "tkinter" not in vars(sys.modules[__name__])


def test_settings_defaults_malformed_history_and_round_trip() -> None:
    assert ApplicationSettings.from_mapping(None) == ApplicationSettings()
    settings = ApplicationSettings.from_mapping(
        {
            "game_dir": "game",
            "image_dir": "images",
            "output_dir": "output",
            "stitched_sessions": "invalid",
            "auto_contrast": False,
        }
    )

    assert settings.stitched_sessions == {}
    assert ApplicationSettings.from_mapping(settings.to_mapping()) == settings


def test_output_naming_companions_and_existing_decision(tmp_path: Path) -> None:
    name = default_output_name("panorama.png", "session", ".jpg")
    plan = plan_outputs(tmp_path, name, ".jpg", coverage=True, thumbnail=True)

    assert plan.panorama == tmp_path / "panorama-session.jpg"
    assert plan.coverage == tmp_path / "panorama-session-coverage.png"
    assert plan.thumbnail == tmp_path / "panorama-session-thumbnail.jpg"
    assert plan.existing() == ()
    plan.coverage.write_bytes(b"existing")
    assert plan.existing() == (plan.coverage,)


def test_exposure_edit_contract() -> None:
    edits = ExposureEdits((1.0, 2.0, 3.0), target=0, selected=frozenset({1, 2}))

    assert edits.apply_match(0.5).gains == (1.0, 1.0, 1.5)
    assert edits.discard().gains == (1.0, 1.0, 1.0)


def test_operation_event_and_error_categories_are_stable() -> None:
    assert requested_render_operation("ready") is ApplicationOperation.PREVIEW
    assert requested_render_operation("preview") is ApplicationOperation.RENDER
    assert requested_render_operation("busy") is None
    assert ApplicationEvent("progress", (1, 2)).kind == "progress"
    assert begin_operation("ready", "preview") is not None
    assert begin_operation("busy", "render") is None
    assert finish_operation("busy", "preview") == "preview"
    assert finish_operation("closing", "preview") == "closing"
    assert cancellation_requested(True) is True
    assert {category.value for category in ApplicationErrorCategory} == {
        "invalid_input",
        "cancelled",
        "gpu_unavailable",
        "render_failed",
    }


def test_cli_defaults_and_required_help_tokens() -> None:
    parser = _parser()
    arguments = parser.parse_args(["render", "session.json", "--output", "output.jpg"])

    assert arguments.blend == "hard"
    assert arguments.jpeg_quality == 95
    assert arguments.memory_budget_mib == 1024
    assert arguments.auto_contrast is True
    assert arguments.gpu is True
    help_text = parser.format_help()
    assert "validate" in help_text
    assert "render" in help_text


@pytest.mark.parametrize("value", ["", "custom.png", " custom.exr "])
def test_edited_output_names_are_preserved(value: str) -> None:
    expected = "panorama-session.png" if not value.strip() else value.strip()
    assert default_output_name(value, "session", ".png") == expected
