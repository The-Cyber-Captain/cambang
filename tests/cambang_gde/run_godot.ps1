param(
    [string]$GodotExe = "C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe",
    [string]$ProjectPath = $PSScriptRoot,
    [ValidateSet("windows", "android")][string]$RunPlatform = "windows",
    [string]$Scene = "",
    [string]$Script = "",
    [int]$QuitAfter = 0,
    [switch]$Windowed,
    [string[]]$ScriptArgs = @(),
    [string[]]$ExtraArgs = @(),
    [switch]$CaptureLogs,
    [string]$LogRoot = "",
    [string]$RunLabel = "",
    [string[]]$HardFailurePatterns = @(
    "SCRIPT ERROR:",
    "Parse Error",
    "Failed to load script",
    "(?im)^\s*FAIL(?:ED)?\b"
),
    [int]$TimeoutSec = 0,
    [string]$AndroidSdkRoot = "",
    [string]$AdbExe = "",
    [string]$AndroidExportPreset = "Android",
    [string]$AndroidDeviceSerial = "",
    [string]$AndroidPackage = "",
    [string]$AndroidActivity = ""
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

function ConvertFrom-HarnessVerdictMarkers {
    param([string]$Text)

    $markers = New-Object System.Collections.Generic.List[object]
    if ([string]::IsNullOrWhiteSpace($Text)) {
        return @($markers.ToArray())
    }

    $pattern = '\[CamBANG\]\[HarnessVerdict\]\s+scene=(?<scene>\S+)\s+status=(?<status>ok|expected_unsupported|fail|error)\s+exit_code=(?<exit_code>-?\d+)(?:\s+reason=(?<reason>\S+))?'
    $lineNumber = 0
    foreach ($line in ($Text -split "\r?\n")) {
        $lineNumber += 1
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $match = [Regex]::Match($line, $pattern)
        if (-not $match.Success) {
            continue
        }

        $markers.Add([PSCustomObject][ordered]@{
            scene = $match.Groups["scene"].Value
            status = $match.Groups["status"].Value
            exit_code = [int]$match.Groups["exit_code"].Value
            reason = $match.Groups["reason"].Value
            line = $lineNumber
            raw = $line
        })
    }

    return @($markers.ToArray())
}

function Get-LastHarnessVerdict {
    param([string]$Text)

    $markers = @(ConvertFrom-HarnessVerdictMarkers -Text $Text)
    if ($markers.Count -eq 0) {
        return $null
    }

    return $markers[$markers.Count - 1]
}

function Get-WindowsGodotExtraArgBuckets {
    param([string[]]$ExtraArgValues)

    $userArgPrefixes = @(
        "--cambang-synth-producer-output-form=",
        "--cambang-synth-stream-capability-downgrades=",
        "--cambang-synth-capture-capability-downgrades=",
        "--cambang-bench-provider=",
        "--cambang-bench-seed=",
        "--cambang-bench-human-phase-sec=",
        "--cambang-bench-superhuman-phase-sec=",
        "--cambang-bench-rig-phase-sec=",
        "--cambang-bench-warmup-sec=",
        "--cambang-bench-superhuman-actions-per-tick=",
        "--cambang-bench-max-inflight-captures=",
        "--cambang-bench-headless-texture=",
        "--cambang-harness-rendering-method="
    )
    $userArgFlags = @(
        "--cambang-synth-producer-output-form",
        "--cambang-synth-stream-capability-downgrades",
        "--cambang-synth-capture-capability-downgrades",
        "--cambang-bench-provider",
        "--cambang-bench-seed",
        "--cambang-bench-human-phase-sec",
        "--cambang-bench-superhuman-phase-sec",
        "--cambang-bench-rig-phase-sec",
        "--cambang-bench-warmup-sec",
        "--cambang-bench-superhuman-actions-per-tick",
        "--cambang-bench-max-inflight-captures",
        "--cambang-bench-headless-texture",
        "--cambang-harness-rendering-method"
    )
    $engineArgValuePrefixes = @(
        "--rendering-method=",
        "--rendering-driver="
    )
    $engineArgFlagsWithValue = @(
        "--rendering-method",
        "--rendering-driver"
    )

    $engineArgs = New-Object System.Collections.Generic.List[string]
    $userArgs = New-Object System.Collections.Generic.List[string]
    $requestedRenderingMethod = ""

    for ($index = 0; $index -lt $ExtraArgValues.Count; $index++) {
        $arg = $ExtraArgValues[$index]
        if ([string]::IsNullOrWhiteSpace($arg) -or $arg -eq "--") {
            continue
        }

        $matchedPrefix = $false
        foreach ($prefix in $userArgPrefixes) {
            if ($arg.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
                $userArgs.Add($arg)
                $matchedPrefix = $true
                break
            }
        }
        if ($matchedPrefix) {
            continue
        }

        if ($userArgFlags -contains $arg) {
            if ($index + 1 -ge $ExtraArgValues.Count) {
                throw "Expected value after maintainer override arg '$arg'."
            }
            $index++
            $userArgs.Add(("{0}={1}" -f $arg, $ExtraArgValues[$index]))
            continue
        }

        if ($engineArgFlagsWithValue -contains $arg) {
            if ($index + 1 -ge $ExtraArgValues.Count) {
                throw "Expected value after engine arg '$arg'."
            }
            $index++
            $value = $ExtraArgValues[$index]
            if ($arg -eq "--rendering-method") {
                $value = Normalize-RenderingMethodValue -RenderingMethod $value
                $requestedRenderingMethod = $value
            }
            $engineArgs.Add($arg)
            $engineArgs.Add($value)
            continue
        }

        $matchedEngineValuePrefix = $false
        foreach ($prefix in $engineArgValuePrefixes) {
            if ($arg.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
                $value = $arg.Substring($prefix.Length)
                if ($prefix -eq "--rendering-method=") {
                    $value = Normalize-RenderingMethodValue -RenderingMethod $value
                    $requestedRenderingMethod = $value
                }
                $engineArgs.Add($prefix.Substring(0, $prefix.Length - 1))
                $engineArgs.Add($value)
                $matchedEngineValuePrefix = $true
                break
            }
        }
        if ($matchedEngineValuePrefix) {
            continue
        }

        $engineArgs.Add($arg)
    }

    if (-not [string]::IsNullOrWhiteSpace($requestedRenderingMethod)) {
        $userArgs.Add(("--cambang-harness-rendering-method={0}" -f $requestedRenderingMethod))
    }

    return [PSCustomObject]@{
        EngineArgs = @($engineArgs.ToArray())
        UserArgs = @($userArgs.ToArray())
    }
}

function Get-RunIdentity {
    param(
        [string]$ExplicitLabel,
        [string]$ScenePath,
        [string]$ScriptPath,
        [string]$Platform,
        [switch]$IsWindowed
    )

    $parts = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($ExplicitLabel)) {
        $parts.Add((Convert-ToSafeName $ExplicitLabel))
    }

    $parts.Add((Convert-ToSafeName (Get-RunLeafName -ScenePath $ScenePath -ScriptPath $ScriptPath)))
    if ($Platform -eq "android") {
        $parts.Add("android")
    }
    else {
        $parts.Add($(if ($IsWindowed) { "windowed" } else { "headless" }))
    }
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
        [Parameter(Mandatory)][ValidateSet("ok", "error", "expected_unsupported")][string]$Bucket
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

function Ensure-FileExists {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType File -Force -Path $Path | Out-Null
    }
}

function Add-LogText {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$Text
    )

    Ensure-FileExists -Path $Path
    if ($null -eq $Text) {
        return
    }

    Add-Content -Path $Path -Value $Text
}

function Add-LogSection {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Header,
        [string]$Body
    )

    $section = "===== $Header =====`r`n"
    if (-not [string]::IsNullOrEmpty($Body)) {
        $section += $Body
        if (-not $Body.EndsWith("`n") -and -not $Body.EndsWith("`r")) {
            $section += "`r`n"
        }
    }
    $section += "`r`n"
    Add-LogText -Path $Path -Text $section
}

function ConvertTo-StructuredRecordPath {
    param(
        [Parameter(Mandatory)][string]$RecordsDir,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Kind,
        [Parameter(Mandatory)][bool]$Partial
    )

    $safeName = Convert-ToSafeName -Value $Name
    $safeId = Convert-ToSafeName -Value $Id
    $baseName = "{0}__{1}" -f $safeName, $safeId
    $extension = if ($Kind -eq "json") { ".json" } else { ".txt" }
    if ($Partial) {
        return Join-Path $RecordsDir ($baseName + ".partial" + $extension)
    }

    return Join-Path $RecordsDir ($baseName + $extension)
}

function ConvertFrom-StructuredRecordLog {
    param(
        [Parameter(Mandatory)][string]$LogText,
        [Parameter(Mandatory)][string]$SourceLog
    )

    $recordTable = @{}
    if ([string]::IsNullOrWhiteSpace($LogText)) {
        return $recordTable
    }

    $startPattern = '\[CamBANG\]\[RecordStart\] id=(?<id>\S+) name=(?<name>\S+) kind=(?<kind>\S+) chunks=(?<chunks>\d+) encoding=base64(?:\s*)$'
    $chunkPattern = '\[CamBANG\]\[RecordChunk\] id=(?<id>\S+) index=(?<index>\d+) data=(?<data>.*)$'
    $endPattern = '\[CamBANG\]\[RecordEnd\] id=(?<id>\S+)(?:\s*)$'

    foreach ($line in ($LogText -split "\r?\n")) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $startMatch = [Regex]::Match($line, $startPattern)
        if ($startMatch.Success) {
            $recordId = $startMatch.Groups["id"].Value
            if (-not $recordTable.ContainsKey($recordId)) {
                $recordTable[$recordId] = [ordered]@{
                    id = $recordId
                    name = $startMatch.Groups["name"].Value
                    kind = $startMatch.Groups["kind"].Value
                    source_log = $SourceLog
                    recovered_path = ""
                    complete = $false
                    chunk_count_seen = 0
                    chunk_count_expected = [int]$startMatch.Groups["chunks"].Value
                    end_seen = $false
                    chunks = @{}
                }
            }
            continue
        }

        $chunkMatch = [Regex]::Match($line, $chunkPattern)
        if ($chunkMatch.Success) {
            $recordId = $chunkMatch.Groups["id"].Value
            if (-not $recordTable.ContainsKey($recordId)) {
                $recordTable[$recordId] = [ordered]@{
                    id = $recordId
                    name = ""
                    kind = "json"
                    source_log = $SourceLog
                    recovered_path = ""
                    complete = $false
                    chunk_count_seen = 0
                    chunk_count_expected = $null
                    end_seen = $false
                    chunks = @{}
                }
            }
            $record = $recordTable[$recordId]
            $chunkIndex = [int]$chunkMatch.Groups["index"].Value
            if (-not $record.chunks.ContainsKey($chunkIndex)) {
                $record.chunks[$chunkIndex] = $chunkMatch.Groups["data"].Value
                $record.chunk_count_seen = $record.chunks.Count
            }
            $recordTable[$recordId] = $record
            continue
        }

        $endMatch = [Regex]::Match($line, $endPattern)
        if ($endMatch.Success) {
            $recordId = $endMatch.Groups["id"].Value
            if (-not $recordTable.ContainsKey($recordId)) {
                $recordTable[$recordId] = [ordered]@{
                    id = $recordId
                    name = ""
                    kind = "json"
                    source_log = $SourceLog
                    recovered_path = ""
                    complete = $false
                    chunk_count_seen = 0
                    chunk_count_expected = $null
                    end_seen = $true
                    chunks = @{}
                }
            }
            else {
                $record = $recordTable[$recordId]
                $record.end_seen = $true
                $recordTable[$recordId] = $record
            }
        }
    }

    return $recordTable
}

