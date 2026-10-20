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
  [string]$SettingsSourceRoot = '',
  [string]$VcpkgRoot = '',
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
    (Join-Path $binDir 'FusionSettings.exe'),
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

$temporaryRoot = Join-Path $OutDir ('.stage-fusion-' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $temporaryRoot 'package'
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
  foreach ($name in @('FusionHost.exe', 'FusionTray.exe', 'FusionSettings.exe')) {
    Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination (Join-Path $stage 'bin') -Force
  }
  foreach ($name in @('FusionHost.exe', 'FusionTray.exe', 'FusionSettings.exe', 'FusionTsf.dll', 'FusionFcitxHost.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage "bin\$name"))) {
      throw "Release build did not produce bin\$name"
    }
  }
  $qtPlugins = Join-Path $binDir 'plugins'
  if (-not (Test-Path -LiteralPath (Join-Path $qtPlugins 'platforms\qwindows.dll'))) {
    throw "Release build did not stage Qt's Windows platform plugin: $qtPlugins"
  }
  Copy-Item -LiteralPath $qtPlugins -Destination (Join-Path $stage 'bin\plugins') -Recurse -Force
  $qtConf = Join-Path $binDir 'qt.conf'
  if (-not (Test-Path -LiteralPath $qtConf)) { throw 'Release build did not stage bin\qt.conf' }
  Copy-Item -LiteralPath $qtConf -Destination (Join-Path $stage 'bin\qt.conf') -Force

  # data/ and lib/libime: the Fcitx data root the runtime resolves from ..\data.
  Copy-Item -LiteralPath $dataDir -Destination (Join-Path $stage 'data') -Recurse -Force
  $builtLocale = Join-Path $binDir 'data\locale'
  if (Test-Path -LiteralPath $builtLocale) {
    New-Item -ItemType Directory -Force (Join-Path $stage 'data\locale') | Out-Null
    Copy-Item -LiteralPath (Join-Path $builtLocale '*') -Destination (Join-Path $stage 'data\locale') -Recurse -Force
  }
  New-Item -ItemType Directory -Force (Join-Path $stage 'lib') | Out-Null
  Copy-Item -LiteralPath (Join-Path $libDir 'libime') -Destination (Join-Path $stage 'lib\libime') -Recurse -Force
  $runtime = [ordered]@{
    identity = $RuntimeIdentity
    machine = $env:COMPUTERNAME
    rebuilt = 'rebuilt from the pinned sources by tools/fcitx/build-native.ps1 for this owner-test package'
    patches = @('fcitx5-msvc.patch', 'chinese-addons-msvc.patch')
  }
  if ($UpstreamRoot) {
    New-Item -ItemType Directory -Force (Join-Path $stage 'licenses\runtime') | Out-Null
    $sourceAccess = Join-Path $RepoRoot 'third_party\settings-ui\SOURCE-ACCESS.txt'
    foreach ($pair in @(@('fcitx5','fcitx5'),@('fcitx5-chinese-addons','fcitx5-chinese-addons'),@('libime','libime'))) {
      $source = Join-Path $UpstreamRoot "$($pair[0])"
      if (-not (Test-Path -LiteralPath (Join-Path $source '.git'))) { throw "Pinned runtime source is missing: $source" }
      $licensePath = Join-Path $source 'LICENSES\LGPL-2.1-or-later.txt'
      if (-not (Test-Path -LiteralPath $licensePath)) { throw "Required LGPL license text is missing: $licensePath" }
      Copy-Item -LiteralPath $licensePath -Destination (Join-Path $stage "licenses\runtime\$($pair[1])-LGPL-2.1-or-later.txt") -Force
      $archive = Join-Path $stage "source\runtime\$($pair[1])-source.zip"
      $archiveParent = Split-Path -Parent $archive
      New-Item -ItemType Directory -Force $archiveParent | Out-Null
      $sourceCommit = (& git -C $source rev-parse HEAD).Trim()
      if ($LASTEXITCODE -ne 0) { throw "Cannot identify pinned source HEAD: $source" }
      $oldPreference = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'
      try { & git -C $source archive --format=zip --output=$archive $sourceCommit }
      finally { $ErrorActionPreference = $oldPreference }
      if ($LASTEXITCODE -ne 0) { throw "Could not create corresponding-source archive for $source" }
      $runtime[$pair[1]] = $sourceCommit
    }
    $runtime.sourceAccess = 'source/runtime/*-source.zip (pinned source)'
    $runtime.buildInstructions = 'licenses/settings-ui/SOURCE-ACCESS.txt'
  }

  # Package the exact upstream Chinese translation catalogs used by the Host and
  # build the settings catalog from its pinned upstream .po file.
  if (-not $SettingsSourceRoot -or -not (Test-Path -LiteralPath (Join-Path $SettingsSourceRoot 'fcitx5-configtool\po\fcitx5-configtool\zh_CN.po'))) {
    throw 'Pass the patch-applied pinned settings source tree as -SettingsSourceRoot'
  }
  if (-not $VcpkgRoot) { throw 'Pass the pinned vcpkg root as -VcpkgRoot to locate msgfmt.exe' }
  if (-not $UpstreamRoot) { throw 'Pass the pinned Fcitx upstream source root as -UpstreamRoot for the translation catalog' }
  $msgfmt = Join-Path $VcpkgRoot 'installed\x64-windows\tools\gettext\bin\msgfmt.exe'
  if (-not (Test-Path -LiteralPath $msgfmt)) { throw "Pinned gettext msgfmt.exe not found: $msgfmt" }
  $localeDir = Join-Path $stage 'data\locale\zh_CN\LC_MESSAGES'
  New-Item -ItemType Directory -Force $localeDir | Out-Null
  foreach ($translation in @(
      @((Join-Path $SettingsSourceRoot 'fcitx5-configtool\po\fcitx5-configtool\zh_CN.po'), 'fcitx5-configtool.mo'),
      @((Join-Path $UpstreamRoot 'fcitx5-chinese-addons\po\zh_CN.po'), 'fcitx5-chinese-addons.mo'))) {
    if (-not (Test-Path -LiteralPath $translation[0])) { throw "Pinned translation source missing: $($translation[0])" }
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $msgfmt -o (Join-Path $localeDir $translation[1]) $translation[0] }
    finally { $ErrorActionPreference = $oldPreference }
    if ($LASTEXITCODE -ne 0) { throw "msgfmt failed for $($translation[0]) with exit $LASTEXITCODE" }
  }

  # Ship scoped notices and the reproducible source patch inputs, not build binaries
  # or the full upstream repositories. SourceAccess.txt names exact upstream revisions.
  $noticeRoot = Join-Path $RepoRoot 'third_party\settings-ui'
  if (-not (Test-Path -LiteralPath (Join-Path $noticeRoot 'SOURCE-ACCESS.txt'))) {
    throw "Settings source-access and license notices are missing: $noticeRoot"
  }
  Copy-Item -LiteralPath $noticeRoot -Destination (Join-Path $stage 'licenses\settings-ui') -Recurse -Force
  New-Item -ItemType Directory -Force (Join-Path $stage 'source\settings-patches') | Out-Null
  foreach ($patch in @('fcitx5-qt.patch')) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot "cmake\patches\settings\$patch") `
      -Destination (Join-Path $stage "source\settings-patches\$patch") -Force
  }

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
  if ($dirty) { throw 'Owner-test package must be built from a clean committed fusion-windows worktree' }

  $settingsPins = Get-Content -LiteralPath (Join-Path $RepoRoot 'cmake\settings-ui\versions.cmake') -Raw
  $vcpkgMatch = [Regex]::Match($settingsPins, '(?m)^set\(FUSION_SETTINGS_VCPKG_COMMIT "([0-9a-f]{40})"\)$')
  if (-not $vcpkgMatch.Success) { throw 'Missing exact vcpkg commit pin' }
  $runtime.vcpkg = $vcpkgMatch.Groups[1].Value
  $runtime.msvc = '14.51.36231'
  $runtime.windowsSdk = '10.0.26100.0'
  foreach ($pair in @(@('fcitx5-configtool','FUSION_SETTINGS_CONFIGTOOL_COMMIT'),
      @('fcitx5-qt','FUSION_SETTINGS_FCITXQT_COMMIT'),
      @('fcitx5','FUSION_SETTINGS_FCITX5_COMMIT'),
      @('fcitx5-chinese-addons','FUSION_SETTINGS_CHINESE_ADDONS_COMMIT'),
      @('libime','FUSION_SETTINGS_LIBIME_COMMIT'))) {
    $match = [Regex]::Match($settingsPins, '(?m)^set\(' + [Regex]::Escape($pair[1]) + ' "([0-9a-f]{40})"\)$')
    if (-not $match.Success) { throw "Missing settings source pin $($pair[1])" }
    $runtime[$pair[0]] = $match.Groups[1].Value
  }
  $runtime.qt = '6.8.3 win64_msvc2022_64'
  $runtime.ecm = '6.30.0 build-time only'
  if (-not $UpstreamRoot) { throw 'The pinned native source root is required to provide corresponding runtime source' }
  foreach ($pair in @(@('fcitx5', 'fcitx5'), @('fcitx5-chinese-addons', 'chineseAddons'), @('libime', 'libime'))) {
    $path = Join-Path $UpstreamRoot $pair[0]
    if (-not (Test-Path -LiteralPath (Join-Path $path '.git'))) { throw "Pinned upstream source checkout missing: $path" }
    $head = (& git -C $path rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw "Unable to identify upstream source HEAD: $path" }
    $expectedRuntimePin = if ($pair[1] -eq 'chineseAddons') { $runtime['fcitx5-chinese-addons'] } else { $runtime[$pair[1]] }
    if (([string]$head).Trim() -ne $expectedRuntimePin) {
      throw "Upstream checkout head does not match pinned source $($pair[1]): $path (expected $expectedRuntimePin, got $(([string]$head).Trim()))"
    }
    $runtime[$pair[1]] = ([string]$head).Trim()
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
  settings      = [ordered]@{
    implementationStatus = 'implemented; isolated-product-validated when recorded by the delivery report'
    experimentalResult   = 'not owner-installed or owner-accepted; owner-test package only'
    selectionUi          = 'official InputMethodManager group APIs; Items/0 selected, DefaultIM preserved as fallback'
    applySemantics       = 'official inputmethod/<name> schema and setConfigForInputMethod; changed apply checks contexts and preserves unexposed values'
    sourceCommit         = 'covered by package sourceCommit'
    repository           = 'https://github.com/choicky/fcitx5-configtool'
    branch               = 'windows/fusion-settings'
    commit               = '0234c64b2b66f7dcb0d03e1997310d34a3ae43b0'
    sourceAccess         = 'licenses/settings-ui/SOURCE-ACCESS.txt'
  }
  layout        = 'bin\ (Host + Tray + Settings + TSF DLL + runtime/Qt/plugins), Fcitx data under bin\data\, lib\libime\, licenses\, source\'
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
  Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
