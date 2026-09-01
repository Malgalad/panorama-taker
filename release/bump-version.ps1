[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"
if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
    throw "Version must use MAJOR.MINOR.PATCH without a leading v"
}

if (-not $ProjectRoot) {
    $ProjectRoot = Join-Path $PSScriptRoot ".."
}
$projectRoot = (Resolve-Path -LiteralPath $ProjectRoot).ProviderPath
$components = $Version.Split(".")
$targets = @(
    [PSCustomObject]@{
        Path = "stitcher\native\VERSION"
        Pattern = '^\s*\d+\.\d+\.\d+\s*$'
        Replacement = "$Version`n"
    }
    [PSCustomObject]@{
        Path = "mod\cet\PanoramaCaptureProbe\init.lua"
        Pattern = '(?m)^local MOD_VERSION = "[^"]+"(?=\r?$)'
        Replacement = "local MOD_VERSION = `"$Version`""
    }
    [PSCustomObject]@{
        Path = "mod\src\plugin.cpp"
        Pattern = 'RED4EXT_V1_SEMVER\(\d+, \d+, \d+\)'
        Replacement = "RED4EXT_V1_SEMVER($($components[0]), $($components[1]), $($components[2]))"
    }
    [PSCustomObject]@{
        Path = "contracts\example-session.json"
        Pattern = '(?m)^(  "mod_version": )"[^"]+"'
        Replacement = "`${1}`"$Version`""
    }
    [PSCustomObject]@{
        Path = ".github\workflows\release.yml"
        Pattern = '(?m)^(        default: )"[^"]+"(?=\r?$)'
        Replacement = "`${1}`"$Version`""
    }
)

$updates = @()
foreach ($target in $targets) {
    $path = Join-Path $projectRoot $target.Path
    $source = [IO.File]::ReadAllText($path)
    $expression = [regex]::new($target.Pattern)
    if ($expression.Matches($source).Count -ne 1) {
        throw "Expected one version declaration in $($target.Path)"
    }
    $updates += [PSCustomObject]@{
        Path = $path
        RelativePath = $target.Path
        Contents = $expression.Replace($source, $target.Replacement)
    }
}

$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($update in $updates) {
    [IO.File]::WriteAllText($update.Path, $update.Contents, $utf8)
    Write-Host $update.RelativePath
}
