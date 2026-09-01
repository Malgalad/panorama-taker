[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,

    [string]$ProjectRoot = "",
    [string]$ReportPath = "",
    [switch]$AllowUnavailable,
    [switch]$AuditSourceTree,
    [string]$SourceTreeRoot = "",
    [switch]$PolicyOnly
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "native-only-policy.ps1")
$archive = (Resolve-Path -LiteralPath $ArchivePath).Path
if (-not $ProjectRoot) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
if (-not $SourceTreeRoot) {
    $SourceTreeRoot = $ProjectRoot
}
if (-not $ReportPath) {
    $ReportPath = "$archive.audit.txt"
}
$extractRoot = Join-Path $ProjectRoot "build\Panorama Capture Release Audit"
$probeResult = Join-Path $ProjectRoot "build\gpu-runtime-probe.txt"

if (Test-Path -LiteralPath $extractRoot) {
    Remove-Item -LiteralPath $extractRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
try {
    Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot
    $files = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File)
    Assert-NativeOnlyArchive -Root $extractRoot
    if ($AuditSourceTree) {
        Assert-NativeOnlySourceTree -Root $SourceTreeRoot
    }
    $executables = @($files | Where-Object { $_.Name -eq "PanoramaCaptureStitcher.exe" })
    $nativeDll = @($files | Where-Object { $_.Name -eq "pano_gpu.dll" })
    if ($executables.Count -ne 1 -or $nativeDll.Count -ne 1) {
        throw "Archive must contain exactly PanoramaCaptureStitcher.exe and pano_gpu.dll"
    }
    $forbidden = @($files | Where-Object { $_.Extension -match '(?i)^\.(hlsl|cso|pdb)$' })
    if ($forbidden.Count -gt 0) {
        throw "Archive contains forbidden runtime/compiler/shader files: $($forbidden.Name -join ', ')"
    }
    if ($PolicyOnly) {
        Write-Host "Native-only policy checks passed"
        return
    }

    $dumpbinCommand = Get-Command dumpbin.exe -CommandType Application `
        -ErrorAction SilentlyContinue
    $dumpbin = if ($dumpbinCommand) { $dumpbinCommand.Source } else { "" }
    if (-not $dumpbin) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} `
            "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $dumpbin = @(& $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -find "VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe") |
                Sort-Object -Descending |
                Select-Object -First 1
        }
    }
    if (-not $dumpbin -or -not (Test-Path -LiteralPath $dumpbin -PathType Leaf)) {
        throw "Cannot locate the x64 Visual C++ dependency tool dumpbin.exe"
    }
    Write-Host "Using dumpbin: $dumpbin"

    $runtimeResults = @()
    $quotedProbeResult = "`"$probeResult`""
    foreach ($executable in $executables) {
        Remove-Item -LiteralPath $probeResult -Force -ErrorAction SilentlyContinue
        $probeProcess = Start-Process -FilePath $executable.FullName -ArgumentList @("--verify-gpu-runtime", $quotedProbeResult) -Wait -PassThru
        if ($probeProcess.ExitCode -ne 0 -and -not ($AllowUnavailable -and $probeProcess.ExitCode -eq 2)) {
            $detail = if (Test-Path -LiteralPath $probeResult) {
                Get-Content -LiteralPath $probeResult -Raw
            } else {
                "no diagnostic file"
            }
            throw "$($executable.Name) runtime verification failed with exit $($probeProcess.ExitCode)`: $detail"
        }
        $runtimeResults += "$($executable.Name)=$((Get-Content -LiteralPath $probeResult -Raw).Trim())"
    }

    $peDependencies = @()
    foreach ($binary in @($executables) + @($nativeDll[0])) {
        $dependencies = & $dumpbin /dependents $binary.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "dumpbin failed for $($binary.Name)"
        }
        $peDependencies += "[$($binary.Name)]"
        $peDependencies += @($dependencies | Where-Object {
            $_ -match '(?i)^\s*[A-Za-z0-9_.-]+\.dll\s*$'
        } |
            ForEach-Object { $_.Trim() } | Sort-Object -Unique)
    }
    $forbiddenDependencyPattern = '(?i)(d3dcompiler|nv' + 'rtc|cu' + 'dart|cu' + 'da)'
    if ($peDependencies -match $forbiddenDependencyPattern) {
        throw "PE dependency audit found a forbidden compiler or vendor runtime"
    }
    if ($peDependencies -match '(?i)(msvcp|vcruntime)') {
        throw "Native-only archive depends on an external MSVC redistributable"
    }

    $nativeBackup = "$($nativeDll[0].FullName).verified"
    Copy-Item -LiteralPath $nativeDll[0].FullName -Destination $nativeBackup
    try {
        [IO.File]::WriteAllBytes($nativeDll[0].FullName, [byte[]](0, 1, 2, 3))
        foreach ($executable in $executables) {
            Remove-Item -LiteralPath $probeResult -Force -ErrorAction SilentlyContinue
            $failureProcess = Start-Process -FilePath $executable.FullName -ArgumentList @("--verify-gpu-runtime", $quotedProbeResult) -Wait -PassThru
            if ($failureProcess.ExitCode -ne 3) {
                throw "Corrupted native DLL must make $($executable.Name) produce runtime-failure exit 3; got $($failureProcess.ExitCode)"
            }
        }
    }
    finally {
        Move-Item -LiteralPath $nativeBackup -Destination $nativeDll[0].FullName -Force
    }

    $extractedBytes = ($files | Measure-Object -Property Length -Sum).Sum
    $report = @(
        "archive=$archive"
        "archive_sha256=$((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant())"
        "archive_bytes=$((Get-Item -LiteralPath $archive).Length)"
        "extracted_bytes=$extractedBytes"
        "payload_mode=native"
        "native_dll_sha256=$((Get-FileHash -LiteralPath $nativeDll[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant())"
        "executables:"
    )
    $report += $executables | ForEach-Object {
        "  $($_.Name)=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())"
    }
    $report += "runtime:"
    $report += $runtimeResults | ForEach-Object { "  $_" }
    $report += "pe_dependencies:"
    $report += $peDependencies | ForEach-Object { "  $_" }
    $report += "shader_source_sha256:"
    $report += Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "stitcher\native\shaders") -File -Filter "*.hlsl" |
        Sort-Object Name | ForEach-Object {
            "  $($_.Name)=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())"
        }
    Set-Content -LiteralPath $ReportPath -Value $report -Encoding utf8
    $report | Write-Host
}
finally {
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    Remove-Item -LiteralPath $probeResult -Force -ErrorAction SilentlyContinue
}
