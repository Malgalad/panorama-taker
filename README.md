# Cyberpunk 2077 Panorama Capture

## Requirements

- Cyberpunk 2077 for Windows x64.
- Cyber Engine Tweaks (CET).
- Codeware (for the standalone capture camera).
- ReShade with add-on support.
- The Panorama Capture mod release archive.

## Install

1. Close Cyberpunk 2077.
2. Extract `PanoramaCapture-Mod-<version>.zip` into the game directory.
3. Start the game and load a save.
4. In CET's bindings UI, assign:
   - `Panorama: start full-sphere pose session`
   - `Panorama: abort full-sphere pose session`

## Capture

Use normal gameplay outside Photo Mode, vehicles, menus, scripted camera scenes, and active AMM cameras. FreeFly may remain active because PanoramaCapture spawns and controls its own fixed camera without rotating the player. Frame the scene and press the start binding. The mod captures every planned view through ReShade and restores the camera and game state when finished.

Keep each generated `PanoramaCaptureBridge.pano-<session-id>.json` with its screenshots. If capture stops, inspect the latest `[PanoramaCaptureProbe]` message in the CET log.

## Stitcher

1. Extract `PanoramaCapture-Stitcher-<version>-win-x64.zip` outside the game directory.
2. Run `PanoramaCaptureStitcher.exe`.
3. Choose the Cyberpunk 2077 game directory.
4. Select a session from the list. Sessions are read from:

   ```text
   bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe
   ```

5. Choose an output directory and format, then click Render.

For a D3D12 smoke test, leave **Use GPU acceleration** enabled, select a
completed session, and render a preview. The status must identify the D3D12 adapter rather than a
CPU fallback.

The native stitcher requires the Microsoft Edge WebView2 Runtime. If it is unavailable, startup
shows the native prerequisite prompt and exits instead of exposing another application interface.
To verify the memory-bounded CPU backend on a D3D12-capable system, launch it from a console with
`PanoramaCaptureStitcher.exe --no-gpu`.

For D3D12 failure diagnostics, launch `PanoramaCaptureStitcher.exe --d3d12-debug`. This enables the
D3D12 debug layer, GPU-based validation, synchronized queue validation, and DRED breadcrumbs/page
fault reporting. A timestamped log is written under
`%LOCALAPPDATA%\PanoramaCapture\logs`. The log records adapter and memory-plan details, preview
stages, source uploads, command submissions, fence waits, HRESULT/device-removal details, and CPU
fallback. This mode intentionally adds validation overhead and should not be used for performance
measurement.

The editable WebView sources are `stitcher/native/resources/pano_app_ui.html`,
`pano_app_ui.css`, and `pano_app_ui.js`. The native build inlines all three into one generated
HTML resource; those source files are not shipped beside the executable.

Release diagnostics can be written without opening the GUI:

```text
PanoramaCaptureStitcher.exe --verify-gpu-runtime gpu-runtime.txt
```

Exit `0` verifies ABI loading, hardware adapter admission, embedded-pipeline creation, dispatch,
readback, and cleanup. Exit `2` means no compatible hardware adapter is available; exit `3` means
the native runtime, ABI, pipeline, dispatch, or readback failed. Details are written to the supplied
file because the release executable uses the Windows GUI subsystem.

The stitcher does not alter pose exposure unless requested. If the game changed exposure during a
capture, render a preview, open **Correct exposure**, choose the desired baseline pose with **Target
exposure**, and click **Automatic correction**. The stitcher propagates exposure matching through
overlapping poses, preserving current relative exposure across unmeasurable geometric overlaps;
geometrically disconnected poses remain available for manual correction.
For manual correction, enable **Show boundaries overlay**, select the affected poses, choose a
neighboring target, and click **Match exposure**. Corrections are non-destructive, can be discarded,
and last only for the current stitcher session.

The exposure panel's **Exposure** slider applies a final `-2.0` to `+2.0 EV` adjustment after
per-pose correction and before SDR conversion and Auto contrast. It updates the retained preview
and JPEG/PNG output; EXR remains unchanged. The CLI equivalent is `--final-exposure EV`.

For HDR captures, correcting overexposed or underexposed screenshots may leave their colors
desaturated. To minimize this, preserve as much color information as possible in the source
screenshots; for example, do not set **Blowout** to `0` in the RenoDX ReShade add-on. This does not
apply to SDR captures.

The session list shows the local capture date, whether capture completed, and whether it has been stitched before. The Screenshots field can be used when captures were moved from the game directory. The default memory budget is 1024 MiB.

The session actions can copy saved `x, y, z` coordinates, delete only the JSON, or delete the JSON
and captured screenshots. Coordinate copying is available for sessions that contain location data.
For stitched sessions, deleting source files preserves the stitched panorama. JSON-only deletion of
an incomplete session does not require confirmation; other deletion actions ask for confirmation.
