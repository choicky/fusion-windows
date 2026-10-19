# Stage 6 WP4 shared helpers for install.ps1 / uninstall.ps1 / verify-wp4.ps1.
#
# Everything here is read-only except Stop-FusionHostOrderly, which asks a running
# Host for an *orderly* stop through the product's own named stop event. Nothing in
# this file terminates a process, and nothing writes user data.

Set-StrictMode -Version 2.0

if (-not ('FusionNative' -as [type])) {
  Add-Type -Namespace '' -Name 'FusionNative' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr OpenEventW(uint access, bool inherit, string name);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetEvent(System.IntPtr handle);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(System.IntPtr handle);
[System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
public static extern bool OpenProcessToken(System.IntPtr process, uint access, out System.IntPtr token);
[System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
public static extern bool GetTokenInformation(System.IntPtr token, int infoClass, System.IntPtr info, int length, out int returned);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern System.IntPtr OpenProcess(uint access, bool inherit, int processId);
[System.Runtime.InteropServices.DllImport("advapi32.dll")]
public static extern System.IntPtr GetSidSubAuthority(System.IntPtr sid, uint index);
[System.Runtime.InteropServices.DllImport("advapi32.dll")]
public static extern System.IntPtr GetSidSubAuthorityCount(System.IntPtr sid);

public static string IntegrityLevel(int processId) {
  System.IntPtr process = OpenProcess(0x1000, false, processId); // PROCESS_QUERY_LIMITED_INFORMATION
  if (process == System.IntPtr.Zero) { return "unknown"; }
  System.IntPtr token = System.IntPtr.Zero;
  try {
    if (!OpenProcessToken(process, 0x0008, out token)) { return "unknown"; }
    int size = 0;
    GetTokenInformation(token, 25, System.IntPtr.Zero, 0, out size); // TokenIntegrityLevel
    if (size <= 0) { return "unknown"; }
    System.IntPtr buffer = System.Runtime.InteropServices.Marshal.AllocHGlobal(size);
    try {
      int returned = 0;
      if (!GetTokenInformation(token, 25, buffer, size, out returned)) { return "unknown"; }
      System.IntPtr sid = System.Runtime.InteropServices.Marshal.ReadIntPtr(buffer);
      System.IntPtr countPointer = GetSidSubAuthorityCount(sid);
      byte count = System.Runtime.InteropServices.Marshal.ReadByte(countPointer);
      System.IntPtr ridPointer = GetSidSubAuthority(sid, (uint)(count - 1));
      int rid = System.Runtime.InteropServices.Marshal.ReadInt32(ridPointer);
      switch (rid) {
        case 0x0000: return "untrusted";
        case 0x1000: return "low";
        case 0x2000: return "medium";
        case 0x2100: return "medium-plus";
        case 0x3000: return "high";
        case 0x4000: return "system";
        case 0x5000: return "protected";
        default: return "0x" + rid.ToString("X");
      }
    } finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(buffer); }
  } finally {
    if (token != System.IntPtr.Zero) { CloseHandle(token); }
    CloseHandle(process);
  }
}
'@
}

# The stop-event name the product Host creates: per user SID x session, in Local\.
# The elevated scripts compute it from their own token, which under UAC is the same
# user and session as the Host they must stop.
function Get-FusionStopEventName {
  $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $session = (Get-Process -Id $PID).SessionId
  return "Local\FusionHost-stop-$sid-$session"
}

function Test-FusionElevated {
  $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
  return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Running Hosts, with the identity the evidence record needs: path, user, session
# and integrity level.
function Get-FusionHostState {
  $result = @()
  foreach ($process in @(Get-Process -Name FusionHost -ErrorAction SilentlyContinue)) {
    $path = $null
    try { $path = $process.Path } catch { }
    $owner = ''
    try {
      $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)" -ErrorAction Stop
      if ($cim) {
        $who = Invoke-CimMethod -InputObject $cim -MethodName GetOwner -ErrorAction Stop
        if ($who -and $who.User) { $owner = "$($who.Domain)\$($who.User)" }
      }
    } catch { }
    # StartTime throws for a process that exited between the enumeration and here
    # (and for a protected one); it is evidence, not a reason to fail the caller.
    $started = $null
    try { $started = $process.StartTime } catch { }
    $result += [pscustomobject]@{
      Id        = $process.Id
      Path      = $path
      User      = $owner
      SessionId = $process.SessionId
      Integrity = [FusionNative]::IntegrityLevel($process.Id)
      StartTime = $started
    }
  }
  return $result
}

# Registration inventory (same keys tools/tsf/check-state.ps1 inspects), as data.
function Get-FusionRegistrationState {
  $clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
  $profile = '{D4A1F6B2-3C58-4E77-8A90-1B5D9C06E4F1}'
  $result = @()
  foreach ($hive in @('HKLM', 'HKCU')) {
    $com = "${hive}:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
    $tip = "${hive}:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
    $dll = $null
    if (Test-Path -LiteralPath $com) { $dll = (Get-Item -LiteralPath $com).GetValue('') }
    $languages = @()
    if (Test-Path -LiteralPath "$tip\LanguageProfile") {
      foreach ($language in Get-ChildItem -LiteralPath "$tip\LanguageProfile") {
        $languages += [pscustomobject]@{
          Language = $language.PSChildName
          Profile  = Test-Path -LiteralPath (Join-Path $language.PSPath $profile)
        }
      }
    }
    $result += [pscustomobject]@{
      Hive      = $hive
      ComKey    = Test-Path -LiteralPath $com
      Dll       = $dll
      DllExists = if ($dll) { Test-Path -LiteralPath $dll } else { $false }
      TipKey    = Test-Path -LiteralPath $tip
      Category  = Test-Path -LiteralPath "$tip\Category"
      Languages = $languages
    }
  }
  return $result
}

function Get-FusionScheduledTask {
  $task = Get-ScheduledTask -TaskName 'FusionHost' -ErrorAction SilentlyContinue
  if (-not $task) { return $null }
  $info = Get-ScheduledTaskInfo -TaskName 'FusionHost' -ErrorAction SilentlyContinue
  # A task with no trigger yields $null here, and @($null) is a one-element array of
  # null: filter it out before reading CimClass (StrictMode would throw). The product
  # task always has triggers; the WP7 restart tool also reads tasks registered by hand.
  $triggers = @($task.Triggers | Where-Object { $null -ne $_ })
  return [pscustomobject]@{
    TaskName  = $task.TaskName
    State     = [string]$task.State
    Execute   = ($task.Actions | ForEach-Object { $_.Execute }) -join ';'
    Arguments = ($task.Actions | ForEach-Object { $_.Arguments }) -join ';'
    UserId    = $task.Principal.UserId
    GroupId   = $task.Principal.GroupId
    LogonType = [string]$task.Principal.LogonType
    RunLevel  = [string]$task.Principal.RunLevel
    Instances = [string]$task.Settings.MultipleInstances
    TimeLimit = [string]$task.Settings.ExecutionTimeLimit
    TriggerCount = $triggers.Count
    Triggers  = ($triggers | ForEach-Object {
        $_.CimClass.CimClassName -replace '^MSFT_Task', '' -replace 'Trigger$', ''
      }) -join ';'
    # The R34 event trigger's XPath, so a run can record that it exists and what it
    # selects (empty when the task has no event trigger).
    EventSubscription = (@($triggers |
          Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskEventTrigger' } |
          ForEach-Object { $_.Subscription }) -join ';')
    LastResult = if ($info) { $info.LastTaskResult } else { $null }
  }
}

# The DLL the machine-wide (HKLM) COM registration points at, or $null.
#
# `(Get-FusionRegistrationState | Where-Object {...}).Dll` is a trap under
# Set-StrictMode -Version 2.0: when the filter matches nothing the pipeline yields
# $null and the property read throws "cannot be found on this object". Every caller
# that needs this value goes through here instead.
function Get-FusionRegisteredDll {
  $entries = @(Get-FusionRegistrationState | Where-Object { $_.Hive -eq 'HKLM' })
  if ($entries.Count -eq 0) { return $null }
  $dll = $entries[0].Dll
  if ([string]::IsNullOrEmpty($dll)) { return $null }
  return [string]$dll
}

# The per-user input state that outlives the machine-wide registration: the HKCU
# CTF\TIP key (written by EnableLanguageProfile, not by registration - it holds only
# `Enable = 1` under LanguageProfile\<lang>\<profile>) and this user's language-list
# tips. After an uninstall that removed only HKLM, the enable flag is what keeps a
# "ghost" input method visible in the user's input indicator.
function Get-FusionUserInputState {
  $clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
  $profile = '{D4A1F6B2-3C58-4E77-8A90-1B5D9C06E4F1}'
  $tipKey = "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
  $enableKey = "$tipKey\LanguageProfile\0x00000804\$profile"
  $enable = $null
  if (Test-Path -LiteralPath $enableKey) { $enable = (Get-Item -LiteralPath $enableKey).GetValue('Enable') }
  $tips = @()
  try {
    foreach ($language in @(Get-WinUserLanguageList)) {
      foreach ($entry in @($language.InputMethodTips)) { $tips += [string]$entry }
    }
  } catch {
    # A read-only inventory must not fail an uninstall when the query is unavailable.
  }
  $needle = "0804:$clsid$profile"
  $listed = @($tips | Where-Object { $_.ToUpperInvariant() -eq $needle.ToUpperInvariant() })
  return [pscustomobject]@{
    TipKey          = Test-Path -LiteralPath $tipKey
    EnableKey       = Test-Path -LiteralPath $enableKey
    Enable          = $enable
    FusionTip       = $needle
    FusionTipListed = ($listed.Count -gt 0)
    Tips            = $tips
  }
}

# Removes the per-user input state for one CLSID/profile: the HKCU CTF\TIP key and, if
# this user has one, the language-list tip. Current user only; nothing else is touched.
# `-Clsid`/`-Profile` exist so the proof can exercise this exact code against a
# synthetic key instead of the real registration.
function Remove-FusionUserInputState {
  param(
    [string]$Clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}',
    [string]$Profile = '{D4A1F6B2-3C58-4E77-8A90-1B5D9C06E4F1}'
  )
  $result = [pscustomobject]@{
    TipKey         = "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$Clsid"
    TipKeyExisted  = $false
    EnableWas      = $null
    TipKeyRemoved  = $false
    LanguageTip    = "0804:$Clsid$Profile"
    LanguageTipRemoved = $false
    Warning        = ''
  }
  if (Test-Path -LiteralPath $result.TipKey) {
    $result.TipKeyExisted = $true
    $enableKey = "$($result.TipKey)\LanguageProfile\0x00000804\$Profile"
    if (Test-Path -LiteralPath $enableKey) {
      $result.EnableWas = (Get-Item -LiteralPath $enableKey).GetValue('Enable')
    }
    Remove-Item -LiteralPath $result.TipKey -Recurse -Force
    $result.TipKeyRemoved = -not (Test-Path -LiteralPath $result.TipKey)
  }
  # The language list is the other place a user could still see the input method. It is
  # only rewritten when this user actually has the tip: rewriting it otherwise would
  # touch settings for nothing, and a failure here must not fail an uninstall.
  try {
    $needle = $result.LanguageTip
    $list = @(Get-WinUserLanguageList)
    $found = $false
    foreach ($language in $list) {
      # InputMethodTips is a get-only StringCollection: remove from it, do not assign.
      $tips = $language.InputMethodTips
      $doomed = @($tips | Where-Object { $_.ToUpperInvariant() -eq $needle.ToUpperInvariant() })
      foreach ($tip in $doomed) { [void]$tips.Remove($tip) }
      if ($doomed.Count -gt 0) { $found = $true }
    }
    if ($found) {
      Set-WinUserLanguageList -LanguageList $list -Force
      $result.LanguageTipRemoved = $true
    }
  } catch {
    $result.Warning = "the language list was not changed: $($_.Exception.Message)"
  }
  return $result
}