function Get-StructuredRecordRecoveryText {
    param([Parameter(Mandatory)][object]$Record)

    $orderedIndexes = @($Record.chunks.Keys | Sort-Object)
    if ($orderedIndexes.Count -eq 0) {
        return ""
    }

    $maxContiguousIndex = -1
    for ($index = 0; $index -lt $orderedIndexes.Count; $index++) {
        if ([int]$orderedIndexes[$index] -ne $index) {
            break
        }
        $maxContiguousIndex = $index
    }

    if ($maxContiguousIndex -lt 0) {
        return ""
    }

    $builder = New-Object System.Text.StringBuilder
    for ($index = 0; $index -le $maxContiguousIndex; $index++) {
        $null = $builder.Append([string]$Record.chunks[$index])
    }

    $base64Text = $builder.ToString()
    if ([string]::IsNullOrWhiteSpace($base64Text)) {
        return ""
    }

    $remainder = $base64Text.Length % 4
    if ($remainder -ne 0) {
        $base64Text = $base64Text + ("=" * (4 - $remainder))
    }

    try {
        $bytes = [Convert]::FromBase64String($base64Text)
        return [System.Text.Encoding]::UTF8.GetString($bytes)
    }
    catch {
        return ""
    }
}

function Recover-StructuredRecords {
    param(
        [Parameter(Mandatory)][string]$RunDir,
        [Parameter(Mandatory)][hashtable]$SourceLogs
    )

    $recordsDir = Join-Path $RunDir "records"
    New-Item -ItemType Directory -Force -Path $recordsDir | Out-Null

    $recordsById = @{}
    foreach ($sourceLogPath in $SourceLogs.Keys) {
        if ([string]::IsNullOrWhiteSpace([string]$sourceLogPath) -or -not (Test-Path $sourceLogPath)) {
            continue
        }

        $sourceLogText = Get-Content -LiteralPath $sourceLogPath -Raw -ErrorAction SilentlyContinue
        if ([string]::IsNullOrWhiteSpace($sourceLogText)) {
            continue
        }

        $parsedRecords = ConvertFrom-StructuredRecordLog -LogText $sourceLogText -SourceLog $sourceLogPath
        foreach ($recordId in $parsedRecords.Keys) {
            $parsedRecord = $parsedRecords[$recordId]
            if (-not $recordsById.ContainsKey($recordId)) {
                $recordsById[$recordId] = $parsedRecord
                continue
            }

            $existingRecord = $recordsById[$recordId]
            if ([string]::IsNullOrWhiteSpace([string]$existingRecord.name) -and -not [string]::IsNullOrWhiteSpace([string]$parsedRecord.name)) {
                $existingRecord.name = $parsedRecord.name
            }
            if ([string]::IsNullOrWhiteSpace([string]$existingRecord.kind) -and -not [string]::IsNullOrWhiteSpace([string]$parsedRecord.kind)) {
                $existingRecord.kind = $parsedRecord.kind
            }
            if ([string]::IsNullOrWhiteSpace([string]$existingRecord.source_log)) {
                $existingRecord.source_log = $parsedRecord.source_log
            }
            if ($existingRecord.chunk_count_expected -eq $null -and $parsedRecord.chunk_count_expected -ne $null) {
                $existingRecord.chunk_count_expected = $parsedRecord.chunk_count_expected
            }
            foreach ($chunkIndex in $parsedRecord.chunks.Keys) {
                if (-not $existingRecord.chunks.ContainsKey($chunkIndex)) {
                    $existingRecord.chunks[$chunkIndex] = $parsedRecord.chunks[$chunkIndex]
                }
            }
            $existingRecord.chunk_count_seen = $existingRecord.chunks.Count
            $existingRecord.end_seen = [bool]$existingRecord.end_seen -or [bool]$parsedRecord.end_seen
            $recordsById[$recordId] = $existingRecord
        }
    }

    $recoveredSummaries = New-Object System.Collections.Generic.List[object]
    foreach ($recordId in $recordsById.Keys) {
        $record = $recordsById[$recordId]
        if ([string]::IsNullOrWhiteSpace([string]$record.kind)) {
            $record.kind = "json"
        }
        if ([string]::IsNullOrWhiteSpace([string]$record.name)) {
            $record.name = [string]$record.id
        }

        $decodedText = Get-StructuredRecordRecoveryText -Record $record
        $complete = [bool]$record.end_seen
        if ($record.chunk_count_expected -ne $null) {
            $complete = $complete -and ($record.chunk_count_seen -ge [int]$record.chunk_count_expected)
        }
        if ([string]::IsNullOrWhiteSpace($decodedText)) {
            $complete = $false
        }

        $recoveredPath = ConvertTo-StructuredRecordPath -RecordsDir $recordsDir -Name $record.name -Id $record.id -Kind $record.kind -Partial (-not $complete)
        if (-not [string]::IsNullOrWhiteSpace($decodedText)) {
            Set-Content -LiteralPath $recoveredPath -Value $decodedText -NoNewline
        }
        else {
            Ensure-FileExists -Path $recoveredPath
        }

        $record.recovered_path = $recoveredPath
        $record.complete = $complete
        $recoveredSummaries.Add([ordered]@{
            id = $record.id
            name = $record.name
            kind = $record.kind
            source_log = $record.source_log
            recovered_path = $recoveredPath
            complete = $complete
            chunk_count_seen = $record.chunk_count_seen
            chunk_count_expected = $record.chunk_count_expected
        })
    }

    $scene70SummaryJson = $null
    $scene870SummaryJson = $null
    foreach ($summaryRecord in $recoveredSummaries) {
        if ($summaryRecord.name -eq "scene70_summary" -and $summaryRecord.kind -eq "json") {
            $scene70SummaryJson = [string]$summaryRecord.recovered_path
        }
        if ($summaryRecord.name -eq "scene870_to_image_soak_summary" -and $summaryRecord.kind -eq "json") {
            $scene870SummaryJson = [string]$summaryRecord.recovered_path
        }
    }

    return [ordered]@{
        StructuredRecords = @($recoveredSummaries.ToArray())
        Scene70SummaryJson = $scene70SummaryJson
        Scene870SummaryJson = $scene870SummaryJson
    }
}


function ConvertFrom-AndroidStructuredRecordFileMarkers {
    param([string]$LogText)

    $markers = New-Object System.Collections.Generic.List[object]
    if ([string]::IsNullOrWhiteSpace($LogText)) {
        return @($markers.ToArray())
    }

    $markerPattern = '\[CamBANG\]\[RecordFile\] id=(?<id>\S+) name=(?<name>\S+) kind=(?<kind>\S+) user_path=(?<user_path>\S+) bytes=(?<bytes>\d+)'
    $seen = @{}
    foreach ($line in ($LogText -split "\r?\n")) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $match = [Regex]::Match($line, $markerPattern)
        if (-not $match.Success) {
            continue
        }

        $recordId = $match.Groups["id"].Value
        $recordName = $match.Groups["name"].Value
        $recordKind = $match.Groups["kind"].Value
        $userPath = $match.Groups["user_path"].Value
        $dedupeKey = "{0}|{1}|{2}|{3}" -f $recordId, $recordName, $recordKind, $userPath
        if ($seen.ContainsKey($dedupeKey)) {
            continue
        }
        $seen[$dedupeKey] = $true

        $markers.Add([ordered]@{
            id = $recordId
            name = $recordName
            kind = $recordKind
            user_path = $userPath
            bytes = [int64]$match.Groups["bytes"].Value
        })
    }

    return @($markers.ToArray())
}

