# Current implementation state

## D3D12 stitcher migration

- Native review rewind (2026-08-28): review found Release-disabled native assertions,
  non-idempotent cancellation-token destruction, exception leakage on allocation failure,
  first-hardware-adapter selection before capability checks, falsely successful missing memory
  budgets, and skipped aligned 32-row band candidates. These findings reopen steps 3, 4, and 6;
  the earliest incomplete substep is 3a. Step 5 must be rerun after the Release harness/ABI repair,
  and 7a must not start until every reopened gate passes.
- Step 3a repair (2026-08-28): replaced native-test `assert` calls with an always-evaluated check
  helper and added an expected-failure CTest. Portable Debug/Release CTests and the Windows MSVC
  Release expected-failure test now prove checks execute under `NDEBUG`; the regular Windows WARP
  dispatch test remains reopened separately after exposing a device-removal failure.
- Step 3c/3d repair (2026-08-28): made cancellation-token creation clear its out-handle and
  translate allocation failures to `PANO_GPU_OUT_OF_MEMORY`; test-only allocation injection proves
  that path. Destroy APIs now consume pointer-to-handle slots and null them before releasing, so
  repeated cleanup is harmless. Portable Debug/Release CTests and the focused MSVC Release token
  test pass.
- Step 4b/4c repair (2026-08-28): adapter enumeration now attempts device/capability/budget
  admission for every non-software candidate instead of accepting the first hardware adapter.
  Failed `IDXGIAdapter3` conversion or local-memory query rejects the candidate; probe output is
  initialized deterministically before fallible work. The poisoned portable unavailable-path test
  and MSVC Release WARP preflight pass.
- Step 6b repair (2026-08-28): band planning begins at the largest 32-row-aligned candidate and
  stops safely at 32 rows. The native regression proves a 40-row output selects a 32-row band when
  the resident output does not fit. Portable Debug/Release CTests, 109 available Python tests,
  Ruff, formatting, mypy, and focused MSVC Release native tests pass.
- Step 5 repair and Step 6 rerun (2026-08-28): the WARP self-test output resource now declares
  `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`, separate from the plain upload/readback buffers.
  This fixes the prior device-removal failure at command-allocator creation; deterministic WARP
  dispatch/readback passes. All portable Debug/Release CTests and all four MSVC Release CTests now
  pass, including WARP preflight and dispatch.
- Step 7a.1–7a.3 (2026-08-28): added versioned live-object diagnostics, an explicit-test WARP
  device handle owning adapter/device/queue/fence, idempotent handle destruction, and handle-based
  fill dispatch. The compatibility self-test now creates, dispatches, and destroys that handle.
  Portable Debug/Release CTests and all MSVC Release CTests pass; the WARP contract proves direct
  and wrapper dispatch return device, queue, and fence counts to zero.
- Step 7a.4 (2026-08-28): extracted the fully compatible adapter-selection factory for both
  preflight and persistent device creation, enabled product hardware creation through it, and added
  device-scoped identity/usable-memory diagnostics. Product creation continues past incompatible
  adapters and rejects software; WARP remains opt-in. Portable Debug/Release CTests and all MSVC
  Release CTests pass. Physical-adapter identity acceptance remains a manual hardware matrix item.
- Step 7b.1/7b.2 (2026-08-28): declared the fixed-width 72-byte x64 empty-session ABI and native
  sample types, then added non-allocating validation of the parent LUID, dimensions, native sample
  type, packed RGB stride, and coherent optional metadata buffers. Native and ctypes ABI-layout
  tests pass; portable Debug/Release and MSVC Release CTests pass.
- Step 7b.3 (2026-08-28): moved device ownership behind an internal reference-counted core, then
  added empty sessions that retain it without source resources. The native C ABI is now version 2
  to reject old diagnostics layouts. WARP tests cover session-before-device and device-before-
  session destruction plus injected session-allocation failure; all live device, queue, fence, and
  session counters return to zero. Portable Debug/Release and MSVC Release CTests pass.
- Step 7c.1 (2026-08-28): added an explicit, checked, 64 KiB-aligned default-heap allocation for
  one native-precision source buffer. A test-only byte query and allocation-failure hook prove a
  tiny RGB8 source allocates exactly 64 KiB, failure leaves zero bytes, and repeat allocation is
  rejected. Portable Debug/Release and MSVC Release CTests pass.
- Step 7c.2 (2026-08-28): extended the resident source buffer to all validated frames before
  alignment. WARP verifies a two-frame RGB8 fixture reserves ten 64 KiB blocks, distinct from the
  one-frame allocation; portable Debug/Release and MSVC Release CTests pass.
- Step 7c.3.1 (2026-08-28): added immutable default-heap rotation storage with independent
  requested-versus-allocated byte tracking and injected allocation failure. WARP proves failed
  allocation reports zero bytes, successful one-frame matrices reserve 64 KiB, and repeated
  allocation is rejected. Portable Debug/Release and MSVC Release CTests pass.
- Step 7c.3.2 (2026-08-28): added one validated rotation upload through a temporary mapped upload
  resource, command list, and monotonic persistent-device fence. The staging object is released
  after completion and repeat upload is rejected. Portable Debug/Release and MSVC Release CTests
  pass; the WARP sequence also covers upload followed by direct dispatch.
- Step 7c.3.3 (2026-08-28): added test-only temporary readback of uploaded rotation matrices.
  Deterministic nonzero matrices round-trip byte-for-byte on WARP; no readback object is retained.
  Portable Debug/Release and MSVC Release CTests pass.
- Step 7c.4.1 (2026-08-28): added optional immutable encoding-metadata storage. Null metadata
  creates no resource; present metadata uses checked 64 KiB-aligned default-heap storage and has an
  independent allocation-failure hook. Portable Debug/Release and MSVC Release CTests pass.
- Step 7c.4.2 (2026-08-28): added one validated optional encoding-metadata upload through a
  temporary staging resource and monotonic persistent-device fence. Repeat upload is rejected and
  staging does not persist. Portable Debug/Release and MSVC Release CTests pass.
- Step 7c.4.3 (2026-08-28): added test-only temporary encoding-metadata readback. The present
  five-byte fixture round-trips exactly on WARP, while absent metadata retains no bytes. Portable
  Debug/Release and MSVC Release CTests pass.
- Step 7c.5 (2026-08-28): added versioned session allocation diagnostics for planned and actual
  source, rotation, and encoding-metadata bytes. WARP verifies plans match allocations, including
  absent metadata. Portable Debug/Release and MSVC Release CTests pass.
- Step 7d.1 (2026-08-28): added the versioned caller-buffer source-upload ABI and pure validation
  against session frame index, sample type, row stride, and exact frame byte count. WARP contract
  tests cover valid input and invalid index/byte-count/sample-type cases; portable Debug/Release
  and MSVC Release CTests pass.
- Step 7d.2 (2026-08-28): added one persistent mapped upload-heap slot, sized to a checked aligned
  native source frame and explicitly unmapped during session destruction. WARP verifies one 64 KiB
  slot for the RGB8 fixture and safely rejects repeat allocation. Portable Debug/Release and MSVC
  Release CTests pass.
- Step 7d.3.1 (2026-08-28): added a validated frame-zero source copy through the persistent slot,
  command list, and monotonic fence. The WARP contract submits a deterministic nonzero RGB8 frame
  after validation; portable Debug/Release and MSVC Release CTests pass.
- Step 7d.3.2 (2026-08-28): added test-only temporary frame-zero source readback. WARP proves the
  deterministic RGB8 upload is byte-exact; portable Debug/Release and MSVC Release CTests pass.
- Step 7d.4/7d.5 (2026-08-28): generalized source copy/readback internals while preserving strict
  frame-zero wrappers, then verified two distinct frames reuse one 64 KiB persistent slot and
  round-trip in order on WARP. Session diagnostics now report checked upload count, raw byte total,
  and last completed fence. Portable Debug/Release and MSVC Release CTests pass; the ABI is v3.

- Step 1 (2026-08-28): added platform-independent `gpu_contract`, future Windows WARP, and
  hardware-acceptance pytest markers; extracted deterministic renderer-test builders; added CPU
  regression coverage for output formats, PQ conversion, and incomplete coverage/magenta pixels.
- Step 2 (2026-08-28): added backend-neutral GPU contract names and a `"gpu"` selector while
  preserving the temporary CUDA compatibility surface.
- Step 3 (2026-08-28): added the standalone native C ABI skeleton, idempotent destroy entry points,
  cancellation token, controlled unavailable probe, native CTest, and Linux-safe ctypes loader.
  Linux verification passed: native CMake/CTest plus Ruff, formatting, mypy, and 109 Python tests
  excluding CUDA hardware runtime tests. Windows DLL loading and WARP validation remain pending.
- Step 4 (2026-08-28): added versioned probe options and adapter diagnostics plus DXGI
  high-performance hardware enumeration, software-adapter rejection, explicit test-only WARP,
  feature-level 11_0 device creation, and local-memory budget/usage query. The portable contract
  path passes locally; the required Windows WARP build/probe and physical-adapter acceptance remain
  pending, so later D3D12 dispatch work must not yet proceed.
- Step 4 Windows gate (2026-08-28): staged the native source under `C:\dev` for MSVC because
  Windows cannot use the WSL UNC working directory. MSVC 19.51/Windows SDK 10.0.26100 built the
  DLL and CTest successfully performed explicit WARP preflight.
- Step 5 (2026-08-28): added a committed SM 5.1 fill shader compiled by `fxc.exe` at CMake build
  time and embedded into the DLL. The WARP CTest creates the root signature, pipeline, descriptor
  heap, upload/default/readback resources, direct queue, command list, and fence, then verifies
  deterministic readback. `dumpbin /dependents` confirms the release DLL depends on D3D12/DXGI and
  standard Windows/MSVC runtime libraries only; it has no runtime shader compiler, CUDA, or vendor
  library dependency.
- Step 6 (2026-08-28): added the native checked-arithmetic D3D12 memory planner with 64 KiB
  resource alignment, the existing reserve/minimum-banded policy, descriptor accounting, and a
  `uint32` histogram-population admission guard. Portable and MSVC CTests cover resident admission
  and histogram overflow; MSVC's Windows `max` macro collision was fixed with `NOMINMAX`.

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

Packet 12 plan (2026-08-21): automatic linear-HDR overlap-graph exposure
normalization is the next stitcher requirement and must support both SDR and
HDR inputs. Native Settings integration is optional at runtime and will provide
capture-only horizontal FoV, an estimated screenshot-count summary, and a
0.1–3.0 second settling-delay control (default 1.0); settings must be applied
only while idle and restored after a capture. The packet also reduces normal
CET and ReShade logging to user-facing session events and includes a
release-oriented review of the stitcher, add-on, and CET state machine. FoV
conversion, plan estimation, capture metadata, and stitching tests must support
4:3, 16:9, 16:10, 21:9, and 32:9 without cropping or a hidden 16:9 assumption.

Packet 12 implementation started (2026-08-21): the stitcher now performs a
bounded-memory linear-light exposure prepass, reports the overlap graph and
gains in CLI/GUI, and applies those gains during streaming compositing. The
CET development build also has optional Native Settings controls for temporary
capture FoV and settling delay (default 1.0 second), with capture-only FoV
restoration. In-game Native Settings verification and the remaining logging
cleanup are still pending.

Packet 11 release tooling (2026-08-21): shareable artifacts are now built only
from version tags by GitHub Actions. A manual, non-publishing dispatch executes
the same Windows build and retains its ZIPs, checksums, and provenance for seven
days for a clean-machine smoke test. The pinned-action release workflow builds
the ReShade add-on with MSVC x64, bundles the GUI with PyInstaller, stages CET
files, validates ZIP contents, produces `SHA256SUMS.txt`, and publishes the
assets plus `BUILD-INFO.txt` provenance through the GitHub CLI. The schema is
now internal to the PyInstaller bundle rather than a visible extra directory.
The packaged GUI records unexpected worker failures in the user configuration
directory, so windowed EXR failures retain their tracebacks. The successful
hosted dry run verified the packaged GUI's JPEG and EXR output; trimming broad
PyInstaller collection of direct imports reduced its package by about 8 MiB
while keeping the dynamically loaded OpenEXR and Imath modules. A separate CI
workflow runs stitcher checks for pull requests and `master`. `README.md`
documents the tag-driven release process; an independent clean-machine
installation test remains before the first public tag.

An in-game stock IGCS Connector panorama test completed camera movement and restoration, producing nine 3840×2160 baseline 8-bit JPEGs. Their HDR colors were incorrect, confirming that the connector's `uint8_t` capture path cannot replace normal ReShade HDR screenshots.

Scope correction: a horizontal 360-degree band is diagnostic only. The MVP requires a complete 360×180 sphere. The published IGCS Connector ABI has yaw but no pitch command, so IGCS remains a candidate rather than the accepted production backend until Packet 5 proves precise pitch control.

