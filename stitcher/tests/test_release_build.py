import tomllib
from pathlib import Path


def test_release_workflow_publishes_existing_plain_version_release() -> None:
    workflow = (
        Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release.yml"
    ).read_text(encoding="utf-8")

    assert '- "[0-9]*"' in workflow
    assert "inputs.release_version || github.ref" in workflow
    assert "github.event_name == 'push' || inputs.publish" in workflow
    assert "gh release upload $env:RELEASE_VERSION $assets --clobber" in workflow


def test_github_actions_run_complete_test_suite() -> None:
    project_root = Path(__file__).resolve().parents[2]
    for workflow_name in ("ci.yml", "release.yml"):
        workflow = (project_root / ".github" / "workflows" / workflow_name).read_text()
        assert "pytest -q stitcher/tests reshade-addon/tests" in workflow
        assert "test_gpu_runtime.py" not in workflow


def test_release_versions_are_synchronized() -> None:
    project_root = Path(__file__).resolve().parents[2]
    pyproject = tomllib.loads((project_root / "stitcher" / "pyproject.toml").read_text())
    version = pyproject["project"]["version"]
    semver_arguments = ", ".join(version.split("."))
    expected_declarations = {
        "stitcher/src/pano_stitch/__init__.py": f'__version__ = "{version}"',
        "stitcher/uv.lock": f'name = "pano-stitch"\nversion = "{version}"',
        "mod/cet/PanoramaCaptureProbe/init.lua": f'local MOD_VERSION = "{version}"',
        "mod/src/plugin.cpp": f"RED4EXT_V1_SEMVER({semver_arguments})",
        "contracts/example-session.json": f'"mod_version": "{version}"',
        "release/build-windows-release.ps1": f'[string]$Version = "{version}"',
        ".github/workflows/release.yml": f'default: "{version}"',
    }

    for relative_path, declaration in expected_declarations.items():
        assert declaration in (project_root / relative_path).read_text()


def test_windows_release_script_builds_python_comparison_and_native_archives() -> None:
    script = (
        Path(__file__).resolve().parents[2] / "release" / "build-windows-release.ps1"
    ).read_text(encoding="utf-8")

    assert '.venv-release"' in script
    assert '"$projectRoot\\stitcher[bundle]"' in script
    assert '"PanoramaCapture-Stitcher-$Version-$archiveSuffix.zip"' in script
    assert "function New-DeterministicZip" in script
    assert "Sort-Object" in script
    assert "2000, 1, 1, 0, 0, 0" in script
    assert '$env:PYTHONHASHSEED = "0"' in script
    assert '$env:SOURCE_DATE_EPOCH = "946684800"' in script
    assert "$maximumBundleBytes" in script
    assert "$maximumArchiveBytes" in script
    assert '[ValidateSet("python", "comparison", "native")]' in script
    assert '[string]$StitcherFrontend = "python"' in script
    assert '"PanoramaCaptureStitcher-Python"' in script
    assert '"PanoramaCaptureStitcher-Native.exe"' in script
    assert '"comparison-win-x64"' in script
    assert '"native-candidate-win-x64"' in script
    assert 'if ($StitcherFrontend -eq "native")' in script
    assert 'Copy-Item $nativeGui (Join-Path $stitcherStage "PanoramaCaptureStitcher.exe")' in script
    assert 'Copy-Item $nativeDll (Join-Path $stitcherStage "pano_gpu.dll")' in script
    assert 'Copy-Item "$projectRoot\\stitcher\\native\\third_party\\licenses"' in script
    assert "foreach ($probeExecutable in $probeExecutables)" in script
    assert '"--specpath", $pyInstallerSpec' in script
    assert "Remove-Item -LiteralPath $buildRoot -Recurse -Force" in script
    assert "Removed temporary release build files from $buildRoot" in script
    assert 'cmake -S (Join-Path $projectRoot "stitcher\\native")' in script
    assert "cmake --build $nativeBuild --config Release" in script
    assert "ctest --test-dir $nativeBuild -C Release --output-on-failure" in script
    assert 'if ($StitcherFrontend -eq "python")' in script
    assert '"--add-binary", "$nativeDll;pano_stitch"' in script
    assert "bundle must contain exactly one pano_gpu.dll" in script
    assert '$quotedProbeResult = "`"$probeResult`""' in script
    assert '"--verify-gpu-runtime", $quotedProbeResult' in script
    assert 'ArgumentList @("--verify-gpu-runtime", $probeResult)' not in script
    assert "Start-Process" in script
    assert "-Wait -PassThru" in script
    assert "$probeProcess.ExitCode -notin @(0, 2)" in script
    assert "StitcherFlavor" not in script
    assert "PANO_STITCH_BUILD_FLAVOR" not in script

    entrypoint = (Path(__file__).resolve().parents[1] / "scripts" / "pano_stitch_gui.py").read_text(
        encoding="utf-8"
    )
    assert '"--verify-gpu-runtime"' in entrypoint
    assert "GPU_RUNTIME_UNAVAILABLE_EXIT = 2" in entrypoint
    assert "GPU_RUNTIME_FAILURE_EXIT = 3" in entrypoint


def test_package_has_no_vendor_compute_dependencies_or_lock_entries() -> None:
    project_root = Path(__file__).resolve().parents[1]
    pyproject = (project_root / "pyproject.toml").read_text(encoding="utf-8")
    lockfile = (project_root / "uv.lock").read_text(encoding="utf-8")
    for dependency in ("cu" + "py", "cu" + "da-runtime", "nv" + "rtc", "cu" + "dart"):
        assert dependency not in pyproject.lower()
        assert dependency not in lockfile.lower()


def test_windows_archive_audit_covers_runtime_dependencies_hashes_and_spaces() -> None:
    script = (
        Path(__file__).resolve().parents[2] / "release" / "audit-windows-stitcher.ps1"
    ).read_text(encoding="utf-8")

    assert '"build\\Panorama Capture Release Audit"' in script
    assert '$quotedProbeResult = "`"$probeResult`""' in script
    assert script.count('"--verify-gpu-runtime", $quotedProbeResult') == 2
    assert 'ArgumentList @("--verify-gpu-runtime", $probeResult)' not in script
    assert "Start-Process" in script
    assert "-Wait -PassThru" in script
    assert "dumpbin.exe /dependents" in script
    assert "archive_sha256=" in script
    assert '"executables:"' in script
    assert '"PanoramaCaptureStitcher-Python.exe"' in script
    assert '"PanoramaCaptureStitcher-Native.exe"' in script
    assert "native_dll_sha256=" in script
    assert "[switch]$RequireNativeOnly" in script
    assert '"payload_mode=$(if ($RequireNativeOnly)' in script
    assert "Native-only archive contains Python runtime payload" in script
    assert "Native-only archive depends on an external MSVC redistributable" in script
    assert (
        "Corrupted native DLL must make $($executable.Name) produce runtime-failure exit 3"
        in script
    )
    assert "shader_source_sha256:" in script
    assert "d3dcompiler|nvrtc|cudart|cupy|cuda" in script
    assert "\\.(hlsl|cso|pdb)" in script
    assert "finally" in script
    assert "[switch]$AllowUnavailable" in script

    workflow = (
        Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release.yml"
    ).read_text(encoding="utf-8")
    assert "audit-windows-stitcher.ps1" in workflow
    assert "-AllowUnavailable" in workflow
    assert "dist/*.audit.txt" in workflow
