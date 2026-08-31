from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

NATIVE_ROOT = Path(__file__).parents[1] / "native"
SHADER_CONTRACTS = (
    (
        "candidate_uint8.hlsl",
        "generate_uint8_candidate",
        "c457545c1cee1742206ca3e4957e1ca68eaf58360083056e25c610d1e63a88df",
    ),
    (
        "exposure_pair_projection.hlsl",
        "project_exposure_pair",
        "9f17b6ccccdfc1c9d2a05dcc99cd5d26a86ad833f44642f9f48ec8901665d744",
    ),
    (
        "exposure_pair_classify_resident.hlsl",
        "classify_resident_exposure_pair",
        "1e7748cb8a2dbeaaa4760a8164a0c002384de243df9aec1b5683d19c04f276e8",
    ),
    (
        "hard_selection.hlsl",
        "hard_select",
        "2e17350b9bcf645ea981319a2b3b5bf741096fae9e01170ffb9837b49ca00779",
    ),
    (
        "feather_accumulate.hlsl",
        "feather_accumulate",
        "04d2c0cf09d09fddff038c3647dfc114662c7483d766cc8ff7ecca0d8b6c316d",
    ),
    (
        "render_preview_overlay.hlsl",
        "render_preview_overlay",
        "1a35f312dcc0b61901218ab039332946c3a8058796e9c9999bf73305cc332a32",
    ),
)


@pytest.mark.parametrize(("filename", "entry_point", "expected_hash"), SHADER_CONTRACTS)
def test_native_hlsl_entry_point_and_hash(
    filename: str, entry_point: str, expected_hash: str
) -> None:
    shader = NATIVE_ROOT / "shaders" / filename
    source = shader.read_bytes()
    cmake = (NATIVE_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    assert hashlib.sha256(source).hexdigest() == expected_hash
    assert f"void {entry_point}(" in source.decode("utf-8")
    assert f"/E {entry_point}" in cmake
    assert f"shaders/{filename}" in cmake
