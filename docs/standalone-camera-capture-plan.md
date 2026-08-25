# Standalone camera capture implementation plan

## Objective

Replace PanoramaCapture's player/FPP-camera pose control with a spawned standalone camera entity whose world position remains fixed for the entire panorama. Control its orientation and FoV directly, record its position per frame, and remove the FPP-specific FOV Control integration.

Do not depend on Appearance Menu Mod. Reimplement only the verified lifecycle using the built-in `base\entities\cameras\simple_free_camera.ent` entity. This requires CET and Codeware's `exEntitySpawner`.

## Scope and non-goals

This packet changes capture-side camera ownership only. Preserve the current planner, ReShade mailbox, screenshot acknowledgement flow, metadata publication, settling, time dilation, HUD hiding, player/weapon mesh hiding, input restrictions, and stitcher behavior.

Do not:

- import or call AMM modules;
- keep the old FPP path as an automatic fallback;
- move or teleport the spawned camera after its initial placement;
- use incremental Euler rotations;
- add feature matching or stitcher-side alignment;
- change screenshot naming or bridge protocols;
- change existing CET input action names or ordering.

## Verified reference behavior

AMM creates a standalone camera as follows:

1. Copy a world transform and set its position.
2. Call `exEntitySpawner.Spawn("base\\entities\\cameras\\simple_free_camera.ent", transform, '')`.
3. Poll `Game.FindEntityByID(entityID)` until the entity exists.
4. Obtain `entity:FindComponentByName("camera")`.
5. Set FoV with `component:SetFOV(value)`.
6. Set orientation with `component:SetLocalOrientation(quaternion)`.
7. Activate with `component:Activate(blendSeconds, false)`.
8. Deactivate with `component:Deactivate(blendSeconds, false)`.
9. Restore the player camera with `player:GetFPPCameraComponent():Activate()`.
10. Destroy the standalone entity with `handle:Dispose()`.

Reference source: installed AMM `Modules/camera.lua`, especially `Spawn`, `Activate`, `Deactivate`, `SetFOV`, and `Despawn`.

## Files to change

- `mod/cet/PanoramaCaptureProbe/init.lua`
- `mod/cet/README.md`
- `mod/README.md`
- root `README.md` only where dependencies or capture behavior are described
- `PLAN.md` and `docs/progress.md` only after implementation and in-game verification

Do not change the stitcher or ReShade add-on in this packet.

## Step 1: remove FOV Control ownership

In `mod/cet/PanoramaCaptureProbe/init.lua`:

1. Remove `fovControlInstalled` state and `FOV_APPLY_TIMEOUT_SECONDS`.
2. Remove `detectFovControl`, `fovControlNativeCall`, `fovSchedulerActive`, `cameraInternalFov`, and the current FPP `setDisplayFov` implementation.
3. Remove the FOV Control lock/unlock and restoration branches from capture setup and cleanup.
4. Preserve the Native Settings capture-FoV option, but make it control the standalone camera component directly.
5. If no configured capture FoV exists, use the current active camera's observed vertical FoV as the standalone camera target.
6. Update configuration validation and status text to stop referring to FOV Control.

Expected result: no runtime reference to `FovControl` remains.

## Step 2: add standalone-camera state

Add a single capture-owned structure to the environment transaction. Suggested fields:

```lua
standaloneCamera = {
    entityID = nil,
    handle = nil,
    component = nil,
    referencePosition = nil,
    targetVerticalFov = nil,
    spawnElapsed = 0.0,
}
```

Keep this state inside the existing environment/session transaction. Do not expose it globally beyond what shutdown cleanup requires.

Add constants:

```lua
local STANDALONE_CAMERA_PATH = "base\\entities\\cameras\\simple_free_camera.ent"
local CAMERA_SPAWN_TIMEOUT_SECONDS = 3.0
local CAMERA_POSITION_TOLERANCE = 0.001
local CAMERA_FOV_TOLERANCE_DEGREES = 0.05
```

Treat world-position units as engine units in metadata until an in-game measurement proves they are metres. Do not append a unit suffix to field names.

## Step 3: guard dependencies and conflicting cameras

