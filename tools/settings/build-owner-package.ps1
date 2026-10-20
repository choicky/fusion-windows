param(
  [Parameter(Mandatory = $true)][string]$BuildRoot,
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$RepoRoot = '',
  [string]$AqtExecutable = (Join-Path $env:USERPROFILE '.workbuddy-ai\binaries\python\envs\default\Scripts\aqt.exe'),
  [string]$VisualStudioRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community',
  [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
  [string]$WindowsSdkVersion = '10.0.26100.0',
  [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if (-not (Test-Path -LiteralPath $BuildRoot -PathType Container)) {
  throw "BuildRoot must be an existing task-local directory: $BuildRoot"
}
$nativeRoot = Join-Path $BuildRoot 'native'
$qtParent = Join-Path $BuildRoot 'qt'

$vsMsvc = Join-Path $VisualStudioRoot 'VC\Tools\MSVC\14.51.36231'
$cmake = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninja = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$ucrt = Join-Path $WindowsSdkRoot "Redist\$WindowsSdkVersion\ucrt\DLLs\x64"
$env:INCLUDE = "$vsMsvc\include;$WindowsSdkRoot\Include\$WindowsSdkVersion\ucrt;$WindowsSdkRoot\Include\$WindowsSdkVersion\shared;$WindowsSdkRoot\Include\$WindowsSdkVersion\um;$WindowsSdkRoot\Include\$WindowsSdkVersion\winrt;$WindowsSdkRoot\Include\$WindowsSdkVersion\cppwinrt"
$env:LIB = "$vsMsvc\lib\x64;$WindowsSdkRoot\Lib\$WindowsSdkVersion\ucrt\x64;$WindowsSdkRoot\Lib\$WindowsSdkVersion\um\x64"
$env:PATH = "$vsMsvc\bin\Hostx64\x64;$WindowsSdkRoot\bin\$WindowsSdkVersion\x64;$cmake;$ninja;$ucrt;$env:PATH"

Write-Host '== Preparing pinned Qt 6.8.3 (task-local)' -ForegroundColor Cyan
$qtOutput = @(& (Join-Path $PSScriptRoot 'prepare-qt.ps1') -Destination $qtParent -AqtExecutable $AqtExecutable)
$qtLine = $qtOutput | Where-Object { $_ -like 'QT_ROOT=*' } | Select-Object -Last 1
if (-not $qtLine) { throw 'Qt preparation did not return QT_ROOT' }
$qtRoot = $qtLine.Substring('QT_ROOT='.Length)

Write-Host '== Building the Fcitx runtime from pinned source' -ForegroundColor Cyan
& (Join-Path $RepoRoot 'tools\fcitx\build-native.ps1') -Configuration Release `
  -WorkRoot $nativeRoot -UseCurrentToolchainEnvironment
$runtimeRoot = Join-Path $nativeRoot 'runtime-release'
$PinnedNativeSourceRoot = Join-Path $nativeRoot 'upstream'
if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot 'bin\Fcitx5Utils.dll'))) {
  throw "Pinned Release runtime build did not produce Fcitx5Utils.dll: $runtimeRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $PinnedNativeSourceRoot 'fcitx5\.git'))) {
  throw "Pinned native source checkout missing: $PinnedNativeSourceRoot"
}
$vcpkgRoot = Join-Path $nativeRoot 'deps\vcpkg'

Write-Host '== Preparing pinned upstream UI sources and building FusionSettings.exe' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'build-settings.ps1') -BuildRoot $BuildRoot `
  -RuntimeRoot $runtimeRoot -QtRoot $qtRoot `
  -EcmRoot (Join-Path $vcpkgRoot 'installed\x64-windows') `
  -VcpkgRoot $vcpkgRoot -PinnedNativeSourceRoot $PinnedNativeSourceRoot `
  -RepoRoot $RepoRoot -Configuration Release `
  -VisualStudioRoot $VisualStudioRoot -WindowsSdkRoot $WindowsSdkRoot `
  -WindowsSdkVersion $WindowsSdkVersion

if (-not $OutputDirectory) { $OutputDirectory = Join-Path $BuildRoot 'packages' }
$buildDir = Join-Path $BuildRoot 'build-release'
$sourceRoot = Join-Path $BuildRoot 'settings-upstream'
& (Join-Path $RepoRoot 'tools\package\build-package.ps1') `
  -Version $Version -BuildDir $buildDir -RuntimeIdentity 'runtime-settings-pinned-release' `
  -UpstreamRoot $PinnedNativeSourceRoot -SettingsSourceRoot $sourceRoot `
  -VcpkgRoot $vcpkgRoot -OutDir $OutputDirectory -RepoRoot $RepoRoot
