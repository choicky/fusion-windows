# Stage 6 WP4 A4: elevated install / upgrade of an owner-only test package.
#
# What it does, in this order:
#   1. refuse to run unelevated (persistent TIP registration writes HKLM);
#   2. verify every file against the package manifest (SHA-256);
#   3. close a running tray and mark the window as an intentional restart (U7),
#      so an upgrade raises no "Host stopped" balloon and the old tray releases
#      its version directory;
#   4. ask a running Host for an orderly stop (upgrade), so Fcitx saves and no DLL
#      is replaced under a live process;
#   5. copy the package into C:\Program Files\Fcitx5 Fusion\<version>\ (one
#      directory per version, never a shared directory that an upgrade rewrites);
#   6. register the TIP from the new directory (reusing tools\register.ps1);
#   7. create or update the per-user logon tasks (Host and tray; interactive
#      token, limited run level, no execution time limit, no second instance);
#   8. start the Host for the current interactive user and report its identity,
#      then start the tray from the new version directory;
#   9. remove older version directories that no process holds and that are not the
#      registered one.
#
# Nothing here modifies PATH, and nothing deletes a directory a process holds open.
#
# Failures are self-describing: every phase sets $currentStep, and the catch prints
# "INSTALL FAILED at step '<x>': <message>" with the script position before exiting
# 1. With -TranscriptPath the whole run (including the phases that only run
# elevated) is written to that file by this script itself.
#
# Usage (elevated):
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-PackageDir <dir>] [-NoStart] [-TranscriptPath <file>]
#   powershell -ExecutionPolicy Bypass -File install.ps1 -VerifyOnly   # read-only check

param(
  [string]$PackageDir = $PSScriptRoot,
  [switch]$NoStart,
  [switch]$VerifyOnly,
  [string]$TranscriptPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$taskName = 'FusionHost'
$trayTaskName = 'FusionTray'
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
    # A logging failure must not block an install; the console still carries it.
    Write-Host "WARNING: cannot start the transcript at ${TranscriptPath}: $($_.Exception.Message)" -ForegroundColor Yellow
  }
}

