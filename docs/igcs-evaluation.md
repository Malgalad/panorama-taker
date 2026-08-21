# IGCS Connector evaluation

Verified: 2026-08-20 against IGCS Connector commit `bad4f50fb6b00475419546e28fade5022263e208`, the Cyberpunk 2077 camera-tool documentation, and an in-game panorama session.

## Definitive result

The Cyberpunk 2077 IGCS camera tools officially support IGCS Connector 2.0+. The connector controls the injected camera-tools DLL through a small in-process C ABI, not through ReShade-specific camera hooks or IPC. A native module in the game process can use the same mechanism by enumerating loaded modules, resolving the documented exports with `GetProcAddress`, and calling them directly.

This is suitable for a horizontal 360-degree diagnostic. It is not sufficient by itself for the project MVP because automated full-sphere capture requires pitch rows and the published ABI has no pitch-rotation or absolute-pose command.

## Published camera-tools ABI

The connector resolves these exports from the injected IGCS camera DLL:

- `IGCS_StartScreenshotSession(uint8_t type)`
- `IGCS_MoveCameraPanorama(float stepAngleRadians)`
- `IGCS_MoveCameraMultishot(float leftRight, float upDown, float fovDegrees, bool fromStartPosition)`
- `IGCS_EndScreenshotSession()`

Session type `0` is panorama. Starting a session asks the camera tools to preserve the initial camera state and reject incompatible states such as a disabled camera or active camera path. `IGCS_MoveCameraPanorama` applies relative horizontal rotation in radians. Ending the session restores camera state as required by the camera tools.

`IGCS_MoveCameraMultishot` translates the camera left/right and up/down; it does not rotate pitch. No published export sets an absolute position, quaternion, yaw, pitch, roll, or FoV.

## Stock connector capability

The existing ReShade add-on already implements horizontal panorama capture:

1. Read the current horizontal FoV from the camera-tools data buffer.
2. Compute `step = fov * (1 - overlap)`.
3. Start an IGCS panorama session.
4. Rotate to the left edge, capture frames with a configurable frame wait, and advance by the relative yaw step.
5. End the session and save the frames.

This is useful as an immediate compatibility test and reference implementation, but it does not meet the complete project contract:

- It captures ReShade screenshots into an 8-bit `uint8_t` RGBA buffer and writes 8-bit BMP, JPEG, or PNG.
- It writes numbered images but no per-frame pose/FoV manifest.
- It provides horizontal yaw capture only, not pitch rows for a 2:1 full sphere.
- Its camera moves are relative and accumulated rather than commanded as independently derived absolute poses.

Runtime confirmation: the stock connector completed a nine-frame panorama and restored the camera, but wrote baseline 8-bit JPEG files. Their HDR colors were incorrect. The known-good normal ReShade capture from the same setup is a 16-bit Rec.2020/PQ PNG. Choosing PNG in the connector cannot fix this because its capture buffer is already 8-bit before encoding.

## Integration conclusion

The published connector interface remains useful for horizontal panorama diagnostics, but it is not the selected full-sphere backend. The project returned to the verified normal FPP camera path because CET can command both yaw and pitch there, and CET-accessible time-dilation and HUD-hiding APIs can provide the missing capture environment.

If IGCS integration is revisited, the required bridge would:

1. Find the loaded IGCS camera-tools module and resolve the four exports exactly as IGCS Connector does.
2. Call `IGCS_StartScreenshotSession(0)` and fail clearly on its documented nonzero return codes.
3. Use `IGCS_MoveCameraPanorama()` for each calculated yaw step.
4. Read back the actual active transform, FoV, basis, and aspect ratio through the already verified CET `gameCameraSystem` APIs after every move.
5. Trigger ReShade's normal screenshot path so the existing 16-bit Rec.2020/PQ PNG workflow is preserved; do not use IGCS Connector's internal 8-bit screenshot capture.
6. Associate each detected screenshot with the read-back pose and write the session manifest atomically.
7. Always call `IGCS_EndScreenshotSession()` on completion or abort so IGCS can restore its saved state.
8. Require a supported pitch/absolute-pose operation before being considered for full-sphere production.

The bridge may be implemented as a ReShade add-on or an in-process native plugin. A ReShade add-on is the natural long-term location if it also owns screenshot callbacks. The existing RED4ext plugin can host a proof of concept because it is already loaded in the same process and can call ordinary Windows DLL exports, but it should remain isolated from REDengine startup camera queries.

CET Lua cannot directly call arbitrary native DLL exports, so CET alone cannot invoke this ABI.

Do not run the stock connector panorama controller and the custom bridge simultaneously; both would compete for the same IGCS screenshot session.

## Full-sphere limitation

The published IGCS Connector ABI cannot automate pitch rows. Full-sphere support therefore needs one of these before Packet 9:

- an additional supported IGCS export for pitch or absolute quaternion control;
- a documented IGCSClient camera-path interface that can be generated and stepped deterministically;
- cooperation from the IGCS author to extend the connector ABI; or
- a separate verified native camera-control implementation.

Undocumented offsets or internal functions in the paid camera DLL are not an acceptable release dependency. If the pitch experiment cannot meet tolerance, the IGCS route must not ship as a horizontal-only substitute for the requested full sphere.

## Runtime validation still required

With Cyberpunk running and the paid tools injected:

1. Enable the IGCS camera and confirm IGCS Connector reports `Camera tools connected`.
2. Run the stock connector's horizontal panorama test at 360 degrees and verify movement and restoration. Completed on 2026-08-20; movement and restoration work, but its output is unsuitable for HDR.
3. Inspect the injected Cyberpunk camera DLL export table to confirm the four documented exports in the installed version.
4. Prototype direct calls from a minimal native bridge and compare CET read-back angles against commanded steps.
5. Verify the regular ReShade hotkey still produces the known 16-bit HDR PNG during an IGCS session. This is now the required screenshot path for the prototype.

## Sources

- IGCS Connector repository: <https://github.com/FransBouma/IgcsConnector>
- Cyberpunk 2077 camera documentation: <https://opm.fransbouma.com/Cameras/cyberpunk2077.htm>
