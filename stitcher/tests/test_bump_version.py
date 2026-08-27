from __future__ import annotations

import subprocess
import sys
from pathlib import Path

TARGETS = (
    "stitcher/pyproject.toml",
    "stitcher/src/pano_stitch/__init__.py",
    "stitcher/uv.lock",
    "mod/cet/PanoramaCaptureProbe/init.lua",
    "mod/src/plugin.cpp",
    "contracts/example-session.json",
    "release/build-windows-release.ps1",
    ".github/workflows/release.yml",
)


def _copy_version_targets(project_root: Path, destination: Path) -> None:
    for relative_path in TARGETS:
        source = project_root / relative_path
        target = destination / relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())


def test_bump_version_updates_every_release_declaration(tmp_path: Path) -> None:
    project_root = Path(__file__).resolve().parents[2]
    _copy_version_targets(project_root, tmp_path)

    result = subprocess.run(
        [
            sys.executable,
            str(project_root / "release" / "bump_version.py"),
            "2.3.4",
            "--project-root",
            str(tmp_path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    assert set(result.stdout.splitlines()) == set(TARGETS)
    assert 'version = "2.3.4"' in (tmp_path / "stitcher/pyproject.toml").read_text()
    assert (
        '__version__ = "2.3.4"' in (tmp_path / "stitcher/src/pano_stitch/__init__.py").read_text()
    )
    assert (
        'local MOD_VERSION = "2.3.4"'
        in (tmp_path / "mod/cet/PanoramaCaptureProbe/init.lua").read_text()
    )
    assert "RED4EXT_V1_SEMVER(2, 3, 4)" in (tmp_path / "mod/src/plugin.cpp").read_text()
    assert '"mod_version": "2.3.4"' in (tmp_path / "contracts/example-session.json").read_text()
    assert (
        '[string]$Version = "2.3.4"' in (tmp_path / "release/build-windows-release.ps1").read_text()
    )
    assert 'default: "2.3.4"' in (tmp_path / ".github/workflows/release.yml").read_text()


def test_bump_version_rejects_invalid_version_without_writes(tmp_path: Path) -> None:
    project_root = Path(__file__).resolve().parents[2]
    _copy_version_targets(project_root, tmp_path)
    before = {path: (tmp_path / path).read_bytes() for path in TARGETS}

    result = subprocess.run(
        [
            sys.executable,
            str(project_root / "release" / "bump_version.py"),
            "v2.3.4",
            "--project-root",
            str(tmp_path),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode != 0
    assert "version must use MAJOR.MINOR.PATCH" in result.stderr
    assert before == {path: (tmp_path / path).read_bytes() for path in TARGETS}
