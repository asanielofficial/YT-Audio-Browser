; Inno Setup script. Build the app first, then:
;   ISCC /DAppVersion=1.2.0 installer\windows.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppName "Samplite Explorer"
#define AppExe "Samplite Explorer.exe"

[Setup]
; Never change AppId: it is how upgrades find the existing installation.
AppId={{6B0E2C7A-3F4D-4E19-9C58-2A7D1E90B4F3}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Samplite
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\dist
OutputBaseFilename=Samplite-Explorer-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=classic
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequiredOverridesAllowed=dialog

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "..\build\SampliteExplorer_artefacts\Release\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