New primary path: use the already working normal FPP yaw/pitch controller outside Photo Mode. Installed CET mods prove access to near-zero time dilation (`SetTimeDilation`/`UnsetTimeDilation`), HUD hiding through `inkHUDLayer` opacity or the `/interface/hud` settings group, and player-renderer hiding by disabling the player puppet's mesh components. Use mesh-component hiding rather than gameplay `SetInvisible`, preserve every component's original enabled state, and re-scan for newly created equipment meshes. Ultra+ additionally demonstrates real-FPS measurement from CET update deltas, frame-generation setting lookup, and optional native presented-FPS measurement. Temporal settling will count at least eight real CET update frames after each final pose write; generated presents do not count. These controls still require a combined in-game visual/restoration test before production capture work. Implementation references and restoration requirements are recorded in `docs/cet-fpp-reference-implementations.md`.

Packet 12 current status (2026-08-21): automatic bounded-memory exposure normalization is implemented for SDR and PQ/Rec.2020 HDR sources. It solves robust overlap-graph luminance gains in a linear-light prepass, applies gains during streaming composition, reports the anchor/edge/gain summary in CLI and GUI, and rejects disconnected or unsampled graphs. Synthetic SDR gain recovery, HDR round-trip, memory, and cancellation tests pass. Native Settings is intentionally limited to the verified settling-delay and ReShade-toast-cooldown controls; capture FoV override and live planned-count controls were removed because the game camera did not reliably accept the override and Native Settings could not update the summary. The active in-game FPP FoV remains authoritative and already drives pose planning correctly across tested aspect ratios.

Packet 12 logging cleanup (2026-08-21): CET per-pose movement, metadata, readiness, acknowledgement, and settling diagnostics are now disabled behind `DEVELOPMENT_MODE`; lifecycle, cancellation, and error messages remain. The ReShade add-on no longer logs routine screenshot request/save messages, retaining startup and timeout/error reporting. The capture-relative CET basis fix is verified in a real render: yaw-zero screen centre now maps to panorama centre rather than a 90-degree world-axis offset. Full review and clean-machine packaged-artifact testing remain release gates.

Packet 12 aspect regression coverage (2026-08-21): projection tests now exercise
4:3, 16:9, 16:10, 21:9, and 32:9 source geometry, checking finite maps and
complete full-sphere coverage. The stitcher suite now passes 26 tests. Native
Windows compilation and in-game verification are intentionally delegated to
Jenkins after local Windows build directories were removed.

Packet 12 console cleanup v0.1.32 (2026-08-21): routine CET probe transitions,
pose queueing, readiness, screenshot acknowledgements, and inactive-binding
messages are disabled unless `DEVELOPMENT_MODE` is enabled. Version/startup,
session lifecycle, status, and actionable error messages remain visible. This
keeps normal capture output concise without removing diagnostic data from the
session JSON or the explicit status binding.

Packet 12 binding cleanup v0.1.33 (2026-08-21): production CET registration
now exposes only Start and Abort. The environment probe, status, and manual
advance handlers remain in source but are hidden behind the disabled
`DEVELOPMENT_MODE` flag for future manual-mode work.

Packet 12 review corrections v0.1.34 (2026-08-21): release captures now reject
`automatedScreenshots = false` with an explicit development-only diagnostic;
manual Advance remains unregistered. Terminal pitch-correction exhaustion is
kept as a normal cancellation error. Completion state is captured before Lua
session state is cleared, so successful production runs report completion.

Packet 12 completion-state correction v0.1.35 (2026-08-21): restoration now
distinguishes completed production sessions from incomplete/failed production
sessions before clearing state; only the former emits the completion message,
while the latter emits an aborted message.

ReShade add-on reliability review (2026-08-21): in-flight bridge requests now
receive an error acknowledgement when an effect runtime is destroyed. Worker
filesystem polling uses non-throwing error-code APIs with rate-limited error
reporting, acknowledgement files are checked after flush/close before atomic
publication, and CMake rejects non-64-bit Windows configurations. Native MSVC
compilation remains a Jenkins verification step.

ReShade contract tests (2026-08-21): added CI/release checks for the 64-bit
CMake requirement and the add-on bridge failure-handling safeguards. The
combined Python test suite now passes 28 tests; native compilation and runtime
behavior remain Jenkins/game acceptance checks.

Capture recovery hardening v0.1.36 (2026-08-21): CET now aborts after a
bounded ReShade acknowledgement wait, refuses to settle/capture when HUD or
equipment re-hide fails, and treats any metadata publication failure as
terminal. Active Native Settings values are copied into the production-session
snapshot at start. Aborted metadata remains removed in normal builds; setting
`DEVELOPMENT_MODE` enables incomplete-session metadata, diagnostic logging, and
development-only bindings together.

Development-mode flag consolidation v0.1.37 (2026-08-21): all CET developer
facilities now share the single `DEVELOPMENT_MODE` switch.

Packet 12 final review hardening v0.1.38 (2026-08-21): the capture bridge and
stitcher now have bounded acknowledgement recovery, safe metadata publication,
and safe add-on worker failure handling. CET treats write or close failures when
publishing metadata as terminal and preserves manual advance only when the
single `DEVELOPMENT_MODE` flag is enabled. Release mode still requires automated
screenshots. The stitcher accepts JPEG inputs as SDR, filters unavailable frames
before CLI/GUI resource estimates and rendering when incomplete sessions are
explicitly allowed, and prompts before replacing either the panorama or its
coverage diagnostic. Version-1 timing/FPS fields remain schema-compatible but
are deprecated and ignored; active capture timing is seconds-based only.

Current acceptance state (2026-08-21): Ruff, mypy, and the combined stitcher
and ReShade contract suite pass (31 tests). The latest reviewer pass found no
remaining actionable correctness issues. Jenkins/native MSVC compilation and
in-game testing remain the required acceptance gates for a tagged release.

Session location metadata v0.1.39 (2026-08-22): production CET captures now
write an optional root `location` object containing the player world position
from `GetWorldPosition()` and heading from `GetWorldYaw()` at session start,
before camera movement. The shared schema accepts this field while keeping it
optional so older version-1 manifests remain renderable.

Capture FoV integration v0.1.40 (2026-08-24): Native Settings now exposes a
30–120° capture FoV control. When the first-party FOV Control redscript
integration is installed separately, PanoramaCapture applies that display FoV
to the FPP camera through `PendingSetFOV()`/`SetDisplayFOV()`, reads back the
effective projection, plans from the observed values, and restores the original
FoV on completion or abort. Without the integration, capture remains usable
with the game’s current FoV and the requested override is skipped safely.

Manual release-build acceptance (2026-08-21): the GitHub Actions Windows
workflow-dispatch dry run completed successfully. Native MSVC ReShade
compilation, PyInstaller stitcher bundling, separate mod/stitcher archives,
archive validation, checksums, and build provenance were verified. No public
version tag has been created; code review remains the release gate.

Packet 12 capture and compositor acceptance (2026-08-24): `settings.json`
persistence is verified. With the separately installed first-party FOV Control
helper, Panorama Capture applies the requested FoV before time dilation,
waits for the camera transition, plans from the observed FoV, and restores the
original FoV after abort; the 35°-FoV capture plan stitched with 100% coverage.
The stitcher now safely splits OpenCV remaps wider or taller than 32,766 pixels,
uses bounded parallel strip compositing with OpenCV's nested worker pool
temporarily limited to one thread, and stores its colour/weight accumulators in
disk-backed memory maps. The resulting large render has verified improved CPU
utilization, negligible disk reads, and modest resident RAM; Windows CI now
explicitly closes mappings before temporary-file cleanup.

Packet 12 live capture estimate v0.1.58 (2026-08-24): the ReShade add-on now
publishes the active swapchain range (`hdr`, `sdr`, or `unknown`) atomically to
the bridge directory as soon as the game swapchain initializes. CET polls that
status once per second and presents a live Native Settings summary in the form
`Capture estimate: N shots / ~time · ~MP`. HDR estimates conservatively allow
one second per screenshot (SDR allows 0.1 seconds); unknown range intentionally
shows `timing unavailable`. A Lua multi-return parsing bug that dropped the
range field from the valid `1<TAB>hdr` bridge record was corrected. The custom
summary widget was restyled to 36 px with the NSUI label hue at 80% brightness
and a 40 px left indent. CET v0.1.58 is copied to the game directory; the
updated live HDR timing display awaits in-game confirmation.

Exposure compensation follow-up (implementation brief, 2026-08-25): the
current solver estimates relative log-exposure offsets correctly, but applying
one global gain to every pixel of each source relights areas that were not part
of the overlap (for example, a dark ground beneath a bright sky). Replace that
application with overlap-local compensation. Keep the geometric overlap
solver, but treat its values as relative log corrections `c[i]`. Construct a
disk-backed float32 correction field before compositing using the same smooth
feather weights as the compositor: `C(p) = sum(w[i,p] * c[i]) / sum(w[i,p])`.
While sampling frame `i`, multiply linear RGB only at valid pixels by
`exp(c[i] - C(p))`, then perform the normal hard or feather blend. In
single-source regions the multiplier is exactly one; overlaps share a smoothly
varying exposure, and any constant added to all corrections cancels. Never use
histogram matching, CLAHE, tone mapping, or global per-image normalization.
Preserve unclipped float HDR for EXR and the existing deterministic SDR output
conversion for PNG/JPEG.

Centralize weight calculation so exposure mapping and final blending cannot
drift. Account for the extra one-channel scratch map in resource estimates and
progress reporting, close all memmaps explicitly on Windows, and retain bounded
strip parallelism. Keep relative-exposure diagnostics, but describe them as
estimates rather than applied global gains. Add tests proving that non-overlap
bright/dark regions remain unchanged, overlap seams have no luminance step for
hard and feather modes, HDR extrema remain unclipped, a global correction offset
is invariant, and serial/parallel output remains deterministic. Also exercise
rejection of near-black, clipped, high-gradient, non-finite, and high-MAD
overlap samples plus the existing disconnected-graph failure.

Exposure-local compensation implementation (2026-08-25): renders now build a
disk-backed per-output-pixel exposure field from geometric feather weights and
apply only `exp(frame_log_gain - local_exposure)` to valid linear samples. The
previous global per-source gain application was removed; single-source regions
therefore retain their original exposure while overlaps transition smoothly.
The resource estimate includes the additional float map, OpenCV remap limits
remain handled, and overlap estimation now rejects clipped/high-gradient
samples and high-MAD edges. Focused stitcher tests and mypy pass (35 tests).

Final Auto contrast implementation (2026-08-25): the rejected default
histogram equalization has been removed. The stitcher now applies a shared RGB
levels stretch to finalized SDR output using bounded strip passes, with
default-on CLI/GUI controls, five phase-local progress stages, cancellation
cleanup, and regression coverage. EXR remains unchanged because a Photoshop-
style level-255 white point is undefined for scene-linear HDR. The detailed
contract and implementation notes are in `docs/auto-contrast.md`.

Full-GPU stitcher WSL benchmark (2026-08-26): on an NVIDIA GeForce RTX 5090,
the strict 15-frame, 256×128 shared-scene benchmark at 512px produced a CPU
median of 0.157 s and a CUDA median of 0.073 s. CUDA recorded 15 source
uploads (1,474,560 bytes H2D), 393,300 bytes D2H, ten kernel launches, and no
disk scratch. Its warm cached 1024px preview median was 0.002 s versus 0.296 s
for CPU (118.63×). A 4096px full render measured 2.014 s CPU versus 0.760 s
CUDA (2.65×); CPU/CUDA PNGs differed by at most one RGB code. The cache,
transfer, parity, and simulated 6 GiB admission gates are therefore evidenced
locally. Clean Windows driver-only packaged-artifact validation remains the
external release gate.

GUI exposure-preview acceleration (2026-08-28): CUDA preview admission now
includes a retained RGB8 full preview, one full-resolution byte mask per pose,
the high-quality overview, and a reusable viewport output. Compact CPU masks
are expanded once on CUDA; moving crops, hover tint, and one-pixel boundaries
then compose in a single GPU kernel. The display worker presents the newest
completed generation during continuous pointer motion instead of starving on
newer queued requests. Tk still applies the downloaded viewport image. CPU
fallback retains the background Pillow compositor. Focused CPU tests and the
elevated 13-test CUDA runtime suite pass; packaged Windows responsiveness and
visual hit testing remain part of the clean-machine release gate.

D3D12 migration 7e.1 (2026-08-28): sessions now allocate a second bounded,
persistent mapped upload slot only after the proven first slot exists. Each slot
has distinct mapping and fence state, duplicate second-slot allocation is
rejected, and session destruction explicitly unmaps both. Native Debug/Release
contract suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 7e.2-7e.3 (2026-08-28): source uploads now use an explicit
local slot selection, preserving the first-slot-only path when no second slot
exists and selecting resident slots round-robin when it does. The WARP
readback test verifies both frame byte sequences and test-only slot-fence
diagnostics prove both slots were selected. Native Debug/Release contract
suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 7e.4-7e.6 (2026-08-28): source upload waiting is isolated in
a fence helper, uploads defer completion until their selected persistent slot
is reused, and diagnostics report only fences observed complete. The WARP
fixture uploads three frame payloads, proves first-slot fence advancement on
reuse, and reads every frame back byte-for-byte. Native Debug/Release contract
suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 7e.7-7e.9 (2026-08-28): a new ABI-compatible cancellable
upload entry point preserves the legacy uncancelled entry point. Uploads reject
pre-cancelled tokens before a slot wait or command submission, and recheck
cancellation after a reuse wait but before overwriting mapped bytes. A
test-only hook forces that boundary and proves frame-zero bytes and slot fence
state survive cancellation. Native Debug/Release contract suites and the
Windows MSVC Release/WARP suite pass.

