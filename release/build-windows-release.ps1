[CmdletBinding()]
param(
    [string]$AddonPath = "",

    [string]$Version = "1.0.4",
    [string]$PythonCommand = "python",
    [string]$OutputDirectory = "",
    [ValidateSet("python", "comparison", "native")]
    [string]$StitcherFrontend = "python"
)

$ErrorActionPreference = "Stop"
$env:PYTHONHASHSEED = "0"
$env:SOURCE_DATE_EPOCH = "946684800"

function New-DeterministicZip {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDirectory,

        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $source = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $archive = [IO.Compression.ZipFile]::Open(
        $DestinationPath,
        [IO.Compression.ZipArchiveMode]::Create
    )
    try {
        Get-ChildItem -LiteralPath $source -Recurse -File |
            Sort-Object { $_.FullName.Substring($source.Length + 1) } |
            ForEach-Object {
                $relative = $_.FullName.Substring($source.Length + 1).Replace("\", "/")
                $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $inputStream = $_.OpenRead()
                $outputStream = $entry.Open()
                try {
                    $inputStream.CopyTo($outputStream)
                }
                finally {
                    $outputStream.Dispose()
                    $inputStream.Dispose()
                }
            }
    }
    finally {
        $archive.Dispose()
    }
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$buildRoot = Join-Path $projectRoot "build\release"
$pyproject = Get-Content -LiteralPath (Join-Path $projectRoot "stitcher\pyproject.toml") -Raw
$versionMatch = [regex]::Match($pyproject, '(?m)^version\s*=\s*"(?<version>[^"]+)"\s*\r?$')

if (-not $versionMatch.Success) {
    throw "Cannot determine stitcher version from stitcher\pyproject.toml"
}
$packageVersion = $versionMatch.Groups["version"].Value
if ($packageVersion -ne $Version) {
    throw "Release version $Version does not match stitcher package version $packageVersion"
}

Remove-Item -LiteralPath $buildRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null

$nativeBuild = Join-Path $buildRoot "stitcher-native"
& cmake -S (Join-Path $projectRoot "stitcher\native") -B $nativeBuild -A x64
if ($LASTEXITCODE -ne 0) {
    throw "Failed to configure the native D3D12 stitcher"
}
& cmake --build $nativeBuild --config Release
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build the native D3D12 stitcher"
}
& ctest --test-dir $nativeBuild -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "Native D3D12 stitcher tests failed"
}
$nativeDll = Join-Path $nativeBuild "Release\pano_gpu.dll"
if (-not (Test-Path -LiteralPath $nativeDll -PathType Leaf)) {
    throw "Native build did not produce pano_gpu.dll"
}
$nativeGui = Join-Path $nativeBuild "Release\pano-stitch-native-gui.exe"
if (-not (Test-Path -LiteralPath $nativeGui -PathType Leaf)) {
    throw "Native build did not produce pano-stitch-native-gui.exe"
}

if ($AddonPath) {
    if (-not $AddonPath -or -not (Test-Path -LiteralPath $AddonPath -PathType Leaf)) {
        throw "AddonPath is required for a full release"
    }
    $addon = (Resolve-Path $AddonPath).Path
    if ([IO.Path]::GetExtension($addon).ToLowerInvariant() -ne ".addon64") {
        throw "AddonPath must point to PanoramaCaptureReShade.addon64"
    }

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
}

