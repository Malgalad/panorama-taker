# Native-only cutover inventory

This inventory freezes the Step 24 final product boundary before deletion begins. It describes the
tracked tree as of 2026-08-31. “Retain” means the component remains in the final repository;
“replace, then delete” means its native replacement must pass before deletion; and “delete” means
no compatibility copy remains after Step 24.

Step 24 completed this inventory on 2026-09-01. The native fixtures now live under
`stitcher/native/tests/fixtures`; the application/package/test trees and transitional release paths
listed for deletion are removed; and the tracked `build-debug`/`build-release` trees are deleted.
The final archive and active-source policies enforce this boundary.

The final stitcher release has one application executable, `PanoramaCaptureStitcher.exe`, and one
backend library, `pano_gpu.dll`. Despite its historical name, `pano_gpu.dll` is the native D3D12
backend and contains no CUDA implementation. The native memory-bounded CPU compositor is linked
into the application. WebView2 is the only application frontend.

## Shipped products and runtime dependencies

| Current component | Final disposition |
| --- | --- |
| `pano-stitch-native-gui.exe` build target, renamed to `PanoramaCaptureStitcher.exe` in the archive | Retain as the only shipped stitcher executable. |
| `pano_gpu.dll` | Retain as the only shipped stitcher DLL and D3D12 backend. |
| Native CPU compositor in `stitcher/native/src/pano_app_cpu*.cpp` | Retain as the in-process, memory-bounded fallback. It is not a separate executable or DLL. |
| Embedded `pano_app_ui.*` and `pano_app_exposure.*` resources | Retain as the only application UI. |
| `PanoramaCaptureReShade.addon64` and the CET `PanoramaCaptureProbe` Lua mod | Retain in the separate mod archive; they are capture components, not alternate stitcher frontends. |
| `pano-stitch-native` | Retain in the repository as the native CLI/headless application target; do not add it to the GUI archive. |
| `pano_app_headless_probe` and native contract-test executables | Retain as test-only targets; do not ship them. |
| Python/PyInstaller `PanoramaCaptureStitcher.exe` | Delete after the native release gate passes. |
| `PanoramaCaptureStitcher-Python.exe` and `PanoramaCaptureStitcher-Native.exe` comparison pair | Delete; the final archive has no comparison entry points. |
| `--native-ui` inside the native GUI executable | Delete after the WebView replacements pass; it is not a final release mode. |

The final native build retains these source/build dependencies:

- vendored `yyjson` for session and bridge JSON;
- OpenEXR, Imath, OpenJPH, and libdeflate for native codecs, with their checked-in notices;
- the WebView2 Loader static library and its checked-in license/notice; the installed Evergreen
  WebView2 Runtime remains an external Windows prerequisite;
- Windows system APIs used by the host and D3D12 backend, including D3D12, DXGI, User32, Common
  Controls, OLE, Shell, Shlwapi, Version, and Delayimp;
- build-time FXC for checked-in HLSL sources embedded into `pano_gpu.dll`. No shader source,
  compiler DLL, CUDA library, or MSVC redistributable is shipped.

The final archive retains only the application executable, backend DLL, `README.md`, and the
required files under `licenses/`. Python executables and libraries, `_internal`,
`base_library.zip`, `.pyc`/`.pyd` files, Python packages, PyInstaller metadata, CUDA/CuPy/NVRTC
runtime files, loose shaders, PDBs, test executables, and comparison executables are forbidden.

## GUI surface classification

Application UI belongs in the embedded WebView document:

| Surface | Current implementation | Final disposition |
| --- | --- | --- |
| Input, Preview, and Output pages | Embedded WebView plus transitional hidden Win32 controls | Retain the WebView pages; replace and delete the hidden control adapter. |
| Exposure page | Embedded WebView in a native host window plus transitional native error paths | Retain the WebView page and host chrome; move application errors into WebView. |
| Edit Tag | `ModalKind::session_tag` Win32 modal | Replace with the WebView modal host, then delete. |
| Input Options | `ModalKind::input_options` Win32 modal | Replace with the WebView modal host, then delete. |
| Preview Options | `ModalKind::preview_options` Win32 modal | Replace with the WebView modal host, then delete. |
| App Settings | `ModalKind::app_settings` Win32 modal | Replace with the WebView modal host, then delete. |
| Delete-session/image confirmation | Native `TaskDialogIndirect`/`MessageBoxW` application dialog | Replace with WebView confirmation, then delete. |
| Overwrite-output confirmation | Native `MessageBoxW` application dialog | Replace with WebView confirmation, then delete. |
| Validation, render, exposure, command, and recoverable runtime notices/errors | Native `MessageBoxW` paths or inline status | Present through WebView modal/toast state, then delete post-start application message boxes. |

The following are operating-system services, not legacy application UI, and remain native:

- top-level window and Exposure-window chrome;
- the D3D12 preview child and its GDI CPU-fallback presentation child;
- filesystem folder pickers, which return selected paths to typed native state;
- taskbar progress and the native completion notification;
- the native WebView2 bootstrap prompt and runtime-download action, used only when WebView2 cannot
  be created or used and therefore cannot display its own error.

All other legacy control construction, owner drawing, accessibility annotations for removed
controls, `ModalKind`, modal window procedures, synthetic `WM_COMMAND` forwarding, and native
application-dialog fallbacks are replace-then-delete items.

