# Stage 6 WP4 B3 — owner walkthrough for the packaging / lifecycle work package.
#
# The owner never edits a file and never types a command: this script performs
# every action itself, writes every file it needs, and accepts only Enter / y / n.
# Prompts appear in the order the owner can actually perform them, and every result
# the owner reports is captured (a y/n answer per question), not only "Enter".
#
# It records, before and after every step:
#   * C:\Program Files\Fcitx5 Fusion (version directories and their contents)
#   * HKLM/HKCU CTF TIP + COM keys (and the DLL they point at)
#   * the FusionHost scheduled task
#   * running FusionHost processes (path, user, integrity level, session)
#   * %APPDATA%\Fcitx5 (file count/size and the key files' size/mtime/hash)
#   * the machine and user PATH (to show nothing changed it)
#
# The development registration found before the run is recorded and restored at the
# end. Run it non-elevated: every elevated action is launched through UAC by the
# script itself.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File verify-wp4.ps1 -PackageDir <dir with the two zips>
#   powershell -ExecutionPolicy Bypass -File verify-wp4.ps1 -Resume      # after sign-in
#   powershell -ExecutionPolicy Bypass -File verify-wp4.ps1 -DryRun      # stop at the first prompt

param(
  [string]$PackageDir = '',
  [string]$EvidenceRoot = 'C:\Users\choicky\worktmp\fusion-windows\stage6-wp4',
  [switch]$Resume,
  [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'host-state.ps1')

$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Fcitx5 Fusion'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
$statePath = Join-Path $EvidenceRoot 'state.json'
$snapshotIndex = 0

function Write-Head([string]$text) {
  Write-Host ''
  Write-Host ('=' * 78)
  Write-Host $text
  Write-Host ('=' * 78)
}

function Ask-Enter([string]$text) {
  Write-Host ''
  Write-Host $text -ForegroundColor Cyan
  $answer = Read-Host '准备好后按 Enter（输入 n 放弃本次走查）'
  return ($answer -notmatch '^\s*[nN]\s*$')
}

function Ask-YesNo([string]$question) {
  while ($true) {
    Write-Host ''
    Write-Host $question -ForegroundColor Cyan
    $answer = Read-Host '请输入 y 或 n'
    if ($answer -match '^\s*[yY](es)?\s*$') { return $true }
    if ($answer -match '^\s*[nN](o)?\s*$') { return $false }
    Write-Host '  只接受 y 或 n。' -ForegroundColor Yellow
  }
}

# A short free-text answer (a rank, a pinyin string). It is recorded verbatim, so a
# reported observation is never reduced to "Enter".
function Ask-Text([string]$question, [string]$hint) {
  Write-Host ''
  Write-Host $question -ForegroundColor Cyan
  $answer = Read-Host $hint
  if ([string]::IsNullOrWhiteSpace($answer)) { return '(未填写)' }
  return $answer.Trim()
}

# Starts Notepad with one typed character so the logoff asks to save and can be
# cancelled. If the synthetic keystroke does not land, the prompt asks the owner to
# type a character themselves.
function Start-UnsavedNotepad {
  $process = [System.Diagnostics.Process]::Start('notepad.exe')
  Start-Sleep -Seconds 2
  try {
    $shell = New-Object -ComObject WScript.Shell
    [void]$shell.AppActivate($process.Id)
    Start-Sleep -Milliseconds 400
    $shell.SendKeys('x')
  } catch {
    # The owner types the character instead; the prompt says so.
  }
  return $process
}

function Add-Result([string]$step, [string]$question, $answer) {
  $line = "[{0}] {1} => {2}" -f $step, $question, $answer
  Add-Content -LiteralPath $script:logPath -Value $line -Encoding UTF8
  Write-Host "  记录：$line" -ForegroundColor DarkGray
}

function Get-UserDataSummary {
  $root = Join-Path $env:APPDATA 'Fcitx5'
  if (-not (Test-Path -LiteralPath $root)) { return 'absent' }
  $files = @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue)
  $lines = @("root=$root", "files=$($files.Count) bytes=$((($files | Measure-Object Length -Sum).Sum))")
  foreach ($name in @('pinyin\user.dict', 'pinyin\user.history', 'config\fcitx5\profile', 'config\fcitx5\conf\pinyin.conf')) {
    $path = Join-Path $root $name
    if (Test-Path -LiteralPath $path) {
      $file = Get-Item -LiteralPath $path
      $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.Substring(0, 16)
      $lines += ("{0} size={1} mtime={2:yyyy-MM-dd HH:mm:ss} sha256={3}" -f $name, $file.Length, $file.LastWriteTime, $hash)
    } else {
      $lines += "$name absent"
    }
  }
  return ($lines -join "`n")
}

