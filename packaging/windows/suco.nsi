; SUCO Windows installer (NSIS).
;
;   makensis -DVERSION=0.10.5 -DSRCDIR=dist\suco-0.10.5-windows-x64 suco.nsi
;
; SRCDIR is the staged payload the release workflow already builds for the zip
; (the .exe files plus the MinGW/OpenSSL runtime DLLs), so the installer and the
; zip ship byte-identical binaries.

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRCDIR
  !error "SRCDIR is required: -DSRCDIR=<staged payload dir>"
!endif

!define APPNAME    "SUCO"
!define PUBLISHER  "MicBur"
!define HOMEPAGE   "https://github.com/MicBur/suco"
!define REGKEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME} ${VERSION}"
OutFile "suco-${VERSION}-windows-x64-setup.exe"
Unicode True
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
; Program Files and HKLM both need elevation.
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_LINK "${APPNAME} on GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "${HOMEPAGE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "German"

; --- PATH handling -----------------------------------------------------------
; Deliberately NOT setx: it truncates PATH at 1024 characters and would corrupt
; the machine environment. Reading/writing through PowerShell keeps the full
; value, and -Type ExpandString preserves REG_EXPAND_SZ so entries like
; %SystemRoot% keep expanding.
!macro PathAdd Dir
  nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -Command \
    "$$k=''HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment''; \
     $$p=(Get-ItemProperty -Path $$k -Name Path).Path; \
     if ($$p -split '';'' -notcontains ''${Dir}'') { \
       Set-ItemProperty -Path $$k -Name Path -Value ($$p.TrimEnd('';'') + '';${Dir}'') -Type ExpandString }"'
!macroend

!macro PathRemove Dir
  nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -Command \
    "$$k=''HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment''; \
     $$p=(Get-ItemProperty -Path $$k -Name Path).Path; \
     $$n=($$p -split '';'' | Where-Object { $$_ -ne ''${Dir}'' -and $$_ -ne '''' }) -join '';''; \
     Set-ItemProperty -Path $$k -Name Path -Value $$n -Type ExpandString"'
!macroend

Section "${APPNAME} (required)" SEC_CORE
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; Everything the zip ships: binaries + runtime DLLs + docs.
  File /r "${SRCDIR}\*.*"

  ; A console that already has SUCO on PATH and the daemon disabled — usable
  ; even when the user declines the machine-wide PATH change below.
  FileOpen $0 "$INSTDIR\suco-prompt.cmd" w
  FileWrite $0 "@echo off$\r$\n"
  FileWrite $0 "set $\"PATH=%~dp0;%PATH%$\"$\r$\n"
  FileWrite $0 "set SUCO_NO_DAEMON=1$\r$\n"
  FileWrite $0 "echo SUCO ${VERSION} - suco-cl / suco-cl++ / suco-worker / suco-coordinator$\r$\n"
  FileWrite $0 "echo Set SUCO_COORDINATOR_HOST to your coordinator, e.g. set SUCO_COORDINATOR_HOST=192.168.0.10$\r$\n"
  FileWrite $0 "cmd /k$\r$\n"
  FileClose $0

  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\${APPNAME}" "Version"    "${VERSION}"

  ; Add/Remove Programs
  WriteRegStr   HKLM "${REGKEY}" "DisplayName"     "${APPNAME} ${VERSION}"
  WriteRegStr   HKLM "${REGKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKLM "${REGKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKLM "${REGKEY}" "URLInfoAbout"    "${HOMEPAGE}"
  WriteRegStr   HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" "$0"

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Start menu shortcuts" SEC_SHORTCUTS
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\SUCO Command Prompt.lnk" "$INSTDIR\suco-prompt.cmd"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\README.lnk"              "$INSTDIR\README-windows.txt"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "Add to system PATH" SEC_PATH
  !insertmacro PathAdd "$INSTDIR"
SectionEnd

Section "Set SUCO_NO_DAEMON=1" SEC_NODAEMON
  ; The IPC daemon is Unix-socket based, so on Windows the client must always
  ; run without it. Setting it machine-wide keeps every shell working.
  WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "SUCO_NO_DAEMON" "1"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE}      "The SUCO binaries and the runtime DLLs they need."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_SHORTCUTS} "Start menu entries, including a console preconfigured for SUCO."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PATH}      "Make suco-cl / suco-cl++ callable from any shell."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_NODAEMON}  "Required on Windows: the IPC daemon uses Unix sockets."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInstSuccess
  ; Tell running shells the environment changed.
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=2000
FunctionEnd

Section "Uninstall"
  !insertmacro PathRemove "$INSTDIR"
  DeleteRegValue HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "SUCO_NO_DAEMON"

  Delete "$SMPROGRAMS\${APPNAME}\SUCO Command Prompt.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\README.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"

  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\suco-prompt.cmd"
  ; Only the payload we shipped, then the directory if it is empty.
  Delete "$INSTDIR\*.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\README-windows.txt"
  Delete "$INSTDIR\LICENSE"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"

  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=2000
SectionEnd