D3D12 migration 7e.10 (2026-08-28): an explicit idle-safe upload finish entry
waits the submitted first-slot fence without allocating or submitting further
work. Native Debug/Release contract suites and the Windows MSVC Release/WARP
suite pass.

D3D12 migration 7e.11 (2026-08-28): finishing now waits each distinct submitted
persistent upload-slot fence without allocating or submitting further work. The
alternating three-frame fixture checks the final completion diagnostic. Native
Debug/Release contract suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 7e.12 (2026-08-28): upload finishing has an optional
cancellation token with checks around each wait; the forced first-wait
cancellation test leaves the ordinary finish path to complete both slots.
Native Debug/Release contract suites and the Windows MSVC Release/WARP suite
pass.

D3D12 migration 7f.1 (2026-08-28): the C ABI now states that successfully
uploaded source data remains resident until session destruction, establishing
the ownership rule required before output and preview jobs can retain sessions.

D3D12 migration 7f.2 (2026-08-28): a backend-neutral resident-session
identity now includes backend kind, adapter LUID, and ABI version. Focused
cache-key equality regressions prove every identity component affects reuse;
Ruff, formatting, mypy, and focused pytest pass.

D3D12 migration 7f.3 (2026-08-28): the existing CUDA key builder now carries
a defaulted backend-neutral CUDA compatibility identity without changing its
source, geometry, budget, cache-hit, replacement, or invalidation behavior.
Ruff, formatting, mypy, and focused pytest pass.

D3D12 migration 7f.4-7f.5 (2026-08-28): ctypes now declares native retained
device/session lifecycle calls and a closeable prepared-session owner releases
the session before its device, exactly once. Fake-library ABI and ownership
tests, Ruff, formatting, and mypy pass.

D3D12 migration 8a.1 (2026-08-28): versioned output-job options and a
validation-only ABI now reject malformed resident/banded plan inputs before
any job or GPU resource exists. Native Debug/Release contract suites and the
Windows MSVC Release/WARP suite pass.

D3D12 migration 8a.2.1 (2026-08-28): opaque native sessions now use an
internal reference count, preserving all existing creation, upload,
diagnostic, and pointer-to-handle destruction behavior while permitting a
future child output handle to retain the session safely. Native Debug/Release
contract suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 8a.2.2 (2026-08-28): an empty native output handle now
retains its session after caller-handle destruction and releases that final
reference exactly once on output destruction. Native Debug/Release contract
suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 8a.2.3 (2026-08-28): resident output handles now calculate
and allocate one aligned float32 linear-RGB resource exactly once, and expose
planned/actual allocation diagnostics. The portable Debug/Release contract
suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 8a.2.4 (2026-08-28): resident output handles now separately
plan and allocate their full-resolution coverage buffer, reporting exact
planned/actual RGB and coverage bytes. The portable Debug/Release contract
suites and the Windows MSVC Release/WARP suite pass.

D3D12 migration 8a.3.1 (2026-08-28): output diagnostics now retain and
report the accepted resident-versus-banded mode without allocating a resource.
The portable Debug/Release contract suites and the Windows MSVC Release/WARP
suite pass.

D3D12 migration 8a.3.2 (2026-08-28): a banded output now plans only its
initial `[0, band_rows)` RGB and coverage storage range. A large forced-banded
contract fixture proves those plans are bounded below resident output size;
portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8a.3.3 (2026-08-28): the existing output allocators now have
WARP coverage for a large forced-banded job, proving they create only the
initial RGB and coverage band sizes. Portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 8a.4.1 (2026-08-28): native diagnostics now report live
output-job handles, registered only after construction succeeds and released
once with the handle. Resident, banded, and parent-release contract paths
pass in portable Debug/Release and Windows MSVC Release/WARP suites.

D3D12 migration 8a.4.2 (2026-08-28): focused output-handle allocation
failure injection now returns ABI-safe out-of-memory, preserves a null
out-handle, and leaves all live diagnostics unchanged. Portable Debug/Release
and Windows MSVC Release/WARP suites pass.

D3D12 migration 8a.4.3 (2026-08-28): output teardown now releases its RGB
and coverage resources before its retained session. Allocated resident output
survives parent session/device-handle release and repeated destroy; all live
counts return to zero in portable Debug/Release and Windows MSVC Release/WARP
suites.

D3D12 migration 8b.1 (2026-08-28): a test-only versioned projection contract
now validates output geometry, row ranges, latitude/FoVs, and finite row-major
rotation values without allocating D3D12 resources. It returns exact world-ray,
camera-ray, projected-coordinate, and validity buffer sizes. Portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8b.2.1 (2026-08-28): the dedicated `cs_5_1` ray-only shader
now compiles to an embedded header independently of the existing fill self-test
shader. Portable CMake Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8b.2.2 (2026-08-28): a test-only D3D12 ray dispatch now binds
validated row-range constants and a temporary float3 UAV, then waits for its
fence without sampling sources or projecting cameras. Portable Debug/Release
and Windows MSVC Release/WARP suites pass.

D3D12 migration 8b.2.3 (2026-08-28): the ray dispatch now optionally copies
its temporary float3 UAV to an exact-size caller-owned buffer after fence
completion. WARP results match the independent pixel-center 4x2 CPU oracle,
and invalid readback buffers are rejected. Portable Debug/Release and Windows
MSVC Release/WARP suites pass.

D3D12 migration 8b.3.1-8b.3.2 (2026-08-28): ray dispatch now uploads a
16-byte-aligned row-major world-to-camera matrix and returns camera rays.
Identity and 90-degree Y-axis WARP readbacks match independent CPU row-vector
multiplication. Windows MSVC Release/WARP suite passes.

D3D12 migration 8b.4.1 (2026-08-28): the ray test shader and dispatch now
bind source dimensions plus focal lengths in a 16-byte-aligned constant block,
without changing camera-ray generation or requiring a projected-coordinate
readback. Portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8b.4.2 (2026-08-28): the ray test dispatch now writes a
second float2 UAV and optionally reads its exact validated byte count back to
the caller. WARP center and unclamped edge/behind-camera coordinates match an
independent CPU projection oracle; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 8b.4.3.1 (2026-08-28): the ray test shader now produces a
cleared packed validity-bit UAV using `z > 0` and half-pixel bounds, which the
test hook expands into its one-byte-per-pixel caller layout. WARP center,
edge, and behind-camera masks match the CPU oracle; portable Debug/Release and
Windows MSVC Release/WARP suites pass.

D3D12 migration 8b.4.3.2 (2026-08-28): projected coordinates are now
clamped to source pixel centers only after their raw half-pixel validity
decision. WARP center, boundary, and behind-camera coordinate/mask readbacks
match the CPU oracle; portable Debug/Release and Windows MSVC Release/WARP
suites pass.

D3D12 migration 8c.1 (2026-08-28): a versioned test-only `uint8` sampling
contract now validates exact coordinate/result byte counts and source-frame
readiness. Session uploads retain a per-frame completion fence so a completed
but different frame cannot be sampled; portable Debug/Release and Windows
MSVC Release/WARP suites pass.

D3D12 migration 8c.2.1 (2026-08-28): resident source buffers now transition
from upload `COPY_DEST` to the combined non-pixel-shader/copy-source read
state only after `finish_uploads`, then transition back before later uploads.
Repeated upload/finish/readback cycles and the strengthened completion-fence
diagnostic pass in portable Debug/Release and Windows MSVC Release/WARP suites.

D3D12 migration 8c.2.2 (2026-08-28): a dedicated embedded `cs_5_1` shader
now binds completed resident `R8_UINT` source data, a caller coordinate SRV,
and a temporary float3 result UAV. Exact corner loads match direct CPU bytes
normalized by 255; portable Debug/Release and Windows MSVC Release/WARP suites
pass.

D3D12 migration 8c.3 (2026-08-28): the `uint8` sampling shader now performs
four interleaved-RGB loads and float bilinear interpolation. Exact corners and
an interior half-pixel match an independent CPU four-tap oracle; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8c.4 (2026-08-28): finite `uint8` sampling coordinates now
clamp to source centers in the shader before manual bilinear loads, while
non-finite coordinates remain rejected at the ABI boundary. Corner, interior,
and clipped-coordinate CPU fixtures pass in portable Debug/Release and Windows
MSVC Release/WARP suites.

D3D12 migration 8d.1 (2026-08-28): `uint16` sampling now has its own
test-only admission function while reusing the established coordinate/result
layout. An isolated resident `uint16` fixture proves unready and wrong-type
calls are rejected and a finished frame returns exact layouts; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8d.2.1 (2026-08-28): the Windows build now independently
compiles and embeds a `cs_5_1` `uint16` sampling shader with typed native-value
loads and `/65535` normalization. Portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 8d.2.2 (2026-08-28): the sampling dispatch now selects the
resident session's typed `R8_UINT` or `R16_UINT` SRV, corresponding bytecode,
and element stride/offset without duplicating resource lifecycle code. `uint16`
exact-corner readbacks match direct CPU `/65535` loads; portable Debug/Release
and Windows MSVC Release/WARP suites pass.

D3D12 migration 8d.3 (2026-08-28): the native `uint16` shader now performs
four-tap bilinear interpolation; an interior half-pixel matches the independent
CPU oracle alongside direct corners. Portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 8d.4 (2026-08-28): finite `uint16` coordinates now clamp to
source centers before sampling, preserving the existing non-finite rejection.
Corner, interior, and clipped-coordinate fixtures pass in portable Debug/Release
and Windows MSVC Release/WARP suites.

D3D12 migration 8e.1 (2026-08-28): float32 sampling admission now has an
explicit test-only layout contract. It rejects malformed, unready, wrong-type,
and non-finite-coordinate requests, while an uploaded resident NaN/±∞ EXR-like
source is deliberately admitted so the later sampling/compositing pipeline keeps
the CPU's IEEE propagation boundary. Portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 8e.2 (2026-08-28): the Windows build independently compiles
and embeds a `cs_5_1` float32 sampler. Its typed `R32_FLOAT` SRV direct loads
preserve finite corner and interior values without source expansion or integer
normalization; portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8e.3.1 (2026-08-28): the float32 sampler now uses four native
float taps and bilinear interpolation for interior coordinates. A finite
half-pixel fixture matches an independent CPU four-tap oracle; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8e.3.2 (2026-08-28): finite float32 coordinates now clamp to
source centers before four-tap sampling. Corner and out-of-range fixtures match
the CPU interpolation oracle; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 8e.4 (2026-08-28): the admitted float32 NaN/±∞ fixture now
has a WARP sampling regression. Native IEEE interpolation produces the same
NaN result category as CPU `cv2.remap`, with no divergent source sanitization;
portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.1 (2026-08-28): one-frame composite bands now have a
test-only ABI contract that validates frame/source type, finite projection
geometry, output row bounds, finished residency, and exact caller-buffer byte
layouts before dispatch. Focused malformed/unready/wrong-type/out-of-band tests
pass in portable Debug/Release and Windows MSVC Release/WARP suites.

D3D12 migration 8f.2.1 (2026-08-28): one-frame bands now reuse the proven
projection shader through their composite contract. A nonzero-offset 8×2 WARP
band verifies every clipped source coordinate and packed validity bit against
the CPU equations; portable Debug/Release and Windows MSVC Release/WARP suites
pass.

D3D12 migration 8f.2.2.1 (2026-08-28): the Windows build independently
compiles and embeds a `cs_5_1` uint8 one-frame candidate shader. It combines
the proven projection, validity, source-center clipping, and native `R8_UINT`
bilinear equations; portable Debug/Release and Windows MSVC Release/WARP suites
pass.

D3D12 migration 8f.2.2.2 (2026-08-28): finished resident `R8_UINT` sources
now bind to the one-frame candidate shader through a correctly partitioned
SRV/UAV descriptor table. An 8×2 nonzero-offset band round-trips candidate RGB
and validity against the independent CPU projection/bilinear oracle; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.2.3.1 (2026-08-28): the Windows build independently
compiles and embeds a `cs_5_1` uint16 one-frame candidate shader, preserving
the projection/validity/clipping path with typed `R16_UINT` `/65535` loads.
Portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.2.3.2 (2026-08-28): one shared integer candidate dispatch
now binds `R8_UINT` or `R16_UINT` sources through strict public type wrappers,
with element-based offsets for uint16. A finished uint16 8×2 band matches the
independent CPU projection/bilinear `/65535` RGB and validity oracle; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.2.4.1 (2026-08-28): the Windows build independently
compiles and embeds a `cs_5_1` float32 one-frame candidate shader, preserving
projection/validity/clipping with typed `R32_FLOAT` four-tap loads and no
non-finite sanitization. Portable Debug/Release and Windows MSVC Release/WARP
suites pass.

