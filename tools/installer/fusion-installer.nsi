; Fusion Windows — single-EXE installer (NSIS 3).
;
; This script is the distribution entry point only. It does not install the
; product: it unpacks the owner-test package that tools/package/build-package.ps1
; produced, hands it to that package's own install.ps1 (which verifies every
; file against package.json, stops a running Host orderly, copies into a new
; version directory, registers the TIP, refreshes the logon tasks and starts the
; Host), and then writes the one thing install.ps1 does not write — the Windows
; "installed apps" entry that gives the product a name, a version and an
; uninstall command. The unpacked payload is then deleted while the progress page is
; still up, not left to NSIS's cleanup after the last click. Uninstall runs the same
; package's uninstall.ps1 and removes that entry.
;
; Everything that decides what an install *is* stays in install.ps1/uninstall.ps1:
; the install root, the per-version directory, the elevation requirement, the TIP
; registration, the scheduled-task principal (interactive, limited — the resident
; Host never runs elevated because this installer was), the tray and Start Menu
; entries, the orderly stop event, and keeping user data on uninstall.
;
; Build with /INPUTCHARSET UTF8 — the strings below are Chinese and are not
; representable in the ANSI code page the compiler would otherwise assume.
; Use tools/installer/build-installer.ps1, which also verifies the payload.

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

; ---- inputs -----------------------------------------------------------------
; PRODUCT_VERSION  the package's own version identifier (0.0.0-winui3+<sha>)
; PRODUCT_BUILD    its four-hex-digit numeric form, for the PE version resource
; PAYLOAD_DIR      directory holding an extracted owner-test package
; OUT_FILE         the EXE to write
!ifndef PRODUCT_VERSION
  !error "PRODUCT_VERSION is required (pass /DPRODUCT_VERSION=...)"
!endif
!ifndef PRODUCT_BUILD
  !define PRODUCT_BUILD 0
!endif
!ifndef PAYLOAD_DIR
  !error "PAYLOAD_DIR is required (pass /DPAYLOAD_DIR=...)"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "fusion-setup.exe"
!endif

; ---- product identity -------------------------------------------------------
; The display name is the one the product already uses (the settings window
; title, the tray tooltip); the version is the package's, not a new one.
!define PRODUCT_NAME "Fusion 输入法"
!define INSTALL_ROOT "$PROGRAMFILES64\Fcitx5 Fusion"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fusion"
!define LOG_DIR "$LOCALAPPDATA\fusion-installer"
; The shell that runs install.ps1 and uninstall.ps1 must be the native 64-bit
; Windows PowerShell, not the 32-bit one beside it. An NSIS installer is a 32-bit
; process, so its $SYSDIR is redirected to SysWOW64 and "$SYSDIR\WindowsPowerShell"
; is the 32-bit shell; everything those scripts decide from their own bitness then
; comes out 32-bit, measured with a read-only probe:
;   [Environment]::Is64BitProcess      False
;   process path                       C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe
;   [Environment]::GetFolderPath(...)  C:\Program Files (x86)
; install.ps1 would then install into "Program Files (x86)", host-state.ps1 would
; read HKLM in the WOW64 view, and the bare regsvr32 that register.ps1 runs would
; be the 32-bit one, which cannot register a 64-bit TSF DLL. Sysnative is the
; WOW64 alias for the real system directory, so this is the 64-bit shell; the
; installer and the uninstaller use this one define, so they cannot disagree.
!define SHELL "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"

Name "${PRODUCT_NAME}"
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "${OUT_FILE}"
InstallDir "${INSTALL_ROOT}"
RequestExecutionLevel admin
XPStyle on

VIProductVersion "0.0.0.${PRODUCT_BUILD}"
VIAddVersionKey "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME} 安装程序"
VIAddVersionKey "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "LegalCopyright" "Fusion Windows"

