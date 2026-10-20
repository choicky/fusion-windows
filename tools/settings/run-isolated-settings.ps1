param(
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [Parameter(Mandatory = $true)][string]$ProfileRoot,
  [Parameter(Mandatory = $true)][string]$PipeName,
  [string]$Screenshot = '',
  [switch]$NoInstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$ProfileRoot = [IO.Path]::GetFullPath($ProfileRoot)
New-Item -ItemType Directory -Force $ProfileRoot | Out-Null
$bin = Join-Path $BuildDir 'bin'
$hostExe = Join-Path $bin 'FusionHost.exe'
$settings = Join-Path $bin 'FusionSettings.exe'
foreach ($path in @($hostExe, $settings, (Join-Path $bin 'FusionFcitxHost.dll'))) {
  if (-not (Test-Path -LiteralPath $path)) { throw "Missing isolated product binary: $path" }
}
$env:APPDATA = Join-Path $ProfileRoot 'AppData\Roaming'
$env:LOCALAPPDATA = Join-Path $ProfileRoot 'AppData\Local'
$env:USERPROFILE = Join-Path $ProfileRoot 'User'
New-Item -ItemType Directory -Force $env:APPDATA,$env:LOCALAPPDATA,$env:USERPROFILE | Out-Null
$pipe = "\\.\pipe\$PipeName"
$env:PATH = "$bin;$env:PATH"
$psi = [Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $hostExe
$psi.Arguments = "--console --pipe-name `"$pipe`" --stop-event-scope `"settings-$PipeName`""
$psi.WorkingDirectory = $BuildDir
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$hostProcess = [Diagnostics.Process]::Start($psi)
try {
  $deadline = (Get-Date).AddSeconds(15)
  while ((Get-Date) -lt $deadline) {
    if ($hostProcess.HasExited) { throw "FusionHost exited early with code $($hostProcess.ExitCode)" }
    try {
      $probe = [IO.Pipes.NamedPipeClientStream]::new('.', $PipeName, [IO.Pipes.PipeDirection]::InOut)
      $probe.Connect(100)
      $probe.Dispose()
      break
    } catch { Start-Sleep -Milliseconds 100 }
  }
  if ((Get-Date) -ge $deadline) { throw 'FusionHost did not create the isolated settings pipe' }
  $args = "--pipe-name `"$pipe`""
  if ($Screenshot) { $args += " --screenshot `"$Screenshot`"" }
  $args += ' --self-test'
  $uiPsi = [Diagnostics.ProcessStartInfo]::new()
  $uiPsi.FileName = $settings
  $uiPsi.Arguments = $args
  $uiPsi.WorkingDirectory = $BuildDir
  $uiPsi.UseShellExecute = $false
  $uiPsi.CreateNoWindow = $true
  $uiProcess = [Diagnostics.Process]::Start($uiPsi)
  $uiProcess.WaitForExit()
  Write-Output "SETTINGS_EXIT=$($uiProcess.ExitCode)"
  if ($uiProcess.ExitCode -ne 0) { throw 'FusionSettings self-test failed' }
} finally {
  if ($hostProcess -and -not $hostProcess.HasExited) {
    $hostProcess.Kill()
    $hostProcess.WaitForExit(5000)
  }
}
