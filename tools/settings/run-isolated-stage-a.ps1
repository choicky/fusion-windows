param(
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [Parameter(Mandatory = $true)][string]$ProfileRoot,
  [Parameter(Mandatory = $true)][string]$PipeName,
  [string]$OutFile = ''
)

# Stage A closure checks against a real, isolated Host.
#
# One disposable profile, one isolated pipe, two consumers:
#   1. stage4a integration — the real-engine assertions (Pinyin keys, the commit,
#      the projected candidate). It types, so it runs only here, against a Host
#      this script started, with the endpoint passed explicitly.
#   2. FusionSettings --self-test — global apply, Global Cancel/Reload, the
#      input-method list and the no-change pass.
#
# Around and between them the script asserts the two things the evidence pack
# claimed but had not shown: configuration the apply does not submit survives, and
# the engine's learning files are not rewritten by a settings-only session. It also
# proves the safe default: with no endpoint the integration mode skips, and this
# user's production endpoint is refused in both spellings before any request.
#
# Isolation is applied to the Host itself: APPDATA/LOCALAPPDATA/USERPROFILE are
# redirected in this process, so every child inherits the disposable profile. The
# production Host is never addressed — only the named stop event of this scope is
# signalled, and the production pipe name is rejected as a parameter.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

trap {
  [Console]::Error.WriteLine("HARNESS ERROR line={0}: {1}" -f `
    $_.InvocationInfo.ScriptLineNumber, $_.Exception.Message)
  exit 1
}

# The real profile, read-only, before anything is redirected: the refusal checks
# below must not be able to write to it.
$realPinyinDir = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'fcitx5\pinyin'

$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$ProfileRoot = [IO.Path]::GetFullPath($ProfileRoot)
New-Item -ItemType Directory -Force $ProfileRoot | Out-Null

if ($PipeName -match '^FusionHost-v1-') {
  throw "Refusing '$PipeName': that is the product pipe identity, and a Host started on it would collide with the installed one. Pass an isolated name."
}
if ($PipeName -match '[\\/:*?"<>|\x00-\x1f]') {
  throw "Refusing '$PipeName': not a single pipe name component."
}

$bin = Join-Path $BuildDir 'bin'
$hostExe = Join-Path $bin 'FusionHost.exe'
$settingsExe = Join-Path $bin 'FusionSettings.exe'
$stage4aExe = Join-Path $bin 'stage4a.exe'
foreach ($path in @($hostExe, $settingsExe, $stage4aExe, (Join-Path $bin 'FusionFcitxHost.dll'))) {
  if (-not (Test-Path -LiteralPath $path)) { throw "Missing isolated product binary: $path" }
}

$env:APPDATA = Join-Path $ProfileRoot 'AppData\Roaming'
$env:LOCALAPPDATA = Join-Path $ProfileRoot 'AppData\Local'
$env:USERPROFILE = Join-Path $ProfileRoot 'User'
New-Item -ItemType Directory -Force $env:APPDATA, $env:LOCALAPPDATA, $env:USERPROFILE | Out-Null
$env:PATH = "$bin;$env:PATH"
$env:FUSION_HOST_DIAGNOSTICS = '1'

$pipe = "\\.\pipe\$PipeName"
$stopScope = "stage-a-$PipeName"
$stopEventName = "Local\FusionHost-stop-$stopScope"

Add-Type -Namespace StageAClosure -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr OpenEventW(uint access, bool inherit, string name);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool SetEvent(System.IntPtr handle);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(System.IntPtr handle);
'@

# Fcitx resolves its user directory from the profile, not from APPDATA: the files
# this script seeds and compares live under %USERPROFILE%\AppData\Roaming\Fcitx5.
$fcitxRoot = Join-Path $env:USERPROFILE 'AppData\Roaming\Fcitx5'
$configDir = Join-Path $fcitxRoot 'config\fcitx5'
$globalConfig = Join-Path $configDir 'config'
$pinyinConfig = Join-Path $configDir 'conf\pinyin.conf'
$learningDir = Join-Path $fcitxRoot 'pinyin'

function Invoke-Bounded {
  param(
    [string]$Exe,
    [string]$Arguments,
    [string]$IntegrationEndpoint = '',
    [int]$TimeoutMs = 60000
  )
  # The child inherits this process's environment. An empty value is how this
  # script says "unset": the integration mode treats an empty
  # FUSION_STAGE4A_INTEGRATION_PIPE exactly like a missing one, which is the case
  # the safe-default check needs.
  $previous = $env:FUSION_STAGE4A_INTEGRATION_PIPE
  $env:FUSION_STAGE4A_INTEGRATION_PIPE = $IntegrationEndpoint
  try {
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.Arguments = $Arguments
    $psi.WorkingDirectory = $BuildDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $proc = [Diagnostics.Process]::Start($psi)
    $stderr = $proc.StandardError.ReadToEndAsync()
    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $finished = $proc.WaitForExit($TimeoutMs)
    if (-not $finished) {
      $proc.Kill()
      $proc.WaitForExit(5000)
    }
    $sw.Stop()
    return @{
      ExitCode  = $(if ($finished) { $proc.ExitCode } else { -1 })
      TimedOut  = (-not $finished)
      ElapsedMs = $sw.ElapsedMilliseconds
      Stdout    = $stdout.Result.Trim()
      Stderr    = $stderr.Result.Trim()
    }
  } finally {
    $env:FUSION_STAGE4A_INTEGRATION_PIPE = $previous
  }
}

function Start-IsolatedHost {
  $psi = [Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = $hostExe
  $psi.Arguments = "--console --pipe-name `"$pipe`" --stop-event-scope `"$stopScope`""
  $psi.WorkingDirectory = $BuildDir
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $proc = [Diagnostics.Process]::Start($psi)
  # Readiness is taken from the Host's own diagnostic log rather than from a probe
  # connection: the Host listens on one instance at a time, so a probe would consume
  # it and leave the consumer that follows with nothing to connect to.
  $logPath = Join-Path (Join-Path $env:LOCALAPPDATA 'fusion-host') "fusion-host-$($proc.Id).log"
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) { throw "FusionHost exited early with code $($proc.ExitCode)" }
    if (Test-Path -LiteralPath $logPath) {
      if (Select-String -LiteralPath $logPath -Pattern 'accept_loop' -Quiet -ErrorAction SilentlyContinue) {
        Start-Sleep -Milliseconds 150
        return $proc
      }
    }
    Start-Sleep -Milliseconds 50
  }
  throw "FusionHost did not reach its accept loop (expected $logPath)"
}