Before changing the environment:

1. Verify `exEntitySpawner` exists and exposes `Spawn`.
2. Reject capture with an actionable message if Codeware/entity spawning is unavailable.
3. Allow FreeFly; the verified standalone camera does not rotate the player or borrow FreeFly's camera.
4. Reject Photo Mode as today.
5. If AMM is loaded and `AMM.Director.activeCamera` is non-nil, reject capture.
6. Do not deactivate or dispose a camera owned by another mod.

The error must name the conflict and leave the current camera untouched.

## Step 4: snapshot the real active camera origin

Immediately before spawning:

1. Read the active camera basis and FoV through `Game.GetCameraSystem()`.
2. Try `GetActiveCameraWorldTransform()` and extract its position.
3. If that readback is unavailable in normal FPP, derive the initial standalone-camera position from the FPP camera component or use the player's position plus the current camera offset only after an explicit in-game probe proves the result matches the rendered viewpoint.
4. Do not silently substitute the player position. If no trustworthy optical-center position is available, cancel capture with a diagnostic error.
5. Preserve the original FPP component and orientation only for restoration; do not use them for subsequent panorama poses.

Add a development log containing the chosen initial camera position and its source API.

## Step 5: spawn asynchronously

Create `beginStandaloneCameraSpawn(environment)`:

1. Copy a safe world transform.
2. Set its position to the observed optical center.
3. Set a deterministic base orientation. Prefer an identity/entity orientation plus component-local panorama orientation. If the entity must inherit the player's heading, record that heading and consistently compose all local quaternions relative to it.
4. Call `exEntitySpawner.Spawn` once.
5. Store the returned entity ID.
6. Enter a new `camera_spawn_pending` state.

In `onUpdate` while pending:

1. Call `Game.FindEntityByID(entityID)`.
2. Once found, obtain `FindComponentByName("camera")`.
3. Reject a nil component.
4. Save `handle`, `component`, and an immutable copy of `handle:GetWorldPosition()` as `referencePosition`.
5. Apply target FoV with `component:SetFOV(targetVerticalFov)`.
6. Apply the first absolute pose quaternion.
7. Activate with `component:Activate(0, false)`.
8. Advance to a short activation/readback state.
9. Cancel and clean up if the timeout expires.

Never call the spawn API repeatedly while polling.

## Step 6: define absolute orientation composition

Implement one helper that converts the planned panorama-relative yaw and pitch into the component-local quaternion. Use engine `EulerAngles`/quaternion APIs consistently with the current observed basis convention.

Requirements:

1. Every pose is computed from the unchanged base orientation.
2. No pose depends on the preceding pose.
3. Roll remains zero relative to the panorama frame.
4. Do not clamp pitch using AMM's manual-control ±85-degree rule. Planned row centers may approach the poles.
5. Do not use the player's yaw teleport or FPP `SetLocalOrientation` anywhere in production capture.

Add a development-only three-pose probe before enabling full capture:

- yaw 0°, pitch 0°;
- yaw 90°, pitch 0°;
- yaw 0°, pitch +45°.

For each, verify the active-camera basis direction and sign. Correct composition order before proceeding; do not compensate in the stitcher.

## Step 7: verify activation, FoV, pose, and fixed position

After activation and after every pose change, wait for the existing transition/settling sequence, then perform one authoritative readback immediately before requesting a screenshot:

1. `handle:GetWorldPosition()` from the owned camera entity.
2. Active forward/right/up from `Game.GetCameraSystem()`.
3. Active vertical FoV from `GetActiveCameraFOV()`.
4. Aspect ratio and derived horizontal FoV.
5. Display resolution as viewport fallback.

Calculate Euclidean displacement from `referencePosition`.

Reject the pose and abort safely when:

- displacement exceeds `CAMERA_POSITION_TOLERANCE`;
- observed FoV differs by more than `CAMERA_FOV_TOLERANCE_DEGREES`;
- the basis is non-orthonormal;
- pitch/yaw differs from the planned pose beyond the existing pose tolerance;
- the standalone component is no longer the active camera.

Do not merely log these failures and continue capturing.

## Step 8: simplify per-pose application

