# WebView2 UI-only POC

This isolated Windows experiment evaluates WebView2 for the panorama stitcher's interface. It is
not part of the native application build and contains no session discovery, stitching, rendering,
deletion, or publication code.

The single implemented screen follows `C:\dev\native-gui-prototype\proto-1.html`'s Input design.
Controls are interactive only at the presentation layer. The optional preview benchmark transfers
a generated 2048x1024 RGBA buffer through WebView2's shared-buffer API and presents it on a canvas.
It measures the proposed native-to-web presentation boundary without invoking the D3D12 backend.
The Game and Screenshots browse buttons use native Windows folder pickers through a structured
WebView2 message bridge. Settings and Options use HTML dialogs; their sample fields are not wired
to production settings.

## Build

From an x64 Visual Studio developer PowerShell:

```powershell
.\build.ps1
```

The script pins `Microsoft.Web.WebView2` SDK `1.0.4191.47` under the POC build directory. The SDK is
a build dependency only. The executable uses the shared Evergreen WebView2 Runtime installed on
Windows 11.

## Run

```powershell
.\build\Release\panorama-webview2-ui-poc.exe
```

The window starts with a 1100x780 logical-pixel client area, has a 920x680 logical minimum, and
follows per-monitor DPI changes without bitmap scaling. WebView zoom remains 1.0. By default its
browser profile is stored under `%LOCALAPPDATA%\PanoramaStitcher\WebView2Poc`; no `.WebView2`
directory is created beside the executable.

Useful diagnostic options:

```text
--user-data-dir PATH  Isolate the WebView2 browser profile.
--report PATH         Write native readiness and preview-transfer timing JSON.
--benchmark           Transfer and display a generated 8 MiB RGBA preview.
--native-probe        Reserve a sibling native HWND strip beside WebView2.
```

## Measure

Use a new output directory; the script refuses to overwrite prior evidence:

```powershell
.\measure.ps1 -OutputDirectory C:\dev\webview2-ui-poc-measurement-1
```

The report includes:

- executable and deployed POC payload size;
- cold and warm DOM-ready time;
- peak and mean process-tree working set and private commit;
- normalized idle CPU;
- GPU engine, dedicated-memory, and shared-memory counters when available;
- shared-buffer fill, post, JavaScript canvas presentation, and end-to-end timing;
- UI Automation descendant names and control types;
- a sibling-native-HWND accessibility/capture probe;
- screenshots of the Input screen, preview-transfer overlay, and native sibling layout.

The process-tree totals include the host and all descendant `msedgewebview2.exe` processes. They do
not include the disk size of the shared Evergreen Runtime because it is an operating-system/shared
dependency. The synthetic preview benchmark does not prove D3D12 interop or production panorama
correctness; it quantifies one plausible CPU/shared-memory presentation route. A production decision
still requires choosing between that route and a native preview surface beside the web UI.

## Measured baseline

The DPI-aware POC was measured on the current Windows 11/NVIDIA development machine on 2026-08-30
with WebView2 Runtime `151.0.4129.107`. The complete machine-local report and captures are
preserved in `C:\dev\webview2-ui-poc-measurement-20260830-r6`.

| Measurement | Result |
| --- | ---: |
| Executable / deployed POC payload | 0.101 MiB / 0.109 MiB (2 files) |
| Effective DPI / WebView raster scale / zoom | 96 / 1.0 / 1.0 |
| Client dimensions, physical / logical | 1099x778 / 1099x778 |
| Cold / warm external ready | 414 ms / 362 ms |
| Cold idle process-tree working set, mean / peak | 353.4 MiB / 355.4 MiB |
| Warm idle process-tree working set, mean / peak | 339.0 MiB / 343.3 MiB |
| Warm idle private commit, mean / peak | 368.5 MiB / 369.5 MiB |
| Warm idle normalized CPU, mean / peak | 0.18% / 0.73% |
| Warm idle dedicated / shared GPU memory | 27.4 MiB / 2.0 MiB |
| 8 MiB preview native fill / post call | 2.7 ms / 0.03 ms |
| 8 MiB preview native-to-present / JavaScript copy-and-present | 8.3 ms / 5.2 ms |
| Post-preview working set, mean / peak | 370.8 MiB / 373.7 MiB |
| Post-preview dedicated / shared GPU memory | 38.6 MiB / 10.1 MiB |

Idle GPU engine utilization was 0% in both samples. The host and WebView2 runtime used seven
processes. Windows UI Automation found the WebView panes and the sibling native control, but did
not expose the HTML controls in this default configuration; accessibility must therefore be solved
and re-tested before adopting this architecture. The 160-pixel native sibling and WebView rendered
together without overflow at the revised default size.

The same HTML was also rendered by headless Edge at the reported 1099x778 client size. Comparing
every embedded-client RGB pixel against that browser reference produced 0.0000% changed pixels and
a 0.0000 mean absolute channel delta. The earlier softness was DPI virtualization in the host and
capture path, not a difference in WebView2's renderer.

A separate launch on the 150% primary monitor reported DPI 144, a 1650x1170 physical client for
exactly 1100x780 logical pixels, WebView raster scale 1.5, and zoom 1.0. This verifies both monitor
scales used by the current development system.

## Production packaging notes

The HTML remains external in this POC so it is easy to inspect and revise. A production build can
embed HTML, CSS, JavaScript, and icons as Windows resources and serve them to WebView2 from memory,
leaving only the executable in the application payload. This prevents casual editing but is not a
security boundary: a determined user can still extract embedded resources or inspect rendered
content. Native code must continue to validate every message and allow-list every operation.

Microsoft Edge Stable is not a supported production backing runtime for WebView2. Production must
detect the separate Evergreen WebView2 Runtime and arrange installation when it is absent. Windows
11 includes it and the vast majority of eligible Windows 10 systems have it, but stripped, offline,
LTSC, enterprise-managed, or damaged installations remain possible. The recommended small-package
path is an Evergreen bootstrapper in the installer plus a clear startup error; a bundled Fixed
Version Runtime is more deterministic but substantially increases distribution size.
