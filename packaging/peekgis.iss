#define MyAppName "peekGIS"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "peekGIS"
#define MyAppURL "https://github.com/wuqi/peekGIS"
#define MyAppExeName "peekgis.exe"

[Setup]
; Stable application identifier used by Windows for upgrade/uninstall detection.
AppId={{8E65A735-54C8-4E64-9A63-66D2A112F109}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Per-user install keeps the exe-relative cache directory writable without admin rights.
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
OutputDir=..\build\packaging
OutputBaseFilename=peekGIS-Setup-{#MyAppVersion}
SetupIconFile=..\assets\icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
WizardResizable=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription=peekGIS GIS viewer installer

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: checkedonce

[Files]
; Main application and runtime libraries.
Source: "..\build\windows\x64\release\bin\peekgis.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; GDAL command-line tools used by the toolbox panel.
Source: "..\build\windows\x64\release\bin\gdal*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\ogr*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\gnm*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\nearblack.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\sozip.exe"; DestDir: "{app}"; Flags: ignoreversion

; User-facing vector-cache utilities.
Source: "..\build\windows\x64\release\bin\vt_build.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\vt_estimate.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\release\bin\mkqix.exe"; DestDir: "{app}"; Flags: ignoreversion

; Runtime data, fonts, icons and toolbox scripts.
Source: "..\build\windows\x64\release\bin\share\*"; DestDir: "{app}\share"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build\windows\x64\release\bin\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build\windows\x64\release\bin\scripts\*"; DestDir: "{app}\scripts"; Flags: ignoreversion recursesubdirs createallsubdirs

; Keep the user's existing configuration when upgrading.
Source: "..\config.toml"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\cache"
Type: files; Name: "{app}\peekgis.log"
Type: files; Name: "{app}\imgui.ini"
