# Stage 2.5 development unregistration (elevated, reversible).
#
# Removes everything register.ps1 created: TIP profile/category/COM class
# registration plus the HKLM COM CLSID key. Verify cleanup afterwards with
# check-state.ps1.
#
# Usage: powershell -ExecutionPolicy Bypass -File unregister.ps1 <FusionTsf.dll>

param(
  [Parameter(Mandatory = $true)]
  [string]$DllPath
)

$ErrorActionPreference = 'Stop'
$dll = Resolve-Path $DllPath
if (-not $dll) {
  Write-Error "DLL not found: $DllPath"
  exit 2
}
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
