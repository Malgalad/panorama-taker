"""Discover and manage CET panorama capture sessions."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from pano_stitch.metadata import SessionMetadata, load_session

MOD_RELATIVE_PATH = (
    Path("bin") / "x64" / "plugins" / "cyber_engine_tweaks" / "mods" / "PanoramaCaptureProbe"
)
SESSION_PATTERN = "PanoramaCaptureBridge.pano-*.json"


@dataclass(frozen=True)
class SessionRecord:
    """One discoverable CET session and its source files."""

    path: Path
    metadata: SessionMetadata
    image_paths: tuple[Path, ...]
    local_date: str
    stitched_name: str | None
    error: str | None = None

    @property
    def complete(self) -> bool:
        return self.metadata.completed


def mod_directory(game_directory: Path) -> Path:
    """Return the installed PanoramaCaptureProbe directory for a game."""

    return game_directory.expanduser().resolve() / MOD_RELATIVE_PATH


def _local_date(session_id: str) -> str:
    try:
        timestamp = int(session_id.split("-", 1)[0])
        return datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M:%S")
    except (ValueError, OSError, OverflowError):
        return "Unknown date"


def discover_sessions(game_directory: Path, history: dict[str, Any]) -> tuple[SessionRecord, ...]:
    """Load valid session JSON files from the game mod directory."""

    directory = mod_directory(game_directory)
    if not directory.is_dir():
        return ()
    records: list[SessionRecord] = []
    for path in sorted(directory.glob(SESSION_PATTERN), key=lambda item: item.name, reverse=True):
        try:
            metadata = load_session(path, image_directory=directory)
            image_paths = tuple(dict.fromkeys(Path(frame.filename) for frame in metadata.frames))
            key = history_key(game_directory, metadata.session_id)
            entry = history.get(key, {})
            stitched_name = entry.get("output_name") if isinstance(entry, dict) else None
            records.append(
                SessionRecord(
                    path, metadata, image_paths, _local_date(metadata.session_id), stitched_name
                )
            )
        except (OSError, ValueError) as error:
            records.append(
                SessionRecord(
                    path,
                    _error_metadata(path),
                    (),
                    "Unknown date",
                    None,
                    str(error),
                )
            )
    return tuple(records)


def _error_metadata(path: Path) -> SessionMetadata:
    """Create a minimal metadata value for a row that failed to load."""

    from pano_stitch.metadata import CaptureMode

    return SessionMetadata(0, path.stem, CaptureMode.HORIZONTAL, 1.0, 1.0, 0.0, (), False)


def history_key(game_directory: Path, session_id: str) -> str:
    return f"{game_directory.expanduser().resolve()}::{session_id}"


def mark_stitched(
    history: dict[str, Any], game_directory: Path, session_id: str, output_name: str
) -> None:
    history[history_key(game_directory, session_id)] = {"output_name": output_name}


def deletion_targets(record: SessionRecord, include_images: bool) -> tuple[Path, ...]:
    targets = [record.path]
    if include_images:
        targets.extend(record.image_paths)
    return tuple(dict.fromkeys(targets))


def delete_files(paths: tuple[Path, ...]) -> tuple[int, int]:
    deleted = missing = 0
    for path in paths:
        try:
            path.unlink()
            deleted += 1
        except FileNotFoundError:
            missing += 1
    return deleted, missing
