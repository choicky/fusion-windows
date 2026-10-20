# Stage 6 WP7: manual Host restart for the installed package.
#
# Restarts *this user's* Host in *this* session, reusing the existing lifecycle
# mechanisms instead of adding a second one:
#   1. ask the running Host for an orderly stop through the product's own named
#      stop event (Fcitx saves on instance exit) and wait, bounded, for exactly the
#      verified pids;
#   2. only if one is still alive, force-stop that verified instance and say so;
#   3. start the installed Host through the existing per-user logon task (Medium
#      integrity, interactive token) - never a new start mechanism;
#   4. verify the exact executable path, the integrity level and that exactly one Host
#      from the installed directory is running.
#
# One verified scope is used for both stopping and starting: a Host whose owner cannot
# be read is *not* treated as this user's - it is reported and the tool refuses to act.
# Another user's or session's process is never touched and never waited for.
#
# An absent Host is a normal restart case. Nothing here enables automatic restart or
# changes the task's restart-on-failure setting.
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
$expectedExe = Join-Path $installDir 'bin\FusionHost.exe'
$sessionId = (Get-Process -Id $PID).SessionId
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

function Write-Step([string]$message) { Write-Host "== $message" }

# Every Host in this session. Ownership is whatever the process reports; an empty
# owner is "unverified", never "mine".
function Get-SessionHosts {
  @(Get-FusionHostState | Where-Object { $_.SessionId -eq $sessionId })
}
# Verified this user's Hosts: session and owner both match.
function Get-VerifiedHosts {
  @(Get-SessionHosts | Where-Object { $_.User -eq $me })
}
# Hosts in this session whose owner could not be read (or is someone else).
function Get-UnverifiedHosts {
  @(Get-SessionHosts | Where-Object { $_.User -ne $me })
}
# Other sessions' Hosts: reported, never touched and never waited for.
function Get-OtherSessionHosts {
  @(Get-FusionHostState | Where-Object { $_.SessionId -ne $sessionId })
}

function Format-Host($host_) {
  "pid=$($host_.Id) session=$($host_.SessionId) user=$(if ($host_.User) { $host_.User } else { '(unreadable)' }) integrity=$($host_.Integrity) path=$(if ($host_.Path) { $host_.Path } else { '(unreadable)' })"
}

function Test-ExpectedPath($host_) {
  $host_.Path -and [string]::Equals($host_.Path, $expectedExe, [StringComparison]::OrdinalIgnoreCase)
}

