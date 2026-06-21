param(
    [string]$GodotExe = "C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe",
    [string]$ProjectPath = $PSScriptRoot,
    [string]$Scene = "",
    [string]$Script = "",
    [int]$QuitAfter = 0,
    [switch]$Windowed,
    [string[]]$ScriptArgs = @(),
    [string[]]$ExtraArgs = @(),
    [switch]$CaptureLogs,
    [string]$LogRoot = "",
    [string]$RunLabel = "",
    [string]$ExpectedOkPattern = "",
    [string[]]$HardFailurePatterns = @(
        "SCRIPT ERROR:",
        "Parse Error",
        "Failed to load script",
        "(?im)^\s*FAIL(?:ED)?\b"
    ),
    [int]$TimeoutSec = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RunLeafName {
    param(
        [string]$ScenePath,
        [string]$ScriptPath
    )

    $sourcePath = if (-not [string]::IsNullOrWhiteSpace($ScenePath)) {
        $ScenePath
    }
    else {
        $ScriptPath
    }

    if ([string]::IsNullOrWhiteSpace($sourcePath)) {
        return "run"
    }

    return [System.IO.Path]::GetFileNameWithoutExtension($sourcePath)
}

function Convert-ToSafeName {
    param([Parameter(Mandatory)][string]$Value)

    $safe = $Value -replace "[^A-Za-z0-9._-]", "_"
    $safe = $safe.Trim("_")
    if ([string]::IsNullOrWhiteSpace($safe)) {
        return "run"
    }

    return $safe
}

function Test-PatternMatch {
    param(
        [string]$Text,
        [string[]]$Patterns
    )

    if ([string]::IsNullOrEmpty($Text)) {
        return $false
    }

    foreach ($pattern in $Patterns) {
        if ([string]::IsNullOrWhiteSpace($pattern)) {
            continue
        }

        if ($Text -match $pattern) {
            return $true
        }
    }

    return $false
}

function Get-RunIdentity {
    param(
        [string]$ExplicitLabel,
        [string]$ScenePath,
        [string]$ScriptPath,
        [switch]$IsWindowed
    )

    $parts = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($ExplicitLabel)) {
        $parts.Add((Convert-ToSafeName $ExplicitLabel))
    }

    $parts.Add((Convert-ToSafeName (Get-RunLeafName -ScenePath $ScenePath -ScriptPath $ScriptPath)))
    $parts.Add($(if ($IsWindowed) { "windowed" } else { "headless" }))
    return $parts -join "__"
}

function New-LogRecord {
    param(
        [string]$LogsRoot,
        [string]$RunIdentity
    )

    $timestampUtc = [DateTime]::UtcNow
    $timestampToken = $timestampUtc.ToString("yyyyMMddTHHmmssfffZ")
    $root = if ([string]::IsNullOrWhiteSpace($LogsRoot)) {
        Join-Path $projectFullPath "run-logs"
    }
    else {
        if ([System.IO.Path]::IsPathRooted($LogsRoot)) {
            $LogsRoot
        }
        else {
            Join-Path $projectFullPath $LogsRoot
        }
    }

    $pendingRoot = Join-Path $root "_pending"
    $runDirName = "$timestampToken`__$RunIdentity"
    $runDir = Join-Path $pendingRoot $runDirName
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    return [PSCustomObject]@{
        Root = $root
        PendingRoot = $pendingRoot
        RunDirName = $runDirName
        RunDir = $runDir
        TimestampUtc = $timestampUtc
        TimestampToken = $timestampToken
        StdoutPath = Join-Path $runDir "stdout.log"
        StderrPath = Join-Path $runDir "stderr.log"
        VerdictPath = Join-Path $runDir "verdict.txt"
        MetaPath = Join-Path $runDir "meta.json"
        SummaryPath = Join-Path $root "summary.jsonl"
    }
}