function Get-InstallRootSummary {
  if (-not (Test-Path -LiteralPath $installRoot)) { return 'absent' }
  $lines = @()
  foreach ($directory in Get-ChildItem -LiteralPath $installRoot -Directory) {
    $files = @(Get-ChildItem -LiteralPath $directory.FullName -Recurse -File -ErrorAction SilentlyContinue)
    $lines += ("{0}\ files={1} bytes={2}" -f $directory.Name, $files.Count, (($files | Measure-Object Length -Sum).Sum))
    foreach ($relative in @('bin\FusionHost.exe', 'bin\FusionTsf.dll', 'bin\FusionFcitxHost.dll')) {
      $path = Join-Path $directory.FullName $relative
      if (Test-Path -LiteralPath $path) {
        $lines += ("  {0} sha256={1}" -f $relative, (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)
      }
    }
  }
  if ($lines.Count -eq 0) { return 'empty' }
  return ($lines -join "`n")
}

function New-Snapshot([string]$label) {
  $script:snapshotIndex++
  $name = '{0:d2}-{1}.txt' -f $script:snapshotIndex, $label
  $lines = @()
  $lines += "label: $label"
  $lines += "time: $((Get-Date).ToString('o'))"
  $lines += "elevated: $(Test-FusionElevated)"
  $lines += "user: $([System.Security.Principal.WindowsIdentity]::GetCurrent().Name)"
  $lines += "session: $((Get-Process -Id $PID).SessionId)"
  $lines += ''
  $lines += '--- install root ---'
  $lines += Get-InstallRootSummary
  $lines += ''
  $lines += '--- registration ---'
  foreach ($entry in Get-FusionRegistrationState) {
    $lines += ("{0} COM={1} DLL={2} exists={3} TIP={4} category={5} languages={6}" -f
      $entry.Hive, $entry.ComKey, $entry.Dll, $entry.DllExists, $entry.TipKey, $entry.Category,
      (($entry.Languages | ForEach-Object { "$($_.Language):$($_.Profile)" }) -join ','))
  }
  $lines += ''
  $lines += '--- scheduled task ---'
  $task = Get-FusionScheduledTask
  if ($task) {
    $lines += ("name={0} state={1} execute={2} args={3} user={4} group={5} logon={6} runLevel={7} instances={8} timeLimit={9} lastResult={10}" -f
      $task.TaskName, $task.State, $task.Execute, $task.Arguments, $task.UserId, $task.GroupId,
      $task.LogonType, $task.RunLevel, $task.Instances, $task.TimeLimit, $task.LastResult)
  } else {
    $lines += 'absent'
  }
  $lines += ''
  $lines += '--- FusionHost processes ---'
  $hosts = @(Get-FusionHostState)
  if ($hosts.Count -eq 0) {
    $lines += 'none'
  } else {
    foreach ($process in $hosts) {
      $lines += ("pid={0} session={1} user={2} integrity={3} path={4}" -f
        $process.Id, $process.SessionId, $process.User, $process.Integrity, $process.Path)
    }
  }
  $lines += ''
  $lines += '--- %APPDATA%\Fcitx5 ---'
  $lines += Get-UserDataSummary
  $lines += ''
  $lines += '--- PATH ---'
  $lines += "machine=$([Environment]::GetEnvironmentVariable('PATH', 'Machine'))"
  $lines += "user=$([Environment]::GetEnvironmentVariable('PATH', 'User'))"
  $text = $lines -join "`n"
  Set-Content -LiteralPath (Join-Path $script:snapshotDir $name) -Value $text -Encoding UTF8
  Add-Content -LiteralPath $script:logPath -Value "`n### SNAPSHOT $name`n$text" -Encoding UTF8
  Write-Host "  状态已记录：$name" -ForegroundColor DarkGray
  return $text
}

# Starts a script elevated through UAC and returns its exit code. ProcessStartInfo
# with UseShellExecute + runas is used instead of Start-Process: Start-Process
# builds an environment dictionary and fails on a machine that has both `http_proxy`
# and `HTTP_PROXY` defined.
function Start-ElevatedProcess {
  param([string[]]$Arguments)
  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = 'powershell.exe'
  $startInfo.Arguments = ($Arguments | ForEach-Object {
      if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
  $startInfo.UseShellExecute = $true
  $startInfo.Verb = 'runas'
  $process = [System.Diagnostics.Process]::Start($startInfo)
  $process.WaitForExit()
  return $process.ExitCode
}

# Runs an elevated script through UAC and captures its transcript. The elevated
# process writes the transcript itself, because a runas launch cannot redirect its
# output. A small generated wrapper is used instead of -Command: an argument list is
# joined with spaces, so a -Command script would be split.
function Invoke-ElevatedScript {
  param([string]$ScriptPath, [string[]]$Arguments, [string]$LogName)
  $log = Join-Path $script:runDir $LogName
  $argumentText = ($Arguments | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join ' '
  $wrapper = Join-Path $script:runDir ("elevated-" + [IO.Path]::GetFileNameWithoutExtension($LogName) + ".ps1")
  $lines = @(
    "Start-Transcript -Path '$log' -Force | Out-Null",
    "& '$ScriptPath' $argumentText",
    '$code = $LASTEXITCODE',
    'Stop-Transcript | Out-Null',
    'exit $code'
  )
  Set-Content -LiteralPath $wrapper -Value $lines -Encoding UTF8
  $code = Start-ElevatedProcess -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $wrapper)
  Add-Content -LiteralPath $script:logPath -Value "`n### ELEVATED $LogName exit=$code" -Encoding UTF8
  if (Test-Path -LiteralPath $log) {
    Add-Content -LiteralPath $script:logPath -Value (Get-Content -LiteralPath $log -Raw) -Encoding UTF8
  }
  return $code
}

function Get-PackageVersion([string]$zipPath) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
  try {
    $entry = $archive.Entries | Where-Object { $_.FullName -eq 'package.json' } | Select-Object -First 1
    if (-not $entry) { throw "package.json not found in $zipPath" }
    $reader = New-Object System.IO.StreamReader($entry.Open())
    try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    return $manifest
  } finally { $archive.Dispose() }
}

# ---------------------------------------------------------------------------
# Run directory / state
# ---------------------------------------------------------------------------

if ($Resume) {
  if (-not (Test-Path -LiteralPath $statePath)) { throw "No state file to resume: $statePath" }
  $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
  $script:runDir = $state.runDir
  $script:snapshotDir = $state.snapshotDir
  $script:logPath = $state.logPath
  $PackageDir = $state.packageDir
} else {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $script:runDir = Join-Path $EvidenceRoot "wp4-run-$stamp"
  $script:snapshotDir = Join-Path $script:runDir 'snapshots'
  $script:logPath = Join-Path $script:runDir 'verify-wp4.log'
  New-Item -ItemType Directory -Force $script:snapshotDir | Out-Null
  if (-not $PackageDir) { throw 'PackageDir is required (directory containing the wp4.1 and wp4.2 zips)' }
  $PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
  $state = $null
}

Write-Head 'Stage 6 WP4 安装与生命周期走查'
Write-Host "证据目录: $script:runDir"
Write-Host "日志:     $script:logPath"

# ---------------------------------------------------------------------------
# Fresh run: identify the packages and record the pre-run state
# ---------------------------------------------------------------------------

if (-not $Resume) {
  $zips = @(Get-ChildItem -LiteralPath $PackageDir -Filter 'fusion-*.zip' | Sort-Object Name)
  if ($zips.Count -lt 2) { throw "Expected two packages in $PackageDir, found $($zips.Count)" }
  $first = $zips[0]
  $second = $zips[-1]
  $firstManifest = Get-PackageVersion $first.FullName
  $secondManifest = Get-PackageVersion $second.FullName
  Write-Host "包 1: $($first.Name)  version=$($firstManifest.version) commit=$($firstManifest.sourceCommit)"
  Write-Host "包 2: $($second.Name)  version=$($secondManifest.version) commit=$($secondManifest.sourceCommit)"

  $extractRoot = Join-Path $script:runDir 'packages'
  New-Item -ItemType Directory -Force $extractRoot | Out-Null
  Expand-Archive -LiteralPath $first.FullName -DestinationPath (Join-Path $extractRoot $firstManifest.version) -Force
  Expand-Archive -LiteralPath $second.FullName -DestinationPath (Join-Path $extractRoot $secondManifest.version) -Force

  Add-Content -LiteralPath $script:logPath -Value "package1=$($first.Name) version=$($firstManifest.version) sha256=$((Get-FileHash -LiteralPath $first.FullName -Algorithm SHA256).Hash)" -Encoding UTF8
  Add-Content -LiteralPath $script:logPath -Value "package2=$($second.Name) version=$($secondManifest.version) sha256=$((Get-FileHash -LiteralPath $second.FullName -Algorithm SHA256).Hash)" -Encoding UTF8

  # The development registration that exists before the run, restored at the end.
  $before = Get-FusionRegistrationState
  $dev = ($before | Where-Object { $_.Hive -eq 'HKLM' -and $_.Dll }).Dll
  Write-Host "走查前的开发注册: $dev"
  New-Snapshot 'before'
  $state = [pscustomobject]@{
    runDir          = $script:runDir
    snapshotDir     = $script:snapshotDir
    logPath         = $script:logPath
    packageDir      = $PackageDir
    version1        = $firstManifest.version
    version2        = $secondManifest.version
    package1        = $first.Name
    package2        = $second.Name
    package1Path    = (Join-Path $extractRoot $firstManifest.version)
    package2Path    = (Join-Path $extractRoot $secondManifest.version)
    devRegistration = $dev
    stage           = 'a'
  }
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
} else {
  Write-Host "继续走查（步骤 $($state.stage)）"
  New-Snapshot ("resume-" + $state.stage)
}

$version1 = $state.version1
$version2 = $state.version2
$version1Dir = Join-Path $installRoot $version1
$version2Dir = Join-Path $installRoot $version2
$install1 = Join-Path $state.package1Path 'install.ps1'
$install2 = Join-Path $state.package2Path 'install.ps1'
$uninstall2 = Join-Path $version2Dir 'uninstall.ps1'

# ---------------------------------------------------------------------------
# a. install wp4.1
# ---------------------------------------------------------------------------

if ($state.stage -eq 'a') {
  Write-Head '步骤 a：安装 wp4.1'
  Write-Host '接下来会弹出一次 UAC，请允许。安装完成后会自动启动 Host（无窗口、普通权限）。'
  Write-Host '然后请在记事本中输入 nihao 并按空格，应得到“你好”。'
  if ($DryRun) {
    Write-Host ''
    Write-Host 'DRY RUN：已到达第一个所有者提示，未执行任何安装。' -ForegroundColor Yellow
    Write-Host "将要执行的命令：powershell -File `"$install1`" -PackageDir `"$($state.package1Path)`"（提权）"
    exit 0
  }
  if (-not (Ask-Enter '按 Enter 开始安装 wp4.1（UAC 确认后自动继续）。')) { Write-Host '已放弃。'; exit 0 }
  $code = Invoke-ElevatedScript -ScriptPath $install1 -Arguments @('-PackageDir', $state.package1Path) -LogName 'install-wp4.1.log'
  Add-Result 'a' 'install.ps1 退出码' $code
  New-Snapshot 'after-install-wp4.1'

  $hosts = @(Get-FusionHostState)
  Add-Result 'a' 'Host 进程数' $hosts.Count
  foreach ($process in $hosts) {
    Add-Result 'a' "Host pid=$($process.Id)" "path=$($process.Path) integrity=$($process.Integrity) session=$($process.SessionId) user=$($process.User)"
  }
  Write-Host ''
  Write-Host '请在记事本中输入 nihao 并按空格。如果输入法不是 Fusion，请按 Win+空格 切换。'
  Add-Result 'a' '记事本 nihao → 你好' (Ask-YesNo '记事本里输入 nihao + 空格，是否得到“你好”？')
  Add-Result 'a' '默认为拼音（这台机器没有 WP3 学习数据，只有首次运行的默认配置）' (Ask-YesNo '当前输入法是拼音（不是双拼），且没有历史学习记录？')
  $state.stage = 'b'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# b. second start attempt
# ---------------------------------------------------------------------------

if ($state.stage -eq 'b') {
  Write-Head '步骤 b：第二次启动尝试'
  Write-Host '脚本会再运行一次计划任务，并直接启动一次 FusionHost.exe；两者都不应产生第二个服务端。'
  if (-not (Ask-Enter '按 Enter 继续（无需操作）。')) { Write-Host '已放弃。'; exit 0 }

  Start-ScheduledTask -TaskName 'FusionHost'
  Start-Sleep -Seconds 3
  $afterTask = @(Get-FusionHostState)
  Add-Result 'b' '再次运行任务后的 Host 进程数' $afterTask.Count

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = Join-Path $version1Dir 'bin\FusionHost.exe'
  $psi.UseShellExecute = $false
  $psi.RedirectStandardError = $true
  $psi.RedirectStandardOutput = $true
  $psi.WorkingDirectory = $version1Dir
  $direct = [System.Diagnostics.Process]::Start($psi)
  $stderr = $direct.StandardError.ReadToEnd()
  $direct.WaitForExit()
  Add-Result 'b' '直接启动 FusionHost.exe 的退出码' $direct.ExitCode
  Add-Result 'b' '直接启动的 stderr' ($stderr.Trim() -replace "`r?`n", ' | ')
  New-Snapshot 'after-second-start'
  $afterDirect = @(Get-FusionHostState)
  Add-Result 'b' '直接启动后的 Host 进程数' $afterDirect.Count
  Add-Result 'b' '输入仍然正常' (Ask-YesNo '记事本里继续输入中文，是否仍然正常？')
  $state.stage = 'c'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# c. sign out / sign in
# ---------------------------------------------------------------------------

if ($state.stage -eq 'c') {
  Write-Head '步骤 c：造学习记录，然后注销并重新登录'
  # The learning must be produced with a Host that is already running, in the order
  # the owner can actually do it.
  $hosts = @(Get-FusionHostState)
  Add-Result 'c' '造学习记录前的 Host 进程数' $hosts.Count
  if ($hosts.Count -eq 0) {
    Write-Host '当前没有运行中的 Host，无法产生学习记录。请先重新运行步骤 b。' -ForegroundColor Red
    exit 6
  }
  Write-Host '请打开记事本，用 Fusion 输入法输入一个词并选择一次候选（例如输入 shuangpin 后选一个候选），'
  Write-Host '注意选择前后该候选排在第几位。脚本会记录你报告的名次，再请你注销。'
  Add-Result 'c' '学习用的拼音串' (Ask-Text '你输入的拼音串是什么？' '直接输入拼音串，例如 shuangpin')
  Add-Result 'c' '候选名次变化（选择前→选择后）' (Ask-Text '该候选学习前在第几位、选择后升到第几位？' '例如 5→1')
  if (-not (Ask-Enter '记录好名次后按 Enter，脚本会保存当前用户数据状态。')) { Write-Host '已放弃。'; exit 0 }
  New-Snapshot 'before-signout'
  $state.stage = 'c-resume'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8

  Write-Head '现在请注销，然后重新登录'
  Write-Host '1. 先保存或关闭记事本里要保留的内容；本脚本窗口会被注销结束。'
  Write-Host '2. 开始菜单 → 用户头像 → 注销。'
  Write-Host '3. 重新登录后，打开 PowerShell，运行：'
  Write-Host ''
  Write-Host ("   powershell -ExecutionPolicy Bypass -File `"{0}`" -Resume" -f $PSCommandPath) -ForegroundColor Yellow
  Write-Host ''
  Write-Host "证据目录：$script:runDir"
  exit 0
}

if ($state.stage -eq 'c-resume') {
  Write-Head '步骤 c（续）：登录后的状态'
  New-Snapshot 'after-signin'
  $hosts = @(Get-FusionHostState)
  Add-Result 'c' '登录后的 Host 进程数' $hosts.Count
  foreach ($process in $hosts) {
    Add-Result 'c' "Host pid=$($process.Id)" "path=$($process.Path) integrity=$($process.Integrity) session=$($process.SessionId) user=$($process.User)"
  }
  Write-Host ''
  Write-Host '请打开记事本，输入同样的拼音串，确认输入法仍可用，并检查刚才学习过的候选现在排第几位。'
  Add-Result 'c' '登录后输入可用' (Ask-YesNo '登录后记事本里输入中文是否正常？')
  Add-Result 'c' '登录后该候选的名次' (Ask-Text '现在同一个候选排在第几位？' '例如 1')
  Add-Result 'c' '注销前的学习在登录后仍在（R26）' (Ask-YesNo '与注销前选择后的名次相比，学习结果是否保住了？')
  $state.stage = 'c2'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# c2. a cancelled logoff must not stop the Host
# ---------------------------------------------------------------------------

if ($state.stage -eq 'c2') {
  Write-Head '步骤 c2：取消一次注销，Host 必须仍在运行'
  Write-Host '注销可以在发出后仍被取消（例如记事本询问是否保存、用户点取消）。Host 不能在这种取消后消失。'
  $before = @(Get-FusionHostState)
  $beforeIds = ($before | ForEach-Object { $_.Id } | Sort-Object) -join ','
  Add-Result 'c2' '取消注销前的 Host pid' $beforeIds
  if ($before.Count -eq 0) {
    Write-Host '当前没有运行中的 Host，无法验证取消注销。' -ForegroundColor Red
    exit 6
  }
  $notepad = Start-UnsavedNotepad
  Add-Result 'c2' '用于触发保存提示的记事本 pid' $notepad.Id
  Write-Host '脚本已打开一个记事本并输入了一个字符（若记事本里没有字符，请手动输入一个），不要保存。'
  if (-not (Ask-Enter '现在：开始菜单 → 用户头像 → 注销；Windows 提示“记事本有未保存的更改”时点“取消”中止注销，回到桌面后按 Enter。')) { Write-Host '已放弃。'; exit 0 }
  Start-Sleep -Seconds 3
  $after = @(Get-FusionHostState)
  $afterIds = ($after | ForEach-Object { $_.Id } | Sort-Object) -join ','
  Add-Result 'c2' '取消注销后的 Host 进程数' $after.Count
  Add-Result 'c2' '取消注销后的 Host pid' $afterIds
  Add-Result 'c2' '取消注销后仍是同一个 Host' ($afterIds -eq $beforeIds)
  Add-Result 'c2' '取消注销后输入仍可用' (Ask-YesNo '取消注销后，在记事本里输入中文是否仍然正常？')
  New-Snapshot 'after-cancelled-logoff'
  $state.stage = 'd'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# d. upgrade to wp4.2 with Notepad open (also Notepad A–E regression)
# ---------------------------------------------------------------------------

if ($state.stage -eq 'd') {
  Write-Head '步骤 d：在记事本保持打开的情况下升级到 wp4.2'
  Write-Host '请保持当前记事本窗口打开（不要关闭），然后按 Enter。升级会先请求旧 Host 有序停止，再安装新版本。'
  if (-not (Ask-Enter '记事本保持打开，按 Enter 开始升级（UAC）。')) { Write-Host '已放弃。'; exit 0 }
  New-Snapshot 'before-upgrade'
  $code = Invoke-ElevatedScript -ScriptPath $install2 -Arguments @('-PackageDir', $state.package2Path) -LogName 'install-wp4.2.log'
  Add-Result 'd' 'install.ps1 (wp4.2) 退出码' $code
  New-Snapshot 'after-upgrade'

  $hosts = @(Get-FusionHostState)
  Add-Result 'd' '升级后的 Host 进程数' $hosts.Count
  foreach ($process in $hosts) {
    Add-Result 'd' "Host pid=$($process.Id)" "path=$($process.Path) integrity=$($process.Integrity)"
  }
  Add-Result 'd' '新 Host 来自 wp4.2 目录' (@($hosts | Where-Object { $_.Path -and $_.Path.StartsWith($version2Dir, [StringComparison]::OrdinalIgnoreCase) }).Count)
  Add-Result 'd' 'wp4.1 目录是否仍存在' (Test-Path -LiteralPath $version1Dir)

  Write-Host ''
  Write-Host '现在请在“仍然打开的那个记事本窗口”里输入中文（它加载的是旧 DLL）。'
  Add-Result 'd' '旧记事本（旧 DLL）仍可输入' (Ask-YesNo '仍然打开的那个记事本，输入中文是否正常？')
  Write-Host '再新开一个记事本，输入中文（它加载的是新 DLL）。'
  Add-Result 'd' '新记事本（新 DLL）可输入' (Ask-YesNo '新开的记事本，输入中文是否正常？')

  Write-Host ''
  Write-Host 'Notepad A–E 回归（在新记事本里逐项确认）：'
  Add-Result 'd' 'A 连续输入' (Ask-YesNo 'A 连续输入 nihao+空格 得到“你好”，无重复/丢失字符？')
  Add-Result 'd' 'B 部分选择后继续输入' (Ask-YesNo 'B 选部分候选后继续输入，最终文字正确？')
  Add-Result 'd' 'C 标点与 QuickPhrase' (Ask-YesNo 'C 输入 , . ; 得到 Fcitx 标点，且输入 www. 出现 QuickPhrase 提示？')
  Add-Result 'd' 'D 键盘选词与翻页' (Ask-YesNo 'D 数字键、空格、- = 、PageUp/PageDown、方向键选词都正确？')
  Add-Result 'd' 'E 焦点切换' (Ask-YesNo 'E 输入中途切换窗口/焦点，无跨窗口上屏、无崩溃？')
  $state.stage = 'e'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# e. uninstall without -RemoveUserData
# ---------------------------------------------------------------------------

if ($state.stage -eq 'e') {
  Write-Head '步骤 e：卸载（保留用户数据）'
  Write-Host '脚本会请 Host 有序停止，然后反注册、删除计划任务、删除未被占用的版本目录。'
  Write-Host '%APPDATA%\Fcitx5 应保留。'
  if (-not (Ask-Enter '按 Enter 开始卸载（UAC）。')) { Write-Host '已放弃。'; exit 0 }
  $code = Invoke-ElevatedScript -ScriptPath $uninstall2 -Arguments @() -LogName 'uninstall-keep-data.log'
  Add-Result 'e' 'uninstall.ps1 退出码' $code
  New-Snapshot 'after-uninstall-keep-data'

  $stateNow = Get-FusionRegistrationState
  Add-Result 'e' 'HKLM COM 仍存在' (($stateNow | Where-Object { $_.Hive -eq 'HKLM' }).ComKey)
  Add-Result 'e' 'HKLM TIP 仍存在' (($stateNow | Where-Object { $_.Hive -eq 'HKLM' }).TipKey)
  Add-Result 'e' '计划任务仍存在' ([bool](Get-FusionScheduledTask))
  Add-Result 'e' 'Host 进程数' (@(Get-FusionHostState).Count)
  Add-Result 'e' '安装根目录仍存在' (Test-Path -LiteralPath $installRoot)
  Add-Result 'e' '%APPDATA%\Fcitx5 保留' (Test-Path -LiteralPath (Join-Path $env:APPDATA 'Fcitx5'))

  $checkState = Join-Path $repoRoot 'tools\tsf\check-state.ps1'
  $checkLog = Join-Path $script:runDir 'check-state-after-uninstall.txt'
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $checkState *>&1 | Tee-Object -FilePath $checkLog
  Add-Result 'e' 'check-state 输出已记录' $checkLog
  $state.stage = 'f'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# f. reinstall wp4.2 and uninstall with -RemoveUserData
# ---------------------------------------------------------------------------

if ($state.stage -eq 'f') {
  Write-Head '步骤 f：重装 wp4.2，再带 -RemoveUserData 卸载'
  Write-Host '请先关闭所有记事本窗口（否则版本目录会被占用而保留）。'
  if (-not (Ask-Enter '关闭所有记事本后按 Enter（UAC 两次）。')) { Write-Host '已放弃。'; exit 0 }
  $code = Invoke-ElevatedScript -ScriptPath $install2 -Arguments @('-PackageDir', $state.package2Path) -LogName 'reinstall-wp4.2.log'
  Add-Result 'f' '重装退出码' $code
  New-Snapshot 'after-reinstall-wp4.2'

  $code = Invoke-ElevatedScript -ScriptPath (Join-Path $version2Dir 'uninstall.ps1') -Arguments @('-RemoveUserData') -LogName 'uninstall-remove-data.log'
  Add-Result 'f' '带 -RemoveUserData 卸载退出码' $code
  New-Snapshot 'after-uninstall-remove-data'

  Add-Result 'f' 'HKLM COM 仍存在' ((Get-FusionRegistrationState | Where-Object { $_.Hive -eq 'HKLM' }).ComKey)
  Add-Result 'f' 'HKLM TIP 仍存在' ((Get-FusionRegistrationState | Where-Object { $_.Hive -eq 'HKLM' }).TipKey)
  Add-Result 'f' '计划任务仍存在' ([bool](Get-FusionScheduledTask))
  Add-Result 'f' 'Host 进程数' (@(Get-FusionHostState).Count)
  Add-Result 'f' '安装根目录仍存在' (Test-Path -LiteralPath $installRoot)
  Add-Result 'f' '%APPDATA%\Fcitx5 已删除' (-not (Test-Path -LiteralPath (Join-Path $env:APPDATA 'Fcitx5')))
  Add-Result 'f' '%LOCALAPPDATA%\Fcitx5 已删除' (-not (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'Fcitx5')))
  $state.stage = 'restore'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

# ---------------------------------------------------------------------------
# restore the development registration
# ---------------------------------------------------------------------------

if ($state.stage -eq 'restore') {
  Write-Head '恢复走查前的开发注册'
  $dev = $state.devRegistration
  if ($dev) {
    Write-Host "恢复：$dev"
    if (Test-Path -LiteralPath $dev) {
      $register = Join-Path $repoRoot 'tools\tsf\register.ps1'
      $code = Start-ElevatedProcess -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $register, '-DllPath', $dev)
      Add-Result 'restore' 'register.ps1 退出码' $code
    } else {
      Add-Result 'restore' '开发注册 DLL 不存在，无法恢复' $dev
    }
  } else {
    Write-Host '走查前没有开发注册，无需恢复。'
    Add-Result 'restore' '走查前没有开发注册' $true
  }
  New-Snapshot 'after-restore'
  $state.stage = 'done'
  $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

Write-Head '走查结束'
Write-Host "证据目录: $script:runDir"
Write-Host "汇总日志: $script:logPath"
Write-Host '请把该目录整体保留，用于写入 docs/windows-stage6-wp4.md。'
