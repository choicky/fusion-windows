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

# Enumerating visible top-level windows. Needed because on this machine notepad.exe is
# the packaged Windows 11 Notepad (tabs): `Process.Start('notepad.exe')` returns a
# launcher that hands the window to another process and exits, so the PID the harness
# started is not the PID that owns the window the owner types in.
if (-not ('FusionWindows' -as [type])) {
  Add-Type -Namespace '' -Name 'FusionWindows' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumWindowsProc callback, System.IntPtr lParam);
public delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint processId);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr hWnd);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowTextW(System.IntPtr hWnd, System.Text.StringBuilder text, int count);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassNameW(System.IntPtr hWnd, System.Text.StringBuilder text, int count);

public static string[] VisibleWindows() {
  var list = new System.Collections.Generic.List<string>();
  EnumWindows(delegate(System.IntPtr hWnd, System.IntPtr lParam) {
    if (!IsWindowVisible(hWnd)) { return true; }
    var title = new System.Text.StringBuilder(512);
    GetWindowTextW(hWnd, title, 512);
    if (title.Length == 0) { return true; }
    var cls = new System.Text.StringBuilder(256);
    GetClassNameW(hWnd, cls, 256);
    uint pid;
    GetWindowThreadProcessId(hWnd, out pid);
    list.Add(hWnd.ToInt64().ToString() + "|" + pid.ToString() + "|" + cls.ToString() + "|" + title.ToString());
    return true;
  }, System.IntPtr.Zero);
  return list.ToArray();
}
'@
}

# Every Notepad window that is visible right now, as data. "Notepad window" means the
# `Notepad` window class (both the classic and the packaged editor use it), with a
# title check as a fallback if a future build renames the class.
function Get-NotepadWindows {
  $result = @()
  $seen = @{}
  foreach ($raw in [FusionWindows]::VisibleWindows()) {
    $parts = $raw -split '\|', 4
    if ($parts.Count -lt 4) { continue }
    $handle = 0L
    if (-not [long]::TryParse($parts[0], [ref]$handle)) { continue }
    $processId = 0
    if (-not [int]::TryParse($parts[1], [ref]$processId)) { continue }
    $class = $parts[2]
    $title = $parts[3]
    # The window class is the reliable signal (classic and packaged Notepad both use
    # `Notepad`). The title fallback must end with "- Notepad": another editor called
    # "Notepad--" ends with "--" and must never be picked as the window to check.
    if ($class -ne 'Notepad' -and $title -notlike '*- Notepad') { continue }
    # The packaged app can report more than one top-level window with the same title;
    # the record should list processes, not windows.
    $key = "$processId|$title"
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $process) { continue }
    $path = ''
    try { $path = $process.Path } catch { }
    $started = $null
    try { $started = $process.StartTime } catch { }
    $result += [pscustomobject]@{
      Id        = $process.Id
      Handle    = [IntPtr]$handle
      Path      = $path
      Title     = $title
      Class     = $class
      StartTime = $started
    }
  }
  return @($result)
}

# The Notepad window the owner is looking at: the newest visible one, preferring a
# window created at or after the launch (so an older Notepad the owner had open is not
# mistaken for the one this run opened). Waits, bounded, for such a window to appear.
function Resolve-NotepadWindow {
  param(
    [datetime]$StartedAfter = [datetime]::MinValue,
    [int]$ExcludePid = 0,
    [int]$TimeoutMs = 15000
  )
  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  while ($true) {
    $candidates = @(Get-NotepadWindows | Where-Object { $_.Id -ne $ExcludePid })
    $fresh = @($candidates | Where-Object { $_.StartTime -and $_.StartTime -ge $StartedAfter })
    # @( ) around the if: an if-expression enumerates its output, so a one-element
    # array would come back as a scalar and `.Count` would throw under strict mode.
    $pool = @(if ($fresh.Count -gt 0) { $fresh } else { $candidates })
    if ($pool.Count -gt 0) {
      $chosen = $pool | Sort-Object -Property StartTime -Descending | Select-Object -First 1
      $reason = if ($fresh.Count -gt 0) {
        'newest visible Notepad window created at or after the launch'
      } else {
        'newest visible Notepad window (none was created after the launch)'
      }
      return [pscustomobject]@{ Window = $chosen; Reason = $reason; Candidates = $candidates }
    }
    if ((Get-Date) -ge $deadline) {
      return [pscustomobject]@{ Window = $null; Reason = "no visible Notepad window within $TimeoutMs ms"; Candidates = $candidates }
    }
    Start-Sleep -Milliseconds 250
  }
}

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

# Starts a Notepad the way the walkthrough always has and resolves the process that
# actually owns its window.
#
# The returned object carries both PIDs: `LauncherPid` (what Process.Start returned,
# which is dead within a second on the packaged Windows 11 Notepad) and `Window`
# (the process to inspect). The caller checks `Window`, never the launcher.
function Start-NotepadForCheck {
  param([int]$TimeoutMs = 15000)
  $launched = Get-Date
  $launcher = [System.Diagnostics.Process]::Start('notepad.exe')
  $resolution = Resolve-NotepadWindow -StartedAfter $launched.AddSeconds(-2) -TimeoutMs $TimeoutMs
  $launcherAlive = $false
  try { $launcherAlive = -not $launcher.HasExited } catch { }
  $reason = $resolution.Reason
  if ($resolution.Window) {
    if ($launcher.Id -eq $resolution.Window.Id) {
      $reason = "$reason; the launched pid is the window owner"
    } else {
      $reason = ("{0}; the launched pid {1} is not the window owner (alive={2})" -f $reason, $launcher.Id, $launcherAlive)
    }
  }
  return [pscustomobject]@{
    Window      = $resolution.Window
    Reason      = $reason
    Candidates  = $resolution.Candidates
    LauncherPid = $launcher.Id
  }
}