function Stop-IsolatedHostOrderly {
  param([Diagnostics.Process]$Process)
  $handle = [StageAClosure.Native]::OpenEventW(0x0002, $false, $stopEventName)
  if ($handle -eq [IntPtr]::Zero) { throw "cannot open the isolated stop event $stopEventName" }
  try {
    if (-not [StageAClosure.Native]::SetEvent($handle)) { throw 'cannot signal the isolated stop event' }
  } finally {
    [void][StageAClosure.Native]::CloseHandle($handle)
  }
  if (-not $Process.WaitForExit(30000)) {
    $Process.Kill()
    $Process.WaitForExit(5000)
    return @{ ExitCode = -1; TimedOut = $true }
  }
  Start-Sleep -Milliseconds 300
  return @{ ExitCode = $Process.ExitCode; TimedOut = $false }
}

function Get-FileFingerprint {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { return @{ Exists = $false; Length = 0; Sha256 = '' } }
  $item = Get-Item -LiteralPath $Path
  return @{
    Exists = $true
    Length = $item.Length
    Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
  }
}

# INI value reader: the seeded keys are plain `Key=Value` lines in one section.
function Get-IniValue {
  param([string]$Path, [string]$Section, [string]$Key)
  if (-not (Test-Path -LiteralPath $Path)) { return $null }
  $current = ''
  foreach ($line in Get-Content -LiteralPath $Path) {
    if ($line -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
    if ($current -ne $Section) { continue }
    if ($line -match "^\s*$([regex]::Escape($Key))\s*=\s*(.*)$") { return $Matches[1].Trim() }
  }
  return $null
}

$result = [ordered]@{}
$result.pipe = $PipeName
$result.profileRoot = $ProfileRoot

# ---- seed: configuration the apply must not submit, plus an unknown key --------
# Both are written before any Host starts, so their survival spans every save this
# run performs. ShowInputMethodInformation and SpellEnabled are schema-known and
# non-default; no step in the self-test changes them, which is what "unexposed by
# this apply" means. FusionStageAUnknownProbe is not in any schema.
New-Item -ItemType Directory -Force (Split-Path -Parent $pinyinConfig) | Out-Null
# DefaultPageSize starts away from the value the self-test applies, so that apply is
# a real write rather than an empty delta. The files are written without a BOM:
# Fcitx parses the first line as the first section header.
[IO.File]::WriteAllText($globalConfig, (@(
  '[Behavior]',
  'DefaultPageSize=5',
  'ShowInputMethodInformation=False',
  'FusionStageAUnknownProbe=keep-me'
) -join "`r`n") + "`r`n")
[IO.File]::WriteAllText($pinyinConfig, (@(
  'PageSize=8',
  'SpellEnabled=False',
  'FusionStageAUnknownProbe=keep-me'
) -join "`r`n") + "`r`n")
$result.seededGlobalUnknownKey = (Get-IniValue -Path $globalConfig -Section 'Behavior' -Key 'FusionStageAUnknownProbe')
$result.seededGlobalKnownValue = (Get-IniValue -Path $globalConfig -Section 'Behavior' -Key 'ShowInputMethodInformation')
$result.seededPinyinKnownValue = (Get-IniValue -Path $pinyinConfig -Section '' -Key 'SpellEnabled')

# ---- safe default and refusal, before any Host is started ---------------------
# 1. No endpoint: the mode must skip without connecting.
$result.missingEndpoint = Invoke-Bounded -Exe $stage4aExe -Arguments 'integration' `
  -IntegrationEndpoint '' -TimeoutMs 20000
$result.missingEndpointSkipped = ($result.missingEndpoint.ExitCode -eq 0 -and
  $result.missingEndpoint.Stdout -like 'SKIP integration*')

# 2. This user's production endpoint, in both spellings, must be refused before any
#    request. The owner's learning files are fingerprinted around the attempt: if
#    the guard ever stopped working, this run would have typed into them.
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$session = (Get-Process -Id $PID).SessionId
$productionLogical = "FusionHost-v1-$sid-$session"
$before = @{}
foreach ($name in @('user.dict', 'user.history')) {
  $before[$name] = Get-FileFingerprint -Path (Join-Path $realPinyinDir $name)
}
$result.productionRefusalLogical = Invoke-Bounded -Exe $stage4aExe -Arguments 'integration' `
  -IntegrationEndpoint $productionLogical -TimeoutMs 20000
$result.productionRefusalFullPath = Invoke-Bounded -Exe $stage4aExe -Arguments 'integration' `
  -IntegrationEndpoint "\\.\pipe\$productionLogical" -TimeoutMs 20000
$result.malformedEndpointRefusal = Invoke-Bounded -Exe $stage4aExe -Arguments 'integration' `
  -IntegrationEndpoint 'bad\name' -TimeoutMs 20000
$untouched = $true
foreach ($name in @('user.dict', 'user.history')) {
  $after = Get-FileFingerprint -Path (Join-Path $realPinyinDir $name)
  if ($after.Sha256 -ne $before[$name].Sha256 -or $after.Length -ne $before[$name].Length) {
    $untouched = $false
  }
}
$refused = { param($r) $r.ExitCode -ne 0 -and $r.Stderr -like '*refusing the production endpoint*' }
$result.productionEndpointRefused = ((& $refused $result.productionRefusalLogical) -and
  (& $refused $result.productionRefusalFullPath))
$result.productionLearningUntouched = $untouched
$result.malformedEndpointRefused = ($result.malformedEndpointRefusal.ExitCode -ne 0 -and
  $result.malformedEndpointRefusal.Stderr -like '*not a single pipe name*')

# ---- phase 1: real-engine integration against the isolated Host ---------------
$host1 = $null
try {
  $host1 = Start-IsolatedHost
  $result.integration = Invoke-Bounded -Exe $stage4aExe -Arguments 'integration' `
    -IntegrationEndpoint $PipeName -TimeoutMs 120000
} finally {
  if ($host1 -and -not $host1.HasExited) {
    $stop = Stop-IsolatedHostOrderly -Process $host1
    $result.integrationHostExit = $stop.ExitCode
    $result.integrationHostStoppedInTime = (-not $stop.TimedOut)
  }
}

# ---- learning fixtures: produced by that run, recorded before the settings pass -
# The engine wrote these while committing; they are the valid fixtures. A synthetic
# dictionary is not used, and the owner's files are never read as data.
$learningBefore = [ordered]@{}
foreach ($name in @('user.dict', 'user.history')) {
  $learningBefore[$name] = Get-FileFingerprint -Path (Join-Path $learningDir $name)
}
$result.learningBefore = $learningBefore

# ---- phase 2: settings session — global apply, Cancel/Reload, list, no-change --
$host2 = $null
try {
  $host2 = Start-IsolatedHost
  $result.selfTest = Invoke-Bounded -Exe $settingsExe `
    -Arguments "--pipe-name `"$pipe`" --self-test" -TimeoutMs 120000
} finally {
  if ($host2 -and -not $host2.HasExited) {
    $stop = Stop-IsolatedHostOrderly -Process $host2
    $result.selfTestHostExit = $stop.ExitCode
    $result.selfTestHostStoppedInTime = (-not $stop.TimedOut)
  }
}

# ---- after: learning files and configuration, compared at the same point ------
$learningAfter = [ordered]@{}
foreach ($name in @('user.dict', 'user.history')) {
  $learningAfter[$name] = Get-FileFingerprint -Path (Join-Path $learningDir $name)
}
$result.learningAfter = $learningAfter
$result.learningDictByteIdentical =
  ($learningBefore['user.dict'].Sha256 -eq $learningAfter['user.dict'].Sha256)
$result.learningHistoryByteIdentical =
  ($learningBefore['user.history'].Sha256 -eq $learningAfter['user.history'].Sha256)
$result.learningPresentAfter = ($learningAfter['user.dict'].Exists -and
  $learningAfter['user.history'].Exists)

$result.globalUnknownKeyAfter = (Get-IniValue -Path $globalConfig -Section 'Behavior' -Key 'FusionStageAUnknownProbe')
$result.globalKnownValueAfter = (Get-IniValue -Path $globalConfig -Section 'Behavior' -Key 'ShowInputMethodInformation')
$result.globalPageSizeAfter = (Get-IniValue -Path $globalConfig -Section 'Behavior' -Key 'DefaultPageSize')
$result.pinyinKnownValueAfter = (Get-IniValue -Path $pinyinConfig -Section '' -Key 'SpellEnabled')
$result.pinyinUnknownKeyAfter = (Get-IniValue -Path $pinyinConfig -Section '' -Key 'FusionStageAUnknownProbe')

$result.globalUnknownKeyPreserved = ($result.globalUnknownKeyAfter -eq 'keep-me')
$result.pinyinUnknownKeyPreserved = ($result.pinyinUnknownKeyAfter -eq 'keep-me')
$result.globalKnownUnsubmittedPreserved = ($result.globalKnownValueAfter -eq 'False')
$result.pinyinKnownUnsubmittedPreserved = ($result.pinyinKnownValueAfter -eq 'False')

$json = $result | ConvertTo-Json -Depth 6
if ($OutFile) {
  [IO.File]::WriteAllText($OutFile, $json)
}
$json
