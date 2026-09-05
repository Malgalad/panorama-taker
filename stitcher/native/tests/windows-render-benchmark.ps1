[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SessionPath,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$Executable = 'C:\dev\pano-stitch-native-stage\build\RelWithDebInfo\pano_app_headless_probe.exe',
    [string[]]$Formats = @('png', 'jpeg', 'exr'),
    [string[]]$Blends = @('hard', 'feather'),
    [int]$Iterations = 1,
    [int]$Width = 0,
    [switch]$FastPng
)
$ErrorActionPreference = 'Stop'
if ($Label -notmatch '^[a-zA-Z0-9_-]+$' -or $Iterations -lt 1) { throw 'Invalid benchmark parameters' }
[IO.Directory]::CreateDirectory($ArtifactDirectory) | Out-Null
foreach ($format in $Formats) {
    if ($format -notin @('png', 'jpeg', 'exr')) { throw 'Invalid format' }
    $extension = if ($format -eq 'jpeg') { 'jpg' } else { $format }
    foreach ($blend in $Blends) {
        if ($blend -notin @('hard', 'feather')) { throw 'Invalid blend' }
        for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
            $prefix = Join-Path $ArtifactDirectory "$Label-$format-$blend-$iteration"
            foreach ($suffix in @(".$extension", '.log', '.err', '.json')) {
                if (Test-Path -LiteralPath "$prefix$suffix") { throw "Refusing to overwrite $prefix$suffix" }
            }
            $arguments = @('render', $SessionPath, '--output', "$prefix.$extension",
                '--format', $format, '--blend', $blend, '--gpu-strict',
                '--gpu-memory-budget-mib', '4096', '--probe-preview-width', '128')
            if ($Width -gt 0) { $arguments += @('--width', "$Width") }
            if ($FastPng) { $arguments += '--fast-png' }
            $quoted = $arguments | ForEach-Object {
                if ($_.Contains('"')) { throw 'Unsupported quote in argument' }
                '"' + $_ + '"'
            }
            $process = Start-Process -FilePath $Executable -ArgumentList ($quoted -join ' ') `
                -WorkingDirectory ([IO.Path]::GetDirectoryName($Executable)) `
                -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.err" `
                -NoNewWindow -Wait -PassThru
            if ($process.ExitCode -ne 0) { throw "Probe failed: $(Get-Content -LiteralPath "$prefix.err" -Raw)" }
            $log = Get-Content -LiteralPath "$prefix.log"
            $record = [ordered]@{
                label = $Label; format = $format; blend = $blend; iteration = $iteration
                fast_png = [bool]$FastPng
                measurements = @($log | Where-Object { $_ -match '^(render_timings|render_resources|result) |^png_encoder=' } | ForEach-Object { "$_" })
                sha256 = (Get-FileHash -LiteralPath "$prefix.$extension" -Algorithm SHA256).Hash
                bytes = (Get-Item -LiteralPath "$prefix.$extension").Length
            }
            $record | ConvertTo-Json | Set-Content -LiteralPath "$prefix.json" -Encoding UTF8
            $record | ConvertTo-Json -Compress
        }
    }
}
