# R34 B1b probe — which System events does a CANCELLED restart or shutdown write?
#
# Not part of the package and not run by the walkthrough: it needs an interactive owner who
# starts a restart/shutdown and clicks Cancel. It exists so the measurement can be repeated on
# any machine with the repository (the 5700X evidence was collected by hand on 2026-10-19:
# a cancelled restart and a cancelled shutdown both wrote User32 1073, and the completed
# restart that followed wrote none).
#
# It starts the walkthrough's own shutdown blocker first, so the machine cannot actually
# restart or shut down while it runs: the blocker answers FALSE to WM_QUERYENDSESSION and
# registers a block reason, so Windows shows its "these apps are preventing..." screen and
# waits for the owner.
#
# Usage (from any directory):
#   powershell -ExecutionPolicy Bypass -File tools\package\probe-b1b.ps1 -Action restart
#   powershell -ExecutionPolicy Bypass -File tools\package\probe-b1b.ps1 -Action shutdown
#
# The terminal may be closed by the attempt itself (one of the 2026-10-19 shutdowns killed
# it before the Cancel). The baseline record is written to the output file before the prompt,
# so the events can be collected afterwards from a fresh terminal:
#   powershell -ExecutionPolicy Bypass -File tools\package\probe-b1b.ps1 -CollectOnly -OutFile <that file>
param(
  [ValidateSet('restart', 'shutdown')][string]$Action,
  [string]$OutFile,
  [switch]$CollectOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# Get-FusionHostState lives in host-state.ps1; the blocker and the session/event helpers in
# walkthrough-assertions.ps1.
. (Join-Path $PSScriptRoot 'host-state.ps1')
. (Join-Path $PSScriptRoot 'walkthrough-assertions.ps1')

if ($CollectOnly) {
  if (-not $OutFile) { throw '-CollectOnly needs -OutFile: the file that holds the baseline record.' }
  if (-not (Test-Path -LiteralPath $OutFile)) { throw "no such file: $OutFile" }
} else {
  if (-not $Action) { throw 'give -Action restart or -Action shutdown (or -CollectOnly -OutFile <file>).' }
  if (-not $OutFile) { $OutFile = Join-Path $PSScriptRoot ("probe-b1b-{0}.txt" -f $Action) }
  if (Test-Path -LiteralPath $OutFile) { [IO.File]::Delete($OutFile) }
}
# Appending must use the encoding the file already has: an owner-collected baseline can be
# UTF-16 (Tee-Object writes UTF-16LE), and UTF-8 bytes appended to it would be unreadable.
function Get-FileEncoding([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return (New-Object System.Text.UTF8Encoding($false)) }
  $bytes = [IO.File]::ReadAllBytes($path)
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
    return (New-Object System.Text.UnicodeEncoding($false, $true))
  }
  if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    return (New-Object System.Text.UTF8Encoding($true))
  }
  return (New-Object System.Text.UTF8Encoding($false))
}
$script:encoding = Get-FileEncoding $OutFile
function Log([string]$text) {
  if ($text -eq '') { [IO.File]::AppendAllText($OutFile, [Environment]::NewLine, $script:encoding); Write-Host ''; return }
  [IO.File]::AppendAllText($OutFile, $text + [Environment]::NewLine, $script:encoding)
  Write-Host $text
}

# Every System event newer than the baseline, sorted, with the User32 cancel candidates in
# full XML. Used by both modes so the two files look the same.
function Write-Events([long]$baselineRecord) {
  Log ''
  Log ("=== every System event newer than record {0} ===" -f $baselineRecord)
  $events = @()
  try {
    $events = @(Get-WinEvent -LogName 'System' -MaxEvents 400 -ErrorAction Stop |
        Where-Object { [long]$_.RecordId -gt $baselineRecord })
  } catch {
    Log ("  could not read the System log: {0}" -f $_.Exception.Message)
  }
  if ($events.Count -eq 0) {
    Log '  (none) — a cancelled attempt wrote no System event on this machine'
  }
  foreach ($event in ($events | Sort-Object RecordId)) {
    Log ("  record={0} {1:HH:mm:ss} {2} id={3} user={4}" -f $event.RecordId, $event.TimeCreated, $event.ProviderName, $event.Id, $event.UserId)
    Log ("    {0}" -f (($event.Message -split "`n")[0]))
  }
  Log ''
  Log '=== User32 1073/1074/1076/1077 XML (the cancel candidates) ==='
  foreach ($event in ($events | Where-Object { $_.ProviderName -eq 'User32' -and $_.Id -in 1073, 1074, 1076, 1077 })) {
    Log ("--- record={0} id={1}" -f $event.RecordId, $event.Id)
    Log ($event.ToXml())
  }
}