; ---- pages ------------------------------------------------------------------
; The welcome and finish texts are kept to what MUI's text area really shows: a
; longer string is silently clipped, and the two facts that must not be clipped
; are that the resident Host does not run elevated and that user data is kept.
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${PRODUCT_NAME} ${PRODUCT_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "本向导将把 ${PRODUCT_NAME} ${PRODUCT_VERSION} 安装到本机：输入法 Host、输入法框架组件、托盘程序、Fusion 设置，以及它们运行所需的文件、许可证与对应源代码材料。$\r$\n$\r$\n安装需要管理员权限（登记输入法组件要写入 HKLM）；常驻的 Host 不会以管理员身份运行。$\r$\n$\r$\n用户词库、学习数据和配置保存在 %APPDATA%\Fcitx5，安装与升级都不会改动它们。"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "${PRODUCT_NAME} ${PRODUCT_VERSION} 安装完成"
!define MUI_FINISHPAGE_TEXT "${PRODUCT_NAME} ${PRODUCT_VERSION} 已安装，Host 已在本会话中以普通权限启动。$\r$\n$\r$\n开始菜单的 Fusion 组里有 Fusion Settings 和 Restart FusionHost；托盘图标提供同样的入口。$\r$\n$\r$\n安装已启用本输入法，但不会强行把它设为当前输入法——请在 Windows 语言栏中选择 Fusion。"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---- state used by both halves ----------------------------------------------
Var InstallLog
Var UninstallLog
Var VersionDir
Var Entry
Var DataFlag

Function .onInit
  ; The product is x64 only (its TIP is a 64-bit DLL), so a 32-bit Windows cannot run
  ; it and is refused here rather than failing part-way through an install.
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT_NAME} 只能在 64 位 Windows 上安装：它的输入法组件是 64 位 DLL。"
    Abort
  ${EndIf}

  ; A 64-bit product: every registry read and write in this script and in the
  ; package's own scripts must land in the 64-bit view, or the "installed apps"
  ; entry and the TIP registration would be split across two views.
  SetRegView 64
  StrCpy $InstallLog "${LOG_DIR}\install-${PRODUCT_VERSION}.log"

  IfSilent fusion_ready

  ReadRegStr $R9 HKLM "${UNINST_KEY}" "DisplayVersion"
  ${If} $R9 == ""
    Goto fusion_ready
  ${EndIf}
  ${If} $R9 == "${PRODUCT_VERSION}"
    MessageBox MB_OKCANCEL|MB_ICONINFORMATION "已安装同一版本（${PRODUCT_VERSION}）。$\r$\n$\r$\n继续会重新应用这次安装：校验文件、重新登记输入法组件、刷新登录任务并启动 Host。已经相同的文件不会被重写，用户词库与配置不会被改动。$\r$\n$\r$\n是否继续？" IDOK fusion_ready
    Abort
  ${Else}
    MessageBox MB_OKCANCEL|MB_ICONINFORMATION "检测到已安装版本 $R9。$\r$\n$\r$\n继续将从 $R9 升级到 ${PRODUCT_VERSION}：先按产品自己的停止事件有序停止正在运行的 Host（它会保存），再安装新的版本目录并重新登记。$\r$\n$\r$\n用户词库、学习数据和配置会保留；仍被应用程序加载的旧版本目录会留下并报告。$\r$\n$\r$\n是否继续？" IDOK fusion_ready
    Abort
  ${EndIf}

fusion_ready:
FunctionEnd

