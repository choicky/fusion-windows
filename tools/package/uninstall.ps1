# Stage 6 WP4 A4: elevated uninstall of an owner-only test package.
#
# What it does, in this order:
#   1. refuse to run unelevated;
#   2. ask the running Host for an orderly stop through the product's own stop
#      event and wait, bounded — never terminate it;
#   3. unregister the TIP with the same script the install used, and remove the
#      CTF\TIP / COM keys that the COM unregister leaves behind;
#   4. delete the logon task;
#   5. remove the version directories, leaving any directory that is still locked
#      by a running application in place and reporting it (never force-deleted);
#   6. keep %APPDATA%\Fcitx5 unless -RemoveUserData is given, and then remove only
#      the current user's %APPDATA%\Fcitx5 and %LOCALAPPDATA%\Fcitx5.
#
# Usage (elevated):
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-RemoveUserData] [-Version <version>]

param(
  [switch]$RemoveUserData,
  [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$taskName = 'FusionHost'
$clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Fcitx5 Fusion'

function Write-Step([string]$message) { Write-Host "== $message" }

if (-not (Test-FusionElevated)) {
  Write-Host 'This script must run elevated: persistent TIP registration lives in HKLM (R9).' -ForegroundColor Red
  exit 2
}

if (-not (Test-Path -LiteralPath $installRoot)) {
  Write-Host "Nothing installed: $installRoot does not exist"
} else {
  Write-Host "Install root: $installRoot"
}

Write-Step 'asking the running Host for an orderly stop'
$stop = Stop-FusionHostOrderly -TimeoutMs 20000 -InstallRoot $installRoot
if ($stop.Found) {
  Write-Host "  Host pid(s) $($stop.Pids -join ','); stop event $($stop.EventName)"
  Write-Host "  signalled=$($stop.Signalled) exited=$($stop.Exited) after $($stop.WaitedMs) ms"
  if ($stop.Error) { Write-Host "  $($stop.Error)" -ForegroundColor Yellow }
  if (-not $stop.Exited) {
    Write-Host 'The running Host did not stop within the bound. Nothing was removed.' -ForegroundColor Red
    Write-Host 'A Host started outside the installed task (development build) must be stopped from its own console.' -ForegroundColor Red
    exit 5
  }
} else {
  Write-Host '  no running Host from this installation'
}

Write-Step 'unregistering the TIP'
$registered = (Get-FusionRegistrationState | Where-Object { $_.Hive -eq 'HKLM' -and $_.Dll }).Dll
$ours = $registered -and $registered.StartsWith($installRoot, [StringComparison]::OrdinalIgnoreCase)
if ($ours) {
  if (Test-Path -LiteralPath $registered) {
    & (Join-Path $PSScriptRoot 'tools\unregister.ps1') -DllPath $registered
    if ($LASTEXITCODE -ne 0) { Write-Host "  unregister.ps1 exited $LASTEXITCODE" -ForegroundColor Yellow }
  } else {
    Write-Host "  registered DLL is already gone: $registered"
  }
  # The COM unregister removes the profile, the categories and the COM class. The
  # CTF\TIP container is a registration artifact of the persistent profile and is
  # removed here so "no TIP remains" is true after the run.
  foreach ($key in @("HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid",
                     "HKLM:\SOFTWARE\Classes\CLSID\$clsid")) {
    if (Test-Path -LiteralPath $key) {
      Remove-Item -LiteralPath $key -Recurse -Force
      Write-Host "  removed $key"
    }
  }
} elseif ($registered) {
  Write-Host "  HKLM registration points elsewhere ($registered); left untouched" -ForegroundColor Yellow
} else {
  Write-Host '  no HKLM registration found'
}

Write-Step "deleting the logon task '$taskName'"
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
  Write-Host '  task removed'
} else {
  Write-Host '  no task'
}

Write-Step 'removing version directories'
$removed = @()
$kept = @()
if (Test-Path -LiteralPath $installRoot) {
  foreach ($directory in Get-ChildItem -LiteralPath $installRoot -Directory) {
    if ($Version -and $directory.Name -ne $Version) { continue }
    try {
      Remove-Item -LiteralPath $directory.FullName -Recurse -Force -ErrorAction Stop
      $removed += $directory.Name
      Write-Host "  removed $($directory.Name)"
    } catch {
      $kept += $directory.Name
      Write-Host "  kept $($directory.Name): locked ($($_.Exception.Message.Trim()))" -ForegroundColor Yellow
    }
  }
  if (@(Get-ChildItem -LiteralPath $installRoot -Force).Count -eq 0) {
    Remove-Item -LiteralPath $installRoot -Force
    Write-Host "  removed empty $installRoot"
  } elseif (-not $Version) {
    Write-Host "  $installRoot is not empty; kept" -ForegroundColor Yellow
  }
}
if ($kept.Count -gt 0) {
  Write-Host '  Locked directories are left for a later install/uninstall; close the applications that still hold the DLL.' -ForegroundColor Yellow
}

Write-Step 'user data'
if ($RemoveUserData) {
  foreach ($path in @((Join-Path $env:APPDATA 'Fcitx5'), (Join-Path $env:LOCALAPPDATA 'Fcitx5'))) {
    if (Test-Path -LiteralPath $path) {
      Write-Host "  removing $path"
      Remove-Item -LiteralPath $path -Recurse -Force
    } else {
      Write-Host "  absent $path"
    }
  }
} else {
  Write-Host '  kept %APPDATA%\Fcitx5 (U1: uninstall keeps user data unless -RemoveUserData is given)'
  Write-Host "  $([Environment]::GetFolderPath('ApplicationData'))\Fcitx5"
}

Write-Host ''
Write-Host "Uninstall finished. Removed versions: $(if ($removed.Count) { $removed -join ', ' } else { 'none' }); kept: $(if ($kept.Count) { $kept -join ', ' } else { 'none' })"
