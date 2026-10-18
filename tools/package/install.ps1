# Stage 6 WP4 A4: elevated install / upgrade of an owner-only test package.
#
# What it does, in this order:
#   1. refuse to run unelevated (persistent TIP registration writes HKLM);
#   2. verify every file against the package manifest (SHA-256);
#   3. ask a running Host for an orderly stop (upgrade), so Fcitx saves and no DLL
#      is replaced under a live process;
#   4. copy the package into C:\Program Files\Fcitx5 Fusion\<version>\ (one
#      directory per version, never a shared directory that an upgrade rewrites);
#   5. register the TIP from the new directory (reusing tools\register.ps1);
#   6. create or update the per-user logon task (interactive token, limited run
#      level, no execution time limit, no second instance);
#   7. start the Host for the current interactive user and report its identity;
#   8. remove older version directories that no process holds and that are not the
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
  # Copy the *contents* of each directory into the matching subdirectory. Copying a
  # directory onto an existing one would nest it (bin\bin) on a reinstall over a
  # partially installed version, which is worse than merging over it.
  foreach ($item in @('bin', 'data', 'lib', 'tools')) {
    $source = Join-Path $PackageDir $item
    if (-not (Test-Path -LiteralPath $source)) { continue }
    if (@(Get-ChildItem -LiteralPath $source -Force).Count -eq 0) { continue }
    $target = Join-Path $versionDir $item
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
  }
  foreach ($file in @('package.json', 'install.ps1', 'uninstall.ps1', 'host-state.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PackageDir $file) -Destination $versionDir -Force
  }
  Write-Host "  $(@(Get-ChildItem -LiteralPath $versionDir -Recurse -File).Count) files installed"

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
  # Interactive token + limited run level: the Host always runs as the logged-on
  # user at Medium integrity, never elevated. HostClaim, not the task, remains the
  # authority for "one Host per user x session", so IgnoreNew only avoids a queue of
  # pointless starts.
  $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
  $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable:$false
  Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Force | Out-Null
  $task = Get-FusionScheduledTask
  if (-not $task) { throw "the task '$taskName' was registered but cannot be read back" }
  Write-Host "  execute=$($task.Execute) args='$($task.Arguments)' user=$($task.UserId) logon=$($task.LogonType) runLevel=$($task.RunLevel) instances=$($task.Instances) timeLimit=$($task.TimeLimit)"

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
      Write-Host "  kept $($directory.Name): in use by pid(s) $($inUse.Pids -join ',')" -ForegroundColor Yellow
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
  if ($transcriptStarted) {
    try { Stop-Transcript | Out-Null } catch { }
  }
}