D3D12 migration 8f.2.4.2 (2026-08-28): the shared typed candidate dispatch
now binds strict `R32_FLOAT` sources. A finished float32 8×2 band matches the
independent CPU projection/bilinear RGB and validity oracle; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.2.5 (2026-08-28): all typed one-frame candidate shaders
and their shared band-sized readback now surface the clipped source-edge
distance used by hard blending. Uint8, uint16, and float32 fixtures match the
CPU edge-distance oracle at interior, edge, and invalid pixels; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.3.1 (2026-08-28): hard selection now has a test-only ABI
contract for candidate RGB/validity/edge distance and prior RGB/weight
accumulators. It rejects malformed layouts, non-binary validity, and
non-finite or negative weights before dispatch; portable Debug/Release and
Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.3.2 (2026-08-28): the Windows build independently compiles
and embeds a `cs_5_1` hard-selection shader. It derives the established valid
candidate weight, retains prior RGB on equal weights through strict `>`, and
writes coverage from the resulting weight; portable Debug/Release and Windows
MSVC Release/WARP suites pass.

D3D12 migration 8f.3.3 (2026-08-28): hard selection now binds five input
SRVs and three selected-band UAVs, then reads selected RGB/weight/coverage
after a fence. Its WARP fixture proves replacement, invalid-candidate and
lower-weight retention, strict equal-weight ties, and coverage; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.3.4 prerequisite (2026-08-28): hard selection now consumes
the same packed GPU validity bits produced by candidate generation. Its host
test fixture packs the ABI byte mask before upload, eliminating a hidden
representation conversion from the forthcoming GPU-to-GPU command sequence;
portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.3.4.1 (2026-08-28): uint8 candidate generation and hard
selection now execute as two GPU passes in one command list. Candidate RGB,
packed validity, and edge distance transition from UAV to non-pixel SRV state
between passes; only final selected RGB/weight/coverage are read back. The WARP
fixture matches the separately CPU-verified candidate band and zeroed prior
accumulator; portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.3.4.2 (2026-08-28): the same GPU-resident command sequence
now supports typed `R16_UINT` candidate generation with element-based source
offsets. A zero-prior uint16 band reaches selected RGB/weight/coverage without
host candidate copies and matches the independently verified candidate results;
portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 8f.3.4.3 (2026-08-28): the GPU-resident command sequence now
also supports typed `R32_FLOAT` candidate generation with raw float element
offsets and established IEEE sample behavior. A zero-prior float32 band reaches
selected RGB/weight/coverage without host candidate copies and matches the
independently verified candidate results; portable Debug/Release and Windows
MSVC Release/WARP suites pass.

D3D12 migration 8f.4 (2026-08-28): the first complete one-frame native band
checkpoint is verified. Uint8, uint16, and float32 WARP paths each read back
only final selected linear RGB, weight, and coverage after GPU-resident
candidate generation and hard selection; the type-specific candidate results
were independently compared with the CPU projection/bilinear oracle. SDR
conversion and production adapter routing remain out of scope for this stage.

D3D12 migration 9a.1 (2026-08-28): a test-only ordered hard-composition band
contract now admits two or more finished, strictly capture-ordered frame
requests only when they share one source type and output-band geometry. It
returns the final selected RGB, weight, and coverage layouts before any GPU
work; empty, reversed, mixed-band, and out-of-range lists fail. Portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9a.2.1 (2026-08-28): the first two-frame hard-composition
unit now narrows that ordered contract to exactly two finished uint8 frames.
It returns the existing final selected RGB, weight, and coverage layouts and
rejects non-uint8 or wrong-count requests before GPU resource allocation.
Portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9a.2 (2026-08-28): two finished uint8 candidates now remain
GPU-resident through a ping-pong hard-selection accumulator. The two-pass
WARP fixture checks overlapping output, strict equal-weight ties, and a single
final RGB/weight/coverage readback. Candidate constants now preserve the
shader's padded float4 rotation rows. Windows MSVC Release/WARP passes all
native CTest cases.

D3D12 migration 9a.3 (2026-08-28): the ordered uint8 hard-selection sequence
now admits an explicit three-frame test dispatch. It retains only two selected
RGB/weight accumulator pairs by ping-ponging them, keeps all candidate and
prior data resident between passes, and reads back just the final band. The
three-frame WARP oracle verifies capture order, lower-weight retention, and
strict equal-weight retention; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 9a.4.1 (2026-08-28): a test-only two-frame uint16 ordered
hard-composition contract now shares the established final-band layout while
requiring two finished uint16 sources. A WARP fixture admits a finished
two-frame uint16 session and rejects a mixed-type list before dispatch;
portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9a.4.2 (2026-08-28): the bounded resident hard-composition
loop now selects the existing uint16 candidate shader and `R16_UINT` source
view, with element-based row/frame offsets. Its two-frame uint16 WARP oracle
matches independently generated candidates and reads back only final
RGB/weight/coverage; portable Debug/Release and Windows MSVC Release/WARP
suites pass.

D3D12 migration 9a.4.3 (2026-08-28): the uint16 hard-composition entry point
now also runs three capture-ordered frames through the same two-pair ping-pong
accumulator. The WARP oracle verifies final RGB, weight, and coverage against
independent three-frame candidates; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 9a.5.1-9a.5.2 (2026-08-28): two-frame float32 ordered
hard-composition now has a type-specific admission contract and a resident
dispatch using `R32_FLOAT`, the existing float32 candidate shader, and
element-based source offsets. A finite-source WARP oracle matches independent
float32 candidates with a single final RGB/weight/coverage readback; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9a.5.3 (2026-08-28): float32 now also runs three
capture-ordered frames through the existing typed ping-pong accumulator. The
finite-source WARP oracle verifies final RGB, weight, and coverage against
independent three-frame candidates; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 9a.6.1 (2026-08-28): a test-only output hard-composition
admission contract now requires exact output dimensions/current storage range,
an ordered request against the output's retained session, and allocated linear
and coverage resources. WARP verifies missing storage and mismatched geometry
fail before dispatch, while matching full-height storage is admitted; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9a.6.2 (2026-08-28): the typed hard-composition recorder can
now write an ordered uint8 result directly into an allocated output handle.
It copies only final selected RGB and coverage into existing `COPY_DEST`
storage, with no candidate/prior host intermediates or scheduling changes.
Portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9a.6.3 (2026-08-28): a test-only output-band readback copies
stored linear RGB and coverage through bounded readback resources, restores
the output buffers to `COPY_DEST`, and exposes only logical bytes rather than
alignment padding. The WARP output-handle result matches the independent
three-frame uint8 hard-selection oracle; portable Debug/Release and Windows
MSVC Release/WARP suites pass.

D3D12 migration 9b.1 (2026-08-28): a test-only feather-accumulation ABI
contract now validates source dimensions, exact candidate/accumulator layouts,
binary validity, finite nonnegative edge distances, and finite nonnegative
accumulated weights before any dispatch. Portable Debug/Release and Windows
MSVC Release/WARP suites pass.

D3D12 migration 9b.2 (2026-08-28): an SM 5.1 feather-weight shader now packs
the binary validity mask for a raw SRV, computes the existing clamped feather
width on WARP, and reads back only one scalar-weight buffer. Interior, edge,
invalid, and minimum-dimension weights match the CPU rule; portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9b.3 (2026-08-28): an SM 5.1 feather-accumulation shader now
adds one resident candidate RGB/weight pair to independent RGB and scalar
weight accumulators, then reads back only the final pair. A WARP fixture covers
nonzero prior accumulators and a zero-weight candidate; portable Debug/Release
and Windows MSVC Release/WARP suites pass.

D3D12 migration 9b.4.1 (2026-08-28): a two-frame feather test dispatch now
runs both weighted candidate passes in one command list. It transitions the
first RGB/weight accumulator pair directly into SRVs for the second pass and
reads back only the final pair. The overlapping-seam WARP fixture matches CPU
accumulated RGB and weights; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration 9b.4.2-9b.4.3 (2026-08-28): the bounded feather chain now
accepts exactly two or three ordered weighted inputs while retaining only two
ping-pong RGB/weight accumulator pairs. The three-frame pole fixture matches
CPU RGB and weights, and an order-sensitive `(1e20 + -1e20) + 1` fixture
returns `1` on WARP, proving sequential float32 pass ordering. Portable
Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9b.5 (2026-08-28): an SM 5.1 normalization pass now divides
accumulated RGB only for positive scalar weights and preserves zero-weight RGB
unchanged for incomplete-output handling. WARP covers whole, fractional, and
uncovered weights; portable Debug/Release and Windows MSVC Release/WARP suites
pass.

D3D12 migration 9c.1 (2026-08-28): a test-only exposure ABI contract now
requires one finite positive supplied gain per session frame and one finite
quarter-resolution local field using exact ceil-divided output dimensions.
Mismatched counts, non-finite gains/field values, and malformed dimensions fail
before dispatch; portable Debug/Release and Windows MSVC Release/WARP suites
pass.

D3D12 migration 9c.2 (2026-08-28): an SM 5.1 global-gain pass now multiplies
one linear candidate RGB buffer by a finite positive supplied gain before any
composition. Identity and non-identity WARP fixtures match CPU linear RGB;
portable Debug/Release and Windows MSVC Release/WARP suites pass.

D3D12 migration 9c.3 (2026-08-28): the three typed candidate shaders now
receive an explicitly padded per-frame global gain, defaulting to one for all
existing paths. A gain-aware two-frame uint8 hard-composition hook applies each
frame's own gain before selection while retaining strict weight ties; its WARP
overlap fixture matches CPU RGB, weight, and coverage. Portable Debug/Release
and Windows MSVC Release/WARP suites pass.

D3D12 migration 9c.4.1 (2026-08-28): ABI version 5 appends explicit
rectilinear-output metadata and a vertical output FOV to the one-frame output
geometry request. Zero-valued metadata retains the existing equirectangular
path; all three typed candidate shaders implement the established 90-degree
horizontal rectilinear ray convention. Native contract checks reject malformed
mode/FOV pairs, while portable Debug/Release and Windows MSVC Release/WARP
suites pass.

D3D12 migration 9c.4.2 (2026-08-28): an SM 5.1 equirectangular
quarter-resolution local-exposure dispatch now uses the existing center-sample
projection and validity convention, writes a bounded field, and reads it back
through the normal fence lifecycle. The one-frame WARP fixture verifies the
valid center value equals the supplied log gain; portable Debug/Release and
Windows MSVC Release/WARP suites pass.

D3D12 migration 9c.4.3 (2026-08-28): the same bounded local-exposure dispatch
now accepts the ABI-v5 rectilinear output mode and vertical-FOV convention.
Its WARP thumbnail-center fixture verifies the valid one-frame field equals
the supplied log gain; portable Debug/Release and Windows MSVC Release/WARP
suites pass.

D3D12 migration 9c.5 (2026-08-28): an SM 5.1 local-exposure pass now samples
the ceil-quarter field with the existing half-pixel, clamp, and bilinear
conventions, then applies its exponential to candidate linear RGB. WARP
interior and clipped samples match the CPU oracle; portable Release and
Windows MSVC Release/WARP suites pass.

D3D12 migration 9c.6 (2026-08-28): ordered hard composition now accepts one
bounded ceil-quarter local field per frame, applies it after each frame's
global gain and before strict hard selection, and retains the existing
selection/coverage rules. Identity, global-only, and spatial-field WARP
fixtures match the CPU oracle; portable Debug/Release and Windows MSVC
Release/WARP suites pass.

D3D12 migration prerequisite (2026-08-28): synchronized the ctypes adapter's
native ABI declaration and fake-DLL contract from version 3 to the native
header's version 4. This prevents a valid current DLL from being rejected at
load time; focused adapter pytest, Ruff, and mypy pass.