## Retained native source and validation

Retain `stitcher/native` with these deliberate categories:

- `include/`, `src/`, `shaders/`, `resources/`, `cmake/`, and `CMakeLists.txt` for the native CLI,
  GUI, D3D12 DLL/API, CPU coordinator, codecs, session/discovery/settings/publication logic,
  embedded WebView resources, and native release build;
- `third_party/yyjson` and `third_party/licenses`;
- `tests/pano_app_contract_test.cpp` for schema, planning, codecs, CPU/D3D12 coordination,
  publication, cancellation, and failure contracts;
- `tests/pano_gpu_contract_test.cpp` for D3D12 ABI, shader, WARP, cancellation, and hardware
  contracts;
- `tests/pano_app_webview_contract_test.cpp` for embedded resources, bridge allow-listing, DOM,
  accessibility, and WebView lifetime contracts;
- `tests/pano_app_headless_probe.cpp` for Windows resource and real-session acceptance;
- the bounded application-contract and codec fixtures under `stitcher/native/tests/fixtures`.

Retain `contracts/session.schema.json` and `contracts/example-session.json` as the shared capture
session contract. Retain native-focused release/audit PowerShell after removing its transitional
branches. Retain the CMake-based ReShade add-on build and replace its Python build-contract test
with a non-Python release or CMake check.

The former tracked `stitcher/native/build-debug` and `stitcher/native/build-release` generated trees
were removed from version control before the final repository audit. Build directories remain
ignored local output rather than source or vendored inputs.

## Python, Tk, ctypes, and CUDA deletion set

Delete the complete Python application package after its native contracts pass:

- `stitcher/src/pano_stitch/__init__.py`, `application.py`, `cli.py`, `compositor.py`,
  `metadata.py`, `planner.py`, `projection.py`, `screenshot_watch.py`, and `sessions.py`;
- `stitcher/src/pano_stitch/gui.py`, including all Tkinter UI;
- `stitcher/src/pano_stitch/d3d12_adapter.py`, including the ctypes ABI adapter;
- `stitcher/src/pano_stitch/gpu.py`, including the CUDA/CuPy implementation.

Delete all Python entry points and helper scripts after replacement or retirement:

- `stitcher/scripts/pano_stitch_gui.py` (PyInstaller GUI entry point);
- `stitcher/scripts/benchmark_compositor.py` (Python CPU/CUDA benchmark);
- `stitcher/scripts/generate_codec_fixtures.py` (replace with checked-in fixtures or native
  generation);
- `stitcher/scripts/extract_cet_pose_manifest.py` (replace its retained check without Python);
- `release/bump_version.py` (replace with the native-owned version source and PowerShell/CMake
  consistency checks);
- `reshade-addon/tests/test_build_contract.py` (replace with CMake or PowerShell validation).

Delete the Python package/test environment: `stitcher/pyproject.toml`, `stitcher/uv.lock`, every
file under `stitcher/tests` after the retained fixtures move, and Python build/cache output. The
Python tests accounted for by that directory are application contracts/core, version bumping, CET
manifest extraction, codec fixtures, compositor, ctypes D3D12 adapter, CUDA backend, GUI cache,
metadata/planner, native shader inspection, release build, runtime probe, screenshot watch, and
session management.

Delete or rewrite every active Python invocation in `.github/workflows/ci.yml`,
`.github/workflows/release.yml`, `release/build-windows-release.ps1`, and the active README. The
final checks must not install Python, pip, uv, Ruff, mypy, pytest, or PyInstaller.

Delete every active CUDA/CuPy/NVRTC path, including the Python backend and tests, optional package
dependencies and lock entries, benchmark selection, release collection/probes, comparison archive
checks, workflow setup, and user-facing setup/troubleshooting text. Historical design, migration,
acceptance, and progress documents may retain clearly historical CUDA references; they are neither
active commands nor supported-backend documentation. Generic native identifiers such as
`pano_gpu.dll`, `PanoGpu*`, and `--gpu-strict` refer to D3D12 and remain unless separately renamed.

## Release modes and automation

The current release builder has three frontend modes. Their final dispositions are:

| Mode | Current output | Final disposition |
| --- | --- | --- |
| `python` | PyInstaller `PanoramaCaptureStitcher.exe` plus `pano_gpu.dll` | Delete. |
| `comparison` | Python and native executables plus `pano_gpu.dll` | Delete. |
| `native` | Native GUI as `PanoramaCaptureStitcher.exe` plus `pano_gpu.dll` | Make unconditional; remove the selector and candidate suffix. |

The final builder obtains its version from one native/release-owned source, configures and tests the
native project, builds the ReShade add-on when requested, stages the single native stitcher shape,
runs the unconditional native-only audit, and creates deterministic archives without invoking
Python. The GitHub CI and release workflows must follow the same native-only path.

The final retained test matrix is portable native Release CTest, Windows MSVC Release `/W4 /WX`,
Windows CTest/WARP, WebView DOM/bridge/accessibility and hidden-GUI teardown, focused PowerShell
release/audit tests, deterministic archive checks, controlled runtime/DLL/WebView failures, and the
recorded Windows 11/NVIDIA workflow/resource acceptance. Windows 10, AMD, Intel, and physical
CPU-only validation remain deferred and do not retain any removed implementation.