function Recover-AndroidStructuredRecordFiles {
    param(
        [Parameter(Mandatory)][string]$RunDir,
        [Parameter(Mandatory)][string]$LogText,
        [Parameter(Mandatory)][string]$AdbPath,
        [Parameter(Mandatory)][string]$DeviceSerial,
        [Parameter(Mandatory)][string]$PackageName,
        [Parameter(Mandatory)][string]$ProjectFullPath,
        [string]$StderrLogPath = ""
    )

    $recordsDir = Join-Path $RunDir "records"
    New-Item -ItemType Directory -Force -Path $recordsDir | Out-Null

    $recoveredSummaries = New-Object System.Collections.Generic.List[object]
    $scene870SummaryJson = $null

    if ([string]::IsNullOrWhiteSpace($PackageName)) {
        return [ordered]@{
            StructuredRecords = @($recoveredSummaries.ToArray())
            Scene870SummaryJson = $scene870SummaryJson
        }
    }

    $markers = @(ConvertFrom-AndroidStructuredRecordFileMarkers -LogText $LogText)
    foreach ($marker in $markers) {
        $recordName = [string]$marker.name
        $recordId = [string]$marker.id
        $recordKind = [string]$marker.kind
        $userPath = [string]$marker.user_path
        if ([string]::IsNullOrWhiteSpace($recordName) -or [string]::IsNullOrWhiteSpace($recordId) -or [string]::IsNullOrWhiteSpace($userPath)) {
            continue
        }

        $remoteRelativePath = "files/" + ($userPath -replace '^/+', '')
        try {
            $catResult = Invoke-CapturedProcess `
                -FilePath $AdbPath `
                -Arguments @("-s", $DeviceSerial, "exec-out", "run-as", $PackageName, "cat", $remoteRelativePath) `
                -WorkingDirectory $ProjectFullPath `
                -CommandTimeoutSec 30
            if ($catResult.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($catResult.StdoutText)) {
                if (-not [string]::IsNullOrWhiteSpace($StderrLogPath)) {
                    Add-LogSection `
                        -Path $StderrLogPath `
                        -Header "android_record_file_recovery_failed" `
                        -Body ("id={0}`r`nuser_path={1}`r`nexit_code={2}`r`nstderr={3}" -f $recordId, $userPath, $catResult.ExitCode, $catResult.StderrText)
                }
                continue
            }

            $recoveredPath = ConvertTo-StructuredRecordPath -RecordsDir $recordsDir -Name $recordName -Id $recordId -Kind $recordKind -Partial $false
            Set-Content -LiteralPath $recoveredPath -Value $catResult.StdoutText -NoNewline
            $recoveredSummaries.Add([ordered]@{
                id = $recordId
                name = $recordName
                kind = $recordKind
                source_log = "android_user_file:$userPath"
                recovered_path = $recoveredPath
                complete = $true
                chunk_count_seen = 1
                chunk_count_expected = 1
            })

            if ($recordName -eq "scene870_to_image_soak_summary" -and $recordKind -eq "json") {
                $scene870SummaryJson = [string]$recoveredPath
            }
        }
        catch {
            if (-not [string]::IsNullOrWhiteSpace($StderrLogPath)) {
                Add-LogSection `
                    -Path $StderrLogPath `
                    -Header "android_record_file_recovery_exception" `
                    -Body ("id={0}`r`nuser_path={1}`r`nerror={2}" -f $recordId, $userPath, $_.Exception.Message)
            }
        }
    }

    return [ordered]@{
        StructuredRecords = @($recoveredSummaries.ToArray())
        Scene870SummaryJson = $scene870SummaryJson
    }
}


function Get-CommandText {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    return "{0} {1}" -f $FilePath, (Convert-ToProcessArgumentString -Arguments $Arguments)
}

function Convert-ToPowerShellLiteral {
    param([string]$Value)

    if ($null -eq $Value) {
        return "''"
    }

    return "'" + ($Value -replace "'", "''") + "'"
}

function Get-PowerShellInvocationText {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add("&")
    $parts.Add((Convert-ToPowerShellLiteral -Value $FilePath))
    foreach ($arg in $Arguments) {
        $parts.Add((Convert-ToPowerShellLiteral -Value $arg))
    }

    return $parts -join " "
}

function Convert-ToProcessArgumentString {
    param([string[]]$Arguments = @())

    $quoted = foreach ($arg in $Arguments) {
        if ($null -eq $arg -or $arg.Length -eq 0) {
            '""'
            continue
        }

        if ($arg -notmatch '[\s"]') {
            $arg
            continue
        }

        $escaped = $arg -replace '(\\*)"', '$1$1\"'
        $escaped = $escaped -replace '(\\+)$', '$1$1'
        '"' + $escaped + '"'
    }

    return ($quoted -join " ")
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = "",
        [int]$CommandTimeoutSec = 300,
        [string]$StdoutLogPath = "",
        [string]$StderrLogPath = "",
        [string]$StepLabel = "",
        [switch]$AppendToLogs
    )

    $workingDirResolved = if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) { $PWD.Path } else { $WorkingDirectory }
    $stdoutTemp = [System.IO.Path]::GetTempFileName()
    $stderrTemp = [System.IO.Path]::GetTempFileName()
    try {
        Push-Location $workingDirResolved
        try {
            $previousErrorActionPreference = $ErrorActionPreference
            $hasNativeCommandPreference = Test-Path variable:PSNativeCommandUseErrorActionPreference
            $previousNativeCommandPreference = $false
            try {
                # Let native stderr stay in the captured log files; the real failure signal is exit code.
                $ErrorActionPreference = "Continue"
                if ($hasNativeCommandPreference) {
                    $previousNativeCommandPreference = $PSNativeCommandUseErrorActionPreference
                    $PSNativeCommandUseErrorActionPreference = $false
                }

                & $FilePath @Arguments 1> $stdoutTemp 2> $stderrTemp
                $exitCode = $LASTEXITCODE
            }
            finally {
                if ($hasNativeCommandPreference) {
                    $PSNativeCommandUseErrorActionPreference = $previousNativeCommandPreference
                }
                $ErrorActionPreference = $previousErrorActionPreference
            }

            if ($null -eq $exitCode -or [string]::IsNullOrWhiteSpace([string]$exitCode)) {
                $exitCode = if ($?) { 0 } else { 1 }
            }
        }
        finally {
            Pop-Location
        }

        $timedOut = $false
        $stdoutText = Get-Content -Raw $stdoutTemp -ErrorAction SilentlyContinue
        $stderrText = Get-Content -Raw $stderrTemp -ErrorAction SilentlyContinue

        if ($AppendToLogs) {
            $header = if ([string]::IsNullOrWhiteSpace($StepLabel)) {
                "{0} {1}" -f $FilePath, ($Arguments -join " ")
            }
            else {
                "{0}: {1} {2}" -f $StepLabel, $FilePath, ($Arguments -join " ")
            }

            if (-not [string]::IsNullOrWhiteSpace($StdoutLogPath)) {
                Add-LogSection -Path $StdoutLogPath -Header $header -Body $stdoutText
            }

            if (-not [string]::IsNullOrWhiteSpace($StderrLogPath)) {
                $stderrBody = if ($timedOut) {
                    "Command timed out after $CommandTimeoutSec seconds.`r`n$stderrText"
                }
                else {
                    $stderrText
                }
                Add-LogSection -Path $StderrLogPath -Header $header -Body $stderrBody
            }
        }

        return [PSCustomObject]@{
            ExitCode = $exitCode
            StdoutText = $stdoutText
            StderrText = $stderrText
            TimedOut = $timedOut
        }
    }
    finally {
        Remove-Item -LiteralPath $stdoutTemp -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrTemp -Force -ErrorAction SilentlyContinue
    }
}

function Start-AsyncProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = ""
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = Convert-ToProcessArgumentString -Arguments $Arguments
    $psi.WorkingDirectory = $(if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) { $PWD.Path } else { $WorkingDirectory })
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $null = $proc.Start()
    return $proc
}

function Stop-ProcessTree {
    param([int]$ProcessId)

    if ($ProcessId -le 0) {
        return
    }

    try {
        & taskkill.exe /PID $ProcessId /T /F | Out-Null
    }
    catch { }
}

function Set-SingleLineValue {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$LinePrefix,
        [Parameter(Mandatory)][string]$NewValue
    )

    $pattern = "(?m)^" + [Regex]::Escape($LinePrefix) + ".*$"
    $replacement = $LinePrefix + $NewValue
    if (-not [Regex]::IsMatch($Text, $pattern)) {
        throw "Failed to update setting line: $LinePrefix"
    }
    $updated = [Regex]::Replace($Text, $pattern, $replacement)

    return $updated
}

function Get-SingleLineValue {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$LinePrefix
    )

    $pattern = "(?m)^" + [Regex]::Escape($LinePrefix) + "(.*)$"
    $match = [Regex]::Match($Text, $pattern)
    if (-not $match.Success) {
        throw "Failed to read setting line: $LinePrefix"
    }

    return $match.Groups[1].Value.Trim()
}

function Set-OrInsertSingleLineValue {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$LinePrefix,
        [Parameter(Mandatory)][string]$NewValue,
        [string]$InsertAfterLinePrefix = ""
    )

    $pattern = "(?m)^" + [Regex]::Escape($LinePrefix) + ".*$"
    $replacement = $LinePrefix + $NewValue
    if ([Regex]::IsMatch($Text, $pattern)) {
        return [Regex]::Replace($Text, $pattern, $replacement)
    }

    if (-not [string]::IsNullOrWhiteSpace($InsertAfterLinePrefix)) {
        $afterPattern = "(?m)^" + [Regex]::Escape($InsertAfterLinePrefix) + ".*$"
        $afterMatch = [Regex]::Match($Text, $afterPattern)
        if ($afterMatch.Success) {
            $insertAt = $afterMatch.Index + $afterMatch.Length
            return $Text.Insert($insertAt, "`r`n" + $replacement)
        }
    }

    throw "Failed to update setting line: $LinePrefix"
}

function Try-GetSingleLineValue {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$LinePrefix,
        [string]$DefaultValue = ""
    )

    $pattern = "(?m)^" + [Regex]::Escape($LinePrefix) + "(.*)$"
    $match = [Regex]::Match($Text, $pattern)
    if (-not $match.Success) {
        return $DefaultValue
    }

    return $match.Groups[1].Value.Trim()
}

function Normalize-RenderingMethodValue {
    param([string]$RenderingMethod)

    if ([string]::IsNullOrWhiteSpace($RenderingMethod)) {
        return ""
    }

    switch ($RenderingMethod.Trim().ToLowerInvariant()) {
        "mobile" {
            return "mobile"
        }
        "compatibility" {
            return "gl_compatibility"
        }
        "gl_compatibility" {
            return "gl_compatibility"
        }
        default {
            throw "Unsupported rendering method '$RenderingMethod'. Supported values here are: mobile, compatibility, gl_compatibility."
        }
    }
}

