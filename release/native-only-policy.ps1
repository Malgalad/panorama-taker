function Get-NativeOnlyRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd("\", "/")
    $fullPath = [IO.Path]::GetFullPath($Path)
    return $fullPath.Substring($rootPath.Length).TrimStart("\", "/").Replace("\", "/")
}

function Test-NativeOnlyExecutableText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Needle
    )

    $bytes = [IO.File]::ReadAllBytes($Path)
    return [Text.Encoding]::ASCII.GetString($bytes).Contains($Needle) -or
        [Text.Encoding]::Unicode.GetString($bytes).Contains($Needle)
}

function Assert-NativeOnlyArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File)
    $executables = @($files | Where-Object { $_.Extension -eq ".exe" })
    $libraries = @($files | Where-Object { $_.Extension -eq ".dll" })
    $relativeFiles = @($files | ForEach-Object {
        [PSCustomObject]@{
            File = $_
            Path = Get-NativeOnlyRelativePath -Root $Root -Path $_.FullName
        }
    })
    $comparisonPayload = @($relativeFiles | Where-Object {
        $_.File.Name -in @(
            "PanoramaCaptureStitcher-Python.exe",
            "PanoramaCaptureStitcher-Native.exe"
        )
    })
    if ($comparisonPayload.Count -ne 0) {
        throw "Native-only archive contains comparison entry point: $($comparisonPayload.Path -join ', ')"
    }

    $vendorComputePayload = @($relativeFiles | Where-Object {
        $_.Path -match '(?i)(cuda|cupy|nvrtc|cudart|cublas|cufft|curand|cusolver|cusparse|nvjpeg)'
    })
    if ($vendorComputePayload.Count -ne 0) {
        throw "Native-only archive contains CUDA-family payload: $($vendorComputePayload.Path -join ', ')"
    }

    $pythonPayload = @($relativeFiles | Where-Object {
        $_.File.Extension -match '(?i)^\.(py|pyc|pyo|pyd|pyz|whl|egg|spec)$' -or
        $_.File.Name -match '(?i)^(python(?:\d+(?:\.\d+)*)?\.(exe|dll)|libpython.*|base_library\.zip|pyvenv\.cfg)$' -or
        $_.Path -match '(?i)(^|/)(_internal|site-packages|dist-packages|[^/]+\.(dist-info|egg-info))(/|$)' -or
        $_.File.Name -match '(?i)^(analysis|build|pkg|pyz)-?\d*\.toc$'
    })
    if ($pythonPayload.Count -ne 0) {
        throw "Native-only archive contains Python package, runtime, or PyInstaller payload: $($pythonPayload.Path -join ', ')"
    }

    if ($executables.Count -ne 1 -or $executables[0].Name -ne "PanoramaCaptureStitcher.exe") {
        throw "Native-only archive must contain exactly PanoramaCaptureStitcher.exe"
    }
    if ($libraries.Count -ne 1 -or $libraries[0].Name -ne "pano_gpu.dll") {
        throw "Native-only archive must contain exactly pano_gpu.dll"
    }

    if (Test-NativeOnlyExecutableText -Path $executables[0].FullName -Needle "--native-ui") {
        throw "Native-only application still contains the legacy --native-ui switch"
    }
}

function Test-NativeOnlySourceExclusion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    if ($RelativePath -match '(?i)(^|/)(\.git|\.local|\.pytest_cache|\.venv[^/]*|__pycache__|[^/]+\.(egg-info|dist-info)|build(?:-[^/]*)?|dist)(/|$)') {
        return $true
    }
    if ($RelativePath -match '^(docs/|PLAN\.md$)') {
        return $true
    }
    if ($RelativePath -match '(?i)(^|/)(deps|third_party)(/|$)' -or
        $RelativePath -match '(?i)(^|/)(license|notice)(\.[^/]*)?$') {
        return $true
    }
    return $RelativePath -in @(
        "release/native-only-policy.ps1",
        "release/test-native-only-policy.ps1"
    )
}

function Assert-NativeOnlySourceTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $forbiddenPaths = @()
    $forbiddenDependencies = @()
    $textExtensions = @(
        ".c", ".cc", ".cmake", ".cpp", ".css", ".h", ".hpp", ".html", ".js", ".json",
        ".lua", ".md", ".ps1", ".rc", ".toml", ".txt", ".xml", ".yaml", ".yml"
    )
    $dependencyPattern = '(?i)(^|[^A-Za-z0-9_])(python(?:3(?:\.\d+)?)?|pip|pyinstaller|pytest|ruff|mypy|uv|ctypes|tkinter|cuda|cupy|nvrtc|cudart)([^A-Za-z0-9_]|$)'
    $legacyPattern = '(?i)(--native-ui|PanoramaCaptureStitcher-(Python|Native)\.exe|StitcherFrontend\s*=\s*["'']?(python|comparison))'

    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relativePath = Get-NativeOnlyRelativePath -Root $Root -Path $file.FullName
        if (Test-NativeOnlySourceExclusion -RelativePath $relativePath) {
            continue
        }
        if ($file.Extension -match '(?i)^\.(py|pyc|pyo|pyd|pyz|whl|egg|spec)$' -or
            $file.Name -match '(?i)^(pyproject\.toml|uv\.lock|requirements[^/]*\.txt|Pipfile(?:\.lock)?|setup\.(cfg|py)|tox\.ini)$') {
            $forbiddenPaths += $relativePath
            continue
        }
        if ($file.Extension.ToLowerInvariant() -notin $textExtensions -and
            $file.Name -ne "CMakeLists.txt") {
            continue
        }
        $contents = [IO.File]::ReadAllText($file.FullName)
        if ($contents -match $dependencyPattern -or $contents -match $legacyPattern) {
            $forbiddenDependencies += $relativePath
        }
    }

    if ($forbiddenPaths.Count -ne 0) {
        throw "Native-only source tree contains forbidden Python/package files: $($forbiddenPaths -join ', ')"
    }
    if ($forbiddenDependencies.Count -ne 0) {
        throw "Native-only source tree contains forbidden dependency or legacy-frontend references: $($forbiddenDependencies -join ', ')"
    }
}
