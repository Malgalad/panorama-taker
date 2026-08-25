# PanoramaCaptureProbe

## Install

Copy this directory to:

```text
Cyberpunk 2077/bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe
```

Reload CET after replacing `init.lua`. Do not reload while a capture is active.

## Use

Bind these CET actions:

- `Panorama: start full-sphere pose session`
- `Panorama: abort full-sphere pose session`
- `Panorama: report capture status` (development builds only)

Start captures in normal first-person gameplay. Do not use Photo Mode, vehicles, scripted cameras, or FreeFly during capture. The mod hides the HUD and player meshes, drives each camera pose, requests screenshots through the ReShade add-on, and restores the original state after completion or abort.

Completed sessions write `PanoramaCaptureBridge.pano-<session-id>.json` beside the mod. Keep that JSON file with all screenshots from the session.

## Stitch

Run the Windows stitcher and choose the Cyberpunk 2077 game directory. Select a session from the list, then choose an output directory and render format. The stitcher finds sessions under:

```text
bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe
```
