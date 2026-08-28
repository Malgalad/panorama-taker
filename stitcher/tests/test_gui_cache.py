from __future__ import annotations

import threading
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image

from pano_stitch.gui import (
    StitcherApp,
    UiState,
    _backend_status,
    _compose_preview_display,
    _magnified_crop_box,
)


class _Cache:
    def __init__(self) -> None:
        self.reasons: list[str] = []

    def invalidate(self, reason: str) -> None:
        self.reasons.append(reason)


class _Widget:
    def __init__(self) -> None:
        self.state: str | None = None
        self.packed = True
        self.configuration: dict[str, object] = {}

    def configure(self, **_kwargs: object) -> None:
        self.configuration.update(_kwargs)
        state = _kwargs.get("state")
        if isinstance(state, str):
            self.state = state

    def pack_forget(self) -> None:
        self.packed = False

    def winfo_width(self) -> int:
        return 100

    def winfo_height(self) -> int:
        return 50


class _Variable:
    def __init__(self, value: str) -> None:
        self._value = value

    def get(self) -> str:
        return self._value

    def set(self, value: str) -> None:
        self._value = value


class _BoolVariable:
    def __init__(self, value: bool) -> None:
        self.value = value

    def get(self) -> bool:
        return self.value

    def set(self, value: bool) -> None:
        self.value = value


class _IntVariable:
    def __init__(self, value: int) -> None:
        self.value = value

    def get(self) -> int:
        return self.value


class _StatusVariable:
    def __init__(self) -> None:
        self.value = ""

    def set(self, value: str) -> None:
        self.value = value


class _Root:
    def after(self, _milliseconds: int, _callback: object) -> str:
        return "after-id"

    def after_cancel(self, _identifier: str) -> None:
        pass


def test_magnified_crop_tracks_pointer_and_clamps_at_edges() -> None:
    assert _magnified_crop_box((2000, 1000), (500, 250), (0.5, 0.5)) == (
        750,
        375,
        1250,
        625,
    )


def test_render_operation_is_driven_by_ui_state() -> None:
    app = StitcherApp.__new__(StitcherApp)

    app._state = UiState.READY
    assert app._requested_render_operation() == "preview"
    app._state = UiState.PREVIEW
    assert app._requested_render_operation() == "render"
    app._state = UiState.BUSY
    assert app._requested_render_operation() is None
    app._state = UiState.CLOSING
    assert app._requested_render_operation() is None
    assert _magnified_crop_box((2000, 1000), (500, 250), (0.0, 0.0)) == (0, 0, 500, 250)
    assert _magnified_crop_box((2000, 1000), (500, 250), (1.2, 1.2)) == (
        1500,
        750,
        2000,
        1000,
    )


def test_gui_cache_invalidates_for_discard_and_input_change() -> None:
    app = StitcherApp.__new__(StitcherApp)
    cache = _Cache()
    app._cuda_session_cache = cache  # type: ignore[assignment]
    app._preview_report = None
    app._preview_photo = None
    app._display_lock = threading.Lock()
    app._display_generation = 0
    app._display_pending = None
    app.render_button = _Widget()
    app.discard_button = _Widget()
    app.discard_exposure_button = _Widget()  # type: ignore[assignment]
    app.overlay_var = _BoolVariable(True)  # type: ignore[assignment]
    app.root = _Root()
    app._validated = True
    app._validation_after = None
    app.session_var = _Variable("session.json")
    app.image_dir_var = _Variable("images")

    app.discard_preview()
    app._input_changed()

    assert cache.reasons == ["preview discarded", "input changed"]
    assert app._validated is False
    assert app._validation_after == "after-id"
    assert app.overlay_var.get() is False
    assert app.discard_exposure_button.packed is False


def test_gui_preview_option_change_does_not_revalidate_session() -> None:
    app = StitcherApp.__new__(StitcherApp)
    cache = _Cache()
    app._cuda_session_cache = cache  # type: ignore[assignment]
    app._preview_report = None
    app._validated = True
    app._validation_after = None

    app._preview_option_changed()

    assert cache.reasons == []
    assert app._validated is True
    assert app._validation_after is None


def test_output_name_change_preserves_preview_and_validation() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._output_name_dirty = False
    app._preview_report = object()  # type: ignore[assignment]
    app._validated = True

    app._output_name_changed()

    assert app._output_name_dirty is True
    assert app._preview_report is not None
    assert app._validated is True


def test_valid_target_selection_exits_target_mode() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._worker = None
    app._target_mode = True
    app._target_pose = None
    app._selected_poses = set()
    app.target_button = _Widget()  # type: ignore[assignment]
    app.preview_label = _Widget()  # type: ignore[assignment]
    app.status_var = _StatusVariable()  # type: ignore[assignment]
    app._refresh_pose_grid = lambda: None  # type: ignore[method-assign]
    app._refresh_preview_display = lambda: None  # type: ignore[method-assign]

    app._pose_clicked(3)

    assert app._target_pose == 3
    assert app._target_mode is False


def test_preview_click_maps_active_crop_to_compact_mask() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._worker = None
    app._preview_image = Image.new("RGB", (400, 200))
    app._preview_viewport = (100, 50)
    app._active_preview_crop = (200, 100, 300, 150)
    mask = np.zeros((2, 4), dtype=bool)
    mask[1, 2] = True
    app._coverage_masks = (mask,)
    app.overlay_var = _BoolVariable(True)  # type: ignore[assignment]
    app.preview_label = _Widget()  # type: ignore[assignment]
    app._target_mode = False
    app._target_pose = None
    app._selected_poses = set()
    app._refresh_pose_grid = lambda: None  # type: ignore[method-assign]
    app._refresh_preview_display = lambda: None  # type: ignore[method-assign]

    app._preview_clicked(SimpleNamespace(x=50, y=25))

    assert app._selected_poses == {0}