if ($CollectOnly) {
  # The attempt killed the terminal before it could read the log. The baseline is the first
  # line the run wrote, so the collection needs nothing but this file.
  $text = Get-Content -LiteralPath $OutFile -Raw
  $match = [regex]::Match($text, 'baseline record\s*:\s*(\d+)')
  if (-not $match.Success) { throw "no 'baseline record' line in $OutFile" }
  $baselineRecord = [long]$match.Groups[1].Value
  # The first run's last line may not end with a newline (Tee-Object leaves it out).
  $existing = Get-Content -LiteralPath $OutFile -Raw
  if ($existing -and -not $existing.EndsWith("`n")) {
    [IO.File]::AppendAllText($OutFile, [Environment]::NewLine, $script:encoding)
  }
  $session = Get-SessionIdentity
  $hosts = @(Get-FusionHostState)
  Log ''
  Log '=== collected after the attempt (the first terminal did not survive) ==='
  Log ("  time            : {0:yyyy-MM-dd HH:mm:ss}" -f (Get-Date))
  Log ("  machine         : {0}" -f $env:COMPUTERNAME)
  Log ("  baseline record : {0} (from the file)" -f $baselineRecord)
  Log ("  session now     : {0} logon={1}" -f $session.SessionId, $session.LogonText)
  Log ("  FusionHost now  : {0}" -f $(if ($hosts.Count -gt 0) { ($hosts | ForEach-Object { $_.Id }) -join ',' } else { '(none)' }))
  Write-Events $baselineRecord
  Log ("evidence: {0}" -f $OutFile)
  return
}

# The baseline is the newest System record before the attempt: everything newer is the
# attempt's own record, whatever its provider or id.
$baseline = Get-WinEvent -LogName 'System' -MaxEvents 1 -ErrorAction Stop
$baselineRecord = [long]$baseline.RecordId
$session = Get-SessionIdentity
$hostsBefore = @(Get-FusionHostState)

Log ("R34 B1b probe: cancelled {0}" -f $Action)
Log ("  time            : {0:yyyy-MM-dd HH:mm:ss}" -f (Get-Date))
Log ("  machine         : {0}" -f $env:COMPUTERNAME)
Log ("  baseline record : {0} ({1:yyyy-MM-dd HH:mm:ss}, {2} id={3})" -f $baselineRecord, $baseline.TimeCreated, $baseline.ProviderName, $baseline.Id)
Log ("  session         : {0} logon={1}" -f $session.SessionId, $session.LogonText)
Log ("  FusionHost before: {0}" -f $(if ($hostsBefore.Count -gt 0) { ($hostsBefore | ForEach-Object { $_.Id }) -join ',' } else { '(none)' }))

$title = "R34 probe: cancel the $Action"
$blocker = Start-ShutdownBlocker -Title $title -Reason $title
Log ("  blocker         : started={0} alive={1} reason='{2}'" -f $blocker.Started, $blocker.State.Alive, $blocker.State.Reason)
if (-not $blocker.Started -or -not $blocker.State.BlockRegistered) {
  Stop-ShutdownBlocker
  throw 'the blocker did not register its block reason; nothing was attempted'
}

Write-Host ''
Write-Host "Now start the $Action and CANCEL it:" -ForegroundColor Cyan
if ($Action -eq 'restart') {
  Write-Host '  Start menu -> power button -> Restart.'
} else {
  Write-Host '  Start menu -> power button -> Shut down.'
}
Write-Host "  Windows shows the blocking screen with '$title' in the list."
Write-Host '  Click Cancel (do NOT click "restart anyway" / "shut down anyway").'
Write-Host '  This terminal may close even though the Cancel worked; if it does, run the'
Write-Host ("  -CollectOnly command printed below in a new terminal.") -ForegroundColor Yellow
Write-Host ''
Write-Host ("  powershell -ExecutionPolicy Bypass -File `"{0}`" -CollectOnly -OutFile `"{1}`"" -f $PSCommandPath, $OutFile)
Write-Host ''
Read-Host 'Press Enter after you cancelled and are back at the desktop' | Out-Null

Start-Sleep -Seconds 3
$after = Get-SessionIdentity
$hostsAfter = @(Get-FusionHostState)
Log ''
Log ("after: session={0} logon={1} (unchanged={2})" -f $after.SessionId, $after.LogonText,
  (($after.SessionId -eq $session.SessionId) -and ($after.LogonText -eq $session.LogonText)))
Log ("after: FusionHost={0}" -f $(if ($hostsAfter.Count -gt 0) { ($hostsAfter | ForEach-Object { $_.Id }) -join ',' } else { '(none)' }))

Write-Events $baselineRecord

Stop-ShutdownBlocker
Log ''
Log ("blocker closed: {0}" -f (-not (Get-ShutdownBlockerState).Alive))
Log ("evidence: {0}" -f $OutFile)
