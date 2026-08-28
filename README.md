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

1. Extract one stitcher archive outside the game directory:
   - `PanoramaCapture-Stitcher-<version>-cpu-win-x64.zip` for any supported Windows x64 system;
   - `PanoramaCapture-Stitcher-<version>-cuda-win-x64.zip` only for NVIDIA GPUs with a supported driver.
2. Run `PanoramaCaptureStitcher.exe`.
3. Choose the Cyberpunk 2077 game directory.
4. Select a session from the list. Sessions are read from:

   ```text
   bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe
   ```

5. Choose an output directory and format, then click Render.

The stitcher does not alter pose exposure unless requested. If the game changed exposure during a
capture, render a preview, open **Correct exposure**, choose the desired baseline pose with **Target
exposure**, and click **Automatic correction**. The stitcher propagates exposure matching through
overlapping poses; disconnected poses produce a warning and remain available for manual correction.
For manual correction, enable **Show boundaries overlay**, select the affected poses, choose a
neighboring target, and click **Match exposure**. Corrections are non-destructive, can be discarded,
and last only for the current stitcher session.

For HDR captures, correcting overexposed or underexposed screenshots may leave their colors
desaturated. To minimize this, preserve as much color information as possible in the source
screenshots; for example, do not set **Blowout** to `0` in the RenoDX ReShade add-on. This does not
apply to SDR captures.

The session list shows the local capture date, whether capture completed, and whether it has been stitched before. The Screenshots field can be used when captures were moved from the game directory. The default memory budget is 1024 MiB.

The session actions can delete only the JSON, or the JSON and captured screenshots. For stitched sessions, deleting source files preserves the stitched panorama. JSON-only deletion of an incomplete session does not require confirmation; other deletion actions ask for confirmation.
