# Stage 2.5 development registration (elevated, reversible).
#
# Runs regsvr32 elevated on the FusionTsf TSF text service.
# Elevation is required: persistent cross-process TIP registration writes
# HKLM\Software\Microsoft\CTF\TIP plus the HKLM COM CLSID key (D072 §14
# registration observation). Unregister with unregister.ps1; nothing else
# persists on the machine.
#
# Exit contract (relied on by install.ps1 and verify-wp4.ps1): 0 on success,
# 1 when regsvr32 failed, 2 when the DLL is missing. The script always exits, so a
# caller never has to guess whether a status exists.
#
# Usage: powershell -ExecutionPolicy Bypass -File register.ps1 <FusionTsf.dll>

param(
  [Parameter(Mandatory = $true)]
  [string]$DllPath
)

$ErrorActionPreference = 'Stop'

# Test-Path before Resolve-Path: Resolve-Path raises a terminating error for a
# missing path under $ErrorActionPreference = 'Stop', which would bypass the exit
# code below and surface as an unhandled exception in the caller.
if (-not (Test-Path -LiteralPath $DllPath)) {
  Write-Host "DLL not found: $DllPath" -ForegroundColor Red
  exit 2
}
$dll = Resolve-Path -LiteralPath $DllPath

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
exit 0
