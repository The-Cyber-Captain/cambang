<#
.SYNOPSIS
    Runs a scene across a matrix of target OS, renderer, provider backing and
    synthetic producer output form, one child process per combination.

.DESCRIPTION
    Each combination is executed by run_godot.ps1 in its own PowerShell child
    process, so a crash, hang or hard exit in one combination cannot take the
    matrix down with it. Every run keeps its own classified run directory and
    summary.jsonl entry exactly as a single run_godot.ps1 invocation would --
    this script adds no logging of its own beyond a per-combination banner and
    a final tally.

    Four axes, each a list:

      -TargetOs              windows | android      (the OS, not the provider)
      -Renderers             compatibility | gl_compatibility | mobile
                             (forward_plus is rejected by run_godot.ps1 on every
                              target; it needs a direct Godot invocation)
      -ProviderBackings      synthetic | platform   (Synthetic vs platform-backed)
      -ProviderOutputForms   auto | cpu_only | gpu_only | cpu_and_gpu

    "platform" is deliberately confined to -ProviderBackings. The OS axis is
    -TargetOs, matching the parameter of the same name on run_godot.ps1, which
    was renamed away from RunPlatform for exactly this reason.

.NOTES
    ProviderOutputForm is a SyntheticProvider knob. Combining it with a
    platform backing produces runs that differ only in an argument the provider
    ignores, so those combinations are skipped by default and reported as
    skipped. -NoSkipRedundant runs them anyway.

    Android is always windowed by the harness; -Windowed is passed only for
    windows runs, matching run_godot.ps1's own behaviour.

.EXAMPLE
    .\godot_matrix_runner.ps1 -Scenes res://scenes/870_to_image_soak_benchmark.tscn `
        -TargetOs windows -Renderers compatibility,mobile `
        -ProviderBackings synthetic,platform -ProviderOutputForms cpu_only,gpu_only `
        -TimeoutSec 600 -DryRun
