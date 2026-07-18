param(
    [string]$GodotExe = "C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe",
    [string]$ProjectPath = $PSScriptRoot,
    [int]$Iterations = 25,
    [int]$TimeoutSec = 30,
    [string]$LogRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectFullPath = (Resolve-Path $ProjectPath).Path
$launcher = Join-Path $projectFullPath "run_godot.ps1"
$logRootResolved = if ([string]::IsNullOrWhiteSpace($LogRoot)) {
    Join-Path $projectFullPath "run-logs\gpu_display_teardown_race_stress"
} else {
    $LogRoot
}
New-Item -ItemType Directory -Force -Path $logRootResolved | Out-Null

$hardFailures = @(
    "SCRIPT ERROR:",
    "Parse Error",
    "Failed to load script",
    "(?im)^\s*FAIL(?:ED)?\b",
    "ObjectDB instances leaked at exit",
    "RID allocations of type .* were leaked at exit",
    "release rejected after .* bridge closure",
    "wrong.thread"
)

$failed = New-Object System.Collections.Generic.List[object]
for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
    $label = "gpu_teardown_{0:D4}" -f $iteration
    & $launcher `
        -GodotExe $GodotExe `
        -ProjectPath $projectFullPath `
        -Windowed `
        -Script "res://scripts/gpu_display_teardown_race_stress.gd" `
        -CaptureLogs `
        -LogRoot $logRootResolved `
        -RunLabel $label `
        -TimeoutSec $TimeoutSec `
        -HardFailurePatterns $hardFailures `
        -ExtraArgs @("--rendering-method=mobile", "--cambang-synth-producer-output-form=gpu_only")
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $failed.Add([PSCustomObject]@{ Iteration = $iteration; ExitCode = $exitCode; RunLabel = $label })
        Write-Host ("  iteration {0}: FAIL exit={1}" -f $iteration, $exitCode) -ForegroundColor Red
    } else {
        Write-Host ("  iteration {0}: PASS" -f $iteration)
    }
}

Write-Host ("GPU teardown stress: iterations={0} passed={1} failed={2} artifacts={3}" -f `
    $Iterations, ($Iterations - $failed.Count), $failed.Count, $logRootResolved)
if ($failed.Count -ne 0) {
    $failed | Format-Table -AutoSize
    exit 1
}
exit 0
