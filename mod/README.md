# PanoramaCaptureProbe

This Packet 4 probe supports Cyberpunk 2077 2.31 and RED4ext v1.30.0. `PANORAMA_PROBE_STAGE` selects progressively riskier read-only diagnostics; stage 0 only registers a game-state callback. No stage changes the camera, UI, input, or screenshots.

## Build

Run these commands from the **x64 Native Tools Command Prompt for VS 2026**:

```bat
pushd \\wsl.localhost\<your-distro>\home\pogorelov\panorama-taker
cmake -S mod -B C:\build\panorama-taker-x64 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRED4EXT_USE_PCH=ON
cmake --build C:\build\panorama-taker-x64
popd
```

The DLL is `C:\build\panorama-taker-x64\PanoramaCaptureProbe.dll`.

## Manual deployment and verification

Close the game, then copy the DLL into this new directory (leave every existing RED4ext/Vortex file untouched):

```bat
mkdir "F:\GoG Games\Cyberpunk 2077\red4ext\plugins\PanoramaCaptureProbe"
copy /Y "C:\build\panorama-taker-x64\PanoramaCaptureProbe.dll" "F:\GoG Games\Cyberpunk 2077\red4ext\plugins\PanoramaCaptureProbe\"
```

Configure a stage explicitly when testing:

```bat
cmake -S mod -B C:\build\panorama-taker-x64 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPANORAMA_PROBE_STAGE=1
```

Stages 1–3 are the RTTI lookup, `ScriptGameInstance` construction, and deferred `GetCameraSystem` invocation. Stage 3 waits roughly 10 seconds of update frames before invoking the getter. Stages 4–6 then test `GetType()`, class-name conversion, and method lookups separately. Launch the game, load a save, and inspect `F:\GoG Games\Cyberpunk 2077\red4ext\logs\red4ext.log`.

```text
PanoramaCaptureProbe loaded for Cyberpunk 2077 2.31.0.
Camera probe: running stage 1.
Camera probe: RTTI lookup succeeded.
```

If a stage crashes, revert to the previous stage and provide the last log line. Delete only `PanoramaCaptureProbe.dll` to disable this probe.