function Get-AndroidLaunchSettingsFromExtraArgs {
    param([string[]]$ExtraArgValues)

    $result = [ordered]@{
        RenderingMethod = ""
        HasRenderingMethod = $false
        SyntheticProducerOutputForm = ""
        HasSyntheticProducerOutputForm = $false
        SyntheticStreamCapabilityDowngrades = ""
        HasSyntheticStreamCapabilityDowngrades = $false
        SyntheticCaptureCapabilityDowngrades = ""
        HasSyntheticCaptureCapabilityDowngrades = $false
        UnsupportedArgs = New-Object System.Collections.Generic.List[string]
    }

    for ($index = 0; $index -lt $ExtraArgValues.Count; $index++) {
        $arg = $ExtraArgValues[$index]
        if ([string]::IsNullOrWhiteSpace($arg) -or $arg -eq "--") {
            continue
        }

        if ($arg -like "--rendering-method=*") {
            $result.RenderingMethod = $arg.Substring("--rendering-method=".Length)
            $result.HasRenderingMethod = $true
            continue
        }

        if ($arg -eq "--rendering-method") {
            if ($index + 1 -ge $ExtraArgValues.Count) {
                throw "Expected value after --rendering-method."
            }
            $index++
            $result.RenderingMethod = $ExtraArgValues[$index]
            $result.HasRenderingMethod = $true
            continue
        }

        if ($arg -like "--cambang-synth-producer-output-form=*") {
            $result.SyntheticProducerOutputForm = $arg.Substring("--cambang-synth-producer-output-form=".Length)
            $result.HasSyntheticProducerOutputForm = $true
            continue
        }

        if ($arg -like "--cambang-synth-stream-capability-downgrades=*") {
            $result.SyntheticStreamCapabilityDowngrades = $arg.Substring("--cambang-synth-stream-capability-downgrades=".Length)
            $result.HasSyntheticStreamCapabilityDowngrades = $true
            continue
        }

        if ($arg -like "--cambang-synth-capture-capability-downgrades=*") {
            $result.SyntheticCaptureCapabilityDowngrades = $arg.Substring("--cambang-synth-capture-capability-downgrades=".Length)
            $result.HasSyntheticCaptureCapabilityDowngrades = $true
            continue
        }

        $result.UnsupportedArgs.Add($arg)
    }

    if ($result.HasRenderingMethod) {
        $result.RenderingMethod = Normalize-RenderingMethodValue -RenderingMethod $result.RenderingMethod
    }

    return [PSCustomObject]$result
}

function Get-PatchedAndroidProjectText {
    param(
        [Parameter(Mandatory)][string]$ProjectText,
        [Parameter(Mandatory)][string]$ScenePath,
        [Parameter(Mandatory)][object]$AndroidLaunchSettings
    )

    $updated = $ProjectText
    $updated = Set-SingleLineValue -Text $updated -LinePrefix 'run/main_scene=' -NewValue ('"{0}"' -f $ScenePath)
    if ($AndroidLaunchSettings.HasSyntheticProducerOutputForm) {
        $updated = Set-SingleLineValue -Text $updated -LinePrefix 'maintainer/synthetic_producer_output_form=' -NewValue ('"{0}"' -f $AndroidLaunchSettings.SyntheticProducerOutputForm)
    }
    if ($AndroidLaunchSettings.HasSyntheticStreamCapabilityDowngrades) {
        $updated = Set-SingleLineValue -Text $updated -LinePrefix 'maintainer/synthetic_stream_capability_downgrades=' -NewValue ('"{0}"' -f $AndroidLaunchSettings.SyntheticStreamCapabilityDowngrades)
    }
    if ($AndroidLaunchSettings.HasSyntheticCaptureCapabilityDowngrades) {
        $updated = Set-SingleLineValue -Text $updated -LinePrefix 'maintainer/synthetic_capture_capability_downgrades=' -NewValue ('"{0}"' -f $AndroidLaunchSettings.SyntheticCaptureCapabilityDowngrades)
    }
    $renderingMethod = if ($AndroidLaunchSettings.HasRenderingMethod) {
        $AndroidLaunchSettings.RenderingMethod
    }
    else {
        "mobile"
    }
    $updated = Set-SingleLineValue -Text $updated -LinePrefix 'renderer/rendering_method=' -NewValue ('"{0}"' -f $renderingMethod)
    $updated = Set-OrInsertSingleLineValue `
        -Text $updated `
        -LinePrefix 'renderer/rendering_method.mobile=' `
        -NewValue ('"{0}"' -f $renderingMethod) `
        -InsertAfterLinePrefix 'renderer/rendering_method='
    $updated = Set-OrInsertSingleLineValue `
        -Text $updated `
        -LinePrefix 'maintainer/harness_rendering_method=' `
        -NewValue ('"{0}"' -f $renderingMethod) `
        -InsertAfterLinePrefix 'maintainer/synthetic_producer_output_form='
    return $updated
}

function Set-AndroidManifestRenderingMethodText {
    param(
        [Parameter(Mandatory)][string]$ManifestText,
        [Parameter(Mandatory)][string]$RenderingMethod
    )

    $pattern = 'android:name="org\.godotengine\.rendering\.method"\s+android:value="[^"]+"'
    if (-not [Regex]::IsMatch($ManifestText, $pattern)) {
        throw "Failed to find Android rendering-method manifest metadata."
    }

    return [Regex]::Replace(
            $ManifestText,
            $pattern,
            ('android:name="org.godotengine.rendering.method" android:value="{0}"' -f $RenderingMethod),
            1
    )
}

function Read-AndroidCommandLineAssetArguments {
    param([Parameter(Mandatory)][string]$AssetPath)

    if (-not (Test-Path $AssetPath)) {
        return @()
    }

    $bytes = [System.IO.File]::ReadAllBytes($AssetPath)
    if ($bytes.Length -eq 0) {
        return @()
    }

    $stream = New-Object System.IO.MemoryStream(,$bytes)
    $reader = New-Object System.IO.BinaryReader($stream, [System.Text.Encoding]::UTF8)
    try {
        $count = $reader.ReadInt32()
        $arguments = New-Object System.Collections.Generic.List[string]
        for ($index = 0; $index -lt $count; $index++) {
            $length = $reader.ReadInt32()
            $argBytes = $reader.ReadBytes($length)
            $arguments.Add([System.Text.Encoding]::UTF8.GetString($argBytes))
        }

        return @($arguments.ToArray())
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Write-AndroidCommandLineAssetArguments {
    param(
        [Parameter(Mandatory)][string]$AssetPath,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($stream, [System.Text.Encoding]::UTF8)
    try {
        $writer.Write([int]$Arguments.Count)
        foreach ($argument in $Arguments) {
            $argBytes = [System.Text.Encoding]::UTF8.GetBytes($argument)
            $writer.Write([int]$argBytes.Length)
            $writer.Write($argBytes)
        }
        $writer.Flush()
        [System.IO.File]::WriteAllBytes($AssetPath, $stream.ToArray())
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Get-PatchedAndroidCommandLineArguments {
    param(
        [string[]]$ExistingArguments = @(),
        [Parameter(Mandatory)][string]$RenderingMethod
    )

    $patched = New-Object System.Collections.Generic.List[string]
    $skipNextValue = $false
    foreach ($argument in $ExistingArguments) {
        if ($skipNextValue) {
            $skipNextValue = $false
            continue
        }

        if ($argument -eq "--rendering-method" -or $argument -eq "--rendering-driver") {
            $skipNextValue = $true
            continue
        }

        $patched.Add($argument)
    }

    if ($RenderingMethod -eq "gl_compatibility") {
        $patched.Add("--rendering-method")
        $patched.Add("gl_compatibility")
        $patched.Add("--rendering-driver")
        $patched.Add("opengl3")
    }
    elseif ($RenderingMethod -eq "mobile") {
        $patched.Add("--rendering-method")
        $patched.Add("mobile")
        $patched.Add("--rendering-driver")
        $patched.Add("vulkan")
    }

    return @($patched.ToArray())
}

function Resolve-AndroidSdkRootPath {
    param([string]$ExplicitSdkRoot)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitSdkRoot)) {
        return $ExplicitSdkRoot
    }

    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        return $env:ANDROID_SDK_ROOT
    }

    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        return $env:ANDROID_HOME
    }

    throw "Android SDK root was not supplied and neither ANDROID_SDK_ROOT nor ANDROID_HOME is set."
}

function Resolve-AndroidDebugKeystorePath {
    $homePath = [Environment]::GetFolderPath("UserProfile")
    if ([string]::IsNullOrWhiteSpace($homePath)) {
        throw "Unable to resolve the user profile path for the Android debug keystore."
    }

    $debugKeystorePath = Join-Path $homePath ".android\debug.keystore"
    if (-not (Test-Path $debugKeystorePath)) {
        throw "Android debug keystore not found: $debugKeystorePath"
    }

    return $debugKeystorePath
}

function Resolve-AdbExePath {
    param(
        [string]$ExplicitAdbExe,
        [string]$SdkRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitAdbExe)) {
        if (-not (Test-Path $ExplicitAdbExe)) {
            throw "adb executable not found: $ExplicitAdbExe"
        }
        return $ExplicitAdbExe
    }

    $candidate = Join-Path $SdkRoot "platform-tools\adb.exe"
    if (-not (Test-Path $candidate)) {
        throw "adb executable not found at expected path: $candidate"
    }

    return $candidate
}

function Get-ConnectedAndroidDevices {
    param(
        [Parameter(Mandatory)][string]$AdbPath,
        [Parameter(Mandatory)][string]$ProjectFullPath
    )

    $result = Invoke-CapturedProcess -FilePath $AdbPath -Arguments @("devices") -WorkingDirectory $ProjectFullPath -CommandTimeoutSec 30
    if ($result.ExitCode -ne 0) {
        throw "Failed to query adb devices. Exit code: $($result.ExitCode)"
    }

    $devices = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($result.StdoutText -split "`r?`n")) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed -like "List of devices attached*") {
            continue
        }

        if ($trimmed -match "^(\S+)\s+device\b") {
            $devices.Add($Matches[1])
        }
    }

    return @($devices.ToArray())
}

function Resolve-AndroidDeviceSerialValue {
    param(
        [string]$ExplicitDeviceSerial,
        [Parameter(Mandatory)][string]$AdbPath,
        [Parameter(Mandatory)][string]$ProjectFullPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitDeviceSerial)) {
        return $ExplicitDeviceSerial
    }

    $devices = @(Get-ConnectedAndroidDevices -AdbPath $AdbPath -ProjectFullPath $ProjectFullPath)
    if ($devices.Count -eq 0) {
        throw "No connected Android devices were reported by adb."
    }

    if ($devices.Count -gt 1) {
        throw "Multiple Android devices were reported by adb. Supply -AndroidDeviceSerial explicitly."
    }

    return $devices[0]
}

function Get-LatestFileByFilter {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][string]$Filter
    )

    $file = Get-ChildItem -Path $RootPath -Recurse -File -Filter $Filter |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1

    if ($null -eq $file) {
        throw "Could not find $Filter beneath $RootPath."
    }

    return $file.FullName
}