; ---- install ----------------------------------------------------------------
Section "安装 ${PRODUCT_NAME} ${PRODUCT_VERSION}"

  InitPluginsDir
  DetailPrint "解包 ${PRODUCT_VERSION}…"
  SetOutPath "$PLUGINSDIR\payload"
  ; The whole owner-test package, byte for byte, so install.ps1 can verify every
  ; file against package.json exactly as it does for the zip. NSIS decompresses
  ; losslessly and checks its own CRCs, and install.ps1 checks the SHA-256 of all
  ; of them before it copies anything.
  File /r "${PAYLOAD_DIR}\*.*"

  DetailPrint "运行安装流程（校验、停止 Host、复制、登记、启动）…"
  DetailPrint "日志：$InstallLog"
  ; The working directory must not be $PLUGINSDIR: NSIS extracts its own plug-ins
  ; there, and one of them is a native System.dll. Add-Type in host-state.ps1 compiles
  ; C# and resolves the default reference "System.dll" against the working directory
  ; first, so it picks up that 12 KB native plug-in instead of the .NET assembly and
  ; dies with "not a valid assembly" (SOURCE_CODE_ERROR) before install.ps1 writes a
  ; single line. A dedicated empty directory has nothing a reference can collide with.
  SetOutPath "$PLUGINSDIR\run"
  nsExec::ExecToLog '"${SHELL}" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\payload\install.ps1" -PackageDir "$PLUGINSDIR\payload" -TranscriptPath "$InstallLog"'
  Pop $0
  ; The scripts are done with the payload tree, so the working directory leaves it now: a
  ; directory cannot be removed while it is the process's current one (measured:
  ; RemoveDirectory on the current directory fails with "another program is using this
  ; file"). The payload is removed further down; moving out first is also what lets NSIS's
  ; own cleanup of $PLUGINSDIR work on the failure path — measured, with the working
  ; directory left inside the tree, that cleanup left the whole payload in %TEMP%.
  SetOutPath "$TEMP"
  ${If} $0 != "0"
    ; install.ps1 names the step it failed at on the details page and in the log;
    ; the installer must not finish as if it had succeeded.
    MessageBox MB_ICONSTOP "安装未完成（退出代码 $0）。$\r$\n$\r$\n产品可能没有被安装，或只安装了一部分；失败发生在哪一步，写在下面这份日志里：$\r$\n$InstallLog$\r$\n$\r$\n再次运行本安装程序可以重试。"
    Abort
  ${EndIf}

  ; Only now does the product exist: register it in "已安装的应用" and give it an
  ; uninstaller. Both point at this version's own files.
  CreateDirectory "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Fusion"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${PRODUCT_VERSION}\bin\FusionTray.exe,0"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $R1 $R2 $R3
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" $R1

  ; The payload is deleted here, on the progress page, rather than left to NSIS's own
  ; cleanup of $PLUGINSDIR when the installer quits. That cleanup is the only work
  ; between the "完成" click and the process leaving: ~285 MB in ~660 files, deleted
  ; after the last click, with nothing on screen to explain it. On a machine where
  ; deleting those files is slow — a rotating disk, an antivirus that scans each one —
  ; that is the wait the owner reported. Doing it here keeps the same work (nothing is
  ; skipped, the files really are removed) but puts it where there is a progress line,
  ; and leaves the exit after the click with nothing to do. Measured on this machine
  ; with the real payload: click to exit 0.19 s before, 0.04 s after.
  DetailPrint "清理临时安装文件…"
  RMDir /r "$PLUGINSDIR\payload"
  RMDir /r "$PLUGINSDIR\run"

SectionEnd

; ---- uninstall --------------------------------------------------------------
Function un.onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT_NAME} 只能在 64 位 Windows 上卸载：它的输入法组件是 64 位 DLL。"
    Abort
  ${EndIf}
  SetRegView 64
  StrCpy $UninstallLog "${LOG_DIR}\uninstall-${PRODUCT_VERSION}.log"
FunctionEnd

