from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "addon.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")


def test_addon_build_requires_64_bit_windows() -> None:
    assert "if(NOT WIN32)" in CMAKE
    assert "CMAKE_SIZEOF_VOID_P EQUAL 8" in CMAKE


def test_bridge_failure_paths_are_present() -> None:
    assert '"ERROR:runtime_destroyed"' in SOURCE
    assert "std::filesystem::exists(g_bridge_request_path, poll_error)" in SOURCE
    assert "output.flush();" in SOURCE
    assert "if (!output)" in SOURCE