function Resolve-AndroidPackageValue {
    param(
        [string]$ExplicitPackage,
        [Parameter(Mandatory)][string]$ProjectFullPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPackage)) {
        return $ExplicitPackage
    }

    $metadataPath = Get-LatestFileByFilter -RootPath (Join-Path $ProjectFullPath "android\build") -Filter "output-metadata.json"
    $metadata = Get-Content -Raw $metadataPath | ConvertFrom-Json
    if (-not [string]::IsNullOrWhiteSpace($metadata.applicationId)) {
        return [string]$metadata.applicationId
    }

    throw "Unable to resolve Android package name from $metadataPath."
}

function Resolve-AndroidActivityValue {
    param(
        [string]$ExplicitActivity,
        [Parameter(Mandatory)][string]$ProjectFullPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitActivity)) {
        return $ExplicitActivity
    }

    $manifestPath = Get-LatestFileByFilter -RootPath (Join-Path $ProjectFullPath "android\build") -Filter "AndroidManifest.xml"
    [xml]$manifest = Get-Content -Raw $manifestPath
    $androidNamespace = "http://schemas.android.com/apk/res/android"
    foreach ($activity in $manifest.manifest.application.activity) {
        $name = $activity.GetAttribute("name", $androidNamespace)
        if ([string]::IsNullOrWhiteSpace($name)) {
            continue
        }

        foreach ($intentFilter in $activity.'intent-filter') {
            $hasMain = $false
            $hasLauncher = $false
            foreach ($action in $intentFilter.action) {
                if ($action.GetAttribute("name", $androidNamespace) -eq "android.intent.action.MAIN") {
                    $hasMain = $true
                    break
                }
            }
            foreach ($category in $intentFilter.category) {
                if ($category.GetAttribute("name", $androidNamespace) -eq "android.intent.category.LAUNCHER") {
                    $hasLauncher = $true
                    break
                }
            }

            if ($hasMain -and $hasLauncher) {
                return [string]$name
            }
        }
    }

    throw "Unable to resolve launcher activity from $manifestPath."
}

function Get-AndroidAppPid {
    param(
        [Parameter(Mandatory)][string]$AdbPath,
        [Parameter(Mandatory)][string]$DeviceSerial,
        [Parameter(Mandatory)][string]$PackageName,
        [Parameter(Mandatory)][string]$ProjectFullPath
    )

    $result = Invoke-CapturedProcess `
        -FilePath $AdbPath `
        -Arguments @("-s", $DeviceSerial, "shell", "pidof", $PackageName) `
        -WorkingDirectory $ProjectFullPath `
        -CommandTimeoutSec 15

    if ($result.ExitCode -ne 0) {
        return ""
    }

    return $result.StdoutText.Trim()
}

function Write-AndroidLogcatSnapshot {
    param(
        [Parameter(Mandatory)][string]$AdbPath,
        [Parameter(Mandatory)][string]$DeviceSerial,
        [Parameter(Mandatory)][string]$ProjectFullPath,
        [Parameter(Mandatory)][string]$DeviceLogcatPath
    )

    $result = Invoke-CapturedProcess `
        -FilePath $AdbPath `
        -Arguments @("-s", $DeviceSerial, "logcat", "-d", "-v", "time") `
        -WorkingDirectory $ProjectFullPath `
        -CommandTimeoutSec 30

    if ($result.ExitCode -ne 0) {
        throw "Failed to read adb logcat. Exit code: $($result.ExitCode)"
    }

    $logcatText = if ([string]::IsNullOrEmpty($result.StderrText)) {
        $result.StdoutText
    }
    else {
        $result.StdoutText + "`r`n" + $result.StderrText
    }

    Set-Content -Path $DeviceLogcatPath -Value $logcatText
    return $logcatText
}

function Invoke-AndroidGradleRebuildForRenderer {
    param(
        [Parameter(Mandatory)][object]$LogRecord,
        [Parameter(Mandatory)][string]$ProjectFullPath,
        [Parameter(Mandatory)][string]$RenderingMethod,
        [Parameter(Mandatory)][string]$PackageName
    )

    $androidBuildRoot = Join-Path $ProjectFullPath "android\build"
    $gradleWrapperPath = Join-Path $androidBuildRoot "gradlew.bat"
    $debugManifestPath = Join-Path $androidBuildRoot "src\debug\AndroidManifest.xml"
    $commandLineAssetPath = Join-Path $androidBuildRoot "assets\_cl_"
    $rebuiltApkPath = Join-Path $androidBuildRoot "build\outputs\apk\standard\debug\android_debug.apk"

    if (-not (Test-Path $gradleWrapperPath)) {
        throw "Gradle wrapper not found for Android rebuild: $gradleWrapperPath"
    }
    if (-not (Test-Path $debugManifestPath)) {
        throw "Android debug manifest not found for renderer rebuild: $debugManifestPath"
    }

    $originalDebugManifestText = Get-Content -Raw $debugManifestPath
    $patchedDebugManifestText = Set-AndroidManifestRenderingMethodText `
        -ManifestText $originalDebugManifestText `
        -RenderingMethod $RenderingMethod
    $originalCommandLineAssetBytes = if (Test-Path $commandLineAssetPath) {
        [System.IO.File]::ReadAllBytes($commandLineAssetPath)
    }
    else {
        @()
    }
    $patchedCommandLineArguments = Get-PatchedAndroidCommandLineArguments `
        -ExistingArguments (Read-AndroidCommandLineAssetArguments -AssetPath $commandLineAssetPath) `
        -RenderingMethod $RenderingMethod
    $debugKeystorePath = Resolve-AndroidDebugKeystorePath

    Add-LogSection -Path $LogRecord.StdoutPath -Header "android_gradle_renderer_rebuild" -Body @"
android_build_root=$androidBuildRoot
debug_manifest_path=$debugManifestPath
command_line_asset_path=$commandLineAssetPath
rendering_method=$RenderingMethod
package_name=$PackageName
debug_keystore_path=$debugKeystorePath
patched_command_line_args=$($patchedCommandLineArguments -join ' ')
"@

    try {
        if ($patchedDebugManifestText -ne $originalDebugManifestText) {
            Set-Content -Path $debugManifestPath -Value $patchedDebugManifestText
        }
        Write-AndroidCommandLineAssetArguments -AssetPath $commandLineAssetPath -Arguments $patchedCommandLineArguments

        $gradleArguments = @(
            "-Pperform_signing=true",
            "-Pperform_zipalign=true",
            ("-Pdebug_keystore_file={0}" -f $debugKeystorePath),
            "-Pdebug_keystore_password=android",
            "-Pdebug_keystore_alias=androiddebugkey",
            ("-Pexport_package_name={0}" -f $PackageName),
            "assembleStandardDebug"
        )
        $gradleResult = Invoke-CapturedProcess `
            -FilePath $gradleWrapperPath `
            -Arguments $gradleArguments `
            -WorkingDirectory $androidBuildRoot `
            -CommandTimeoutSec 1800 `
            -StdoutLogPath $LogRecord.StdoutPath `
            -StderrLogPath $LogRecord.StderrPath `
            -StepLabel "gradle_renderer_rebuild" `
            -AppendToLogs
        if ($gradleResult.ExitCode -ne 0) {
            throw "Gradle renderer rebuild failed. Exit code: $($gradleResult.ExitCode)"
        }

        if (-not (Test-Path $rebuiltApkPath)) {
            throw "Gradle renderer rebuild did not produce expected APK: $rebuiltApkPath"
        }

        return $rebuiltApkPath
    }
    finally {
        if ($originalCommandLineAssetBytes.Length -gt 0) {
            [System.IO.File]::WriteAllBytes($commandLineAssetPath, $originalCommandLineAssetBytes)
        }
        if ($patchedDebugManifestText -ne $originalDebugManifestText) {
            Set-Content -Path $debugManifestPath -Value $originalDebugManifestText
        }
    }
}