Section "Uninstall"

  SetRegView 64

  ; Which uninstall.ps1 to run. The Uninstall key names the version the user sees in
  ; "已安装的应用", and that version's directory is the one this EXE installed, so it
  ; is the entry point whenever it is still there. Older version directories can be
  ; left behind by an upgrade (a process held them), and their scripts are older
  ; copies: picking one of those — the old code took whichever name FindFirst returned
  ; first, which is alphabetical, not newest — would run a stale script.
  ; Only when the named directory is gone: take the newest uninstall.ps1 on disk,
  ; ordered by the OS's own last-write time (dir /o-d, newest first).
  StrCpy $Entry ""
  ReadRegStr $VersionDir HKLM "${UNINST_KEY}" "DisplayVersion"
  ${If} $VersionDir != ""
    StrCpy $Entry "$INSTDIR\$VersionDir\uninstall.ps1"
    IfFileExists "$Entry" fusion_entry_ready
    DetailPrint "已安装版本目录不在：$INSTDIR\$VersionDir"
  ${EndIf}
  StrCpy $Entry ""
  InitPluginsDir
  ; `dir` accepts a wildcard only in the last component — "…\*\uninstall.ps1" is a
  ; syntax error — so the directories are listed newest-first and the first one that
  ; actually holds an uninstall.ps1 wins.
  nsExec::ExecToStack '"$SYSDIR\cmd.exe" /c dir /b /ad /o-d "$INSTDIR" > "$PLUGINSDIR\fusion-candidates.txt"'
  Pop $R0
  Pop $R1
  FileOpen $0 "$PLUGINSDIR\fusion-candidates.txt" r
  fusion_scan:
    ClearErrors
    StrCpy $1 ""
    FileRead $0 $1
    IfErrors fusion_scan_done
    ; FileRead keeps the line's CRLF; drop exactly that, and only if it is there.
    StrCpy $2 $1 "" -2
    ${If} $2 == "$\r$\n"
      StrCpy $1 $1 -2
    ${EndIf}
    StrCmp $1 "" fusion_scan
    IfFileExists "$INSTDIR\$1\uninstall.ps1" 0 fusion_scan
      StrCpy $Entry "$INSTDIR\$1\uninstall.ps1"
  fusion_scan_done:
  FileClose $0
  StrCmp $Entry "" fusion_entry_missing

fusion_entry_ready:
  DetailPrint "卸载入口：$Entry"
fusion_entry_missing:

  ${If} $Entry == ""
    MessageBox MB_OK|MB_ICONINFORMATION "没有找到已安装的版本目录，只清理安装记录。"
  ${Else}
    StrCpy $DataFlag ""
    IfSilent fusion_keep_data
    MessageBox MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION "是否同时删除用户数据？$\r$\n$\r$\n是：删除 %APPDATA%\Fcitx5 与 %LOCALAPPDATA%\Fcitx5，包括词库、学习记录和配置。$\r$\n否（默认）：保留它们，下次安装会继续使用。" IDNO fusion_keep_data
    StrCpy $DataFlag "-RemoveUserData"
    fusion_keep_data:

    DetailPrint "运行卸载流程（停止 Host、注销输入法组件、删除任务与版本目录）…"
    ; Same rule as the install step: an empty working directory, never $PLUGINSDIR,
    ; because the native System.dll plug-in NSIS unpacks there would be the
    ; "System.dll" that Add-Type resolves first inside uninstall.ps1.
    SetOutPath "$PLUGINSDIR\run"
    nsExec::ExecToLog '"${SHELL}" -NoProfile -ExecutionPolicy Bypass -File "$Entry" $DataFlag -TranscriptPath "$UninstallLog"'
    Pop $0
    ${If} $0 != "0"
      ; Nothing is removed from the registry on a failed uninstall: the entry
      ; stays so the user can retry after the cause is fixed.
      MessageBox MB_ICONSTOP "卸载未完成（退出代码 $0）。$\r$\n$\r$\n产品可能仍在原处；失败发生在哪一步，写在下面这份日志里：$\r$\n$UninstallLog"
      Abort
    ${EndIf}
  ${EndIf}

  ; The entry goes only now, after uninstall.ps1 reported success, so a failure
  ; above leaves the product retryable. The uninstaller then removes itself — it is
  ; running from its own copy in %TEMP%, so the file in the install root can be
  ; deleted — and the root is removed only if that left it empty. Nothing here
  ; recurses: a version directory a process still holds was left in place by
  ; uninstall.ps1 and keeps the root, as it should.
  DeleteRegKey HKLM "${UNINST_KEY}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

SectionEnd
