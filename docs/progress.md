# Current implementation state

Last verified: 2026-08-20.

## Working now

- The CET Lua probe loads successfully and can be reloaded without restarting the game.
- Reload verification is confirmed by the log marker `reload verification marker v0.1.5`.
- The pose hotkey is a two-click latch: first click saves the player/camera pose and applies the test yaw/pitch; second click restores the saved pose.
- Yaw and pitch changes are visible in the camera direction logs, and restoration has been verified in exactly two clicks.
- Packet 3H's initial HDR path is working: the supplied ReShade PNG is 16-bit Rec.2020/PQ/full-range, OpenCV reads it without truncation, the stitcher decodes it to linear `float32`, and OpenEXR output preserves the HDR working values.
- The stitcher streams HDR/PQ input directly to an explicitly tone-mapped lossless 8-bit SDR PNG preview. It creates no EXR intermediate; exact display-primary conversion and tone-map tuning remain release work.
- The deployed CET file is kept at `bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe/init.lua` in the game installation. Deploy by copying `mod/cet/PanoramaCaptureProbe/init.lua`.
- Packet 4 includes reloadable Photo Mode diagnostics, `gameuiPhotoModeMenuController` open/close logs, active-camera pose inspection, and guarded mutation probes.
- Packet 4 diagnostic result: `Game.GetCameraSystem()` follows the Photo Mode drone; forward/right vectors changed between manual drone movements. The Photo Mode callbacks are slightly ahead/behind the blackboard flag, so production capture must wait for a settled active-state observation.
- `gameCameraSystem:GetActiveCameraWorldTransform()` returns the actual Photo Mode drone position and quaternion. `GetActiveCameraData()` returns the same quaternion; the system also exposes active forward/right/up vectors, FoV, and aspect ratio.
- `Game.GetPhotoModeSystem()` succeeds. `gamePhotoModeSystem:GetCameraLocation()` returns the drone position as `WorldPosition` fixed-point coordinates; repeated samples track manual drone movement.
- The player `gameFPPCameraComponent` is not the Photo Mode drone. Guarded `SetLocalTransform()`/FoV writes changed the FPP component but did not move the active Photo Mode view. The two-click probe restored the FPP state safely.
- Neither `gameCameraSystem` nor `gamePhotoModeSystem` exposes a camera transform setter through CET RTTI. CET can read the complete active drone pose and optics, but the tested CET interfaces cannot command an absolute Photo Mode drone pose.
- The CET probe now has a read-only `Panorama: dump camera objects` binding using `GameDump()` to identify writable camera/controller methods before implementing mutation.

## Important lessons

- Do not query the camera system from the native RED4ext plugin during startup. The attempted RTTI `ExecuteFunction(GetCameraSystem)` path crashed before the main menu. CET's `Game.GetCameraSystem()` is safe after a save is loaded and is the current integration path.
- CET's Lua sandbox does not guarantee `_G`; using `rawget(_G, ...)` caused a load failure. Reload cleanup currently uses `onShutdown` instead.
- Avoid applying teleports or camera writes every frame. The current state machine performs one write per transition and only waits for deterministic frame settling.
- Preserve and restore the exact FPP camera local orientation, rather than reconstructing pitch from Euler angles; this avoids cumulative pitch drift.
- Do not treat `gameFPPCameraComponent` as the detached Photo Mode camera. Its apparent world transform can resemble the rendered camera, but writing it does not affect the Photo Mode drone.
- Packet 4's CET-only absolute Photo Mode control criterion did not pass. Keep CET for active-pose/FoV metadata and lifecycle observation unless a new writable camera API is found.

## Next implementation work

1. Automate or robustly detect the normal ReShade HDR screenshot and bind its stable filename to each ready pose, eliminating manual log/image association.
2. Add validated capture configuration and a Status operation to the CET frontend.
3. Tune and document Rec.2020/PQ-to-SDR PNG color conversion; keep EXR as the archival HDR path.
4. Add adversarial unsupported-state and restoration tests, while retaining the documented frozen-time F9 limitation.
5. Package the CET mod, stitcher, manifest extractor, installation instructions, and cleanup guidance.

