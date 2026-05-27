param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$ShortcutName = "Symbion"
)

$ErrorActionPreference = "Stop"

$exePath = Join-Path $RepoRoot "native\build\Release\symbion_native.exe"
if (-not (Test-Path $exePath)) {
    throw "Native app not found at $exePath. Build it first with scripts\build-native.ps1."
}

$desktop = [Environment]::GetFolderPath("Desktop")
$shortcutPath = Join-Path $desktop "$ShortcutName.lnk"

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $exePath
$shortcut.WorkingDirectory = $RepoRoot
$shortcut.IconLocation = "$exePath,0"
$shortcut.Description = "Symbion native desktop app"
$shortcut.Save()

Write-Host "Created desktop shortcut: $shortcutPath"
