; Fusion Windows — single-EXE installer (NSIS 3).
;
; This script is the distribution entry point only. It does not install the
; product: it unpacks the owner-test package that tools/package/build-package.ps1
; produced, hands it to that package's own install.ps1 (which verifies every
; file against package.json, stops a running Host orderly, copies into a new
; version directory, registers the TIP, refreshes the logon tasks and starts the
; Host), and then writes the one thing install.ps1 does not write — the Windows
; "installed apps" entry that gives the product a name, a version and an
; uninstall command. Uninstall runs the same package's uninstall.ps1 and removes
; that entry.
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
!define SHELL "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"

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
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${PRODUCT_NAME} ${PRODUCT_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "此向导将把 ${PRODUCT_NAME} ${PRODUCT_VERSION} 安装到本机。$\r$\n$\r$\n$\r$\n安装内容包括：输入法 Host、输入法框架组件（TIP）、托盘程序、Fusion 设置，以及它们运行所需的文件与许可证；安装程序会一并登记随包的对应源代码材料。$\r$\n$\r$\n$\r$\n安装需要管理员权限：持久登记输入法组件会写入 HKLM。常驻的 Host 不会以管理员身份运行——它由登录任务以当前用户、普通权限启动。$\r$\n$\r$\n$\r$\n用户词库、学习数据和配置保存在 %APPDATA%\Fcitx5，安装与升级都不会改动它们。"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "${PRODUCT_NAME} ${PRODUCT_VERSION} 安装完成"
!define MUI_FINISHPAGE_TEXT "${PRODUCT_NAME} ${PRODUCT_VERSION} 已安装，Host 已在本会话中以普通权限启动。$\r$\n$\r$\n$\r$\n开始菜单的 Fusion 组里有 Fusion Settings（设置）和 Restart FusionHost（重启 Host）；托盘图标提供同样的入口与日志目录。$\r$\n$\r$\n$\r$\n安装已登记并启用本输入法，但不会强行把它设为当前输入法——请在 Windows 语言栏中选择 Fusion。"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---- state used by both halves ----------------------------------------------
Var InstallLog
Var UninstallLog
Var VersionDir
Var DataFlag

Function .onInit
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
  SetOutPath "$PLUGINSDIR"
  nsExec::ExecToLog '"${SHELL}" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\payload\install.ps1" -PackageDir "$PLUGINSDIR\payload" -TranscriptPath "$InstallLog"'
  Pop $0
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

SectionEnd

; ---- uninstall --------------------------------------------------------------
Function un.onInit
  SetRegView 64
  StrCpy $UninstallLog "${LOG_DIR}\uninstall-${PRODUCT_VERSION}.log"
FunctionEnd

Section "Uninstall"

  ; The version directories are the product's; install.ps1 created the newest one.
  ; uninstall.ps1 removes all of them, so any one of them is a valid entry point —
  ; and it is the same script the zip-based uninstall uses.
  StrCpy $VersionDir ""
  FindFirst $0 $1 "$INSTDIR\*.*"
  fusion_find_loop:
    StrCmp $1 "" fusion_find_done
    StrCmp $1 "." fusion_find_next
    StrCmp $1 ".." fusion_find_next
    IfFileExists "$INSTDIR\$1\uninstall.ps1" 0 fusion_find_next
      StrCpy $VersionDir "$INSTDIR\$1"
      Goto fusion_find_done
  fusion_find_next:
    FindNext $0 $1
    Goto fusion_find_loop
  fusion_find_done:
  FindClose $0

  ${If} $VersionDir == ""
    MessageBox MB_OK|MB_ICONINFORMATION "没有找到已安装的版本目录，只清理安装记录。"
  ${Else}
    StrCpy $DataFlag ""
    IfSilent fusion_keep_data
    MessageBox MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION "是否同时删除用户数据？$\r$\n$\r$\n是：删除 %APPDATA%\Fcitx5 与 %LOCALAPPDATA%\Fcitx5，包括词库、学习记录和配置。$\r$\n否（默认）：保留它们，下次安装会继续使用。" IDNO fusion_keep_data
    StrCpy $DataFlag "-RemoveUserData"
    fusion_keep_data:

    DetailPrint "运行卸载流程（停止 Host、注销输入法组件、删除任务与版本目录）…"
    nsExec::ExecToLog '"${SHELL}" -NoProfile -ExecutionPolicy Bypass -File "$VersionDir\uninstall.ps1" $DataFlag -TranscriptPath "$UninstallLog"'
    Pop $0
    ${If} $0 != "0"
      ; Nothing is removed from the registry on a failed uninstall: the entry
      ; stays so the user can retry after the cause is fixed.
      MessageBox MB_ICONSTOP "卸载未完成（退出代码 $0）。$\r$\n$\r$\n产品可能仍在原处；失败发生在哪一步，写在下面这份日志里：$\r$\n$UninstallLog"
      Abort
    ${EndIf}
  ${EndIf}

  DeleteRegKey HKLM "${UNINST_KEY}"
  RMDir "$INSTDIR"

SectionEnd
