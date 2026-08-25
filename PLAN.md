# Cyberpunk 2077 Panorama Taker — Implementation Plan

## Objective

Build three cooperating pieces of software:

1. `PanoramaCapture`: a Cyberpunk 2077 mod that positions the camera at precise, repeatable angles and coordinates screenshot capture.
2. `PanoramaCapture.ReShade`: a small ReShade add-on that requests screenshots through ReShade and reports completed screenshot paths.
3. `pano-stitch`: a command-line script that uses the captured images and their metadata to produce an equirectangular panorama without discarding source dynamic range. Use OpenEXR for the HDR proof of concept and provide PNG output in the final release.

The repository began as a new project; the packet status sections below identify the accepted implementation slices.

## Scope clarification

A yaw-only sequence produces a 360-degree horizontal band, not a complete spherical panorama. A complete 2:1 equirectangular panorama requires multiple pitch rows covering both poles.

Support two modes:

- `horizontal`: yaw-only, producing a cropped 360-degree equirectangular band.
- `full_sphere`: yaw and pitch coverage from -90 to +90 degrees, producing a standard 2:1 equirectangular image.

Use `horizontal` only as a camera-control and stitching diagnostic. The project MVP is `full_sphere`; a yaw-only band does not satisfy the objective.

## Recommended architecture

### PanoramaCapture mod

Implement the production controller in CET Lua using the verified first-person player/FPP camera path outside Photo Mode. This path already changes yaw and pitch with exact two-click restoration. Full-sphere execution uses player world orientation for yaw and the FPP camera's local orientation for pitch, always deriving each pose from the saved origin rather than accumulating Euler changes.

Required runtime components are CET, its required RED4ext runtime, and ReShade for 16-bit HDR screenshots. The project does not require the paid IGCS tools or IGCS Connector. Keep the custom RED4ext plugin as a fallback only if one measured CET requirement fails.

The CET capture environment must:

- apply a uniquely named time dilation near zero and remove only that dilation on restore;
- keep CET/camera updates responsive while the world is frozen;
- apply reversible `NoMovement` and `NoCameraControl` restrictions while capture is active so screenshots cannot be invalidated by accidental input; the standalone-camera path is verified to work while FreeFly remains active;
- snapshot the `inkHUDLayer` widget opacities, hide the layer's children during capture, and restore the exact saved values;
- reapply HUD hiding at capture transitions so newly created HUD children cannot appear in later frames;
- snapshot every player and active-weapon mesh component's enabled state, disable both renderers during capture, and restore the exact saved states;
- re-scan and track newly created player or active-weapon mesh components before each capture pose;
- restore camera, player meshes, HUD, and time state on completion, abort, Photo Mode/menu transitions, player detach, CET shutdown, and recoverable errors.

Use CET `registerInput` for Start/Advance/Abort bindings. Read back the active camera transform, basis, FoV, and aspect ratio after every pose change. Never infer metadata only from requested angles.

Measure real game FPS from CET `onUpdate(deltaTime)` and detect frame-generation settings through `Game.GetSettingsSystem()`. Keep real game frames distinct from generated presentation frames. After every final pose write, count at least the configured number of real `onUpdate` frames before capture; default to 8. A native swap-chain present counter may provide optional presented-FPS diagnostics, but must not become a dependency or advance the temporal-settling counter.

Do not use the FPP setters while Photo Mode is active and do not execute the native REDengine `GetCameraSystem` RTTI query during startup.

See `docs/igcs-evaluation.md` for why IGCS Connector is not the selected full-sphere backend.
See `docs/cet-fpp-reference-implementations.md` for the installed CET-mod implementations used as the basis for time freeze, HUD suppression, and player-renderer hiding.

### ReShade screenshot add-on

Implement screenshot capture and completion reporting as a small native ReShade add-on. Vendor the official headers from the ReShade `v6.7.3` tag (API 18), matching the installed ReShade 6.7.3 runtime; do not build against the moving `main` headers, which currently advertise a newer API. Register `init_effect_runtime`, `destroy_effect_runtime`, `reshade_present`, and `reshade_screenshot` callbacks through the official add-on API.

- Accept at most one pending capture request carrying a session ID, pose index, and short filesystem-safe correlation token.
- Consume the request from `reshade_present`, then call `effect_runtime::save_screenshot(token)` on the active runtime. Do not call runtime methods from an IPC worker thread; the public API does not document that as thread-safe.
- Treat `addon_event::reshade_screenshot(runtime, path)` as the authoritative successful-save signal. It is documented as running after the screenshot has been saved and supplies the exact path.
- Match completion by both runtime and correlation token. Ignore unrelated screenshots instead of binding them to the current pose.
- Reject concurrent requests and emit a timeout/error without advancing the camera if no matching event arrives.
- Use ReShade's configured screenshot path, naming, format, effects, and HDR pipeline. Do not parse the on-screen notification and do not discover successful captures by directory polling in the primary path.
- Use the callback path as authoritative; reading `[SCREENSHOT] SavePath` is unnecessary for association. If preflight/status needs the configured directory, query it through ReShade's `get_config_value` API instead of parsing `ReShade.ini` independently.
- Keep the existing stable-file watcher and Print Screen simulation only as compatibility fallbacks.

Before integration, prove in game that `save_screenshot()` produces the same kind of 3840×2160 16-bit Rec.2020/PQ PNG as the verified ReShade hotkey. The API uses ReShade's disk-save path, but identical Cyberpunk HDR behavior is an empirical compatibility gate. If this gate fails, retain the event callback for exact completion/path reporting and trigger ReShade's normal screenshot binding as the fallback.

### pano-stitch command-line tool

Use Python 3.12 with:

- NumPy for projection calculations
- OpenCV for image remapping
- an HDR-capable image layer for 16-bit PNG and floating-point OpenEXR
- Pillow only for ordinary 8-bit PNG compatibility where appropriate
- pytest for tests
- Ruff for linting and formatting
- mypy for type checking

The stitcher must use recorded camera geometry. Do not use image feature matching in the MVP.

The stitcher must remain below 1,000,000,000 bytes of peak resident memory while rendering a complete session of 30 3840×2160 HDR screenshots. Treat this as a hard MVP acceptance requirement, not an optional optimization. Use a conservative internal working-memory target of at most 768 MiB so decoder, Python runtime, and output-writer overhead cannot push the process over the limit.

## Suggested repository layout

