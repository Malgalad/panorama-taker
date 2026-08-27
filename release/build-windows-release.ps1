[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path $_ -PathType Leaf })]
    [string]$AddonPath,

    [string]$Version = "0.1.0",
    [string]$PythonCommand = "python",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$buildRoot = Join-Path $projectRoot "build\release"
$addon = (Resolve-Path $AddonPath).Path
$pyproject = Get-Content -LiteralPath (Join-Path $projectRoot "stitcher\pyproject.toml") -Raw
$versionMatch = [regex]::Match($pyproject, '(?m)^version\s*=\s*"(?<version>[^"]+)"\s*\r?$')

if (-not $versionMatch.Success) {
    throw "Cannot determine stitcher version from stitcher\pyproject.toml"
}
$packageVersion = $versionMatch.Groups["version"].Value
if ($packageVersion -ne $Version) {
    throw "Release version $Version does not match stitcher package version $packageVersion"
}

if ([IO.Path]::GetExtension($addon).ToLowerInvariant() -ne ".addon64") {
    throw "AddonPath must point to PanoramaCaptureReShade.addon64"
}

Remove-Item -LiteralPath $buildRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null

$modStage = Join-Path $buildRoot "PanoramaCapture-Mod-$Version"
$cetDestination = Join-Path $modStage "bin\x64\plugins\cyber_engine_tweaks\mods\PanoramaCaptureProbe"
$reshadeDestination = Join-Path $modStage "bin\x64"
New-Item -ItemType Directory -Force -Path $cetDestination, $reshadeDestination | Out-Null
Copy-Item "$projectRoot\mod\cet\PanoramaCaptureProbe\init.lua" $cetDestination
Copy-Item $addon (Join-Path $reshadeDestination "PanoramaCaptureReShade.addon64")
Copy-Item "$projectRoot\README.md" (Join-Path $cetDestination "README.md")

$modArchive = Join-Path $outputRoot "PanoramaCapture-Mod-$Version.zip"
Remove-Item -LiteralPath $modArchive -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $modStage "*") -DestinationPath $modArchive
Write-Host "Created $modArchive"