D3D12 migration 8f.2.4.2 (2026-08-28): the shared typed candidate dispatch
now binds strict `R32_FLOAT` sources. A finished float32 8×2 band matches the
independent CPU projection/bilinear RGB and validity oracle; portable
Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 9d.1 (2026-08-28): added a GPU post-hard-selection incomplete-output pass that writes linear magenta only for zero final weight and leaves coverage unbound. The focused WARP contract fixture verifies uncovered marking, covered RGB preservation, and unchanged coverage; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 9d.2 (2026-08-28): the same final-weight marker is now covered after feather normalization: only the zero-weight pixel becomes linear magenta and normalized covered pixels remain unchanged. Portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 9e.1 (2026-08-28): added the bounded output-band range binding required to execute a later 32-row band. The WARP fixture dispatches rows 32–63 of a 64-row output and matches the direct GPU hard-composition oracle while allocating only the requested band. Portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 9e.2 (2026-08-28): the bounded hard-composition output job now has a two-adjacent-band regression. Rows 0–31 and 32–63 are dispatched separately, and their concatenated RGB and coverage match one resident 64-row GPU composition across the boundary. Portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 9e.3 (2026-08-28): added a two-band feather regression using the resident two-frame accumulator and normalizer independently on adjacent 32-row slices. Their concatenated normalized pixels match a resident 64-row feather result. Portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 9f.1 (2026-08-28): added a D3D12 output-band scheduler boundary that delegates unchanged to the existing backend-neutral 64–1024-row watchdog policy and accepts elapsed time only for completed bands. Focused ruff, format, mypy, and 28 GPU/adapter contract tests pass.
D3D12 migration 9f.2–9f.3 (2026-08-28): added the native output-band runner. It publishes each completed row count only after the completed-band callback returns, records its elapsed time into the unchanged scheduler policy, rejects cancellation before another submission, and invokes bounded-output cleanup. Focused ruff, format, mypy, and 7 D3D12 adapter contract tests pass.
D3D12 migration 10a.1 (2026-08-28): added an allocation-free retained exposure-proxy layout contract. It preserves CUDA's `min(256, width)` width and ties-to-even rounded proportional height, plus float32 RGB frame offsets and total-byte overflow validation. Odd/even source fixtures pass; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10a.2 (2026-08-28): added a uint8 native-precision exposure-proxy dispatch using the existing fractional area footprint. It requires all source uploads to have completed and the source to be shader-readable, respects padded row/frame strides, and reads back only the bounded float32 proxy result for the test hook. Padded odd/even WARP sources match the independent CPU area oracle; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10a.3 (2026-08-28): extended the same proxy footprint and readiness contract to native `R16_UINT` and `R32_FLOAT` source bindings. Finished uint16 values match normalized CPU pixels, while float32 proxy pixels preserve permitted NaN and signed-infinity values; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10a.4 (2026-08-28): added a one-shot session-owned proxy build operation. It retains the completed bounded float32 GPU proxy in shader-readable state for downstream exposure work, rejects duplicate builds, and releases it through normal session destruction; padded odd/even WARP fixtures verify retained sizing alongside the CPU proxy oracle. Portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10b.1 (2026-08-28): added the one-pair exposure-grid admission contract. It requires two distinct completed frames from a session with retained proxies and uploaded rotations, finite established projection geometry, and exact caller-owned `float4` pair-coordinate plus byte-overlap layouts. Two-frame WARP fixtures verify valid admission and malformed/same-frame rejection; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10b.2 (2026-08-28): added the SM 5.1 retained-rotation pair-grid projection dispatch. It projects equirectangular sample centers with the established camera convention, clamps stored proxy coordinates, and returns a byte overlap mask only when both views are visible. Identity-rotation WARP fixtures cover front-facing and clipped samples against the CPU equations; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10b.3 (2026-08-28): added the SM 5.1 retained-proxy pair sampler. It uploads only the bounded projected-coordinate grid, manually bilinearly samples each frame's resident float32 proxy slice, and leaves the caller-owned geometric overlap mask unchanged. The WARP fixture matches both known source slices at every projected coordinate; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10c.1 (2026-08-28): added the SM 5.1 finite/luminance pair classifier. It produces independent Rec.709 luminance values and accepts only geometrically shared, finite, positive (`> 1e-5`) pair samples; it contains no clipping or gradient filtering. WARP cases cover valid high luminance, low luminance, NaN, infinity, and geometry exclusion; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10c.2 (2026-08-28): bumped the native/ctypes ABI to v6 and added an explicit session transfer-function enum (`sRGB`, `PQ`, `linear`). Exposure proxies now decode the declared transfer before downsampling. The pair classifier rejects samples with any channel at or above `0.995` for sRGB/PQ only; linear sources remain unbounded. WARP fixtures verify decoded sRGB pair samples, clipped SDR rejection, and above-one linear acceptance; portable Debug/Release, Windows MSVC Release/WARP, Ruff, mypy, and focused ctypes pytest pass.
D3D12 migration 10c.3 (2026-08-28): verified the established linear-HDR category is the explicit v6 `linear` transfer path introduced for 10c.2: it retains finite/positive and geometry checks but never applies SDR saturation rejection. The above-one linear WARP case passes while the same saturated SDR input is rejected; all 10c.2 gates pass.
D3D12 migration 10d.1 (2026-08-28): added a separate D3D12 exposure-pair gradient pass over classified luminance, matching the existing CUDA log-luminance Sobel stencil and edge policy. WARP contract fixtures compare flat, textured, and edge grids against a CPU oracle; acceptance remains unchanged for 10d.2.
D3D12 migration 10d.2 (2026-08-28): added a D3D12 pair-quality filter that combines the existing category mask with finite per-frame gradients and their established p90 limits. WARP fixtures verify independent frame limits, category preservation, and non-finite gradient rejection; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10e.1 (2026-08-28): added the SM 5.1 accepted-pair log-ratio pass. It retains the accepted mask and float ratio scratch in default-heap session resources for trimming, while the focused test reads back only its small oracle fixture. Accepted samples match the established `log(first / second)` CPU ratio; portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration 10e.2.1–10e.2.4 (2026-08-28): added a fully device-resident one-pair trimming path. An SM 5.1 initializer writes accepted ratios plus infinity sentinels into power-of-two session scratch, staged bitonic dispatches order it in place, exact integer-tenths rank arithmetic extracts CUDA-compatible 0.1/0.9 interpolated bounds, and an inclusive mask pass retains only in-range accepted samples for reduction. Odd/even accepted-count and exact-boundary fixtures match CPU oracles. MSVC AddressSanitizer also exposed and fixed a negative validation test that had zeroed the shared valid layout and silently skipped later loop assertions. Portable Debug/Release and Windows MSVC Release/WARP suites pass.
D3D12 migration audit repair (2026-08-28): reopened completion claims after reviewing Steps 1–10e. Step 1 now has an explicit regression proving a failure after CUDA dispatch begins propagates without restarting on CPU. The full current NVIDIA hardware suite passes (13 tests) on an RTX 5090 with driver 610.88.

D3D12 migration Step 4d NVIDIA acceptance (2026-08-28): product-mode D3D12 preflight admitted the current NVIDIA GeForce RTX 5090 (vendor `10de`, device `2b85`, LUID `14331`) with 33,750,515,712 dedicated bytes, 32,945,209,344 local-budget bytes, 11,796,480 usage bytes, and 32,933,412,864 usable bytes. The native contract executable now provides an opt-in `--hardware-probe-only` record; WARP CI behavior is unchanged. AMD and Intel acceptance are intentionally deferred until the remaining migration is complete.

D3D12 migration Step 6a repair (2026-08-28): bumped the synchronized native/ctypes ABI to 7 and made memory requests explicitly declare session workspace, per-pixel/fixed output workspace, upload, per-pixel/fixed readback, and descriptor requirements. Admission now aligns and sums all coexisting source, session, upload, retained-preview, output, and readback categories for both resident and banded modes, rejects under-accounted requests, and reports charged values. Portable Debug/Release CTests and Windows MSVC Release/WARP CTests pass.

D3D12 migration Step 10d.2 repair (2026-08-28): replaced caller-invented gradient thresholds with an SM 5.1 finite-gradient p90 operation. Each pair channel is padded with positive infinity, ordered independently by device bitonic passes, and evaluated with the CUDA-compatible `(count - 1) * 0.9` linear interpolation rule; only the final two limits are read back. The acceptance fixture now obtains its limits from D3D12 and compares them to a CPU oracle before filtering. Windows MSVC Release/WARP CTests pass without FXC warnings.

D3D12 migration Step 7f.6 output ownership (2026-08-28): bound empty-output creation/destruction in ctypes and added a closeable D3D12 output owner retained by its prepared session. Explicit child closure is idempotent; prepared-session closure drains every output before destroying session and device and refuses new children afterward. Focused adapter tests, Ruff, formatting, and mypy pass. Preview-child ownership remains deferred to the Step 12 native preview creation API rather than inventing an unbacked handle.

D3D12 migration Step 7f.8 (2026-08-28): completed one-shot native failure injection for the first and second source upload-slot allocations, encoding-metadata submission, source-upload fence signaling, and the existing upload-slot/finish wait cancellation points. Each fixture verifies unchanged partial accounting, retries safely, and reaches the suite's final zero device/queue/fence/session/output counters. A `BUILD_TESTING=OFF` production build, portable CTests, and Windows MSVC Release/WARP CTests pass.

D3D12 migration Step 10e.3 (2026-08-28): added a device-resident one-pair reducer. It derives valid/inlier counts and the inlier median from retained sorted ratios, builds and bitonically orders padded absolute deviations for MAD, and downloads one 32-byte packet containing rejection reason, counts, difference, MAD, and weight. Explicit insufficient-valid/nonfinite/excessive-dispersion reasons preserve the CUDA thresholds (24 valid, 12 inliers, MAD at most 0.5). Accepted and low-sample WARP fixtures match CPU scalar oracles; production, portable, and MSVC Release/WARP builds pass.

D3D12 migration Step 1 repair (2026-08-28): completed phase-specific CUDA cancellation coverage on the current RTX 5090. Upload now observes cancellation after the final per-frame callback and before synchronization; encoding checks cancellation before writing and again before atomic publication. Hardware regressions cancel during upload, compositing, conversion/download, and encoding, preserve an existing destination byte-for-byte, and leave no staged artifact. A separate dispatch-failure regression proves numerical failures never restart on CPU. All four cancellation cases pass on hardware.

CUDA baseline race repair (2026-08-28): the complete hardware gate exposed nondeterministic resident output caused by writing `log_gains` on CuPy's default stream and consuming it from the session's non-blocking compute stream. Gain allocation/upload now occurs on the compute stream. Forced-banded output again matches resident and CPU output exactly, and all 17 CUDA hardware tests pass on the RTX 5090.

D3D12 physical numerical acceptance (2026-08-28): the native contract executable now accepts `--hardware-full`, retains the product-selected physical device, and runs the complete numerical/lifecycle suite without switching to WARP. The full contract passes on the RTX 5090. Physical memory assertions compare stable identity and each snapshot's internal budget/usage arithmetic rather than incorrectly requiring usage to remain equal across separate DXGI queries.

D3D12 migration Step 10f.1.1 (2026-08-28): added versioned reduced-equation, scalar pair-report, and exposure-graph diagnostic layouts plus session-owned replacement-safe storage. Preparation reserves temporary equation/report vectors and swaps only after both succeed; injected allocation failure preserves the previous graph. One/two-frame, zero-capacity, invalid-capacity, replacement, clear-idempotence, and layout fixtures pass in production, portable, and Windows MSVC Release/WARP builds.

D3D12 migration Step 10f.1.2 (2026-08-28): added checked `uint32` pair-count planning and deterministic session enumeration in upper-triangle order (`left < right`). Reports begin in an explicit pending state rather than masquerading as numerical rejection, and caller-owned copies expose only scalar report records. Zero/one/two/many-frame order, overflow, byte-count, and empty-copy fixtures pass in production, portable, and Windows MSVC Release/WARP builds.
## 2026-08-28 — D3D12 migration 10f.1.3.1

- Split the resident one-pair chain into four independently gated implementation steps.
- Added session-owned, transactionally replaced device scratch for all fifteen projection-through-reduction intermediates.
- Added checked sample/sort-capacity byte accounting and diagnostics proving the scratch owns 15 device resources and zero readback bytes.
- Added focused lifecycle, exact-accounting, allocation-failure preservation, replacement, and idempotent-clear coverage.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.3.2

- Added a production projection-plus-proxy-sampling dispatch over retained one-pair device scratch.
- Kept coordinates, overlap, and both RGB sample buffers device-resident with no production upload/readback heap.
- Added a test-only readback hook that restores resource states after inspection.
- Verified retained coordinates, overlap, and all sampled channels against the established staged GPU oracles.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.3.3.1

- Split classification-through-ratio construction into three independently gated resident stages.
- Added a resident classifier that consumes the two device `float3` sample buffers directly and preserves the existing transfer-function clipping rule.
- Added an ordered production classification dispatch with no intermediate CPU transfer and a state-restoring test-only readback.
- Verified all retained luminance and candidate-mask values against the established staged GPU classifier.
- Verified portable Debug and Release CTest (3/3 each), production native build, FXC compilation without warnings, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.3.3.2

- Added an ordered production gradient, padded two-channel bitonic sort, and exact finite-p90 dispatch over retained device scratch.
- Kept luminance, gradients, sortable values, and the two percentile limits device-resident with no production readback.
- Added a state-restoring test-only gradient/limit readback.
- Verified every retained gradient and both limits against the established staged GPU oracles.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.3.3.3

- Added a resident-only filter shader that reads p90 limits directly from device memory.
- Reused the obsolete overlap buffer as filtered-mask output while retaining the classification categories separately.
- Added ordered production filter and log-ratio dispatches with no intermediate CPU transfer plus a state-restoring test hook.
- Verified the retained filtered mask and every ratio against the established staged GPU oracles.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.3.4.1

- Split resident trim/reduction into independently gated trim and scalar-reduction stages.
- Added production ratio preparation, in-place bitonic sort, percentile-bound extraction, and inlier-mask dispatches over retained scratch.
- Reused the obsolete classification-category buffer for the trimmed mask without increasing memory.
- Added a state-restoring test hook and verified sorted ratios, bounds, and every inlier flag against CPU/staged oracles.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.3.4.2

- Added retained summary, median/MAD deviation sort, and final-result dispatches using the existing reduction shaders.
- Kept every sample-sized reduction buffer on the device and allocated only one 32-byte readback packet.
- Verified rejection reason, valid/inlier counts, median difference, MAD, weight, and exact downloaded byte count against a CPU oracle.
- Completed the full resident one-pair projection-through-reduction chain.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).
## 2026-08-28 — D3D12 migration 10f.1.4

