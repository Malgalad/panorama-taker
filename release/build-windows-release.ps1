[CmdletBinding()]
param(
    [string]$AddonPath = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"

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
                $entry.LastWriteTime = $archiveTimestamp
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
$sourceDateEpoch = $env:SOURCE_DATE_EPOCH
if (-not $sourceDateEpoch) {
    $sourceDateEpoch = (& git -C $projectRoot show -s --format=%ct HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot determine the source commit timestamp"
    }
}
$sourceDateEpochSeconds = 0L
if (-not [long]::TryParse($sourceDateEpoch, [ref]$sourceDateEpochSeconds)) {
    throw "SOURCE_DATE_EPOCH must be a Unix timestamp"
}
try {
    $archiveTimestamp = [DateTimeOffset]::FromUnixTimeSeconds($sourceDateEpochSeconds)
}
catch {
    throw "SOURCE_DATE_EPOCH is outside the supported timestamp range"
}
if ($archiveTimestamp.Year -lt 1980 -or $archiveTimestamp.Year -gt 2107) {
    throw "SOURCE_DATE_EPOCH is outside the ZIP timestamp range"
}
$env:SOURCE_DATE_EPOCH = $sourceDateEpochSeconds.ToString()

$versionFile = Join-Path $projectRoot "stitcher\native\VERSION"
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "stitcher\native\VERSION must use MAJOR.MINOR.PATCH"
}
$cetSource = Join-Path $projectRoot "mod\cet\PanoramaCaptureProbe\init.lua"
$cetVersionMatch = [regex]::Match(
    (Get-Content -LiteralPath $cetSource -Raw),
    '(?m)^local MOD_VERSION = "(?<version>[^\"]+)"\s*$'
)
if (-not $cetVersionMatch.Success) {
    throw "Cannot determine MOD_VERSION from $cetSource"
}
if ($cetVersionMatch.Groups["version"].Value -ne $version) {
    throw "Native version $version does not match CET mod version $($cetVersionMatch.Groups['version'].Value)"
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$buildRoot = Join-Path $projectRoot "build\release"
Remove-Item -LiteralPath $buildRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null

$fxcCommand = Get-Command fxc.exe -CommandType Application -ErrorAction SilentlyContinue
$fxc = if ($fxcCommand) { $fxcCommand.Source } else { "" }
if (-not $fxc) {
    $windowsKits = Get-ItemProperty `
        -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" `
        -Name KitsRoot10 -ErrorAction SilentlyContinue
    if ($windowsKits.KitsRoot10) {
        $fxc = Get-ChildItem `
            -Path (Join-Path $windowsKits.KitsRoot10 "bin\*\x64\fxc.exe") `
            -File -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $fxc -or -not (Test-Path -LiteralPath $fxc -PathType Leaf)) {
    throw "Cannot locate the x64 Windows SDK shader compiler fxc.exe"
}
Write-Host "Using FXC: $fxc"

$nativeBuild = Join-Path $buildRoot "stitcher-native"
& cmake -S (Join-Path $projectRoot "stitcher\native") -B $nativeBuild -A x64 `
    "-DFXC_EXECUTABLE=$fxc"
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

$nativeCli = Join-Path $nativeBuild "Release\pano-stitch-native.exe"
$nativeGui = Join-Path $nativeBuild "Release\pano-stitch-native-gui.exe"
$nativeDll = Join-Path $nativeBuild "Release\pano_gpu.dll"
foreach ($artifact in @($nativeCli, $nativeGui, $nativeDll)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Native build did not produce $artifact"
    }
}
$cliVersion = (& $nativeCli --version).Trim()
if ($LASTEXITCODE -ne 0 -or $cliVersion -ne "pano-stitch-native $version") {
    throw "Native CLI version mismatch: $cliVersion"
}
$guiVersion = (Get-Item -LiteralPath $nativeGui).VersionInfo
if ($guiVersion.FileVersion -ne $version -or $guiVersion.ProductVersion -ne $version) {
    throw "Native GUI metadata version mismatch: file=$($guiVersion.FileVersion), product=$($guiVersion.ProductVersion)"
}

if ($AddonPath) {
    if (-not (Test-Path -LiteralPath $AddonPath -PathType Leaf)) {
        throw "AddonPath is required for a full release"
    }
    $addon = (Resolve-Path -LiteralPath $AddonPath).Path
    if ([IO.Path]::GetExtension($addon).ToLowerInvariant() -ne ".addon64") {
        throw "AddonPath must point to PanoramaCaptureReShade.addon64"
    }

    $modStage = Join-Path $buildRoot "PanoramaCapture-Mod-$version"
    $cetDestination = Join-Path $modStage "bin\x64\plugins\cyber_engine_tweaks\mods\PanoramaCaptureProbe"
    $reshadeDestination = Join-Path $modStage "bin\x64"
    New-Item -ItemType Directory -Force -Path $cetDestination, $reshadeDestination | Out-Null
    Copy-Item $cetSource $cetDestination
    Copy-Item $addon (Join-Path $reshadeDestination "PanoramaCaptureReShade.addon64")
    Copy-Item (Join-Path $projectRoot "README.md") (Join-Path $cetDestination "README.md")

    $modArchive = Join-Path $outputRoot "PanoramaCapture-Mod-$version.zip"
    Remove-Item -LiteralPath $modArchive -Force -ErrorAction SilentlyContinue
    New-DeterministicZip -SourceDirectory $modStage -DestinationPath $modArchive
    Write-Host "Created $modArchive"
}

$stitcherStage = Join-Path $buildRoot "PanoramaCapture-Stitcher-$version-win-x64"
New-Item -ItemType Directory -Force -Path $stitcherStage | Out-Null
Copy-Item $nativeGui (Join-Path $stitcherStage "PanoramaCaptureStitcher.exe")
Copy-Item $nativeDll (Join-Path $stitcherStage "pano_gpu.dll")
Copy-Item (Join-Path $projectRoot "stitcher\native\third_party\licenses") `
    (Join-Path $stitcherStage "licenses") -Recurse
Copy-Item (Join-Path $projectRoot "README.md") (Join-Path $stitcherStage "README.md")

$probeResult = Join-Path $buildRoot "gpu-runtime-probe.txt"
$quotedProbeResult = "`"$probeResult`""
$probeProcess = Start-Process -FilePath (Join-Path $stitcherStage "PanoramaCaptureStitcher.exe") `
    -ArgumentList @("--verify-gpu-runtime", $quotedProbeResult) -Wait -PassThru
if ($probeProcess.ExitCode -notin @(0, 2)) {
    if (Test-Path -LiteralPath $probeResult) {
        Get-Content -LiteralPath $probeResult | Write-Host
    }
    throw "PanoramaCaptureStitcher.exe GPU runtime verification failed (exit code $($probeProcess.ExitCode))"
}
if ($probeProcess.ExitCode -eq 2) {
    Get-Content -LiteralPath $probeResult | Write-Host
    Write-Host "No compatible hardware adapter is available on the build machine"
}

$bundleBytes = (Get-ChildItem -LiteralPath $stitcherStage -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
$maximumBundleBytes = 512MB
if ($bundleBytes -gt $maximumBundleBytes) {
    throw "Stitcher bundle is $([math]::Round($bundleBytes / 1MB)) MiB; limit is $([math]::Round($maximumBundleBytes / 1MB)) MiB"
}
$stitcherArchive = Join-Path $outputRoot "PanoramaCapture-Stitcher-$version-win-x64.zip"
Remove-Item -LiteralPath $stitcherArchive -Force -ErrorAction SilentlyContinue
New-DeterministicZip -SourceDirectory $stitcherStage -DestinationPath $stitcherArchive
$archiveBytes = (Get-Item -LiteralPath $stitcherArchive).Length
$maximumArchiveBytes = 256MB
if ($archiveBytes -gt $maximumArchiveBytes) {
    throw "Stitcher archive is $([math]::Round($archiveBytes / 1MB)) MiB; limit is $([math]::Round($maximumArchiveBytes / 1MB)) MiB"
}
Write-Host "Created $stitcherArchive ($([math]::Round($archiveBytes / 1MB)) MiB; extracted payload $([math]::Round($bundleBytes / 1MB)) MiB)"

Remove-Item -LiteralPath $buildRoot -Recurse -Force
Write-Host "Removed temporary release build files from $buildRoot"