Packet 4 live milestone (2026-08-20): the combined CET probe passed in-game with DLSS frame generation enabled. The log confirmed real FPS measurement, frame-generation setting detection, eight real CET update frames after the pose write, time dilation, camera movement/restoration, HUD restoration, and player mesh restoration. Remaining Packet 4 work is adversarial coverage and failure-path testing, not basic environment feasibility.

Packet 5 adaptive capture (2026-08-20): an initial full-sphere session produced 30 screenshots using row-adaptive azimuth counts of 3/5/7/7/5/3 from nadir to zenith. Every pose settled for at least eight real update frames and approximately 1.5 seconds, which matches the observed global-illumination stabilization floor. The camera, HUD, player mesh, time dilation, and frame-generation state were restored successfully at session end. Metadata-only stitching validation later found 44 small diagonal coverage gaps at the 7↔5 row transitions; the planner now applies a 5% adaptive-yaw guard, producing the covered 3/6/7/7/6/3 schedule (32 poses) at this FoV. The remaining production gap is writing a manifest that binds each screenshot filename to its pose and render-timing metadata.

Packet 5 observed-pose slice (2026-08-20): the CET production session now emits one `POSE_METADATA` record after each pose satisfies both real-frame and GI settling gates. Records include commanded row/yaw/pitch, observed active-camera forward/right/up basis, observed horizontal/vertical FoV, settle frames/seconds, smoothed real FPS, and an orthonormal-basis validity flag. Screenshot filename association remains deliberately external for manual ReShade captures; the next integration step must bind these records to stable image files before the stitcher consumes them.

Log cleanup (2026-08-20): removed redundant per-pose direction dumps, unavailable FPP world-property dumps, and repeated raw frame-generation setting lines from the live CET path. `POSE_METADATA`, readiness, failure, restoration, and compact environment timing/frame-generation status remain. These diagnostics had already been validated and were inflating pasted logs without adding capture data.

Observed-basis stitcher adapter (2026-08-20): the shared session schema now permits per-frame observed basis vectors and a canonical camera-basis matrix. The compositor prefers this matrix over commanded yaw/pitch, preserving the engine readback (including pitch offset and roll) during projection. `extract_cet_pose_manifest.py` converts indexed CET records plus chronological screenshots into a schema-valid session manifest.

Coverage finding (2026-08-20, corrected): the initial observed-basis adapter transposed the camera basis incorrectly and interpreted the engine's 59.23° vertical FoV as horizontal. Using basis rows as camera-to-world vectors requires a transpose before the world-to-camera projection; at 16:9 the correct FoV is approximately 90.60° horizontal by 59.23° vertical. A quarter-resolution corrected render aligns the scene coherently and leaves only a thin magenta band at the positive pole. The remaining polar coverage needs capture-side pitch calibration, but the large gaps and duplicated scene geometry were stitcher conversion bugs rather than radians/degrees errors.

Capture calibration v0.1.5 (2026-08-20): the CET planner now treats `GetActiveCameraFOV()` as vertical FoV and derives horizontal FoV from the active aspect ratio. Production poses use closed-loop pitch control: after settling, the mod derives observed elevation from `asin(forward.z)`, learns the persistent FPP pitch offset, and reuses it across poses. A pose is exposed for capture only within 0.25° of its target; three failed corrections cancel and restore the session. Polar rows extend half the configured overlap beyond the nominal edge-aligned pitch so pole pixels receive non-zero feather weight. This replaces the old 32-pose schedule with a FoV-correct schedule (expected 16 poses at 3840×2160 and the measured 59.23° vertical FoV).

Capture calibration acceptance (2026-08-20): v0.1.5 captured 16 HDR screenshots with the 3/5/5/3 schedule at 90.60° horizontal by 59.23° vertical FoV. Every reported pitch matched its target within approximately 0.00002°, every observed basis passed validation, and environment restoration completed. Strict compositor validation found complete coverage without `--allow-incomplete`; the 2984×1492 quarter-dimension feathered PNG has coherent geometry and no magenta zenith/nadir pixels. The accepted manifest and preview are `cp2077-2026-08-20-202151.json` and `cp2077-2026-08-20-202151-quarter.png` beside the source screenshots.

