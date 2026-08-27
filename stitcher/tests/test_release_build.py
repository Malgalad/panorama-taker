from pathlib import Path


def test_windows_release_script_builds_isolated_cpu_and_cuda_archives() -> None:
    script = (
        Path(__file__).resolve().parents[2] / "release" / "build-windows-release.ps1"
    ).read_text(encoding="utf-8")

    assert ".venv-release-$Flavor" in script
    assert 'New-StitcherArchive -Flavor "cpu" -IncludeCuda $false' in script
    assert 'New-StitcherArchive -Flavor "cuda" -IncludeCuda $true' in script
    assert "PanoramaCapture-Stitcher-$Version-$Flavor-win-x64.zip" in script
    assert '"bundle,gpu"' in script
    assert '"bundle"' in script
    assert '"--collect-all", "cuda.pathfinder"' in script
    assert '"--collect-all", "cupy_backends"' in script
    assert '"--hidden-import", "graphlib"' in script
    assert '"--collect-all", "cuda_pathfinder"' not in script
    assert '"--collect-all", "cupyx"' not in script
    assert "--verify-cupy-import $probeResult" in script
    assert "Removed $($unneededCudaDlls.Count) unused CUDA math DLLs" in script
    assert "^(cublas|cufft|curand|cusolver|cusparse|cutensor|nvjitlink)" in script
    assert "$maximumBundleBytes" in script
    assert "$maximumArchiveBytes" in script
    assert (
        'Copy-Item (Join-Path $pyInstallerDist "PanoramaCaptureStitcher") $stitcherStage' in script
    )
    assert '"--specpath", $pyInstallerSpec' in script
    assert '"--runtime-hook", $runtimeHook' in script
    assert '"PANO_STITCH_BUILD_FLAVOR`"] = `"$Flavor`"' in script
    assert "Remove-Item -LiteralPath $buildRoot -Recurse -Force" in script
    assert "Removed temporary release build files from $buildRoot" in script
    assert "CPU stitcher bundle unexpectedly contains CuPy or CUDA runtime files" in script

    entrypoint = (Path(__file__).resolve().parents[1] / "scripts" / "pano_stitch_gui.py").read_text(
        encoding="utf-8"
    )
    assert '"--verify-cuda-runtime"' in entrypoint

    gui_source = (Path(__file__).resolve().parents[1] / "src" / "pano_stitch" / "gui.py").read_text(
        encoding="utf-8"
    )
    assert 'PANO_STITCH_BUILD_FLAVOR", "cuda"' in gui_source
    assert "tk.BooleanVar(value=self.cuda_build)" in gui_source
    assert gui_source.count("if self.cuda_build:") >= 3


def test_gpu_package_uses_only_runtime_and_nvrtc_components() -> None:
    project_root = Path(__file__).resolve().parents[1]
    pyproject = (project_root / "pyproject.toml").read_text(encoding="utf-8")
    lockfile = (project_root / "uv.lock").read_text(encoding="utf-8")
    locked_project = lockfile.split('name = "pano-stitch"', 1)[1].split("[[package]]", 1)[0]
    gpu_source = (project_root / "src" / "pano_stitch" / "gpu.py").read_text(encoding="utf-8")
    kernel_source = (project_root / "src" / "pano_stitch" / "cuda_kernels.py").read_text(
        encoding="utf-8"
    )

    assert "cupy-cuda12x[ctk]" not in pyproject
    assert 'extra = ["ctk"]' not in locked_project
    assert "nvidia-cuda-runtime-cu12" in pyproject
    assert "nvidia-cuda-nvrtc-cu12" in pyproject
    assert "nvidia-cuda-runtime-cu12" in locked_project
    assert "nvidia-cuda-nvrtc-cu12" in locked_project
    assert "cp.linalg" not in gpu_source
    assert " @ " not in gpu_source
    assert "solve_exposure_system" in kernel_source
