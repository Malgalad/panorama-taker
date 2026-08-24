[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path $_ -PathType Leaf })]
    [string]$AddonPath,

    [string]$Version = "0.1.0",
    [string]$PythonCommand = "python",
    [string]$OutputDirectory = "",
    [string]$FovControlRoot = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$buildRoot = Join-Path $projectRoot "build\release"
$venvRoot = Join-Path $projectRoot ".venv-release"
$python = Join-Path $venvRoot "Scripts\python.exe"
$addon = (Resolve-Path $AddonPath).Path
$fovControlRoot = $null
if ($FovControlRoot) {
    $fovControlRoot = (Resolve-Path $FovControlRoot).Path
}
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

if (-not (Test-Path $python)) {
    & $PythonCommand -m venv $venvRoot
}
& $python -m pip install --upgrade pip
& $python -m pip install "$projectRoot\stitcher[bundle]"

$pyInstallerWork = Join-Path $buildRoot "pyinstaller-work"
$pyInstallerDist = Join-Path $buildRoot "pyinstaller-dist"
# OpenEXR and Imath are loaded dynamically; the remaining runtime libraries are direct imports.
& $python -m PyInstaller --noconfirm --clean --windowed `
    --name PanoramaCaptureStitcher `
    --workpath $pyInstallerWork `
    --distpath $pyInstallerDist `
    --collect-all OpenEXR `
    --collect-all Imath `
    --add-data "$projectRoot\contracts\session.schema.json;contracts" `
    "$projectRoot\stitcher\scripts\pano_stitch_gui.py"

$modStage = Join-Path $buildRoot "PanoramaCapture-Mod-$Version"
$cetDestination = Join-Path $modStage "bin\x64\plugins\cyber_engine_tweaks\mods\PanoramaCaptureProbe"
$reshadeDestination = Join-Path $modStage "bin\x64"
New-Item -ItemType Directory -Force -Path $cetDestination, $reshadeDestination | Out-Null
Copy-Item "$projectRoot\mod\cet\PanoramaCaptureProbe\init.lua" $cetDestination
Copy-Item $addon (Join-Path $reshadeDestination "PanoramaCaptureReShade.addon64")
if ($fovControlRoot) {
    $fovDll = Get-ChildItem -LiteralPath $fovControlRoot -Recurse -File -Filter "FovControl.dll" |
        Select-Object -First 1
    $fovScript = Join-Path $fovControlRoot "bin\r6\scripts\FovControl\FovControl.reds"
    if ($null -eq $fovDll -or -not (Test-Path -LiteralPath $fovScript)) {
        throw "FovControl build outputs were not found under $fovControlRoot"
    }
    $fovDestination = Join-Path $modStage "red4ext\plugins\FovControl"
    $fovScriptDestination = Join-Path $modStage "r6\scripts\FovControl"
    New-Item -ItemType Directory -Force -Path $fovDestination, $fovScriptDestination | Out-Null
    Copy-Item $fovDll.FullName (Join-Path $fovDestination "FovControl.dll")
    Copy-Item $fovScript (Join-Path $fovScriptDestination "FovControl.reds")
    Copy-Item (Join-Path $fovControlRoot "LICENSE.txt") (Join-Path $fovDestination "FovControl-LICENSE.txt")
}
Copy-Item "$projectRoot\README.md" (Join-Path $cetDestination "README.md")

$stitcherStage = Join-Path $buildRoot "PanoramaCapture-Stitcher-$Version-win-x64"
Copy-Item $pyInstallerDist $stitcherStage -Recurse
Copy-Item "$projectRoot\README.md" (Join-Path $stitcherStage "README.md")

$modArchive = Join-Path $outputRoot "PanoramaCapture-Mod-$Version.zip"
$stitcherArchive = Join-Path $outputRoot "PanoramaCapture-Stitcher-$Version-win-x64.zip"
Remove-Item -LiteralPath $modArchive, $stitcherArchive -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $modStage "*") -DestinationPath $modArchive
Compress-Archive -Path (Join-Path $stitcherStage "*") -DestinationPath $stitcherArchive

Write-Host "Created $modArchive"
Write-Host "Created $stitcherArchive"
