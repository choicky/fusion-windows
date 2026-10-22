# Build the single-EXE installer from an owner-test package.
#
# The EXE is a thin distribution wrapper: it carries the package that
# tools/package/build-package.ps1 produced and runs that package's own
# install.ps1 / uninstall.ps1. Nothing about the install lifecycle is
# reimplemented here or in the .nsi - see the header of fusion-installer.nsi.
#
# Two inputs are enough:
#   -PackageZip  an existing fusion-<version>.zip (from build-package.ps1), or
#   -BuildDir / -SettingsAppDir / ... to call build-package.ps1 first.
#
# Usage (from a clean committed worktree):
#   powershell -ExecutionPolicy Bypass -File tools\installer\build-installer.ps1 `
#     -PackageZip build-packages\fusion-0.0.0-winui3+85084f3.zip `
#     -NsisRoot C:\path\to\nsis-3.11
#
# NSIS 3 is required and is not vendored: pass -NsisRoot (the directory that
# contains makensis.exe) or set NSIS_ROOT.

param(
  [string]$PackageZip = '',
  [string]$PayloadDir = '',
  [string]$OutDir = '',
  [string]$NsisRoot = $env:NSIS_ROOT,
  [string]$RepoRoot = '',
  [string]$StageDir = '',
  # Forwarded to build-package.ps1 when neither -PackageZip nor -PayloadDir is given.
  [string]$BuildDir = '',
  [string]$SettingsAppDir = '',
  [string]$RuntimeIdentity = 'runtime-wp3-20261018',
  [string]$UpstreamRoot = '',
  [string]$VcpkgRoot = '',
  [string]$SettingsSourceRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
# ZipFile is used instead of Expand-Archive: 652 files and 139 MB go through it in
# seconds, and it keeps the entry names exactly as the package wrote them. The
# assembly has to be named explicitly under Windows PowerShell.
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'build-packages' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

# The package manifest already refuses a dirty worktree; this refuses one earlier,
# so an EXE built here cannot name a commit whose content it does not have.
$dirty = (& git -C $RepoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'git status failed' }
if ($dirty -and -not $PayloadDir) { throw 'Build the installer from a clean committed worktree' }

if (-not $NsisRoot) { throw 'Pass -NsisRoot <directory containing makensis.exe> or set NSIS_ROOT' }
$makensis = Join-Path $NsisRoot 'makensis.exe'
if (-not (Test-Path -LiteralPath $makensis)) { throw "makensis.exe not found: $makensis" }
$nsisVersion = (& $makensis /VERSION 2>&1 | Out-String).Trim()
if ($nsisVersion -notmatch '^v3\.') { throw "Unexpected makensis version output: $nsisVersion" }
Write-Host "NSIS:     $nsisVersion ($makensis)"

if (-not $StageDir) { $StageDir = Join-Path ([IO.Path]::GetTempPath()) ('fusion-installer-' + [Guid]::NewGuid().ToString('N')) }
New-Item -ItemType Directory -Force $StageDir | Out-Null
$payload = if ($PayloadDir) { (Resolve-Path -LiteralPath $PayloadDir).Path } else { Join-Path $StageDir 'payload' }

try {
  if (-not $PayloadDir) {
    if (-not $PackageZip) {
      if (-not $BuildDir -or -not $SettingsAppDir) {
        throw 'Pass -PackageZip, or -PayloadDir, or -BuildDir with -SettingsAppDir so build-package.ps1 can run'
      }
      Write-Host 'Building the owner-test package first...'
      $packageArgs = @{
        BuildDir = $BuildDir; SettingsAppDir = $SettingsAppDir; RuntimeIdentity = $RuntimeIdentity
        OutDir = $OutDir; RepoRoot = $RepoRoot
      }
      if ($UpstreamRoot) { $packageArgs.UpstreamRoot = $UpstreamRoot }
      if ($VcpkgRoot) { $packageArgs.VcpkgRoot = $VcpkgRoot }
      if ($SettingsSourceRoot) { $packageArgs.SettingsSourceRoot = $SettingsSourceRoot }
      & (Join-Path $PSScriptRoot '..\package\build-package.ps1') @packageArgs | Out-Null
      $PackageZip = (Get-ChildItem -LiteralPath $OutDir -Filter 'fusion-*.zip' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
      if (-not $PackageZip) { throw "No package zip was produced in $OutDir" }
    }
    $PackageZip = (Resolve-Path -LiteralPath $PackageZip).Path
    Write-Host "Package:  $PackageZip"
    if (Test-Path -LiteralPath $payload) { Remove-Item -LiteralPath $payload -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($PackageZip, $payload)
  } else {
    Write-Host "Payload:  $payload"
  }

  $manifestPath = Join-Path $payload 'package.json'
  if (-not (Test-Path -LiteralPath $manifestPath)) { throw "No package.json in the payload: $payload" }
  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  $version = $manifest.version
  if ($version -notmatch '^[0-9A-Za-z][0-9A-Za-z.+-]*$') { throw "Refusing unexpected version identifier: $version" }
  if ($manifest.sourceDirty) { throw 'The payload records a dirty source tree; an owner-test EXE must name a clean commit' }
  Write-Host "Version:  $version"
  Write-Host "Source:   $($manifest.sourceCommit)"

  # Verify the payload with the product's own verifier - the same script and the
  # same manifest check the EXE will run at install time, so this is not a second
  # opinion but the real thing, exercised before the 139 MB is compressed.
  Write-Host 'Verifying the payload against package.json...'
  $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $verify = & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $payload 'install.ps1') `
    -PackageDir $payload -VerifyOnly 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "install.ps1 -VerifyOnly failed:`n$verify" }
  $verified = ($verify -split "`r?`n" | Where-Object { $_ -match 'Package verified:' } | Select-Object -Last 1)
  if (-not $verified) { throw "install.ps1 -VerifyOnly did not report a result:`n$verify" }
  Write-Host ("  " + $verified.Trim())

  # The PE version resource needs four numbers; the fourth is the first four hex
  # digits of the source commit, so two builds of different commits never share
  # one. The name and version a user sees are the package's own, above.
  $build = [Convert]::ToInt32(($manifest.sourceCommit.Substring(0, 4)), 16)
  $outFile = Join-Path $OutDir ("fusion-$version-setup.exe")
  if (Test-Path -LiteralPath $outFile) { Remove-Item -LiteralPath $outFile -Force }

  Write-Host 'Compiling the installer...'
  # The defines carrying paths are quoted: a payload under a user name with a space
  # would otherwise split at the space and pack a directory that does not exist.
  $nsisArgs = @(
    '/INPUTCHARSET', 'UTF8',
    "/DPRODUCT_VERSION=$version",
    "/DPRODUCT_BUILD=$build",
    "/DPAYLOAD_DIR=`"$payload`"",
    "/DOUT_FILE=`"$outFile`"",
    (Join-Path $PSScriptRoot 'fusion-installer.nsi')
  )
  $nsisLog = & $makensis '/V3' @nsisArgs 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw ("makensis failed with exit code {0}:`n{1}" -f $LASTEXITCODE, $nsisLog) }
  # Kept as evidence that this build packed this payload, file by file.
  $buildLog = [IO.Path]::ChangeExtension($outFile, '.build.log')
  Set-Content -LiteralPath $buildLog -Value $nsisLog -Encoding UTF8
  # Warnings matter here: a duplicate define or an unhandled page would mean the
  # script does not do what it says.
  foreach ($line in ($nsisLog -split "`r?`n" | Where-Object { $_ -match 'warning:|error:' })) {
    Write-Host "  $line"
  }
  if (-not (Test-Path -LiteralPath $outFile)) { throw "makensis reported success but $outFile does not exist" }

  $info = Get-Item -LiteralPath $outFile
  $hash = (Get-FileHash -LiteralPath $outFile -Algorithm SHA256).Hash
  Write-Host ''
  Write-Host "Installer: $($info.FullName)"
  Write-Host ("Size:      {0} bytes" -f $info.Length)
  Write-Host "SHA-256:   $hash"
  Write-Host "Version:   $version"
  Write-Host "Source:    $($manifest.sourceCommit)"
  Write-Host "Manifest:  $($manifest.files.Count) files verified before packaging"
  Write-Host "Build log: $buildLog"
} finally {
  if (-not $PayloadDir -and (Test-Path -LiteralPath $StageDir)) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force -ErrorAction SilentlyContinue
  }
}
