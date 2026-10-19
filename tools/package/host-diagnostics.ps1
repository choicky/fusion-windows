# Stage 6 WP7: explicit opt-in / opt-out for the Host technical log (R30).
#
# The product default stays OFF. This helper turns the log on or off for *this user*
# only, through the same environment variable the Host reads at startup
# (FUSION_HOST_DIAGNOSTICS = 1), restarts the Host, and then **verifies the effect on
# the process that was actually launched**:
#   * enable  -> a log file for the new pid whose write time is at or after that
#                process's start (a historical file, including one left by an earlier
#                process with the same pid, does not count);
#   * disable -> no log file for the new pid written at or after its start.
# A failed verification is reported as failure, not as success. The mere existence of
# the log directory is never taken as evidence of current logging.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File host-diagnostics.ps1 -Status
#   powershell -ExecutionPolicy Bypass -File host-diagnostics.ps1 -Enable  [-NoRestart]
#   powershell -ExecutionPolicy Bypass -File host-diagnostics.ps1 -Disable [-NoRestart]

param(
  [switch]$Enable,
  [switch]$Disable,
  [switch]$Status,
  [switch]$NoRestart,
  [switch]$Pause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$variable = 'FUSION_HOST_DIAGNOSTICS'
$logDir = Join-Path $env:LOCALAPPDATA 'fusion-host'
$sessionId = (Get-Process -Id $PID).SessionId
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

if (@($Enable, $Disable, $Status | Where-Object { $_ }).Count -gt 1) {
  throw 'give at most one of -Enable, -Disable, -Status'
}
if (-not ($Enable -or $Disable -or $Status)) { $Status = $true }

# New processes read the user environment block; explorer and the Task Scheduler
# build it per launch, but an already-running process never sees the change.
if (-not ('FusionEnvBroadcast' -as [type])) {
  Add-Type -Namespace '' -Name 'FusionEnvBroadcast' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Auto)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint msg, System.UIntPtr wParam,
    string lParam, uint flags, uint timeout, out System.UIntPtr result);
