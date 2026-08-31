# Native application dependencies

## JSON parser (Step 18c.1)

The native application core vendors **yyjson 0.12.0**, pinned to upstream commit
`8b4a38dc994a110abaec8a400615567bd996105f`. The unchanged distribution files are under
`stitcher/native/third_party/yyjson/` with their MIT license.

The production comparison considered:

| Candidate | Result |
| --- | --- |
| yyjson 0.12.0 | Selected: two-file C implementation, strict parsing by default, length-aware UTF-8 strings, distinct integer/real queries, useful byte-offset diagnostics, MIT license, and no runtime library. |
| nlohmann/json | Rejected for this boundary: excellent C++ API and diagnostics, but a materially larger header/archive and compile-time cost for the small validated projection used here. |
| RapidJSON | Rejected: small runtime footprint and MIT license, but its older template-heavy API adds more validation/error plumbing and has less direct UTF-8 validation behavior for this use. |

The CMake target compiles yyjson statically and links it only into `pano_app_core`. Vendored source
is not rewritten and is not subjected to project warning-as-error flags; all application wrapper
code remains under the strict project warnings. The contract test covers valid, invalid,
non-object, malformed, and Unicode documents on portable and MSVC builds. Non-standard numbers
and malformed UTF-8 are rejected by the default reader flags.

## Image codecs (Step 19a)

The native application uses two codec providers:

- Windows Imaging Component (WIC), supplied by Windows, for JPEG and PNG; and
- OpenEXRCore **3.4.13** for EXR, built statically from upstream commit
  `c1194b2cb23a1bdf76fe5e756b22e8436b9a98c9` with Imath **3.2.2** commit
  `1e480d11cb98b032a2dece9b9a8730512effc7f6`.

`cmake/PanoCodecDependencies.cmake` pins both immutable source archives and SHA-256 hashes. It
keeps their tests, tools, examples, Python bindings, installation rules, shared libraries, and the
high-level OpenEXR C++ library out of the product graph. `pano_app_core` links only
`OpenEXR::OpenEXRCore`; Windows builds additionally link the system import libraries
`windowscodecs`, `ole32`, and `oleaut32`. There is no codec DLL to collect. OpenEXRCore's build also
contains its upstream-pinned OpenJPH 0.26.3 and libdeflate sources. Their unchanged binary
redistribution notices, and the OpenEXR/Imath notices, are retained under
`stitcher/native/third_party/licenses/` and must accompany the native release archive.

### Probe results

| Requirement | WIC | OpenEXRCore 3.4.13 |
| --- | --- | --- |
| License/runtime | Windows system API; no added runtime or archive | BSD-3-Clause; static linkage, no added runtime DLL |
| Required formats | JPEG, 8-bit RGB PNG, 16-bit RGB PNG | Float RGB scanline EXR with PIZ compression |
| UTF-16 paths | `IWICStream::InitializeFromFilename` and decoder filename APIs passed non-ASCII input/output probes | Public custom stream callbacks backed by `CreateFileW` passed non-ASCII input/output probes |
| Bounded operation | `CopyPixels` accepts a caller rectangle/buffer; `WritePixels` accepted two sequential one-row bands | Chunk/scanline decode and encode APIs operate on caller-owned bounded buffers |
| Metadata | Dimensions, native pixel format, JPEG frame header, and standard WIC metadata are available; PNG `cICP` is not exposed and must be parsed from bounded raw chunks | Channels, sample types, windows, compression, and standard/custom header attributes are available before pixel decode |
| Cancellation | Check before metadata/copy/write calls and between rows/bands; WIC calls themselves are synchronous | Check in custom read/write callbacks and between chunks; injected callback cancellation failed promptly without publishing output |
| Verified build | MSVC 19.51 `/W4 /WX`; Windows 11 decode/encode probe | GCC portable build plus MSVC 19.51 `/W4 /WX` wrapper; PIZ float round trip preserved finite negative and above-one values |

The WIC probe decoded every frozen JPEG/PNG fixture into caller-owned RGB buffers, preserved native
`uint16` PNG samples, found the exact PNG `cICP=(9,16,0,1)` chunk through a separate bounded parser,
and encoded PNG/JPEG in sequential bands. The encoded JPEG frame reported sample factors
`0x111111` (4:4:4). The encoded PNG round-tripped exactly. Both decoding and encoding used Unicode
paths.

The OpenEXR probe decoded the frozen PIZ file and streamed a PIZ output through Windows-handle
callbacks, then reopened it and compared all native float samples. A statically linked Release
probe was 1,179,648 bytes. For comparison, the generated MSVC archives were 2,769,270 bytes for
OpenEXRCore, 495,052 bytes for Imath, and 1,328,664 bytes for OpenJPH; static linking excludes
unreferenced code from the final executable. A clean portable application build and CTest passed
with only `OpenEXRCore` on the application link interface.

### Rejected alternatives

| Candidate | Result |
| --- | --- |
| libjpeg-turbo + libpng | Capable and permissively licensed, but duplicates codecs already shipped and serviced by Windows, adds two source/runtime dependency surfaces, and still requires a separate PNG `cICP` parser. |
| OpenEXR high-level C++ API | Functionally passed the PIZ/Unicode probe, but adds the 13,693,468-byte high-level archive plus Iex/IlmThread surfaces that the chunk-oriented application does not need. |
| TinyEXR v3.2.0 | The upstream C11 sanitizer suite passed 246/246 on Linux and its streaming PIZ memory probe peaked at 86,970 bytes versus 2,496,332 bytes for full decode. It was rejected because stock MSVC failed on `_Atomic`; Microsoft's `/experimental:c11atomics` advanced the build, but upstream code then failed strict MSVC compilation on GCC-only `__builtin_clz` and other portability warnings. Adopting it would require maintaining a codec fork. |

Primary API references: [WIC native pixel formats](https://learn.microsoft.com/windows/win32/wic/-wic-codec-native-pixel-formats),
[WIC sequential `WritePixels`](https://learn.microsoft.com/windows/win32/api/wincodec/nf-wincodec-iwicbitmapframeencode-writepixels),
[WIC JPEG options](https://learn.microsoft.com/windows/win32/wic/jpeg-format-overview),
[OpenEXR installation and dependencies](https://openexr.com/en/latest/install.html), and
[OpenEXR license](https://openexr.com/en/latest/license.html).

## WebView2 GUI host (Step 23a)

The native GUI uses Microsoft.Web.WebView2 SDK **1.0.4191.47** as a build-only dependency. CMake
pins the NuGet archive and SHA-256 hash, or accepts an explicitly selected extracted SDK for an
offline Windows build. The x64 static loader is linked into `pano-stitch-native-gui.exe`; no
`WebView2Loader.dll` is shipped.

The browser engine remains the separately serviced Evergreen WebView2 Runtime. The application
queries that runtime before creating a browser environment and can present a native
download/retry/exit prompt when it is absent, so the recovery path itself has no browser
dependency. WebView profile data is isolated under
`%LOCALAPPDATA%\PanoramaStitcher\WebView2`; the existing roaming application settings path is
unchanged. The SDK license and third-party notice are retained under
`stitcher/native/third_party/licenses/` and accompany the native release archive.
