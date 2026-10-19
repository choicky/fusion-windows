# Stage 6 WP7: explicit opt-in / opt-out for the Host technical log (R30).
#
# The product default stays OFF. This helper turns the log on for *this user* only,
# through the same environment variable the Host reads at startup
# (FUSION_HOST_DIAGNOSTICS = 1), and then restarts the Host so the setting actually
# reaches the process that writes the log. It verifies that the launch path received
# it by looking for the new process's log file; a launcher that keeps an old
# environment block is reported instead of assumed to work.
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
function Get-MyHosts {
  @(Get-FusionHostState | Where-Object {
      $_.SessionId -eq $sessionId -and ($_.User -eq '' -or $_.User -eq $me)
    })
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
  Write-Host ("log directory: {0}" -f $logDir)
  if (Test-Path -LiteralPath $logDir) {
    $newest = Get-ChildItem -LiteralPath $logDir -Filter 'fusion-host-*.log' -File -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest) {
      Write-Host ("newest log:    {0} ({1:yyyy-MM-dd HH:mm:ss}, {2} bytes)" -f $newest.Name, $newest.LastWriteTime, $newest.Length)
    } else {
      Write-Host 'newest log:    (none)'
    }
  } else {
    Write-Host 'newest log:    (no directory: the log has never been enabled for this user)'
  }
  $hosts = @(Get-MyHosts)
  if ($hosts.Count -gt 0) {
    foreach ($host_ in $hosts) {
      $file = Join-Path $logDir ("fusion-host-{0}.log" -f $host_.Id)
      Write-Host ("running Host:  pid={0} log-for-this-pid={1}" -f $host_.Id, (Test-Path -LiteralPath $file))
    }
  } else {
    Write-Host 'running Host:  (none)'
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
  } else {
    Write-Host ''
    Write-Host '== restarting the Host so the setting reaches it'
    $before = @(Get-MyHosts | ForEach-Object { $_.Id })
    Restart-Host
    $after = @(Get-MyHosts)
    $new = @($after | Where-Object { $before -notcontains $_.Id })
    if ($new.Count -eq 0) { $new = $after }
    Write-Host ''
    if ($Enable) {
      $verified = $false
      foreach ($host_ in $new) {
        $file = Join-Path $logDir ("fusion-host-{0}.log" -f $host_.Id)
        $exists = Test-Path -LiteralPath $file
        Write-Host ("  pid={0} expected log {1}: {2}" -f $host_.Id, $file, $exists)
        if ($exists) { $verified = $true }
      }
      if (-not $verified) {
        Write-Host 'WARNING: the restarted Host wrote no log. Its launch path did not receive the setting' -ForegroundColor Yellow
        Write-Host '         (a launcher with a cached environment block). Sign out and sign in, then run -Status.' -ForegroundColor Yellow
        $exitCode = 1
      }
    } else {
      foreach ($host_ in $new) {
        $file = Join-Path $logDir ("fusion-host-{0}.log" -f $host_.Id)
        Write-Host ("  pid={0} log for this pid exists: {1} (it will not grow)" -f $host_.Id, (Test-Path -LiteralPath $file))
      }
    }
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
