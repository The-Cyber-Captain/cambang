param(
    [string]$GodotExe = "C:\Program Files\Godot4.5\Godot_v4.5.1-stable_win64_console.exe",
    [string]$ProjectPath = $PSScriptRoot,
    [string]$Scene = "",
    [string]$Script = "",
    [int]$QuitAfter = 0,
    [switch]$Windowed,
    [string[]]$ScriptArgs = @(),
    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path $GodotExe)) {
    throw "Godot executable not found: $GodotExe"
}

$projectFullPath = (Resolve-Path $ProjectPath).Path
$arguments = New-Object System.Collections.Generic.List[string]

if ([string]::IsNullOrWhiteSpace($Scene) -eq [string]::IsNullOrWhiteSpace($Script)) {
    throw "Specify exactly one of -Scene or -Script."
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

Write-Host ("RUN: {0} {1}" -f $GodotExe, ($arguments -join " "))
& $GodotExe @arguments
exit $LASTEXITCODE