- Added transactional all-edge graph reduction using the proven resident one-pair scratch chain.
- Added retained-equation copying and committed only scalar pair reports/equations after every enumerated edge succeeds.
- Verified a rejected direct edge and a three-frame accepted graph containing all upper-triangle pairs `(0,1)`, `(0,2)`, `(1,2)`.
- Verified retained reports and equations match the direct scalar reduction and deterministic enumeration order.
- Verified portable Debug and Release CTest (3/3 each), production native build, and Windows MSVC Release CTest/WARP (4/4).

## 2026-08-28 — D3D12 migration 10f.2.1

- Reused the scalar pair report's reserved word for the geometric-overlap count and retained that
  count on-device while the resident classification mask is repurposed by later stages.
- Expanded scalar scratch by 16 bytes and carried the count in the existing 32-byte final reduction
  packet; production still performs no image-sized exposure readback.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 10f.2.2

- Added separately retained solve equations so measured reductions remain immutable.
- Reproduced CUDA's measured-edge transitive reachability and simultaneous lowest-index geometric
  bridge selection, retaining bridges as symmetric weight-1, zero-difference constraints.
- Verified empty single-frame, measured-chain plus bridge, and geometrically disconnected fixtures;
  measured weights remain unchanged and diagnostic edge counts match.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 10f.3

- Added a retained native exposure result with float log gains plus anchor, edge-count, and frame-count
  scalars.
- Reproduced CUDA's weighted double-precision Laplacian, frame-0 numerical anchor, pivoted
  Gauss-Jordan singular-row handling, median centering, `[-ln(2), ln(2)]` clamp, and first
  nearest-median anchor selection.
- Added single-frame, neutral-bridge, disconnected, and overdetermined weighted fixtures; the
  weighted fixture deliberately differs from weight-ignoring propagation.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 10g.1

- Added a one-shot session gain upload that converts retained clamped log gains to finite positive
  multiplicative gains and rejects duplicate upload.
- Bound those retained gains to the established hard-composite candidate constants without
  re-uploading source pixels.
- Verified a three-frame solved `(0.5, 1, 2)` gain fixture changes every selected output pixel by
  the correct frame gain while preserving selection weights and coverage.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 10g.2

- Added a versioned retained exposure report with anchor, edge count, frame count, gain-binding
  state, and monotonic solve/upload generation counters.
- Verified two consecutive preview/final-style session-gain compositions reuse the same completed
  solve and gain upload without changing either generation.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 10g.3

- Added explicit idempotent invalidation reasons for manual gains and exposure geometry.
- Manual-gain invalidation clears only bound multiplicative gains while retaining the solved report;
  geometry invalidation clears pair scratch, graph, solve, and report while preserving source and
  decoded exposure-proxy residency.
- Verified invalid bound-gain use is rejected, re-upload after manual invalidation succeeds, unknown
  reasons are rejected, and later output allocation/composition remains valid after geometry cleanup.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 11a.1

- Added a versioned 4096-bin `uint32` histogram layout with exact 16 KiB storage and checked
  maximum output population.
- Admitted populations through `UINT32_MAX` and rejected any larger job before allocation or
  dispatch, proving no single bin can overflow.
- Verified one-pixel, `65535²`, exact-maximum, zero, and first-overflow boundary fixtures in portable
  Debug/Release, production native, and Windows MSVC Release/WARP builds.

## 2026-08-28 — D3D12 migration 11c.1 prerequisite

- Implemented the CUDA-authoritative negative clamp and sRGB transfer as a reusable HLSL helper,
  with normalized output clamped to `[0, 1]`.
- Added a real D3D12 test dispatch covering negative, zero, transfer-breakpoint, one, highlight, and
  non-finite rejection cases against a double-precision oracle.
- Verified portable Debug and Release CTest (3/3 each), the production native build, FXC shader
  compilation, and Windows MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 11a.2

- Added a 4096-bin `uint32` D3D12 histogram shader that reuses the verified sRGB transfer helper,
  excludes uncovered and non-finite RGB, clamps encoded luminance, and uses SM 5.1 atomics.
- Added a bounded one-band clear/dispatch/readback harness and verified empty, sparse with a covered
  NaN, and full finite histograms against every CPU-oracle bin.
- Verified portable Debug and Release CTest (3/3 each), the production native build, FXC shader
  compilation, and Windows MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 11a.3

- Added output-job ownership for one fixed histogram, with one-shot clear and retained accumulation
  diagnostics.
- Added production accumulation from current resident/banded linear and coverage buffers, enforcing
  sequential non-overlapping bands, restoring both inputs to `COPY_DEST`, and never clearing between
  bands.
- Verified exact resident 4×4 and two-band 2×64 histograms, one clear, exact band/pixel counts, and
  duplicate accumulation rejection on WARP.
- Verified portable Debug and Release CTest (3/3 each), the production native build, and Windows
  MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 11b.1

- Added a warning-clean one-thread selector over the retained `uint32` histogram, reproducing
  CUDA's 0.5%/99.5% ranks, within-bin interpolation, empty `(0,1)`, and flat `(0,0)` sentinel.
- Retained the two-float levels resource on the output job and downloaded only its eight scalar
  bytes for the report.
- Verified empty, flat, and two-level percentile fixtures plus duplicate-selection rejection in
  portable Debug/Release, production native, FXC, and Windows MSVC Release/WARP gates.

## 2026-08-28 — D3D12 migration 11b.2

- Added output-job-owned normalized-sRGB storage and an encoded-space auto-contrast application
  shader consuming retained levels.
- Preserved CUDA behavior: apply `(value - black) / (white - black)` only when enabled and
  `white > black`; disabled and flat-sentinel paths leave transferred values unchanged.
- Restored the linear band to `COPY_DEST`, reused bounded normalized storage, and read back only the
  current band with `CopyBufferRegion`.
- Verified enabled/disabled two-level output against CPU formulas in portable Debug/Release,
  production native, warning-clean FXC, and Windows MSVC Release/WARP gates.

## 2026-08-28 — D3D12 migration 11c.2

- Added output-job-owned 8-bit sRGB storage and a bounded quantization dispatch over the current
  resident or banded normalized-sRGB buffer.
- Matched CUDA/NumPy nearest-even conversion explicitly, including saturation before scaling, and
  restored reusable input/output resource states after dispatch and test readback.
- Verified every resident channel exactly and every banded channel within the planned one-code
  tolerance in portable Debug/Release, production native, warning-clean FXC, and Windows MSVC
  Release/WARP gates.

## 2026-08-28 — D3D12 migration 11d.1

- Added an output-job-owned Rec.2020 tone-mapped linear intermediate bounded to the resident or
  current band allocation.
- Preserved CUDA's exact operation order: per-channel negative clamp, explicit
  `10000/reference_white_nits` scaling, Rec.2020 luminance, and shared `L/(1+L)` scale.
- Verified invalid reference white plus neutral, negative-channel, and unequal-highlight fixtures
  against the CUDA-authoritative CPU formula in portable Debug/Release, production native,
  warning-clean FXC, and Windows MSVC Release/WARP gates.

## 2026-08-28 — D3D12 migration 11d.2

- Added the CUDA-authoritative Rec.2020-to-linear-sRGB matrix as an isolated reusable HLSL helper
  and output-job dispatch over the retained tone-mapped intermediate.
- Retained a bounded linear-sRGB intermediate, rejected conversion before its matching active-band
  prerequisite, and restored both resources for reuse.
- Verified every matrix channel for negative-clamped, neutral, and unequal-highlight fixtures in
  portable Debug/Release, production native, warning-clean FXC, and Windows MSVC Release/WARP
  gates.

## 2026-08-28 — D3D12 migration 11d.3

- Reused the proven auto-contrast/sRGB shader implementation with the retained converted
  linear-sRGB input, preserving the original linear-sRGB entry point and resource states.
- Fed the result through the existing nearest-even 8-bit quantizer without introducing a PQ-only
  transfer or quantization variant.
- Verified negative gamut excursions, neutral pixels, and saturated highlights end-to-end within
  one code value in portable Debug/Release, production native, and Windows MSVC Release/WARP
  gates.

## 2026-08-28 — D3D12 migration 11e.1–11e.2

- Added an output-job-owned float output buffer and exact active-band `CopyBufferRegion` path,
  preserving values above one without SDR conversion or arithmetic.
- Matched the current CUDA/EXR policy for non-finite composed values: NaN and positive/negative
  infinity are preserved byte-for-byte rather than clamped or rejected at download time.
- Verified exact resident and banded float copies, including above-one and non-finite fixtures, in
  portable Debug/Release, production native, and Windows MSVC Release/WARP gates.

## 2026-08-28 — D3D12 migration 11f.1–11f.3

- Added versioned caller-owned download layouts and separate integer/float production entry points
  that reject wrong width, row range, or byte count before submission.
- Added one output-job-owned, 64 KiB-aligned readback buffer reused across formats and bands, with
  cancellation checks before submission and after fence completion.
- Added transfer diagnostics and verified cancelled calls leave destinations/counters unchanged,
  while successful integer and float calls produce exact payload counts and no disk scratch.
- Verified portable Debug/Release, production native, and Windows MSVC Release/WARP gates.

## 2026-08-28 — D3D12 migration 12a

- Replaced the preview destroy stub with a session-retaining opaque preview owner for RGB8 full
  preview, overview, and compact per-pose masks.
- Added exact overflow-safe creation layouts, all-or-nothing publication, idempotent destruction,
  retained-byte/dimension/live-count diagnostics, and shader-readable resource states.
- Verified malformed creation leaves a null handle and byte-for-byte retained buffers return from
  WARP, with portable Debug/Release and production native gates also passing.

## 2026-08-28 — D3D12 migration 12b

- Added fixed-viewport overview/crop rendering over retained RGB8 buffers with a versioned,
  caller-owned request and reusable viewport/readback allocations.
- Preserved CUDA's exact overview pixels and integer crop coordinates; viewport-sized crops at the
  exact lower/right boundary succeed while oversized or out-of-range crops fail before dispatch.
- Verified overview, center crop, boundary crop, and untouched rejection destinations in portable
  Debug/Release, production native, warning-clean FXC, and Windows MSVC Release/WARP gates.

## 2026-08-28 — D3D12 migration 12c–12e

- Ported CUDA's single preview-overlay kernel over retained RGB8 images and compact masks, mapping
  mask coordinates directly instead of retaining CUDA's expanded-mask cache.
- Preserved hover tint/outline, target color and target-mode behavior, boundary composition,
  horizontal wrap, and overlay priority.
- Verified no-overlay identity, hovered/uncovered pixels, target hover, and boundary-only target
  fixtures in portable Debug/Release, production native, warning-clean FXC, and Windows MSVC
  Release/WARP gates.

## 2026-08-28 — D3D12 migration 12f

- Added monotonic preview generations, pre-submit/post-fence stale rejection, token cancellation,
  and atomic concurrent-render rejection without moving Tk ownership into native code.
- Retained the existing GUI worker's one-pending-request coalescing and event-generation publication
  filter for Step 13 binding.
- Ran the full native contract on the installed NVIDIA GeForce RTX 5090; 31 generation-aware
  preview requests measured 0.851 ms median and 1.324 ms p95 request-to-caller-buffer latency.

## 2026-08-28 — D3D12 migration 13a

- Replaced neutral product selection's temporary CUDA translation with native D3D12 adapter probe
  and checked memory admission; explicit CUDA selection remains available only to oracle tests.
- Preserved strict-GPU errors and pre-dispatch CPU fallback, returned the existing transitional
  plan shape without changing render routing, and conservatively charged the peak conversion chain
  plus committed-resource alignment.
- Verified mocked D3D12 selection/fallback/strict behavior, no CUDA probe invocation, full Python
  Ruff/format/mypy gates, and 137 passing tests; physical NVIDIA native probe/admission remained
  covered by the RTX 5090 hardware-full contract.

## 2026-08-28 — D3D12 migration 13b.1

- Promoted ordered hard composition to a production output-job entry point for the session's
  source type and frame count, replacing fixed three-frame resource-owner arrays with bounded
  dynamic ownership.
- Restored ping-pong accumulators to UAV state before their next write, enabling the fourth and
  subsequent ordered passes without changing strict-greater winner or tie arithmetic.
- Switched existing resident and repeated-band fixtures to the production entry point and added a
  four-frame WARP regression checked against independently dispatched per-frame candidates.
- Verified portable Debug/Release native contracts and Windows MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 13b.2a

- Generalized the existing feather accumulation chain from fixed two/three-frame resource arrays
  to any positive frame count with allocation-safe dynamic resource ownership.
- Restored reused ping-pong accumulation targets from SRV to UAV state before the third and later
  ordered writes, preserving sequential floating-point addition.
- Added an array-form test hook and explicit one-/four-frame regressions alongside the existing
  two-/three-frame fixtures; the four-frame case verifies order-sensitive arithmetic.
- Verified portable Debug/Release contracts, the production native build, and Windows MSVC
  Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 13b.2b

