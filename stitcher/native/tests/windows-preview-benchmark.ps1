[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SessionPath,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$Executable = "C:\dev\pano-stitch-native-stage\build\Release\pano_app_headless_probe.exe",
    [int]$Iterations = 3,
    [int]$PreviewWidth = 768,
    [int]$GpuBudgetMiB = 4096,
    [string[]]$Blends = @("hard", "feather"),
    [switch]$NoAutoContrast
)
$ErrorActionPreference = "Stop"
if ($Label -notmatch '^[a-zA-Z0-9_-]+$' -or $Iterations -lt 1) {
    throw "Invalid benchmark label or iteration count"
}
[IO.Directory]::CreateDirectory($ArtifactDirectory) | Out-Null
$results = @()
foreach ($blend in $Blends) {
    if ($blend -notin @("hard", "feather")) { throw "Invalid blend" }
    for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
        $prefix = Join-Path $ArtifactDirectory "$Label-$blend-$iteration"
        foreach ($suffix in @(".rgb", ".log", ".err", ".png")) {
            if (Test-Path -LiteralPath "$prefix$suffix") { throw "Refusing to overwrite $prefix$suffix" }
        }
        $arguments = @("render", $SessionPath, "--output", "$prefix.png",
            "--format", "png", "--blend", $blend, "--gpu-strict",
            "--gpu-memory-budget-mib", "$GpuBudgetMiB", "--probe-preview-width",
            "$PreviewWidth", "--probe-preview-only", "--probe-preview-dump", "$prefix.rgb")
        if ($NoAutoContrast) { $arguments += "--no-auto-contrast" }
        $quoted = $arguments | ForEach-Object {
            if ($_.Contains('"')) { throw "Unsupported quote in probe argument" }
            '"' + $_ + '"'
        }
        $process = Start-Process -FilePath $Executable -ArgumentList ($quoted -join ' ') `
            -WorkingDirectory ([IO.Path]::GetDirectoryName($Executable)) `
            -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.err" `
            -NoNewWindow -Wait -PassThru
        if ($process.ExitCode -ne 0) {
            throw "Probe failed: $(Get-Content -LiteralPath "$prefix.err" -Raw)"
        }
        $log = Get-Content -LiteralPath "$prefix.log" -Raw
        $time = [regex]::Match($log, 'preview_time ms=([0-9.]+)')
        $memory = [regex]::Match($log,
            'preview_resources samples=(\d+) working_bytes=(\d+) private_bytes=(\d+) local_bytes=(\d+) nonlocal_bytes=(\d+)')
        if (-not $time.Success -or -not $memory.Success) { throw "Missing benchmark measurements" }
        $result = [ordered]@{
            label = $Label; blend = $blend; iteration = $iteration
            preview_ms = [double]::Parse($time.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
            samples = [int]$memory.Groups[1].Value
            working_bytes = [uint64]$memory.Groups[2].Value
            private_bytes = [uint64]$memory.Groups[3].Value
            local_bytes = [uint64]$memory.Groups[4].Value
            nonlocal_bytes = [uint64]$memory.Groups[5].Value
            sha256 = (Get-FileHash -LiteralPath "$prefix.rgb" -Algorithm SHA256).Hash
        }
        $results += [pscustomobject]$result
        $result | ConvertTo-Json -Compress
    }
}
$summary = Join-Path $ArtifactDirectory "$Label-summary.json"
if (Test-Path -LiteralPath $summary) { throw "Refusing to overwrite $summary" }
$results | ConvertTo-Json | Set-Content -LiteralPath $summary -Encoding UTF8
