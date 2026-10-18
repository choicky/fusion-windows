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
#   8. remove older version directories that are no longer locked.
#
# Nothing here modifies PATH, and nothing deletes a locked directory.
#
# Usage (elevated):
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-PackageDir <dir>] [-NoStart]

param(
  [string]$PackageDir = $PSScriptRoot,
  [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$taskName = 'FusionHost'
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Fcitx5 Fusion'

function Write-Step([string]$message) { Write-Host "== $message" }

if (-not (Test-FusionElevated)) {
  Write-Host 'This script must run elevated: persistent TIP registration writes HKLM (R9).' -ForegroundColor Red
  exit 2
}

$manifestPath = Join-Path $PackageDir 'package.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "package.json not found in $PackageDir" }
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

Write-Step 'verifying package integrity'
$bad = @()
foreach ($entry in $manifest.files) {
  $path = Join-Path $PackageDir ($entry.path -replace '/', '\')
  if (-not (Test-Path -LiteralPath $path)) { $bad += "$($entry.path) (missing)"; continue }
  $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  if ($hash -ne $entry.sha256) { $bad += "$($entry.path) (sha256 mismatch)" }
}
if ($bad.Count -gt 0) {
  Write-Host "Package integrity check FAILED:`n  $($bad -join "`n  ")" -ForegroundColor Red
  exit 4
}
Write-Host "  $($manifest.files.Count) files verified"

Write-Step 'asking a running Host for an orderly stop'
$stop = Stop-FusionHostOrderly -TimeoutMs 20000
if ($stop.Found) {
  Write-Host "  Host pid(s) $($stop.Pids -join ','); stop event $($stop.EventName)"
  Write-Host "  signalled=$($stop.Signalled) exited=$($stop.Exited) after $($stop.WaitedMs) ms"
  if ($stop.Error) { Write-Host "  $($stop.Error)" -ForegroundColor Yellow }
  if (-not $stop.Exited) {
    Write-Host 'The running Host did not stop within the bound; refusing to replace its files.' -ForegroundColor Red
    Write-Host 'Stop it from its own console (development build) and re-run the installer.' -ForegroundColor Red
    exit 5
  }
} else {
  Write-Host '  no running Host'
}

Write-Step "installing files into $versionDir"
New-Item -ItemType Directory -Force $versionDir | Out-Null
foreach ($item in @('bin', 'data', 'lib', 'tools')) {
  $source = Join-Path $PackageDir $item
  if (Test-Path -LiteralPath $source) {
    Copy-Item -LiteralPath $source -Destination $versionDir -Recurse -Force
  }
}
foreach ($file in @('package.json', 'install.ps1', 'uninstall.ps1', 'host-state.ps1')) {
  Copy-Item -LiteralPath (Join-Path $PackageDir $file) -Destination $versionDir -Force
}
Write-Host "  $((Get-ChildItem -LiteralPath $versionDir -Recurse -File).Count) files installed"

Write-Step 'registering the TIP from the installed directory'
& (Join-Path $versionDir 'tools\register.ps1') -DllPath $tsfDll
if ($LASTEXITCODE -ne 0) { throw "register.ps1 failed with exit $LASTEXITCODE" }

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
Write-Host "  execute=$($task.Execute) args='$($task.Arguments)' user=$($task.UserId) logon=$($task.LogonType) runLevel=$($task.RunLevel) instances=$($task.Instances) timeLimit=$($task.TimeLimit)"

if ($NoStart) {
  Write-Step 'not starting the Host (-NoStart)'
} else {
  Write-Step 'starting the Host for the current interactive user'
  Start-ScheduledTask -TaskName $taskName
  $deadline = (Get-Date).AddSeconds(25)
  $started = $null
  while ((Get-Date) -lt $deadline) {
    $started = @(Get-FusionHostState | Where-Object { $_.Path -and $_.Path.StartsWith($versionDir, [StringComparison]::OrdinalIgnoreCase) })
    if ($started.Count -gt 0) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($started.Count -eq 0) {
    Write-Host 'The Host did not start. Installation is complete but nothing is running.' -ForegroundColor Red
    Write-Host "Task last result: $((Get-FusionScheduledTask).LastResult)" -ForegroundColor Red
    exit 3
  }
  foreach ($process in $started) {
    Write-Host "  pid=$($process.Id) session=$($process.SessionId) user=$($process.User) integrity=$($process.Integrity)"
    Write-Host "  path=$($process.Path)"
  }
  if ($started[0].Integrity -ne 'medium') {
    Write-Host "  WARNING: expected Medium integrity, got $($started[0].Integrity)" -ForegroundColor Yellow
  }
}

Write-Step 'removing older version directories that are not locked'
$registered = (Get-FusionRegistrationState | Where-Object { $_.Hive -eq 'HKLM' -and $_.Dll }).Dll
foreach ($directory in Get-ChildItem -LiteralPath $installRoot -Directory) {
  if ($directory.FullName -eq $versionDir) { continue }
  if ($registered -and $registered.StartsWith($directory.FullName, [StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "  kept $($directory.Name): still the registered directory"
    continue
  }
  try {
    Remove-Item -LiteralPath $directory.FullName -Recurse -Force -ErrorAction Stop
    Write-Host "  removed $($directory.Name)"
  } catch {
    Write-Host "  kept $($directory.Name): locked ($($_.Exception.Message.Trim()))" -ForegroundColor Yellow
  }
}

Write-Host ''
Write-Host "Installed $version. User data (%APPDATA%\Fcitx5) was not touched."
