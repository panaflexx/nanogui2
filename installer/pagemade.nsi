; pagemade Windows installer (NSIS)
; Builds Pagemade-Setup.exe from the CMake runtime output directory.
; Packaged by build_win_pagemade.ps1, the same way build_win_pkg.ps1 packs nmail.

!ifndef PAGEMADE_VERSION
  !define PAGEMADE_VERSION "0.1.0"
!endif
!ifndef PAGEMADE_BIN
  !define PAGEMADE_BIN "..\build\bin"
!endif

!include "MUI2.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

Name "pagemade"
OutFile "Pagemade-Setup-${PAGEMADE_VERSION}.exe"
Unicode True
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\pagemade"
InstallDirRegKey HKLM "Software\pagemade" "InstallDir"
ShowInstDetails show
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
; Paths are relative to this .nsi file.
!define MUI_ICON "..\resources\pagemade.ico"
!define MUI_UNICON "..\resources\pagemade.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "pagemade" SecApp
  SetOutPath "$INSTDIR"
  File "${PAGEMADE_BIN}\pagemade.exe"
  ; VC runtime. The Universal CRT (api-ms-win-crt-*) stays on the system.
  File "${PAGEMADE_BIN}\vcruntime140.dll"
  File "${PAGEMADE_BIN}\vcruntime140_1.dll"
  File "${PAGEMADE_BIN}\msvcp140.dll"

  ; NanoGUI loads resources/*.ttf relative to the working directory.
  ; Hyphenation is resources/hyphenation/ beside the executable.
  SetOutPath "$INSTDIR\resources"
  File "${PAGEMADE_BIN}\resources\*.ttf"
  SetOutPath "$INSTDIR\resources\hyphenation"
  File "${PAGEMADE_BIN}\resources\hyphenation\hyph-en-us.pat.txt"
  File "${PAGEMADE_BIN}\resources\hyphenation\hyph-en-us.hyp.txt"
  File "${PAGEMADE_BIN}\resources\hyphenation\hyph-en-us.LICENSE.txt"
  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\pagemade"
  CreateShortCut "$SMPROGRAMS\pagemade\pagemade.lnk" "$INSTDIR\pagemade.exe"
  CreateShortCut "$DESKTOP\pagemade.lnk" "$INSTDIR\pagemade.exe"

  WriteRegStr HKLM "Software\pagemade" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\pagemade" "DisplayName" "pagemade"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\pagemade" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\pagemade" "DisplayVersion" "${PAGEMADE_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\pagemade" "Publisher" "panaflexx"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\pagemade" "DisplayIcon" "$INSTDIR\pagemade.exe,0"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\pagemade.exe"
  Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\vcruntime140_1.dll"
  Delete "$INSTDIR\msvcp140.dll"
  Delete "$INSTDIR\resources\hyphenation\hyph-en-us.pat.txt"
  Delete "$INSTDIR\resources\hyphenation\hyph-en-us.hyp.txt"
  Delete "$INSTDIR\resources\hyphenation\hyph-en-us.LICENSE.txt"
  RMDir "$INSTDIR\resources\hyphenation"
  Delete "$INSTDIR\resources\*.ttf"
  RMDir "$INSTDIR\resources"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\pagemade\pagemade.lnk"
  RMDir "$SMPROGRAMS\pagemade"
  Delete "$DESKTOP\pagemade.lnk"
  DeleteRegKey HKLM "Software\pagemade"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\pagemade"
SectionEnd