function Finalize-LogRecord {
    param(
        [Parameter(Mandatory)][object]$LogRecord,
        [Parameter(Mandatory)][ValidateSet("ok", "error")][string]$Bucket
    )

    $bucketRoot = Join-Path $LogRecord.Root $Bucket
    New-Item -ItemType Directory -Force -Path $bucketRoot | Out-Null

    $finalRunDir = Join-Path $bucketRoot $LogRecord.RunDirName
    if (Test-Path $finalRunDir) {
        Remove-Item -LiteralPath $finalRunDir -Recurse -Force
    }

    $lastMoveError = $null
    for ($attempt = 1; $attempt -le 20; $attempt++) {
        try {
            Move-Item -LiteralPath $LogRecord.RunDir -Destination $finalRunDir
            $lastMoveError = $null
            break
        }
        catch {
            $lastMoveError = $_
            if (Test-Path $finalRunDir) {
                Remove-Item -LiteralPath $finalRunDir -Recurse -Force -ErrorAction SilentlyContinue
            }
            Start-Sleep -Milliseconds 200
        }
    }

    if ($null -ne $lastMoveError) {
        throw $lastMoveError
    }

    $LogRecord.RunDir = $finalRunDir
    $LogRecord.StdoutPath = Join-Path $finalRunDir "stdout.log"
    $LogRecord.StderrPath = Join-Path $finalRunDir "stderr.log"
    $LogRecord.VerdictPath = Join-Path $finalRunDir "verdict.txt"
    $LogRecord.MetaPath = Join-Path $finalRunDir "meta.json"
    return $LogRecord
}

if (-not (Test-Path $GodotExe)) {
    throw "Godot executable not found: $GodotExe"
}

$projectFullPath = (Resolve-Path $ProjectPath).Path
$arguments = New-Object System.Collections.Generic.List[string]

if ([string]::IsNullOrWhiteSpace($Scene) -eq [string]::IsNullOrWhiteSpace($Script)) {
    throw "Specify exactly one of -Scene or -Script."
}

if ($TimeoutSec -lt 0) {
    throw "TimeoutSec must be zero or greater."
}

if (-not $Windowed) {
    $arguments.Add("--headless")
}

$arguments.Add("--path")
$arguments.Add($projectFullPath)

if (-not [string]::IsNullOrWhiteSpace($Scene)) {
    $arguments.Add("--scene")
    $arguments.Add($Scene)
}
else {
    $arguments.Add("--script")
    $arguments.Add($Script)
    if ($ScriptArgs.Count -gt 0) {
        $arguments.Add("--")
        foreach ($arg in $ScriptArgs) {
            $arguments.Add($arg)
        }
    }
}

if ($QuitAfter -gt 0) {
    $arguments.Add("--quit-after")
    $arguments.Add($QuitAfter.ToString())
}

foreach ($arg in $ExtraArgs) {
    $arguments.Add($arg)
}

$commandText = "{0} {1}" -f $GodotExe, ($arguments -join " ")
$runIdentity = Get-RunIdentity -ExplicitLabel $RunLabel -ScenePath $Scene -ScriptPath $Script -IsWindowed:$Windowed

if (-not $CaptureLogs) {
    Write-Host ("RUN: {0}" -f $commandText)
    & $GodotExe @arguments
    exit $LASTEXITCODE
}

$logRecord = New-LogRecord -LogsRoot $LogRoot -RunIdentity $runIdentity
$startTime = Get-Date
$timedOut = $false
$processExitCode = $null

Write-Host ("RUN: {0}" -f $commandText)
Write-Host ("LOG ROOT: {0}" -f $logRecord.Root)
Write-Host ("LOG DIR:  {0}" -f $logRecord.RunDir)

