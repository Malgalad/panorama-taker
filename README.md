# Cyberpunk 2077 Panorama Capture

Panorama Capture automates a complete 360×180° first-person capture in
Cyberpunk 2077, records the exact observed camera basis for every screenshot,
and stitches the completed session into an equirectangular panorama.

The verified production path is Cyber Engine Tweaks (CET) plus a ReShade
add-on. CET freezes and prepares the scene, drives camera poses, and records
metadata. The add-on requests ReShade screenshots and reports the exact saved
file path back to CET. The Windows stitcher GUI converts the resulting capture
JSON and screenshots into JPEG, lossless PNG, or HDR EXR.

## Requirements

- Cyberpunk 2077 for Windows x64; the tested game version is 2.31.
- A current, working CET installation.
- ReShade with add-on support; the tested runtime is ReShade 6.7.3/API 18.
- ReShade configured to save the desired screenshots. The verified HDR input
  is its 16-bit Rec.2020/PQ PNG mode.

The capture FoV override is optional. Install the first-party FOV Control
RED4ext plugin and redscript helper separately only if you want that override.
Without it, Panorama Capture uses the game's active FoV and does not show a
Capture FoV slider in Native Settings.

Do not run FreeFly during capture. Vehicles are intentionally rejected, and a
near-zero time dilation session prevents F9 quick-load from being processed
until the capture is restored or aborted.

## Install the game components

1. Close Cyberpunk 2077.
2. Extract `PanoramaCapture-Mod-<version>.zip` into the Cyberpunk 2077 game
   directory. It supplies:

   ```text
   bin\x64\PanoramaCaptureReShade.addon64
   bin\x64\plugins\cyber_engine_tweaks\mods\PanoramaCaptureProbe\init.lua
   ```

3. Start the game and load a save. Confirm CET loads Panorama Capture and
   ReShade logs that it loaded `PanoramaCaptureReShade.addon64`.
4. In CET's bindings UI, assign:

   - `Panorama: start full-sphere pose session`
   - `Panorama: abort full-sphere pose session`
   - `Panorama: report capture status` (development builds only)

The shipped configuration enables automatic ReShade screenshots. With Native
Settings installed, settling delay, toast cooldown, and an optional capture FoV
override are saved immediately in `PanoramaCaptureProbe/settings.json` and
survive CET reloads. Until the FoV slider is changed, a capture uses the active
in-game FoV. Keep `bridgeDirectory = "."` unless the ReShade add-on and CET
bridge are deliberately moved together.

## Capture workflow

1. Enter normal first-person gameplay, outside Photo Mode, a vehicle, a menu,
   or a scripted camera scene.
2. Frame the scene and press the start binding once.
3. Wait. For each pose, CET verifies camera pitch, waits the configured 1.5 s
   temporal-stability floor, asks ReShade to save a screenshot, and advances
   only after ReShade confirms the saved path.
4. CET restores camera pose, HUD, player/weapon meshes, input, and time
   dilation after the last screenshot. Abort uses the same restoration path.
5. Find `PanoramaCaptureBridge.pano-<session-id>.json` in the CET mod bridge
   directory and keep it with the corresponding screenshots.

If capture cancels, read the latest `[PanoramaCaptureProbe]` line in CET's log.
An `ERROR:timeout` indicates ReShade did not report a screenshot within ten
seconds; verify the add-on loaded and the ReShade screenshot path is writable.

## Stitch workflow

1. Extract `PanoramaCapture-Stitcher-<version>-win-x64.zip` anywhere outside
   the game directory.
2. Run `PanoramaCaptureStitcher.exe`; no Python or WSL installation is needed.
3. Select the capture JSON. The GUI will infer the screenshots directory from
   recorded absolute screenshot paths when possible.
4. Wait for background validation to enable Render. The default is a quality
   95 JPEG; use PNG for lossless SDR or EXR to preserve HDR data.

