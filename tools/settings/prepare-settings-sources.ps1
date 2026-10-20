param(
  [Parameter(Mandatory = $true)][string]$BuildRoot,
  [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$sourceRoot = Join-Path $BuildRoot 'settings-upstream'
New-Item -ItemType Directory -Force $sourceRoot | Out-Null
$pinsPath = Join-Path $RepoRoot 'cmake\settings-ui\versions.cmake'
$pinText = Get-Content -LiteralPath $pinsPath -Raw
function Get-Pin([string]$Name) {
  $pattern = '(?m)^set\(' + [Regex]::Escape($Name) + ' "([0-9a-f]{40})"\)$'
  $match = [Regex]::Match($pinText, $pattern)
  if (-not $match.Success) { throw "Missing exact 40-hex pin: $Name" }
  return $match.Groups[1].Value
}
function Invoke-Git([string[]]$Arguments) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & git @Arguments }
  finally { $ErrorActionPreference = $old }
  if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed with exit $LASTEXITCODE" }
}
function Get-PinnedCheckout([string]$Name, [string]$Url, [string]$Pin, [string]$AllowedPatch = '') {
  $path = Join-Path $sourceRoot $Name
  $created = $false
  if (-not (Test-Path -LiteralPath (Join-Path $path '.git'))) {
    if (Test-Path -LiteralPath $path) { throw "Refusing existing non-checkout source path: $path" }
    Invoke-Git @('clone','--no-checkout',$Url,$path)
    $created = $true
  }
  $remote = (& git -C $path remote get-url origin).Trim()
  if ($LASTEXITCODE -ne 0 -or $remote -ne $Url) { throw "Unexpected origin in $path" }
  if ($created) {
    Invoke-Git @('-C',$path,'fetch','origin',$Pin)
    Invoke-Git @('-C',$path,'checkout','--detach',$Pin)
  } else {
    $status = @(& git -C $path status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect working tree: $path" }
    if ($status.Count -gt 0 -and (-not $AllowedPatch -or -not (Test-PatchApplied $path $AllowedPatch))) {
      throw "Preserve and inspect modified upstream source before refreshing: $path"
    }
    $head = (& git -C $path rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -ne $Pin) {
      Invoke-Git @('-C',$path,'fetch','origin',$Pin)
      Invoke-Git @('-C',$path,'checkout','--detach',$Pin)
    }
  }
  $head = (& git -C $path rev-parse HEAD).Trim()
  if ($head -ne $Pin) { throw "Pinned source mismatch at ${path}: $head" }
  return $path
}
function Test-PatchApplied([string]$Path, [string]$Patch) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & git -C $Path apply --reverse --check $Patch 2>&1 | Out-Null }
  finally { $ErrorActionPreference = $old }
  return ($LASTEXITCODE -eq 0)
}
function Apply-Patch([string]$Path, [string]$Patch) {
  if (Test-PatchApplied $Path $Patch) { return }
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & git -C $Path apply --check $Patch }
  finally { $ErrorActionPreference = $old }
  if ($LASTEXITCODE -ne 0) { throw "Patch check failed: $Patch" }
  Invoke-Git @('-C',$Path,'apply',$Patch)
}
$configtoolRepository = [Regex]::Match($pinText, '(?m)^set\(FUSION_SETTINGS_CONFIGTOOL_REPOSITORY "([^"]+)"\)$')
if (-not $configtoolRepository.Success) { throw 'Missing owner-fork repository URL pin' }
$configtool = Get-PinnedCheckout 'fcitx5-configtool' $configtoolRepository.Groups[1].Value `
  (Get-Pin 'FUSION_SETTINGS_CONFIGTOOL_COMMIT')
$configtoolBase = Get-Pin 'FUSION_SETTINGS_CONFIGTOOL_BASE_COMMIT'
$old = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & git -C $configtool merge-base --is-ancestor $configtoolBase (Get-Pin 'FUSION_SETTINGS_CONFIGTOOL_COMMIT') }
finally { $ErrorActionPreference = $old }
if ($LASTEXITCODE -ne 0) { throw 'The pinned configtool fork commit does not descend from the reviewed upstream revision' }
$fcitxqtPatch = Join-Path $RepoRoot 'cmake\patches\settings\fcitx5-qt.patch'
$fcitxqt = Get-PinnedCheckout 'fcitx5-qt' 'https://github.com/fcitx/fcitx5-qt.git' `
  (Get-Pin 'FUSION_SETTINGS_FCITXQT_COMMIT') $fcitxqtPatch
Apply-Patch $fcitxqt $fcitxqtPatch
Write-Output "FUSION_SETTINGS_SOURCE_ROOT=$sourceRoot"
Write-Output "fcitx5-configtool=$((git -C $configtool rev-parse HEAD).Trim())"
Write-Output "fcitx5-qt=$((git -C $fcitxqt rev-parse HEAD).Trim())"