Replace the current player-teleport/FPP pitch-correction loop:

1. Select the planned pose.
2. Set the standalone component's absolute local orientation once.
3. Wait the existing fixed transition frames.
4. Verify the observed basis.
5. If a bounded retry is retained, reapply the same absolute quaternion; never add an error correction incrementally.
6. Run the existing settling timer.
7. Refresh authoritative metadata immediately before the screenshot request.

Remove FPP pitch calibration state if the standalone camera follows the requested quaternion accurately.

## Step 9: metadata changes

For every pose, require and serialize:

- `camera_position` from the owned entity handle;
- `camera_displacement` from the first pose/reference position;
- observed `forward`, `right`, and `up`;
- observed horizontal and vertical FoV;
- viewport dimensions and source;
- screenshot path and settling time.

Position is no longer optional for standalone-camera sessions. Failure to read it cancels capture.

Continue validating the complete temporary JSON with `json.decode` before atomic publication. Preserve the last valid JSON and `.tmp` diagnostics on serialization failure. Aborted sessions must remain published with `state: "aborted"`.

Projection/view matrices remain optional because CET has not exposed verified APIs for them. Do not invent method names or claim they were captured.

## Step 10: teardown must be idempotent

Implement one `destroyStandaloneCamera(environment)` helper used by every exit path:

1. If the owned component exists and is active, call `Deactivate(0, false)` inside `pcall`.
2. Reactivate the original player FPP camera inside `pcall`.
3. Dispose only the entity handle created by this capture.
4. Clear entity ID, handle, component, and reference position.
5. Calling the helper twice must be harmless.

Call it on:

- successful completion;
- user abort;
- screenshot timeout/error;
- metadata failure;
- spawn or activation timeout;
- invalid pose/FoV/position readback;
- player replacement/detach;
- game/menu transition paths that currently abort capture;
- CET `onShutdown`.

Restore HUD, meshes, input restrictions, time dilation, and other environment state even if camera destruction fails. Log the cleanup failure without skipping the remaining restoration steps.

## Step 11: documentation and dependency update

Update documentation to state:

- Codeware is required for standalone camera spawning.
- AMM is not required.
- FOV Control is no longer required and should be removed from setup instructions.
- AMM and other conflicting custom cameras must be inactive during capture; FreeFly is supported.
- Capture uses a fixed standalone optical center and records its position per frame.

Do not remove FOV Control files from a user's game installation; only remove this project's integration and documentation dependency.

## Step 12: static and in-game verification

Static checks:

1. Search confirms no production reference to `FovControl` remains.
2. Search confirms production pose application no longer teleports the player or changes the FPP camera orientation.
3. JSON serializer validation remains in place.
4. Existing bridge action names and protocol remain unchanged.
5. `git diff --check` passes.
6. Run any available Lua linter/parser. If unavailable, report that explicitly.

In-game smoke test:

1. Start capture and confirm the view switches to the spawned camera.
2. Abort before the first screenshot; verify FPP restoration and no orphan camera.
3. Start again and capture three poses.
4. Confirm JSON is valid and contains camera positions/displacements.
5. Confirm all displacements are below tolerance.
6. Reload CET during an active session and verify FPP restoration/no orphan camera.
7. Trigger screenshot timeout and verify aborted JSON survives.

Full acceptance test:

1. Capture a static indoor scene with close geometry using the same settings that produced duplicated edges.
2. Verify every frame reports a stable optical center.
3. Render both hard and feather panoramas.
4. Inspect equator, zenith, and nadir at 100% scale.
5. Require no repeated high-contrast edges attributable to pose overlap.
6. If duplication remains despite stable position, perform projection/FoV calibration next; do not add stitcher feature alignment in this packet.

## Required handoff report

The implementing model must report:

- exact files changed;
- the chosen active-camera position API and observed value;
- quaternion composition order and three-pose probe results;
- measured maximum optical-center displacement;
- observed requested versus effective FoV;
- abort, timeout, completion, and shutdown cleanup results;
- valid JSON sample path;
- hard/feather panorama comparison;
- unavailable tests or unresolved runtime APIs.