Capture guards v0.1.7 (2026-08-21): vendor UI exposed a pause path not caught reliably by `IsGamePaused()`. The mod now also observes `gameuiPopupsManager.OnMenuUpdate`, matching the installed GameSession implementation, and latches Photo Mode show/hide events. Both environment probes and production sessions refuse to start while that UI pause state is active. Vehicle detection remains based on `UI_ActiveVehicleData.IsPlayerMounted`; ladders and NPC conversations remain safe, vehicles remain unsupported, and scripted scenes retain bounded three-attempt pitch correction and graceful restoration.

Settling split v0.1.8 (2026-08-21): pitch-correction retries no longer consume the 1.5-second global-illumination wait. After a correction write, the mod checks pitch after two real updates; once pitch is within tolerance, it starts the normal eight-real-frame/1.5-second settling gate and only then emits screenshot readiness. This keeps internal control responsive without shortening the stabilization interval before manual screenshots.

HUD lifecycle guard v0.1.9 (2026-08-21): opening and closing the CET UI can rebuild or reset HUD widgets while a capture is active. The mod now schedules three consecutive post-lifecycle re-hide updates, while preserving the original widget opacities for restoration. This closes the observed gap where the HUD stayed visible until the next pose transition.

CET lifecycle fixes v0.1.10 (2026-08-21): game UI observers did not reliably coincide with closing the CET overlay, so HUD enforcement now also uses CET's direct `onOverlayClose` event and reapplies hiding for five updates. Each pass reacquires the current `inkHUDLayer` virtual window instead of assuming the original widget tree survived the overlay. Reloading all mods exposed a separate shutdown bug: environment controls were restored but the multi-update pose restoration could not run after Lua teardown. `onShutdown` now synchronously restores saved player position/yaw and exact FPP camera orientation before clearing environment controls and session state.

Held-weapon suppression v0.1.11 (2026-08-21): the active weapon is a separate entity, so scanning only `PlayerPuppet:GetComponents()` can leave it rendered after the body is hidden. The capture mesh transaction now includes AMM's proven `EquipmentSystem:GetActiveWeaponObject(player, 40)` lookup, namespaces saved component keys by owner entity, re-scans player and active-weapon meshes at pose transitions, and restores every discovered component to its original enabled state. Exotic scripted hand props remain unverified until a reproducible prop exposes its owning entity/API.

Input lock and pitch calibration v0.1.12 (2026-08-21): capture now applies AMM's `GameplayRestriction.NoMovement` and `GameplayRestriction.NoCameraControl` effects while active, then removes them during normal restore, abort, and CET shutdown. This prevents accidental player or camera input from disturbing a ready pose. The effects are not ownership-aware: another mod using the same restriction can have it cleared on PanoramaCapture restore; FreeFly was observed to conflict and must remain inactive. The production session also uses a two-update zero-pitch calibration before pose 1 to learn the persistent FPP offset before the first full 1.5-second temporal settling interval. Bounded post-write pitch correction remains as a safety check, but should no longer be the normal first-pose path.

Packet 5 acceptance (2026-08-21): in-game adversarial checks now cover ladders, NPC conversations, menus, vendor UI, scripted scenes, vehicle rejection, CET overlay close, CET reload, HUD restoration, player/weapon hiding, input lock, and camera/pitch restoration. Packet 5 is accepted with documented limitations: F9 cannot be processed while near-zero dilation is active, FreeFly conflicts, the input-restriction effects are not ownership-aware, vehicle cameras are unsupported, and unobserved scripted hand props remain outside the verified hide set.

Packet 6 frontend start v0.1.13 (2026-08-21): settings are now a validated user-editable `captureConfig` table in the CET source. Invalid values block both probe and production start with a specific error. The new `Panorama: report capture status` binding reports idle/active state, pose progress, active FoV, real FPS, normalized frame-generation state, settling configuration, and a live estimate of readiness while settling.

