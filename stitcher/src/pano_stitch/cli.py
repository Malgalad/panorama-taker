"""Command-line interface for metadata validation and panorama rendering."""

from __future__ import annotations

import argparse
from pathlib import Path

from pano_stitch.compositor import render_session, validate_images
from pano_stitch.metadata import load_session


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pano-stitch")
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="validate a capture manifest and its images")
    validate.add_argument("session", type=Path)
    validate.add_argument("--allow-incomplete", action="store_true")
    render = commands.add_parser("render", help="render a capture manifest to a PNG")
    render.add_argument("session", type=Path)
    render.add_argument("--output", required=True, type=Path)
    render.add_argument("--width", type=int)
    render.add_argument("--blend", choices=("hard", "feather"), default="hard")
    render.add_argument("--allow-incomplete", action="store_true")
    return parser


def main() -> None:
    """Validate or render one capture session."""
    arguments = _parser().parse_args()
    try:
        session_path = arguments.session.resolve()
        session = load_session(session_path)
        validate_images(session, session_path.parent, arguments.allow_incomplete)
        if arguments.command == "render":
            render_session(
                session,
                session_path.parent,
                arguments.output,
                arguments.width,
                arguments.blend,
                arguments.allow_incomplete,
            )
            print(f"wrote {arguments.output}")
        else:
            print(f"valid session: {session.session_id}")
    except (OSError, ValueError) as error:
        raise SystemExit(f"error: {error}") from error
