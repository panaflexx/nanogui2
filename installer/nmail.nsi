; nmail Windows installer (NSIS)
; Builds Nmail-Setup.exe from the CMake runtime output directory.

!ifndef NMAIL_VERSION
  !define NMAIL_VERSION "0.1.0"
!endif
!ifndef NMAIL_BIN
  !define NMAIL_BIN "..\build\bin"
!endif

!include "MUI2.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

Name "nmail"
OutFile "Nmail-Setup-${NMAIL_VERSION}.exe"
Unicode True
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\nmail"
InstallDirRegKey HKLM "Software\nmail" "InstallDir"
ShowInstDetails show
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
; Paths are relative to this .nsi file.
!define MUI_ICON "..\resources\nmail.ico"
!define MUI_UNICON "..\resources\nmail.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "nmail" SecApp
  SetOutPath "$INSTDIR"
  File "${NMAIL_BIN}\nmail.exe"
  File /nonfatal "${NMAIL_BIN}\nmail_view.exe"
  File /nonfatal "${NMAIL_BIN}\*.dll"

  ; Emoji (and other) faces loaded from resources/*.ttf at runtime
  SetOutPath "$INSTDIR\resources"
  File /nonfatal "${NMAIL_BIN}\resources\*.ttf"
  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\nmail"
  CreateShortCut "$SMPROGRAMS\nmail\nmail.lnk" "$INSTDIR\nmail.exe"
  CreateShortCut "$DESKTOP\nmail.lnk" "$INSTDIR\nmail.exe"

  WriteRegStr HKLM "Software\nmail" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\nmail" "DisplayName" "nmail"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\nmail" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\nmail" "DisplayVersion" "${NMAIL_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\nmail" "Publisher" "panaflexx"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\nmail" "DisplayIcon" "$INSTDIR\nmail.exe,0"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\nmail.exe"
  Delete "$INSTDIR\nmail_view.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\resources\*.ttf"
  RMDir "$INSTDIR\resources"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\nmail\nmail.lnk"
  RMDir "$SMPROGRAMS\nmail"
  Delete "$DESKTOP\nmail.lnk"
  DeleteRegKey HKLM "Software\nmail"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\nmail"
SectionEnd