```text
cp2077-panorama-taker/
├── contracts/
│   ├── session.schema.json
│   └── example-session.json
├── mod/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── camera_controller.cpp
│   │   ├── camera_controller.hpp
│   │   ├── capture_planner.cpp
│   │   ├── capture_planner.hpp
│   │   ├── capture_session.cpp
│   │   ├── capture_session.hpp
│   │   ├── screenshot_watcher.cpp
│   │   ├── screenshot_watcher.hpp
│   │   └── plugin.cpp
│   ├── scripts/PanoramaCapture.reds
│   └── cet/init.lua
├── reshade-addon/
│   ├── CMakeLists.txt
│   ├── include/
│   ├── src/addon.cpp
│   └── tests/
├── stitcher/
│   ├── pyproject.toml
│   ├── src/pano_stitch/
│   │   ├── __init__.py
│   │   ├── cli.py
│   │   ├── metadata.py
│   │   ├── projection.py
│   │   ├── compositor.py
│   │   └── validation.py
│   └── tests/
└── docs/
    ├── installation.md
    └── capture-guide.md
```

## Shared metadata contract

Define `contracts/session.schema.json` before implementing either application. Both pieces must follow this contract.

Example:

```json
{
  "schema_version": 1,
  "session_id": "20260817-221500",
  "capture_mode": "full_sphere",
  "projection": "rectilinear",
  "viewport": {
    "width": 3840,
    "height": 2160
  },
  "image_encoding": {
    "sample_type": "float16",
    "color_primaries": "rec2020",
    "transfer_function": "linear",
    "reference_white_nits": 203.0
  },
  "fov": {
    "horizontal_deg": 100.0,
    "vertical_deg": 67.67,
    "source": "render_projection_matrix"
  },
  "base_pose": {
    "position": [0.0, 0.0, 0.0],
    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0]
  },
  "planner": {
    "overlap_fraction": 0.08,
    "yaw_step_deg": 90.0,
    "pitch_step_deg": 60.0
  },
  "render_timing": {
    "required_real_settle_frames": 8,
    "real_fps_at_start": 47.8,
    "presented_fps_at_start": 95.4,
    "frame_generation": {
      "enabled": true,
      "active_backend": "dlss",
      "raw_settings": {
        "DLSSFrameGen": true,
        "DLSS_MultiFrameGeneration": 1
      }
    }
  },
  "frames": [
    {
      "index": 0,
      "filename": "Cyberpunk2077_001.png",
      "yaw_deg": 0.0,
      "pitch_deg": 0.0,
      "roll_deg": 0.0,
      "position": [0.0, 0.0, 0.0],
      "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "view_matrix_row_major": [],
      "projection_matrix_row_major": [],
      "settled_real_frames": 8,
      "real_fps_before_capture": 46.9,
      "status": "captured"
    }
  ],
  "completed": true
}
```

Contract requirements:

- Store both horizontal and vertical FoV to eliminate convention ambiguity.
- Record source sample type, color primaries, transfer function, and reference-white/max-luminance metadata needed to decode HDR values consistently.
- Require one compatible encoding for all frames in a session. Reject mixed SDR/HDR or mixed transfer functions until explicit conversion is implemented.
- Record final observed projection and view matrices when the active CET camera exposes them. In the next schema revision, make matrices optional and require verified horizontal/vertical FoV plus viewport dimensions as the projection fallback.
- State matrix layout, handedness, axis directions, quaternion order, and angle signs in the schema descriptions.
- Record commanded yaw, pitch, and roll as canonical angles relative to the panorama origin.
- Record smoothed real FPS, required/observed real settle-frame counts, normalized frame-generation state, and every available raw frame-generation setting. Record presented FPS only when a native present counter is available.
- Reject a frame-generation setting change during an active session; a new configuration requires a new session.
- Keep screenshot filenames relative to the manifest.
- Include the game version, RED4ext version, mod version, and UTC timestamps.
- Rewrite the manifest atomically after each successful frame: write a temporary file, flush it, and replace the old manifest.
- Never mark a frame as captured until its screenshot exists, has stopped growing, and can be decoded.
- Mark incomplete, aborted, and failed sessions explicitly.

## PanoramaCapture design

### Phase 0: camera-control feasibility spike

This is a mandatory gate before implementing the complete mod.

1. Pin the exact Cyberpunk 2077, ReShade, CET, and required RED4ext runtime versions.
2. Detect and reject Photo Mode and other unsupported camera/player states before using the normal FPP camera.
3. Log the active camera position, orientation/basis, FoV, and viewport. Log projection data if CET exposes it; do not require unavailable matrices.
4. Snapshot, apply, verify, and restore an absolute FPP player/camera pose.
5. Verify that yaw and pitch rotate around one unchanged optical center and introduce no roll.
6. Change FoV and read the effective value back from the active FPP camera.
7. Repeat the absolute-pose test across a complete yaw circle and near both pitch poles.
8. Confirm safe restoration on the normal restore command, abort, CET reload, save transition, and script error where recoverable.
9. Verify the capture environment restores time dilation, exact HUD state, and exact player mesh states alongside the camera pose.
10. Verify body, arms, equipment, shadows, and reflections are absent throughout equatorial and polar views.
11. Reject unsupported or unverified runtime behavior clearly.

Existing camera and FoV projects may be used as references, but do not copy version-specific offsets or signatures without validating them against the pinned runtime.

Spike acceptance criteria:

- Optical-center movement below 1 mm between rotations, or the smallest reliably measurable CET tolerance if camera-world position is not exposed directly.
- Commanded versus observed orientation error below 0.01 degrees, or a documented tighter-than-one-output-pixel angular tolerance derived from the target resolution.
- FoV remains fixed across at least 300 rendered frames.
- Restoration returns the camera to its original pose and FoV.
- Reloading CET or changing saves while idle is safe; interrupting an active probe restores immediately when CET provides an applicable shutdown/exit callback.
- Unsupported game versions or camera modes fail with a useful log message rather than crashing.

The CET Photo Mode control gate failed, but this does not apply to the normal first-person path: FPP yaw/pitch changes and exact restoration are already verified. CET-accessible time dilation, HUD-layer controls, and player mesh-component hiding have also been found in working installed mods. The remaining gate is an in-game FPP capture-environment test covering frozen-world behavior, complete HUD and player-renderer suppression, polar coverage, stable optical center/FoV, and restoration.

### Capture planning

Apply the requested FoV, then read the effective horizontal and vertical FoV back from the final render projection. Use effective values for planning and metadata rather than trusting the requested setting.

For either axis:

```text
desiredStep = effectiveFov * (1 - overlapFraction)
frameCount  = ceil(axisRange / desiredStep)
actualStep  = axisRange / frameCount
```

This ensures uniform angles and makes the final frame close exactly onto the first without duplicating a 360-degree frame.

Horizontal mode:

```text
axisRange = 360 degrees
yaw[i] = i * actualYawStep
pitch = 0 degrees
```

