from __future__ import annotations

import threading
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image

import pano_stitch.gui as gui_module
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
    def __init__(self) -> None:
        self.geometry_value = ""

    def after(self, _milliseconds: int, _callback: object) -> str:
        return "after-id"

    def after_cancel(self, _identifier: str) -> None:
        pass

    def update_idletasks(self) -> None:
        pass

    def winfo_width(self) -> int:
        return 680

    def winfo_height(self) -> int:
        return 600

    def geometry(self, value: str) -> None:
        self.geometry_value = value


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


def test_close_cancels_active_worker_and_finishes_exactly_once() -> None:
    calls: list[str] = []

    class Worker:
        alive = True

        def is_alive(self) -> bool:
            return self.alive

    app = StitcherApp.__new__(StitcherApp)
    worker = Worker()
    app._state = UiState.BUSY
    app._worker = worker  # type: ignore[assignment]
    app._close_when_idle = False
    app._close_finished = False
    app.status_var = _StatusVariable()  # type: ignore[assignment]
    app.cancel = lambda: calls.append("cancel")  # type: ignore[method-assign]
    app._close_gpu_preview_display = lambda: calls.append("preview_close")  # type: ignore[method-assign]
    app._gpu_session_cache = SimpleNamespace(close=lambda: calls.append("cache_close"))
    app._save_settings = lambda: calls.append("save")  # type: ignore[method-assign]
    app.root = SimpleNamespace(destroy=lambda: calls.append("destroy"))

    app._close()
    app._close()

    assert app._state is UiState.CLOSING
    assert app._close_when_idle is True
    assert calls == ["cancel"]
    worker.alive = False

    app._close()
    app._close()

    assert calls == ["cancel", "preview_close", "cache_close", "save", "destroy"]


def test_gui_cache_invalidates_for_discard_and_input_change() -> None:
    app = StitcherApp.__new__(StitcherApp)
    cache = _Cache()
    app._gpu_session_cache = cache  # type: ignore[assignment]
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
    app._gpu_session_cache = cache  # type: ignore[assignment]
    app._state = UiState.READY
    app._preview_image = None
    app._preview_report = None
    app._validated = True
    app._validation_after = None

    app._preview_option_changed()

    assert cache.reasons == []
    assert app._validated is True
    assert app._validation_after is None


def test_gui_preview_option_change_discards_preview_when_report_is_missing() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._state = UiState.PREVIEW
    app._preview_image = Image.new("RGB", (2, 1))
    app._preview_report = None
    discarded = False

    def discard_preview() -> None:
        nonlocal discarded
        discarded = True

    app.discard_preview = discard_preview  # type: ignore[method-assign]

    app._preview_option_changed()

    assert discarded is True


def test_delete_selected_discards_preview(monkeypatch: object) -> None:
    app = StitcherApp.__new__(StitcherApp)
    app.sessions_tree = SimpleNamespace(selection=lambda: ("0",))
    app._sessions = (
        SimpleNamespace(
            error=None,
            complete=False,
            path=Path("session.json"),
            image_paths=(),
        ),
    )
    app.status_var = _StatusVariable()  # type: ignore[assignment]
    discarded = False

    def discard_preview() -> None:
        nonlocal discarded
        discarded = True

    app.discard_preview = discard_preview  # type: ignore[method-assign]
    app._refresh_sessions = lambda: None  # type: ignore[method-assign]
    monkeypatch.setattr(gui_module, "delete_files", lambda _paths: (1, 0))  # type: ignore[attr-defined]

    app._delete_selected(False)

    assert discarded is True
    assert app.status_var.value == "Deleted 1 file(s); 0 already missing."


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


def test_preview_leave_immediately_restores_overview_photo() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._preview_magnified_photo = object()
    overview_photo = object()
    app._preview_photo = overview_photo
    app._hovered_poses = {0}
    app.preview_label = _Widget()  # type: ignore[assignment]
    app._refresh_pose_grid = lambda: None  # type: ignore[method-assign]
    requested: list[object] = []
    app._refresh_preview_display = lambda crop=None: requested.append(crop)  # type: ignore[method-assign]

    app._restore_preview_overview()

    assert app._preview_magnified_photo is None
    assert app._hovered_poses == set()
    assert app.preview_label.configuration["image"] is overview_photo
    assert requested == [None]


