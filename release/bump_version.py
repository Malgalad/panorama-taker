#!/usr/bin/env python3
"""Synchronize project-owned release version declarations."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

VERSION_PATTERN = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")


@dataclass(frozen=True)
class VersionTarget:
    path: str
    pattern: re.Pattern[str]
    replacement: str


def _targets(version: str) -> tuple[VersionTarget, ...]:
    major, minor, patch = version.split(".")
    return (
        VersionTarget(
            "stitcher/pyproject.toml",
            re.compile(r'(?m)^version = "[^"]+"$'),
            f'version = "{version}"',
        ),
        VersionTarget(
            "stitcher/src/pano_stitch/__init__.py",
            re.compile(r'(?m)^__version__ = "[^"]+"$'),
            f'__version__ = "{version}"',
        ),
        VersionTarget(
            "stitcher/uv.lock",
            re.compile(r'(?m)(^name = "pano-stitch"\n)version = "[^"]+"$'),
            rf'\g<1>version = "{version}"',
        ),
        VersionTarget(
            "mod/cet/PanoramaCaptureProbe/init.lua",
            re.compile(r'(?m)^local MOD_VERSION = "[^"]+"$'),
            f'local MOD_VERSION = "{version}"',
        ),
        VersionTarget(
            "mod/src/plugin.cpp",
            re.compile(r"RED4EXT_V1_SEMVER\(\d+, \d+, \d+\)"),
            f"RED4EXT_V1_SEMVER({major}, {minor}, {patch})",
        ),
        VersionTarget(
            "contracts/example-session.json",
            re.compile(r'(?m)^(  "mod_version": )"[^"]+"'),
            rf'\g<1>"{version}"',
        ),
        VersionTarget(
            "release/build-windows-release.ps1",
            re.compile(r'(?m)^(    \[string\]\$Version = )"[^"]+"'),
            rf'\g<1>"{version}"',
        ),
        VersionTarget(
            ".github/workflows/release.yml",
            re.compile(r'(?m)^(        default: )"[^"]+"'),
            rf'\g<1>"{version}"',
        ),
    )


def bump_version(project_root: Path, version: str) -> list[Path]:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise ValueError("version must use MAJOR.MINOR.PATCH without a leading v")

    updates: list[tuple[Path, str]] = []
    for target in _targets(version):
        path = project_root / target.path
        source = path.read_text(encoding="utf-8")
        updated, count = target.pattern.subn(target.replacement, source)
        if count != 1:
            raise RuntimeError(
                f"expected one version declaration in {target.path}, found {count}"
            )
        updates.append((path, updated))

    for path, updated in updates:
        path.write_text(updated, encoding="utf-8")
    return [path for path, _updated in updates]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="new version in MAJOR.MINOR.PATCH form")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help=argparse.SUPPRESS,
    )
    arguments = parser.parse_args()
    project_root = arguments.project_root.resolve()

    for path in bump_version(project_root, arguments.version):
        print(path.relative_to(project_root))


if __name__ == "__main__":
    main()
