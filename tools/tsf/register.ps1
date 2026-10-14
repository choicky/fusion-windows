# Stage 2.5 development registration (elevated, reversible).
#
# Runs regsvr32 elevated on the FusionTsf development TSF text service.
# Elevation is required: persistent cross-process TIP registration writes
# HKLM\Software\Microsoft\CTF\TIP plus the HKLM COM CLSID key (D072 §14
# registration observation). Unregister with unregister.ps1; nothing else
# persists on the machine.
#
# Usage: powershell -ExecutionPolicy Bypass -File register.ps1 <FusionTsf.dll>

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
$psi.Arguments = '/s "' + $dll.Path + '"'
$psi.UseShellExecute = $true
$psi.Verb = 'runas'
$process = [System.Diagnostics.Process]::Start($psi)
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
  Write-Host ("regsvr32 exited with 0x{0:X8} (declined elevation or registration error)" -f $process.ExitCode)
  exit 1
}
Write-Host "Registered (elevated): $($dll.Path)"