function Invoke-AndroidRun {
    param(
        [Parameter(Mandatory)][object]$LogRecord,
        [Parameter(Mandatory)][string]$ProjectFullPath,
        [Parameter(Mandatory)][string]$GodotPath,
        [Parameter(Mandatory)][string]$AndroidSdkRootResolved,
        [Parameter(Mandatory)][string]$ScenePath,
        [Parameter(Mandatory)][object]$AndroidLaunchSettings,
        [Parameter(Mandatory)][string]$AdbPath,
        [Parameter(Mandatory)][string]$DeviceSerial,
        [Parameter(Mandatory)][string]$ExportPreset,
        [int]$ObservationTimeoutSec,
        [string]$ExplicitPackageName,
        [string]$ExplicitActivityName,
        [string[]]$FailurePatterns
    )

    $deviceLogcatPath = Join-Path $LogRecord.RunDir "device_logcat.log"
    Ensure-FileExists -Path $deviceLogcatPath

    $projectFilePath = Join-Path $ProjectFullPath "project.godot"
    $originalProjectText = Get-Content -Raw $projectFilePath
    $patchedProjectText = Get-PatchedAndroidProjectText `
        -ProjectText $originalProjectText `
        -ScenePath $ScenePath `
        -AndroidLaunchSettings $AndroidLaunchSettings

    $exportApkPath = Join-Path $LogRecord.RunDir "android_debug.apk"
    $exportGodotLogPath = Join-Path $LogRecord.RunDir "godot_export.log"
    $exportStagingDir = Join-Path ([System.IO.Path]::GetTempPath()) ("cambang-run-godot-export-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $exportStagingDir | Out-Null
    $stagedExportApkPath = Join-Path $exportStagingDir "android_debug.apk"
    $stagedExportGodotLogPath = Join-Path $exportStagingDir "godot_export.log"
    $androidPackageName = ""
    $androidActivityName = ""
    $androidPid = ""
    $sawAppRunning = $false
    $sawHarnessVerdict = $false
    $lastHarnessVerdict = $null
    $timedOut = $false
    $appStillRunningAtShutdown = $false
    $processExitCode = $null
    $logcatText = ""
    $projectRestored = $false
    $patchedProjectRenderingMethod = Get-SingleLineValue -Text $patchedProjectText -LinePrefix 'renderer/rendering_method='
    $patchedProjectRenderingMethodMobile = Try-GetSingleLineValue -Text $patchedProjectText -LinePrefix 'renderer/rendering_method.mobile=' -DefaultValue '(absent)'

    Add-LogSection -Path $LogRecord.StdoutPath -Header "android_run" -Body @"
run_platform=android
scene=$ScenePath
device_serial=$DeviceSerial
export_preset=$ExportPreset
android_sdk_root=$AndroidSdkRootResolved
rendering_method=$(if ($AndroidLaunchSettings.HasRenderingMethod) { $AndroidLaunchSettings.RenderingMethod } else { "mobile" })
synthetic_producer_output_form=$($AndroidLaunchSettings.SyntheticProducerOutputForm)
synthetic_stream_capability_downgrades=$($AndroidLaunchSettings.SyntheticStreamCapabilityDowngrades)
synthetic_capture_capability_downgrades=$($AndroidLaunchSettings.SyntheticCaptureCapabilityDowngrades)
patched_project_renderer_rendering_method=$patchedProjectRenderingMethod
patched_project_renderer_rendering_method_mobile=$patchedProjectRenderingMethodMobile
"@

    try {
        if ($patchedProjectText -ne $originalProjectText) {
            Set-Content -Path $projectFilePath -Value $patchedProjectText
        }

        $exportArguments = @("--headless", "--quit", "--path", $ProjectFullPath, "--log-file", $stagedExportGodotLogPath, "--export-debug", $ExportPreset, $stagedExportApkPath)
        Add-LogSection -Path $LogRecord.StdoutPath -Header "godot_export_command" -Body (Get-CommandText -FilePath $GodotPath -Arguments $exportArguments)
        $exportWrapperCommand = Get-PowerShellInvocationText -FilePath $GodotPath -Arguments $exportArguments
        $exportExitCode = $null
        $exportSuccessObserved = $false
        $exportProc = $null
        try {
            $exportProc = Start-AsyncProcess `
                -FilePath "powershell.exe" `
                -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $exportWrapperCommand) `
                -WorkingDirectory $ProjectFullPath

            $exportDeadlineUtc = (Get-Date).ToUniversalTime().AddSeconds(1800)
            while ($true) {
                if (Test-Path $stagedExportGodotLogPath) {
                    $stagedExportLogText = Get-Content -Raw $stagedExportGodotLogPath -ErrorAction SilentlyContinue
                    if ((Test-Path $stagedExportApkPath) -and ($stagedExportLogText -match '(?m)^\[ DONE \] export$')) {
                        $exportSuccessObserved = $true
                        break
                    }
                }

                $exportProc.Refresh()
                if ($exportProc.HasExited) {
                    $exportExitCode = $exportProc.ExitCode
                    break
                }

                if ((Get-Date).ToUniversalTime() -ge $exportDeadlineUtc) {
                    break
                }

                Start-Sleep -Milliseconds 500
            }

            if ($exportSuccessObserved) {
                if (-not $exportProc.HasExited) {
                    Stop-ProcessTree -ProcessId $exportProc.Id
                    Start-Sleep -Milliseconds 500
                    $exportProc.Refresh()
                }
                $exportExitCode = 0
            }
            elseif (-not $exportProc.HasExited) {
                Stop-ProcessTree -ProcessId $exportProc.Id
                Start-Sleep -Milliseconds 500
                $exportProc.Refresh()
                if ($exportProc.HasExited) {
                    $exportExitCode = $exportProc.ExitCode
                }
                else {
                    $exportExitCode = -7776
                }
            }
        }
        finally {
            if ($null -ne $exportProc) {
                $exportProc.Dispose()
            }
        }

        if (Test-Path $stagedExportGodotLogPath) {
            Copy-Item -LiteralPath $stagedExportGodotLogPath -Destination $exportGodotLogPath -Force
            Add-LogSection -Path $LogRecord.StdoutPath -Header "godot_export_output" -Body (Get-Content -Raw $stagedExportGodotLogPath -ErrorAction SilentlyContinue)
        }

        if ($exportExitCode -ne 0) {
            $processExitCode = $exportExitCode
            throw "Godot Android export failed. Exit code: $exportExitCode"
        }

        if (-not (Test-Path $stagedExportApkPath)) {
            $processExitCode = 1
            throw "Expected exported APK was not produced: $stagedExportApkPath"
        }

        Move-Item -LiteralPath $stagedExportApkPath -Destination $exportApkPath -Force

        if (-not $projectRestored) {
            Set-Content -Path $projectFilePath -Value $originalProjectText
            $projectRestored = $true
        }

        $androidPackageName = Resolve-AndroidPackageValue -ExplicitPackage $ExplicitPackageName -ProjectFullPath $ProjectFullPath
        $androidActivityName = Resolve-AndroidActivityValue -ExplicitActivity $ExplicitActivityName -ProjectFullPath $ProjectFullPath

        if ($AndroidLaunchSettings.HasRenderingMethod -and $AndroidLaunchSettings.RenderingMethod -ne "mobile") {
            $rebuiltApkPath = Invoke-AndroidGradleRebuildForRenderer `
                -LogRecord $LogRecord `
                -ProjectFullPath $ProjectFullPath `
                -RenderingMethod $AndroidLaunchSettings.RenderingMethod `
                -PackageName $androidPackageName
            Copy-Item -LiteralPath $rebuiltApkPath -Destination $exportApkPath -Force
        }

        $clearLogcatResult = Invoke-CapturedProcess `
            -FilePath $AdbPath `
            -Arguments @("-s", $DeviceSerial, "logcat", "-c") `
            -WorkingDirectory $ProjectFullPath `
            -CommandTimeoutSec 30 `
            -StdoutLogPath $LogRecord.StdoutPath `
            -StderrLogPath $LogRecord.StderrPath `
            -StepLabel "adb_logcat_clear" `
            -AppendToLogs
        if ($clearLogcatResult.ExitCode -ne 0) {
            $processExitCode = $clearLogcatResult.ExitCode
            throw "Failed to clear adb logcat buffer. Exit code: $($clearLogcatResult.ExitCode)"
        }

        $forceStopInitialResult = Invoke-CapturedProcess `
            -FilePath $AdbPath `
            -Arguments @("-s", $DeviceSerial, "shell", "am", "force-stop", $androidPackageName) `
            -WorkingDirectory $ProjectFullPath `
            -CommandTimeoutSec 30 `
            -StdoutLogPath $LogRecord.StdoutPath `
            -StderrLogPath $LogRecord.StderrPath `
            -StepLabel "adb_force_stop_initial" `
            -AppendToLogs
        if ($forceStopInitialResult.ExitCode -ne 0) {
            $processExitCode = $forceStopInitialResult.ExitCode
            throw "Failed to force-stop existing Android app instance. Exit code: $($forceStopInitialResult.ExitCode)"
        }

        $installResult = Invoke-CapturedProcess `
            -FilePath $AdbPath `
            -Arguments @("-s", $DeviceSerial, "install", "-r", $exportApkPath) `
            -WorkingDirectory $ProjectFullPath `
            -CommandTimeoutSec 600 `
            -StdoutLogPath $LogRecord.StdoutPath `
            -StderrLogPath $LogRecord.StderrPath `
            -StepLabel "adb_install" `
            -AppendToLogs
        if ($installResult.ExitCode -ne 0) {
            $processExitCode = $installResult.ExitCode
            throw "Failed to install Android APK. Exit code: $($installResult.ExitCode)"
        }

        $startResult = Invoke-CapturedProcess `
            -FilePath $AdbPath `
            -Arguments @("-s", $DeviceSerial, "shell", "am", "start", "-W", "-S", "-n", "$androidPackageName/$androidActivityName") `
            -WorkingDirectory $ProjectFullPath `
            -CommandTimeoutSec 60 `
            -StdoutLogPath $LogRecord.StdoutPath `
            -StderrLogPath $LogRecord.StderrPath `
            -StepLabel "adb_start" `
            -AppendToLogs
        if ($startResult.ExitCode -ne 0) {
            $processExitCode = $startResult.ExitCode
            throw "Failed to start Android activity. Exit code: $($startResult.ExitCode)"
        }

        $deadlineUtc = if ($ObservationTimeoutSec -gt 0) {
            (Get-Date).ToUniversalTime().AddSeconds($ObservationTimeoutSec)
        }
        else {
            $null
        }

        while ($true) {
            Start-Sleep -Milliseconds 750
            $androidPid = Get-AndroidAppPid `
                -AdbPath $AdbPath `
                -DeviceSerial $DeviceSerial `
                -PackageName $androidPackageName `
                -ProjectFullPath $ProjectFullPath
            $isRunning = -not [string]::IsNullOrWhiteSpace($androidPid)
            if ($isRunning) {
                $sawAppRunning = $true
            }

            $logcatText = Write-AndroidLogcatSnapshot `
                -AdbPath $AdbPath `
                -DeviceSerial $DeviceSerial `
                -ProjectFullPath $ProjectFullPath `
                -DeviceLogcatPath $deviceLogcatPath

            $lastHarnessVerdict = Get-LastHarnessVerdict -Text $logcatText
            $sawHarnessVerdict = $null -ne $lastHarnessVerdict

            if ($sawHarnessVerdict) {
                # A shared harness verdict is a terminal scene-level classification.
                # Very fast expected-unsupported Android runs can emit the verdict and exit
                # before PID polling ever observes the package as running. Treat the verdict
                # as proof that the launched scene executed far enough to classify the run,
                # then perform a short bounded drain for trailing record-file markers.
                for ($drainIndex = 0; $drainIndex -lt 4; $drainIndex++) {
                    Start-Sleep -Milliseconds 750
                    $logcatText = Write-AndroidLogcatSnapshot `
                        -AdbPath $AdbPath `
                        -DeviceSerial $DeviceSerial `
                        -ProjectFullPath $ProjectFullPath `
                        -DeviceLogcatPath $deviceLogcatPath

                    $latestHarnessVerdict = Get-LastHarnessVerdict -Text $logcatText
                    if ($null -ne $latestHarnessVerdict) {
                        $lastHarnessVerdict = $latestHarnessVerdict
                        $sawHarnessVerdict = $true
                    }
                }
                break
            }

            if ($sawAppRunning -and -not $isRunning) {
                $confirmedStopped = $true
                for ($stopCheckIndex = 0; $stopCheckIndex -lt 2; $stopCheckIndex++) {
                    Start-Sleep -Milliseconds 500
                    $androidPid = Get-AndroidAppPid `
                        -AdbPath $AdbPath `
                        -DeviceSerial $DeviceSerial `
                        -PackageName $androidPackageName `
                        -ProjectFullPath $ProjectFullPath
                    if (-not [string]::IsNullOrWhiteSpace($androidPid)) {
                        $isRunning = $true
                        $sawAppRunning = $true
                        $confirmedStopped = $false
                        break
                    }

                    $logcatText = Write-AndroidLogcatSnapshot `
                        -AdbPath $AdbPath `
                        -DeviceSerial $DeviceSerial `
                        -ProjectFullPath $ProjectFullPath `
                        -DeviceLogcatPath $deviceLogcatPath

                    $latestHarnessVerdict = Get-LastHarnessVerdict -Text $logcatText
                    if ($null -ne $latestHarnessVerdict) {
                        $lastHarnessVerdict = $latestHarnessVerdict
                        $sawHarnessVerdict = $true
                        break
                    }
                }

                if ($sawHarnessVerdict) {
                    break
                }

                if ($confirmedStopped -and -not $isRunning) {
                    for ($drainIndex = 0; $drainIndex -lt 4; $drainIndex++) {
                        Start-Sleep -Milliseconds 750
                        $logcatText = Write-AndroidLogcatSnapshot `
                            -AdbPath $AdbPath `
                            -DeviceSerial $DeviceSerial `
                            -ProjectFullPath $ProjectFullPath `
                            -DeviceLogcatPath $deviceLogcatPath

                        $latestHarnessVerdict = Get-LastHarnessVerdict -Text $logcatText
                        if ($null -ne $latestHarnessVerdict) {
                            $lastHarnessVerdict = $latestHarnessVerdict
                            $sawHarnessVerdict = $true
                        }
                    }
                    break
                }
            }

            if ($null -ne $deadlineUtc -and (Get-Date).ToUniversalTime() -ge $deadlineUtc) {
                $timedOut = $true
                break
            }
        }
    }
    finally {
        try {
            $androidPackageForStop = $androidPackageName
            if ([string]::IsNullOrWhiteSpace($androidPackageForStop)) {
                try {
                    $androidPackageForStop = Resolve-AndroidPackageValue -ExplicitPackage $ExplicitPackageName -ProjectFullPath $ProjectFullPath
                }
                catch { }
            }

            if (-not [string]::IsNullOrWhiteSpace($androidPackageForStop)) {
                $currentAndroidPid = Get-AndroidAppPid `
                    -AdbPath $AdbPath `
                    -DeviceSerial $DeviceSerial `
                    -PackageName $androidPackageForStop `
                    -ProjectFullPath $ProjectFullPath
                $appStillRunningAtShutdown =
                    -not [string]::IsNullOrWhiteSpace($currentAndroidPid)
                if ($appStillRunningAtShutdown -and -not $sawHarnessVerdict -and
                    -not $timedOut -and $ObservationTimeoutSec -gt 0) {
                    $timedOut = $true
                }
            }

            if ($appStillRunningAtShutdown) {
                Invoke-CapturedProcess `
                    -FilePath $AdbPath `
                    -Arguments @("-s", $DeviceSerial, "shell", "am", "force-stop", $androidPackageForStop) `
                    -WorkingDirectory $ProjectFullPath `
                    -CommandTimeoutSec 30 `
                    -StdoutLogPath $LogRecord.StdoutPath `
                    -StderrLogPath $LogRecord.StderrPath `
                    -StepLabel "adb_force_stop_final" `
                    -AppendToLogs | Out-Null
            }
        }
        catch {
            Add-LogSection -Path $LogRecord.StderrPath -Header "adb_force_stop_final_error" -Body $_.Exception.Message
        }

        try {
            $logcatText = Write-AndroidLogcatSnapshot `
                -AdbPath $AdbPath `
                -DeviceSerial $DeviceSerial `
                -ProjectFullPath $ProjectFullPath `
                -DeviceLogcatPath $deviceLogcatPath
        }
        catch {
            Add-LogSection -Path $LogRecord.StderrPath -Header "adb_logcat_snapshot_error" -Body $_.Exception.Message
        }

        if (-not $projectRestored) {
            Set-Content -Path $projectFilePath -Value $originalProjectText
        }
        Remove-Item -LiteralPath $exportStagingDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    $lastHarnessVerdict = Get-LastHarnessVerdict -Text $logcatText
    $sawHarnessVerdict = $null -ne $lastHarnessVerdict

    if (($null -eq $processExitCode) -and (Test-PatternMatch -Text $logcatText -Patterns $FailurePatterns)) {
        $processExitCode = 1
    }

    return [PSCustomObject]@{
        ProcessExitCode = $processExitCode
        TimedOut = $timedOut
        SawHarnessVerdict = $sawHarnessVerdict
        HarnessVerdict = $lastHarnessVerdict
        SawAppRunning = $sawAppRunning
        AppStillRunningAtShutdown = $appStillRunningAtShutdown
        AndroidPackage = $androidPackageName
        AndroidActivity = $androidActivityName
        AndroidPid = $androidPid
        ExportApkPath = $exportApkPath
        DeviceLogcatPath = $deviceLogcatPath
        DeviceLogcatText = $logcatText
    }
}