try {
    $proc = Start-Process `
        -FilePath $GodotExe `
        -ArgumentList $arguments `
        -WorkingDirectory $projectFullPath `
        -NoNewWindow `
        -PassThru `
        -RedirectStandardOutput $logRecord.StdoutPath `
        -RedirectStandardError $logRecord.StderrPath

    if ($TimeoutSec -gt 0) {
        $exited = $proc.WaitForExit($TimeoutSec * 1000)
        if (-not $exited) {
            $timedOut = $true
            try {
                $proc.Kill($true)
            }
            catch {
                try { $proc.Kill() } catch { }
            }
            $processExitCode = -7777
        }
        else {
            $proc.Refresh()
            $processExitCode = $proc.ExitCode
        }
    }
    else {
        $proc.WaitForExit()
        $proc.Refresh()
        $processExitCode = $proc.ExitCode
    }
}
finally {
    if (-not (Test-Path $logRecord.StdoutPath)) {
        New-Item -ItemType File -Path $logRecord.StdoutPath | Out-Null
    }
    if (-not (Test-Path $logRecord.StderrPath)) {
        New-Item -ItemType File -Path $logRecord.StderrPath | Out-Null
    }
}

$endTime = Get-Date
$durationSec = [math]::Round(($endTime - $startTime).TotalSeconds, 2)
$stdoutText = Get-Content $logRecord.StdoutPath -Raw -ErrorAction SilentlyContinue
$stderrText = Get-Content $logRecord.StderrPath -Raw -ErrorAction SilentlyContinue
$combinedText = "$stdoutText`n$stderrText"

if ($null -eq $processExitCode -or [string]::IsNullOrWhiteSpace([string]$processExitCode)) {
    $processExitCode = $null
}

$verdictReasons = New-Object System.Collections.Generic.List[string]
if ($timedOut) {
    $verdictReasons.Add("timeout")
}

if ($null -ne $processExitCode -and $processExitCode -ne 0) {
    $verdictReasons.Add("exit_code=$processExitCode")
}

if (Test-PatternMatch -Text $combinedText -Patterns $HardFailurePatterns) {
    $verdictReasons.Add("hard_failure_pattern")
}

if (-not [string]::IsNullOrWhiteSpace($ExpectedOkPattern) -and -not ($combinedText -match $ExpectedOkPattern)) {
    $verdictReasons.Add("missing_expected_ok_pattern")
}

$bucket = if ($verdictReasons.Count -eq 0) { "ok" } else { "error" }
$finalVerdict = if ($bucket -eq "ok") { "OK" } else { "ERROR" }
$returnedExitCode = if ($bucket -eq "ok") {
    if ($null -eq $processExitCode) { 0 } else { $processExitCode }
}
elseif ($null -ne $processExitCode -and $processExitCode -ne 0) {
    $processExitCode
}
else {
    1
}
$logRecord = Finalize-LogRecord -LogRecord $logRecord -Bucket $bucket

$meta = [ordered]@{
    timestamp_utc = $logRecord.TimestampUtc.ToString("o")
    run_identity = $runIdentity
    run_label = $RunLabel
    verdict = $finalVerdict
    verdict_reasons = @($verdictReasons)
    process_exit_code = $processExitCode
    returned_exit_code = $returnedExitCode
    timed_out = $timedOut
    duration_sec = $durationSec
    windowed = [bool]$Windowed
    scene = $Scene
    script = $Script
    script_args = @($ScriptArgs)
    extra_args = @($ExtraArgs)
    quit_after = $QuitAfter
    expected_ok_pattern = $ExpectedOkPattern
    hard_failure_patterns = @($HardFailurePatterns)
    command = $commandText
    project_path = $projectFullPath
    stdout_log = $logRecord.StdoutPath
    stderr_log = $logRecord.StderrPath
    run_dir = $logRecord.RunDir
}

Set-Content -Path $logRecord.VerdictPath -Value $finalVerdict -NoNewline
$meta | ConvertTo-Json -Depth 6 | Set-Content -Path $logRecord.MetaPath
($meta | ConvertTo-Json -Depth 6 -Compress) | Add-Content -Path $logRecord.SummaryPath

Write-Host ("VERDICT:  {0}" -f $finalVerdict) -ForegroundColor $(if ($bucket -eq "ok") { "Green" } else { "Red" })
Write-Host ("EXIT:     {0}" -f $returnedExitCode)
Write-Host ("DURATION: {0} sec" -f $durationSec)
Write-Host ("RUN DIR:  {0}" -f $logRecord.RunDir)
Write-Host ("SUMMARY:  {0}" -f $logRecord.SummaryPath)

if ($bucket -eq "error") {
    Write-Host ("REASONS:  {0}" -f ($verdictReasons -join ", ")) -ForegroundColor Yellow
}

exit $returnedExitCode
