param(
    [string]$Executable = "$PSScriptRoot\build\Release\panorama-webview2-ui-poc.exe",
    [string]$OutputDirectory = "$PSScriptRoot\measurements",
    [int]$IdleSeconds = 8
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Measurement output already exists: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$captureSource = @'
using System;
using System.Runtime.InteropServices;
public static class PocCapture {
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out RECT bounds);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr context, uint flags);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter,
        int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
Add-Type -TypeDefinition $captureSource

function Wait-PocWindow([Diagnostics.Process]$Process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 25
        $Process.Refresh()
    } while (-not $Process.HasExited -and $Process.MainWindowHandle -eq 0 -and
        [DateTime]::UtcNow -lt $deadline)
    if ($Process.HasExited -or $Process.MainWindowHandle -eq 0) {
        throw 'POC did not create its top-level window'
    }
}

function Wait-PocReady([Diagnostics.Process]$Process, [string]$ExpectedTitle = 'Ready') {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 50
        $Process.Refresh()
    } while (-not $Process.HasExited -and
        ($Process.MainWindowHandle -eq 0 -or
         $Process.MainWindowTitle.IndexOf(
             $ExpectedTitle, [StringComparison]::OrdinalIgnoreCase) -lt 0) -and
        [DateTime]::UtcNow -lt $deadline)
    if ($Process.HasExited -or $Process.MainWindowHandle -eq 0 -or
        $Process.MainWindowTitle.IndexOf(
            $ExpectedTitle, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "POC did not reach '$ExpectedTitle': $($Process.MainWindowTitle)"
    }
}

function Move-PocToSecondary([Diagnostics.Process]$Process) {
    $screens = @([Windows.Forms.Screen]::AllScreens)
    $screen = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1
    if ($null -eq $screen) { return }
    $area = $screen.WorkingArea
    if (-not [PocCapture]::SetWindowPos(
            $Process.MainWindowHandle, [IntPtr]::Zero,
            $area.Left + 24, $area.Top + 24, 0, 0, 0x0015)) {
        throw 'Cannot move POC window to the secondary monitor'
    }
}

function Get-ProcessTreeIds([int]$RootId) {
    $all = @(Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId)
    $ids = [Collections.Generic.HashSet[int]]::new()
    [void]$ids.Add($RootId)
    do {
        $added = $false
        foreach ($candidate in $all) {
            if ($ids.Contains([int]$candidate.ParentProcessId) -and
                $ids.Add([int]$candidate.ProcessId)) {
                $added = $true
            }
        }
    } while ($added)
    return @($ids)
}

function Get-TreeProcesses([int]$RootId) {
    $ids = Get-ProcessTreeIds $RootId
    return @(foreach ($id in $ids) {
        Get-Process -Id $id -ErrorAction SilentlyContinue
    })
}

