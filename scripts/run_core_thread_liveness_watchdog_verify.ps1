# Death-test driver for core_thread_liveness_watchdog_verify.exe (see
# docs/dev/current_tranche.md, Step 2: enforce the provider prompt/bounded
# contract, and src/smoke/core_thread_liveness_watchdog_verify.cpp).
#
# The .exe deliberately induces a provider call that hangs well past
# CoreRuntime::check_core_thread_liveness()'s 5s staleness threshold. Because
# it is a CAMBANG_INTERNAL_SMOKE build, the watchdog firing means
# std::abort() -- the child process is EXPECTED to terminate abnormally, not
# exit cleanly and not time out. This script is the actual PASS/FAIL verdict
# (the .exe itself cannot self-report success by normal return: abort() ends
# the process at the point of detection).
#
# Mirrors the launch-a-process-and-inspect-its-exit pattern already
# established by run_cpu_display_teardown_race_stress.ps1, but with the
# polarity inverted: there, a crash-like exit is the anomaly being hunted
# for; here, a crash-like exit (specifically, the watchdog's abort()) is the
# expected, required outcome.

param(
    [string]$ExePath = (Join-Path $PSScriptRoot "..\out\core_thread_liveness_watchdog_verify.exe"),
    [int]$TimeoutSec = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Stop-ProcessTreeSafe {
    param([int]$ProcessId)
    if ($ProcessId -le 0) { return }
    try { & taskkill.exe /PID $ProcessId /T /F | Out-Null } catch { }
}

$exeFullPath = (Resolve-Path $ExePath).Path

Write-Host "Running core_thread_liveness_watchdog_verify death test..."
Write-Host "Exe: $exeFullPath"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exeFullPath
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

# The watchdog's always-on log line (CoreRuntime::check_core_thread_liveness()).
# Its presence is direct evidence the detection half of the mechanism ran,
# independent of how the OS ultimately reports the subsequent abort().
$staleDetectedLogged = ($stdoutText -match [Regex]::Escape("stale task detected")) -or
                       ($stderrText -match [Regex]::Escape("stale task detected"))

# The .exe's own controlled failure paths (setup failure, or the watchdog
# simply never firing) return exactly 1. Any other outcome -- a crash-like
# exit code, or a timeout that required a force-kill -- is consistent with
# std::abort() having fired (or, in the timeout case, with abort() having
# triggered an OS-level crash-reporting prompt that never returns control to
# this script; the force-kill above prevents that from hanging the test
# indefinitely, at the cost of not being able to fully distinguish that case
# from a genuine non-abort hang by exit code alone -- staleDetectedLogged is
# the corroborating signal for that case).
$controlledFailureExit = (-not $timedOut) -and ($exitCode -eq 0 -or $exitCode -eq 1)

$passed = $staleDetectedLogged -and (-not $controlledFailureExit)

Write-Host ""
Write-Host "===== core_thread_liveness_watchdog_verify result ====="
Write-Host ("Exit code:            {0}" -f $exitCode)
Write-Host ("Timed out (killed):   {0}" -f $timedOut)
Write-Host ("Stale-task log seen:  {0}" -f $staleDetectedLogged)
Write-Host ""

if (-not $passed) {
    Write-Host "FAIL: watchdog did not demonstrably abort the process as expected." -ForegroundColor Red
    Write-Host "--- stdout ---"
    Write-Host $stdoutText
    Write-Host "--- stderr ---"
    Write-Host $stderrText
    exit 1
}

Write-Host "PASS: process was aborted by the core-thread liveness watchdog as expected"
Write-Host "(stale-task log line observed; exit was not a controlled 0/1 return)."
exit 0
