[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$policyScript = Join-Path $PSScriptRoot "native-only-policy.ps1"
$auditScript = Join-Path $PSScriptRoot "audit-windows-stitcher.ps1"
. $policyScript

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("pano-native-only-policy-" + [guid]::NewGuid())

function New-CleanArchiveFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $root = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Force -Path (Join-Path $root "licenses") | Out-Null
    [IO.File]::WriteAllBytes(
        (Join-Path $root "PanoramaCaptureStitcher.exe"),
        [Text.Encoding]::ASCII.GetBytes("native-webview-application")
    )
    [IO.File]::WriteAllBytes(
        (Join-Path $root "pano_gpu.dll"),
        [Text.Encoding]::ASCII.GetBytes("native-d3d12-backend")
    )
    Set-Content -LiteralPath (Join-Path $root "README.md") -Value "Native stitcher"
    Set-Content -LiteralPath (Join-Path $root "licenses\NOTICE.txt") -Value "Notices"
    return $root
}

function New-FixtureArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $archive = "$Root.zip"
    Compress-Archive -Path (Join-Path $Root "*") -DestinationPath $archive
    return $archive
}

function Assert-ArchivePasses {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Mutate
    )

    $root = New-CleanArchiveFixture -Name $Name
    & $Mutate $root
    $archive = New-FixtureArchive -Root $root
    & $auditScript -ArchivePath $archive -ProjectRoot $testRoot -PolicyOnly
}

function Assert-ArchiveRejected {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Expected,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Mutate
    )

    try {
        Assert-ArchivePasses -Name $Name -Mutate $Mutate
    }
    catch {
        if ($_.Exception.Message -notlike "*$Expected*") {
            throw "Archive fixture $Name hit the wrong policy: $($_.Exception.Message)"
        }
        Write-Host "Rejected archive fixture $Name`: $($_.Exception.Message)"
        return
    }
    throw "Archive fixture $Name was not rejected"
}

function New-CleanSourceFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $root = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Force -Path (Join-Path $root "stitcher\native\src") | Out-Null
    Set-Content -LiteralPath (Join-Path $root "stitcher\native\src\main.cpp") `
        -Value @(
            "// D3D12 WebView2 pano_gpu.dll --gpu-strict"
            "int main() { return 0; }"
        )
    return $root
}

function Assert-SourceRejected {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Expected,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Mutate
    )

    $root = New-CleanSourceFixture -Name $Name
    & $Mutate $root
    try {
        Assert-NativeOnlySourceTree -Root $root
    }
    catch {
        if ($_.Exception.Message -notlike "*$Expected*") {
            throw "Source fixture $Name hit the wrong policy: $($_.Exception.Message)"
        }
        Write-Host "Rejected source fixture $Name`: $($_.Exception.Message)"
        return
    }
    throw "Source fixture $Name was not rejected"
}

try {
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

    Assert-ArchivePasses -Name "clean-archive" -Mutate {}
    Assert-ArchiveRejected -Name "python-runtime" -Expected "Python package, runtime, or PyInstaller" -Mutate {
        param($root)
        Set-Content -LiteralPath (Join-Path $root "python312.dll") -Value "runtime"
    }
    Assert-ArchiveRejected -Name "python-package" -Expected "Python package, runtime, or PyInstaller" -Mutate {
        param($root)
        $path = Join-Path $root "site-packages\example"
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        Set-Content -LiteralPath (Join-Path $path "payload.dat") -Value "package"
    }
    Assert-ArchiveRejected -Name "pyinstaller-metadata" -Expected "Python package, runtime, or PyInstaller" -Mutate {
        param($root)
        $path = Join-Path $root "_internal"
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        Set-Content -LiteralPath (Join-Path $path "archive.dat") -Value "bundle"
    }
    foreach ($payload in @("cuda.dll", "cupy.pyd", "nvrtc64.dll", "cudart64.dll")) {
        Assert-ArchiveRejected -Name ("vendor-" + $payload.Replace(".", "-")) `
            -Expected "CUDA-family payload" -Mutate {
            param($root)
            Set-Content -LiteralPath (Join-Path $root $payload) -Value "vendor runtime"
        }.GetNewClosure()
    }
    Assert-ArchiveRejected -Name "comparison-entry" -Expected "comparison entry point" -Mutate {
        param($root)
        Set-Content -LiteralPath (Join-Path $root "PanoramaCaptureStitcher-Python.exe") `
            -Value "comparison"
    }
    Assert-ArchiveRejected -Name "legacy-switch" -Expected "legacy --native-ui switch" -Mutate {
        param($root)
        [IO.File]::AppendAllText(
            (Join-Path $root "PanoramaCaptureStitcher.exe"),
            "--native-ui",
            [Text.Encoding]::Unicode
        )
    }

    $cleanSource = New-CleanSourceFixture -Name "clean-source"
    Assert-NativeOnlySourceTree -Root $cleanSource
    $sourceAuditRoot = New-CleanArchiveFixture -Name "source-audit-archive"
    $sourceAuditArchive = New-FixtureArchive -Root $sourceAuditRoot
    & $auditScript -ArchivePath $sourceAuditArchive -ProjectRoot $testRoot `
        -AuditSourceTree -SourceTreeRoot $cleanSource -PolicyOnly
    Assert-SourceRejected -Name "python-extension" -Expected "forbidden Python/package files" -Mutate {
        param($root)
        Set-Content -LiteralPath (Join-Path $root "tool.py") -Value "pass"
    }
    Assert-SourceRejected -Name "python-package-metadata" -Expected "forbidden Python/package files" -Mutate {
        param($root)
        Set-Content -LiteralPath (Join-Path $root "pyproject.toml") -Value "[project]"
    }
    foreach ($dependency in @("Python", "PyInstaller", "ctypes", "Tkinter", "CUDA", "CuPy", "NVRTC")) {
        Assert-SourceRejected -Name ("dependency-" + $dependency.ToLowerInvariant()) `
            -Expected "forbidden dependency or legacy-frontend references" -Mutate {
            param($root)
            Add-Content -LiteralPath (Join-Path $root "stitcher\native\src\main.cpp") `
                -Value "// $dependency"
        }.GetNewClosure()
    }
    Assert-SourceRejected -Name "legacy-native-ui" `
        -Expected "forbidden dependency or legacy-frontend references" -Mutate {
        param($root)
        Add-Content -LiteralPath (Join-Path $root "stitcher\native\src\main.cpp") `
            -Value '// --native-ui'
    }
    Assert-SourceRejected -Name "comparison-selector" `
        -Expected "forbidden dependency or legacy-frontend references" -Mutate {
        param($root)
        Set-Content -LiteralPath (Join-Path $root "release.ps1") `
            -Value '$StitcherFrontend = "comparison"'
    }

    $excludedSource = New-CleanSourceFixture -Name "excluded-source"
    foreach ($directory in @("docs", "build", ".local", "third_party\licenses")) {
        $path = Join-Path $excludedSource $directory
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        Set-Content -LiteralPath (Join-Path $path "historical.py") -Value "import cupy"
    }
    Assert-NativeOnlySourceTree -Root $excludedSource
    $sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath
    Assert-NativeOnlySourceTree -Root $sourceRoot
    Write-Host "Native-only policy fixtures passed"
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