function Measure-ProcessTree([Diagnostics.Process]$Root, [int]$Seconds) {
    $logicalProcessors = [Environment]::ProcessorCount
    $samples = [Collections.Generic.List[object]]::new()
    $previousAt = [DateTime]::UtcNow
    $previousCpu = @{}
    foreach ($process in (Get-TreeProcesses $Root.Id)) {
        $previousCpu[$process.Id] = $process.TotalProcessorTime.TotalMilliseconds
    }
    for ($index = 0; $index -lt $Seconds; ++$index) {
        Start-Sleep -Seconds 1
        $now = [DateTime]::UtcNow
        $elapsed = [Math]::Max(1.0, ($now - $previousAt).TotalMilliseconds)
        $processes = @(Get-TreeProcesses $Root.Id)
        $working = 0L
        $private = 0L
        $cpuDelta = 0.0
        foreach ($process in $processes) {
            $process.Refresh()
            $working += $process.WorkingSet64
            $private += $process.PrivateMemorySize64
            $currentCpu = $process.TotalProcessorTime.TotalMilliseconds
            if ($previousCpu.ContainsKey($process.Id)) {
                $cpuDelta += $currentCpu - [double]$previousCpu[$process.Id]
            }
            $previousCpu[$process.Id] = $currentCpu
        }
        $samples.Add([pscustomobject]@{
            ProcessCount = $processes.Count
            WorkingSetBytes = $working
            PrivateBytes = $private
            CpuPercent = 100.0 * $cpuDelta / ($elapsed * $logicalProcessors)
        })
        $previousAt = $now
    }

    $pids = @(Get-ProcessTreeIds $Root.Id)
    $gpuDedicated = 0.0
    $gpuShared = 0.0
    $gpuPercent = 0.0
    $gpuValid = $false
    $counters = Get-Counter -Counter @(
        '\GPU Process Memory(*)\Dedicated Usage',
        '\GPU Process Memory(*)\Shared Usage',
        '\GPU Engine(*)\Utilization Percentage'
    ) -ErrorAction SilentlyContinue
    if ($null -ne $counters) {
        foreach ($counter in $counters.CounterSamples) {
            $matchesProcess = $false
            foreach ($id in $pids) {
                if ($counter.InstanceName.StartsWith("pid_${id}_")) {
                    $matchesProcess = $true
                    break
                }
            }
            if (-not $matchesProcess -or $counter.Status -notin @(0, 1)) { continue }
            if ($counter.Path -like '*\Dedicated Usage') {
                $gpuDedicated += $counter.CookedValue
                $gpuValid = $true
            } elseif ($counter.Path -like '*\Shared Usage') {
                $gpuShared += $counter.CookedValue
            } elseif ($counter.Path -like '*\Utilization Percentage') {
                $gpuPercent += $counter.CookedValue
            }
        }
    }
    return [ordered]@{
        samples = $samples.Count
        process_count_max = ($samples | Measure-Object ProcessCount -Maximum).Maximum
        working_set_mib_peak = [Math]::Round(
            ($samples | Measure-Object WorkingSetBytes -Maximum).Maximum / 1MB, 3)
        working_set_mib_mean = [Math]::Round(
            ($samples | Measure-Object WorkingSetBytes -Average).Average / 1MB, 3)
        private_mib_peak = [Math]::Round(
            ($samples | Measure-Object PrivateBytes -Maximum).Maximum / 1MB, 3)
        private_mib_mean = [Math]::Round(
            ($samples | Measure-Object PrivateBytes -Average).Average / 1MB, 3)
        cpu_percent_peak = [Math]::Round(
            ($samples | Measure-Object CpuPercent -Maximum).Maximum, 3)
        cpu_percent_mean = [Math]::Round(
            ($samples | Measure-Object CpuPercent -Average).Average, 3)
        gpu_counters_valid = $gpuValid
        gpu_percent = if ($gpuValid) { [Math]::Round($gpuPercent, 3) } else { $null }
        gpu_dedicated_mib = if ($gpuValid) { [Math]::Round($gpuDedicated / 1MB, 3) } else { $null }
        gpu_shared_mib = if ($gpuValid) { [Math]::Round($gpuShared / 1MB, 3) } else { $null }
    }
}

