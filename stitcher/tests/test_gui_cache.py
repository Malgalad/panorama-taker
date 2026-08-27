from __future__ import annotations

from pathlib import Path

from pano_stitch.gui import StitcherApp, _backend_status


class _Cache:
    def __init__(self) -> None:
        self.reasons: list[str] = []

    def invalidate(self, reason: str) -> None:
        self.reasons.append(reason)


class _Widget:
    def __init__(self) -> None:
        self.state: str | None = None

    def configure(self, **_kwargs: object) -> None:
        state = _kwargs.get("state")
        if isinstance(state, str):
            self.state = state

    def pack_forget(self) -> None:
        pass


class _Variable:
    def __init__(self, value: str) -> None:
        self._value = value

    def get(self) -> str:
        return self._value


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


def test_gui_cache_invalidates_for_discard_and_input_change() -> None:
    app = StitcherApp.__new__(StitcherApp)
    cache = _Cache()
    app._cuda_session_cache = cache  # type: ignore[assignment]
    app._preview_report = None
    app._preview_photo = None
    app.render_button = _Widget()
    app.discard_button = _Widget()
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
        == "Backend: CUDA RESIDENT — CUDA resident; reserve=384 MB"
    )
    assert (
        _backend_status(
            "cuda banded",
            "CUDA banded; reserve=4992899482 bytes",
            cuda_requested=True,
            log_directory=tmp_path,
        )
        == "Backend: CUDA BANDED — CUDA banded; reserve=4.65 GB"
    )


def test_cuda_fallback_status_points_to_log_directory(tmp_path: Path) -> None:
    assert (
        _backend_status("cpu", "driver failed", cuda_requested=True, log_directory=tmp_path)
        == f"Backend: CPU — CUDA error occurred; check logs in {tmp_path}"
    )
    assert (
        _backend_status(
            "cpu", "GPU acceleration disabled", cuda_requested=False, log_directory=tmp_path
        )
        == "Backend: CPU — GPU acceleration disabled"
    )