Packet 6 frontend acceptance (2026-08-21): v0.1.13 loaded and the Status binding was verified idle, during pose 1, during pose 2, and after abort. The initial zero-pitch calibration observed a persistent -8.537° offset and applied +8.537° before the first target write; both tested poses reached their commanded pitch without a correction retry. Status accurately reported that the eight-frame gate had completed while the independent 1.5-second temporal-stability floor still had 0.60–0.71 seconds remaining. Abort restored the environment and returned Status to idle.

Packet 7 association slice (2026-08-21): `extract_cet_pose_manifest.py` now exposes a reusable document builder, filters supported non-empty screenshot files in deterministic filename order, requires an exact image/metadata count match, and atomically writes manifests through a flushed/fsynced `.partial` file. Tests cover successful binding, unsupported-file filtering, count-mismatch refusal, and cleanup of the partial path. Automatic screenshot triggering and per-pose filename detection remain subsequent Packet 7 work.

Packet 7 ReShade pivot (2026-08-21): official ReShade API research confirms that a small add-on can call `effect_runtime::save_screenshot(postfix)` and receive `addon_event::reshade_screenshot(runtime, path)` after the file is saved. The exact callback path replaces overlay-notification scraping and primary-path directory polling; a correlation postfix prevents unrelated screenshots from being bound to a pose. Runtime calls will be confined to `reshade_present`, with IPC threads limited to bounded plain-data queues. Packet 7 now gates on proving that add-on-triggered capture preserves the verified 3840×2160 16-bit Rec.2020/PQ output. The existing stable-file watcher and Print Screen simulation remain compatibility fallbacks rather than the production design. Detailed findings are in `docs/reshade-addon-research.md`.

Packet 7 add-on scaffold (2026-08-21): `reshade-addon/` now contains a CMake target producing a 64-bit `.addon64`, API-18 headers byte-matched to the official ReShade v6.7.3 source, runtime lifecycle tracking, an F10 test trigger, correlated `save_screenshot()` requests, and machine-readable success lines from `reshade_screenshot`. The add-on does not touch the screenshot directory or simulate Print Screen. WSL lacks a Windows compiler/Ninja, so compilation and in-game HDR verification remain to be performed in the existing Visual Studio x64 environment.

Packet 7 add-on acceptance (2026-08-21): after rebuilding and restarting the game, F10 produced one correlated screenshot request and one completion event with the exact path. The resulting `pano-test-000002.png` is 3840×2160, 16-bit RGB, decodes to `uint16`, and contains HDR-range samples up to approximately 50,000. This proves `save_screenshot()` preserves the required ReShade HDR path. Only CET↔add-on request/acknowledgement automation remains for Packet 8.

Current resource and cleanup audit (2026-08-20): rerendering that 16-image preview peaked at 432,068 KiB RSS (421.9 MiB), used no swap, allocated 71,234,048 bytes (67.9 MiB) of disk scratch, and wrote a 7,108,960-byte (6.78 MiB) PNG in 37.53 seconds. The compositor writes PNG directly, atomically renames a same-directory `.partial` file, and scopes disk-backed accumulators to a `TemporaryDirectory`; no scratch directory, partial output, or EXR conversion artifact remained. Native 11,938×5,969 output is estimated to require 1,140,126,752 bytes (1.06 GiB) of scratch. Successful requested outputs are intentionally retained. The current `/tmp` inventory also contains about 2.1 GiB of older manually requested EXR/PNG benchmark and diagnostic products; these are not leaked compositor intermediates and should only be removed with explicit approval.

Cleanup caveat (2026-08-20): normal success and handled `Exception` failures remove scratch and partial output. `TemporaryDirectory` also unwinds during ordinary Python termination, but the current atomic-output guard does not catch `KeyboardInterrupt`, and no in-process design can clean after `SIGKILL` or power loss. Add verified Ctrl+C cleanup plus narrowly scoped stale `pano-stitch-*`/`.partial` recovery before packaging.

