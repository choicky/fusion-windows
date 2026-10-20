param(
  [Parameter(Mandatory = $true)][string]$BuildRoot,
  [Parameter(Mandatory = $true)][string]$RuntimeRoot,
  [Parameter(Mandatory = $true)][string]$QtRoot,
  [Parameter(Mandatory = $true)][string]$EcmRoot,
  [Parameter(Mandatory = $true)][string]$VcpkgRoot,
  [Parameter(Mandatory = $true)][string]$PinnedNativeSourceRoot,
  [string]$RepoRoot = '',
  [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
  [string]$VisualStudioRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community',
  [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
  [string]$WindowsSdkVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
foreach ($path in @($RuntimeRoot,$QtRoot,$EcmRoot,$VcpkgRoot,$PinnedNativeSourceRoot)) {
  if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Required pinned dependency root not found: $path" }
}
$msvc = Join-Path $VisualStudioRoot 'VC\Tools\MSVC\14.51.36231'
$cmake = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$rc = Join-Path $WindowsSdkRoot "bin\$WindowsSdkVersion\x64\rc.exe"
$mt = Join-Path $WindowsSdkRoot "bin\$WindowsSdkVersion\x64\mt.exe"
foreach ($tool in @($cmake,$ninja,$rc,$mt,(Join-Path $msvc 'bin\Hostx64\x64\cl.exe'))) {
  if (-not (Test-Path -LiteralPath $tool)) { throw "Required build tool not found: $tool" }
}

# Task-local environment only. Nothing is written to the machine/user PATH.
$env:INCLUDE = "$msvc\include;$WindowsSdkRoot\Include\$WindowsSdkVersion\ucrt;$WindowsSdkRoot\Include\$WindowsSdkVersion\shared;$WindowsSdkRoot\Include\$WindowsSdkVersion\um;$WindowsSdkRoot\Include\$WindowsSdkVersion\winrt;$WindowsSdkRoot\Include\$WindowsSdkVersion\cppwinrt"
$env:LIB = "$msvc\lib\x64;$WindowsSdkRoot\Lib\$WindowsSdkVersion\ucrt\x64;$WindowsSdkRoot\Lib\$WindowsSdkVersion\um\x64"
$env:PATH = "$msvc\bin\Hostx64\x64;$WindowsSdkRoot\bin\$WindowsSdkVersion\x64;$(Split-Path $cmake);$(Split-Path $ninja);$env:PATH"

$sourceOutput = @(& (Join-Path $PSScriptRoot 'prepare-settings-sources.ps1') -BuildRoot $BuildRoot -RepoRoot $RepoRoot)
$sourceLine = $sourceOutput | Where-Object { $_ -like 'FUSION_SETTINGS_SOURCE_ROOT=*' } | Select-Object -Last 1
if (-not $sourceLine) { throw 'The source preparation script did not report its pinned source root' }
$sourceRoot = $sourceLine.Substring('FUSION_SETTINGS_SOURCE_ROOT='.Length)
$buildDir = Join-Path $BuildRoot ('build-' + $Configuration.ToLowerInvariant())
New-Item -ItemType Directory -Force $BuildRoot | Out-Null
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
$installed = Join-Path $VcpkgRoot 'installed\x64-windows'
$prefix = "$QtRoot;$EcmRoot;$RuntimeRoot;$installed"
$configure = @(
  '-S', $RepoRoot, '-B', $buildDir, '-G', 'Ninja',
  "-DCMAKE_MAKE_PROGRAM=$ninja",
  "-DCMAKE_CXX_COMPILER=$(Join-Path $msvc 'bin\Hostx64\x64\cl.exe')",
  "-DCMAKE_RC_COMPILER=$($rc -replace '\\','/')",
  "-DCMAKE_MT=$($mt -replace '\\','/')",
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
  "-DCMAKE_PREFIX_PATH=$prefix",
  "-DBoost_INCLUDE_DIR=$(Join-Path $installed 'include')",
  "-DPThreads4W_INCLUDE_DIR=$(Join-Path $installed 'include')",
  "-DPThreads4W_LIBRARY_RELEASE=$(Join-Path $installed 'lib\pthreadVC3.lib')",
  "-DPThreads4W_CXXEXC_LIBRARY_RELEASE=$(Join-Path $installed 'lib\pthreadVCE3.lib')",
  "-DPThreads4W_STRUCTEXC_LIBRARY_RELEASE=$(Join-Path $installed 'lib\pthreadVSE3.lib')",
  "-DCMAKE_BUILD_TYPE=$Configuration",
  '-DFUSION_WITH_FCITX5=ON', "-DFUSION_FCITX5_ROOT=$RuntimeRoot",
  '-DFUSION_WITH_SETTINGS_UI=ON', "-DFUSION_SETTINGS_SOURCE_ROOT=$sourceRoot",
  "-DFUSION_SETTINGS_NATIVE_SOURCE_ROOT=$PinnedNativeSourceRoot",
  '-DBUILD_TESTING=ON')
$old = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & $cmake @configure }
finally { $ErrorActionPreference = $old }
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit $LASTEXITCODE" }
$old = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & $cmake --build $buildDir --config $Configuration --target FusionHostExe FusionTrayExe FusionSettingsExe stage4a }
finally { $ErrorActionPreference = $old }
if ($LASTEXITCODE -ne 0) { throw "Release product build failed with exit $LASTEXITCODE" }

# Reuse upstream Simplified-Chinese catalogs (configtool labels and Fcitx Chinese
# addon/schema strings) for the local product build and for the packaged ZIP.
$gettextBin = Join-Path $VcpkgRoot 'installed\x64-windows\tools\gettext\bin'
$gettextRuntime = Join-Path $VcpkgRoot 'installed\x64-windows\bin'
$env:PATH = "$gettextBin;$gettextRuntime;$env:PATH"
$msgfmt = Join-Path $gettextBin 'msgfmt.exe'
$localeDir = Join-Path $buildDir 'data\locale\zh_CN\LC_MESSAGES'
New-Item -ItemType Directory -Force $localeDir | Out-Null
$nativeRoot = Split-Path -Parent $RuntimeRoot
$catalogs = @(
  @((Join-Path $sourceRoot 'fcitx5-configtool\po\fcitx5-configtool\zh_CN.po'), 'fcitx5-configtool.mo'),
  @((Join-Path $nativeRoot 'upstream\fcitx5-chinese-addons\po\zh_CN.po'), 'fcitx5-chinese-addons.mo'))
foreach ($catalog in $catalogs) {
  if (-not (Test-Path -LiteralPath $catalog[0])) { throw "Pinned translation catalog missing: $($catalog[0])" }
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $msgfmt -o (Join-Path $localeDir $catalog[1]) $catalog[0] }
  finally { $ErrorActionPreference = $old }
  if ($LASTEXITCODE -ne 0) { throw "msgfmt failed for $($catalog[0]) with exit $LASTEXITCODE" }
}
Write-Output "BUILD_DIR=$buildDir"
Write-Output "SOURCE_ROOT=$sourceRoot"
