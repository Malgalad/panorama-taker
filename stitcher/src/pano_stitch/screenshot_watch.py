"""Detect stable ReShade screenshots created after a pose becomes ready."""

from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path

SUPPORTED_EXTENSIONS = frozenset({".exr", ".jpeg", ".jpg", ".png"})


@dataclass(frozen=True)
class FileStamp:
    """Filesystem state used to detect a completed screenshot write."""

    size: int
    modified_ns: int


def image_snapshot(directory: Path) -> dict[str, FileStamp]:
    """Return supported screenshot names and their current filesystem state."""

    if not directory.is_dir():
        raise ValueError(f"screenshot directory does not exist: {directory}")
    snapshot: dict[str, FileStamp] = {}
    for path in directory.iterdir():
        if not path.is_file() or path.suffix.lower() not in SUPPORTED_EXTENSIONS:
            continue
        stat = path.stat()
        snapshot[path.name] = FileStamp(stat.st_size, stat.st_mtime_ns)
    return snapshot


def wait_for_new_screenshot(
    directory: Path,
    before: dict[str, FileStamp],
    timeout_seconds: float = 30.0,
    poll_seconds: float = 0.1,
) -> Path:
    """Wait for exactly one new, non-empty screenshot whose stamp stays unchanged."""

    if timeout_seconds < 0 or poll_seconds < 0:
        raise ValueError("timeout_seconds and poll_seconds must be non-negative")
    deadline = time.monotonic() + timeout_seconds
    candidate: str | None = None
    previous: FileStamp | None = None
    while True:
        current = image_snapshot(directory)
        new_names = sorted(name for name in current if name not in before)
        if len(new_names) > 1:
            raise ValueError("multiple new screenshots detected: " + ", ".join(new_names))
        if new_names:
            candidate = new_names[0]
            stamp = current[candidate]
            if stamp.size > 0 and stamp == previous:
                return directory / candidate
            previous = stamp
        if time.monotonic() >= deadline:
            if candidate is None:
                raise TimeoutError("no new screenshot detected before timeout")
            raise TimeoutError(f"screenshot did not become stable before timeout: {candidate}")
        time.sleep(poll_seconds)
