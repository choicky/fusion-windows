# Stage 6 WP4 A4: elevated uninstall of an owner-only test package.
#
# What it does, in this order:
#   1. refuse to run unelevated;
#   2. ask the running Host for an orderly stop through the product's own stop
#      event and wait, bounded - never terminate it;
#   3. unregister the TIP with the same script the install used, and remove the
#      CTF\TIP / COM keys that the COM unregister leaves behind;
#   4. delete the logon task;
#   5. remove the version directories, leaving any directory a process still holds
#      open in place and reporting it (never force-deleted, never partially deleted);
#   6. keep %APPDATA%\Fcitx5 unless -RemoveUserData is given, and then remove only
#      the current user's %APPDATA%\Fcitx5 and %LOCALAPPDATA%\Fcitx5.
#
# Failures are self-describing: every phase sets $currentStep, and the catch prints
# "UNINSTALL FAILED at step '<x>': <message>" with the script position before exiting
# 1. With -TranscriptPath the whole run is written to that file by this script.
#
# Usage (elevated):
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-RemoveUserData] [-Version <version>] [-TranscriptPath <file>]

param(
  [switch]$RemoveUserData,
  [string]$Version = '',
  [string]$TranscriptPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$taskName = 'FusionHost'
$clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Fcitx5 Fusion'
$currentStep = 'start'

function Write-Step([string]$message) { Write-Host "== $message" }

$transcriptStarted = $false
if ($TranscriptPath) {
  try {
    $parent = Split-Path -Parent $TranscriptPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
      New-Item -ItemType Directory -Force $parent | Out-Null
    }
    Start-Transcript -Path $TranscriptPath -Force | Out-Null
    $transcriptStarted = $true
  } catch {
    Write-Host "WARNING: cannot start the transcript at ${TranscriptPath}: $($_.Exception.Message)" -ForegroundColor Yellow
  }
}

