# Stage 6 WP4 walkthrough assertions and argument formatting, shared by
# verify-wp4.ps1 and the harness proof so both exercise the same code.
#
# Dot-source host-state.ps1 first: Test-FusionHostFrom uses [FusionNative].
#
# The lesson behind this file: a *profile name* ("Fusion Stage 2.5/3B") never
# identifies a build. Only the loaded module path plus its SHA-256 do, so every
# typing question is preceded by an assertion on the module the application really
# loaded and on the Host that serves it.

Set-StrictMode -Version 2.0

# Builds a PowerShell argument list from a hashtable. Parameter NAMES are emitted
# bare and only VALUES are quoted: a quoted '-PackageDir' is a string, so PowerShell
# binds it positionally and the real value is lost (that is exactly how the 11:55
# walkthrough turned into "package.json not found in -PackageDir"). A [bool] true is
# a switch (bare name), false is omitted.
function Format-ArgumentList {
  param([hashtable]$Parameters)
  $parts = @()
  foreach ($name in @($Parameters.Keys | Sort-Object)) {
    $value = $Parameters[$name]
    if ($value -is [bool]) {
      if ($value) { $parts += "-$name" }
      continue
    }
    $parts += "-$name '" + ([string]$value -replace "'", "''") + "'"
  }
  return ($parts -join ' ')
}

# The SHA-256 a package manifest records for one file, or $null.
function Get-ManifestFileHash {
  param([string]$ManifestPath, [string]$RelativePath)
  if (-not (Test-Path -LiteralPath $ManifestPath)) { return $null }
  $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
  $entry = $manifest.files | Where-Object { $_.path -eq $RelativePath } | Select-Object -First 1
  if (-not $entry) { return $null }
  return [string]$entry.sha256
}

