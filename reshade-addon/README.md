# PanoramaCapture ReShade add-on

This Packet 7 proof uses the ReShade v6.7.3 API 18 headers vendored in
`deps/reshade-api`. Build the 64-bit target with Visual Studio and install the
resulting `PanoramaCaptureReShade.addon64` beside `ReShade.ini`.

From an x64 Native Tools prompt in the project checkout:

```bat
cmake -S reshade-addon -B C:\build\panorama-reshade-addon -G Ninja
cmake --build C:\build\panorama-reshade-addon --config Release
```

Copy `C:\build\panorama-reshade-addon\PanoramaCaptureReShade.addon64` to
the ReShade installation directory next to `dxgi.dll` and `ReShade.ini`.
ReShade must report API 18 compatibility; do not replace the vendored headers
with the current upstream `main` headers.

The test trigger is F10. On the next ReShade present callback it calls
`effect_runtime::save_screenshot()` with a correlation token. A matching
`reshade_screenshot` callback writes the exact saved path to the ReShade log:

```text
PanoramaCaptureReShade: screenshot saved token=pano-test-000001 path=...
```

If ReShade does not emit a matching completion event within 10 seconds, the
add-on logs a timeout and returns to idle so the next test can be retried.

The add-on also implements the Packet 8 control bridge. It does not inspect
the screenshot directory and does not simulate Print Screen: CET writes an
atomic tab-separated `PanoramaCaptureBridge.request` in its own mod folder,
and the add-on resolves that folder beneath ReShade's base directory. It
writes `PanoramaCaptureBridge.ack` with the exact saved path after ReShade
reports the screenshot event (or `ERROR:timeout` on failure). Set
`automatedScreenshots = true` and `bridgeDirectory = "."` in the CET mod to
enable it; leave automation disabled for the manual workflow.
