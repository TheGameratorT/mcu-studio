; NSIS installer for MCU Studio.
;
; Driven from build_installer.cmake, which passes every path/version in via /D:
;   VERSION      x.y.z (also used for the exe's version resource)
;   VIVERSION    x.y.z.0 (VIProductVersion needs four fields)
;   DIST_DIR     the deployed dist/ tree (the exe and all DLLs), native path
;   LICENSE_FILE the GPLv3 text shown on the license page
;   ICON         the .ico used for both the installer and uninstaller
;   OUTFILE      where to write the Setup exe

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef VIVERSION
  !define VIVERSION "0.0.0.0"
!endif

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MCU Studio"

Name "MCU Studio"
BrandingText "MCU Studio ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\MCU Studio"
InstallDirRegKey HKLM "Software\MCU Studio" "InstallDir"
RequestExecutionLevel admin

VIProductVersion "${VIVERSION}"
VIAddVersionKey "ProductName" "MCU Studio"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "TheGameratorT"
VIAddVersionKey "LegalCopyright" "GNU General Public License v3"
VIAddVersionKey "FileDescription" "MCU Studio Setup"
VIAddVersionKey "FileVersion" "${VERSION}"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING

!define MUI_FINISHPAGE_RUN "$INSTDIR\mcu-studio.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run MCU Studio"

!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "MCU Studio" SecMain
    SetOutPath "$INSTDIR"
    ; The whole deployed tree: the exe, Qt/MinGW DLLs, plugin subdirs
    ; (platforms/, imageformats/, ...), and the license notices.
    File /r "${DIST_DIR}\*"

    ; All-users shortcuts.
    SetShellVarContext all
    CreateDirectory "$SMPROGRAMS\MCU Studio"
    CreateShortcut "$SMPROGRAMS\MCU Studio\MCU Studio.lnk" \
        "$INSTDIR\mcu-studio.exe" "" "$INSTDIR\mcu-studio.exe" 0 \
        SW_SHOWNORMAL "" "Repair damaged JPEGs by editing their DCT coefficients"
    CreateShortcut "$DESKTOP\MCU Studio.lnk" \
        "$INSTDIR\mcu-studio.exe" "" "$INSTDIR\mcu-studio.exe" 0 \
        SW_SHOWNORMAL "" "Repair damaged JPEGs by editing their DCT coefficients"

    ; An "Open with MCU Studio" entry on the JPEG context menu. Deliberately a
    ; verb under SystemFileAssociations rather than a ProgID: it adds a way to
    ; open a photo here without taking over the default handler, which for an
    ; occasional repair tool would be the wrong thing to grab.
    WriteRegStr HKCR "SystemFileAssociations\.jpg\shell\McuStudio" "" "Open with MCU Studio"
    WriteRegStr HKCR "SystemFileAssociations\.jpg\shell\McuStudio" "Icon" "$INSTDIR\mcu-studio.exe,0"
    WriteRegStr HKCR "SystemFileAssociations\.jpg\shell\McuStudio\command" "" '"$INSTDIR\mcu-studio.exe" "%1"'
    WriteRegStr HKCR "SystemFileAssociations\.jpeg\shell\McuStudio" "" "Open with MCU Studio"
    WriteRegStr HKCR "SystemFileAssociations\.jpeg\shell\McuStudio" "Icon" "$INSTDIR\mcu-studio.exe,0"
    WriteRegStr HKCR "SystemFileAssociations\.jpeg\shell\McuStudio\command" "" '"$INSTDIR\mcu-studio.exe" "%1"'

    WriteRegStr HKLM "Software\MCU Studio" "InstallDir" "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Add/Remove Programs entry.
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "MCU Studio"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "TheGameratorT"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\mcu-studio.exe"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
    SetShellVarContext all
    Delete "$SMPROGRAMS\MCU Studio\MCU Studio.lnk"
    RMDir "$SMPROGRAMS\MCU Studio"
    Delete "$DESKTOP\MCU Studio.lnk"

    DeleteRegKey HKCR "SystemFileAssociations\.jpg\shell\McuStudio"
    DeleteRegKey HKCR "SystemFileAssociations\.jpeg\shell\McuStudio"

    ; Run normally (no _?= switch), so NSIS copies this uninstaller to $TEMP and
    ; executes from there, letting RMDir /r remove $INSTDIR including the
    ; original Uninstall.exe.
    RMDir /r "$INSTDIR"

    DeleteRegKey HKLM "${UNINST_KEY}"
    DeleteRegKey HKLM "Software\MCU Studio"
SectionEnd