if (-not (Test-Path $GodotExe)) {
    throw "Godot executable not found: $GodotExe"
}

$projectFullPath = (Resolve-Path $ProjectPath).Path

if ([string]::IsNullOrWhiteSpace($Scene) -eq [string]::IsNullOrWhiteSpace($Script)) {
    throw "Specify exactly one of -Scene or -Script."
}

if ($TimeoutSec -lt 0) {
    throw "TimeoutSec must be zero or greater."
}

$runIdentity = Get-RunIdentity `
    -ExplicitLabel $RunLabel `
    -ScenePath $Scene `
    -ScriptPath $Script `
    -Platform $RunPlatform `
    -IsWindowed:$Windowed

$commandText = ""
$deviceLogcatPath = $null
$androidPackageName = ""
$androidActivityName = ""
$androidDeviceSerialResolved = ""
$androidApkPath = ""
$androidSawHarnessVerdict = $false
$androidHarnessVerdict = $null
$androidSawAppRunning = $false
$runnerErrorMessage = ""

if ($RunPlatform -eq "windows") {
    $arguments = New-Object System.Collections.Generic.List[string]
    $extraArgBuckets = Get-WindowsGodotExtraArgBuckets -ExtraArgValues $ExtraArgs

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

    foreach ($arg in $extraArgBuckets.EngineArgs) {
        $arguments.Add($arg)
    }

    if ($extraArgBuckets.UserArgs.Count -gt 0) {
        $arguments.Add("--")
        foreach ($arg in $extraArgBuckets.UserArgs) {
            $arguments.Add($arg)
        }
    }

    $commandText = "{0} {1}" -f $GodotExe, ($arguments -join " ")

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
        $capturedResult = Invoke-CapturedProcess `
            -FilePath $GodotExe `
            -Arguments $arguments.ToArray() `
            -WorkingDirectory $projectFullPath `
            -CommandTimeoutSec $TimeoutSec `
            -StdoutLogPath $logRecord.StdoutPath `
            -StderrLogPath $logRecord.StderrPath `
            -StepLabel "godot_run" `
            -AppendToLogs

        $timedOut = $capturedResult.TimedOut
        $processExitCode = $capturedResult.ExitCode
    }
    finally {
        Ensure-FileExists -Path $logRecord.StdoutPath
        Ensure-FileExists -Path $logRecord.StderrPath
    }
}
else {
    if (-not [string]::IsNullOrWhiteSpace($Script)) {
        throw "Android mode currently supports -Scene only."
    }
    if ($ScriptArgs.Count -gt 0) {
        throw "Android mode does not support -ScriptArgs."
    }
    if ($QuitAfter -ne 0) {
        throw "Android mode does not support -QuitAfter. Use -TimeoutSec as the outer observation guard."
    }
    if (-not $CaptureLogs) {
        throw "Android mode requires -CaptureLogs so export/deploy/run artifacts stay local under run-logs."
    }

    $androidLaunchSettings = Get-AndroidLaunchSettingsFromExtraArgs -ExtraArgValues $ExtraArgs
    if ($androidLaunchSettings.UnsupportedArgs.Count -gt 0) {
        throw ("Android mode does not know how to translate these ExtraArgs: {0}" -f ($androidLaunchSettings.UnsupportedArgs -join ", "))
    }

    $androidSdkRootResolved = Resolve-AndroidSdkRootPath -ExplicitSdkRoot $AndroidSdkRoot
    $adbExeResolved = Resolve-AdbExePath -ExplicitAdbExe $AdbExe -SdkRoot $androidSdkRootResolved
    $androidDeviceSerialResolved = Resolve-AndroidDeviceSerialValue `
        -ExplicitDeviceSerial $AndroidDeviceSerial `
        -AdbPath $adbExeResolved `
        -ProjectFullPath $projectFullPath

    $commandText = "android export/deploy scene=$Scene device=$androidDeviceSerialResolved preset=$AndroidExportPreset"
    $logRecord = New-LogRecord -LogsRoot $LogRoot -RunIdentity $runIdentity
    $startTime = Get-Date
    $timedOut = $false
    $processExitCode = $null
    $deviceLogcatPath = Join-Path $logRecord.RunDir "device_logcat.log"

    Write-Host ("RUN: {0}" -f $commandText)
    Write-Host ("LOG ROOT: {0}" -f $logRecord.Root)
    Write-Host ("LOG DIR:  {0}" -f $logRecord.RunDir)

    try {
        Ensure-FileExists -Path $logRecord.StdoutPath
        Ensure-FileExists -Path $logRecord.StderrPath
        Ensure-FileExists -Path $deviceLogcatPath

        $androidResult = Invoke-AndroidRun `
            -LogRecord $logRecord `
            -ProjectFullPath $projectFullPath `
            -GodotPath $GodotExe `
            -AndroidSdkRootResolved $androidSdkRootResolved `
            -ScenePath $Scene `
            -AndroidLaunchSettings $androidLaunchSettings `
            -AdbPath $adbExeResolved `
            -DeviceSerial $androidDeviceSerialResolved `
            -ExportPreset $AndroidExportPreset `
            -ObservationTimeoutSec $TimeoutSec `
            -ExplicitPackageName $AndroidPackage `
            -ExplicitActivityName $AndroidActivity `
            -FailurePatterns $HardFailurePatterns

        $timedOut = $androidResult.TimedOut
        $processExitCode = $androidResult.ProcessExitCode
        $deviceLogcatPath = $androidResult.DeviceLogcatPath
        $androidPackageName = $androidResult.AndroidPackage
        $androidActivityName = $androidResult.AndroidActivity
        $androidApkPath = $androidResult.ExportApkPath
        $androidSawHarnessVerdict = $androidResult.SawHarnessVerdict
        $androidHarnessVerdict = $androidResult.HarnessVerdict
        $androidSawAppRunning = $androidResult.SawAppRunning
    }
    catch {
        $runnerErrorMessage = $_.Exception.Message
        $processExitCode = 1
        Add-LogSection -Path $logRecord.StderrPath -Header "android_run_error" -Body $runnerErrorMessage
    }
    finally {
        Ensure-FileExists -Path $logRecord.StdoutPath
        Ensure-FileExists -Path $logRecord.StderrPath
        if ($null -ne $deviceLogcatPath -and -not (Test-Path $deviceLogcatPath)) {
            Ensure-FileExists -Path $deviceLogcatPath
        }
    }
}

$endTime = Get-Date
$durationSec = [math]::Round(($endTime - $startTime).TotalSeconds, 2)
$stdoutText = Get-Content $logRecord.StdoutPath -Raw -ErrorAction SilentlyContinue
$stderrText = Get-Content $logRecord.StderrPath -Raw -ErrorAction SilentlyContinue
$deviceLogcatText = if ($null -ne $deviceLogcatPath -and (Test-Path $deviceLogcatPath)) {
    Get-Content $deviceLogcatPath -Raw -ErrorAction SilentlyContinue
}
else {
    ""
}
$combinedText = "$stdoutText`n$stderrText`n$deviceLogcatText"
$harnessVerdicts = @(ConvertFrom-HarnessVerdictMarkers -Text $combinedText)
$harnessVerdict = if ($harnessVerdicts.Count -gt 0) { $harnessVerdicts[$harnessVerdicts.Count - 1] } else { $null }
$harnessVerdictObserved = $null -ne $harnessVerdict
$scriptParseLoadFailurePatterns = @(
    "SCRIPT ERROR:",
    "Parse Error",
    "Failed to load script"
)
$scriptParseLoadFailureObserved = Test-PatternMatch -Text $combinedText -Patterns $scriptParseLoadFailurePatterns
$hardFailureObserved = Test-PatternMatch -Text $combinedText -Patterns $HardFailurePatterns

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

if (-not [string]::IsNullOrWhiteSpace($runnerErrorMessage)) {
    $verdictReasons.Add("runner_exception")
}

if ($RunPlatform -eq "android" -and -not $androidSawAppRunning -and -not $harnessVerdictObserved) {
    $verdictReasons.Add("app_never_observed_running")
}

if ($scriptParseLoadFailureObserved) {
    $verdictReasons.Add("script_parse_load_failure")
}
elseif ($hardFailureObserved) {
    $verdictReasons.Add("hard_failure_pattern")
}

if (-not $harnessVerdictObserved) {
    $verdictReasons.Add("missing_harness_verdict")
    $bucket = "error"
    $finalVerdict = "ERROR"
}
else {
    $harnessStatus = [string]$harnessVerdict.status
    $harnessReason = [string]$harnessVerdict.reason
    if ([string]::IsNullOrWhiteSpace($harnessReason)) {
        $harnessReason = "unspecified"
    }

    switch ($harnessStatus) {
        "ok" {
            if ($verdictReasons.Count -eq 0) {
                $bucket = "ok"
                $finalVerdict = "OK"
            }
            else {
                $bucket = "error"
                $finalVerdict = "ERROR"
            }
        }
        "expected_unsupported" {
            if ($verdictReasons.Count -eq 0) {
                $bucket = "expected_unsupported"
                $finalVerdict = "EXPECTED_UNSUPPORTED"
            }
            else {
                $bucket = "error"
                $finalVerdict = "ERROR"
            }
        }
        "fail" {
            $verdictReasons.Add("harness_status=fail")
            $verdictReasons.Add("harness_reason=$harnessReason")
            $bucket = "error"
            $finalVerdict = "ERROR"
        }
        "error" {
            $verdictReasons.Add("harness_status=error")
            $verdictReasons.Add("harness_reason=$harnessReason")
            $bucket = "error"
            $finalVerdict = "ERROR"
        }
        default {
            $verdictReasons.Add("unknown_harness_status=$harnessStatus")
            $bucket = "error"
            $finalVerdict = "ERROR"
        }
    }
}

$returnedExitCode = if ($bucket -eq "ok" -or $bucket -eq "expected_unsupported") {
    0
}
elseif ($null -ne $processExitCode -and $processExitCode -ne 0) {
    $processExitCode
}
else {
    1
}
$logRecord = Finalize-LogRecord -LogRecord $logRecord -Bucket $bucket
$finalDeviceLogcatPath = if ($RunPlatform -eq "android") {
    Join-Path $logRecord.RunDir "device_logcat.log"
}
else {
    ""
}

$structuredRecordSourceLogs = @{}
if (-not [string]::IsNullOrWhiteSpace($logRecord.StdoutPath)) {
    $structuredRecordSourceLogs[$logRecord.StdoutPath] = $true
}
if (-not [string]::IsNullOrWhiteSpace($logRecord.StderrPath)) {
    $structuredRecordSourceLogs[$logRecord.StderrPath] = $true
}
if ($RunPlatform -eq "android") {
    $deviceLogcatPath = $finalDeviceLogcatPath
}
if (-not [string]::IsNullOrWhiteSpace($deviceLogcatPath) -and (Test-Path $deviceLogcatPath)) {
    $structuredRecordSourceLogs[$deviceLogcatPath] = $true
}

$structuredRecordResult = Recover-StructuredRecords -RunDir $logRecord.RunDir -SourceLogs $structuredRecordSourceLogs
$androidRecordFileResult = [ordered]@{
    StructuredRecords = @()
    Scene870SummaryJson = $null
}
if ($RunPlatform -eq "android" -and -not [string]::IsNullOrWhiteSpace($deviceLogcatText) -and -not [string]::IsNullOrWhiteSpace($androidPackageName)) {
    $androidRecordFileResult = Recover-AndroidStructuredRecordFiles `
        -RunDir $logRecord.RunDir `
        -LogText $deviceLogcatText `
        -AdbPath $adbExeResolved `
        -DeviceSerial $androidDeviceSerialResolved `
        -PackageName $androidPackageName `
        -ProjectFullPath $projectFullPath `
        -StderrLogPath $logRecord.StderrPath
}