def test_magnified_preview_hover_marks_click_candidates_and_pose_border() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._worker = None
    app._preview_image = Image.new("RGB", (400, 200))
    app._preview_viewport = (100, 50)
    mask = np.zeros((2, 4), dtype=bool)
    mask[1, 2] = True
    app._coverage_masks = (mask,)
    app.overlay_var = _BoolVariable(True)  # type: ignore[assignment]
    app.preview_label = _Widget()  # type: ignore[assignment]
    app._target_mode = False
    app._target_pose = None
    app._selected_poses = set()
    app._hovered_poses = set()
    app._pose_widgets = [_Widget()]  # type: ignore[list-item]
    app.match_exposure_button = _Widget()  # type: ignore[assignment]
    requested: list[tuple[int, int, int, int]] = []
    app._refresh_preview_display = requested.append  # type: ignore[method-assign]

    app._magnify_preview(SimpleNamespace(x=50, y=25))

    assert app._hovered_poses == {0}
    assert app._pose_widgets[0].configuration["highlightbackground"] == "magenta"
    assert requested == [(150, 75, 250, 125)]


def test_preview_overlay_composes_compact_mask_for_magnified_crop() -> None:
    source = Image.new("RGB", (400, 200), "black")
    mask = np.zeros((50, 100), dtype=bool)
    mask[:, 50:] = True

    display = _compose_preview_display(
        source,
        (100, 50),
        (mask,),
        True,
        (200, 100, 300, 150),
        frozenset({0}),
        None,
        False,
    )

    assert display.size == (100, 50)
    assert np.any(np.asarray(display)[:, :, 0] > 0)


def test_preview_hover_tints_and_outlines_only_hovered_pose_without_overlay() -> None:
    source = Image.new("RGB", (6, 3), (100, 100, 100))
    left = np.zeros((3, 6), dtype=bool)
    left[:, :4] = True
    right = ~left

    display = _compose_preview_display(
        source,
        (6, 3),
        (left, right),
        False,
        None,
        frozenset({0}),
        None,
        False,
    )

    pixels = np.asarray(display)
    assert np.array_equal(pixels[1, 0], np.array((255, 0, 255), dtype=np.uint8))
    assert np.array_equal(pixels[1, 1], np.array((131, 80, 131), dtype=np.uint8))
    assert np.array_equal(pixels[1, 5], np.array((100, 100, 100), dtype=np.uint8))


def test_enabling_form_preserves_match_exposure_disabled_state() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app.advanced_button = _Widget()  # type: ignore[assignment]
    app.cancel_button = _Widget()  # type: ignore[assignment]
    app.render_button = _Widget()  # type: ignore[assignment]
    app.match_exposure_button = _Widget()  # type: ignore[assignment]
    app._form_controls = [app.match_exposure_button]
    app._pose_widgets = []
    app._target_pose = None
    app._selected_poses = set()
    app._validated = True
    app._output_dir_writable = True

    app._set_busy(False)

    assert app.match_exposure_button.state == "disabled"


def test_expected_resolution_uses_scale_or_explicit_width() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._native_output_size = (12000, 6000)
    app._resolution_geometry = ("full_sphere", 180.0)
    app.expected_resolution_var = _Variable("")  # type: ignore[assignment]
    app.resolution_percent_var = _IntVariable(50)  # type: ignore[assignment]
    app.width_var = _Variable("")  # type: ignore[assignment]

    app._update_expected_resolution()
    assert app.expected_resolution_var.get() == "Expected: 6000 × 3000"

    app.width_var = _Variable("4097")  # type: ignore[assignment]
    app._update_expected_resolution()
    assert app.expected_resolution_var.get() == "Expected: 4096 × 2048"


def test_output_directory_change_only_checks_writability(tmp_path: Path) -> None:
    app = StitcherApp.__new__(StitcherApp)
    app.output_dir_var = _Variable(str(tmp_path))
    app.render_button = _Widget()
    app.status_var = _StatusVariable()
    app._validated = True
    app._validation_after = None

    app._output_directory_changed()

    assert app._output_dir_writable is True
    assert app.render_button.state == "normal"
    assert app._validated is True
    assert app._validation_after is None

    app.output_dir_var = _Variable(str(tmp_path / "not-a-directory"))
    (tmp_path / "not-a-directory").write_text("occupied", encoding="utf-8")

    app._output_directory_changed()

    assert app._output_dir_writable is False
    assert app.render_button.state == "disabled"
    assert app.status_var.value == "Output directory is not writable."
    assert app._validated is True
    assert app._validation_after is None


def test_cuda_backend_status_formats_reserve_size(tmp_path: Path) -> None:
    assert (
        _backend_status(
            "cuda resident",
            "CUDA resident; reserve=402653184 bytes",
            cuda_requested=True,
            log_directory=tmp_path,
        )
        == "CUDA (reserved 384 MB VRAM)"
    )
    assert (
        _backend_status(
            "cuda banded",
            "CUDA banded; reserve=4992899482 bytes",
            cuda_requested=True,
            log_directory=tmp_path,
        )
        == "CUDA (reserved 4.65 GB VRAM)"
    )


def test_cuda_fallback_status_points_to_log_directory(tmp_path: Path) -> None:
    assert (
        _backend_status("cpu", "driver failed", cuda_requested=True, log_directory=tmp_path)
        == f"CPU (failed to initialize CUDA; see error log in {tmp_path})"
    )
    assert (
        _backend_status(
            "cpu", "GPU acceleration disabled", cuda_requested=False, log_directory=tmp_path
        )
        == "CPU"
    )
