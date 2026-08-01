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
      -ProviderOutputForms   runtime_default | cpu_only | cpu_gpu | gpu_only
      -AndroidDeviceSerials  adb serials, or tag=serial (android only)

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

    # The wire vocabulary, not the C++ enum names: parse_synthetic_producer_
    # output_form_mode (imaging/synthetic/config.h) accepts exactly these four
    # strings and nothing else. "auto" and "cpu_and_gpu" look right -- they are
    # the enum spellings -- and are rejected, which surfaces as
    # "bootstrap failed: start(synthetic) returned 1" rather than an argument
    # error, because the parse failure happens inside start().
    [ValidateSet("runtime_default", "cpu_only", "cpu_gpu", "gpu_only")]
    [string[]]$ProviderOutputForms = @("runtime_default"),

    [int]$TimeoutSec = 180,
    [string]$LogRoot = "",
    [string]$LabelPrefix = "",

    # Windows runs only, and that is a hard constraint rather than a preference:
    # Android mode translates exactly five ExtraArgs -- --rendering-method,
    # --cambang-bench-provider, --cambang-synth-producer-output-form and the two
    # capability-downgrade knobs -- and THROWS on anything else
    # ("Android mode does not know how to translate these ExtraArgs").
    # --cambang-bench-seed is not in that set, so sending it to an Android run
    # aborts the run before it starts. The original hand-rolled loop keyed this
    # on windows for exactly that reason; re-keying it on the provider backing
    # broke every Android synthetic combination.
    [string]$BenchSeed = "",

    # Android device axis. Multiplies the android combinations only; windows
    # runs ignore it. Each entry is either a bare adb serial or "tag=serial",
    # where the tag is used in the run label.
    #
    # A wireless endpoint is a valid serial ("192.168.32.223:5555"), but its
    # punctuation cannot go in a run-directory name and run labels are already
    # close to the Windows path limit -- see the hammer_thermal note in scene
    # 870. Bare serials are therefore sanitised and shortened for the label;
    # supply "quest3=192.168.32.223:5555" when you want a label you chose.
    #
    # Left empty, run_godot.ps1 resolves the device itself, which succeeds only
    # when exactly one device is in adb "device" state and throws otherwise.
    [Alias("AndroidDeviceSerial")]
    [string[]]$AndroidDeviceSerials = @(),

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

function Get-DeviceAxisEntries {
    param([string[]]$Entries)

    $parsed = New-Object System.Collections.Generic.List[object]
    if ($Entries.Count -eq 0) {
        # No device axis: one android run per other-axis combination, with
        # run_godot.ps1 resolving the device (single attached device only).
        $parsed.Add([pscustomobject]@{ Tag = ""; Serial = "" })
        return $parsed
    }
    foreach ($entry in $Entries) {
        $tag = ""
        $serial = $entry
        $split = $entry.IndexOf("=")
        if ($split -gt 0) {
            $tag = $entry.Substring(0, $split).Trim()
            $serial = $entry.Substring($split + 1).Trim()
        }
        if ([string]::IsNullOrWhiteSpace($serial)) {
            throw "Empty adb serial in -AndroidDeviceSerials entry: '$entry'"
        }
        if ([string]::IsNullOrWhiteSpace($tag)) {
            # Sanitise for a directory name, then keep the tail, which is the
            # distinctive part of both a hardware serial and a host:port.
            $tag = ($serial -replace "[^A-Za-z0-9]", "_")
            if ($tag.Length -gt 12) { $tag = $tag.Substring($tag.Length - 12) }
            $tag = $tag.Trim("_")
        }
        $parsed.Add([pscustomobject]@{ Tag = $tag; Serial = $serial })
    }
    return $parsed
}

$deviceAxis = Get-DeviceAxisEntries -Entries $AndroidDeviceSerials

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
                    # The device axis multiplies android only; a windows run has
                    # no adb device, so it takes a single empty entry.
                    $devicesForOs = if ($os -eq "android") { $deviceAxis } else { @([pscustomobject]@{ Tag = ""; Serial = "" }) }
                    foreach ($device in $devicesForOs) {
                        $tag = Get-SceneTag -ScenePath $scene
                        $labelParts = @()
                        if (-not [string]::IsNullOrWhiteSpace($LabelPrefix)) { $labelParts += $LabelPrefix }
                        $labelParts += @("scene$tag", $os)
                        if (-not [string]::IsNullOrWhiteSpace($device.Tag)) { $labelParts += $device.Tag }
                        $labelParts += @($renderer, $backing)
                        if ($backing -eq "synthetic") { $labelParts += $form }
                        $combinations.Add([pscustomobject]@{
                            Scene    = $scene
                            TargetOs = $os
                            Renderer = $renderer
                            Backing  = $backing
                            Form     = $form
                            Serial   = $device.Serial
                            Label    = ($labelParts -join "_")
                        })
                    }
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
if ($TargetOs -contains "android" -and -not [string]::IsNullOrWhiteSpace($BenchSeed)) {
    Write-Host "  note: -BenchSeed is applied to windows runs only; Android mode cannot translate --cambang-bench-seed and would abort the run" -ForegroundColor DarkYellow
}
if ($TargetOs.Count -gt 1 -and $TargetOs[0] -ne "android" -and $TargetOs -contains "android") {
    Write-Host "  note: android runs come after windows here. A handset can doze during a long windows leg, and the pre-flight device check then refuses the run. Put android first in -TargetOs, or keep the device awake." -ForegroundColor DarkYellow
}
if ($TargetOs -contains "android" -and $AndroidDeviceSerials.Count -gt 0) {
    Write-Host ("  androidDevices={0}" -f (($deviceAxis | ForEach-Object { "$($_.Tag)=$($_.Serial)" }) -join ", "))
}
elseif ($TargetOs -contains "android") {
    Write-Host "  androidDevices=(auto-resolved; run_godot.ps1 throws unless exactly one device is attached)" -ForegroundColor DarkYellow
}
$longest = ($combinations | ForEach-Object { $_.Label.Length } | Measure-Object -Maximum).Maximum
if ($longest -gt 70) {
    Write-Host ("  note: longest run label is {0} chars; run directory names also carry a timestamp and scene name, and Windows paths cap at 260" -f $longest) -ForegroundColor DarkYellow
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
        if (-not [string]::IsNullOrWhiteSpace($BenchSeed) -and $c.TargetOs -eq "windows") {
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
    if ($c.TargetOs -eq "android" -and -not [string]::IsNullOrWhiteSpace($c.Serial)) {
        $paramLines += ('    AndroidDeviceSerial = "{0}"' -f $c.Serial)
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
