param(
  [Parameter(Mandatory = $true)][string]$Destination,
  [string]$AqtExecutable = (Join-Path $env:USERPROFILE '.workbuddy-ai\binaries\python\envs\default\Scripts\aqt.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not (Test-Path -LiteralPath $AqtExecutable -PathType Leaf)) {
  throw "aqtinstall 3.3.0 executable not found: $AqtExecutable. Install it inside the managed Python environment, not globally."
}
$aqtVersion = (& $AqtExecutable --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $aqtVersion -notmatch '3\.3\.0') {
  throw "Expected pinned aqtinstall 3.3.0; found '$aqtVersion'"
}
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force $Destination | Out-Null
$expected = Join-Path $Destination '6.8.3\msvc2022_64'
if (Test-Path -LiteralPath (Join-Path $expected 'bin\Qt6Core.dll')) {
  Write-Output "QT_ROOT=$expected"
  return
}
$old = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & $AqtExecutable install-qt windows desktop 6.8.3 win64_msvc2022_64 -O $Destination --archives qtbase }
finally { $ErrorActionPreference = $old }
if ($LASTEXITCODE -ne 0) { throw "aqtinstall failed with exit $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath (Join-Path $expected 'bin\Qt6Core.dll'))) {
  throw "Qt 6.8.3 MSVC runtime was not installed at $expected"
}
Write-Output "QT_ROOT=$expected"