# Waits, bounded, until no Notepad window is open and no Notepad process holds a
# FusionTsf.dll that would poison the next check.
#
# Every Notepad window on this machine lives in ONE packaged process (a second launch
# adds a tab), so "open a fresh Notepad" only produces a new process after the old one
# has exited. `ExpectedDll` is the build the next check wants: a process that already
# has that DLL loaded is harmless, any *other* Fusion DLL is not. With
# `RequireNoFusionDll`, every loaded FusionTsf.dll counts (used before an uninstall,
# where the files must be released).
function Wait-NotepadStateClear {
  param(
    [string]$ExpectedDll = '',
    [switch]$RequireNoFusionDll,
    [int]$TimeoutMs = 45000,
    [int]$PollMs = 400
  )
  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  while ($true) {
    $windows = @(Get-NotepadWindows)
    $foreign = @()
    foreach ($process in @(Get-Process -Name notepad -ErrorAction SilentlyContinue)) {
      $loaded = @()
      try { $loaded = @($process.Modules | Where-Object { $_.ModuleName -eq 'FusionTsf.dll' }) } catch { }
      foreach ($module in $loaded) {
        if (-not $RequireNoFusionDll -and $ExpectedDll -and
            $module.FileName.Equals($ExpectedDll, [StringComparison]::OrdinalIgnoreCase)) {
          continue
        }
        $foreign += [pscustomobject]@{ Id = $process.Id; Loaded = $module.FileName }
      }
    }
    $processes = @(Get-Process -Name notepad -ErrorAction SilentlyContinue)
    if ($windows.Count -eq 0 -and $foreign.Count -eq 0) {
      return [pscustomobject]@{ Clear = $true; Windows = @(); Foreign = @(); Processes = $processes }
    }
    if ((Get-Date) -ge $deadline) {
      return [pscustomobject]@{ Clear = $false; Windows = $windows; Foreign = $foreign; Processes = $processes }
    }
    Start-Sleep -Milliseconds $PollMs
  }
}

# The whole item-3 assertion: the Host that serves typing comes from the expected
# version directory, the registration points at that version's DLL, and a Notepad
# really loaded that DLL (path + manifest hash).
#
# `NotepadId` reuses a Notepad the caller already started (step d keeps the old one
# open on purpose); otherwise a fresh Notepad is started here and the process that
# owns its window is the one inspected - never the launcher PID that Process.Start
# returned (see Start-NotepadForCheck).
function Test-InstalledTipInUse {
  param(
    [string]$VersionDir,
    [string]$ManifestPath,
    [int]$NotepadId = 0,
    [switch]$SkipNotepad
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
  $notepadReason = ''
  $notepadCandidates = @()
  $launcherPid = 0
  $module = $null
  if ($SkipNotepad) {
    $notepadReason = 'not checked (-SkipNotepad: this step asserts Host and registration only)'
  } elseif ($failures.Count -eq 0) {
    if ($NotepadId -gt 0) {
      # Reuse the process the previous attempt resolved. It is rebuilt into the same
      # shape as a resolved window (Id/Path/Title/Class/StartTime) so callers never
      # depend on which source it came from.
      $existing = Get-Process -Id $NotepadId -ErrorAction SilentlyContinue
      if ($existing) {
        $existingPath = ''
        try { $existingPath = $existing.Path } catch { }
        $existingTitle = ''
        try { $existingTitle = $existing.MainWindowTitle } catch { }
        $existingStarted = $null
        try { $existingStarted = $existing.StartTime } catch { }
        $notepad = [pscustomobject]@{
          Id        = $existing.Id
          Handle    = [IntPtr]::Zero
          Path      = $existingPath
          Title     = $existingTitle
          Class     = 'Notepad'
          StartTime = $existingStarted
        }
        $notepadReason = 'the process the caller asked for'
      }
    }
    if (-not $notepad) {
      $started = Start-NotepadForCheck
      $notepad = $started.Window
      $notepadReason = $started.Reason
      $notepadCandidates = @($started.Candidates)
      $launcherPid = $started.LauncherPid
    }
    if (-not $notepad) {
      $failures += "no Notepad window to check: $notepadReason"
    } else {
      $module = Test-NotepadLoadedDll -ProcessId $notepad.Id -DllPath $dll -Sha256 $sha
      if (-not $module.Ok) { $failures += "Notepad pid $($notepad.Id) ($notepadReason): $($module.Reason)" }
    }
  }

  return [pscustomobject]@{
    Ok                = ($failures.Count -eq 0)
    Failures          = $failures
    Host              = $hostProcess
    Registration      = $registration
    Notepad           = $notepad
    NotepadReason     = $notepadReason
    NotepadCandidates = $notepadCandidates
    LauncherPid       = $launcherPid
    Dll               = $dll
    Sha256            = $sha
    Loaded            = if ($module) { $module.Loaded } else { '' }
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
  # TranscriptPath is this helper's own parameter *and* an argument of the callee
  # (install.ps1 / uninstall.ps1 write their own log), so it is added to the forwarded
  # list rather than only steering this wrapper.
  $forward = $Parameters
  if ($TranscriptPath) {
    $forward = $Parameters.Clone()
    $forward['TranscriptPath'] = $TranscriptPath
  }
  $argumentText = Format-ArgumentList -Parameters $forward
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