function Get-AccessibilityAudit([Diagnostics.Process]$Process) {
    $root = [Windows.Automation.AutomationElement]::FromHandle($Process.MainWindowHandle)
    $items = $root.FindAll(
        [Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    $named = @()
    foreach ($item in $items) {
        $current = $item.Current
        if (-not [string]::IsNullOrWhiteSpace($current.Name)) {
            $named += [ordered]@{
                name = $current.Name
                control_type = $current.ControlType.ProgrammaticName
                focusable = $current.IsKeyboardFocusable
            }
        }
    }
    return [ordered]@{
        descendant_count = $items.Count
        named_descendant_count = $named.Count
        named_descendants = $named
    }
}

function Save-WindowCapture([Diagnostics.Process]$Process, [string]$Path) {
    $previousDpiContext = [PocCapture]::SetThreadDpiAwarenessContext([IntPtr](-4))
    try {
        $bounds = New-Object PocCapture+RECT
        if (-not [PocCapture]::GetWindowRect(
                $Process.MainWindowHandle, [ref]$bounds)) {
            throw 'Cannot query POC window bounds'
        }
        $bitmap = New-Object Drawing.Bitmap(
            ($bounds.Right - $bounds.Left), ($bounds.Bottom - $bounds.Top))
        try {
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            try {
                $context = $graphics.GetHdc()
                try {
                    if (-not [PocCapture]::PrintWindow(
                            $Process.MainWindowHandle, $context, 2)) {
                        throw 'Cannot capture POC window'
                    }
                } finally {
                    $graphics.ReleaseHdc($context)
                }
            } finally {
                $graphics.Dispose()
            }
            $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $bitmap.Dispose()
        }
    } finally {
        if ($previousDpiContext -ne [IntPtr]::Zero) {
            [void][PocCapture]::SetThreadDpiAwarenessContext($previousDpiContext)
        }
    }
}

function Stop-Poc([Diagnostics.Process]$Process) {
    if ($null -eq $Process -or $Process.HasExited) { return }
    [void]$Process.CloseMainWindow()
    if (-not $Process.WaitForExit(3000)) {
        $Process.Kill()
        $Process.WaitForExit()
    }
}

function Start-PocRun(
    [string]$Profile,
    [string]$NativeReport,
    [switch]$Benchmark,
    [switch]$NativeProbe
) {
    $arguments = @('--user-data-dir', $Profile, '--report', $NativeReport)
    if ($Benchmark) { $arguments += '--benchmark' }
    if ($NativeProbe) { $arguments += '--native-probe' }
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru
    try {
        Wait-PocWindow $process
        Move-PocToSecondary $process
        Wait-PocReady $process
    } catch {
        Stop-Poc $process
        throw
    }
    $stopwatch.Stop()
    return [pscustomobject]@{
        Process = $process
        ExternalReadyMs = $stopwatch.Elapsed.TotalMilliseconds
    }
}

$profile = Join-Path $OutputDirectory 'profile'
$coldNativeReport = Join-Path $OutputDirectory 'cold-native.json'
$cold = Start-PocRun $profile $coldNativeReport
try {
    $coldAccessibility = Get-AccessibilityAudit $cold.Process
    Save-WindowCapture $cold.Process (Join-Path $OutputDirectory 'input.png')
    $coldResources = Measure-ProcessTree $cold.Process $IdleSeconds
} finally {
    Stop-Poc $cold.Process
}

$warmNativeReport = Join-Path $OutputDirectory 'warm-native.json'
$warm = Start-PocRun $profile $warmNativeReport
try {
    $warmResources = Measure-ProcessTree $warm.Process $IdleSeconds
} finally {
    Stop-Poc $warm.Process
}

$benchmarkNativeReport = Join-Path $OutputDirectory 'benchmark-native.json'
$benchmark = Start-PocRun $profile $benchmarkNativeReport -Benchmark
try {
    Wait-PocReady $benchmark.Process 'Benchmark ready'
    $benchmarkResources = Measure-ProcessTree $benchmark.Process 3
    Save-WindowCapture $benchmark.Process (Join-Path $OutputDirectory 'preview-transfer.png')
} finally {
    Stop-Poc $benchmark.Process
}

$nativeProbeReport = Join-Path $OutputDirectory 'native-probe.json'
$nativeProbe = Start-PocRun $profile $nativeProbeReport -NativeProbe
try {
    $nativeProbeAccessibility = Get-AccessibilityAudit $nativeProbe.Process
    Save-WindowCapture $nativeProbe.Process (Join-Path $OutputDirectory 'native-probe.png')
} finally {
    Stop-Poc $nativeProbe.Process
}

$releaseDirectory = Split-Path -Parent $Executable
$payloadFiles = @(
    Get-Item -LiteralPath $Executable
    Get-ChildItem -LiteralPath (Join-Path $releaseDirectory 'ui') -File -Recurse
)
$runtimeKey = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
$runtimeVersion = if (Test-Path $runtimeKey) {
    (Get-ItemProperty $runtimeKey).pv
} else { $null }
$report = [ordered]@{
    measured_at = [DateTimeOffset]::Now.ToString('o')
    executable = $Executable
    executable_mib = [Math]::Round((Get-Item -LiteralPath $Executable).Length / 1MB, 3)
    deployed_payload_mib = [Math]::Round(
        ($payloadFiles | Measure-Object Length -Sum).Sum / 1MB, 3)
    deployed_file_count = $payloadFiles.Count
    webview2_runtime_version = $runtimeVersion
    cold_external_ready_ms = [Math]::Round($cold.ExternalReadyMs, 3)
    warm_external_ready_ms = [Math]::Round($warm.ExternalReadyMs, 3)
    cold_native = Get-Content -LiteralPath $coldNativeReport -Raw | ConvertFrom-Json
    warm_native = Get-Content -LiteralPath $warmNativeReport -Raw | ConvertFrom-Json
    benchmark_native = Get-Content -LiteralPath $benchmarkNativeReport -Raw | ConvertFrom-Json
    cold_idle = $coldResources
    warm_idle = $warmResources
    benchmark_resident = $benchmarkResources
    accessibility = $coldAccessibility
    native_sibling_accessibility = $nativeProbeAccessibility
}
$reportPath = Join-Path $OutputDirectory 'report.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
$report | ConvertTo-Json -Depth 8