# Is any file under `Path` loaded as a module by a running process? Used before
# removing a version directory: a directory whose DLL an application still holds is
# kept and reported, never force-deleted (and never partially deleted, which a
# Remove-Item that fails halfway would do).
#
# Every process is checked, not only the ones a walkthrough starts: on Windows 11 the
# input method is global, so once Fusion is used its DLL stays loaded in shell processes
# (explorer.exe, WindowsTerminal.exe, ...) until sign-out, and those keep the directory
# in use no matter how many Notepad windows are closed.
function Test-DirectoryInUse {
  param([string]$Path)
  $prefix = $Path.TrimEnd('\') + '\'
  $pids = @()
  $names = @()
  foreach ($process in @(Get-Process -ErrorAction SilentlyContinue)) {
    try {
      foreach ($module in $process.Modules) {
        if ($module.FileName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
          $pids += $process.Id
          $names += "$($process.ProcessName)($($process.Id))"
          break
        }
      }
    } catch {
      # Access denied (another user, protected process): it cannot be holding a
      # file in a directory only this user's installation wrote.
    }
  }
  $unique = @($pids | Sort-Object -Unique)
  return [pscustomobject]@{
    InUse = ($unique.Count -gt 0)
    Pids  = $unique
    Names = @($names | Sort-Object -Unique)
  }
}

# Copies a package into a version directory, touching only what differs.
#
# A reinstall into an existing version directory must not touch an identical DLL that
# processes still have mapped: on Windows 11 the input method is global, so
# FusionTsf.dll stays loaded in shell processes (explorer.exe, WindowsTerminal.exe, ...)
# until sign-out, and overwriting it would fail. A file whose SHA-256 already matches the
# manifest is skipped; a file that really must change but is in use returns a failure
# naming the file and every process holding it.
function Copy-FusionPackageFiles {
  param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [Parameter(Mandatory = $true)][string]$VersionDir,
    [Parameter(Mandatory = $true)]$Entries
  )
  $result = [pscustomobject]@{ Copied = 0; Skipped = 0; Files = 0; Failure = $null }
  $expected = @{}
  foreach ($entry in @($Entries)) { $expected[($entry.path -replace '/', '\')] = $entry.sha256 }
  foreach ($relative in @($expected.Keys | Sort-Object)) {
    $source = Join-Path $PackageDir $relative
    $target = Join-Path $VersionDir $relative
    if (Test-Path -LiteralPath $target) {
      $existing = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
      if ($existing -eq $expected[$relative]) { $result.Skipped++; continue }
    }
    $parent = Split-Path -Parent $target
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
      New-Item -ItemType Directory -Force $parent | Out-Null
    }
    try {
      Copy-Item -LiteralPath $source -Destination $target -Force -ErrorAction Stop
      $result.Copied++
    } catch {
      $holders = Get-FusionFileHolders -Path $target
      $who = if ($holders.Names.Count -gt 0) {
        $holders.Names -join ', '
      } else {
        'no process has it mapped; the file is locked by a handle this script cannot identify'
      }
      $result.Failure = ("cannot replace {0}: {1} (holding processes: {2})" -f
        $relative, $_.Exception.Message.Trim(), $who)
      return $result
    }
  }
  # package.json is the manifest itself and is therefore not in its own file list.
  $manifest = Join-Path $PackageDir 'package.json'
  if (Test-Path -LiteralPath $manifest) {
    Copy-Item -LiteralPath $manifest -Destination $VersionDir -Force
  }
  $result.Files = @(Get-ChildItem -LiteralPath $VersionDir -Recurse -File).Count
  return $result
}

# The processes that have this exact file mapped as a module. Used by the installer when
# a file that must change cannot be replaced: the error names the holders instead of
# leaving a bare "access denied".
function Get-FusionFileHolders {
  param([string]$Path)
  $pids = @()
  $names = @()
  foreach ($process in @(Get-Process -ErrorAction SilentlyContinue)) {
    try {
      foreach ($module in $process.Modules) {
        if ($module.FileName.Equals($Path, [StringComparison]::OrdinalIgnoreCase)) {
          $pids += $process.Id
          $names += "$($process.ProcessName)($($process.Id))"
          break
        }
      }
    } catch { }
  }
  return [pscustomobject]@{
    Pids  = @($pids | Sort-Object -Unique)
    Names = @($names | Sort-Object -Unique)
  }
}

# Calls another PowerShell script and reports success without ever reading
# $LASTEXITCODE.
#
# `& script.ps1` does NOT set $LASTEXITCODE unless the callee calls `exit`, and when
# it does set it the value can also be a leftover from an earlier native command
# (the variable lives in the global scope, so it cannot even be cleared locally).
# Reading it under Set-StrictMode -Version 2.0 throws when it is unset - that is how
# a *successful* register.ps1 aborted the first elevated install (run 12:44). The
# contract is therefore $? plus try/catch: both a `throw` and a non-zero `exit`
# leave $? False.
function Invoke-PackageScript {
  param([string]$ScriptPath, [hashtable]$Parameters = @{})
  $outcome = [pscustomobject]@{ Ok = $true; Threw = $false; Error = '' }
  try {
    & $ScriptPath @Parameters
    $outcome.Ok = $?
  } catch {
    $outcome.Ok = $false
    $outcome.Threw = $true
    $outcome.Error = $_.Exception.Message
  }
  return $outcome
}

# Asks the running Host for an orderly stop through the product's own stop event
# and waits, bounded, for the process to exit. Never terminates anything: if the
# event cannot be opened (another principal, or no Host), it reports that instead.
function Stop-FusionHostOrderly {
  param([int]$TimeoutMs = 20000, [string]$InstallRoot = '')
  $eventName = Get-FusionStopEventName
  $hosts = @(Get-FusionHostState)
  if ($InstallRoot) {
    $hosts = @($hosts | Where-Object { $_.Path -and $_.Path.StartsWith($InstallRoot, [StringComparison]::OrdinalIgnoreCase) })
  }
  $result = [pscustomobject]@{
    EventName = $eventName
    Found     = ($hosts.Count -gt 0)
    Pids      = @($hosts | ForEach-Object { $_.Id })
    Signalled = $false
    Exited    = $true
    WaitedMs  = 0
    Error     = $null
  }
  if ($hosts.Count -eq 0) { return $result }

  $handle = [FusionNative]::OpenEventW(0x0002, $false, $eventName) # EVENT_MODIFY_STATE
  if ($handle -eq [IntPtr]::Zero) {
    $result.Signalled = $false
    $result.Exited = $false
    $result.Error = "cannot open the stop event for this principal (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    return $result
  }
  try {
    $result.Signalled = [FusionNative]::SetEvent($handle)
  } finally {
    [void][FusionNative]::CloseHandle($handle)
  }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
    $alive = @($result.Pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if ($alive.Count -eq 0) { break }
    Start-Sleep -Milliseconds 100
  }
  $result.WaitedMs = [int]$watch.ElapsedMilliseconds
  $result.Exited = (@($result.Pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue }).Count -eq 0)
  return $result
}