#>
param(
    [Parameter(Mandatory)][string[]]$Scenes,

    [ValidateSet("windows", "android")]
    [string[]]$TargetOs = @("windows"),

    # run_godot.ps1 normalises these through Normalize-RenderingMethodValue and
    # THROWS on anything else, on every target and not just Android -- so
    # forward_plus is not offered here. Reaching it needs a direct Godot
    # invocation, outside the launcher and therefore outside run-log capture.
    # compatibility and gl_compatibility are the same value after
    # normalisation and are de-duplicated below.
    [ValidateSet("compatibility", "gl_compatibility", "mobile")]
    [string[]]$Renderers = @("compatibility"),

    [ValidateSet("synthetic", "platform")]
    [string[]]$ProviderBackings = @("synthetic"),

    [ValidateSet("auto", "cpu_only", "gpu_only", "cpu_and_gpu")]
    [string[]]$ProviderOutputForms = @("auto"),

    [int]$TimeoutSec = 180,
    [string]$LogRoot = "",
    [string]$LabelPrefix = "",

    # Applied to synthetic-backed runs only, where a fixed seed makes a run
    # reproducible. The original hand-rolled loop keyed this on windows; keying
    # it on the backing is the same intent expressed against the axis that
    # actually determines whether a seed means anything.
    [string]$BenchSeed = "",

    [string]$AndroidDeviceSerial = "",

    # Print the matrix and the exact run_godot.ps1 arguments without running.
    [switch]$DryRun,

    # Run ProviderOutputForm variations even under a platform backing, which
    # ignores them.
    [switch]$NoSkipRedundant,

    # Keep going after a non-zero exit (the default); -StopOnFailure aborts.
    [switch]$StopOnFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-SceneTag {
    param([Parameter(Mandatory)][string]$ScenePath)
    $leaf = Split-Path -Leaf $ScenePath
    $leaf = [System.IO.Path]::GetFileNameWithoutExtension($leaf)
    # Scenes are numbered (870_to_image_soak_benchmark); the number alone keeps
    # run labels legible, and the full name is still in the run directory.
    if ($leaf -match '^(\d+)') { return $Matches[1] }
    return $leaf
}

# Same renderer under two spellings would otherwise run twice for one result.
$normalisedRenderers = @()
foreach ($r in $Renderers) {
    $n = if ($r -eq "compatibility") { "gl_compatibility" } else { $r }
    if ($normalisedRenderers -notcontains $n) { $normalisedRenderers += $n }
}
if ($normalisedRenderers.Count -ne $Renderers.Count) {
    Write-Host ("  note: renderers de-duplicated after normalisation: {0} -> {1}" -f ($Renderers -join ", "), ($normalisedRenderers -join ", ")) -ForegroundColor DarkYellow
}

$combinations = New-Object System.Collections.Generic.List[object]
foreach ($scene in $Scenes) {
    foreach ($os in $TargetOs) {
        foreach ($renderer in $normalisedRenderers) {
            foreach ($backing in $ProviderBackings) {
                foreach ($form in $ProviderOutputForms) {
                    $redundant = ($backing -eq "platform" -and $ProviderOutputForms.Count -gt 1)
                    if ($redundant -and -not $NoSkipRedundant -and $form -ne $ProviderOutputForms[0]) {
                        continue
                    }
                    $tag = Get-SceneTag -ScenePath $scene
                    $labelParts = @()
                    if (-not [string]::IsNullOrWhiteSpace($LabelPrefix)) { $labelParts += $LabelPrefix }
                    $labelParts += @("scene$tag", $os, $renderer, $backing)
                    if ($backing -eq "synthetic") { $labelParts += $form }
                    $combinations.Add([pscustomobject]@{
                        Scene    = $scene
                        TargetOs = $os
                        Renderer = $renderer
                        Backing  = $backing
                        Form     = $form
                        Label    = ($labelParts -join "_")
                    })
                }
            }
        }
    }
}

Write-Host ""
Write-Host ("Matrix: {0} combination(s)" -f $combinations.Count) -ForegroundColor Cyan
Write-Host ("  scenes={0}" -f ($Scenes -join ", "))
Write-Host ("  targetOs={0}  renderers={1}" -f ($TargetOs -join ", "), ($normalisedRenderers -join ", "))
Write-Host ("  providerBackings={0}  providerOutputForms={1}" -f ($ProviderBackings -join ", "), ($ProviderOutputForms -join ", "))
if ($ProviderBackings -contains "platform" -and $ProviderOutputForms.Count -gt 1 -and -not $NoSkipRedundant) {
    Write-Host "  note: ProviderOutputForm variations skipped under platform backing (provider ignores them); -NoSkipRedundant to include" -ForegroundColor DarkYellow
}
if ($TargetOs -contains "platform") {
    Write-Host "  note: 'platform' is a provider backing, not a target OS" -ForegroundColor DarkYellow
}

$passed = 0
$failed = 0
$failedLabels = New-Object System.Collections.Generic.List[string]

foreach ($c in $combinations) {
    Write-Host ""
    Write-Host ("=== {0} ===" -f $c.Label) -ForegroundColor Cyan

    $extra = @("--rendering-method=$($c.Renderer)", "--cambang-bench-provider=$($c.Backing)")
    if ($c.Backing -eq "synthetic") {
        $extra += "--cambang-synth-producer-output-form=$($c.Form)"
        if (-not [string]::IsNullOrWhiteSpace($BenchSeed)) {
            $extra += "--cambang-bench-seed=$BenchSeed"
        }
    }

    $paramLines = @(
        '$params = @{'
        ('    TargetOs = "{0}"' -f $c.TargetOs)
        ('    Scene = "{0}"' -f $c.Scene)
        '    CaptureLogs = $true'
        ('    TimeoutSec = {0}' -f $TimeoutSec)
        ('    RunLabel = "{0}"' -f $c.Label)
        ('    ExtraArgs = @({0})' -f (($extra | ForEach-Object { '"' + $_ + '"' }) -join ", "))
    )
    if (-not [string]::IsNullOrWhiteSpace($LogRoot)) {
        $paramLines += ('    LogRoot = "{0}"' -f $LogRoot)
    }
    if ($c.TargetOs -eq "android" -and -not [string]::IsNullOrWhiteSpace($AndroidDeviceSerial)) {
        $paramLines += ('    AndroidDeviceSerial = "{0}"' -f $AndroidDeviceSerial)
    }
    $paramLines += '}'
    if ($c.TargetOs -eq "windows") {
        $paramLines += '$params.Windowed = $true'
    }
    $paramLines += '& ".\run_godot.ps1" @params'
    $paramLines += 'exit $LASTEXITCODE'
    $childCommand = ($paramLines -join "`n")

    if ($DryRun) {
        Write-Host "  (dry run) run_godot.ps1 arguments:" -ForegroundColor DarkGray
        foreach ($line in $paramLines) { Write-Host ("    " + $line) -ForegroundColor DarkGray }
        $passed++
        continue
    }

    # Child process per combination: one hung or hard-exiting run cannot end the
    # matrix, and each keeps its own classified run directory and summary entry.
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childCommand))
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded
    $exitCode = $LASTEXITCODE

    if ($exitCode -ne 0) {
        $failed++
        $failedLabels.Add($c.Label)
        Write-Host ("Run returned exit code {0}: {1}" -f $exitCode, $c.Label) -ForegroundColor Red
        if ($StopOnFailure) {
            Write-Host "Stopping: -StopOnFailure was set." -ForegroundColor Red
            break
        }
    }
    else {
        $passed++
    }
}

Write-Host ""
Write-Host ("Matrix: combinations={0} passed={1} failed={2}" -f $combinations.Count, $passed, $failed)
if ($failed -gt 0) {
    Write-Host ("  failed: {0}" -f ($failedLabels -join ", ")) -ForegroundColor Red
}
# A non-zero exit code for a non-zero failure count keeps this usable from
# another script; attended runs can ignore it.
exit ($(if ($failed -gt 0) { 1 } else { 0 }))
