# Clean Windows CUDA release validation

This is the external release gate for the CUDA stitcher. Run it on a clean Windows x64 machine
with a supported NVIDIA driver and no CUDA Toolkit, Python, or Visual Studio installed.

## Build

On the signed build machine, build the ReShade add-on and then create the release archives:

```powershell
pwsh -File release\build-windows-release.ps1 -AddonPath <path-to>\PanoramaCaptureReShade.addon64
```

Record the archive names, SHA-256 hashes, Python/CuPy versions, and the collected CUDA DLL names.
The stitcher archive must include the NVRTC and CUDA runtime components required by the bundled
CuPy wheel, but not a CUDA Toolkit installation.

## Clean-machine procedure

1. Verify that `nvcc`, `python`, and the CUDA Toolkit directory are absent.
2. Install only the NVIDIA display driver, reboot, and record its version with `nvidia-smi`.
3. Unpack `PanoramaCapture-Stitcher-<version>-cuda-win-x64.zip` outside the game directory.
4. Launch `PanoramaCaptureStitcher.exe`; select a known complete PNG and a known complete HDR/PQ
   session.
5. Render a preview and a full PNG with CUDA enabled. Confirm the GUI reports `CUDA resident` or
   `CUDA banded`, not CPU fallback.
6. Render an EXR and a thumbnail. Confirm all requested artifacts open and that cancelling a
   separate render leaves no `.partial` output.
7. Close and reopen the GUI, then repeat one preview-to-full render. Collect the stitcher log and
   confirm the session-cache hit and the absence of a second source upload/exposure solve.

Record pass/fail, GPU/driver, archive hash, selected CUDA mode, output checksums, and the relevant
log lines in the release evidence. A failure is a release blocker; WSL validation does not replace
this gate.

Also verify that `PanoramaCapture-Stitcher-<version>-cpu-win-x64.zip` contains neither CuPy nor
CUDA runtime DLLs and starts on a non-NVIDIA Windows x64 system, where it must select CPU rendering.
