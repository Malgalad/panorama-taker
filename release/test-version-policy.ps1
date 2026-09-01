[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$bumpScript = Join-Path $PSScriptRoot "bump-version.ps1"
$version = (Get-Content -LiteralPath (Join-Path $projectRoot "stitcher\native\VERSION") -Raw).Trim()
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "Native version must use MAJOR.MINOR.PATCH"
}
$components = $version.Split(".")
$declarations = [ordered]@{
    "mod\cet\PanoramaCaptureProbe\init.lua" = "local MOD_VERSION = `"$version`""
    "mod\src\plugin.cpp" = "RED4EXT_V1_SEMVER($($components[0]), $($components[1]), $($components[2]))"
    "contracts\example-session.json" = "`"mod_version`": `"$version`""
    ".github\workflows\release.yml" = "default: `"$version`""
}
foreach ($entry in $declarations.GetEnumerator()) {
    $contents = Get-Content -LiteralPath (Join-Path $projectRoot $entry.Key) -Raw
    if (-not $contents.Contains($entry.Value)) {
        throw "$($entry.Key) does not match native version $version"
    }
}
$releaseWorkflow = Get-Content -LiteralPath `
    (Join-Path $projectRoot ".github\workflows\release.yml") -Raw
if (-not $releaseWorkflow.Contains('          ref: ${{ github.ref }}')) {
    throw "Release workflow must check out the triggering ref"
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("pano-version-policy-" + [guid]::NewGuid())
try {
    foreach ($relativePath in $declarations.Keys + @("stitcher\native\VERSION")) {
        $source = Join-Path $projectRoot $relativePath
        $destination = Join-Path $testRoot $relativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }

    & $bumpScript -Version "9.8.7" -ProjectRoot $testRoot | Out-Null
    if ((Get-Content -LiteralPath (Join-Path $testRoot "stitcher\native\VERSION") -Raw).Trim() -ne "9.8.7") {
        throw "Version bump did not update native VERSION"
    }
    $beforeInvalid = Get-ChildItem -LiteralPath $testRoot -Recurse -File | ForEach-Object {
        "$($_.FullName)=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }
    try {
        & $bumpScript -Version "v9.8.7" -ProjectRoot $testRoot | Out-Null
        throw "Invalid version was accepted"
    }
    catch {
        if ($_.Exception.Message -eq "Invalid version was accepted") {
            throw
        }
    }
    $afterInvalid = Get-ChildItem -LiteralPath $testRoot -Recurse -File | ForEach-Object {
        "$($_.FullName)=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }
    if (Compare-Object $beforeInvalid $afterInvalid) {
        throw "Invalid version changed files"
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Write-Host "Version policy checks passed for $version"
