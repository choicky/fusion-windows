# R34 B1b probe — which System events does a CANCELLED restart or shutdown write?
#
# Not part of the package and not run by the walkthrough: it needs an interactive owner who
# starts a restart/shutdown and clicks Cancel. It exists so the measurement can be repeated on
# any machine with the repository (the 5700X evidence was collected by hand).
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
# It writes every System event newer than the pre-cancel baseline to
# tools\package\probe-b1b-<action>.txt (or -OutFile).
param(
  [Parameter(Mandatory = $true)][ValidateSet('restart', 'shutdown')][string]$Action,
  [string]$OutFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

. (Join-Path $PSScriptRoot 'walkthrough-assertions.ps1')

if (-not $OutFile) { $OutFile = Join-Path $PSScriptRoot ("probe-b1b-{0}.txt" -f $Action) }
if (Test-Path -LiteralPath $OutFile) { [IO.File]::Delete($OutFile) }
function Log([string]$text) { $text | Tee-Object -FilePath $OutFile -Append | Out-Null; Write-Host $text }

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
Write-Host ''
Read-Host 'Press Enter after you cancelled and are back at the desktop' | Out-Null

Start-Sleep -Seconds 3
$after = Get-SessionIdentity
$hostsAfter = @(Get-FusionHostState)
Log ''
Log ("after: session={0} logon={1} (unchanged={2})" -f $after.SessionId, $after.LogonText,
  (($after.SessionId -eq $session.SessionId) -and ($after.LogonText -eq $session.LogonText)))
Log ("after: FusionHost={0}" -f $(if ($hostsAfter.Count -gt 0) { ($hostsAfter | ForEach-Object { $_.Id }) -join ',' } else { '(none)' }))

Log ''
Log ("=== every System event newer than record {0} ===" -f $baselineRecord)
$events = @()
try {
  $events = @(Get-WinEvent -LogName 'System' -MaxEvents 200 -ErrorAction Stop | Where-Object { [long]$_.RecordId -gt $baselineRecord })
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
Log '=== User32 1074/1076/1077 XML (the candidates for a distinct cancel event) ==='
foreach ($event in ($events | Where-Object { $_.ProviderName -eq 'User32' -and $_.Id -in 1074, 1076, 1077 })) {
  Log ("--- record={0} id={1}" -f $event.RecordId, $event.Id)
  Log ($event.ToXml())
}

Stop-ShutdownBlocker
Log ''
Log ("blocker closed: {0}" -f (-not (Get-ShutdownBlockerState).Alive))
Log ("evidence: {0}" -f $OutFile)
