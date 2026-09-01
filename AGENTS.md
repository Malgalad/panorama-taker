# Repository guidance

## Workflow

- Before editing, inspect relevant code and summarize the intended change. Ask if requirements
  conflict, logic seems wrong, or scope must expand materially.
- Before each file edit, briefly state what changes and why.
- Keep changes narrow; preserve unrelated user work and public behavior.
- Prefer existing types/helpers. Comment only non-obvious invariants. Add focused regression tests.
- Report only checks actually run.
- If `.local/windows-d3d12-runbook.md` exists, read it before invoking Windows build or test tools.
- Never launch/deploy to the game, delete captures, or overwrite user output without permission.

## Project map

Cyberpunk 2077 full-sphere panorama capture and stitching:

- `mod/cet/PanoramaCaptureProbe/init.lua`: production CET Lua controller.
- `reshade-addon/`: Windows x64 C++17 ReShade screenshot add-on (vendored API 18 headers).
- `stitcher/native/`: C++17 D3D12/CPU compositor with a WebView2 GUI.
- `contracts/`: shared session schema and example.
- `docs/`: focused designs and verified research.
- `mod/src/plugin.cpp`: C++20 RED4ext fallback/probe; do not expand without approval.

Current code and tests are authoritative. Read the relevant README and focused doc before changes.
`PLAN.md` is historical/proposed context and may be stale.

## Critical invariants

- Do not change schema, projection conventions, color pipeline, names, defaults, or session history
  unless requested.
- Stitching uses recorded geometry and preserves HDR/Rec.2020/PQ/EXR behavior.
- Rendering is memory-bounded: use strips, memmaps, streaming writers, cancellation, staged files,
  and failure cleanup. Avoid nested OpenCV/worker thread pools.
- Keep GUI image work off the window thread; update WebView state through the event queue. Keep GUI
  and command-line render options aligned.
- CET state restores exactly on success, abort, shutdown, and failure. Derive poses from the saved
  origin; accept capture only after matching ReShade completion.
- When changing the CET mod, increment its declared version unless the current change already did.
- ReShade runtime calls stay in documented callbacks. Match by runtime and token; reject
  concurrency and ignore unrelated screenshots. Preserve vendored headers.
- Schema changes require an explicit compatibility/version decision and synchronized examples and
  tests.

## Verification

From the repository root:

```text
cmake -S stitcher/native -B build/stitcher-native -DCMAKE_BUILD_TYPE=Release
cmake --build build/stitcher-native --config Release
ctest --test-dir build/stitcher-native -C Release --output-on-failure
```

The GUI, D3D12 backend, and native add-on builds require Windows x64 MSVC; Linux contract tests do
not prove a Windows native build. Lua/game behavior requires manual runtime validation. Before
handoff, review scope, formatting, and generated artifacts; summarize changed files, checks, and
remaining manual validation.