HDR PNG inspection (2026-08-20): HDR/PQ sessions may now render directly to lossless 8-bit SDR PNG. The writer applies a streaming PQ-to-reference-white tone map and zlib level 9; no EXR intermediate is created. PNG has no lossy quality percentage, so “95% quality” is represented by lossless maximum compression. EXR remains the archival HDR output.

Stitcher memory requirement (2026-08-20): a 30-frame 4K HDR session occupies roughly 800 MB even while compressed. The production compositor must therefore use bounded tiles, one-source-at-a-time decoding, disk-backed accumulators, and incremental output. Peak process RSS, including decoder and encoder overhead, must remain below 1,000,000,000 bytes; the implementation should target at most 768 MiB internally for safety margin. The earlier Packet 3H compositor eagerly decoded every source and allocated panorama-sized working arrays, so it must not be used for the complete production session.

Packet 3M implementation (2026-08-20): the compositor validates headers sequentially, decodes one source at a time, composites bounded full-width strips into disk-backed `float32` color/weight scratch files, and streams PNG or scanline EXR output through an atomic temporary path. The CLI caps its production working-memory budget at 768 MiB. Unit tests cover bounded 4K strip selection, streamed full-sphere PNG/EXR rendering, HDR EXR round-trip, and lint/type checks. Real-image measurements below validate the sub-1,000,000,000-byte acceptance criterion.

Packet 3M real-image measurement (2026-08-20): the first production screenshot was confirmed as 3840×2160 16-bit Rec.2020/PQ. At the default 21,223×10,612 output geometry, the compositor selects 89-row strips and requires approximately 3.35 GiB of scratch storage. A real HDR decode plus worst-case full-width feather strip peaked at 311,648 KiB RSS (about 304 MiB) with no swap. A sequential pass over all 30 real images at reduced output width peaked at 273,436 KiB RSS (about 267 MiB), also with no swap, but correctly rejected output because the 30-pose geometry left 44 uncovered pixels at the 7↔5 row transitions. The capture schedule needs six images at each ±43.361° row (32 total poses) before it can satisfy the no-uncovered-pixels requirement.

Packet 3M corrected real-image acceptance (2026-08-20): the replacement 32-image interval `18-13-44_597` through `18-15-32_597` rendered successfully at reduced 512×256 output using the 3/6/7/7/6/3 pose schedule. The streamed EXR completed with no uncovered pixels, no swap, and 272,348 KiB peak RSS (about 266 MiB). The proof file was `/tmp/pano-3m-real-32.exr`; a full 21k-wide render remains a performance exercise because the current bounded implementation prioritizes the hard memory ceiling over throughput.

Packet 3M native-resolution acceptance (2026-08-20): the same 32 real HDR images rendered at the default 21,223×10,612 resolution in 3:25.60, below the requested 10-minute limit. The scanline EXR is 2.4 GiB at `/tmp/pano-3m-real-32-full.exr`; peak RSS was 471,116 KiB (about 460 MiB), with no swap and no leftover compositor scratch directory. This validates the hard memory requirement and native-resolution throughput for the current implementation.

The compositor retains a 768 MiB production default. An explicit 1,536 MiB budget is now permitted for controlled performance experiments such as quarter-linear-resolution rendering; this does not change the production memory target.

Quarter-linear benchmark (2026-08-20): the 32-image session rendered at 5,306×2,653 with the explicit 1,536 MiB budget in 1:24.98. The scanline EXR is 153 MiB at `/tmp/pano-3m-real-32-quarter-linear.exr`; peak RSS was 941,572 KiB (about 919 MiB), still below the 1,000,000,000-byte ceiling, with no swap or leftover scratch directory.

EXR compression (2026-08-20): the streaming writer now explicitly uses lossless PIZ compression through the legacy OpenEXR/Imath scanline API. Existing benchmark files were produced before this change with ZIP compression; future EXRs will carry a verified `Compression.PIZ_COMPRESSION` header. Lossy DWAA/DWAB compression remains intentionally unsupported.

PIZ quarter-resolution rerender (2026-08-20): the 5,306×2,653 32-image render completed in 1:22.52 with 1,536 MiB budget and 941,420 KiB peak RSS. The verified PIZ EXR is 133 MiB at `/tmp/pano-3m-real-32-quarter-linear-piz.exr`, approximately 15% smaller than the prior 156 MiB ZIP result.

