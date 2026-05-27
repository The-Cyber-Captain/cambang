param(
    [string]$GodotExe = "C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe",
    [string]$ProjectPath = ".",
    [string]$LogPath = ".\godot-suite.log",
    [string]$ArtifactDir = ".\artifacts",
    [int]$RenderWidth = 2200,
    [int]$RenderHeight = 1600,
    [int]$StepTimeoutSec = 120,

    # Renderer buckets used by StatusPanel fixture metadata.
    # Override CompatibilityRendererArgs locally if your Godot build prefers e.g.:
    #   -CompatibilityRendererArgs @("--headless", "--rendering-method", "gl_compatibility")
    [string[]]$AnyRendererArgs = @("--headless"),
    [string[]]$GpuRendererArgs = @("--headless"),
    [string[]]$CompatibilityRendererArgs = @("--headless", "--rendering-driver", "opengl3")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ============================================================
# EDIT THESE LISTS
# ============================================================

# StatusPanel harness fixtures are now discovered dynamically from:
#   fixtures/status_panel/**/*.json
# using each fixture's renderer_profile metadata:
#   any | gpu | compatibility
# Missing, unknown, or ambiguous renderer_profile values are preflight failures.
$StatusPanelFixtureRoot = "res://fixtures/status_panel"

# 1) headless standalone scripts
$HeadlessScripts = @(
    "res://scripts/status_panel_provider_only_transition_harness.gd",
    "res://scripts/status_panel_nil_after_stop_harness.gd"
)

# 2) curated non-headless render script fixture list with fixture -> output PNG.
# This remains explicit because visual artifact generation is curated and slower/noisier
# than harness projection validation.
$RenderFixtures = @(
    "res://fixtures/status_panel/fixture_adversarial_missing_native_coverage_authoritative.json",
    "res://fixtures/status_panel/fixture_native_coverage_unknown_no_snapshot_with_retained.json",
    "res://fixtures/status_panel/fixture_native_coverage_unknown_no_snapshot_server_only.json",
    "res://fixtures/status_panel/review_multistream/fixture_review_multistream_preview_active.json",
    "res://fixtures/status_panel/review_multistream/fixture_review_multistream_both_stopped.json",
    "res://fixtures/status_panel/review_multistream/fixture_review_multistream_no_snapshot_with_preserved.json",
    "res://fixtures/status_panel/review_multistream/fixture_review_multistream_preview_stopped_viewfinder_present.json",
    "res://fixtures/status_panel/review_multistream/fixture_review_multistream_viewfinder_active.json",
    "res://fixtures/status_panel/review_orphan/fixture_review_orphan_stream_active.json",
    "res://fixtures/status_panel/review_orphan/fixture_review_orphan_device_branch_active.json",
    "res://fixtures/status_panel/review_orphan/fixture_review_orphan_native_object_active.json"
)

# 3) headless scenes with quit-after
$SceneRuns = @(
    @{ Scene = "res://scenes/60_restart_boundary_abuse.tscn";               QuitAfter = 10   },
    @{ Scene = "res://scenes/61_tick_bounded_coalescing_abuse.tscn";        QuitAfter = 1000 },
    @{ Scene = "res://scenes/62_snapshot_polling_immutability_abuse.tscn";  QuitAfter = 1000 },
    @{ Scene = "res://scenes/63_snapshot_observer_minimal.tscn";            QuitAfter = 10   },
    @{ Scene = "res://scenes/65_public_boundary_verify.tscn";               QuitAfter = 10   },
    @{ Scene = "res://scenes/70_result_retrieval_verification.tscn";        QuitAfter = 20   }
)

# Optional custom success rules by step name.
# Signature:
#   param($exitCode, $stepName, $stdoutText, $stderrText, $combinedText)
# Example:
# $CustomSuccessChecks["scene:61_tick_bounded_coalescing_abuse"] = {
#     param($exitCode, $stepName, $stdoutText, $stderrText, $combinedText)
#     return $exitCode -in 0, 2
# }
$CustomSuccessChecks = @{}

# ============================================================
# INTERNALS
# ============================================================

Remove-Item $LogPath -ErrorAction Ignore
New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

$Results = New-Object System.Collections.Generic.List[object]

function Write-LogLine {
    param([string]$Text = "")
    Add-Content -Path $LogPath -Value $Text
}

function Get-BaseNameNoExt {
    param([Parameter(Mandatory)][string]$Path)

    $leaf = Split-Path $Path -Leaf
    return [System.IO.Path]::GetFileNameWithoutExtension($leaf)
}

function Resolve-ProjectLocalPath {
    param([Parameter(Mandatory)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }

    return Join-Path $ProjectPath $Path
}

function Convert-ResPathToLocalPath {
    param([Parameter(Mandatory)][string]$ResPath)

    if ($ResPath -like "res://*") {
        $relative = $ResPath -replace "^res://", ""
        $relative = $relative -replace "/", "\"
        return Resolve-ProjectLocalPath $relative
    }

    return Resolve-ProjectLocalPath $ResPath
}

function Convert-LocalPathToResPath {
    param([Parameter(Mandatory)][string]$LocalPath)

    $projectFull = [System.IO.Path]::GetFullPath((Resolve-Path $ProjectPath).Path)
    $fileFull = [System.IO.Path]::GetFullPath((Resolve-Path $LocalPath).Path)

    if (-not $projectFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $projectFull = $projectFull + [System.IO.Path]::DirectorySeparatorChar
    }

    if (-not $fileFull.StartsWith($projectFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside project: $LocalPath"
    }

    $relative = $fileFull.Substring($projectFull.Length)
    $relative = $relative -replace "\\", "/"
    return "res://$relative"
}

function Get-RenderOutputPath {
    param(
        [Parameter(Mandatory)][string]$FixturePath,
        [Parameter(Mandatory)][string]$ArtifactDirPath
    )

    $name = Get-BaseNameNoExt $FixturePath
    return Join-Path $ArtifactDirPath "$name.png"
}

function Get-RendererArgsForProfile {
    param([Parameter(Mandatory)][string]$RendererProfile)

    switch ($RendererProfile) {
        "any"           { return @($AnyRendererArgs) }
        "gpu"           { return @($GpuRendererArgs) }
        "compatibility" { return @($CompatibilityRendererArgs) }
        default          { throw "Unsupported renderer_profile '$RendererProfile'" }
    }
}

function Get-StatusPanelFixtureMetadata {
    $fixtureRootLocal = Convert-ResPathToLocalPath $StatusPanelFixtureRoot

    if (-not (Test-Path $fixtureRootLocal)) {
        throw "StatusPanel fixture root not found: $fixtureRootLocal"
    }

    $fixtures = @(Get-ChildItem $fixtureRootLocal -Recurse -Filter *.json | ForEach-Object {
        $json = Get-Content $_.FullName -Raw | ConvertFrom-Json
        $rendererProfile = [string]$json.renderer_profile
        $npsScope = [string]$json.nps_scope
        $expectedValidity = [string]$json.expected_validity

        [PSCustomObject]@{
            ResPath = Convert-LocalPathToResPath $_.FullName
            LocalPath = $_.FullName
            Name = Get-BaseNameNoExt $_.FullName
            RendererProfile = $rendererProfile
            NpsScope = $npsScope
            ExpectedValidity = $expectedValidity
        }
    })

    return $fixtures
}

function Test-ExecutableAvailable {
    param([Parameter(Mandatory)][string]$ExePath)

    if ([System.IO.Path]::IsPathRooted($ExePath)) {
        return (Test-Path $ExePath)
    }

    return $null -ne (Get-Command $ExePath -ErrorAction SilentlyContinue)
}

function Test-ResPathExists {
    param([Parameter(Mandatory)][string]$ResPath)

    $localPath = Convert-ResPathToLocalPath $ResPath
    return (Test-Path $localPath)
}

function Assert-SuitePreflight {
    param([Parameter(Mandatory)][array]$HarnessFixtureMetadata)

    if (-not (Test-ExecutableAvailable $GodotExe)) {
        throw "Godot executable not found: $GodotExe"
    }

    if (-not (Test-Path $ProjectPath)) {
        throw "ProjectPath not found: $ProjectPath"
    }

    $badRendererFixtures = @($HarnessFixtureMetadata | Where-Object {
        [string]::IsNullOrWhiteSpace($_.RendererProfile) -or
        $_.RendererProfile -eq "ambiguous" -or
        $_.RendererProfile -notin @("any", "gpu", "compatibility")
    })

    if ($badRendererFixtures.Count -gt 0) {
        Write-Host "Invalid or unresolved renderer_profile values:" -ForegroundColor Red
        $badRendererFixtures | Format-Table ResPath, RendererProfile, NpsScope, ExpectedValidity -AutoSize
        throw "StatusPanel fixture metadata preflight failed."
    }

    $explicitResPaths = @()
    $explicitResPaths += $HeadlessScripts
    $explicitResPaths += $RenderFixtures
    foreach ($sceneRun in $SceneRuns) {
        $explicitResPaths += [string]$sceneRun.Scene
    }

    $missingPaths = @($explicitResPaths | Where-Object { -not (Test-ResPathExists $_) })
    if ($missingPaths.Count -gt 0) {
        Write-Host "Missing explicit suite paths:" -ForegroundColor Red
        $missingPaths | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        throw "Suite path preflight failed."
    }

    $staleNames = @(
        "fixture_live_frameproducer_under_stream",
        "fixture_review_orphan_frameproducer_active"
    )

    $staleExplicitRefs = @($explicitResPaths | Where-Object {
        $path = $_
        $found = $false
        foreach ($stale in $staleNames) {
            if ($path -match [regex]::Escape($stale)) {
                $found = $true
            }
        }
        $found
    })

    if ($staleExplicitRefs.Count -gt 0) {
        Write-Host "Stale fixture references in explicit suite lists:" -ForegroundColor Red
        $staleExplicitRefs | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        throw "Stale fixture reference preflight failed."
    }
}

function Test-StatusPanelHarnessSuccess {
    param(
        [Parameter(Mandatory)][int]$ExitCode,
        [Parameter(Mandatory)][string]$StepName,
        [string]$StdoutText = "",
        [string]$StderrText = "",
        [string]$CombinedText = ""
    )

    $okMarker = (
        $CombinedText -match "OK: valid fixture behaved as expected" -or
        $CombinedText -match "OK: expected-invalid fixture was rejected/handled as expected"
    )

    $hardFailure = (
        $CombinedText -match "valid fixture failed" -or
        $CombinedText -match "expected-invalid fixture was not rejected" -or
        $CombinedText -match "fixture expectation failed" -or
        $CombinedText -match "failed schema validation" -or
        $CombinedText -match "Parse Error"
    )

    return ($okMarker -and -not $hardFailure)
}


function Test-GodotSceneSuccess {
    param(
        [Parameter(Mandatory)][int]$ExitCode,
        [Parameter(Mandatory)][string]$StepName,
        [string]$StdoutText = "",
        [string]$StderrText = "",
        [string]$CombinedText = ""
    )

    # Scene verifiers are authoritative by explicit OK markers. Some scenes
    # intentionally emit Godot ERROR lines while proving rejection behaviour
    # (for example public-boundary validation), so ERROR alone is review-worthy
    # but not a failure when the scene PASS marker is present.
    $sceneOk = (
        $CombinedText -match "OK:\s+godot .* PASS" -or
        $CombinedText -match "OK:\s+result_retrieval_verification passed"
    )

    $hardFailure = (
        $CombinedText -match "SCRIPT ERROR:" -or
        $CombinedText -match "Parse Error" -or
        $CombinedText -match "Failed to load script" -or
        $CombinedText -match "\bFAIL\b" -or
        $CombinedText -match "FAILED"
    )

    return ($sceneOk -and -not $hardFailure)
}


function Test-GodotHeadlessScriptSuccess {
    param(
        [Parameter(Mandatory)][int]$ExitCode,
        [Parameter(Mandatory)][string]$StepName,
        [string]$StdoutText = "",
        [string]$StderrText = "",
        [string]$CombinedText = ""
    )

    # Standalone headless scripts are authoritative by their explicit OK markers.
    # Godot console builds may leave ExitCode blank/unknown in this runner, so do
    # not use exit code alone for these script verification cases.
    $scriptOk = (
        $CombinedText -match "OK:\s+status panel provider-only transition harness PASS" -or
        $CombinedText -match "OK:\s+status panel retained lifecycle reconciliation PASS"
    )

    $hardFailure = (
        $CombinedText -match "SCRIPT ERROR:" -or
        $CombinedText -match "Parse Error" -or
        $CombinedText -match "Failed to load script" -or
        $CombinedText -match "\bFAIL\b" -or
        $CombinedText -match "FAILED"
    )

    return ($scriptOk -and -not $hardFailure)
}

function Invoke-GodotStep {
    param(
        [Parameter(Mandatory)][string]$StepName,
        [Parameter(Mandatory)][string[]]$Args,
        [string]$Category = "",
        [string]$RendererProfile = "",
        [ScriptBlock]$SuccessCheck
    )

    $startTime = Get-Date
    $commandText = "$GodotExe $($Args -join ' ')"

    Write-Host ("`n=== {0}: {1} ===" -f $Category, $StepName) -ForegroundColor Cyan
    if ($RendererProfile) {
        Write-Host ("RendererProfile: {0}" -f $RendererProfile) -ForegroundColor DarkCyan
    }
    Write-Host ("Command: {0}" -f $commandText) -ForegroundColor DarkGray

    Write-LogLine
    Write-LogLine ("=" * 100)
    Write-LogLine "START:    $StepName"
    if ($Category) {
        Write-LogLine "CATEGORY: $Category"
    }
    if ($RendererProfile) {
        Write-LogLine "RENDERER: $RendererProfile"
    }
    Write-LogLine ("TIME:     {0}" -f $startTime.ToString("yyyy-MM-dd HH:mm:ss"))
    Write-LogLine "COMMAND:  $commandText"
    Write-LogLine ("-" * 100)

    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    $reviewLog = $false
    $stdoutText = ""
    $stderrText = ""
    $combinedText = ""
    $exitCode = -9999
    $timedOut = $false

    try {
        $workingDirectory = (Resolve-Path $ProjectPath).Path

        $proc = Start-Process `
            -FilePath $GodotExe `
            -ArgumentList $Args `
            -WorkingDirectory $workingDirectory `
            -NoNewWindow `
            -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath

        $exited = $proc.WaitForExit($StepTimeoutSec * 1000)
        if (-not $exited) {
            $timedOut = $true
            $reviewLog = $true
            Write-Host ("TIMEOUT after {0}s: {1}" -f $StepTimeoutSec, $StepName) -ForegroundColor Red
            try {
                $proc.Kill($true)
            }
            catch {
                try { $proc.Kill() } catch { }
            }
            $exitCode = -7777
        }
        else {
            $proc.Refresh()
            $exitCode = $proc.ExitCode
        }

        if (Test-Path $stdoutPath) {
            $stdoutText = Get-Content $stdoutPath -Raw -ErrorAction SilentlyContinue
            if (-not [string]::IsNullOrEmpty($stdoutText)) {
                Add-Content -Path $LogPath -Value $stdoutText
            }
        }

        if (Test-Path $stderrPath) {
            $stderrText = Get-Content $stderrPath -Raw -ErrorAction SilentlyContinue
            if (-not [string]::IsNullOrEmpty($stderrText)) {
                Add-Content -Path $LogPath -Value $stderrText
            }
        }

        $combinedText = "$stdoutText`n$stderrText"

        if ($timedOut -or $stderrText -match "ERROR:" -or $combinedText -match "failed schema validation" -or $combinedText -match "Parse Error") {
            $reviewLog = $true
        }
    }
    finally {
        Remove-Item $stdoutPath -ErrorAction Ignore
        Remove-Item $stderrPath -ErrorAction Ignore
    }

    $endTime = Get-Date
    $durationSec = [math]::Round(($endTime - $startTime).TotalSeconds, 2)

    $passed = if ($timedOut) {
        $false
    }
    elseif ($null -ne $SuccessCheck) {
        & $SuccessCheck $exitCode $StepName $stdoutText $stderrText $combinedText
    }
    else {
        $exitCode -eq 0
    }

    $status = if ($passed) { "PASS" } else { "FAIL" }
    $reviewLogText = if ($reviewLog) { "YES" } else { "NO" }

    Write-LogLine ("-" * 100)
    Write-LogLine "END:        $StepName"
    Write-LogLine ("END TIME:   {0}" -f $endTime.ToString("yyyy-MM-dd HH:mm:ss"))
    Write-LogLine "EXIT:       $exitCode"
    Write-LogLine "TIMED OUT:  $timedOut"
    Write-LogLine "STATUS:     $status"
    Write-LogLine "REVIEW LOG: $reviewLogText"
    Write-LogLine "DURATION:   $durationSec sec"
    Write-LogLine ("=" * 100)

    $result = [PSCustomObject]@{
        Name            = $StepName
        Category        = $Category
        RendererProfile = $RendererProfile
        Status          = $status
        ReviewLog       = $reviewLogText
        ExitCode        = $exitCode
        TimedOut        = $timedOut
        DurationSec     = $durationSec
    }

    $script:Results.Add($result) | Out-Null
    return $result
}

function Get-SuccessCheckForStep {
    param([Parameter(Mandatory)][string]$StepName)

    if ($CustomSuccessChecks.ContainsKey($StepName)) {
        return $CustomSuccessChecks[$StepName]
    }

    return $null
}

# ============================================================
# DISCOVER + PREFLIGHT
# ============================================================

$HarnessFixtures = @(Get-StatusPanelFixtureMetadata)
Assert-SuitePreflight -HarnessFixtureMetadata $HarnessFixtures

Write-Host "StatusPanel harness fixture buckets:" -ForegroundColor Cyan
$HarnessFixtures |
    Group-Object RendererProfile |
    Sort-Object Name |
    ForEach-Object {
        [PSCustomObject]@{
            RendererProfile = $_.Name
            Count = $_.Count
        }
    } |
    Format-Table -AutoSize

# ============================================================
# RUN: metadata-discovered StatusPanel harness fixtures
# godot <renderer args> --script res://scripts/status_panel_harness.gd -- <fixture>
# ============================================================

foreach ($fixture in ($HarnessFixtures | Sort-Object RendererProfile, ResPath)) {
    $fixtureName = Get-BaseNameNoExt $fixture.ResPath
    $stepName = "harness:$fixtureName"
    $rendererArgs = @(Get-RendererArgsForProfile $fixture.RendererProfile)

    $args = @()
    $args += $rendererArgs
    $args += @(
        "--path", $ProjectPath,
        "--script", "res://scripts/status_panel_harness.gd",
        "--",
        $fixture.ResPath
    )

    Invoke-GodotStep `
        -StepName $stepName `
        -Category "HarnessFixture" `
        -RendererProfile $fixture.RendererProfile `
        -Args $args `
        -SuccessCheck ${function:Test-StatusPanelHarnessSuccess} | Out-Null
}

# ============================================================
# RUN: headless standalone scripts
# godot --headless --path . --script <gd>
# ============================================================

foreach ($scriptPath in $HeadlessScripts) {
    $scriptName = Get-BaseNameNoExt $scriptPath
    $stepName = "script:$scriptName"

    $args = @(
        "--headless",
        "--path", $ProjectPath,
        "--script", $scriptPath
    )

    $successCheck = Get-SuccessCheckForStep $stepName
    if ($null -eq $successCheck) {
        $successCheck = ${function:Test-GodotHeadlessScriptSuccess}
    }
    Invoke-GodotStep -StepName $stepName -Category "HeadlessScript" -Args $args -SuccessCheck $successCheck | Out-Null
}

# ============================================================
# RUN: render fixtures
# godot --script res://scripts/status_panel_harness.gd -- <fixture> <png> --window-width ... --window-height ...
# ============================================================

foreach ($fixture in $RenderFixtures) {
    $fixtureName = Get-BaseNameNoExt $fixture
    $stepName = "render:$fixtureName"
    $outputPath = Get-RenderOutputPath -FixturePath $fixture -ArtifactDirPath $ArtifactDir

    $args = @(
        "--path", $ProjectPath,
        "--script", "res://scripts/status_panel_harness.gd",
        "--",
        $fixture,
        $outputPath,
        "--window-width", $RenderWidth.ToString(),
        "--window-height", $RenderHeight.ToString()
    )

    $successCheck = Get-SuccessCheckForStep $stepName
    if ($null -eq $successCheck) {
        $successCheck = ${function:Test-StatusPanelHarnessSuccess}
    }
    Invoke-GodotStep -StepName $stepName -Category "RenderFixture" -Args $args -SuccessCheck $successCheck | Out-Null
}

# ============================================================
# RUN: scenes
# godot --headless --path . --scene <scene> --quit-after <n>
# ============================================================

foreach ($sceneRun in $SceneRuns) {
    $scenePath = [string]$sceneRun.Scene
    $quitAfter = [int]$sceneRun.QuitAfter
    $sceneName = Get-BaseNameNoExt $scenePath
    $stepName = "scene:$sceneName"

    $args = @(
        "--headless",
        "--path", $ProjectPath,
        "--scene", $scenePath,
        "--quit-after", $quitAfter.ToString()
    )

    $successCheck = Get-SuccessCheckForStep $stepName
    if ($null -eq $successCheck) {
        $successCheck = ${function:Test-GodotSceneSuccess}
    }
    Invoke-GodotStep -StepName $stepName -Category "Scene" -Args $args -SuccessCheck $successCheck | Out-Null
}

# ============================================================
# SUMMARY
# ============================================================

Write-Host
Write-Host "Run complete. Summary:" -ForegroundColor Cyan
$Results |
    Format-Table Category, RendererProfile, Name, Status, ReviewLog, ExitCode, TimedOut, DurationSec -AutoSize |
    Out-String -Width 4096 |
    Write-Host

Write-Host
Write-Host "Harness renderer summary:" -ForegroundColor Cyan
$Results |
    Where-Object { $_.Category -eq "HarnessFixture" } |
    Group-Object RendererProfile |
    Sort-Object Name |
    ForEach-Object {
        $group = @($_.Group)
        $passed = @($group | Where-Object { $_.Status -eq "PASS" })
        $failed = @($group | Where-Object { $_.Status -eq "FAIL" })
        $review = @($group | Where-Object { $_.ReviewLog -eq "YES" })
        $timeouts = @($group | Where-Object { $_.TimedOut })

        [PSCustomObject]@{
            RendererProfile = $_.Name
            Total = $group.Count
            Passed = $passed.Count
            Failed = $failed.Count
            Review = $review.Count
            Timeout = $timeouts.Count
        }
    } |
    Format-Table -AutoSize |
    Out-String -Width 4096 |
    Write-Host

Write-Host
Write-Host "Category summary:" -ForegroundColor Cyan
$Results |
    Group-Object Category |
    Sort-Object Name |
    ForEach-Object {
        $group = @($_.Group)
        $passed = @($group | Where-Object { $_.Status -eq "PASS" })
        $failed = @($group | Where-Object { $_.Status -eq "FAIL" })
        $review = @($group | Where-Object { $_.ReviewLog -eq "YES" })
        $timeouts = @($group | Where-Object { $_.TimedOut })

        [PSCustomObject]@{
            Category = $_.Name
            Total = $group.Count
            Passed = $passed.Count
            Failed = $failed.Count
            Review = $review.Count
            Timeout = $timeouts.Count
        }
    } |
    Format-Table -AutoSize |
    Out-String -Width 4096 |
    Write-Host

Write-Host
Write-Host "Full log: $LogPath" -ForegroundColor Yellow

$failedResults = @($Results | Where-Object { $_.Status -eq "FAIL" })
$passedResults = @($Results | Where-Object { $_.Status -eq "PASS" })
$reviewResults = @($Results | Where-Object { $_.ReviewLog -eq "YES" })

$passedCount = $passedResults.Count
$failedCount = $failedResults.Count
$reviewCount = $reviewResults.Count

Write-Host
Write-Host "Passed: $passedCount" -ForegroundColor Green
Write-Host "Failed: $failedCount" -ForegroundColor $(if ($failedCount -gt 0) { "Red" } else { "Green" })
Write-Host "Review: $reviewCount" -ForegroundColor Yellow

if ($failedCount -gt 0) {
    exit 1
}
else {
    exit 0
}