$exitCode = 0
# U7: a manual restart is an intentional stop. The marker tells the tray not to
# report the Host's absence as a failure, and is removed when this run ends - also
# when it is started from the Start Menu shortcut rather than the tray.
$restartMarker = ''
try {
  $restartMarker = Start-FusionRestartMarker
} catch {
  Write-Host "WARNING: no restart marker was written: $($_.Exception.Message)" -ForegroundColor Yellow
}
try {
  if (-not (Get-FusionScheduledTask)) {
    throw "the logon task '$taskName' does not exist; install the package first (install.ps1)"
  }

  $others = @(Get-OtherSessionHosts)
  if ($others.Count -gt 0) {
    Write-Host 'Hosts in another session are left alone (not stopped, not waited for):' -ForegroundColor Yellow
    foreach ($host_ in $others) { Write-Host "  $(Format-Host $host_)" }
  }

  # An unverified owner is not proof that the process is ours; refuse rather than
  # risk another principal's Host.
  $unverified = @(Get-UnverifiedHosts)
  if ($unverified.Count -gt 0) {
    Write-Host 'A Host in this session has no verifiable owner; refusing to stop or restart:' -ForegroundColor Red
    foreach ($host_ in $unverified) { Write-Host "  $(Format-Host $host_)" }
    throw 'cannot verify ownership of a Host in this session; stop it manually or sign out'
  }

  $mine = @(Get-VerifiedHosts)
  if ($mine.Count -eq 0) {
    Write-Step 'no Host is running for this user in this session: starting it'
  } else {
    Write-Step "stopping this user's Host: $(($mine | ForEach-Object { $_.Id }) -join ',')"
    # Only the verified pids are waited for, so another session's Host cannot extend
    # the wait.
    $stop = Stop-FusionHostOrderly -TimeoutMs $TimeoutMs -Pids @($mine | ForEach-Object { $_.Id })
    Write-Host ("  orderly stop: signalled={0} exited={1} after {2} ms" -f $stop.Signalled, $stop.Exited, $stop.WaitedMs)
    if ($stop.Error) { Write-Host "  $($stop.Error)" -ForegroundColor Yellow }
    if (-not $stop.Exited) {
      $alive = @(Get-VerifiedHosts)
      if ($alive.Count -eq 0) {
        Write-Host '  the Host exited before the fallback was needed'
      } else {
        Write-Host ("  FALLBACK: the orderly stop did not finish in {0} ms; force-stopping {1}" -f
          $TimeoutMs, (($alive | ForEach-Object { $_.Id }) -join ',')) -ForegroundColor Yellow
        foreach ($host_ in $alive) { Write-Host "    $(Format-Host $host_)" }
        foreach ($host_ in $alive) { Stop-Process -Id $host_.Id -Force -ErrorAction SilentlyContinue }
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $deadline -and @(Get-VerifiedHosts).Count -gt 0) { Start-Sleep -Milliseconds 100 }
        if (@(Get-VerifiedHosts).Count -gt 0) { throw 'the force-stopped Host did not exit within 10 s' }
        Write-Host '  force-stop completed; Fcitx did not run its own save for that instance' -ForegroundColor Yellow
      }
    }
  }

  Write-Step 'starting the installed Host through the logon task'
  Start-ScheduledTask -TaskName $taskName
  $deadline = (Get-Date).AddSeconds(25)
  $started = @()
  while ((Get-Date) -lt $deadline) {
    $started = @(Get-VerifiedHosts | Where-Object { Test-ExpectedPath $_ })
    if ($started.Count -gt 0) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($started.Count -eq 0) {
    $task = Get-FusionScheduledTask
    throw ("no Host from '{0}' started within 25 s (task last result: {1})" -f $expectedExe,
      $(if ($task) { $task.LastResult } else { 'task not readable' }))
  }
  if ($started.Count -gt 1) {
    throw ("{0} Hosts from '{1}' are running; the single-instance verification failed" -f $started.Count, $expectedExe)
  }
  $host_ = $started[0]
  Write-Host "  started: $(Format-Host $host_)"
  if ($host_.Integrity -ne 'medium') {
    throw ("expected Medium integrity, got {0}" -f $host_.Integrity)
  }
  $extra = @(Get-VerifiedHosts | Where-Object { $_.Id -ne $host_.Id })
  if ($extra.Count -gt 0) {
    throw ("another Host of this user is also running ({0}); the single-instance verification failed" -f
      (($extra | ForEach-Object { $_.Id }) -join ','))
  }

  $logDir = Join-Path $env:LOCALAPPDATA 'fusion-host'
  Write-Host ''
  Write-Host 'Host restarted. User data (%APPDATA%\Fcitx5) was not touched.'
  # The directory's existence is not proof of current logging (it can hold older
  # files); the diagnostics helper reports the state of the running process.
  Write-Host ("Host log directory: {0}{1}" -f $logDir,
    $(if (Test-Path -LiteralPath $logDir) { ' (exists; run host-diagnostics.ps1 -Status for the current state)' } else { ' (not created: diagnostics off)' }))
} catch {
  $exitCode = 1
  Write-Host ''
  Write-Host ("RESTART FAILED: {0}" -f $_.Exception.Message) -ForegroundColor Red
} finally {
  # Before the optional pause: the restart is over, so the tray must stop
  # treating an absence as intentional.
  try { Clear-FusionRestartMarker } catch { }
  if ($Pause) {
    Write-Host ''
    Read-Host 'Press Enter to close this window' | Out-Null
  }
}
exit $exitCode