Full-sphere mode:

```text
yawCount   = ceil(360 / desiredHorizontalStep)
pitchCount = ceil(180 / desiredVerticalStep)

yawStep   = 360 / yawCount
pitchStep = 180 / pitchCount

pitch[row] = -90 + pitchStep / 2 + row * pitchStep
yaw[col]   = col * yawStep
```

Use row-adaptive yaw counts after geometric coverage validation. Start from the latitude-scaled azimuth count, then apply a small configurable guard band before rounding so different adjacent rows cannot leave diagonal gaps. For the verified 59.230° × 35.462° capture geometry with 8% overlap and a 5% adaptive-yaw guard, use 3/6/7/7/6/3 frames from nadir to zenith (32 total). Re-run the metadata-only coverage diagnostic whenever FoV, overlap, guard, or projection conventions change.

Generate and validate the complete shot list when the session starts. Store it in the manifest before taking the first screenshot.

### Camera origin and positioning

When starting a session:

1. Snapshot the current camera position, orientation, FoV, and projection.
2. Treat that optical center and orientation as the panorama origin.
3. Apply the configured FoV.
4. Wait until the final projection reports the expected FoV.
5. Build every shot orientation relative to the origin.
6. Apply the current absolute shot pose once, wait for settling, then verify it from the active camera.

Never rotate incrementally. Calculate every pose from the original snapshot:

```text
shotOrientation = baseOrientation * relativeYawPitchOrientation
shotPosition = basePosition
```

This prevents cumulative floating-point drift and ensures rotation occurs around the optical center. Do not write or teleport continuously in `onUpdate`; retry a pose only a small, bounded number of times when verification shows that the first write was ignored.

Define a canonical panorama coordinate system shared with the stitcher. A reasonable choice is:

- Positive X: right
- Positive Y: up
- Positive Z: starting forward direction
- Yaw zero: starting forward direction
- Longitude increases toward positive X
- Quaternion order: XYZW

The mod may convert from engine coordinates internally, but the manifest must use the canonical convention.

Origin modes:

- `current`: the current camera becomes the origin when Start is pressed. Implement this first.
- `saved`: reuse a previously saved origin. Defer until after MVP.

### Capture state machine

```text
Idle
  -> ApplyingOrigin
  -> Settling
  -> Ready
  -> TriggeringScreenshot
  -> AwaitingScreenshotEvent
  -> CommittingFrame
  -> ApplyingNextPose
  -> Settling
  -> ...
  -> Completed
```

Any active state may transition to `Aborted` or `Error`.

Rules:

- Ignore repeated or re-entrant commands while an operation is pending.
- Use both configured `settle_frames` and `settle_ms` minima before declaring a pose ready. The default `settle_ms` is zero so the eight-real-frame requirement is authoritative; use the screenshot timeout as a stall watchdog.
- Keep enforcing the absolute camera pose while settling and capturing.
- Do not rotate until screenshot completion has been confirmed.
- Restore the original transform and FoV after completion or abort by default.
- On screenshot timeout, remain at the current pose and allow retry or abort.
- Persist enough session state to diagnose an interrupted capture.

### Screenshot modes

#### MVP: manual mode

1. The mod moves and locks the camera.
2. The user invokes either the add-on capture binding or ReShade's screenshot binding.
3. The add-on receives `reshade_screenshot` with the exact completed path.
4. The user presses Capture/Advance.
5. The coordinator commits the matching path and pose metadata, then advances.

Only one screenshot may be awaiting association with a frame.

#### Automated mode

1. After the CET pose is ready, send one correlated capture request to the add-on.
2. On the next suitable `reshade_present`, call `effect_runtime::save_screenshot(correlation_token)`.
3. Wait for a matching `reshade_screenshot` event; its path is authoritative.
4. Verify that the completed file can be decoded and has the expected dimensions and HDR encoding.
5. Atomically commit its relative filename and camera metadata.
6. Acknowledge completion to CET and only then advance to the next pose.

The communication transport is a small local command/event bridge. Keep transport I/O off the render callback: an IPC worker may enqueue/dequeue bounded plain-data messages, while `reshade_present` alone touches the runtime. The first implementation uses an add-on-owned capture hotkey plus an append-only event record to prove capture and event correlation. Before unattended capture, spike whether CET can access a named pipe without blocking its update callback. If not, use atomically replaced request/acknowledgement files in a dedicated bridge directory; this polls tiny control records, never the screenshot directory. Do not revive the RED4ext plugin solely for transport unless both approaches fail.

References:

- [ReShade screenshot implementation](https://github.com/crosire/reshade/blob/main/source/runtime.cpp)
- [ReShade API, including direct screenshot capture](https://github.com/crosire/reshade/blob/main/include/reshade_api.hpp)
- [ReShade add-on events](https://github.com/crosire/reshade/blob/main/include/reshade_events.hpp)

ReShade's `capture_screenshot(void *)` is not the selected path: it returns raw back-buffer bytes to the caller and would make this project responsible for HDR encoding, file naming, and save-path handling. Use `save_screenshot()` so ReShade retains ownership of those details.

### Configuration

Start with a JSON configuration similar to:

```json
{
  "capture_mode": "full_sphere",
  "screenshot_mode": "reshade_addon",
  "target_fov_deg": 100.0,
  "fov_axis": "horizontal",
  "overlap_fraction": 0.08,
  "settle_frames": 8,
  "settle_ms": 1500,
  "screenshot_timeout_ms": 10000,
  "screenshot_bridge_mode": "control_files",
  "screenshot_bridge_path": "PanoramaCaptureProbe/bridge",
  "restore_camera": true,
  "hide_ui": true,
  "hide_player": true
}
```

Validate at minimum:

- FoV is greater than zero and safely below 180 degrees.
- Overlap is non-negative and below a documented upper limit.
- The ReShade add-on and a compatible add-on API are loaded.
- The add-on reports an active effect runtime before capture starts.
- Timeouts and settling values are non-negative.
- `settle_frames` counts real CET game-update frames after the final pose write, never frame-generated swap-chain presents; `settle_ms` is an independent lower bound for GI/temporal stabilization.
- Capture mode and screenshot mode use known values.

### Capture-environment guidance

UI and player-renderer hiding are required for unattended FPP capture. Snapshot and restore exact state rather than relying on another mod's toggle state. Reject capture if complete HUD suppression or player-mesh restoration cannot be guaranteed.

Document that users should disable or lock effects that violate the rectilinear-camera assumption or change between frames:

- Motion blur
- Depth of field
- Camera shake and weapon sway
- Lens distortion
- Chromatic aberration
- Vignette where possible
- Automatic exposure where possible

Use Photo Mode or another time-freezing mechanism for moving NPCs, traffic, particles, and weather. Path-traced or temporally accumulated rendering may require a much larger settling period at every angle.

## pano-stitch design

### CLI

Required commands:

```shell
pano-stitch validate path/to/session.json
pano-stitch render path/to/session.json --output panorama.png
```

Useful options:

```text
--width 12000
--blend hard|feather
--allow-incomplete
--debug-coverage coverage.png
```

Default behavior must reject incomplete sessions, missing files, duplicate filenames, mixed source dimensions, unsupported projections, and uncovered output pixels.

### Validation flow

Before allocating the panorama:

1. Validate the manifest against the JSON Schema.
2. Resolve every screenshot relative to the manifest directory.
3. Verify every file exists and can be decoded.
4. Verify all images have the recorded dimensions and compatible color modes.
5. Verify frame indices and filenames are unique.
6. Verify FoV values and matrices are finite.
7. Verify the planned orientations provide coverage for the requested output mode.

### Projection algorithm

For every equirectangular output pixel:

1. Convert its pixel center to longitude and latitude.
2. Convert longitude and latitude into a canonical unit direction.
3. Transform that direction into each candidate camera's local coordinates.
4. Reject it if it is behind the camera or outside the recorded frustum.
5. Project it into the rectilinear screenshot.
6. Sample the source with bilinear interpolation.
7. Select or blend valid samples.

Canonical equirectangular mapping:

```text
longitude = 2*pi * ((x + 0.5) / outputWidth - 0.5)
latitude  = pi * (0.5 - (y + 0.5) / outputHeight)

direction = [
  cos(latitude) * sin(longitude),
  sin(latitude),
  cos(latitude) * cos(longitude)
]
```

Use the recorded projection matrix when possible. Explicit FoV values are the fallback and should also be used for human-readable validation.

For an ordinary rectilinear projection fallback:

```text
fx = sourceWidth / (2 * tan(horizontalFov / 2))
fy = sourceHeight / (2 * tan(verticalFov / 2))

u = cx + fx * cameraX / cameraZ
v = cy - fy * cameraY / cameraZ
```

Use pixel-center conventions consistently in tests and production code.

### Compositing

Implement two modes:

- `hard`: choose the valid frame where the projected point is furthest from a screenshot edge.
- `feather`: blend only inside overlaps, using distance from invalid screenshot edges as weights.

Decode the recorded transfer function into a `float32` linear-light working buffer before interpolation or feathering. Support ordinary sRGB plus the HDR transfer function produced by the verified ReShade workflow. Convert to the selected output encoding only after compositing. Blending gamma-, PQ-, or HLG-encoded values produces incorrect brightness.

Do not implement these in MVP:

- Feature detection or automatic alignment
- Optical flow
- Exposure matching
- Seam finding
- Multiband blending

If exact camera control and metadata are correct, those features are not required. Add them later only in response to demonstrated capture limitations.

### Output size and format

For a full sphere:

```text
outputHeight = outputWidth / 2
```

Choose a default width that approximately preserves the input's central angular resolution:

```text
fx = inputWidth / (2 * tan(horizontalFov / 2))
outputWidth = round(2 * pi * fx)
```

Round the result to an even number. Permit explicit `--width` override.

For horizontal-only mode, emit a cropped equirectangular band. Do not silently generate a 2:1 image with invented or black poles.

For the HDR proof of concept, read a real ReShade HDR capture without reducing it to 8-bit, process it in `float32` linear light, and save a half-float or float OpenEXR panorama. OpenEXR is the reference output used to prove that highlights and values above SDR white survive the complete pipeline.

The release must also expose PNG output:

- SDR sources may produce ordinary lossless 8-bit or 16-bit PNG.
- HDR sources currently produce an explicitly tone-mapped lossless 8-bit SDR PNG preview. Stream this output directly; do not create an EXR intermediate.
- Retain EXR as the archival HDR output. A future HDR-capable PNG mode requires verified transfer-function, primaries, bit-depth, and signaling support in the chosen viewers.
- Exact Rec.2020-to-display-primary conversion and tone-map tuning remain release work; do not describe the SDR PNG preview as preserving source dynamic range.

Pillow cannot be the sole HDR image backend because its multichannel RGB path is limited to 8 bits per channel. Select and test the HDR backend against actual ReShade files before freezing dependencies.

Rendering must be out of core:

- Never retain all decoded source images or a complete floating-point panorama in resident memory.
- Probe source headers and encoding sequentially during validation, then decode at most one source image at a time.
- Convert transfer functions to linear `float32` in bounded row chunks so PQ decoding does not create several full-image temporaries.
- Composite bounded output strips or tiles. Store the panorama and hard/feather weight accumulators in disk-backed memory-mapped scratch arrays; workers may update only disjoint active rows, while the operating system retains or evicts backing pages within the configured working-budget model.
- Derive tile dimensions from a conservative worst-case allocation estimate that includes directions, projection maps, masks, sampled RGB, blend weights, the decoded source, decoder buffers, and writer buffers.
- Stream completed output rows or tiles into an EXR/PNG backend proven not to stage the complete output image. If a selected encoder cannot do that, reject it rather than silently exceeding the memory limit.
- Create output and scratch files atomically where practical, and remove incomplete scratch data after success or a handled failure.
- Treat successful requested outputs as user-owned artifacts and never delete them automatically. Scratch directories and `.partial` outputs are implementation details and must be removed; manually requested benchmark/debug outputs require an explicit cleanup command or user action.
- Ensure `Ctrl+C`/`KeyboardInterrupt` removes the atomic `.partial` output. A hard process kill or power loss cannot run in-process cleanup, so document or implement a narrowly scoped stale-artifact cleanup command that only targets the compositor's recognizable scratch/partial names.

The CLI may expose the calculated tile size and estimated working set for diagnostics, but production defaults must remain under the hard memory ceiling without requiring user tuning.

## Verification plan

### Automated planner and metadata tests

- Planning covers exactly 360 degrees without a duplicated final angle.
- Requested overlap never becomes a gap.
- Full-sphere planning includes both poles.
- Every orientation is derived from the base pose rather than the preceding pose.
- Invalid or incomplete manifests produce actionable errors.
- Missing, duplicate, or dimension-mismatched images are rejected.
- Atomic manifest replacement preserves the previous valid file if writing fails.

### Synthetic stitcher tests

- Generate rectilinear views of a known colored cube or latitude/longitude grid.
- Reconstruct the expected equirectangular panorama.
- Verify known directions land at expected pixels.
- Verify longitude wraps with no blank first or last column.
- Verify both poles are covered in full-sphere mode.
- Verify hard and feather modes do not produce uncovered pixels.
- Verify rendering identical input twice produces identical output.
- Verify the result is a valid PNG and exactly 2:1 in full-sphere mode.
- Verify an HDR synthetic source containing values above 1.0 survives projection and EXR output within floating-point tolerance.
- Verify hard and feather compositing operate in linear light for HDR inputs.
- Run the renderer in a subprocess against 30 3840×2160 HDR fixtures or equivalent deterministic generated inputs, sample its resident set throughout the render, and assert that peak RSS remains below 1,000,000,000 bytes for both hard and feather modes.
- Verify that increasing output width increases scratch-file size and processing time without causing panorama-sized resident allocations.

### In-game acceptance test

Use a static Photo Mode scene with motion blur, depth of field, shake, chromatic aberration, and lens distortion disabled.

Success criteria:

- All frames use the same optical center within 1 mm.
- Commanded and recorded angles differ by less than 0.01 degrees.
- The first/last yaw seam differs by less than two pixels on static geometry.
- The manifest contains exactly one unique screenshot per successful pose.
- The coverage diagnostic contains no uncovered pixels in full-sphere mode.
- Aborting restores the original camera and FoV.
- A screenshot timeout never advances the camera.
- ReShade effects are present in captured images.
- A complete run needs no manual filename editing.

## Implementation work packets

Assign these packets to the implementation model one at a time. Require tests and a short change explanation for every packet. Do not ask it to implement the whole project in one pass.

### Packet 1: contracts and scaffolding

Deliverables:

- Create the proposed directory structure.
- Add `session.schema.json` with fully documented coordinate conventions.
- Add a valid example session.
- Configure CMake and Python packaging minimally.
- Configure clang-format, Ruff, mypy, and pytest.

Acceptance:

- Example metadata validates against the schema.
- Empty native and Python targets build/import cleanly.
- All configured checks pass without warnings.

### Packet 2: Python metadata and capture planner

Deliverables:

- Implement schema-backed metadata loading.
- Implement horizontal and full-sphere shot planning.
- Add coverage and invalid-input tests.

Acceptance:

- Planner tests prove exact 360-degree closure and pole coverage.
- Public functions are typed and documented.
- Ruff, mypy, and pytest pass.

### Packet 3: synthetic stitcher

Deliverables:

- Implement equirectangular direction generation.
- Implement inverse rectilinear projection.
- Implement hard compositing.
- Add synthetic grid/cube fixtures and PNG output.
- Add the `validate` and `render` CLI commands.

Acceptance:

- Synthetic reconstruction has no uncovered pixels.
- Longitude seam and pole tests pass.
- Output is deterministic and lossless PNG.

### Packet 3H: HDR stitcher proof of concept

Deliverables:

- Replace the unconditional Pillow `RGB`/`uint8` decode path with an image backend that preserves a verified ReShade HDR input.
- Add source-encoding metadata and validation.
- Process projection, interpolation, and blending in `float32` linear light.
- Write a half-float or float OpenEXR panorama.
- Add synthetic HDR tests containing values below black-reference, at reference white, and above 1.0.

Acceptance:

- A real ReShade HDR screenshot can be decoded without reducing it to 8-bit.
- Values above 1.0 survive a render/read-back round trip within documented tolerance.
- Hard and feather modes do not clamp HDR values.
- Ruff, mypy, and pytest pass.

### Packet 3M: memory-bounded HDR compositor

Deliverables:

- Replace eager loading of every source image with sequential header validation and one-source-at-a-time decoding.
- Replace the full panorama/direction/weight arrays with bounded tiles and disk-backed scratch accumulators.
- Decode PQ and other transfer functions in bounded chunks without full-image intermediate arrays.
- Add incremental EXR output; keep PNG output on the same bounded writer contract when it is enabled.
- Add allocation estimates and peak-RSS integration measurement for hard and feather rendering.

Acceptance:

- Rendering 30 3840×2160 HDR inputs stays below 1,000,000,000 bytes peak RSS, including Python, decoder, compositor, and encoder memory.
- HDR values and output pixels match the existing eager implementation within documented floating-point tolerance on synthetic fixtures.
- No code path constructs a list of decoded session images or a complete in-memory panorama.
- Interrupted and failed renders do not leave a valid-looking partial output and clean up their scratch files.
- `Ctrl+C` cleanup is verified for both PNG and EXR; stale cleanup after an uncatchable hard kill never deletes a successful requested output.
- Ruff, mypy, and pytest pass without warnings.

Current result (2026-08-20): the 16-image v0.1.5 quarter-dimension PNG render (2984×1492, feather blend) peaked at 432,068 KiB RSS (421.9 MiB), used no swap, and required 71,234,048 bytes (67.9 MiB) of disk scratch. The direct PNG is 7,108,960 bytes (6.78 MiB). No `pano-stitch-*` scratch directory, `.partial` output, or EXR conversion intermediate remained after completion. The same manifest estimates 1,140,126,752 bytes (1.06 GiB) of scratch at its native 11,938×5,969 output size.

### Packet 4: CET FPP capture-environment feasibility

Deliverables:

- Pin supported game, CET, and required RED4ext runtime versions.
- Reject capture while Photo Mode, menus, vehicles, ladders, scripted scenes, or other unsupported camera states are active.
- Require other time-control features to be inactive because CET does not expose the prior value of the global local-player dilation-ignore flag used by the initial probe.
- Add a two-click environment probe: first click snapshots state, applies near-zero time dilation, hides `inkHUDLayer` and player mesh components, and applies a representative FPP yaw/pitch; second click restores everything.
- Verify moving NPCs, vehicles, particles, weather, animation, and player sway remain visually stationary over the maximum expected capture duration.
- Verify the player/FPP camera remains controllable while dilation is active.
- Log smoothed real FPS from CET update deltas and the raw/normalized state of every available DLSS, FSR3, XeSS, and generic frame-generation setting.
- Verify exactly eight or the configured greater number of real CET update frames elapse after the final pose write before the probe reports capture-ready, regardless of frame-generation multiplier.
- If a native present counter is available, log presented FPS and its ratio to real FPS as diagnostics without using it for settling.
- With frame generation off and on, compare CET update count against UltraTool's present count and visually verify that eight CET updates advance temporal accumulation as expected. Document the observed multiplier and reject this counter source if it tracks generated presents.
- Verify all HUD, markers, interaction prompts, quest widgets, crosshair, damage overlays, notifications, and newly spawned HUD children remain absent.
- Verify the player body, head, arms, held weapon, clothing, cyberware, shadows, and reflections remain absent at representative equatorial and polar poses.
- Verify originally disabled mesh components remain disabled after restoration and newly created components are tracked safely.
- Test all planned pitch-row centers, including rows whose FoV covers both poles, without optical-center movement or unexpected roll.
- Test normal completion, abort, CET reload, save reload, and recoverable error restoration.

Acceptance:

- Time freeze, HUD suppression, player-renderer suppression, real-frame temporal settling, full-sphere orientation coverage, FoV stability, and restoration pass in the actual game.
- No production state machine or screenshot automation yet.

If CET cannot meet one specific criterion, investigate a documented game API, established free-camera integration, or narrowly scoped native fallback for that criterion. Do not restore the custom RED4ext plugin to the whole architecture by default.

Current result (2026-08-20): the live CET environment probe passes FPP yaw/pitch movement and exact two-click restoration, near-zero time dilation, HUD suppression/restoration, player mesh suppression/restoration, frame-generation setting detection, and eight-real-update-frame settling. Quick-load while frozen is a known limitation: F9/session teardown does not execute until time is unfrozen, so automatic cleanup cannot run during that interval. Defer deeper input/native-hook work unless unattended abort during a frozen session becomes a release requirement. Packet 4 still needs the planned adversarial checks: polar rows, complete UI/transient coverage, and frame-generation on/off counter validation.

### Packet 5: production CET FPP camera controller

Deliverables:

- Implement absolute FPP poses derived from one saved player/camera origin.
- Implement full-sphere yaw/pitch planning and effective FoV readback.
- Use row-adaptive yaw counts: reduce azimuth samples toward the poles according to the row latitude while preserving the configured overlap margin.
- Integrate capture-environment snapshot, near-zero time dilation, HUD hiding, player mesh hiding, and exact restoration.
- Extend the session schema with render-timing and frame-generation metadata before the capture mod emits production manifests.
- Convert observed engine transforms into canonical metadata coordinates.
- Add runtime guards for unsupported camera/player states.

Acceptance:

- Commanded and observed yaw/pitch agree within tolerance for every full-sphere pose.
- Optical center and FoV remain fixed and roll stays at the commanded value.
- Completion and abort restore camera, player orientation, every recorded player mesh state, HUD state, and time dilation.
- Unsupported states fail safely and clearly.

Current result (2026-08-21): v0.1.12 completed the full Packet 5 acceptance set. In-game checks cover full-sphere capture, guarded 3/5/5/3 planning at 90.60°×59.23° FoV, calibrated pitch, time/HUD/player/weapon/input restoration, menus/vendor UI, vehicles, scripted scenes, CET overlay close, and CET reload. Documented limitations are frozen-time F9, FreeFly conflict, non-ownership-aware input restrictions, unsupported vehicle cameras, and unverified exotic hand props.

### Packet 6: CET frontend and full-sphere session controller

Deliverables:

- Implement deterministic yaw and pitch-row execution from the observed effective horizontal/vertical FoV and configured overlap.
- Implement Start, Advance, Abort, and Status operations in the CET mod.
- Add configurable CET input bindings and validated configuration.
- Report smoothed real FPS, frame-generation configuration, elapsed real settle frames, and estimated readiness time in Status.
- Read back the actual pose/FoV after every FPP pose change and convert it to canonical metadata.
- Prevent Photo Mode or incompatible camera controllers from starting during a session.

Acceptance:

- Exactly one capture pose is produced for every planned yaw/pitch pair without a duplicate 360-degree endpoint.
- Measured optical-center, pitch, yaw, and roll tolerances pass for the complete sphere.
- Invalid configuration prevents session start and explains why.
- Repeated commands cannot corrupt state.
- Changing the frame-generation multiplier cannot shorten the required real-frame settling interval.

Current result (2026-08-21): Packet 6 development controls were verified. Release
registration retains only Start and Abort; manual Advance and Status remain
source-only development handlers. FoV-derived planning, settling, observed
pose/FoV metadata, readiness gating, and zero-pitch calibration are operational.

### Packet 7: manual capture sessions

Deliverables:

- Implement the capture state machine.
- Create per-session output directories and manifests.
- Scaffold a 64-bit ReShade add-on with the official ReShade `v6.7.3` API 18 headers.
- Register runtime lifecycle, present, and screenshot callbacks.
- Add an add-on-owned test capture binding that calls `save_screenshot()` with a pose correlation token.
- Record the exact completed path from `reshade_screenshot`; ignore unrelated captures.
- Verify add-on-triggered output remains 3840×2160 16-bit Rec.2020/PQ PNG with ReShade effects applied.
- Associate one image with one pose.
- Atomically update metadata and advance.

Packet 7 implementation began with `stitcher/scripts/extract_cet_pose_manifest.py`: it binds the latest complete indexed CET metadata session to a caller-selected filename interval, rejects unsupported/empty files and count mismatches, and atomically writes the resulting manifest. The stable-file watcher remains a tested fallback. The ReShade add-on proof and exact event/path correlation are complete; per-pose advancement integration is deferred to Packet 8's CET bridge.

Acceptance:

- Full-sphere capture completes end to end.
- A test request causes exactly one normal ReShade HDR screenshot and exactly one matching completion event.
- The verified add-on request produced a 3840×2160, 16-bit RGB PNG decodable as `uint16`, with HDR-range sample values matching the normal ReShade workflow.
- Manual or third-party screenshots cannot be associated with the pending panorama pose.
- Timeout and correlation failures do not rotate the camera.
- Abort and completion restore the original camera.

### Packet 8: automated ReShade screenshots

Implementation note (2026-08-21): the first transport is the atomic control
file fallback. CET writes `PanoramaCaptureBridge.request` as
`1<TAB>session<TAB>pose<TAB>token`, replacing it only after close. The add-on
consumes that record on a worker thread, invokes `save_screenshot(token)` from
`reshade_present`, and publishes `PanoramaCaptureBridge.ack` as
`1<TAB>session<TAB>pose<TAB>token<TAB>exact-path` after the screenshot event.
Release captures require `automatedScreenshots = true`; the manual advance
handler remains source-only development scaffolding and is not registered in
the shipped CET surface.

Deliverables:

- Add a bounded local command/event bridge between CET and the ReShade add-on: prefer a non-blocking named pipe if the CET transport spike passes, otherwise use atomic request/acknowledgement control files.
- Queue correlated requests outside render callbacks and execute `save_screenshot()` from `reshade_present`.
- Return exact completion paths and errors through the bridge.
- Add decode and dimension validation.
- Add retry and timeout handling.
- Retain Win32 key simulation plus stable-directory polling only as an explicitly selected compatibility fallback.

Acceptance:

- One automated run produces exactly one image per pose.
- Slow screenshot writes cannot cause premature rotation.
- Duplicate, stale, unrelated, or missing completion events produce a safe, recoverable error.
- The add-on unloads/reloads without retaining a runtime pointer or pending request.

Status (2026-08-21): accepted in-game with a complete 16-pose run. CET v0.1.16
and the rebuilt add-on correlated every pose to exactly one ReShade screenshot,
then restored the environment. The mailbox lives in the CET mod folder and is
resolved portably by the add-on relative to ReShade's base directory.

### Packet 9: full-sphere integration

Gate: do not implement this integration packet until Packet 4 proves time freeze, HUD hiding, player-renderer hiding, polar coverage, and restoration in the real FPP camera.

Deliverables:

- Enable pitch-row execution in the mod.
- Verify stable orientations near the poles.
- Feed a complete capture into the stitcher.
- Add a coverage diagnostic image.

Acceptance:

- End-to-end output is a covered 2:1 equirectangular PNG.
- Pole orientation has no unexpected roll.
- No frame requires manual metadata or filename correction.

Status (2026-08-21): the in-game full-sphere capture path and ReShade bridge
are accepted. The stitcher now consumes CET's per-session JSON directly,
including observed camera bases and exact screenshot paths, with portable
basename/`--image-dir` fallback when a Windows capture is moved to another
machine. Completed-state validation is strict; partial sessions are an
explicit `--allow-incomplete` workflow. A direct render of completed session
`1787269393-1` exposed a 90° rotation: CET emits Z-up bases, whereas the
stitcher uses Y-up. The CET adapter now converts its right/up/forward vectors
into the canonical convention; rerendering that session is required before
Packet 9 can be accepted. Orientation is now visually verified in
`/tmp/cp2077-pano-packet9-yup.png`. The optional streaming
`--debug-coverage` PNG output is implemented and tested; one real-session
coverage-image run remains for final acceptance. Packet 9 is now accepted:
the corrected real-session render has the verified way-up, and its coverage
diagnostic is pure white, confirming complete output-pixel coverage.

### Packet 10: stitcher desktop GUI

Scope: provide a pleasant local frontend over the existing metadata validator
and bounded compositor. The GUI must not duplicate projection, validation, or
rendering logic; it invokes the same Python APIs as `pano-stitch`.

Deliverables:

- Provide a Windows-friendly desktop GUI for the stitcher.
- Let the user choose a CET `capture.json`, a screenshots directory, and an
  output directory.
- Validate automatically in the background whenever the JSON or screenshots
  directory changes, and enable Render only after validation succeeds.
- Expose output format, resolution fraction, explicit output width, blend
  mode, memory budget, and incomplete-session handling.
- Show validation errors and renderer progress without making the application
  appear frozen; disable conflicting controls while rendering and provide a
  safe cancel path.
- Remember the most recently selected directories for the current user,
  without embedding machine-specific paths in capture metadata.

Acceptance:

- A completed CET session can be selected, validated, and rendered without
  invoking a terminal.
- Rendering presents responsive determinate progress and a clear terminal
  success/failure state.
- GUI output is byte-for-byte or pixel-equivalent to a CLI render using the
  same options.
- Invalid paths, incomplete sessions, and renderer failures are shown as
  actionable messages, without a traceback-only failure.

Status (2026-08-21): initial Tkinter frontend implemented as
`pano-stitch-gui`, including file/directory pickers, all current render
options, automatic background validation, Render gating, a 1–100% resolution
slider, determinate progress, coverage output, and cooperative cancellation.
Automated tests and static checks pass; Windows GUI
smoke testing remains.
WSLg uses a 1.5 default Tk scale, with `PANO_STITCH_GUI_SCALE` available as an
override for other display configurations.
WSLg font rendering remains a known development-host limitation; defer further
WSL-specific typography work and validate DPI behavior in the native Windows
release executable.
The GUI also persists the last Capture JSON directory, screenshots directory,
and output directory in a per-user settings file using atomic replacement; this
behavior is verified across restarts. Selecting a new JSON refreshes the
screenshots directory from its recorded absolute paths, even when an older
directory was persisted. On native Windows, a blank screenshots field may be
inferred from the CET JSON. Remaining work is native Windows smoke testing.
The latest path-normalization fix was included in the final native Windows
smoke test and verified.
The GUI defaults output names to `panorama-<session-id>.<format>`, asks
before replacing an existing output, and keeps advanced render controls behind
an expandable section while leaving format and resolution visible.
JPEG is available as an SDR export with a visible 1–100 quality slider (default
95) and is the default export; it uses a temporary disk-spooled RGB raster during encoding so rendering
does not require a second full panorama in RAM. PNG remains lossless SDR and
EXR preserves HDR data.

Packet 10 acceptance (2026-08-21): native Windows GUI verification is complete,
including validation gating, rendering, persistence, path inference and
normalization, overwrite confirmation, advanced-options collapse, and the
100% default resolution. JPEG default export at quality 95 and the render-time
form lock were also verified.

### Packet 11: packaging and documentation

Public-release note: defer native Windows packaging until the project is
ready for public distribution. During development and private testing, the
stitcher may continue to run from the documented Python virtual environment.
When release work begins, produce a Windows x64 one-folder bundle (with an
optional one-file convenience build), bundle OpenEXR/NumPy/OpenCV/Pillow and
the schema, and validate it on a clean Windows system without Python or WSL.

Deliverables:

- Build an installable Cyberpunk mod archive.
- Build the Python package and document installation.
- Document ReShade configuration and manual/automatic workflows.
- Document supported versions, incompatibilities, and recovery steps.
- Provide HDR EXR output and a PNG output option; document the verified color encoding and any explicitly selected conversion.
- Add a release verification checklist.

Acceptance:

- A clean user installation can follow the guide end to end.
- All native and Python checks pass without warnings.
- Package contents contain no build artifacts or developer-specific paths.

Status (2026-08-21): shareable artifacts are now release-tag-only GitHub
Actions outputs. The release workflow runs the stitcher checks on a GitHub
Windows runner, builds the ReShade add-on with MSVC x64, calls
`release/build-windows-release.ps1` to produce the mod and one-folder stitcher
archives, validates their contents, writes SHA-256 checksums, and publishes the
assets plus a build-provenance manifest to the matching GitHub Release. CI runs the independent stitcher checks
on pull requests and `master`. The root guide now covers installation,
automated capture, stitching, recovery, compatibility limits, release
verification, tag-driven publication, and the separate local development loop.
A manual, non-publishing dry run has built both ZIPs successfully; the packaged
GUI has launched and rendered JPEG and EXR outputs. The distribution now keeps
the schema inside the PyInstaller bundle, logs GUI worker failures to the user
configuration directory, and avoids broad collection of direct imports, saving
about 8 MiB while retaining the dynamically loaded OpenEXR and Imath modules.
The remaining Packet 11 gate is an independent clean-machine installation from
the downloaded artifact; only then should the first tag be pushed.

### Packet 12: exposure normalization, capture settings, and release review

Purpose: make exposure normalization part of every production stitch, provide
capture-only settings through Native Settings when available, and remove
development noise before the first public release. This packet covers all three
runtime parts: stitcher, CET mod, and ReShade add-on.

#### 12A. Automatic exposure normalization

Implement a linear-HDR prepass before compositing. For every capture session:

1. Decode each source one at a time, make a small linear-light proxy, and keep
   the prepass under the existing 1 GiB process-RSS budget. Preserve both
   supported inputs: SDR sRGB images convert to linear light before estimation;
   HDR PQ/Rec.2020 images retain the existing linear-HDR conversion path.
2. Use metadata camera bases and FoV to sample only geometric overlaps. Reject
   near-black, clipped, high-gradient, and otherwise unreliable luminance
   samples.
3. Estimate robust log-luminance differences for overlap edges; solve a
   weighted least-squares graph for one global RGB-preserving gain per source.
   Anchor the solution to the representative median-exposure frame and clamp
   gains to plus or minus one EV.
4. Require a connected, sufficiently sampled graph. Refuse rendering with a
   precise diagnostic rather than silently outputting unnormalized seams.
5. Apply each gain to linear RGB immediately after decoding and before feather
   blending, SDR tone mapping, JPEG/PNG output, or EXR output.

The GUI and CLI report the anchor, sample/edge count, and final gains. This is
automatic for production renders; no user-facing enable toggle is added in this
packet. Add synthetic SDR and HDR tests with known per-frame exposure offsets,
an unreliable-overlap rejection case, and a disconnected-graph failure case.

#### 12B. Optional Native Settings UI integration

Native Settings is an optional enhancement, not a packaging dependency. During
`onInit`, call `GetMod("nativeSettings")`; if it is absent or incompatible, the
mod retains its built-in defaults and capture remains usable.

When present, register a `Panorama Capture` tab and `Capture` subcategory with:

- `Capture FoV`: optional 30–120 degree FoV applied directly to the standalone
  capture camera. Until the slider is changed, capture planning uses the active
  in-game FoV without an override; no FOV Control helper is required.
- `Settling delay`: 0.1–3.0 seconds, step 0.1, default 1.0. It is the time
  after a verified camera move and before a screenshot request, for temporal
  accumulation; it does not delay pitch-correction attempts.
- `ReShade toast cooldown`: 3.1 seconds by default, ensuring the screenshot
  notification has expired before the next request. Camera rotation and
  temporal settling overlap this cooldown.
- Native Settings restore-defaults resets these controls while idle. Active
  sessions retain their immutable start-of-session snapshot.

Persist settings in `settings.json` beside the CET script; never hard-code a
game installation path. First prove `gameFPPCameraComponent`
FoV override/readback and restoration in-game before making it the production
path. If verification fails, abort before hiding the HUD or moving the camera.
Do not alter existing CET bindings.

The capture and stitcher path must support arbitrary display and screenshot
aspects, including 4:3, 16:9, 16:10, 21:9, and 32:9. Use the active camera
aspect ratio for horizontal-to-vertical FoV conversion and plan construction;
write the observed horizontal and vertical FoV into metadata as today. Do not
letterbox, crop, or normalize source images merely to fit a preferred aspect.
Add planner and projection tests for the listed ratios, verifying complete
sphere coverage, finite map coordinates, correct output dimensions, and a
different recomputed screenshot count where geometry requires one.

#### 12C. User-facing logs and three-part review

- CET console: production capture emits only session started, completed,
  aborted, or errored messages. Per-pose motion, metadata, bridge
  acknowledgements, and HUD
  internals move behind a disabled development-only logger; metadata JSON is the
  authoritative per-pose record.
- ReShade add-on: retain startup and error/timeout logging; remove routine
  per-screenshot request/acknowledgement noise from normal logs.
- Stitcher: retain its GUI log file and user-visible errors; include exposure
  normalization diagnostics in the render report without per-pixel logging.
- Review the CET state machine and restoration paths, bridge token/file cleanup
  and add-on shutdown, renderer memory/resource ownership, error propagation,
  packaged-file layout, and tests. Fix release-blocking findings within this
  packet and record non-blocking limitations in `docs/progress.md`.

Acceptance:

- Deliberately exposure-shifted SDR and HDR captures stitch without visible
  overlap brightness steps, while EXR remains linear HDR and JPEG/PNG remain
  deterministic SDR conversions.
- The exposure prepass stays within the established memory ceiling and refuses
  insufficient coverage with a clear diagnostic.
- With Native Settings installed, settling and toast-cooldown controls apply to
  the next capture, restore correctly after complete/abort/reload, and reset to
  defaults. Without it, capture works unchanged.
- 4:3, 16:9, 16:10, 21:9, and 32:9 captures use their recorded camera aspect
  without cropping or 16:9-specific plan assumptions.
- The normal CET console contains no per-pose spam, while development logs and
  metadata still support diagnosis.
- Stitcher, add-on, and CET review findings are either fixed or documented.

#### Deferred Packet 13: optional upright / horizon correction

After exposure normalization is proven, add a low-resolution equirectangular
preview that proposes (but never silently applies) a global upright correction
from rectilinear line evidence. Preserve manual pitch/roll controls and default
to zero correction for weak or contradictory scenes. Research remains in
`docs/polish-research.md`.

## MVP completion definition

The MVP is complete only when:

1. The mod captures deterministic yaw and pitch rows covering the full sphere from one optical center.
2. It associates every screenshot with explicit FoV and pose metadata.
3. The stitcher converts that session to a covered 2:1 equirectangular output without unintended bit-depth or dynamic-range loss. The proof-of-concept HDR target is EXR; the final release also provides PNG output.
4. A 30-frame 3840×2160 HDR render remains below 1,000,000,000 bytes peak process RSS.
5. Automated and in-game acceptance tests pass.
6. Abort, retry, screenshot timeout, and camera restoration are safe.

Horizontal-only captures remain useful diagnostics, but they do not satisfy MVP completion.

## Explicit non-goals for the first implementation

- Building a general-purpose free-camera mod
- Replacing ReShade
- Automatic image alignment or feature matching
- Dynamic-scene ghost removal
- Exposure or color correction
- Optimized polar frame placement
- Automatic UI hiding
- Final HDR-to-PNG appearance tuning beyond a documented, deterministic conversion

Keeping these out of the MVP is necessary to make the work suitable for sequential implementation by a lower-cost model while preserving a testable path to the complete result.