try {
  if (-not $VerifyOnly -and -not (Test-FusionElevated)) {
    Write-Host 'This script must run elevated: persistent TIP registration writes HKLM (R9).' -ForegroundColor Red
    exit 2
  }

  $currentStep = 'manifest'
  $manifestPath = Join-Path $PackageDir 'package.json'
  if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw ("No package.json in the -PackageDir value received: '{0}'. Pass the extracted package directory (the one that contains bin\, data\, lib\ and package.json)." -f $PackageDir)
  }
  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  $version = $manifest.version
  if ($version -notmatch '^[0-9A-Za-z][0-9A-Za-z.+-]*$') { throw "Refusing unexpected version identifier: $version" }
  $versionDir = Join-Path $installRoot $version
  $hostExe = Join-Path $versionDir 'bin\FusionHost.exe'
  $tsfDll = Join-Path $versionDir 'bin\FusionTsf.dll'

  $user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
  $sessionId = (Get-Process -Id $PID).SessionId
  Write-Host "Package:  $($manifest.package) $version"
  Write-Host "Source:   $($manifest.sourceCommit)$(if ($manifest.sourceDirty) { ' (DIRTY WORKING TREE)' } else { '' })"
  Write-Host "Runtime:  $($manifest.runtime.identity)"
  Write-Host "Target:   $versionDir"
  Write-Host "Task user: $user (session $sessionId, interactive, limited run level)"

  $currentStep = 'verify'
  Write-Step 'verifying package integrity'
  $bad = @()
  $entries = @($manifest.files)
  foreach ($entry in $entries) {
    $path = Join-Path $PackageDir ($entry.path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path)) { $bad += "$($entry.path) (missing)"; continue }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($hash -ne $entry.sha256) { $bad += "$($entry.path) (sha256 mismatch)" }
  }
  if ($bad.Count -gt 0) {
    # Thrown, not exited: every failure then leaves through the one catch below and
    # is reported as "INSTALL FAILED at step 'verify'", so a failure always names
    # its own step.
    throw ("Package integrity check failed:`n  {0}" -f ($bad -join "`n  "))
  }
  Write-Host "  $($entries.Count) files verified"

  if ($VerifyOnly) {
    Write-Host "Package verified: $($entries.Count) files match the manifest. Nothing was installed."
    exit 0
  }

  # U7: close a running tray before the Host stops, and mark the window as an
  # intentional restart. Without this an upgrade would raise a "Host stopped"
  # balloon and the old tray would keep its version directory busy.
  $currentStep = 'tray'
  Write-Step 'closing a running Fusion tray'
  $tray = Stop-FusionTray -TimeoutMs 5000
  if ($tray.Found) {
    Write-Host ("  pid(s) {0}: asked={1} exited={2} after {3} ms{4}" -f ($tray.Pids -join ','),
      $tray.Requested, $tray.Exited, $tray.WaitedMs, $(if ($tray.Forced) { ' (forced)' } else { '' }))
  } else {
    Write-Host '  no tray running'
  }
  if (-not $tray.Exited) { throw 'a running Fusion tray did not exit; use its Exit command and re-run the installer' }
  $restartMarker = Start-FusionRestartMarker
  Write-Host "  restart marker: $restartMarker"

  $currentStep = 'stop'
  Write-Step 'asking a running Host for an orderly stop'
  $stop = Stop-FusionHostOrderly -TimeoutMs 20000
  if ($stop.Found) {
    Write-Host "  Host pid(s) $($stop.Pids -join ','); stop event $($stop.EventName)"
    Write-Host "  signalled=$($stop.Signalled) exited=$($stop.Exited) after $($stop.WaitedMs) ms"
    if ($stop.Error) { Write-Host "  $($stop.Error)" -ForegroundColor Yellow }
    if (-not $stop.Exited) {
      throw 'the running Host did not stop within the bound; refusing to replace its files (stop it from its own console and re-run the installer)'
    }
  } else {
    Write-Host '  no running Host'
  }

  $currentStep = 'copy'
  Write-Step "installing files into $versionDir"
  New-Item -ItemType Directory -Force $versionDir | Out-Null
  $copy = Copy-FusionPackageFiles -PackageDir $PackageDir -VersionDir $versionDir -Entries $entries
  if ($copy.Failure) { throw $copy.Failure }
  Write-Host ("  {0} copied, {1} unchanged (identical SHA-256); {2} files present" -f
    $copy.Copied, $copy.Skipped, $copy.Files)

  $currentStep = 'register'
  Write-Step 'registering the TIP from the installed directory'
  $registration = Invoke-PackageScript -ScriptPath (Join-Path $versionDir 'tools\register.ps1') `
    -Parameters @{ DllPath = $tsfDll }
  if (-not $registration.Ok) {
    throw ("register.ps1 did not succeed{0}" -f $(if ($registration.Error) { ": $($registration.Error)" } else { '' }))
  }

  $currentStep = 'task'
  Write-Step "creating or updating the logon task '$taskName'"
  $action = New-ScheduledTaskAction -Execute $hostExe -WorkingDirectory $versionDir
  $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
  # R34: a cancelled logoff/restart/shutdown ends the Host, and the logon trigger cannot help
  # because the session never ended. Windows writes User32 1077 when a *logoff* is cancelled
  # and User32 1073 when a *restart or shutdown* is cancelled; neither is written when the
  # attempt completes (measured 2026-10-18 and 2026-10-19), so an event trigger on both starts
  # the Host again. Scoped to this user's SID; delayed so the previous Host has released
  # HostClaim (its bounded orderly shutdown is 4 s) before the new instance starts.
  # New-ScheduledTaskTrigger has no event form, hence the CIM class.
  #
  # The SID is an attribute of System/Security, not of System: `@UserID=` on System matches
  # nothing (found in review; the query below is proven against records 104140, 104461 and
  # 104462, and against the completed restart of 2026-10-19 11:24, which it does not match).
  $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $xpath = "*[System[Provider[@Name='User32'] and (EventID=1077 or EventID=1073) and Security[@UserID='$sid']]]"
  # The QueryList must be well-formed XML: closing </Query> is required (a missing one is
  # tolerated by Get-WinEvent but rejected when the task is registered.)
  $subscription = "<QueryList><Query Id=`"0`" Path=`"System`"><Select Path=`"System`">$xpath</Select></Query></QueryList>"
  # Validate the query before it goes into a trigger: a bad XPath would only show up as a
  # trigger that never fires. EventLogReader takes the XPath and returns $null when nothing
  # matches (a valid answer); the QueryList form the trigger actually uses is then checked
  # against the same log whenever at least one event exists.
  $subscriptionOk = $true
  $subscriptionError = ''
  $firstEvent = $null
  try {
    $query = New-Object System.Diagnostics.Eventing.Reader.EventLogQuery('System',
      [System.Diagnostics.Eventing.Reader.PathType]::LogName, $xpath)
    $reader = New-Object System.Diagnostics.Eventing.Reader.EventLogReader($query)
    $firstEvent = $reader.ReadEvent()
    $reader.Dispose()
  } catch {
    $subscriptionOk = $false
    $subscriptionError = $_.Exception.Message
  }
  if ($subscriptionOk -and $null -ne $firstEvent) {
    try {
      # -FilterXml carries its own <Query Path>, so it cannot be combined with -LogName.
      $null = Get-WinEvent -FilterXml $subscription -MaxEvents 1 -ErrorAction Stop
    } catch {
      $subscriptionOk = $false
      $subscriptionError = ("the QueryList form was rejected: {0}" -f $_.Exception.Message)
    }
  }
  if (-not $subscriptionOk) {
    throw ("the event subscription is not a valid query: {0}`n  {1}" -f $subscriptionError, $subscription)
  }
  $eventClass = Get-CimClass -Namespace ROOT/Microsoft/Windows/TaskScheduler -ClassName MSFT_TaskEventTrigger
  $eventTrigger = New-CimInstance -CimClass $eventClass -ClientOnly
  $eventTrigger.Enabled = $true
  $eventTrigger.Subscription = $subscription
  $eventTrigger.Delay = 'PT10S'
  # Interactive token + limited run level: the Host always runs as the logged-on
  # user at Medium integrity, never elevated. HostClaim, not the task, remains the
  # authority for "one Host per user x session", so IgnoreNew only avoids a queue of
  # pointless starts.
  $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
  $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable:$false
  Register-ScheduledTask -TaskName $taskName -Action $action -Trigger @($trigger, $eventTrigger) `
    -Principal $principal -Settings $settings -Force | Out-Null
  $task = Get-FusionScheduledTask
  if (-not $task) { throw "the task '$taskName' was registered but cannot be read back" }
  Write-Host "  execute=$($task.Execute) args='$($task.Arguments)' user=$($task.UserId) logon=$($task.LogonType) runLevel=$($task.RunLevel) instances=$($task.Instances) timeLimit=$($task.TimeLimit)"
  Write-Host "  triggers=$($task.Triggers) count=$($task.TriggerCount)"
  if ($task.EventSubscription -notmatch '1077' -or $task.EventSubscription -notmatch '1073') {
    throw "the task was registered without the User32 1077/1073 event trigger (subscription: '$($task.EventSubscription)')"
  }

  # U7: the tray has its own task, deliberately not a second action on the Host
  # task. A task instance stays alive while any of its actions runs, so a
  # long-lived tray action would keep 'FusionHost' in the Running state and
  # Start-ScheduledTask (MultipleInstances=IgnoreNew, used by restart-host.ps1)
  # would be ignored - a manual restart would silently do nothing.
  $currentStep = 'traytask'
  Write-Step "creating or updating the logon task '$trayTaskName'"
  $trayExe = Join-Path $versionDir 'bin\FusionTray.exe'
  if (-not (Test-Path -LiteralPath $trayExe)) {
    throw "the package does not contain bin\FusionTray.exe ($trayExe)"
  }
  $trayAction = New-ScheduledTaskAction -Execute $trayExe -WorkingDirectory $versionDir
  $trayTrigger = New-ScheduledTaskTrigger -AtLogOn -User $user
  $trayPrincipal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
  $traySettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable:$false
  Register-ScheduledTask -TaskName $trayTaskName -Action $trayAction -Trigger $trayTrigger `
    -Principal $trayPrincipal -Settings $traySettings -Force | Out-Null
  $trayTask = Get-FusionScheduledTaskByName -TaskName $trayTaskName
  if (-not $trayTask) { throw "the task '$trayTaskName' was registered but cannot be read back" }
  Write-Host "  execute=$($trayTask.Execute) user=$($trayTask.UserId) logon=$($trayTask.LogonType) runLevel=$($trayTask.RunLevel) instances=$($trayTask.Instances) triggers=$($trayTask.Triggers)"

  $currentStep = 'shortcut'
  Write-Step 'creating the Start Menu shortcut for the manual restart tool'
  $restartScript = Join-Path $versionDir 'restart-host.ps1'
  if (-not (Test-Path -LiteralPath $restartScript)) {
    throw "the package does not contain restart-host.ps1 ($restartScript)"
  }
  $startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Fusion'
  New-Item -ItemType Directory -Force $startMenu | Out-Null
  $shortcutPath = Join-Path $startMenu 'Restart FusionHost.lnk'
  $powershellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($shortcutPath)
  $shortcut.TargetPath = $powershellExe
  $shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$restartScript`" -Pause"
  $shortcut.WorkingDirectory = $versionDir
  $shortcut.IconLocation = "$powershellExe,0"
  $shortcut.Description = 'Restart the Fusion Host for this user and session'
  $shortcut.Save()
  Write-Host "  $shortcutPath"
  Write-Host "  -> $powershellExe $($shortcut.Arguments)"

  $currentStep = 'start'
  if ($NoStart) {
    Write-Step 'not starting the Host (-NoStart)'
  } else {
    Write-Step 'starting the Host for the current interactive user'
    Start-ScheduledTask -TaskName $taskName
    $deadline = (Get-Date).AddSeconds(25)
    $started = @()
    while ((Get-Date) -lt $deadline) {
      $started = @(Get-FusionHostState | Where-Object { $_.Path -and $_.Path.StartsWith($versionDir, [StringComparison]::OrdinalIgnoreCase) })
      if ($started.Count -gt 0) { break }
      Start-Sleep -Milliseconds 250
    }
    if ($started.Count -eq 0) {
      $lastResult = Get-FusionScheduledTask
      throw ("the Host did not start within 25 s (files are installed and the task exists; task last result: {0})" -f
        $(if ($lastResult) { $lastResult.LastResult } else { 'task not readable' }))
    }
    foreach ($process in $started) {
      Write-Host "  pid=$($process.Id) session=$($process.SessionId) user=$($process.User) integrity=$($process.Integrity)"
      Write-Host "  path=$($process.Path)"
    }
    if ($started[0].Integrity -ne 'medium') {
      Write-Host "  WARNING: expected Medium integrity, got $($started[0].Integrity)" -ForegroundColor Yellow
    }
  }

  # U7: start the tray after the Host, so its first observation is a running
  # Host and it never reports a baseline absence as an exit.
  $currentStep = 'traystart'
  if ($NoStart) {
    Write-Step 'not starting the Fusion tray (-NoStart)'
  } else {
    Write-Step 'starting the Fusion tray'
    Start-ScheduledTask -TaskName $trayTaskName
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and @(Get-FusionTrayState).Count -eq 0) { Start-Sleep -Milliseconds 200 }
    $trays = @(Get-FusionTrayState)
    if ($trays.Count -eq 0) {
      Write-Host '  WARNING: the tray did not start; start the FusionTray task manually' -ForegroundColor Yellow
    } elseif ($trays.Count -gt 1) {
      throw ("{0} tray processes are running; the single-instance rule failed" -f $trays.Count)
    } else {
      Write-Host "  pid=$($trays[0].Id) path=$($trays[0].Path)"
    }
  }

  $currentStep = 'cleanup'
  Write-Step 'removing older version directories that are not in use'
  $registered = Get-FusionRegisteredDll
  foreach ($directory in @(Get-ChildItem -LiteralPath $installRoot -Directory)) {
    if ($directory.FullName -eq $versionDir) { continue }
    if ($registered -and $registered.StartsWith($directory.FullName, [StringComparison]::OrdinalIgnoreCase)) {
      Write-Host "  kept $($directory.Name): still the registered directory"
      continue
    }
    $inUse = Test-DirectoryInUse -Path $directory.FullName
    if ($inUse.InUse) {
      Write-Host ("  kept {0}: in use by {1}" -f $directory.Name, ($inUse.Names -join ', ')) -ForegroundColor Yellow
      continue
    }
    try {
      Remove-Item -LiteralPath $directory.FullName -Recurse -Force -ErrorAction Stop
      Write-Host "  removed $($directory.Name)"
    } catch {
      Write-Host "  kept $($directory.Name): $($_.Exception.Message.Trim())" -ForegroundColor Yellow
    }
  }

  Write-Host ''
  Write-Host "Installed $version. User data (%APPDATA%\Fcitx5) was not touched."
  exit 0
} catch {
  $failure = $_
  Write-Host ''
  Write-Host ("INSTALL FAILED at step '{0}': {1}" -f $currentStep, $failure.Exception.Message) -ForegroundColor Red
  if ($failure.InvocationInfo) { Write-Host $failure.InvocationInfo.PositionMessage -ForegroundColor Red }
  exit 1
} finally {
  # The restart marker covers this run only; a stale one must never silence a
  # later real Host exit.
  try { Clear-FusionRestartMarker } catch { }
  if ($transcriptStarted) {
    try { Stop-Transcript | Out-Null } catch { }
  }
}
