param(
    [string]$BuildDirectory = "$PSScriptRoot\build",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$sdkVersion = '1.0.4191.47'
$sdkDirectory = Join-Path $BuildDirectory "Microsoft.Web.WebView2.$sdkVersion"
$sdkHeader = Join-Path $sdkDirectory 'build\native\include\WebView2.h'

if (-not (Test-Path -LiteralPath $sdkHeader)) {
    New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
    $package = Join-Path $BuildDirectory "Microsoft.Web.WebView2.$sdkVersion.nupkg"
    $archive = Join-Path $BuildDirectory "Microsoft.Web.WebView2.$sdkVersion.zip"
    if (-not (Test-Path -LiteralPath $package)) {
        Invoke-WebRequest `
            -Uri "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$sdkVersion" `
            -OutFile $package
    }
    Copy-Item -LiteralPath $package -Destination $archive -Force
    Expand-Archive -LiteralPath $archive -DestinationPath $sdkDirectory -Force
    Remove-Item -LiteralPath $archive
}

cmake -S $PSScriptRoot -B $BuildDirectory -A x64 `
    "-DWEBVIEW2_SDK_ROOT=$sdkDirectory"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildDirectory --config $Configuration
exit $LASTEXITCODE
