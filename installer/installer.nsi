; NSIS installer for YT Audio Browser (per-user, no admin needed).
; Build:  makensis /DPLUGIN=<path\YtAudioBrowser.clap> /DTOOLS=<dir with yt-dlp.exe, ffmpeg.exe, ffprobe.exe, deno.exe> /DOUTFILE=<out.exe> installer.nsi
;
; Silent install:  Setup.exe /S [/PLUGINDIR=C:\Plugins\CLAP] [/D=C:\Tools\YtAudioBrowser]
;   (/D must be the last argument and unquoted.)

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!insertmacro GetParameters
!insertmacro GetOptions

; build-installer.ps1 writes the defines here; command-line /D defines work too.
; Relative paths resolve from this script's folder.
!include /NONFATAL "..\build\installer-defs.nsh"

!ifndef PLUGIN
  !error "pass /DPLUGIN=path\to\YtAudioBrowser.clap"
!endif
!ifndef SLIM
  !ifndef TOOLS
    !error "pass /DTOOLS=path\to\tools, or /DSLIM=1 /DFETCH=path\to\fetch-tools.ps1 to download tools at install time"
  !endif
!else
  !ifndef FETCH
    !error "SLIM needs /DFETCH=path\to\fetch-tools.ps1"
  !endif
!endif
!ifndef OUTFILE
  !define OUTFILE "YtAudioBrowser-Setup.exe"
!endif

!define NAME "YT Audio Browser"
!define VERSION "0.1.0"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\YtAudioBrowser"
; The plugin reads ToolsDir from here to find yt-dlp when it is not in the default folder.
!define APP_KEY "Software\YtAudioBrowser"

Name "${NAME}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\YtAudioBrowser"
; A reinstall or upgrade starts from the folder chosen last time.
InstallDirRegKey HKCU "${APP_KEY}" "ToolsDir"
SetCompressor /SOLID lzma

Var PluginDir

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME

; Page 1: tools folder ($INSTDIR). Holds yt-dlp, ffmpeg, deno and the uninstaller.
!define MUI_PAGE_HEADER_TEXT "Choose Tools Folder"
!define MUI_PAGE_HEADER_SUBTEXT "Where should yt-dlp, ffmpeg and deno be installed?"
!define MUI_DIRECTORYPAGE_TEXT_TOP "The download tools and the uninstaller go in the folder below. yt-dlp updates itself here, so pick a folder you can write to without admin rights."
!insertmacro MUI_PAGE_DIRECTORY

; Page 2: plugin folder ($PluginDir).
!define MUI_PAGE_HEADER_TEXT "Choose Plugin Folder"
!define MUI_PAGE_HEADER_SUBTEXT "Where should the CLAP plugin be installed?"
!define MUI_DIRECTORYPAGE_TEXT_TOP "The default is the standard per-user CLAP folder, which most DAWs already scan. If you choose a different folder, add it to your DAW's CLAP plugin search paths."
!define MUI_DIRECTORYPAGE_VARIABLE $PluginDir
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "The plugin is installed in:$\r$\n$PluginDir$\r$\n$\r$\nRescan plugins in your DAW to find $\"YT Audio Browser$\".$\r$\n$\r$\nUse the Update yt-dlp button in the plugin when a site stops working."
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  StrCpy $PluginDir "$LOCALAPPDATA\Programs\Common\CLAP"

  ; Reuse the plugin folder from a previous install.
  ReadRegStr $R2 HKCU "${APP_KEY}" "PluginDir"
  ${If} $R2 != ""
    StrCpy $PluginDir $R2
  ${EndIf}

  ; Command line wins (for silent installs).
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/PLUGINDIR=" $R1
  ${IfNot} ${Errors}
  ${AndIf} $R1 != ""
    StrCpy $PluginDir $R1
  ${EndIf}
FunctionEnd

Section "Install"
  ; CLAP plugin.
  SetOutPath "$PluginDir"
  File "${PLUGIN}"

  ; Tools: the plugin finds them through the registry value written below.
  SetOutPath "$INSTDIR\bin"
!ifdef SLIM
  ; Slim installer: fetch the latest tools now instead of bundling them.
  SetOutPath "$INSTDIR"
  File "${FETCH}"
  DetailPrint "Downloading yt-dlp, ffmpeg and deno (about 150 MB)..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\fetch-tools.ps1" -Dest "$INSTDIR\bin"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "Downloading the tools failed (code $0). The plugin is installed, but search and download will not work until you run:$\r$\n$\r$\npowershell -ExecutionPolicy Bypass -File $\"$INSTDIR\fetch-tools.ps1$\" -Dest $\"$INSTDIR\bin$\""
  ${EndIf}
!else
  File "${TOOLS}\yt-dlp.exe"
  File "${TOOLS}\ffmpeg.exe"
  File "${TOOLS}\ffprobe.exe"
  File "${TOOLS}\deno.exe"
!endif

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${APP_KEY}" "ToolsDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_KEY}" "PluginDir" "$PluginDir"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; Where the plugin went, as chosen at install time.
  ReadRegStr $0 HKCU "${APP_KEY}" "PluginDir"
  ${If} $0 == ""
    StrCpy $0 "$LOCALAPPDATA\Programs\Common\CLAP"
  ${EndIf}
  Delete "$0\YtAudioBrowser.clap"

  ; Only the files this installer put there: the chosen folder may hold other things.
  Delete "$INSTDIR\bin\yt-dlp.exe"
  Delete "$INSTDIR\bin\yt-dlp.exe.*"
  Delete "$INSTDIR\bin\ffmpeg.exe"
  Delete "$INSTDIR\bin\ffprobe.exe"
  Delete "$INSTDIR\bin\deno.exe"
  Delete "$INSTDIR\fetch-tools.ps1"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$LOCALAPPDATA\YtAudioBrowser\settings.ini"
  RMDir "$LOCALAPPDATA\YtAudioBrowser"
  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
  ; Downloaded audio in Music\YtAudioBrowser is left alone.
SectionEnd
