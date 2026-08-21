# PanoramaCapture CET probe

This is a save-gated CET FPP capture probe. Packet 4 provides a bounded environment transaction; Packet 5 adds a manual full-sphere pose session on top of the same capture safeguards.

The script clears its active sequence on CET shutdown/reload. A full game restart is still recommended after changing CET itself.

## Install

Copy the `PanoramaCaptureProbe` directory into the CET mods directory so the final path is:

```text
Cyberpunk 2077/bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe/init.lua
```

Remove or rebuild the native `PanoramaCaptureProbe.dll` at stage 2 before testing this CET probe; do not leave the crashing stage-3 DLL deployed.

## Verify

Bind **Panorama: probe FPP capture environment** for Packet 4. From normal FPP view, the first press snapshots the FPP pose, HUD child opacities, and enabled states for player and active-weapon meshes; applies a project-specific near-zero time dilation; hides the HUD, player renderer, and active weapon; and applies a representative `+45°` yaw/`+15°` pitch. It reports smoothed real FPS, frame-generation settings, and waits eight real CET update frames after the final pose write. Press the same binding a second time to restore the pose, mesh states, HUD opacities, and time dilation. Do not run it while Photo Mode or another time-freeze mod is active. If a hide/restore API is unavailable, the probe cancels and unwinds its partial changes.

If the player detaches or becomes unavailable during a probe, the mod attempts to abort, restore the old environment, and clear the transaction. Capture refuses to start while the game is paused or the player is mounted in a vehicle. Pause detection combines the system request state with `gameuiPopupsManager.OnMenuUpdate`, the same callback used by the installed GameSession implementation, because vendor menus may not be reflected by the system query at hotkey time. Active capture applies `GameplayRestriction.NoMovement` and `GameplayRestriction.NoCameraControl` so accidental player/camera input cannot invalidate a pose; these are removed on restore. Do not run FreeFly concurrently, and note that another mod using these same global restriction effects can have its restriction removed when PanoramaCapture restores. If CET UI rebuilds HUD widgets during an active capture, the direct `onOverlayClose` event schedules five re-hide updates while retaining the original values for final restoration. During CET shutdown/reload, the mod synchronously restores the saved player position/yaw and exact camera orientation before restoring HUD, meshes, and time dilation. Vehicle cameras are unsupported because they rotate the vehicle rather than providing an independent FPP view. F9 quick-load is a known exception: while time dilation is active, the game does not process F9/session teardown until time is unfrozen, so cleanup cannot run during that interval. Use the mod hotkey to restore first; emergency input interception is deferred.

## Packet 5 manual pose session

Bind **Panorama: start full-sphere pose session** to begin the production geometry slice. It reads the engine's vertical FoV and aspect ratio, derives horizontal FoV, and plans overlapping yaw columns across multiple pitch rows. The outer rows extend half the configured overlap past each pole so zenith and nadir do not fall on a zero-weight image edge. Before pose 1, it performs a two-update zero-pitch calibration to learn the persistent FPP pitch offset. Before declaring any pose ready, it measures pitch from the active camera direction and corrects the FPP camera until it is within 0.25 degrees of the target (up to three attempts); internal correction checks use only two real updates, while the full 1.5-second settling gate runs after pitch is verified and before screenshot readiness. At each `Production pose ready` log, take the screenshot manually, then press **Panorama: advance full-sphere pose**. Press **Panorama: abort full-sphere pose session** to restore the original camera and capture environment. The final pose automatically queues restoration after the next advance.

At each settled pose the mod also emits one machine-readable `POSE_METADATA` log record containing the commanded row/yaw/pitch, observed forward/right/up basis, observed horizontal/vertical FoV, real settle timing, smoothed real FPS, and a basis-validity flag. It still does not bind a screenshot filename: manual ReShade files must be associated by the capture-side workflow before rendering.

## Packet 7 manifest association

After a manual session completes, use `stitcher/scripts/extract_cet_pose_manifest.py` with the CET log, screenshot directory, first and last screenshot names, session ID, mod version, and UTC timestamp. It selects non-empty PNG/JPEG/EXR files in deterministic filename order and requires an exact count match with the latest complete indexed `POSE_METADATA` session. The manifest is written through a flushed, fsynced `.partial` file and atomic replacement. A count mismatch, missing directory, unsupported-only range, or zero-byte screenshot aborts without producing a manifest.

## Packet 6 settings and status

The user-editable `captureConfig` table is at the top of `init.lua`; reload CET after changing it. Supported settings are `overlap` in `[0, 0.5)`, `adaptiveYawGuard` in `[0, 0.25)`, a positive integer `settleFrames`, `settleSeconds` in `[0, 60]`, a positive `pitchToleranceDegrees` no greater than 10, and non-negative integer correction counts. Invalid values prevent a probe or production session from starting and log the exact field error.

Bind **Panorama: report capture status** to print the current state. While idle it reports active FoV, real FPS, normalized frame-generation state, overlap, and settling settings. During a session it additionally reports pose progress and the remaining settling gate with an FPS-based readiness estimate. CET input bindings remain configurable through CET's binding UI.