Quick-load edge case: F9 does not take effect while the capture time dilation is active; the game does not process the input/session teardown until time is unfrozen. The probe retains detach/entity-hash cleanup safeguards, but they cannot run before the engine resumes. Record this as a known limitation and defer native input interception or a separate emergency-unfreeze binding unless unattended frozen-session abort becomes a release requirement.

The source-level IGCS result is recorded in `docs/igcs-evaluation.md`: direct use of the published in-process exports is viable for the horizontal MVP; stock connector capture is 8-bit and lacks metadata; the published ABI has no pitch or absolute-pose command for full-sphere automation.

Packet 8 implementation started (2026-08-21): the ReShade add-on now has a
worker-thread control mailbox. It consumes an atomic tab-separated request,
triggers `save_screenshot(token)` from `reshade_present`, and publishes an
atomic acknowledgement containing session, pose, token, and exact path after
`reshade_screenshot`. The CET mod has opt-in `automatedScreenshots` mode and
polls only this tiny acknowledgement file; manual mode remains unchanged.
The game-side acceptance test (one automated pose followed by a full run) is
still pending.

Maintenance rule: preserve existing CET `registerInput` action names and
ordering across reloads. Do not alter bindings for ordinary implementation or
version changes; only change them when an input is deliberately added,
removed, or renamed, because CET may require users to rebind changed actions.

Packet 8 acceptance (2026-08-21): v0.1.16 automated capture completed a full
16-pose run at 90.6° HFoV. Every pose emitted one ordered ReShade screenshot
acknowledgement with an exact PNG path, including both polar rows; the final
acknowledgement queued restoration and the environment completed cleanly.
This validates the CET↔ReShade atomic mailbox bridge without screenshot-folder
watching or Print Screen simulation.

Settling simplification (v0.1.17, 2026-08-21): removed FPS/frame-generation
sampling and the redundant real-frame gate. Every corrected pose now waits
only for the configured 1.5-second wall-clock accumulation interval before a
screenshot request. The manifest extractor accepts the new compact metadata
while remaining compatible with older FPS/frame-count records.

Metadata persistence (v0.1.18, 2026-08-21): CET now creates an atomic
`pano-<session>.json` beside the bridge mailbox, updates it after every
acknowledged screenshot, and records pose indices, commanded/observed pitch,
camera basis, FoV, settle duration, exact screenshot path, and session state.
Aborted and failed sessions are marked accordingly.

ReShade shutdown hardening (2026-08-21): a normal game close produced the
MSVC `abort()` dialog, consistent with destruction of a joinable add-on worker
thread. The add-on now exports ReShade's documented two-handle `AddonUninit`,
joins its worker during ordinary add-on unload, and detaches only during
process-termination DLL detach so static thread destruction cannot call
`std::terminate`. Requires rebuild and an exit test.

Packet 9 metadata integration (2026-08-21): the stitcher now accepts CET's
atomic `PanoramaCaptureBridge.pano-*.json` directly, converting each pose's
commanded angles and observed right/up/forward basis into the existing
metadata model. Completed sessions are strict by default; active, aborted, or
failed sessions require `--allow-incomplete`. Windows screenshot paths are
portable: the loader uses the recorded path when available and otherwise
resolves the basename beside the metadata or under `--image-dir`. The legacy
contract/session schema and log-extractor workflow remain supported. CLI
examples are `pano-stitch validate session.json --image-dir screenshots` and
the equivalent `render` invocation. Tests, Ruff, mypy, and diff checks pass.

Plan update (2026-08-21): the former packaging/documentation packet is now
Packet 11. Packet 10 is a desktop GUI for the stitcher: select a CET capture
JSON, screenshot directory, and output directory; validate or render with all
existing output options; and show renderer progress plus actionable errors.
The GUI will be a thin frontend over the same validator and bounded compositor
used by the CLI, so GUI and CLI renders remain equivalent.