function New-StitcherArchive {
    $stitcherStage = Join-Path $buildRoot "PanoramaCapture-Stitcher-$Version-win-x64"
    New-Item -ItemType Directory -Force -Path $stitcherStage | Out-Null
    if ($StitcherFrontend -eq "native") {
        Copy-Item $nativeGui (Join-Path $stitcherStage "PanoramaCaptureStitcher.exe")
        Copy-Item $nativeDll (Join-Path $stitcherStage "pano_gpu.dll")
        Copy-Item "$projectRoot\stitcher\native\third_party\licenses" `
            (Join-Path $stitcherStage "licenses") -Recurse
    } else {
        $venvRoot = Join-Path $projectRoot ".venv-release"
        $python = Join-Path $venvRoot "Scripts\python.exe"
        Remove-Item -LiteralPath $venvRoot -Recurse -Force -ErrorAction SilentlyContinue
        & $PythonCommand -m venv $venvRoot
        & $python -m pip install --upgrade pip
        & $python -m pip install "$projectRoot\stitcher[bundle]"
        $pyInstallerWork = Join-Path $buildRoot "pyinstaller-work"
        $pyInstallerDist = Join-Path $buildRoot "pyinstaller-dist"
        $pyInstallerSpec = Join-Path $buildRoot "pyinstaller-spec"
        $pyInstallerName = if ($StitcherFrontend -eq "comparison") {
            "PanoramaCaptureStitcher-Python"
        } else {
            "PanoramaCaptureStitcher"
        }
        $pyInstallerArgs = @(
            "--noconfirm", "--clean", "--windowed",
            "--name", $pyInstallerName,
            "--workpath", $pyInstallerWork,
            "--distpath", $pyInstallerDist,
            "--specpath", $pyInstallerSpec,
            "--collect-all", "OpenEXR",
            "--collect-all", "Imath"
        )
        if ($StitcherFrontend -eq "python") {
            $pyInstallerArgs += @("--add-binary", "$nativeDll;pano_stitch")
        }
        $pyInstallerArgs += @(
            "--add-data", "$projectRoot\contracts\session.schema.json;contracts",
            "$projectRoot\stitcher\scripts\pano_stitch_gui.py"
        )
        & $python -m PyInstaller @pyInstallerArgs
        $stitcherBundle = Join-Path $pyInstallerDist $pyInstallerName
        Copy-Item (Join-Path $stitcherBundle "*") $stitcherStage -Recurse
        if ($StitcherFrontend -eq "comparison") {
            Copy-Item $nativeGui (Join-Path $stitcherStage "PanoramaCaptureStitcher-Native.exe")
            Copy-Item $nativeDll (Join-Path $stitcherStage "pano_gpu.dll")
            Copy-Item "$projectRoot\stitcher\native\third_party\licenses" `
                (Join-Path $stitcherStage "licenses") -Recurse
        }
    }
    Copy-Item "$projectRoot\README.md" (Join-Path $stitcherStage "README.md")
    $bundledD3D12Dlls = @(
        Get-ChildItem -LiteralPath $stitcherStage -Recurse -File -Filter "pano_gpu.dll"
    )
    if ($bundledD3D12Dlls.Count -ne 1) {
        throw "Stitcher bundle must contain exactly one pano_gpu.dll"
    }
    $probeResult = Join-Path $buildRoot "gpu-runtime-probe.txt"
    $quotedProbeResult = "`"$probeResult`""
    $probeExecutables = if ($StitcherFrontend -eq "comparison") {
        @("PanoramaCaptureStitcher-Python.exe", "PanoramaCaptureStitcher-Native.exe")
    } else {
        @("PanoramaCaptureStitcher.exe")
    }
    foreach ($probeExecutable in $probeExecutables) {
        Remove-Item -LiteralPath $probeResult -Force -ErrorAction SilentlyContinue
        $probeProcess = Start-Process -FilePath (Join-Path $stitcherStage $probeExecutable) -ArgumentList @("--verify-gpu-runtime", $quotedProbeResult) -Wait -PassThru
        if ($probeProcess.ExitCode -notin @(0, 2)) {
            if (Test-Path -LiteralPath $probeResult) {
                Get-Content -LiteralPath $probeResult | Write-Host
            }
            throw "$probeExecutable GPU runtime verification failed (exit code $($probeProcess.ExitCode))"
        }
        if ($probeProcess.ExitCode -eq 2) {
            Get-Content -LiteralPath $probeResult | Write-Host
            Write-Host "No compatible hardware adapter is available on the build machine"
        }
    }
    $bundleBytes = (Get-ChildItem -LiteralPath $stitcherStage -Recurse -File |
        Measure-Object -Property Length -Sum).Sum
    $maximumBundleBytes = 512MB
    if ($bundleBytes -gt $maximumBundleBytes) {
        throw "Stitcher bundle is $([math]::Round($bundleBytes / 1MB)) MiB; limit is $([math]::Round($maximumBundleBytes / 1MB)) MiB"
    }
    $archiveSuffix = switch ($StitcherFrontend) {
        "comparison" { "comparison-win-x64" }
        "native" { "native-candidate-win-x64" }
        default { "win-x64" }
    }
    $stitcherArchive = Join-Path $outputRoot "PanoramaCapture-Stitcher-$Version-$archiveSuffix.zip"
    Remove-Item -LiteralPath $stitcherArchive -Force -ErrorAction SilentlyContinue
    New-DeterministicZip -SourceDirectory $stitcherStage -DestinationPath $stitcherArchive
    $archiveBytes = (Get-Item -LiteralPath $stitcherArchive).Length
    $maximumArchiveBytes = 256MB
    if ($archiveBytes -gt $maximumArchiveBytes) {
        throw "Stitcher archive is $([math]::Round($archiveBytes / 1MB)) MiB; limit is $([math]::Round($maximumArchiveBytes / 1MB)) MiB"
    }
    Write-Host "Created $stitcherArchive ($([math]::Round($archiveBytes / 1MB)) MiB; extracted payload $([math]::Round($bundleBytes / 1MB)) MiB)"
}

New-StitcherArchive
Remove-Item -LiteralPath $buildRoot -Recurse -Force
Write-Host "Removed temporary release build files from $buildRoot"
