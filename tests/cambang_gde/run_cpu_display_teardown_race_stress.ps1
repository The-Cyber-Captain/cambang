# Empirical stress-test driver for the extension-teardown race raised against
# enqueue_live_cpu_texture_create()/request_pending_live_cpu_texture_create_drain()
# (see scripts/cpu_display_teardown_race_stress.gd for what is being raced and why).
#
# Each iteration is a fresh Godot process: the script hammers
# CamBANGStreamResult.get_display_view() on a background Thread with zero
# synchronization, then calls quit() immediately, so the engine's shutdown
# sequence (and eventually the GDExtension terminator) proceeds while that
# thread may still be mid-flight. A single run gives one shot at the race
# window; running many independent processes maximizes the chance of hitting
# whatever interleaving the engine's actual (undocumented-from-here) shutdown
# ordering allows.
#
# This does not prove absence of the race -- it can only report whether any
# of N independent attempts produced observable evidence of a crash. A clean
# run across N iterations is evidence, not proof.

param(
    [string]$GodotExe = "C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe",
    [string]$ProjectPath = $PSScriptRoot,
    [int]$Iterations = 100,
    [int]$TimeoutSec = 20,
    [string]$LogRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Stop-ProcessTreeSafe {
    param([int]$ProcessId)
    if ($ProcessId -le 0) { return }
    try { & taskkill.exe /PID $ProcessId /T /F | Out-Null } catch { }
}

$projectFullPath = (Resolve-Path $ProjectPath).Path
$scriptRelPath = "res://scripts/cpu_display_teardown_race_stress.gd"

$logRootResolved = if ([string]::IsNullOrWhiteSpace($LogRoot)) {
    Join-Path $projectFullPath "run-logs\cpu_display_teardown_race_stress"
} else {
    $LogRoot
}
New-Item -ItemType Directory -Force -Path $logRootResolved | Out-Null

Write-Host "Running cpu_display_teardown_race_stress for $Iterations iteration(s)..."
Write-Host "Logs: $logRootResolved"

$cleanCount = 0
$flaggedRuns = New-Object System.Collections.Generic.List[object]

# Exit codes that correspond to native crashes / abnormal termination on
# Windows rather than an ordinary quit(0)/quit(1). Godot's own controlled
# quit() paths only ever produce small non-negative codes.
function Test-CrashLikeExitCode {
    param([int]$ExitCode)
    if ($ExitCode -eq 0 -or $ExitCode -eq 1) {
        return $false
    }
    return $true
}

$crashSignaturePatterns = @(
    "(?i)access violation",
    "(?i)segmentation fault",
    "(?i)0xC0000005",
    "(?i)stack overflow",
    "(?i)unhandled exception",
    "(?i)Program crashed",
    "(?i)Dr\.?\s*Watson",
    "(?i)Windows Error Reporting"
)

for ($i = 1; $i -le $Iterations; $i++) {
    $stdoutPath = Join-Path $logRootResolved ("iter_{0:D4}_stdout.log" -f $i)
    $stderrPath = Join-Path $logRootResolved ("iter_{0:D4}_stderr.log" -f $i)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $GodotExe
    $psi.Arguments = "--headless --path `"$projectFullPath`" --script $scriptRelPath"
    $psi.WorkingDirectory = $projectFullPath
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi

    $stdoutBuilder = New-Object System.Text.StringBuilder
    $stderrBuilder = New-Object System.Text.StringBuilder
    $stdoutEvent = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($null -ne $Event.SourceEventArgs.Data) {
            $Event.MessageData.AppendLine($Event.SourceEventArgs.Data) | Out-Null
        }
    } -MessageData $stdoutBuilder
    $stderrEvent = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($null -ne $Event.SourceEventArgs.Data) {
            $Event.MessageData.AppendLine($Event.SourceEventArgs.Data) | Out-Null
        }
    } -MessageData $stderrBuilder

    $null = $proc.Start()
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()

    $exited = $proc.WaitForExit($TimeoutSec * 1000)
    $timedOut = -not $exited
    $exitCode = 0
    if ($timedOut) {
        Stop-ProcessTreeSafe -ProcessId $proc.Id
        $proc.WaitForExit(5000) | Out-Null
    } else {
        $exitCode = $proc.ExitCode
    }

    Unregister-Event -SourceIdentifier $stdoutEvent.Name -ErrorAction SilentlyContinue
    Unregister-Event -SourceIdentifier $stderrEvent.Name -ErrorAction SilentlyContinue

    $stdoutText = $stdoutBuilder.ToString()
    $stderrText = $stderrBuilder.ToString()
    Set-Content -LiteralPath $stdoutPath -Value $stdoutText
    Set-Content -LiteralPath $stderrPath -Value $stderrText

    $reachedRacePoint = $stdoutText -match [Regex]::Escape("completed its first get_display_view() call; quitting now")
    $crashSignatureHit = $false
    foreach ($pattern in $crashSignaturePatterns) {
        if ($stdoutText -match $pattern -or $stderrText -match $pattern) {
            $crashSignatureHit = $true
            break
        }
    }

    $isAnomalous = $timedOut -or (Test-CrashLikeExitCode -ExitCode $exitCode) -or $crashSignatureHit

    if ($isAnomalous) {
        $flaggedRuns.Add([PSCustomObject]@{
            Iteration        = $i
            ExitCode         = $exitCode
            TimedOut         = $timedOut
            ReachedRacePoint = $reachedRacePoint
            CrashSignature   = $crashSignatureHit
            StdoutLog        = $stdoutPath
            StderrLog        = $stderrPath
        })
        Write-Host ("  iter {0,4}: FLAGGED (exit={1} timeout={2} reached_race_point={3} crash_signature={4})" -f `
            $i, $exitCode, $timedOut, $reachedRacePoint, $crashSignatureHit) -ForegroundColor Yellow
    } else {
        $cleanCount++
        if (-not $reachedRacePoint) {
            Write-Host ("  iter {0,4}: clean exit, but never reached the race point (bootstrap issue?)" -f $i) -ForegroundColor DarkYellow
        }
    }
}

Write-Host ""
Write-Host "===== cpu_display_teardown_race_stress summary ====="
Write-Host ("Iterations: {0}" -f $Iterations)
Write-Host ("Clean:      {0}" -f $cleanCount)
Write-Host ("Flagged:    {0}" -f $flaggedRuns.Count)

if ($flaggedRuns.Count -gt 0) {
    Write-Host ""
    Write-Host "Flagged iterations (see per-iteration logs for detail):"
    $flaggedRuns | Format-Table -AutoSize
    exit 1
}

Write-Host ""
Write-Host "No crash-like exit codes, timeouts, or crash signatures observed across $Iterations run(s)."
Write-Host "This is empirical evidence for this run count, not a proof of absence."
exit 0