During render, form fields are locked. Cancel safely removes partial output.
Existing output files require confirmation before replacement.

## Recovery and known limitations

- If CET is reloaded or the session is aborted, it restores the capture state.
  Restart the game if a third-party mod remains in an unexpected state.
- Close the CET overlay before judging HUD visibility; the mod re-hides HUD
  widgets after overlay close during a session.
- FreeFly conflicts with the input restrictions used by capture.
- Vehicle and Photo Mode cameras are unsupported.
- A power loss or forced process kill can leave `pano-stitch-*` scratch folders
  beside the selected output directory. They can be removed after confirming no
  render is active.

## Release verification checklist

- [ ] ReShade loads the add-on without an API compatibility warning.
- [ ] CET loads the mod and all three bindings can be assigned.
- [ ] A full automated session saves one screenshot per metadata pose.
- [ ] The final JSON validates in the stitcher GUI.
- [ ] JPEG, PNG, and EXR exports complete; JPEG quality 95 is visually checked.
- [ ] Abort during settle and during screenshot wait restores the game state.
- [ ] The release ZIPs contain no source checkout paths, build caches, or
      developer-specific screenshots.

## Trusted release process

Shareable artifacts are built only by the repository's GitHub Actions release
workflow. Do not attach locally built EXEs or DLLs to a public release.

Before publishing, use **Actions → Release → Run workflow**, enter the version
from `stitcher/pyproject.toml` without its `v` prefix, then download the
temporary artifact from the completed run. It contains the same two ZIPs,
checksum manifest, and provenance that a tag release would create, but never
creates a GitHub Release. The artifact is retained for seven days.

Before tagging a release, a maintainer must run the checklist above against the
candidate commit and update the stitcher version in `stitcher/pyproject.toml`.
Then create and push a matching annotated tag:

```powershell
git tag -a v0.1.0 -m "Panorama Capture v0.1.0"
git push origin v0.1.0
```

The `Release` workflow runs deterministic stitcher checks, builds the ReShade
add-on on GitHub's Windows runner, packages the CET script and one-folder GUI,
validates archive contents, generates `SHA256SUMS.txt`, and attaches both ZIPs
to the matching GitHub Release. Users should download only those release
assets and may verify them against the published checksum file. Each release
also contains `BUILD-INFO.txt` with the tag, source commit, runner image, and
exact packaged Python environment.

`release/build-windows-release.ps1` is the implementation called by the
workflow. Local builds are expected for development and testing, but locally
built archives must never be attached to a public release.

## Local development workflow

Use local builds for rapid iteration; only GitHub Actions outputs are
shareable release artifacts.

Rebuild the ReShade add-on from an **x64 Native Tools Command Prompt**:

```bat
cmake -S reshade-addon -B C:\build\panorama-reshade-addon -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build C:\build\panorama-reshade-addon
copy /Y C:\build\panorama-reshade-addon\PanoramaCaptureReShade.addon64 ^
  "<Cyberpunk 2077>\bin\x64\PanoramaCaptureReShade.addon64"
```

Restart the game after replacing the `.addon64`; ReShade does not hot-reload
add-ons. CET's **Reload All Mods** is appropriate for Lua-only `init.lua`
edits, provided no capture is active.

Run the stitcher from its development virtual environment:

```bash
cd stitcher
../.venv/bin/pano-stitch validate <capture.json> --image-dir <screenshots-dir>
../.venv/bin/pano-stitch render <capture.json> --image-dir <screenshots-dir> --output <panorama.jpg>
../.venv/bin/pano-stitch-gui
```

`pano-stitch render` uses bounded parallel strip compositing by default. Use
`--workers N` to set a specific worker count, or leave the default `--workers 0`
for Auto; `--memory-budget-mib` bounds the combined active strip working set.

On native Windows, use the corresponding `.venv-win\Scripts\pano-stitch*.exe`
entry points from the native checkout. Run `ruff`, `mypy`, and `pytest` before
handing a change to the release pipeline.