- Added an output-only feather normalization shader that writes normalized linear RGB and exact
  byte coverage while preserving the standalone zero-weight RGB rule.
- Kept final feather accumulators on-device and optionally normalized them directly into reusable
  resident/banded output storage; legacy test calls retain their independent readback behavior.
- Enabled UAV access on output-owned linear/coverage buffers without changing allocation sizes or
  their established idle `COPY_DEST` state.
- Verified resident and both repeated 32-row band positions against standalone accumulation and
  normalization fixtures in portable Debug/Release, production native, warning-clean FXC, and
  Windows MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 13b.2c

- Promoted retained-source feather composition behind a production output-job entry point while
  sharing the proven typed candidate generation, band validation, and exposure hooks with hard
  composition.
- Added GPU-only feather-weight, ordered accumulation, and output normalization/coverage branches;
  no image-sized candidate or accumulator crosses to CPU.
- Verified three-frame UINT8/UINT16/FLOAT32 output against independent candidate-weight oracles,
  four-frame UINT8 ordered composition, and full-resident versus two reused 32-row bands.
- Passed portable Debug/Release contracts, production native, MSVC `/W4 /WX`, warning-clean FXC,
  and Windows Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 13b.3

- Added versioned production composite inputs with mutually exclusive identity, explicit
  manual/combined gains, or already-uploaded retained session gains plus optional packed local
  fields and explicit incomplete-magenta behavior.
- Applied the same gain/local-field inputs in shared candidate generation for hard and feather
  modes; retained automatic gains match equivalent explicit gains without another solve/upload.
- Added an in-place output-owned incomplete shader over byte coverage, preserving coverage while
  writing magenta only for uncovered pixels and avoiding another image-sized RGB allocation.
- Verified identity/manual/local/retained and complete/incomplete fixtures for both blend modes in
  portable Debug/Release, production native, warning-clean FXC, and Windows MSVC Release/WARP
  CTest (4/4).

## 2026-08-28 — D3D12 migration 13b.4

- Declared the complete native preparation ABI in ctypes: device/session construction, retained
  rotations and optional metadata, source allocation, exactly two upload slots, frame-zero and
  cancellable later uploads, cancellable finish, and pointer-to-handle destruction.
- Added a transactional prepared-session factory that publishes ownership only after uploads finish
  and otherwise destroys session before device; one native cancellation token spans all waits.
- Verified exact structure/declaration layouts, upload order/indices/bytes, token propagation,
  cancellation translation, idempotent owner close, and injected mid-upload reverse cleanup with
  12 focused tests, Ruff/format, and mypy. Native two-slot/WARP behavior remains covered by CTest.

## 2026-08-28 — D3D12 migration 13b.5

- Bound the retained native exposure proxy, pair reduction, solve, gain-upload, and scalar-report
  workflow to the prepared ctypes session without downloading image-sized intermediates.
- Cached the completed scalar report so repeated consumers reuse the single retained solve and
  gain upload; failures before upload remain retryable and preserve orderly session teardown.
- Verified exact ctypes declarations, native call order and geometry/FOV inputs, one-upload reuse,
  and reduction-failure behavior with 14 focused adapter tests; Ruff/format and mypy pass.

## 2026-08-28 — D3D12 migration 13b.6

- Bound transactional resident/banded output creation, hard/feather composition with retained or
  explicit exposure inputs, auto-contrast operations, SDR/PQ/float conversions, and cancellable
  downloads to exact versioned ctypes layouts.
- Kept output children owned by their prepared session, destroyed unpublished partial allocations,
  and rejected readonly, non-contiguous, or incorrectly sized download buffers before submission.
- Verified declarations, allocation order/cleanup, composition inputs, all conversion families,
  exact download ranges/bytes, and cancellation-token forwarding with 17 focused adapter tests;
  Ruff/format and mypy pass.

## 2026-08-28 — D3D12 migration 13b.7

- Switched final product selection from the legacy CUDA selector to the neutral D3D12 admission
  path and routed resident, non-auto-contrast PNG/JPEG/EXR outputs through native composition.
- Streamed decoded source frames into the native two-slot uploader, preserved staged publication
  and existing encoders, applied explicit sRGB/PQ/linear conversions, and enforced complete
  coverage through the fixed-size native histogram reduction.
- Kept CUDA reachable only through explicit oracle-test injection, added PNG/JPEG-PQ/EXR-linear
  resident routing fixtures, and fixed 3x3 rotation marshaling and preflight token cleanup found by
  them. Ruff/format, mypy, 85 focused Python tests, portable Debug/Release contracts, and Windows
  MSVC Release/WARP CTest (4/4) pass.

## 2026-08-28 — D3D12 migration 13b.8

- Generalized final D3D12 output jobs over resident or memory-planned band rows using the shared
  adaptive watchdog scheduler and exact completed-band downloads into the existing host output.
- Added the required two-pass SDR auto-contrast route: contiguous histogram bands and retained
  levels first, followed by recomposition, transfer-specific conversion, and staged publication.
- Kept non-auto renders single-pass, used the same fixed histogram reduction for strict coverage,
  and checked cancellation before every composition and download boundary.
- Verified resident/banded call counts, one-/two-pass behavior, PNG/JPEG-PQ/EXR conversion routes,
  exact download counts, Ruff/format, mypy, and 69 focused adapter/compositor tests; the underlying
  native operations pass portable Debug/Release and Windows MSVC Release/WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 13b.9

- Enforced the product fallback boundary around D3D12 final rendering: adapter/session/output
  failures before the first composition may restart once on CPU, while strict mode propagates.
- Kept numerical and later conversion/download failures outside the fallback handler so a native
  render can never double-render or publish results from a restarted CPU path.
- Verified non-strict fallback, strict rejection, and injected post-dispatch propagation with 72
  focused adapter/compositor tests; Ruff/format and mypy pass.

## 2026-08-28 — D3D12 migration 13c

- Bound exact native preview creation, session-retaining ownership, base crop/overview rendering,
  cancellation forwarding, and child-before-parent destruction in ctypes.
- Switched product preview selection to the neutral D3D12 path and returned completed SDR preview
  pixels directly from native output downloads without a temporary preview file or Tk ownership.
- Kept CUDA available only through explicit oracle injection and verified retained byte counts,
  crop metadata, cleanup order, and D3D12 in-memory routing with 74 focused tests; Ruff/format and
  mypy pass. Native base-preview pixels remain covered by Windows WARP CTest (4/4).

## 2026-08-28 — D3D12 migration 13d

- Bound generation-aware preview overlays and added a D3D12 display object matching the GUI
  worker's existing render/close interface without moving image or Tk work onto the main thread.
- Extended the single-entry session cache with immutable D3D12 adapter-LUID/ABI identity,
  retained preview ownership, generation/hover/target requests, and child-before-parent cleanup.
- Extracted streaming session preparation so preview and final output reuse one native upload and
  one retained exposure solve; cache misses prepare transactionally and failures invalidate.
- Verified generation advancement, base/overlay selection, hovered/target propagation, cache hit
  and cleanup behavior, plus the full Python gate: Ruff/format, mypy, and 157 passing tests.

## 2026-08-28 — D3D12 migration 13.5

- Added a Windows `gpu` PyInstaller archive that embeds exactly one Release `pano_gpu.dll` beside
  the frozen ctypes adapter, while the CPU and transitional CUDA-oracle archives contain no native
  D3D12 DLL.
- Made the release workflow configure, build, and test the native backend before packaging, then
  run a non-GUI frozen ABI-load probe before publishing the GPU archive. A selected stitcher flavor
  can now be built without an unrelated ReShade add-on artifact.
- Kept the packaged GUI's existing GPU checkbox enabled by the `gpu` flavor so preview selects the
  D3D12 product path by default; CUDA remains reachable only through explicit test-oracle injection.
- Built `PanoramaCapture-Stitcher-1.0.4-gpu-win-x64.zip` on Windows with MSVC 19.51.36256.0. Native
  Release/WARP CTest passed 4/4, the frozen ABI probe passed, and the archive is ready for the manual
  NVIDIA preview/status smoke test.
- The first NVIDIA GUI smoke test reached D3D12 admission and exposed an unaligned Python session
  workspace estimate. Aligned that estimate to the native allocator's 64 KiB committed-resource
  boundary, added an exact request regression assertion, and rebuilt the GPU archive with native
  Release/WARP CTest 4/4 and the frozen ABI probe passing. The same smoke test confirmed CPU
  fallback preview still works.
- The next NVIDIA smoke test completed native session upload and cache insertion, then exposed the
  Python adaptive scheduler incorrectly subdividing a resident output at 1024 rows. Resident output
  now emits one exact full-height composition request while only banded outputs use the adaptive
  scheduler. A 2048-row regression covers the native row-range contract; the full Python gate,
  Windows Release/WARP CTest 4/4, frozen ABI probe, and rebuilt GPU archive pass.
- The first rendered NVIDIA preview and interactive crop succeeded but revealed severe SDR
  overexposure. Native candidate shaders had normalized encoded samples without applying the
  session's explicit transfer function, then the correct output stage encoded those values to sRGB
  a second time. All uint8/uint16/float candidate paths now decode sRGB/PQ/linear per texel before
  interpolation and gains, matching the authoritative CUDA pipeline without inferring from sample
  type. The WARP sRGB candidate oracle now expects decoded-linear values.
- Audited the remaining CUDA/D3D12 color sequence: projection, per-texel decode, bilinear sampling,
  linear gain, hard/feather weighting and normalization, PQ tone mapping, Rec.2020 conversion,
  auto-contrast, and quantization are ordered equivalently. The 17-test CUDA fixture suite passed,
  but a direct WSL device render reported an insufficient CUDA driver/runtime pairing and therefore
  was not physical-device evidence. Windows CUDA-oracle packaging completed for an explicit native
  NVIDIA comparison.
- A same-session Windows NVIDIA oracle isolated the remaining PQ preview overexposure to D3D12
  auto-contrast: non-auto CUDA/D3D12 output differed by at most one code value, but D3D12 built its
  histogram from decoded PQ Rec.2020 before the SDR tone-map/gamut conversion. Bumped the synchronized
  native/ctypes ABI to 8, added converted-linear-sRGB histogram accumulation, and routed PQ histogram
  passes through the same tone-map and Rec.2020-to-sRGB sequence as CUDA. The final 4184x2092 CUDA and
  D3D12 previews have channel means `[66.4032, 52.8418, 32.7798]` on both paths; 99% of channel deltas
  are zero and the maximum delta is one. Ruff/format, mypy, all 158 Python tests, portable
  Debug/Release CTest, and Windows Release/WARP CTest pass. Rebuilt the 71 MiB ABI-8 GPU GUI archive
  at `C:\dev\panorama-taker-gui-dist\PanoramaCapture-Stitcher-1.0.4-gpu-win-x64.zip`.

## 2026-08-28 — D3D12 migration 14a.1 and 14d.2–14d.3

- Replaced the optimistic final-output admission estimate with a conservative peak that charges
  per-frame hard/feather candidate resources, retained output/conversion buffers, alignment, and
  descriptors. The real 17552x8776 session now selects a 1024-row band instead of admitting an
  impossible resident allocation; a strict 512-wide NVIDIA D3D12 render succeeds.
- Routed the optional session thumbnail through a second rectilinear render on the retained D3D12
  session, preserving the established 90-degree horizontal thumbnail projection. Panorama and
  thumbnail are staged and published only after both renders complete; post-dispatch thumbnail
  failures preserve existing outputs, clean staging files, and never restart on CPU.
- Session `1787897185-2` rendered both artifacts on the current NVIDIA device without CPU fallback:
  `C:\dev\panorama-step14-output\session-1787897185-2-thumbnail-test.png` (261513 bytes) and its
  3840x2160 thumbnail (12054341 bytes). Ruff/format, mypy, and 82 focused compositor/GPU tests pass.

## 2026-08-28 — D3D12 migration 14d.1 and ABI 9

- Added the versioned native `pano_gpu_output_download_coverage` operation and synchronized ctypes
  ABI 9. It enforces the same exact completed-band, cancellation, reusable-readback, and transfer
  diagnostic contracts as color downloads; the WARP oracle verifies coverage byte-for-byte.
- Removed the D3D12 coverage fallback and stream each completed native coverage band through the
  existing bounded grayscale PNG writer. Final, coverage, and thumbnail files remain staged; the
  main panorama is published last as the successful-render commit marker.
- The authorized real session rendered panorama (261513 bytes), 3840x2160 thumbnail (12054341
  bytes), and coverage PNG (512 bytes) on the current NVIDIA device with no CPU fallback, under
  `C:\dev\panorama-step14-output\session-1787897185-2-coverage-test*`.
- Full Python verification passes: Ruff, format, mypy, 143 tests passed and 17 CUDA-runtime tests
  skipped. Portable Debug/Release native contracts pass; Windows MSVC Release builds cleanly and
  all four CTest/WARP tests pass.

## 2026-08-28 — D3D12 migration 14 GUI/manual hardening follow-up

- Fixed the native band-row contract exposed by the full-size GUI render. Python had reused the
  adaptive CUDA watchdog scheduler after allocating a fixed 1024-row native output, so its first
  256-row request was correctly rejected. D3D12 now submits exactly the planned row count and only
  shortens the final remainder; a 2500-row regression verifies 1024/1024/452 requests.