def test_preview_ready_expands_window_without_shrinking_height() -> None:
    class Content:
        def winfo_reqwidth(self) -> int:
            return 920

        def winfo_reqheight(self) -> int:
            return 500

    app = StitcherApp.__new__(StitcherApp)
    app.root = _Root()  # type: ignore[assignment]
    app.main_content = Content()  # type: ignore[assignment]

    app._expand_window_for_preview()

    assert app.root.geometry_value == "920x600"  # type: ignore[attr-defined]


def test_starting_final_render_retains_preview_display(monkeypatch: object) -> None:
    class Thread:
        def __init__(self, **_kwargs: object) -> None:
            pass

        def start(self) -> None:
            pass

    monkeypatch.setattr(gui_module.threading, "Thread", Thread)  # type: ignore[attr-defined]
    app = StitcherApp.__new__(StitcherApp)
    app._state = UiState.PREVIEW
    app._active_operation = None
    app._set_busy = lambda _busy: None  # type: ignore[method-assign]
    app._close_gpu_preview_display = lambda: (_ for _ in ()).throw(  # type: ignore[method-assign]
        AssertionError("preview display must be retained")
    )

    app._start_worker("render")

    assert app._state is UiState.BUSY
    assert app._state_before_busy is UiState.PREVIEW


def test_render_error_discards_invalidated_preview(monkeypatch: object) -> None:
    app = StitcherApp.__new__(StitcherApp)
    app._events = gui_module.queue.Queue()
    app._events.put(("render_error", "native failed"))
    app._state = UiState.BUSY
    app._state_before_busy = UiState.PREVIEW
    app._active_operation = "render"
    app._close_when_idle = False
    app.root = _Root()  # type: ignore[assignment]
    app.status_var = _StatusVariable()  # type: ignore[assignment]
    app.progress = {"value": 50}  # type: ignore[assignment]
    busy: list[bool] = []
    app._set_busy = busy.append  # type: ignore[method-assign]
    discarded: list[bool] = []

    def discard_preview() -> None:
        discarded.append(True)
        app._state = UiState.READY

    app.discard_preview = discard_preview  # type: ignore[method-assign]
    monkeypatch.setattr(gui_module.messagebox, "showerror", lambda *_args, **_kwargs: None)  # type: ignore[attr-defined]

    app._drain_events()

    assert app._state is UiState.READY
    assert app._active_operation is None
    assert busy == [False]
    assert discarded == [True]
    assert app.progress["value"] == 0
    assert app.status_var.value.startswith("Render failed:")


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


def test_enabling_form_preserves_exposure_action_requirements() -> None:
    app = StitcherApp.__new__(StitcherApp)
    app.advanced_button = _Widget()  # type: ignore[assignment]
    app.cancel_button = _Widget()  # type: ignore[assignment]
    app.render_button = _Widget()  # type: ignore[assignment]
    app.match_exposure_button = _Widget()  # type: ignore[assignment]
    app.automatic_exposure_button = _Widget()  # type: ignore[assignment]
    app._form_controls = [app.match_exposure_button, app.automatic_exposure_button]
    app._pose_widgets = []
    app._target_pose = None
    app._selected_poses = set()
    app._validated = True
    app._output_dir_writable = True

    app._set_busy(False)

    assert app.match_exposure_button.state == "disabled"
    assert app.automatic_exposure_button.state == "disabled"

    app._target_pose = 12
    app._refresh_pose_grid()

    assert app.match_exposure_button.state == "disabled"
    assert app.automatic_exposure_button.state == "normal"


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


def test_gpu_backend_status_formats_reserve_size(tmp_path: Path) -> None:
    assert (
        _backend_status(
            "gpu resident",
            "D3D12 resident; reserve=402653184 bytes",
            gpu_requested=True,
            log_directory=tmp_path,
        )
        == "D3D12 (reserved 384 MB VRAM)"
    )
    assert (
        _backend_status(
            "gpu banded",
            "D3D12 banded; reserve=4992899482 bytes",
            gpu_requested=True,
            log_directory=tmp_path,
        )
        == "D3D12 (reserved 4.65 GB VRAM)"
    )


def test_gpu_fallback_status_points_to_log_directory(tmp_path: Path) -> None:
    assert (
        _backend_status("cpu", "driver failed", gpu_requested=True, log_directory=tmp_path)
        == f"CPU (failed to initialize GPU; see error log in {tmp_path})"
    )
    assert (
        _backend_status(
            "cpu", "GPU acceleration disabled", gpu_requested=False, log_directory=tmp_path
        )
        == "CPU"
    )
