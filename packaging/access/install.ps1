# Wesnoth Access @ACCESS_VERSION@ installer (for Battle for Wesnoth @GAME_VERSION@)
#
# Finds an existing Battle for Wesnoth @GAME_VERSION@ installation, remembers its
# location and creates "Wesnoth Access" shortcuts on the desktop and in the
# Start menu. The original installation is never modified: Wesnoth Access runs
# its own wesnoth.exe from this folder and reads the original game data with
# the --data-dir option. Run Uninstall.cmd to remove the shortcuts again.
#
# Usage: Install.cmd                       (automatic search)
#        Install.cmd "D:\Games\Wesnoth"    (use this installation folder)

param([string]$GameDir = "")

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Version = "@GAME_VERSION@"
$ConfigFile = Join-Path $Here "data-path.txt"

function Say($text) { Write-Host $text }

function Test-GameDir($dir) {
    if ([string]::IsNullOrWhiteSpace($dir)) { return $false }
    return (Test-Path (Join-Path $dir "data\_main.cfg"))
}

function Get-GameVersionHint($dir) {
    foreach ($name in @("changelog.txt", "changelog.md")) {
        $f = Join-Path $dir $name
        if (Test-Path $f) {
            $head = Get-Content $f -TotalCount 40 -ErrorAction SilentlyContinue
            foreach ($line in $head) {
                if ($line -match "Version\s+([0-9]+\.[0-9]+\.[0-9]+)") { return $Matches[1] }
            }
        }
    }
    return ""
}

Say "Wesnoth Access @ACCESS_VERSION@ setup."
Say "This package needs Battle for Wesnoth $Version. Searching for it."

$candidates = @()
if ($GameDir -ne "") { $candidates += $GameDir }

# Standalone package: the game data is in this folder.
$candidates += $Here

# Locations written by the official Windows installer.
foreach ($root in @("HKCU:\Software", "HKLM:\Software", "HKLM:\Software\WOW6432Node")) {
    $key = Join-Path $root "Battle for Wesnoth\$Version"
    try {
        $value = (Get-ItemProperty -Path $key -ErrorAction Stop)."(default)"
        if ($value) { $candidates += $value }
    } catch { }
}
$candidates += (Join-Path $env:ProgramFiles "Battle for Wesnoth $Version")
if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} "Battle for Wesnoth $Version") }
$candidates += (Join-Path $env:LOCALAPPDATA "Programs\Battle for Wesnoth $Version")

# Steam.
foreach ($steamKey in @("HKCU:\Software\Valve\Steam", "HKLM:\Software\WOW6432Node\Valve\Steam")) {
    try {
        $p = Get-ItemProperty -Path $steamKey -ErrorAction Stop
        $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
        if ($steam) { $candidates += (Join-Path $steam "steamapps\common\wesnoth") }
    } catch { }
}

$found = ""
foreach ($c in $candidates) {
    if (Test-GameDir $c) { $found = (Resolve-Path $c).Path; break }
}

if ($found -eq "") {
    Say "Battle for Wesnoth $Version was not found automatically."
    Say "Type the full path of the folder that contains the Wesnoth data folder, then press Enter."
    Say "For example: C:\Program Files\Battle for Wesnoth $Version"
    $typed = Read-Host "Folder"
    $typed = $typed.Trim('"', ' ')
    if (Test-GameDir $typed) { $found = (Resolve-Path $typed).Path }
}

if ($found -eq "") {
    Say "Setup failed: no Battle for Wesnoth data folder found. Nothing was changed."
    exit 1
}

$hint = Get-GameVersionHint $found
if ($hint -ne "" -and $hint -ne $Version) {
    Say "Warning: the installation in $found looks like version $hint, but Wesnoth Access needs $Version."
    Say "Using game data from another version can cause errors. Type yes and press Enter to continue anyway."
    $answer = Read-Host "Continue"
    if ($answer -ne "yes") { Say "Setup cancelled. Nothing was changed."; exit 1 }
}

# OEM code page: the encoding cmd.exe uses when the launcher reads this file.
Set-Content -Path $ConfigFile -Value $found -Encoding Oem
Say "Using the game data in: $found"

# Shortcuts start wesnoth.exe directly (no console window).
$exe = Join-Path $Here "wesnoth.exe"
$arguments = "--accessibility"
if ($found -ne $Here) { $arguments += " --data-dir `"$found`"" }

$shell = New-Object -ComObject WScript.Shell
$targets = @(
    (Join-Path ([Environment]::GetFolderPath("Desktop")) "Wesnoth Access.lnk"),
    (Join-Path ([Environment]::GetFolderPath("Programs")) "Wesnoth Access.lnk")
)
foreach ($t in $targets) {
    try {
        $lnk = $shell.CreateShortcut($t)
        $lnk.TargetPath = $exe
        $lnk.Arguments = $arguments
        $lnk.WorkingDirectory = $Here
        $lnk.Description = "Battle for Wesnoth with screen reader support (Wesnoth Access)"
        $lnk.Save()
        Say "Created shortcut: $t"
    } catch {
        Say "Could not create shortcut $t : $($_.Exception.Message)"
    }
}

Say "Setup finished. Start the game with the Wesnoth Access shortcut on the desktop,"
Say "or with Start Wesnoth Access.cmd in this folder."
Say "Your original Battle for Wesnoth installation was not changed."
exit 0