$combinedStructuredRecords = New-Object System.Collections.Generic.List[object]
foreach ($record in @($structuredRecordResult.StructuredRecords)) {
    $combinedStructuredRecords.Add($record)
}
foreach ($record in @($androidRecordFileResult.StructuredRecords)) {
    $combinedStructuredRecords.Add($record)
}

$scene870SummaryJson = $structuredRecordResult.Scene870SummaryJson
if ($null -ne $androidRecordFileResult.Scene870SummaryJson -and -not [string]::IsNullOrWhiteSpace([string]$androidRecordFileResult.Scene870SummaryJson)) {
    $scene870SummaryJson = $androidRecordFileResult.Scene870SummaryJson
}

$meta = [ordered]@{
    timestamp_utc = $logRecord.TimestampUtc.ToString("o")
    run_platform = $RunPlatform
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
    hard_failure_patterns = @($HardFailurePatterns)
    harness_verdict_observed = $harnessVerdictObserved
    harness_verdicts = @($harnessVerdicts)
    harness_verdict = $harnessVerdict
    command = $commandText
    project_path = $projectFullPath
    stdout_log = $logRecord.StdoutPath
    stderr_log = $logRecord.StderrPath
    run_dir = $logRecord.RunDir
    structured_records = @($combinedStructuredRecords.ToArray())
}

if ($RunPlatform -eq "android") {
    $meta["android_sdk_root"] = $(Resolve-AndroidSdkRootPath -ExplicitSdkRoot $AndroidSdkRoot)
    $meta["adb_exe"] = $(Resolve-AdbExePath -ExplicitAdbExe $AdbExe -SdkRoot $meta["android_sdk_root"])
    $meta["android_export_preset"] = $AndroidExportPreset
    $meta["android_rendering_method"] = $(if ($androidLaunchSettings.HasRenderingMethod) { $androidLaunchSettings.RenderingMethod } else { "mobile" })
    $meta["android_device_serial"] = $androidDeviceSerialResolved
    $meta["android_package"] = $androidPackageName
    $meta["android_activity"] = $androidActivityName
    $meta["android_apk_path"] = $(Join-Path $logRecord.RunDir "android_debug.apk")
    $meta["device_logcat_log"] = $finalDeviceLogcatPath
    $meta["android_harness_verdict_observed"] = $androidSawHarnessVerdict
    $meta["android_harness_verdict"] = $androidHarnessVerdict
    $meta["android_app_observed_running"] = $androidSawAppRunning
}

if ($null -ne $structuredRecordResult.Scene70SummaryJson -and -not [string]::IsNullOrWhiteSpace([string]$structuredRecordResult.Scene70SummaryJson)) {
    $meta["scene70_summary_json"] = [string]$structuredRecordResult.Scene70SummaryJson
}
if ($null -ne $scene870SummaryJson -and -not [string]::IsNullOrWhiteSpace([string]$scene870SummaryJson)) {
    $meta["scene870_summary_json"] = [string]$scene870SummaryJson
}

$meta["script_parse_load_failure_observed"] = $scriptParseLoadFailureObserved
$meta["hard_failure_observed"] = $hardFailureObserved
if (-not [string]::IsNullOrWhiteSpace($runnerErrorMessage)) {
    $meta["runner_error_message"] = $runnerErrorMessage
}

Set-Content -Path $logRecord.VerdictPath -Value $finalVerdict -NoNewline
$meta | ConvertTo-Json -Depth 6 | Set-Content -Path $logRecord.MetaPath
($meta | ConvertTo-Json -Depth 6 -Compress) | Add-Content -Path $logRecord.SummaryPath

Write-Host ("VERDICT:  {0}" -f $finalVerdict) -ForegroundColor $(if ($bucket -eq "ok") { "Green" } elseif ($bucket -eq "expected_unsupported") { "Yellow" } else { "Red" })
Write-Host ("EXIT:     {0}" -f $returnedExitCode)
Write-Host ("DURATION: {0} sec" -f $durationSec)
Write-Host ("RUN DIR:  {0}" -f $logRecord.RunDir)
Write-Host ("SUMMARY:  {0}" -f $logRecord.SummaryPath)

if ($bucket -ne "ok") {
    Write-Host ("REASONS:  {0}" -f ($verdictReasons -join ", ")) -ForegroundColor Yellow
}

exit $returnedExitCode
