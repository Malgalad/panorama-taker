# Optional OxiPNG integration

Implemented 2026-09-05. Only final RGB8 PNG panoramas opt into the new path.
WIC, JPEG, EXR, coverage and thumbnail encoding otherwise remain unchanged.

OxiPNG finalization displays an indeterminate **Encoding PNG** stage in the GUI
and taskbar, without a numeric title percentage. The encoder does not expose
measurable compression progress. Normal progress resumes for thumbnails and
publishing; an initial WIC fallback keeps its existing streaming progress.
No D3D12 source or shader changes.

## Setting and fallback

- App Settings: **Faster PNG encoding (uses more RAM)**; CLI: `--fast-png`.
- `fast_png` defaults false. On missing settings only, usable total physical RAM
  strictly above 16 GiB selects true and the initial settings are saved.
  Existing files without the field remain false; subsequent launches respect
  the saved boolean, regardless of hardware changes.
- Before allocating an image, require six RGB-sized buffers plus 64 MiB workspace,
  leaving physical memory of at least max(1 GiB, 10% of total), and 512 MiB
  additional commit headroom. Arithmetic is overflow-checked.
- Recheck before compression, subtracting the already buffered RGB image from
  the estimate. If memory is now insufficient, replay that buffer through WIC.
- Unknown memory status, missing/invalid DLL, or failed initial RGB allocation
  falls back to WIC. The final GUI result identifies the selected encoder and
  fallback reason. The headless probe logs the same information.
- Memory checks are conservative admission decisions, not reservations: another
  application can consume memory afterward. In particular, upstream internal
  allocation exhaustion is not guaranteed recoverable by Rust's allocator.
- Full-frame compression is synchronous. Cancellation is checked before/after
  compression and between 1 MiB file writes, but cannot interrupt an in-progress
  libdeflate call. At this resolution cancellation can wait roughly five seconds.
  Cancelled/error outputs are not published; existing staged-file cleanup remains.

Memory fields follow Microsoft's
[MEMORYSTATUSEX definitions](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex).
[GlobalMemoryStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex)
returns volatile availability, not a guarantee. DLL loading uses an absolute path
and the documented DLL-directory/system-directory flags from
[LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw).

## Implementation boundary

The Rust bridge owns the RGB buffer, accepts rows without a second full RGB copy,
and transfers that buffer into OxiPNG RawImage. It uses a single MinSum filter,
libdeflate level 3, no color/depth/palette reductions, and explicit `sRGB=0` and
`gAMA=45455`. Compression/output buffers stay in Rust; Windows file ownership,
Unicode paths, staged publication, and cancellation checks stay in C++.

The DLL is loaded lazily, ABI-checked, and retained for the process lifetime.
It isolates libdeflate symbols from OpenEXR's existing version. Build uses
`--locked`, default Rust crate features disabled (no Rayon pool), static CRT,
and baseline x64 compilation with upstream runtime CPU feature detection.
PE imports verified: Kernel32, ntdll, bcryptprimitives, and Windows synchronization
API-set only. No MSVC redistributable, Rust runtime DLL, or extra codec DLL.

## Integrated measurements

Windows x64 Release, Ryzen 7 9800X3D / RTX 5090. Existing authorized session
`pano-1787897185-2`, 17552 × 8776, hard and feather. Two samples per encoder/blend,
order WIC → OxiPNG → OxiPNG → WIC; no concurrent benchmark/build/analysis load.
This is two blends of one scene, not a broad image corpus.

| Path | Hard export seconds | Feather export seconds | Peak physical RAM MiB, hard / feather | PNG bytes, hard / feather |
| --- | ---: | ---: | ---: | ---: |
| WIC | 8.120–8.493 | 7.992–8.012 | 470.1 / 469.9 | 160,567,130 / 154,557,185 |
| OxiPNG | 5.612–5.695 | 5.675–5.864 | 1109.4 / 1104.3 | 163,250,637 / 157,113,419 |

Mean complete-export time decreased **31.9% hard / 27.9% feather**, with only
**1.67% / 1.65%** larger PNGs. This includes RGB allocation/copy, GPU rendering,
readback, final compression, writing and publication. Preview/source loading is
excluded from export time, but included in the probe's separately logged total.
The first WIC hard sample had higher GPU-stage latency; all samples are retained.

The **1.08 GiB** figure is the integrated headless renderer process, not an extra
allocation. It excludes GUI/WebView child processes. The complete GUI process-tree
peak was not measured in this experiment. Memory increases by approximately
635–639 MiB over the WIC renderer, and scales with image dimensions.

DXGI local usage was unchanged: 3,720,601,600 bytes hard / 3,792,494,592 feather.
Nonlocal usage remained about 242 MB. Private commit rose from approximately
4.23–4.31 GB to 4.64–4.72 GB; those figures include GPU-related commit and are not
physical RAM. No new GPU work is submitted by the encoder; AMD/Intel GPU hardware
verification remains deferred.

OxiPNG moves most codec work into `finish_ms` (~4.87–5.02 s), while `encode_ms`
now measures row buffering (~21 ms). Compare their sum or full export time,
not the row-buffering timer alone. Compression does not overlap with rendering;
the measured full-export improvement already includes that tradeoff.

Evidence: `C:\dev\pano-stitch-native-stage\oxipng-integration-bench` logs/JSON.
Both full-size output blends were independently decoded and compared over every
RGB pixel: **zero differences**. Repeats were byte-identical, including the hashes
of the previously verified direct OxiPNG prototypes and unchanged WIC originals.

## Verification and cleanup

- Windows Release build and CTest, including GUI self-test, native contracts and
  WARP preflight: 8/8 passed. GUI rebuilt at
  `C:\dev\pano-stitch-native-stage\build\Release\pano-stitch-native-gui.exe`.
- Linux Release build/CTest: 4/4 passed. This does not prove Windows behavior.
- Focused tests cover threshold/overflow/physical/commit admission, first-launch
  persistence and existing settings, RGB round-trips, padded rows, initial and
  late low-memory fallback, Unicode paths, overwrite refusal, cancellation and
  partial cleanup; GUI message routing, stale/read-only toggles, and snapshots.
- Isolated missing and invalid optional DLL exports both passed WIC fallback.
- Rust Clippy `-D warnings`, Rust formatting, native strict compiler warnings,
  touched C++ formatting, JS syntax and release/source policy checks passed.
- No game was launched, captures changed, or originals overwritten.

The old experimental encoder target/helpers and their libpng, libspng, zlib-ng,
pngloss and pngquant dependencies were removed. Historical benchmark documents
remain; they describe experiments, not current build dependencies. Local-only
pixel verification helpers are under `.local`, outside the native release source
policy. Only OxiPNG and its required dependency graph are added to the product.
