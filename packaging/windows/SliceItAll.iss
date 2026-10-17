; Slice it All Windows installer.
; Build it with packaging\windows\package.ps1, which passes CrtDir.

#ifndef CrtDir
  #error Pass /DCrtDir to the Visual C++ CRT directory.
#endif

#define MyAppName "Slice it All"
#define MyAppVersion "0.9.0"
#define MyAppPublisher "Slice it All"
#define MyAppURL "https://github.com/synchleague/SliceItAll"
#define MyAppExeName "SliceItAll.exe"
#define BuildDir "..\..\build\src\RelWithDebInfo"
#define ResourceDir "..\..\resources"

[Setup]
AppId={{4F6A9C2E-8B17-4E5D-A3C1-7D90E2B64F18}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\Slice it All
DefaultGroupName=Slice it All
AllowNoIcons=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=SliceItAll-{#MyAppVersion}-win64-setup
SetupIconFile={#ResourceDir}\icons\SliceItAll.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} {#MyAppVersion} setup
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "associate"; Description: "Open STL and 3MF files with Slice it All"; GroupDescription: "File associations:"

[Files]
Source: "{#BuildDir}\SliceItAll.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SliceItAll.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SliceItAll-console.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SliceItAll-gcodeviewer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\OCCTWrapper.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libgmp-10.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libmpfr-4.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\msvcp140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\msvcp140_2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\msvcp140_atomic_wait.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\msvcp140_codecvt_ids.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140_threads.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\concrt140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ResourceDir}\*"; DestDir: "{app}\resources"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Slice it All"; Filename: "{app}\{#MyAppExeName}"; Comment: "Slice for every printer"
Name: "{group}\Slice it All console"; Filename: "{app}\SliceItAll-console.exe"
Name: "{autodesktop}\Slice it All"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCR; Subkey: "SliceItAll.stl"; ValueType: string; ValueName: ""; ValueData: "STL model"; Flags: uninsdeletekey; Tasks: associate
Root: HKCR; Subkey: "SliceItAll.stl\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: associate
Root: HKCR; Subkey: "SliceItAll.stl\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: associate
Root: HKCR; Subkey: ".stl"; ValueType: string; ValueName: ""; ValueData: "SliceItAll.stl"; Flags: uninsclearvalue; Tasks: associate

Root: HKCR; Subkey: "SliceItAll.3mf"; ValueType: string; ValueName: ""; ValueData: "3MF model"; Flags: uninsdeletekey; Tasks: associate
Root: HKCR; Subkey: "SliceItAll.3mf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: associate
Root: HKCR; Subkey: "SliceItAll.3mf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: associate
Root: HKCR; Subkey: ".3mf"; ValueType: string; ValueName: ""; ValueData: "SliceItAll.3mf"; Flags: uninsclearvalue; Tasks: associate

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Slice it All"; Flags: nowait postinstall skipifsilent
