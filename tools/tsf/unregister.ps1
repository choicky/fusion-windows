# Stage 2.5 development unregistration (elevated, reversible).
#
# Removes everything register.ps1 created: TIP profile/category/COM class
# registration plus the HKLM COM CLSID key. Verify cleanup afterwards with
# check-state.ps1.
#
# Exit contract (relied on by uninstall.ps1 and verify-wp4.ps1): 0 on success,
# 1 when regsvr32 failed, 2 when the DLL is missing. The script always exits.
#
# Usage: powershell -ExecutionPolicy Bypass -File unregister.ps1 <FusionTsf.dll>

param(
  [Parameter(Mandatory = $true)]
  [string]$DllPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DllPath)) {
  Write-Host "DLL not found: $DllPath" -ForegroundColor Red
  exit 2
}
$dll = Resolve-Path -LiteralPath $DllPath

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'regsvr32.exe'
$psi.Arguments = '/s /u "' + $dll.Path + '"'
$psi.UseShellExecute = $true
$psi.Verb = 'runas'
$process = [System.Diagnostics.Process]::Start($psi)
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
  Write-Host ("regsvr32 exited with 0x{0:X8} (declined elevation or unregistration error)" -f $process.ExitCode)
  exit 1
}
Write-Host "Unregistered (elevated): $($dll.Path)"
exit 0
