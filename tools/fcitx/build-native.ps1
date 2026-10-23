param(
  [ValidateSet('Debug','Release')][string]$Configuration='Debug',
  [string]$WorkRoot=(Join-Path $env:USERPROFILE 'worktmp\fusion-windows\stage3-real-backend-poc'),
  [string]$VsDevCmd='C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat',
  [switch]$UseCurrentToolchainEnvironment
)
$ErrorActionPreference='Stop'
$taskRepoRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskSupport=Join-Path $taskRepoRoot 'cmake\fcitx-native'
$WorkRoot=[IO.Path]::GetFullPath($WorkRoot)
New-Item -ItemType Directory -Force $WorkRoot | Out-Null
function Invoke-Native([string]$Executable,[string[]]$Arguments) {
  # R23: Windows PowerShell 5.1 turns anything a native tool writes to stderr into a
  # terminating NativeCommandError while $ErrorActionPreference is 'Stop' — a warning
  # from CMake is enough, and `2>$null` does not prevent it. The exit code is the real
  # contract for these tools, so relax the preference around the call, keep the output
  # visible, and decide on $LASTEXITCODE alone.
  $taskPrevious=$ErrorActionPreference
  $ErrorActionPreference='Continue'
  try { & $Executable @Arguments }
  finally { $ErrorActionPreference=$taskPrevious }
  if($LASTEXITCODE -ne 0) {throw "$Executable failed with exit $LASTEXITCODE"}
}
# Import only build-tool environment variables; never print a process environment.
# An explicit current-process environment is supported for machines where the
# VS discovery batch cannot run (for example, reg.exe is blocked by local policy).
# The caller must set INCLUDE/LIB and put the pinned MSVC/SDK/CMake/Ninja/UCRT
# directories on this process's PATH; nothing is written to the user or machine PATH.
if($UseCurrentToolchainEnvironment) {
  if(-not $env:INCLUDE -or -not $env:LIB -or -not (Get-Command cl -ErrorAction SilentlyContinue) -or
     -not (Get-Command cmake -ErrorAction SilentlyContinue) -or -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw 'UseCurrentToolchainEnvironment requires process-local INCLUDE, LIB, cl, cmake and ninja'
  }
} else {
  $taskEnvironment=& $env:ComSpec /d /c ('call "'+$VsDevCmd+'" -arch=x64 -host_arch=x64 >nul && set')
  if($LASTEXITCODE -ne 0) {throw 'Visual Studio environment initialization failed'}
  $taskAllowed=@('PATH','INCLUDE','LIB','LIBPATH','VSINSTALLDIR','VCINSTALLDIR','VCToolsInstallDir','VCToolsVersion','WindowsSdkDir','WindowsSDKVersion','UniversalCRTSdkDir','UCRTVersion','VSCMD_ARG_HOST_ARCH','VSCMD_ARG_TGT_ARCH','VSCMD_VER','VisualStudioVersion')
  foreach($taskLine in $taskEnvironment) {
    if($taskLine -match '^([^=]+)=(.*)$' -and $taskAllowed -contains $Matches[1]) {Set-Item -LiteralPath ('Env:'+$Matches[1]) -Value $Matches[2]}
  }
  & $env:ComSpec /d /c 'chcp 65001 >nul'
}
function Get-PinnedSource([string]$Name,[string]$Url,[string]$Commit,[string]$Parent) {
  $taskPath=Join-Path $Parent $Name
  if(-not (Test-Path -LiteralPath (Join-Path $taskPath '.git'))) {
    if(Test-Path -LiteralPath $taskPath) {throw "Existing non-repository directory: $taskPath"}
    Invoke-Native 'git' @('clone','--no-checkout',$Url,$taskPath) | Out-Host
    Invoke-Native 'git' @('-C',$taskPath,'fetch','--depth','1','origin',$Commit) | Out-Host
    # Check out the committed bytes, not a machine-local rendering of them: the Chinese
    # addon hashes its own committed resources (im/pinyin/english_lexicon.tsv against
    # data-licenses/english/manifest.json) and refuses to configure when the bytes differ.
    # A Windows git with core.autocrlf=true rewrites text files to CRLF at checkout, which
    # would make that check fail on content nobody changed, so the clone's own checkout is
    # pinned to the bytes as committed.
    Invoke-Native 'git' @('-C',$taskPath,'config','core.autocrlf','false') | Out-Host
    Invoke-Native 'git' @('-C',$taskPath,'config','core.eol','lf') | Out-Host
    Invoke-Native 'git' @('-C',$taskPath,'checkout','--detach',$Commit) | Out-Host
  }
  $taskRemote=(& git -C $taskPath remote get-url origin).Trim()
  if($LASTEXITCODE -ne 0 -or $taskRemote -ne $Url) {throw "Unexpected origin at $taskPath"}
  $taskHead=(& git -C $taskPath rev-parse HEAD).Trim()
  if($LASTEXITCODE -ne 0 -or $taskHead -ne $Commit) {throw "Preserve and inspect unexpected source HEAD at $taskPath"}
  return $taskPath
}
function Get-PinnedDownload([string]$Name,[string]$Url,[string]$Sha256) {
  # One rule for every build-time resource: keep it in the task's resource cache and reuse
  # it only while its SHA-256 is the pinned one; a mismatch re-downloads rather than
  # silently building against bytes nobody pinned. Everything here is fetched at build
  # time, so a built and packaged product never needs the network for its own data.
  $taskCache=Join-Path $WorkRoot 'resources'
  New-Item -ItemType Directory -Force $taskCache | Out-Null
  $taskFile=Join-Path $taskCache $Name
  if((Test-Path -LiteralPath $taskFile) -and
     (Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash -eq $Sha256) {return $taskFile}
  # -UseBasicParsing: Windows PowerShell 5.1 otherwise routes the response through the IE
  # engine, which is unavailable in this non-interactive session ("Windows PowerShell 处于
  # 非交互模式"), so the pinned resource could not be fetched at all.
  Invoke-WebRequest -Uri $Url -OutFile $taskFile -UseBasicParsing
  if((Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash -ne $Sha256) {
    Remove-Item -LiteralPath $taskFile -Force
    throw "Resource hash mismatch: $Name"
  }
  return $taskFile
}
$taskSourceRoot=Join-Path $WorkRoot 'upstream'
New-Item -ItemType Directory -Force $taskSourceRoot | Out-Null
$taskCore=Get-PinnedSource 'fcitx5' 'https://github.com/fcitx/fcitx5.git' 'a7f2528773ebfec98a20c49b6eea2333110b1c88' $taskSourceRoot
# The Chinese engine is the fork the Android build uses, pinned to the same commit, so
# both products run one native implementation of the MoQi auxiliary filter and the mixed
# Chinese/English input rather than two. The Fcitx core and LibIME stay at their official
# pins above: the fork is based on the same core, and neither is moved for this.
$taskChinese=Get-PinnedSource 'fcitx5-chinese-addons' 'https://github.com/choicky/fcitx5-chinese-addons.git' '2145c4bf1cff8c62112b9c38db4e6032a190b280' $taskSourceRoot
$taskLibime=Get-PinnedSource 'libime' 'https://github.com/fcitx/libime.git' '171edcf137001e8eb8274f53ec010058cda70b09' $taskSourceRoot
Invoke-Native 'git' @('-C',$taskLibime,'submodule','update','--init','--depth','1')
$taskLibimeDiff=& git -C $taskLibime status --porcelain
if($taskLibimeDiff) {throw 'LibIME must remain unmodified'}
function Test-NativePatchApplied([string]$Source,[string]$Patch) {
  # R23: Windows PowerShell 5.1 turns a native command's stderr into a terminating
  # NativeCommandError while $ErrorActionPreference is 'Stop', and `2>$null` does not
  # prevent it. A fresh checkout makes the reverse probe fail (patch not applied yet),
  # which aborted the whole script before the patch could be applied. Relax the
  # preference for the probe only and decide on the exit code alone.
  $taskPrevious=$ErrorActionPreference
  $ErrorActionPreference='Continue'
  try { & git -C $Source apply --reverse --check $Patch 2>&1 | Out-Null }
  finally { $ErrorActionPreference=$taskPrevious }
  return ($LASTEXITCODE -eq 0)
}
function Apply-NativePatch([string]$Source,[string]$Patch) {
  if(Test-NativePatchApplied $Source $Patch) {return}
  Invoke-Native 'git' @('-C',$Source,'apply','--check',$Patch)
  Invoke-Native 'git' @('-C',$Source,'apply',$Patch)
}
Apply-NativePatch $taskCore (Join-Path $taskRepoRoot 'cmake\patches\fcitx5-msvc.patch')
Apply-NativePatch $taskChinese (Join-Path $taskRepoRoot 'cmake\patches\chinese-addons-msvc.patch')
$taskDeps=Join-Path $WorkRoot 'deps'
New-Item -ItemType Directory -Force $taskDeps | Out-Null
$taskVcpkg=Get-PinnedSource 'vcpkg' 'https://github.com/microsoft/vcpkg.git' '20ceb8adbf0504d4b0d5e4058a32f9d4fc896391' $taskDeps
if(-not(Test-Path -LiteralPath (Join-Path $taskVcpkg 'vcpkg.exe'))) {Invoke-Native (Join-Path $taskVcpkg 'bootstrap-vcpkg.bat') @('-disableMetrics')}
# Verified PoC-local override: current ports build with the existing VS CMake
# 4.3.1. Do not download a replacement CMake/Ninja installation.
$taskCmakeVersion=(& cmake --version | Select-Object -First 1)
if($taskCmakeVersion -match '4\.3\.1') {
  $taskTools=Join-Path $taskVcpkg 'scripts\vcpkg-tools.json'
  $taskToolData=Get-Content -LiteralPath $taskTools -Raw | ConvertFrom-Json
  foreach($taskTool in $taskToolData.tools) {if($taskTool.name -eq 'cmake' -and $taskTool.os -eq 'windows') {$taskTool.version='4.3.1'}}
  $taskToolData | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $taskTools -Encoding UTF8
}
Invoke-Native (Join-Path $taskVcpkg 'vcpkg.exe') @('install','libuv:x64-windows','pthreads:x64-windows','gettext[tools]:x64-windows','ecm:x64-windows','boost-iostreams[core]:x64-windows','zstd:x64-windows','pkgconf:x64-windows','getopt-win32:x64-windows','dirent:x64-windows','boost-ptr-container:x64-windows','boost-bimap:x64-windows','boost-multi-index:x64-windows','boost-unordered:x64-windows','boost-container:x64-windows','--disable-metrics')
$taskSuffix=$Configuration.ToLowerInvariant()
$taskPrefix=Join-Path $WorkRoot ('runtime-'+$taskSuffix)
$taskBuildRoot=Join-Path $WorkRoot ('native-build\'+$Configuration)
$taskToolchain=Join-Path $taskVcpkg 'scripts\buildsystems\vcpkg.cmake'
$taskCommon=@('-G','Ninja',('-DCMAKE_BUILD_TYPE='+$Configuration),('-DCMAKE_TOOLCHAIN_FILE='+$taskToolchain),('-DCMAKE_INSTALL_PREFIX='+$taskPrefix),('-DCMAKE_PREFIX_PATH='+$taskPrefix),('-DFUSION_UPSTREAM_DIR='+$taskSourceRoot))
Write-Output '[1/5] Fcitx core'
$taskCoreBuild=Join-Path $taskBuildRoot 'core'
Invoke-Native 'cmake' (@('-S',(Join-Path $taskSupport 'core'),'-B',$taskCoreBuild)+$taskCommon+@('-DENABLE_SERVER=OFF','-DENABLE_KEYBOARD=OFF','-DENABLE_X11=OFF','-DENABLE_WAYLAND=OFF','-DENABLE_DBUS=OFF','-DENABLE_ENCHANT=OFF','-DENABLE_EMOJI=OFF','-DENABLE_LIBUUID=OFF','-DENABLE_TEST=OFF','-DENABLE_TESTING_ADDONS=OFF','-DENABLE_DL=OFF','-DBUILD_SHARED_FCITX_ADDON=OFF','-DBUILD_SPELL_DICT=OFF','-DENABLE_XDGAUTOSTART=OFF','-DEVENT_LOOP_BACKEND=libuv'))
Invoke-Native 'cmake' @('--build',$taskCoreBuild,'--parallel','8')
Invoke-Native 'cmake' @('--install',$taskCoreBuild)
Write-Output '[2/5] LibIME'
$taskLibimeBuild=Join-Path $taskBuildRoot 'libime'
Invoke-Native 'cmake' (@('-S',(Join-Path $taskSupport 'libime'),'-B',$taskLibimeBuild)+$taskCommon+@('-DENABLE_DATA=OFF','-DENABLE_TEST=OFF','-DENABLE_TOOLS=ON'))
Invoke-Native 'cmake' @('--build',$taskLibimeBuild,'--parallel','8')
Invoke-Native 'cmake' @('--install',$taskLibimeBuild)
Write-Output '[3/5] Chinese addon'
$taskChineseBuild=Join-Path $taskBuildRoot 'chinese'
# The MoQi reverse-lookup table is hash-pinned by the addon itself
# (modules/pinyinhelper/moqima-gb18030.cmake) and fetched by its CMake at configure time.
# Fetch it through the same rule as every other resource and hand it to the configure step,
# so the build does not depend on raw.githubusercontent being reachable and the packaged
# table is the pinned one. The pin is read from the source rather than repeated here.
$taskMoqiPin=Join-Path $taskChinese 'modules\pinyinhelper\moqima-gb18030.cmake'
$taskMoqiPinText=Get-Content -LiteralPath $taskMoqiPin -Raw
$taskMoqiCommit=[regex]::Match($taskMoqiPinText,'MOQI_TABLE_COMMIT\s+"([^"]+)"').Groups[1].Value
$taskMoqiUrl=[regex]::Match($taskMoqiPinText,'MOQI_TABLE_URL\s+"([^"]+)"').Groups[1].Value
$taskMoqiHash=[regex]::Match($taskMoqiPinText,'MOQI_TABLE_SHA256\s+"([^"]+)"').Groups[1].Value
if(-not $taskMoqiCommit -or -not $taskMoqiUrl -or -not $taskMoqiHash) {throw "Cannot read the MoQi table pin: $taskMoqiPin"}
# The pin file writes the URL with the commit as a CMake variable, and this script does not
# evaluate CMake — an earlier version passed the text through unchanged, so a cache miss
# would have requested the literal path ".../${MOQI_TABLE_COMMIT}/moqima_gb18030.txt". The
# commit is the only variable that URL uses and it is read from the same three lines, so it
# is substituted here; anything still unresolved is an error rather than a request.
$taskMoqiUrl=$taskMoqiUrl.Replace('${MOQI_TABLE_COMMIT}',$taskMoqiCommit)
if($taskMoqiUrl -match '\$\{') {throw "Unresolved CMake variable in the MoQi table URL: $taskMoqiUrl"}
Write-Output ('MoQi table URL: '+$taskMoqiUrl)
$taskMoqiTable=Get-PinnedDownload 'moqima_gb18030.txt' $taskMoqiUrl $taskMoqiHash
$taskMoqiTargetDir=Join-Path $taskChineseBuild 'chinese\modules\pinyinhelper'
New-Item -ItemType Directory -Force $taskMoqiTargetDir | Out-Null
Copy-Item -LiteralPath $taskMoqiTable -Destination (Join-Path $taskMoqiTargetDir 'moqima_gb18030.txt') -Force
Invoke-Native 'cmake' (@('-S',(Join-Path $taskSupport 'chinese'),'-B',$taskChineseBuild)+$taskCommon+@('-DENABLE_TEST=OFF','-DENABLE_GUI=OFF','-DENABLE_BROWSER=OFF','-DENABLE_OPENCC=OFF','-DENABLE_CLOUDPINYIN=OFF','-DENABLE_DATA=OFF','-DENABLE_TOOLS=OFF','-DBUILD_SHARED_FCITX_ADDON=OFF'))
Invoke-Native 'cmake' @('--build',$taskChineseBuild,'--parallel','8','--target','pinyin','pinyinhelper','punctuation','py-stroke-extract','py-table-extract','chinese/im/pinyin/pinyin-addon.conf','chinese/im/pinyin/pinyin.conf','chinese/im/pinyin/shuangpin.conf','chinese/modules/punctuation/punctuation.conf','chinese/modules/pinyinhelper/pinyinhelper.conf')
Write-Output '[4/5] Assembling the runtime tree'
$taskData=Join-Path $taskPrefix 'data\fcitx5'
foreach($taskDirectory in @($taskData,(Join-Path $taskData 'addon'),(Join-Path $taskData 'inputmethod'),(Join-Path $taskData 'pinyin'),(Join-Path $taskData 'pinyinhelper'),(Join-Path $taskData 'punctuation'),(Join-Path $taskPrefix 'data\libime'),(Join-Path $taskPrefix 'lib\libime'))) {New-Item -ItemType Directory -Force $taskDirectory | Out-Null}
Copy-Item (Join-Path $taskPrefix 'share\fcitx5\*') $taskData -Recurse -Force
foreach($taskAddon in @('pinyin','pinyinhelper','punctuation')) {Copy-Item (Join-Path $taskChineseBuild ('lib\'+$taskAddon+'.lib')) (Join-Path $taskPrefix 'lib\fcitx5') -Force}
Copy-Item (Join-Path $taskChineseBuild 'chinese\im\pinyin\pinyin-addon.conf') (Join-Path $taskData 'addon\pinyin.conf') -Force
Copy-Item (Join-Path $taskChineseBuild 'chinese\im\pinyin\pinyin.conf') (Join-Path $taskData 'inputmethod\pinyin.conf') -Force
# Shuangpin is a second input method of the same pinyin addon (upstream installs both
# files to share/fcitx5/inputmethod). Without this definition Fcitx exposes only
# Pinyin, so the profile could never select Shuangpin.
Copy-Item (Join-Path $taskChineseBuild 'chinese\im\pinyin\shuangpin.conf') (Join-Path $taskData 'inputmethod\shuangpin.conf') -Force
Copy-Item (Join-Path $taskChineseBuild 'chinese\modules\pinyinhelper\pinyinhelper.conf') (Join-Path $taskData 'addon') -Force
Copy-Item (Join-Path $taskChineseBuild 'chinese\modules\punctuation\punctuation.conf') (Join-Path $taskData 'addon') -Force
Copy-Item (Join-Path $taskChinese 'im\pinyin\symbols') (Join-Path $taskData 'pinyin') -Force
# The mixed Chinese/English engine reads its production lexicon from the Fcitx data root
# (pinyin/english_lexicon.tsv), and the MoQi filter reads pinyinhelper/moqima_gb18030.txt.
# Neither is a downloadable runtime dependency: the lexicon is committed in the pinned
# source (verified against data-licenses/english/manifest.json at configure time) and the
# table came through the pinned download above, so both ship inside the package.
Copy-Item (Join-Path $taskChinese 'im\pinyin\english_lexicon.tsv') (Join-Path $taskData 'pinyin') -Force
# `Join-Path` has a mandatory -ChildPath, so `Join-Path $taskMoqiTable` in an earlier version
# did not build a path at all: it asked for the missing value on the console and the build sat
# there with no child process and no output. The table is already a full path.
Copy-Item -LiteralPath $taskMoqiTable -Destination (Join-Path $taskData 'pinyinhelper\moqima_gb18030.txt') -Force
Copy-Item (Join-Path $taskChineseBuild 'chinese\modules\pinyinhelper\*.mb') (Join-Path $taskData 'pinyinhelper') -Force
Copy-Item (Join-Path $taskChinese 'modules\punctuation\punc.mb.*') (Join-Path $taskData 'punctuation') -Force
$taskNativeBin=Join-Path $taskVcpkg ('installed\x64-windows\'+$(if($Configuration -eq 'Debug') {'debug\bin'} else {'bin'}))
Copy-Item (Join-Path $taskNativeBin '*.dll') (Join-Path $taskPrefix 'bin') -Force
$env:PATH=(Join-Path $taskPrefix 'bin')+';'+$env:PATH
Write-Output '[5/5] Dictionaries and the language model'
$taskResources=Join-Path $WorkRoot 'resources'
function Get-Resource([string]$Name,[string]$Hash) {
  $taskArchive=Get-PinnedDownload $Name ('https://download.fcitx-im.org/data/'+$Name) $Hash
  Push-Location $taskResources
  try {Invoke-Native 'cmake' @('-E','tar','xf',$taskArchive)} finally {Pop-Location}
}
Get-Resource 'lm_sc.arpa-20260629.tar.zst' '06808333b9173e5374cf2cb5afc12d08f5625bf9abb536489cac376fc05f2e7f'
Get-Resource 'dict-20260907.tar.zst' 'fb75a179065e690dfc4559ce1807cbaf4fbe4f0111a5005615be9f435e6b9d76'
Invoke-Native (Join-Path $taskPrefix 'bin\libime_pinyindict.exe') @((Join-Path $taskResources 'dict_sc.txt'),(Join-Path $taskPrefix 'data\libime\sc.dict'))
Invoke-Native (Join-Path $taskPrefix 'bin\libime_pinyindict.exe') @((Join-Path $taskResources 'dict_extb.txt'),(Join-Path $taskPrefix 'data\libime\extb.dict'))
Invoke-Native (Join-Path $taskPrefix 'bin\libime_pinyindict.exe') @((Join-Path $taskChinese 'im\pinyin\chaizi.txt'),(Join-Path $taskData 'pinyin\chaizi.dict'))
Invoke-Native (Join-Path $taskPrefix 'bin\libime_slm_build_binary.exe') @('-s','-a','22','-q','4','trie',(Join-Path $taskResources 'lm_sc.arpa'),(Join-Path $taskPrefix 'lib\libime\zh_CN.lm'))
Write-Output ('Native runtime ready: '+$taskPrefix)
Write-Output ('Configure product with -DFUSION_WITH_FCITX5=ON -DFUSION_FCITX5_ROOT='+$taskPrefix+' -DCMAKE_TOOLCHAIN_FILE='+$taskToolchain)