- Kept the retained preview alive while final rendering starts, preserve it after final-render
  failure, restore controls before showing the error dialog, immediately restore the overview on
  pointer leave, and grow (never shrink) the window when the completed overview is installed.
- Added focused tests for preview ownership, hover restoration, window sizing, failure-state
  recovery, and fixed native bands. The full Python gate passes with 148 tests and 17 intentional
  CUDA-runtime skips; Ruff, format, and mypy pass.
- Real-session 4096x2048 and 8192x4096 NVIDIA D3D12 resident renders succeeded. Artificial 3 GiB
  and 5 GiB caps were rejected during admission because they are below the retained-session plus
  reserve floor; no numerical dispatch occurred. Native-size manual validation remains the direct
  proof of the repaired 1024-row product path.
- A subsequent native-size manual render proved the error persisted after the Python fixed-band
  change. The native output retained its initial band-zero range forever; only test hooks had ever
  advanced `band_row_start`/`band_row_count`, masking the production defect. Production composition
  now validates and selects each planned band, accepts the shortened final band, and permits a
  validated rewind to row zero for auto-contrast's second pass. The WARP oracle composes the later
  band first and rewinds without either test-only setter.
- Final-render failure now fully discards the invalidated retained preview and resets progress after
  enabling controls, while active rendering itself leaves the preview visible. Full Python checks
  still pass (148 passed, 17 CUDA-runtime skips); MSVC Release/WARP CTest passes 4/4. The approved
  in-place `r2` GPU archive was rebuilt, frozen-probed, and its temporary release tree removed.
- Manual NVIDIA GUI validation now confirms correct full-size panorama output for hard and feather
  blending, with and without thumbnail, with and without coverage, and confirms preview exposure
  corrections carry through to the full render. D3D12 is marginally slower than the CUDA oracle in
  this session but completes correctly.
- The generated thumbnail was still a squeezed equirectangular panorama. Native code had copied the
  integer bit pattern `1` into an HLSL `float` projection flag, producing a denormal that the GPU
  flushed to zero. All candidate/local-exposure marshaling sites now provide actual `1.0f/0.0f`
  constants. The strengthened WARP oracle requires rectilinear and equirectangular candidate and
  validity fields to differ; Windows CTest passes 4/4. The same approved `r2` archive was rebuilt in
  place, frozen-probed, and cleaned for thumbnail revalidation.
- Manual validation of the rebuilt archive confirms the session thumbnail now uses the intended
  centered 90-degree rectilinear projection rather than squeezing the equirectangular panorama.

## 2026-08-29 — D3D12 migration 14a.2

- Injected the identical `cannot create D3D12 hard-composite frame buffers` failure on each side of
  the first numerical-dispatch boundary. Preflight restarts once on CPU in non-strict mode;
  post-dispatch propagates and never double-renders, independent of matching error wording.
- Ruff/format and all 61 focused compositor tests pass.
## 2026-08-29 — D3D12 migration 14a.3

- Added test-only one-shot failure injection for device, pipeline, descriptor, resource, and retained-preview construction; existing session and output allocation hooks complete the planned construction boundary set.
- The compact device self-test owns pipeline, descriptor, and resource failure coverage, avoiding production-path refactoring while proving local COM cleanup and one-shot recovery.
- Added native contract assertions for null failed out-handles, preserved live-parent counts, and successful operation immediately after each injected failure.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed.
## 2026-08-29 — D3D12 migration 14a.4

- Added test-only retained-preview live-count observability without changing the production ABI.
- Verified failed preview construction leaves no live preview, repeated preview destruction returns the count to zero, transient pipeline/resource/descriptor failures preserve only the live parent device, and existing child-before-parent ownership tests release device, queue, fence, session, and output counts to zero.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed.
## 2026-08-29 — D3D12 migration 14b.1

- Audited the native upload failure matrix: source, upload-slot, second-slot, and encoding-metadata allocation; metadata-upload rejection; alternating slot reuse; upload-fence signal failure; cancellation after slot wait; and cancellation after upload-finish wait all have focused contract assertions.
- Existing assertions prove failed attempts do not advance upload counts or overwrite completed frames and that retry succeeds. D3D12 queue execution itself is void; the following fence signal is the first reportable submission failure.
- Reverified the matrix in the Windows x64 MSVC Release CTest/WARP gate: 4/4 passed.
## 2026-08-29 — D3D12 migration 14b.2

- Added test-only one-shot failures immediately before and after the production `*_with_inputs` numerical compositor used by the Python D3D12 adapter.
- Native errors identify whether the failure occurred before or after the first D3D12 numerical dispatch; focused WARP assertions prove each hook is one-shot and the output remains reusable.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed.
## 2026-08-29 — D3D12 migration 14b.3

- Added test-only one-shot failures for production output-download readback allocation, pre-submit rejection, post-fence-wait failure, and readback mapping.
- Each native failure reports its phase, leaves the caller's destination buffer untouched, does not advance transfer accounting, and permits the following real download to succeed.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed.
## 2026-08-29 — D3D12 migration 14b.4

- Added WARP-safe device-removal injection before and after the production numerical compose boundary. Errors retain both `DXGI_ERROR_DEVICE_REMOVED` (`0x887a0005`) and the injected removal reason (`0x887a0007`) plus the dispatch phase.
- Extended the compositor regression matrix to prove both allocation and device-removal messages fall back to CPU only when raised as pre-dispatch failures; post-dispatch failures always propagate without double-rendering.
- Verified focused Python ruff, format, and compositor tests: 63 passed.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed.
## 2026-08-29 — D3D12 migration 14c.1

- Audited existing cancellation checkpoints: pre-upload, post-slot-wait, post-upload-finish-wait, pre-download, post-download-fence-wait, and pre-next-band cancellation all have focused native or adapter regressions.
- The tests preserve completed frame/upload accounting, leave cancelled download destinations untouched, close output jobs, and prevent the next band submission.
- Verified focused Python D3D12 adapter ruff, format, and tests: 20 passed.
- Reused today's Windows x64 MSVC Release CTest/WARP result covering native checkpoints: 4/4 passed.
## 2026-08-29 — D3D12 migration 14c.2

- Added a shared test-only fence-timeout injection point that waits for the real GPU fence before reporting the operation-specific timeout, avoiding in-flight resource lifetime hazards.
- The output-download regression bounds the injected path below five seconds, preserves the destination sentinel, and proves the same output remains reusable on the following operations.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed in 2.02 seconds total.
## 2026-08-29 — D3D12 migration 14c.3

- Added test-only retained-preview guard claim/release helpers to deterministically exercise the real concurrent-render rejection branch.
- WARP coverage now proves stale generation and cancellation leave the destination sentinel untouched, a concurrent call is rejected without publishing pixels, and five successive generations render successfully after the guard is released. The physical-adapter latency loop remains 31 renders.
- Verified Linux Release native build and CTest: 3/3 passed.
- Verified Windows x64 MSVC Release build and CTest/WARP: 4/4 passed.
## 2026-08-29 — D3D12 migration 14c.4

- Added an exactly-once GUI shutdown latch. Closing during active work requests cancellation once and waits; after the worker exits, repeated close calls can no longer repeat preview cleanup, session-cache close, settings save, or root destruction.
- Added a focused lifecycle regression that verifies preview cleanup precedes cache/session cleanup and the full finish sequence runs once. Existing adapter tests cover output/preview children closing before their prepared session/device parent.
- Verified focused GUI ruff and format checks, GUI cache tests: 22 passed, and mypy: no issues in 12 source files.
## 2026-08-29 — D3D12 migration 14d.3

- Replaced sequential D3D12 panorama/coverage/thumbnail publication with a rollback transaction: existing destinations move to unique backups, staged files publish as a set, and any failure removes new publications and restores old files in reverse order.
- Failed backup restoration is left recoverable on disk rather than deleted. Non-preflight D3D12/runtime/publication failures now invalidate the retained session cache before propagating.
- Added coverage-, thumbnail-, and panorama-publication failure cases; each preserves all three existing outputs, removes staged debris, forbids CPU double-rendering, and records one cache invalidation.
- Verified focused compositor ruff and format checks, compositor tests: 66 passed, and mypy: no issues in 12 source files.
## 2026-08-29 — D3D12 migration 14d.4

- Added cancellation coverage after panorama, coverage, and thumbnail staging but before publication. Every existing output remains byte-for-byte unchanged and all `.partial` stages are removed.
- D3D12 cancellation now invalidates retained cache state as `render cancelled`; other propagated runtime/publication failures use `render failed`. Existing cache-display tests verify preview then token then prepared-session teardown.
- Together with the successful three-output publication and 14d.3 failure matrix, this completes Step 14 staged-file/cache cleanup.
- Verified focused compositor ruff and format checks, compositor tests: 67 passed, and mypy: no issues in 12 source files.
## 2026-08-29 — D3D12 migration 15a

- Promoted the shared memory plan, band scheduler, preflight error, session-cache key/owner, cache-key builder, preview-display fields, and public render arguments to real backend-neutral `Gpu*`/`gpu_*` definitions; removed all temporary `Gpu* = Cuda*` aliases.
- Removed the CUDA backend literal and hidden CUDA branches from product `render_session`/`render_preview` orchestration. Width scaling and pre/post-dispatch fallback behavioral tests now run through D3D12 with unchanged assertions.
- The explicit CUDA oracle implementation remains directly callable only for the implementation-test replacement/removal steps 15b–15d.
- Verified the full Python gate: ruff and format clean, mypy clean, 157 passed and 17 intentional CUDA-oracle skips.

## 2026-08-29 — D3D12 migration 15b–15d

- Replaced implementation-specific kernel assertions with fixed HLSL source/entry-point hashes for
  candidate projection, exposure projection/classification, hard selection, feather accumulation,
  and retained-preview overlay. Native WARP contracts and adapter/compositor failure matrices now
  own driver, allocation, lifetime, banding, output, exposure, cancellation, and retry coverage.
- Replaced the three-flavor release contract with one cross-vendor archive containing exactly one
  `pano_gpu.dll` and one frozen D3D12 ABI-load probe. CI now runs the complete Python suite rather
  than excluding a hardware-oracle module.
- Removed the optional vendor-compute dependencies and regenerated `uv.lock`; eleven CuPy/vendor
  runtime packages were removed. Removed frozen-runtime probes, collection hooks, archive flavor
  switches, environment switches, and release-workflow flavor assumptions.
- Deleted the dormant CUDA kernel and Python implementation, its direct tests, and the unreachable
  compositor path. The surviving GPU module contains only neutral scheduling/accounting and D3D12
  admission; the benchmark now compares strict D3D12 with CPU.
- Updated README and CLI text to the single archive and D3D12 backend. A focused current-source
  search contains no CUDA, CuPy, NVRTC, or CUDA-runtime names.
- Verified the full Python gate: Ruff lint and format clean, mypy clean, 145 passed. Regenerated the
  lock with normal `uv lock`; package metadata and lock contain no vendor-compute dependency.
- Verified a clean temporary install from current project metadata: eleven ordinary runtime
  packages plus `pano-stitch`, with no vendor compute runtime; removed the temporary environment.
- Verified Linux native CTest 3/3 and the existing Windows x64 MSVC Release/WARP gate 4/4.

## 2026-08-29 — D3D12 migration 16a–16d

- Built the single transitional `PanoramaCapture-Stitcher-1.0.4-win-x64.zip` with the Python GUI,
  ABI-9 native DLL, and embedded shaders. Fixed Python inputs, MSVC `/Brepro`, sorted entries, and
  fixed ZIP timestamps made two clean builds byte-identical at SHA-256 `20878f3155f86c5c55174b4c8409fa449aad0740318a7e268f0b1abf23756c7a`.
- Added `--verify-gpu-runtime` with stable exits 0 verified, 2 unavailable, and 3 failure. The command
  validates ABI, product hardware admission, pipeline creation, dispatch, readback, and cleanup.
  Release probes now synchronously wait for the windowed executable; this fixed premature cleanup
  and stale-exit behavior exposed by the extracted-path audit.
- Added an automated archive audit for paths containing spaces, payload/runtime/compiler rejection,
  PE dependencies, compressed/extracted sizes, archive/EXE/DLL hashes, all shader-source hashes,
  physical adapter details, and a corrupted-DLL exit-3 check. The final RTX 5090 audit passed.
- Ran the authorized real 30-frame 4K SDR PNG session on the RTX 5090 through strict D3D12:
  resident preview, hard panorama, projected thumbnail, coverage, feather JPEG, linear EXR, and a
  4096-wide forced 32-row-banded panorama passed. Cancellation after the first source upload
  published no output and left no partial. Temporary outputs and verifier copies were removed.
- The available Windows 11/NVIDIA run is the approved physical-hardware completion gate for Step
  16. Windows 10, AMD, Intel, CPU-only clean-machine, known JPEG/PQ/EXR input-session, and
  per-target GUI checks are retained as a non-blocking deferred checklist in
  `docs/d3d12-stitcher-acceptance.md` for completion when suitable systems and sessions become
  available.
