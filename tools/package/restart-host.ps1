# Stage 6 WP7: manual Host restart for the installed package.
#
# Restarts *this user's* Host in *this* session, reusing the existing lifecycle
# mechanisms instead of adding a second one:
#   1. ask the running Host for an orderly stop through the product's own named
#      stop event (Fcitx saves on instance exit);
#   2. wait, bounded;
#   3. only if it is still alive, force-stop the verified current-user/current-session
#      FusionHost instance and say so clearly;
#   4. start the installed Host through the existing per-user logon task (Medium
#      integrity, interactive token) - never a new start mechanism;
#   5. verify the executable identity, the integrity level and that exactly one Host
#      from the installed directory is running.
#
# An absent Host is a normal restart case. Nothing here enables automatic restart or
# changes the task's restart-on-failure setting, and no other user's or session's
# process is ever touched.
#
# Not elevated: the stop event and the task belong to the installing user.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File restart-host.ps1 [-TimeoutMs 20000] [-Pause]

param(
  [int]$TimeoutMs = 20000,
  [switch]$Pause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$taskName = 'FusionHost'
$installDir = $PSScriptRoot
$sessionId = (Get-Process -Id $PID).SessionId
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

function Write-Step([string]$message) { Write-Host "== $message" }

function Get-MyHosts {
  @(Get-FusionHostState | Where-Object {
      $_.SessionId -eq $sessionId -and ($_.User -eq '' -or $_.User -eq $me)
    })
}

function Get-ForeignHosts {
  @(Get-FusionHostState | Where-Object {
      -not ($_.SessionId -eq $sessionId -and ($_.User -eq '' -or $_.User -eq $me))
    })
}

function Format-Host($host_) {
  "pid=$($host_.Id) session=$($host_.SessionId) user=$($host_.User) integrity=$($host_.Integrity) path=$($host_.Path)"
}

$exitCode = 0
try {
  if (-not (Get-FusionScheduledTask)) {
    throw "the logon task '$taskName' does not exist; install the package first (install.ps1)"
  }

  $foreign = @(Get-ForeignHosts)
  if ($foreign.Count -gt 0) {
    Write-Host 'Another user''s or session''s Host is running and will not be touched:' -ForegroundColor Yellow
    foreach ($host_ in $foreign) { Write-Host "  $(Format-Host $host_)" }
  }

  $mine = @(Get-MyHosts)
  if ($mine.Count -eq 0) {
    Write-Step 'no Host is running for this user in this session: starting it'
  } else {
    Write-Step "stopping this user's Host: $(($mine | ForEach-Object { $_.Id }) -join ',')"
    $stop = Stop-FusionHostOrderly -TimeoutMs $TimeoutMs
    Write-Host ("  orderly stop: signalled={0} exited={1} after {2} ms" -f $stop.Signalled, $stop.Exited, $stop.WaitedMs)
    if ($stop.Error) { Write-Host "  $($stop.Error)" -ForegroundColor Yellow }
    if (-not $stop.Exited) {
      # Fallback, clearly reported: only the pids verified as this user's and this
      # session's, never a name-wide kill.
      $alive = @(Get-MyHosts)
      if ($alive.Count -eq 0) {
        Write-Host '  the Host exited before the fallback was needed'
      } else {
        Write-Host ("  FALLBACK: the orderly stop did not finish in {0} ms; force-stopping {1}" -f
          $TimeoutMs, (($alive | ForEach-Object { $_.Id }) -join ',')) -ForegroundColor Yellow
        foreach ($host_ in $alive) { Write-Host "    $(Format-Host $host_)" }
        foreach ($host_ in $alive) { Stop-Process -Id $host_.Id -Force -ErrorAction SilentlyContinue }
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $deadline -and @(Get-MyHosts).Count -gt 0) { Start-Sleep -Milliseconds 100 }
        if (@(Get-MyHosts).Count -gt 0) { throw 'the force-stopped Host did not exit within 10 s' }
        Write-Host '  force-stop completed; Fcitx did not run its own save for that instance' -ForegroundColor Yellow
      }
    }
  }

  Write-Step 'starting the installed Host through the logon task'
  Start-ScheduledTask -TaskName $taskName
  $deadline = (Get-Date).AddSeconds(25)
  $started = @()
  while ((Get-Date) -lt $deadline) {
    $started = @(Get-MyHosts | Where-Object {
        $_.Path -and $_.Path.StartsWith($installDir, [StringComparison]::OrdinalIgnoreCase)
      })
    if ($started.Count -gt 0) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($started.Count -eq 0) {
    $task = Get-FusionScheduledTask
    throw ("no Host from '{0}' started within 25 s (task last result: {1})" -f $installDir,
      $(if ($task) { $task.LastResult } else { 'task not readable' }))
  }
  if ($started.Count -gt 1) {
    throw ("{0} Hosts from '{1}' are running; expected exactly one" -f $started.Count, $installDir)
  }
  $host_ = $started[0]
  Write-Host "  started: $(Format-Host $host_)"
  if ($host_.Integrity -ne 'medium') {
    throw ("expected Medium integrity, got {0}" -f $host_.Integrity)
  }
  $otherMine = @(Get-MyHosts | Where-Object { $_.Id -ne $host_.Id })
  if ($otherMine.Count -gt 0) {
    Write-Host ("  WARNING: another Host of this user is also running: {0}" -f
      (($otherMine | ForEach-Object { $_.Id }) -join ',')) -ForegroundColor Yellow
  }

  $logDir = Join-Path $env:LOCALAPPDATA 'fusion-host'
  Write-Host ''
  Write-Host 'Host restarted. User data (%APPDATA%\Fcitx5) was not touched.'
  Write-Host ("Host log: {0}{1}" -f $logDir,
    $(if (Test-Path -LiteralPath $logDir) { ' (diagnostics enabled)' } else { ' (diagnostics off: set FUSION_HOST_DIAGNOSTICS=1)' }))
} catch {
  $exitCode = 1
  Write-Host ''
  Write-Host ("RESTART FAILED: {0}" -f $_.Exception.Message) -ForegroundColor Red
} finally {
  if ($Pause) {
    Write-Host ''
    Read-Host 'Press Enter to close this window' | Out-Null
  }
}
exit $exitCode