try {
  if (-not (Test-FusionElevated)) {
    Write-Host 'This script must run elevated: persistent TIP registration lives in HKLM (R9).' -ForegroundColor Red
    exit 2
  }

  if (-not (Test-Path -LiteralPath $installRoot)) {
    Write-Host "Nothing installed: $installRoot does not exist"
  } else {
    Write-Host "Install root: $installRoot"
  }

  $currentStep = 'stop'
  Write-Step 'asking the running Host for an orderly stop'
  $stop = Stop-FusionHostOrderly -TimeoutMs 20000 -InstallRoot $installRoot
  if ($stop.Found) {
    Write-Host "  Host pid(s) $($stop.Pids -join ','); stop event $($stop.EventName)"
    Write-Host "  signalled=$($stop.Signalled) exited=$($stop.Exited) after $($stop.WaitedMs) ms"
    if ($stop.Error) { Write-Host "  $($stop.Error)" -ForegroundColor Yellow }
    if (-not $stop.Exited) {
      throw 'the running Host did not stop within the bound; nothing was removed (a Host started outside the installed task must be stopped from its own console)'
    }
  } else {
    Write-Host '  no running Host from this installation'
  }

  $currentStep = 'unregister'
  Write-Step 'unregistering the TIP'
  $registered = Get-FusionRegisteredDll
  $ours = ($registered -and $registered.StartsWith($installRoot, [StringComparison]::OrdinalIgnoreCase))
  if ($ours) {
    if (Test-Path -LiteralPath $registered) {
      $unregistration = Invoke-PackageScript -ScriptPath (Join-Path $PSScriptRoot 'tools\unregister.ps1') `
        -Parameters @{ DllPath = $registered }
      if (-not $unregistration.Ok) {
        # Registration removal below still runs: a DLL that refuses to unregister
        # (already gone, or a broken handler) must not leave the TIP keys behind.
        Write-Host ("  unregister.ps1 did not succeed{0}; removing the keys directly" -f
          $(if ($unregistration.Error) { ": $($unregistration.Error)" } else { '' })) -ForegroundColor Yellow
      }
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

    # The per-user half outlives the machine-wide keys: EnableLanguageProfile writes
    # HKCU\...\LanguageProfile\<lang>\<profile> Enable=1, which keeps this TIP in the
    # user's input indicator after the DLL is gone - an entry the user can still select.
    # Remove it for the current user only (and the language-list entry, if this user has
    # one). Nothing else is touched and no other user is affected.
    $userInput = Remove-FusionUserInputState
    if ($userInput.TipKeyExisted) {
      Write-Host ("  HKCU CTF\TIP present (Enable={0}); removed={1}" -f
        $(if ($null -eq $userInput.EnableWas) { '(none)' } else { $userInput.EnableWas }), $userInput.TipKeyRemoved)
      Write-Host "  removed $($userInput.TipKey)"
    } else {
      Write-Host '  no HKCU CTF\TIP key for this user'
    }
    if ($userInput.LanguageTipRemoved) {
      Write-Host "  removed $($userInput.LanguageTip) from this user's language list"
    } else {
      Write-Host '  the language list does not contain this TIP; nothing to remove there'
    }
    if ($userInput.Warning) { Write-Host "  $($userInput.Warning)" -ForegroundColor Yellow }
  } elseif ($registered) {
    Write-Host "  HKLM registration points elsewhere ($registered); left untouched" -ForegroundColor Yellow
  } else {
    Write-Host '  no HKLM registration found'
  }

  $currentStep = 'shortcut'
  Write-Step 'removing the Start Menu shortcut'
  $startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Fusion'
  $shortcutPath = Join-Path $startMenu 'Restart FusionHost.lnk'
  if (Test-Path -LiteralPath $shortcutPath) {
    Remove-Item -LiteralPath $shortcutPath -Force
    Write-Host "  removed $shortcutPath"
  } else {
    Write-Host '  no shortcut to remove'
  }
  if ((Test-Path -LiteralPath $startMenu) -and
      @(Get-ChildItem -LiteralPath $startMenu -Force).Count -eq 0) {
    Remove-Item -LiteralPath $startMenu -Force
    Write-Host "  removed the empty folder $startMenu"
  }

  $currentStep = 'task'
  Write-Step "deleting the logon task '$taskName'"
  if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host '  task removed'
  } else {
    Write-Host '  no task'
  }

  $currentStep = 'remove'
  Write-Step 'removing version directories'
  $removed = @()
  $kept = @()
  if (Test-Path -LiteralPath $installRoot) {
    foreach ($directory in @(Get-ChildItem -LiteralPath $installRoot -Directory)) {
      if ($Version -and $directory.Name -ne $Version) { continue }
      $inUse = Test-DirectoryInUse -Path $directory.FullName
      if ($inUse.InUse) {
        $kept += $directory.Name
        Write-Host ("  kept {0}: in use by {1}" -f $directory.Name, ($inUse.Names -join ', ')) -ForegroundColor Yellow
        continue
      }
      try {
        Remove-Item -LiteralPath $directory.FullName -Recurse -Force -ErrorAction Stop
        $removed += $directory.Name
        Write-Host "  removed $($directory.Name)"
      } catch {
        $kept += $directory.Name
        Write-Host "  kept $($directory.Name): $($_.Exception.Message.Trim())" -ForegroundColor Yellow
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
    Write-Host '  A directory a process still holds is left for a later install/uninstall; close the applications that loaded the DLL.' -ForegroundColor Yellow
  }

  $currentStep = 'userdata'
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
  exit 0
} catch {
  $failure = $_
  Write-Host ''
  Write-Host ("UNINSTALL FAILED at step '{0}': {1}" -f $currentStep, $failure.Exception.Message) -ForegroundColor Red
  if ($failure.InvocationInfo) { Write-Host $failure.InvocationInfo.PositionMessage -ForegroundColor Red }
  exit 1
} finally {
  if ($transcriptStarted) {
    try { Stop-Transcript | Out-Null } catch { }
  }
}