# The running Host that belongs to this version directory, this user and this
# session, or $null. Path prefix is compared case-insensitively against
# `<VersionDir>\bin\`.
function Test-FusionHostFrom {
  param([string]$VersionDir)
  $binPrefix = (Join-Path $VersionDir 'bin') + '\'
  $session = (Get-Process -Id $PID).SessionId
  foreach ($process in @(Get-Process -Name FusionHost -ErrorAction SilentlyContinue)) {
    $path = $null
    try { $path = $process.Path } catch { }
    if (-not $path) { continue }
    if (-not $path.StartsWith($binPrefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
    if ($process.SessionId -ne $session) { continue }
    $user = ''
    try {
      $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)" -ErrorAction Stop
      if ($cim) {
        $who = Invoke-CimMethod -InputObject $cim -MethodName GetOwner -ErrorAction Stop
        if ($who -and $who.User) { $user = "$($who.Domain)\$($who.User)" }
      }
    } catch { }
    return [pscustomobject]@{
      Id        = $process.Id
      Path      = $path
      SessionId = $process.SessionId
      Integrity = [FusionNative]::IntegrityLevel($process.Id)
      User      = $user
      Owner     = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    }
  }
  return $null
}

# Does HKLM InprocServer32 point at exactly this DLL, with no HKCU override?
function Test-RegistrationPointsTo {
  param([string]$DllPath)
  $clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
  $hklmKey = "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
  $hkcuKey = "HKCU:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
  $hklm = if (Test-Path -LiteralPath $hklmKey) { [string](Get-Item -LiteralPath $hklmKey).GetValue('') } else { '' }
  $hkcu = if (Test-Path -LiteralPath $hkcuKey) { [string](Get-Item -LiteralPath $hkcuKey).GetValue('') } else { '' }
  $reason = ''
  if (-not $hklm) {
    $reason = 'HKLM InprocServer32 is absent'
  } elseif (-not $hklm.Equals($DllPath, [StringComparison]::OrdinalIgnoreCase)) {
    $reason = "HKLM InprocServer32 points at $hklm, expected $DllPath"
  } elseif ($hkcu) {
    $reason = "HKCU InprocServer32 overrides HKLM with $hkcu"
  }
  return [pscustomobject]@{
    Ok     = [string]::IsNullOrEmpty($reason)
    Hklm   = $hklm
    Hkcu   = $hkcu
    Reason = $reason
  }
}

# Which FusionTsf.dll a process has loaded, and does it match the expected path and
# hash? Reads the real module list, so a stale DLL cannot pass.
function Test-NotepadLoadedDll {
  param([int]$ProcessId, [string]$DllPath, [string]$Sha256)
  $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  if (-not $process) {
    return [pscustomobject]@{ Ok = $false; Loaded = ''; Sha256 = ''; Reason = "process $ProcessId is gone" }
  }
  $loaded = ''
  try {
    foreach ($module in $process.Modules) {
      if ($module.ModuleName -eq 'FusionTsf.dll') { $loaded = $module.FileName; break }
    }
  } catch {
    return [pscustomobject]@{ Ok = $false; Loaded = ''; Sha256 = ''; Reason = "module list unavailable: $($_.Exception.Message)" }
  }
  if (-not $loaded) {
    return [pscustomobject]@{ Ok = $false; Loaded = ''; Sha256 = ''; Reason = 'FusionTsf.dll is not loaded in the process' }
  }
  $hash = ''
  if (Test-Path -LiteralPath $loaded) { $hash = (Get-FileHash -LiteralPath $loaded -Algorithm SHA256).Hash }
  $reason = ''
  if (-not $loaded.Equals($DllPath, [StringComparison]::OrdinalIgnoreCase)) {
    $reason = "loaded $loaded, expected $DllPath"
  } elseif ($Sha256 -and $hash -ne $Sha256) {
    $reason = "loaded sha256 $hash, manifest says $Sha256"
  }
  return [pscustomobject]@{ Ok = [string]::IsNullOrEmpty($reason); Loaded = $loaded; Sha256 = $hash; Reason = $reason }
}

# Starts a Notepad and waits until its module list is readable.
function Start-NotepadForCheck {
  $process = [System.Diagnostics.Process]::Start('notepad.exe')
  for ($attempt = 0; $attempt -lt 40; $attempt++) {
    Start-Sleep -Milliseconds 250
    try {
      if ($process.Modules.Count -gt 0) { break }
    } catch {
      # The module list is not ready yet.
    }
  }
  return $process
}

# The whole item-3 assertion: the Host that serves typing comes from the expected
# version directory, the registration points at that version's DLL, and a Notepad
# really loaded that DLL (path + manifest hash).
#
# `NotepadId` reuses a Notepad the caller already started (step d keeps the old one
# open on purpose); otherwise a fresh Notepad is started here.
function Test-InstalledTipInUse {
  param(
    [string]$VersionDir,
    [string]$ManifestPath,
    [int]$NotepadId = 0
  )
  $failures = @()

  $hostProcess = Test-FusionHostFrom -VersionDir $VersionDir
  if (-not $hostProcess) {
    $failures += "no FusionHost is running from $VersionDir\bin"
  } else {
    if ($hostProcess.Integrity -ne 'medium') {
      $failures += "Host integrity is $($hostProcess.Integrity), expected medium"
    }
    if ($hostProcess.User -and $hostProcess.User -ne $hostProcess.Owner) {
      $failures += "Host runs as $($hostProcess.User), expected $($hostProcess.Owner)"
    }
  }

  $dll = Join-Path $VersionDir 'bin\FusionTsf.dll'
  $sha = Get-ManifestFileHash -ManifestPath $ManifestPath -RelativePath 'bin/FusionTsf.dll'
  $registration = Test-RegistrationPointsTo -DllPath $dll
  if (-not $registration.Ok) { $failures += $registration.Reason }

  # Only start a Notepad when the Host and registration already match: opening a
  # window on a run that is going to stop anyway helps nobody.
  $notepad = $null
  $module = $null
  if ($failures.Count -eq 0) {
    if ($NotepadId -gt 0) { $notepad = Get-Process -Id $NotepadId -ErrorAction SilentlyContinue }
    if (-not $notepad) { $notepad = Start-NotepadForCheck }
    $module = Test-NotepadLoadedDll -ProcessId $notepad.Id -DllPath $dll -Sha256 $sha
    if (-not $module.Ok) { $failures += "Notepad: $($module.Reason)" }
  }

  return [pscustomobject]@{
    Ok           = ($failures.Count -eq 0)
    Failures     = $failures
    Host         = $hostProcess
    Registration = $registration
    Notepad      = $notepad
    Dll          = $dll
    Sha256       = $sha
    Loaded       = if ($module) { $module.Loaded } else { '' }
  }
}

# --- launching a child script ---------------------------------------------
#
# Lives here rather than in verify-wp4.ps1 so the walkthrough *and* the UAC-free
# proof exercise the same wrapper: the same argument quoting, the same exit-code
# rule, the same transcript hand-off. Both expect $script:runDir and $script:logPath
# to be set by the caller.
#
# Starts a script elevated through UAC and returns its exit code. ProcessStartInfo
# with UseShellExecute + runas is used instead of Start-Process: Start-Process
# builds an environment dictionary and fails on a machine that has both `http_proxy`
# and `HTTP_PROXY` defined.
#
# -NotElevated runs the same way without UAC. It exists so the walkthrough's own
# wrapper (exit-code handling, transcript hand-off) can be exercised for
# `install.ps1 -VerifyOnly`, which needs no elevation; it is not used by a step.
function Start-ElevatedProcess {
  param([string[]]$Arguments, [switch]$NotElevated)
  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = 'powershell.exe'
  $startInfo.Arguments = ($Arguments | ForEach-Object {
      if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
  $startInfo.UseShellExecute = $true
  if (-not $NotElevated) { $startInfo.Verb = 'runas' }
  $process = [System.Diagnostics.Process]::Start($startInfo)
  $process.WaitForExit()
  return $process.ExitCode
}

# Runs an elevated script through UAC and captures its transcript. A runas launch
# cannot redirect its output, so the log is written by the elevated process itself.
#
# `Parameters` is a hashtable: Format-ArgumentList emits parameter names bare and
# quotes only values, so `-PackageDir '<path>'` binds to the parameter and not to a
# string that PowerShell would bind positionally.
#
# `TranscriptPath` is for install.ps1 / uninstall.ps1, which start their own
# transcript (they must, so a failure inside the elevated span names its own step).
# In that case this wrapper starts none - a second, nested Start-Transcript fails -
# and appends the callee's file afterwards.
#
# The wrapper decides its exit code from `$?`, never from `$LASTEXITCODE` alone: a
# .ps1 call only sets that variable when the callee calls `exit`, and the value can
# be a leftover from an earlier native command.
function Invoke-ElevatedScript {
  param([string]$ScriptPath, [hashtable]$Parameters = @{}, [string]$LogName, [string]$TranscriptPath = '', [switch]$RunAsCurrentUser)
  $argumentText = Format-ArgumentList -Parameters $Parameters
  $wrapper = Join-Path $script:runDir ("elevated-" + [IO.Path]::GetFileNameWithoutExtension($LogName) + ".ps1")
  $wrapperLog = Join-Path $script:runDir $LogName
  $lines = @()
  if (-not $TranscriptPath) {
    $lines += "Start-Transcript -Path '$wrapperLog' -Force | Out-Null"
  }
  $lines += "& '$ScriptPath' $argumentText"
  $lines += '$ok = $?'
  $lines += '$code = 0'
  $lines += 'if (-not $ok) {'
  $lines += '  $code = 1'
  $lines += '  $last = Get-Variable -Name LASTEXITCODE -ErrorAction SilentlyContinue'
  $lines += '  if ($last -and $last.Value -is [int] -and $last.Value -ne 0) { $code = $last.Value }'
  $lines += '}'
  if (-not $TranscriptPath) {
    $lines += 'Stop-Transcript | Out-Null'
  }
  $lines += 'exit $code'
  Set-Content -LiteralPath $wrapper -Value $lines -Encoding UTF8
  Add-Content -LiteralPath $script:logPath -Value ("`n### ELEVATED {0}: {1} {2}" -f $LogName, $ScriptPath, $argumentText) -Encoding UTF8
  $code = Start-ElevatedProcess -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $wrapper) -NotElevated:$RunAsCurrentUser
  Add-Content -LiteralPath $script:logPath -Value "### ELEVATED $LogName exit=$code" -Encoding UTF8
  $log = if ($TranscriptPath) { $TranscriptPath } else { $wrapperLog }
  if (Test-Path -LiteralPath $log) {
    Add-Content -LiteralPath $script:logPath -Value (Get-Content -LiteralPath $log -Raw) -Encoding UTF8
  }
  return $code
}