Polish research (2026-08-21): defer exposure normalization and upright/horizon
correction until after MVP completion. The recorded design uses robust,
overlap-derived global HDR luminance gains with an anchor and bounded EV range;
it deliberately defers per-channel and spatial compensation. Upright detection
will be optional and preview-first: detect lines in rectilinear preview views,
propose a global output rotation, and let the user choose its strength or leave
it disabled. Details and sources are in `docs/polish-research.md`.

Packet 9 direct-render finding (2026-08-21): completed CET session
`1787269393-1` validated and rendered directly from
`PanoramaCaptureBridge.pano-1787269393-1.json` to a 2984×1492 2:1 PNG with no
missing-pixel regions or pose-boundary seams. The image nevertheless had a
90° rotation because CET reports a Z-up camera basis and the stitcher uses a
Y-up canonical world. The CET adapter now maps each vector `(x, y, z)` to
`(x, z, y)` before projection. The session must be rerendered to confirm the
corrected orientation before Packet 9 acceptance. The corrected
`/tmp/cp2077-pano-packet9-yup.png` has now been visually verified with the
correct way-up. Packet 9's remaining acceptance item is a real-session run of
the optional coverage diagnostic image.

Coverage diagnostic implementation (2026-08-21): `pano-stitch render` now
accepts `--debug-coverage path.png` and writes a streaming grayscale mask
(white covered pixels, black uncovered pixels) alongside the normal output.
It uses the existing disk-backed weight tiles and adds no panorama-sized RAM
allocation. The compositor test, CLI help, Ruff, mypy, and full test suite pass.

Packet 9 final acceptance (2026-08-21): the corrected real-session panorama
was visually verified with the correct way-up. Its generated coverage mask
`/tmp/cp2077-pano-packet9-coverage.png` is pure white, confirming every
equirectangular output pixel has valid source coverage. Packet 9 is complete;
remaining polish concerns such as exposure normalization and optional upright
correction are deferred.

Packet 10 GUI slice (2026-08-21): added `pano-stitch-gui`, a Tkinter frontend
over the existing validator and bounded compositor. It provides capture JSON,
screenshot-directory, output-directory, and output-name pickers; PNG/EXR,
resolution, width, blend, memory, incomplete-session, and coverage options;
Validate/Render/Cancel actions; determinate progress; actionable dialogs; and
cooperative cancellation that removes partial outputs. Ruff, mypy, tests, and
CLI/package checks pass. A Windows desktop smoke test remains because WSL has
no usable display server.

WSLg scaling fix (2026-08-21): the GUI now applies a 1.5 Tk scaling factor when
`WSL_DISTRO_NAME` is present, matching the current 150% Windows display setup.
Set `PANO_STITCH_GUI_SCALE` to another positive value before launch to override
it, for example `PANO_STITCH_GUI_SCALE=2 .venv/bin/pano-stitch-gui`. Standard
Tk named fonts are explicitly scaled as well because WSLg was enlarging
controls but leaving labels and entry text too small. WSLg still renders the
font metrics poorly in this setup, so further WSL-specific GUI tuning is
deferred; native Windows packaged-executable DPI behavior is the acceptance
target.

GUI settings persistence (2026-08-21): the frontend now remembers the last
Capture JSON directory, screenshots directory, and output directory in a
per-user `PanoramaCapture/gui-settings.json` file, using an atomic replacement
and never writing machine paths into capture metadata. Selecting a new JSON
refreshes the screenshot directory from its recorded absolute paths, even when
an older screenshot directory was persisted. Persistence was verified across
GUI restarts. Packet 10 native Windows smoke testing is complete; release
packaging remains deferred to Packet 11.

Windows path inference (2026-08-21): when the screenshots field is blank, the
GUI now loads the session and infers the directory from an existing absolute
CET screenshot path. This is useful on native Windows, where the recorded
paths are directly accessible; an explicit directory remains available for
relocated captures.

Windows path-picker polish (2026-08-21): the GUI normalizes persisted WSL
`/mnt/X/...` paths to Windows drive paths when running natively, and each
Browse button opens the directory currently shown in its own field instead of
reusing the Capture JSON picker directory.

