# Stage 6 WP4 A4: assemble an owner-only test package from a fresh Release build.
#
# The package is a zip whose root maps directly onto the installed version
# directory (C:\Program Files\Fcitx5 Fusion\<version>\):
#
#   bin\        FusionHost.exe, FusionTray.exe, FusionTsf.dll, FusionFcitxHost.dll and every
#               runtime DLL the Host loads (all beside the executable, so no PATH
#               change is needed and Fcitx resolves its data from ..\data)
#   data\       fcitx5\ (addon/input-method definitions, pinyin data) + libime\
#   lib\libime\ zh_CN.lm
#   tools\      register.ps1 / unregister.ps1 / check-state.ps1 (reused as-is)
#   install.ps1, uninstall.ps1
#   package.json  manifest: version, source commit, runtime identity and the
#                 SHA-256 of every file
#
# No MSI, no installer toolchain, no signing: the owner-only test package decided
# with U4 at WP8. Versions are provisional test identifiers, not a versioning
# policy.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-package.ps1 `
#     -Version 0.0.0-wp4.1+572fdb9 -BuildDir <release-build-dir>

param(
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [string]$RuntimeIdentity = 'runtime-wp3-20261018',
  [string]$UpstreamRoot = '',
  [string]$OutDir = '',
  [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z.+-]*$') {
  throw "Version must be a plain identifier (letters, digits, dot, plus, hyphen): $Version"
}
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'build-packages' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$binDir = Join-Path $BuildDir 'bin'
$dataDir = Join-Path $BuildDir 'data'
$libDir = Join-Path $BuildDir 'lib'
foreach ($required in @(
    (Join-Path $binDir 'FusionHost.exe'),
    (Join-Path $binDir 'FusionTray.exe'),
    (Join-Path $binDir 'FusionTsf.dll'),
    (Join-Path $binDir 'FusionFcitxHost.dll'),
    (Join-Path $binDir 'Fcitx5Core.dll'),
    (Join-Path $binDir 'Fcitx5Utils.dll'),
    (Join-Path $dataDir 'fcitx5\addon\pinyin.conf'),
    (Join-Path $dataDir 'fcitx5\inputmethod\pinyin.conf'),
    (Join-Path $dataDir 'fcitx5\inputmethod\shuangpin.conf'),
    (Join-Path $dataDir 'libime\sc.dict'),
    (Join-Path $libDir 'libime\zh_CN.lm'))) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Release build is incomplete, missing: $required"
  }
}

$stage = Join-Path ([IO.Path]::GetTempPath()) ('fusion-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $stage | Out-Null
try {
  New-Item -ItemType Directory -Force (Join-Path $stage 'bin') | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null

  # bin: the Host, the TSF DLL, the Host's own module and every runtime DLL. The
  # post-build step already staged the runtime beside the binaries, so this is
  # exactly the set the Host resolves without any PATH change (A3). Test
  # executables and the development activation helper are not product files and
  # are deliberately left out.
  foreach ($file in Get-ChildItem -LiteralPath $binDir -File -Filter '*.dll') {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $stage 'bin') -Force
  }
  foreach ($name in @('FusionHost.exe', 'FusionTray.exe')) {
    Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination (Join-Path $stage 'bin') -Force
  }
  foreach ($name in @('FusionHost.exe', 'FusionTray.exe', 'FusionTsf.dll', 'FusionFcitxHost.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage "bin\$name"))) {
      throw "Release build did not produce bin\$name"
    }
  }

  # data/ and lib/libime: the Fcitx data root the runtime resolves from ..\data.
  Copy-Item -LiteralPath $dataDir -Destination (Join-Path $stage 'data') -Recurse -Force
  New-Item -ItemType Directory -Force (Join-Path $stage 'lib') | Out-Null
  Copy-Item -LiteralPath (Join-Path $libDir 'libime') -Destination (Join-Path $stage 'lib\libime') -Recurse -Force

  # Reuse the existing registration scripts instead of duplicating their logic.
  foreach ($script in @('register.ps1', 'unregister.ps1', 'check-state.ps1')) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot "tools\tsf\$script") -Destination (Join-Path $stage 'tools') -Force
  }
  foreach ($script in @('install.ps1', 'uninstall.ps1', 'host-state.ps1', 'restart-host.ps1',
      'host-diagnostics.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $stage -Force
  }

  # git is a native executable, so $LASTEXITCODE *is* the contract here - unlike a
  # .ps1 call. The exit code is checked before the output is used.
  $commit = (& git -C $RepoRoot rev-parse HEAD)
  if ($LASTEXITCODE -ne 0) { throw 'git rev-parse HEAD failed' }
  $commit = ([string]$commit).Trim()
  $dirty = (& git -C $RepoRoot status --porcelain)
  if ($LASTEXITCODE -ne 0) { throw 'git status failed' }

  $runtime = [ordered]@{
    identity = $RuntimeIdentity
    # Which machine produced this runtime. `runtime-wp3-20261018` exists as two
    # builds from the same pins: WP3's on the 7700 and this one. They are not byte
    # identical, so the machine and the file hashes below are the identity, never the
    # directory name alone.
    machine  = $env:COMPUTERNAME
    rebuilt  = 'rebuilt on this machine from the WP3 pins; binary hashes differ from the 7700 build'
    patches  = @('fcitx5-msvc.patch', 'chinese-addons-msvc.patch')
  }
  if ($UpstreamRoot) {
    foreach ($pair in @(@('fcitx5', 'fcitx5'), @('fcitx5-chinese-addons', 'chineseAddons'), @('libime', 'libime'))) {
      $path = Join-Path $UpstreamRoot $pair[0]
      if (Test-Path -LiteralPath (Join-Path $path '.git')) {
        $head = (& git -C $path rev-parse HEAD)
        if ($LASTEXITCODE -eq 0) { $runtime[$pair[1]] = ([string]$head).Trim() }
      }
    }
  }

  $files = @()
  foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
    $relative = $file.FullName.Substring($stage.Length + 1) -replace '\\', '/'
    if ($relative -eq 'package.json') { continue }
    $files += [ordered]@{
      path   = $relative
      size   = $file.Length
      sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
  }

  $manifest = [ordered]@{
    package       = 'fusion-windows'
    version       = $Version
    sourceCommit  = $commit
    sourceDirty   = [bool]$dirty
    builtAt       = (Get-Date).ToString('o')
    builtOn       = $env:COMPUTERNAME
    configuration = 'Release'
    runtime       = $runtime
    layout        = 'bin\ (Host + TSF DLL + runtime DLLs), data\, lib\libime\'
    files         = $files
  }
  $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'package.json') -Encoding UTF8

  $zip = Join-Path $OutDir ("fusion-$Version.zip")
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

  Write-Host "Package: $zip"
  Write-Host ("Zip SHA-256: {0}" -f (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
  Write-Host ("Files: {0}; source commit: {1}{2}" -f $files.Count, $commit, $(if ($dirty) { ' (DIRTY WORKING TREE)' } else { '' }))
  Write-Host ("Runtime: {0}" -f $RuntimeIdentity)
  Write-Host '--- package.json ---'
  Get-Content -LiteralPath (Join-Path $stage 'package.json') -Raw | Write-Host
} finally {
  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