'@
}
function Send-EnvironmentChanged {
  $result = [UIntPtr]::Zero
  # HWND_BROADCAST, WM_SETTINGCHANGE, SMTO_ABORTIFHUNG
  $null = [FusionEnvBroadcast]::SendMessageTimeout([IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result)
}

function Get-Setting { [Environment]::GetEnvironmentVariable($variable, 'User') }
# Same verified scope as the restart tool: session and owner both match.
function Get-VerifiedHosts {
  @(Get-FusionHostState | Where-Object {
      $_.SessionId -eq $sessionId -and $_.User -eq $me
    })
}

# The log file for one process, with whether it was written by *that* launch. A file
# with the same pid from an earlier process is stale and does not count.
function Get-LaunchLog($host_) {
  $file = Join-Path $logDir ("fusion-host-{0}.log" -f $host_.Id)
  if (-not (Test-Path -LiteralPath $file)) {
    return [pscustomobject]@{ Path = $file; Exists = $false; FromThisLaunch = $false; Detail = 'no file' }
  }
  $item = Get-Item -LiteralPath $file
  $started = $host_.StartTime
  if ($null -eq $started) {
    return [pscustomobject]@{ Path = $file; Exists = $true; FromThisLaunch = $false; Detail = 'process start time unreadable' }
  }
  $fresh = $item.LastWriteTime -ge $started.AddSeconds(-1)
  return [pscustomobject]@{
    Path = $file; Exists = $true; FromThisLaunch = $fresh
    Detail = ("written={0:HH:mm:ss} started={1:HH:mm:ss}" -f $item.LastWriteTime, $started)
  }
}

function Restart-Host {
  $restart = Join-Path $PSScriptRoot 'restart-host.ps1'
  if (-not (Test-Path -LiteralPath $restart)) { throw "restart-host.ps1 is missing next to this script: $restart" }
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $restart -TimeoutMs 20000
  if ($LASTEXITCODE -ne 0) { throw "the Host restart failed (exit $LASTEXITCODE)" }
}

function Show-Status {
  $value = Get-Setting
  Write-Host ("{0} (user) = {1}" -f $variable, $(if ($value) { "'$value'" } else { '(not set: the product default, off)' }))
  Write-Host ("log directory: {0}{1}" -f $logDir, $(if (Test-Path -LiteralPath $logDir) { '' } else { ' (does not exist)' }))
  $hosts = @(Get-VerifiedHosts)
  if ($hosts.Count -eq 0) {
    Write-Host 'running Host:  (none)'
    return
  }
  foreach ($host_ in $hosts) {
    $log = Get-LaunchLog $host_
    Write-Host ("running Host:  pid={0} log={1} ({2})" -f $host_.Id,
      $(if ($log.Exists) { $log.Path } else { '(none)' }), $log.Detail)
  }
}

$exitCode = 0
try {
  if ($Status) {
    Show-Status
    exit 0
  }

  if ($Enable) {
    [Environment]::SetEnvironmentVariable($variable, '1', 'User')
    Send-EnvironmentChanged
    Write-Host "Enabled the Host technical log for this user ($variable=1)."
    Write-Host 'Content stays technical only: no key, composition, candidate, document or password text.'
  } else {
    [Environment]::SetEnvironmentVariable($variable, $null, 'User')
    Send-EnvironmentChanged
    Write-Host "Disabled the Host technical log for this user ($variable removed)."
  }

  if ($NoRestart) {
    Write-Host 'The running Host keeps its old setting until it is restarted; run restart-host.ps1 or sign out.'
    Show-Status
    exit 0
  }

  Write-Host ''
  Write-Host '== restarting the Host so the setting reaches it'
  $before = @(Get-VerifiedHosts | ForEach-Object { $_.Id })
  Restart-Host
  $after = @(Get-VerifiedHosts)
  $new = @($after | Where-Object { $before -notcontains $_.Id })
  if ($new.Count -eq 0) { $new = $after }
  if ($new.Count -eq 0) { throw 'no Host is running after the restart, so the setting could not be verified' }

  # Give a freshly started Host a moment to write its startup lines (or to prove it
  # does not write any).
  Start-Sleep -Seconds 2
  $new = @(Get-VerifiedHosts)
  Write-Host ''
  $verified = $true
  foreach ($host_ in $new) {
    $log = Get-LaunchLog $host_
    Write-Host ("  pid={0} log={1} exists={2} from_this_launch={3} ({4})" -f
      $host_.Id, $log.Path, $log.Exists, $log.FromThisLaunch, $log.Detail)
    if ($Enable -and -not $log.FromThisLaunch) { $verified = $false }
    if ($Disable -and $log.FromThisLaunch) { $verified = $false }
  }

  if (-not $verified) {
    if ($Enable) {
      Write-Host 'FAILED: the restarted Host is not writing a log, so the setting did not take effect.' -ForegroundColor Red
      Write-Host '  Possible causes: the launcher kept an older environment block, the process could not create' -ForegroundColor Yellow
      Write-Host '  the file (permissions/disk), or the variable was changed after that process started.' -ForegroundColor Yellow
      Write-Host '  Sign out and sign in, then run -Status to see whether the next Host writes a log.' -ForegroundColor Yellow
    } else {
      Write-Host 'FAILED: the restarted Host still writes a log, so the setting was not applied.' -ForegroundColor Red
      Write-Host '  Check that no other launch path sets the variable (user, machine or process scope), then retry.' -ForegroundColor Yellow
    }
    $exitCode = 1
  } else {
    Write-Host $(if ($Enable) { 'Confirmed: the restarted Host writes its log.' } else { 'Confirmed: the restarted Host writes no log.' })
  }
  Write-Host ''
  Show-Status
} catch {
  $exitCode = 1
  Write-Host ''
  Write-Host ("DIAGNOSTICS SETUP FAILED: {0}" -f $_.Exception.Message) -ForegroundColor Red
} finally {
  if ($Pause) {
    Write-Host ''
    Read-Host 'Press Enter to close this window' | Out-Null
  }
}
exit $exitCode