GUI workflow update (2026-08-21): removed the separate Validate button. Once
both Capture JSON and screenshots directory are present, validation runs in a
debounced background worker and reruns when either input changes. Render stays
disabled until validation succeeds. Resolution is now a 1–100% slider; explicit
width displays an override note; memory accepts up to 8096 MiB with a speed/RAM
tradeoff note. The compositor and CLI budget cap were raised to 8192 MiB.

Packet 10 native Windows acceptance (2026-08-21): background validation and
Render gating, resolution slider, explicit-width override, memory budget,
per-field browsing, JSON directory inference, persistence, and Windows path
normalization were verified. Path assignments now share one native-display
normalization helper, so programmatic inference and Browse selections use the
same slash style. Native Windows verification is complete after copying the
latest GUI source to the Windows checkout.

GUI simplification (2026-08-21): the default output name now uses the capture
metadata `session_id` (including its CET timestamp) and follows PNG/EXR format changes.
Render asks for confirmation before replacing an existing output. Format and
resolution remain visible; width, blend, memory, incomplete-session handling,
and coverage diagnostics are grouped under a collapsible Advanced options
section.

GUI layout polish (2026-08-21): moved the memory-budget performance note
directly below its field and reduced the collapsed window minimum height so
hidden Advanced options no longer leave excessive empty space.

JPEG export (2026-08-21): added SDR JPEG output with a 1–100 quality slider,
defaulting to 95 and using 4:4:4 chroma subsampling; JPEG is the GUI default.
The encoder spools SDR
rows to a temporary raw RGB file and memory-maps it for Pillow's non-progressive
JPEG write, avoiding a second full panorama allocation in Python RAM; the raw
file is removed with the render scratch directory. The CLI exposes the same
control as `--jpeg-quality`.

GUI render lock (2026-08-21): validation and rendering disable every editable
field and Browse button, preventing session settings from changing under a
worker. Cancel and the Advanced options toggle remain available.

Final Packet 10 GUI verification (2026-08-21): native Windows testing confirmed
the JPEG-default export, quality 95, render-time form lock, and replacement
confirmation for an existing output file.

Packet 11 release tooling (2026-08-21): shareable artifacts are now built only
from version tags by GitHub Actions. The pinned-action release workflow builds
the ReShade add-on with MSVC x64, bundles the GUI with PyInstaller, stages CET
and schema files, validates ZIP contents, produces `SHA256SUMS.txt`, and
publishes the assets plus `BUILD-INFO.txt` provenance through the GitHub CLI. A separate CI workflow runs the
stitcher checks for pull requests and `master`. `README.md` documents the
tag-driven release process; the package must still complete its first hosted
build and clean-machine installation test.

An in-game stock IGCS Connector panorama test completed camera movement and restoration, producing nine 3840×2160 baseline 8-bit JPEGs. Their HDR colors were incorrect, confirming that the connector's `uint8_t` capture path cannot replace normal ReShade HDR screenshots.

Scope correction: a horizontal 360-degree band is diagnostic only. The MVP requires a complete 360×180 sphere. The published IGCS Connector ABI has yaw but no pitch command, so IGCS remains a candidate rather than the accepted production backend until Packet 5 proves precise pitch control.

New primary path: use the already working normal FPP yaw/pitch controller outside Photo Mode. Installed CET mods prove access to near-zero time dilation (`SetTimeDilation`/`UnsetTimeDilation`), HUD hiding through `inkHUDLayer` opacity or the `/interface/hud` settings group, and player-renderer hiding by disabling the player puppet's mesh components. Use mesh-component hiding rather than gameplay `SetInvisible`, preserve every component's original enabled state, and re-scan for newly created equipment meshes. Ultra+ additionally demonstrates real-FPS measurement from CET update deltas, frame-generation setting lookup, and optional native presented-FPS measurement. Temporal settling will count at least eight real CET update frames after each final pose write; generated presents do not count. These controls still require a combined in-game visual/restoration test before production capture work. Implementation references and restoration requirements are recorded in `docs/cet-fpp-reference-implementations.md`.
