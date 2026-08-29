# D3D12 stitcher acceptance

## Transitional archive candidate

- Version: `1.0.4`
- Archive: `PanoramaCapture-Stitcher-1.0.4-win-x64.zip`
- SHA-256: `20878f3155f86c5c55174b4c8409fa449aad0740318a7e268f0b1abf23756c7a`
- Compressed bytes: `74,932,826`
- Extracted bytes: `190,702,305`
- Executable SHA-256: `0d61e06d645a016a84d630c6e5c076e9647934f2d6d8123377de48173bd35bac`
- Native DLL SHA-256: `67746bbb1bc3f18e64e02eca2de3af9817411d805a586b6fbcfe10b3afe370ec`
- Native ABI: `9`

Two clean builds using fixed Python build inputs, MSVC `/Brepro`, sorted archive entries, and fixed
entry timestamps produced the identical archive SHA-256 above.

The automated artifact audit passed from an extracted path containing spaces. It found exactly one
application executable and one `pano_gpu.dll`, no loose HLSL/CSO/PDB files, no vendor compute
runtime or runtime shader compiler, and no forbidden PE dependency. The audit report records every
HLSL source hash and the candidate's PE dependencies. Corrupting only the extracted DLL produced
the required runtime-failure exit category `3`.

## Completed physical-hardware acceptance

- Date: 2026-08-29
- OS: Windows 11 `10.0.26200`
- Adapter: NVIDIA GeForce RTX 5090
- Vendor/device: `0x10de` / `0x2b85`
- Adapter LUID: `0x0000000000014331`
- Session: `1787897185-2`, 30 SDR PNG frames at 3840×2160
- Runtime verification: ABI load, product adapter probe, embedded pipeline creation, tiny dispatch,
  readback validation, and cleanup passed.
- Resident rendering: preview and 1024-wide hard panorama passed with projected thumbnail and
  coverage output.
- Other output paths: 512-wide feather JPEG and linear EXR passed.
- Forced banding: the first feasible constrained budget was 6528 MiB; a 4096-wide panorama passed
  using 32-row bands.
- Cancellation: cancellation after the first completed D3D12 source upload raised the cancellation
  category, published no output, and left no partial file.
- Previously completed manual GUI acceptance on this adapter covers retained preview interaction,
  hard/feather rendering, exposure correction, thumbnail projection, coverage, error recovery, and
  preview-to-full behavior.

The locally generated acceptance outputs were hashed for the run and then deleted. Captures and
existing user outputs were not modified.

## Deferred cross-device acceptance checklist (non-blocking)

The completed Windows 11/NVIDIA acceptance above is the physical-hardware gate for the current
D3D12 migration milestone. Complete the broader matrix when suitable systems and capture sessions
become available; these unchecked items do not block Step 16 or subsequent native-application work.

- [ ] Windows 10 candidate startup and rendering.
- [ ] AMD hardware adapter.
- [ ] Intel Arc or supported integrated hardware adapter.
- [ ] Clean CPU-fallback-only machine with no compatible adapter.
- [ ] Known JPEG-input, 16-bit Rec.2020/PQ PNG-input, and float EXR-input capture sessions across
  the available physical-hardware matrix.
- [ ] Exact candidate GUI inspection on each target, including preview interaction and
  cancellation.

No AMD/Intel/CPU-only machine or known PQ/EXR capture session was available during the local run.
WARP and synthetic tests remain required regression gates but are not recorded as physical-hardware
acceptance.
