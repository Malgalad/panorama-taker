# Panorama Capture mod

## Install

1. Close Cyberpunk 2077.
2. Extract the release archive into the game directory.
3. Confirm these files exist:

   ```text
   bin\x64\PanoramaCaptureReShade.addon64
   bin\x64\plugins\cyber_engine_tweaks\mods\PanoramaCaptureProbe\init.lua
   ```

Codeware is required for standalone camera spawning. Appearance Menu Mod and FOV
Control are not required.

4. Start the game and load a save.
5. In CET's bindings UI, assign:
   - `Panorama: start full-sphere pose session`
   - `Panorama: abort full-sphere pose session`
   - `Panorama: report capture status` (development builds only)

## Capture

Use normal gameplay outside Photo Mode, vehicles, menus, scripted camera scenes, and active AMM cameras. FreeFly may remain active because PanoramaCapture spawns and controls its own fixed camera without rotating the player. Frame the scene and press the start binding. The mod captures the planned views automatically, requests a ReShade screenshot for each view, and restores the camera and game state when finished.

If capture stops, inspect the latest `[PanoramaCaptureProbe]` message in the CET log. Keep the generated `PanoramaCaptureBridge.pano-<session-id>.json` and its screenshots together.

## Settings

The optional CET Native Settings panel controls settling delay, screenshot cooldown, and capture FoV. Settings are saved in `PanoramaCaptureProbe/settings.json`.

Keep `bridgeDirectory` set to `.` unless the CET mod and ReShade add-on bridge are moved together.