function New-StitcherArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Flavor,

        [Parameter(Mandatory = $true)]
        [bool]$IncludeCuda
    )

    $venvRoot = Join-Path $projectRoot ".venv-release-$Flavor"
    $python = Join-Path $venvRoot "Scripts\python.exe"
    Remove-Item -LiteralPath $venvRoot -Recurse -Force -ErrorAction SilentlyContinue
    & $PythonCommand -m venv $venvRoot
    & $python -m pip install --upgrade pip
    $stitcherExtra = if ($IncludeCuda) { "bundle,gpu" } else { "bundle" }
    & $python -m pip install "$projectRoot\stitcher[$stitcherExtra]"
    $installedPackages = @((& $python -m pip list --format=json | ConvertFrom-Json) | ForEach-Object {
        $_.name.ToLowerInvariant()
    })
    $forbiddenCudaPackages = @(
        "cuda-toolkit",
        "nvidia-cublas-cu12",
        "nvidia-cufft-cu12",
        "nvidia-curand-cu12",
        "nvidia-cusolver-cu12",
        "nvidia-cusparse-cu12",
        "nvidia-nvjitlink-cu12"
    )
    $unexpectedCudaPackages = @($forbiddenCudaPackages | Where-Object {
        $installedPackages -contains $_
    })
    if ($unexpectedCudaPackages.Count -gt 0) {
        throw "Release environment contains forbidden CUDA packages: $($unexpectedCudaPackages -join ', ')"
    }
    if (-not $IncludeCuda -and $installedPackages -contains "cupy-cuda12x") {
        throw "CPU release environment unexpectedly contains CuPy"
    }

    $pyInstallerWork = Join-Path $buildRoot "pyinstaller-work-$Flavor"
    $pyInstallerDist = Join-Path $buildRoot "pyinstaller-dist-$Flavor"
    $pyInstallerSpec = Join-Path $buildRoot "pyinstaller-spec-$Flavor"
    $runtimeHook = Join-Path $buildRoot "build-flavor-$Flavor.py"
    Set-Content -LiteralPath $runtimeHook -Value @(
        "import os"
        "os.environ[`"PANO_STITCH_BUILD_FLAVOR`"] = `"$Flavor`""
    )
    $pyInstallerArgs = @(
        "--noconfirm", "--clean", "--windowed",
        "--name", "PanoramaCaptureStitcher",
        "--workpath", $pyInstallerWork,
        "--distpath", $pyInstallerDist,
        "--specpath", $pyInstallerSpec,
        "--runtime-hook", $runtimeHook,
        "--collect-all", "OpenEXR",
        "--collect-all", "Imath"
    )
    if ($IncludeCuda) {
        $pyInstallerArgs += @(
            "--collect-all", "cupy",
            "--collect-all", "cupy_backends",
            "--collect-all", "cuda.pathfinder",
            "--hidden-import", "graphlib",
            "--collect-all", "nvidia.cuda_runtime",
            "--collect-all", "nvidia.cuda_nvrtc"
        )
    }
    $pyInstallerArgs += @(
        "--add-data", "$projectRoot\contracts\session.schema.json;contracts",
        "$projectRoot\stitcher\scripts\pano_stitch_gui.py"
    )
    & $python -m PyInstaller @pyInstallerArgs

    $stitcherBundle = Join-Path $pyInstallerDist "PanoramaCaptureStitcher"
    $unneededCudaDlls = @(
        Get-ChildItem -LiteralPath $stitcherBundle -Recurse -File | Where-Object {
            $_.Name -match '^(cublas|cufft|curand|cusolver|cusparse|cutensor|nvjitlink).*\.dll$'
        }
    )
    foreach ($dll in $unneededCudaDlls) {
        Remove-Item -LiteralPath $dll.FullName -Force
    }
    if ($unneededCudaDlls.Count -gt 0) {
        Write-Host "Removed $($unneededCudaDlls.Count) unused CUDA math DLLs from $Flavor stitcher bundle"
    }

    $nvrtc = Get-ChildItem -LiteralPath $stitcherBundle -Recurse -File -Filter "nvrtc64*.dll"
    $cudaRuntime = Get-ChildItem -LiteralPath $stitcherBundle -Recurse -File -Filter "cudart64*.dll"
    $cupyDirectories = Get-ChildItem -LiteralPath $stitcherBundle -Recurse -Directory |
        Where-Object { $_.Name -eq "cupy" }
    if ($IncludeCuda -and ($nvrtc.Count -eq 0 -or $cudaRuntime.Count -eq 0 -or $cupyDirectories.Count -eq 0)) {
        throw "CUDA stitcher bundle is missing CuPy, cudart, or NVRTC"
    }
    if (-not $IncludeCuda -and ($nvrtc.Count -gt 0 -or $cudaRuntime.Count -gt 0 -or $cupyDirectories.Count -gt 0)) {
        throw "CPU stitcher bundle unexpectedly contains CuPy or CUDA runtime files"
    }

    $bundleBytes = (Get-ChildItem -LiteralPath $stitcherBundle -Recurse -File |
        Measure-Object -Property Length -Sum).Sum
    $maximumBundleBytes = if ($IncludeCuda) { 768MB } else { 512MB }
    if ($bundleBytes -gt $maximumBundleBytes) {
        throw "$Flavor stitcher bundle is $([math]::Round($bundleBytes / 1MB)) MiB; limit is $([math]::Round($maximumBundleBytes / 1MB)) MiB"
    }

    if ($IncludeCuda) {
        $probeResult = Join-Path $buildRoot "cuda-import-probe.txt"
        & (Join-Path $stitcherBundle "PanoramaCaptureStitcher.exe") --verify-cupy-import $probeResult
        if ($LASTEXITCODE -ne 0) {
            if (Test-Path -LiteralPath $probeResult) {
                Get-Content -LiteralPath $probeResult | Write-Host
            }
            throw "Frozen CUDA stitcher could not import CuPy (exit code $LASTEXITCODE)"
        }
    }

    $stitcherStage = Join-Path $buildRoot "PanoramaCapture-Stitcher-$Version-$Flavor-win-x64"
    New-Item -ItemType Directory -Force -Path $stitcherStage | Out-Null
    Copy-Item (Join-Path $pyInstallerDist "PanoramaCaptureStitcher") $stitcherStage -Recurse
    Copy-Item "$projectRoot\README.md" (Join-Path $stitcherStage "README.md")
    $stitcherArchive = Join-Path $outputRoot "PanoramaCapture-Stitcher-$Version-$Flavor-win-x64.zip"
    Remove-Item -LiteralPath $stitcherArchive -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stitcherStage "*") -DestinationPath $stitcherArchive
    $archiveBytes = (Get-Item -LiteralPath $stitcherArchive).Length
    $maximumArchiveBytes = if ($IncludeCuda) { 512MB } else { 256MB }
    if ($archiveBytes -gt $maximumArchiveBytes) {
        throw "$Flavor stitcher archive is $([math]::Round($archiveBytes / 1MB)) MiB; limit is $([math]::Round($maximumArchiveBytes / 1MB)) MiB"
    }
    Write-Host "Created $stitcherArchive ($([math]::Round($archiveBytes / 1MB)) MiB; extracted payload $([math]::Round($bundleBytes / 1MB)) MiB)"
}

New-StitcherArchive -Flavor "cpu" -IncludeCuda $false
New-StitcherArchive -Flavor "cuda" -IncludeCuda $true
Remove-Item -LiteralPath $buildRoot -Recurse -Force
Write-Host "Removed temporary release build files from $buildRoot"
