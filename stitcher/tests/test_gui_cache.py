from __future__ import annotations

from pano_stitch.gui import StitcherApp


class _Cache:
    def __init__(self) -> None:
        self.reasons: list[str] = []

    def invalidate(self, reason: str) -> None:
        self.reasons.append(reason)


class _Widget:
    def configure(self, **_kwargs: object) -> None:
        pass

    def pack_forget(self) -> None:
        pass


class _Variable:
    def __init__(self, value: str) -> None:
        self._value = value

    def get(self) -> str:
        return self._value


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
