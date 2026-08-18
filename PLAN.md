# Cyberpunk 2077 Panorama Taker — Implementation Plan

## Objective

Build two pieces of software:

1. `PanoramaCapture`: a Cyberpunk 2077 mod that positions the camera at precise, repeatable angles and coordinates screenshot capture.
2. `pano-stitch`: a command-line script that uses the captured images and their metadata to produce a lossless equirectangular PNG.

The repository is initially empty, so this plan assumes a new project with no existing compatibility constraints.

## Scope clarification

A yaw-only sequence produces a 360-degree horizontal band, not a complete spherical panorama. A complete 2:1 equirectangular panorama requires multiple pitch rows covering both poles.

Support two modes:

- `horizontal`: yaw-only, producing a cropped 360-degree equirectangular band.
- `full_sphere`: yaw and pitch coverage from -90 to +90 degrees, producing a standard 2:1 equirectangular image.

Implement `horizontal` first, but design the metadata and camera controller for `full_sphere` from the beginning.

## Recommended architecture

### PanoramaCapture mod

Treat the mod as one product containing:

- A RED4ext C++ plugin for exact final-camera transform and FoV control.
- A small Cyber Engine Tweaks Lua front end for configurable bindings and user-facing status.

RED4ext is the appropriate camera layer because its SDK exposes game structures and native scripting integration. It also supports runtime compatibility declarations so an unsupported game update can be rejected instead of risking a crash.

References:

- [Creating a RED4ext plugin](https://docs.red4ext.com/mod-developers/creating-a-plugin)
- [RED4ext SDK capabilities](https://docs.red4ext.com/mod-developers/red4ext-and-red4ext.sdk)
- [RED4ext custom native classes](https://docs.red4ext.com/mod-developers/creating-a-custom-native-class)
- [CP2077 FOV Control reference implementation](https://github.com/koryboc/CP2077-FovControl)

Use CET `registerInput` for configurable bindings:

- Start capture session
- Capture/advance in manual mode
- Abort and restore camera

CET hotkeys fire on release and can fail while another game binding is held, so `registerInput` is preferable even though the user-facing concept is still a hotkey.

Reference: [CET hotkey documentation](https://wiki.redmodding.org/cyber-engine-tweaks/cet-functions/hotkeys/registerhotkey)

### pano-stitch command-line tool

Use Python 3.12 with:

- NumPy for projection calculations
- OpenCV for image remapping
- Pillow for PNG input/output
- pytest for tests
- Ruff for linting and formatting
- mypy for type checking

The stitcher must use recorded camera geometry. Do not use image feature matching in the MVP.

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
      "status": "captured"
    }
  ],
  "completed": true
}
```

Contract requirements:

- Store both horizontal and vertical FoV to eliminate convention ambiguity.
- Record the final observed projection and view matrices.
- State matrix layout, handedness, axis directions, quaternion order, and angle signs in the schema descriptions.
- Record commanded yaw, pitch, and roll as canonical angles relative to the panorama origin.
- Keep screenshot filenames relative to the manifest.
- Include the game version, RED4ext version, mod version, and UTC timestamps.
- Rewrite the manifest atomically after each successful frame: write a temporary file, flush it, and replace the old manifest.
- Never mark a frame as captured until its screenshot exists, has stopped growing, and can be decoded.
- Mark incomplete, aborted, and failed sessions explicitly.

## PanoramaCapture design

### Phase 0: camera-control feasibility spike

This is a mandatory gate before implementing the complete mod.

1. Pin the exact Cyberpunk 2077, RED4ext, and RED4ext.SDK versions.
2. Locate the current active render camera using current SDK types and NativeDB.
3. Log its position, orientation, FoV, viewport, and projection matrix.
4. Override its final position and orientation after the ordinary game camera update.
5. Verify that the game does not overwrite that transform before rendering.
6. Change FoV and verify the effective value in the final projection matrix.
7. Confirm that the camera can be restored safely.
8. Reject unsupported game versions clearly.

Existing camera and FoV projects may be used as references, but do not copy version-specific offsets or signatures without validating them against the pinned runtime.

Spike acceptance criteria:

- Optical-center movement below 1 mm between rotations.
- Commanded versus observed orientation error below 0.01 degrees.
- FoV remains fixed across at least 300 rendered frames.
- Restoration returns the camera to its original pose and FoV.
- Unsupported game versions fail with a useful log message rather than crashing.

Do not begin the production camera controller until this spike passes inside the actual game.

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

Use every yaw position in every pitch row initially. This is redundant near the poles but simple and conservative. Optimizing the number of polar frames is a later enhancement.

Generate and validate the complete shot list when the session starts. Store it in the manifest before taking the first screenshot.

### Camera origin and positioning

When starting a session:

1. Snapshot the current camera position, orientation, FoV, and projection.
2. Treat that optical center and orientation as the panorama origin.
3. Apply the configured FoV.
4. Wait until the final projection reports the expected FoV.
5. Build every shot orientation relative to the origin.
6. Reapply the current absolute shot pose on every rendered frame.

Never rotate incrementally. Calculate every pose from the original snapshot:

```text
shotOrientation = baseOrientation * relativeYawPitchOrientation
shotPosition = basePosition
```

This prevents cumulative floating-point drift and ensures rotation occurs around the optical center.

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
  -> AwaitingScreenshotFile
  -> CommittingFrame
  -> ApplyingNextPose
  -> Settling
  -> ...
  -> Completed
```

Any active state may transition to `Aborted` or `Error`.

Rules:

- Ignore repeated or re-entrant commands while an operation is pending.
- Use both `settle_frames` and `settle_ms` before declaring a pose ready.
- Keep enforcing the absolute camera pose while settling and capturing.
- Do not rotate until screenshot completion has been confirmed.
- Restore the original transform and FoV after completion or abort by default.
- On screenshot timeout, remain at the current pose and allow retry or abort.
- Persist enough session state to diagnose an interrupted capture.

### Screenshot modes

#### MVP: manual mode

1. The mod moves and locks the camera.
2. The user invokes the ReShade screenshot binding.
3. The user presses Capture/Advance.
4. The mod locates the new screenshot, commits its metadata, and advances.

Only one screenshot may be awaiting association with a frame.

#### Automated mode

1. Snapshot the configured screenshot directory contents.
2. Simulate the configured screenshot key with Win32 `SendInput`.
3. Poll the directory for a new supported image.
4. If multiple new candidates appear, stop with an association error.
5. Wait until the new file's size is stable over at least two polls.
6. Verify that it can be decoded and has the expected dimensions.
7. Commit its filename and camera metadata.
8. Advance to the next pose.

ReShade's default screenshot key is Print Screen. Its normal input path queues the screenshot for a following frame, so a fixed sleep alone is not sufficient.

References:

- [ReShade screenshot implementation](https://github.com/crosire/reshade/blob/main/source/runtime.cpp)
- [ReShade API, including direct screenshot capture](https://github.com/crosire/reshade/blob/main/include/reshade_api.hpp)

ReShade exposes a direct `save_screenshot` API, but using it requires a ReShade add-on. Treat that as an optional later integration because it adds another native component and communication boundary.

### Configuration

Start with a JSON configuration similar to:

```json
{
  "capture_mode": "full_sphere",
  "screenshot_mode": "manual",
  "target_fov_deg": 100.0,
  "fov_axis": "horizontal",
  "overlap_fraction": 0.08,
  "settle_frames": 10,
  "settle_ms": 500,
  "screenshot_timeout_ms": 10000,
  "screenshot_directory": "D:/Screenshots/Cyberpunk",
  "screenshot_key_vk": 44,
  "restore_camera": true,
  "hide_ui": false
}
```

Validate at minimum:

- FoV is greater than zero and safely below 180 degrees.
- Overlap is non-negative and below a documented upper limit.
- Screenshot directory exists and is writable by ReShade.
- Timeouts and settling values are non-negative.
- Capture mode and screenshot mode use known values.

### UI hiding and stable-scene guidance

UI hiding is low priority. Initially recommend Photo Mode or an existing HUD-hiding mod rather than expanding the camera controller.

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

Convert sRGB values to linear RGB before feathering and convert back to sRGB before saving. Blending gamma-encoded values produces incorrect brightness.

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

Save an 8-bit RGB PNG initially. PNG compression is lossless; compression level affects file size and encoding time, not image fidelity. Preserve ICC metadata only if all source images use the same profile.

For the MVP, a full NumPy output buffer is acceptable with a documented output-size limit. If memory becomes a practical problem, add tiled rendering and streamed PNG writing as a separate optimization.

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

### Packet 4: RED4ext camera feasibility spike

Deliverables:

- Pin supported game and SDK versions.
- Build a minimal plugin that logs final camera parameters.
- Demonstrate absolute camera transform and FoV override.
- Demonstrate restoration.
- Document all verified APIs, hooks, and runtime assumptions.

Acceptance:

- Pass all feasibility criteria defined above in the actual game.
- No production state machine or screenshot automation yet.

Stop the project here if the spike cannot control the final render camera precisely. Investigate a different hook point or an established free-camera integration before continuing.

### Packet 5: production camera controller

Deliverables:

- Implement absolute pose locking from a base transform.
- Implement effective FoV application and readback.
- Convert engine coordinates into canonical metadata coordinates.
- Implement safe restoration and runtime guards.

Acceptance:

- No cumulative rotation drift.
- Position and orientation tolerances pass in game.
- Unsupported runtime behavior is safe and logged.

### Packet 6: CET frontend

Deliverables:

- Expose minimal native Start, Advance, Abort, and Status functions.
- Add configurable CET input bindings.
- Load and validate configuration.
- Show concise state/error notifications.

Acceptance:

- Bindings work while normal movement inputs are held.
- Invalid configuration prevents session start and explains why.
- Repeated commands cannot corrupt state.

### Packet 7: manual capture sessions

Deliverables:

- Implement the capture state machine.
- Create per-session output directories and manifests.
- Detect user-created screenshots in manual mode.
- Associate one image with one pose.
- Atomically update metadata and advance.

Acceptance:

- Horizontal panorama capture completes end to end.
- Timeout and ambiguous-file cases do not rotate the camera.
- Abort and completion restore the original camera.

### Packet 8: automated ReShade screenshots

Deliverables:

- Add configurable Win32 key simulation.
- Add screenshot directory snapshots and file-stability checks.
- Add decode and dimension validation.
- Add retry and timeout handling.

Acceptance:

- One automated run produces exactly one image per pose.
- Slow screenshot writes cannot cause premature rotation.
- Multiple new files produce a safe, recoverable error.

### Packet 9: full-sphere integration

Deliverables:

- Enable pitch-row execution in the mod.
- Verify stable orientations near the poles.
- Feed a complete capture into the stitcher.
- Add a coverage diagnostic image.

Acceptance:

- End-to-end output is a covered 2:1 equirectangular PNG.
- Pole orientation has no unexpected roll.
- No frame requires manual metadata or filename correction.

### Packet 10: packaging and documentation

Deliverables:

- Build an installable Cyberpunk mod archive.
- Build the Python package and document installation.
- Document ReShade configuration and manual/automatic workflows.
- Document supported versions, incompatibilities, and recovery steps.
- Add a release verification checklist.

Acceptance:

- A clean user installation can follow the guide end to end.
- All native and Python checks pass without warnings.
- Package contents contain no build artifacts or developer-specific paths.

## MVP completion definition

The MVP is complete only when:

1. The mod captures a deterministic horizontal 360-degree sequence from one optical center.
2. It associates every screenshot with explicit FoV and pose metadata.
3. The stitcher converts that session to a lossless cropped equirectangular PNG.
4. Automated and in-game acceptance tests pass.
5. Abort, retry, screenshot timeout, and camera restoration are safe.

Full-sphere capture is the next release milestone and must reuse the same manifest and projection pipeline rather than introducing a second format.

## Explicit non-goals for the first implementation

- Building a general-purpose free-camera mod
- Replacing ReShade
- Automatic image alignment or feature matching
- Dynamic-scene ghost removal
- Exposure or color correction
- Optimized polar frame placement
- A graphical desktop application
- Automatic UI hiding
- HDR or 16-bit output

Keeping these out of the MVP is necessary to make the work suitable for sequential implementation by a lower-cost model while preserving a testable path to the complete result.
