# Writes the source-access text that ships in the package, with the addon revision filled in.
#
# The shipped document names the exact fcitx5-chinese-addons revision the runtime was built
# from. That value already has one authoritative home — FUSION_SETTINGS_CHINESE_ADDONS_COMMIT in
# cmake/settings-ui/versions.cmake, which tools/fcitx/build-native.ps1 also checks out and
# package.json records — so the checked-in document carries a token instead of a third copy of
# the sha, and this script is the single place that fills it in. build-package.ps1 calls it on
# the staged copy and refuses the package if the result does not link the revision it built.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File source-access.ps1 `
#     -Template <path to the staged SOURCE-ACCESS.txt> -AddonRevision <40-hex commit>

param(
  [Parameter(Mandatory = $true)][string]$Template,
  [Parameter(Mandatory = $true)][string]$AddonRevision
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$token = '@FUSION_SETTINGS_CHINESE_ADDONS_COMMIT@'

if ($AddonRevision -notmatch '^[0-9a-f]{40}$') {
  throw "The addon revision must be an exact 40-hex commit, not '$AddonRevision'"
}
if (-not (Test-Path -LiteralPath $Template)) {
  throw "The source-access text is missing: $Template"
}

$text = Get-Content -LiteralPath $Template -Raw
$occurrences = ([Regex]::Matches($text, [Regex]::Escape($token))).Count
if ($occurrences -ne 1) {
  throw "SOURCE-ACCESS.txt must carry $token exactly once (found $occurrences), so the packaged link names the built revision"
}

$text = $text.Replace($token, $AddonRevision)
if ($text -match '@[A-Z0-9_]+@') {
  throw 'The source-access text still contains an unsubstituted token'
}
if (-not $text.Contains("fcitx5-chinese-addons/tree/$AddonRevision")) {
  throw "The source-access text does not link fcitx5-chinese-addons at $AddonRevision"
}

# The text is ASCII; UTF8 without a BOM keeps it byte-comparable with the other notices.
[IO.File]::WriteAllText($Template, $text, (New-Object System.Text.UTF8Encoding($false)))
