# Read-only Stage 2.5 development registration inventory (64-bit PowerShell).
$ErrorActionPreference = 'Stop'
$clsid = '{8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}'
$profile = '{D4A1F6B2-3C58-4E77-8A90-1B5D9C06E4F1}'
foreach ($hive in @('HKLM', 'HKCU')) {
  $com = "${hive}:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
  $tip = "${hive}:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
  $server = if (Test-Path -LiteralPath $com) { (Get-Item -LiteralPath $com).GetValue('') } else { $null }
  Write-Host "$hive COM DLL: $server"
  if ($server) { Write-Host "  DLL exists: $(Test-Path -LiteralPath $server)" }
  Write-Host "$hive TIP: $(Test-Path -LiteralPath $tip)"
  if (Test-Path -LiteralPath "$tip\LanguageProfile") {
    Get-ChildItem -LiteralPath "$tip\LanguageProfile" | ForEach-Object {
      Write-Host "  Language $($_.PSChildName) profile: $(Test-Path -LiteralPath (Join-Path $_.PSPath $profile))"
    }
  }
  Write-Host "$hive category: $(Test-Path -LiteralPath "$tip\Category")"
}
Write-Host 'Effective COM lookup prefers HKCU override; active profile is queried with FusionTsfProfile query.'
