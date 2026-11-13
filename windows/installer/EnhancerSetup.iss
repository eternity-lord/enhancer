; Enhancer v3.0 Studio for Windows (x64) — Inno Setup 6 Installer Script
; Built by Mohammad Irani

#ifndef AppVersion
  #define AppVersion "3.0.0"
#endif

#ifndef StageDir
  #define StageDir "..\dist\Enhancer-v3.0-Windows-x64"
#endif

#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{8E4C7D29-53A1-4F92-B6E0-30C47F18A9D3}
AppName=Enhancer v3.0 Studio
AppVersion={#AppVersion}
AppVerName=Enhancer v3.0 Studio (Windows x64)
AppPublisher=Mohammad Irani
AppPublisherURL=https://github.com/eternity-lord/enhancer
AppSupportURL=https://github.com/eternity-lord/enhancer
AppUpdatesURL=https://github.com/eternity-lord/enhancer/releases
DefaultDirName={autopf}\Enhancer Studio
DefaultGroupName=Enhancer v3.0 Studio
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Enhancer-v3.0-Windows-x64-Setup
SetupIconFile=..\Enhancer.Desktop\Assets\enhancer.ico
UninstallDisplayIcon={app}\Enhancer.exe
Compression=lzma2/fast
SolidCompression=no
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Enhancer v3.0 Studio"; Filename: "{app}\Enhancer.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Enhancer v3.0 Studio"; Filename: "{app}\Enhancer.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Enhancer.exe"; Description: "Launch Enhancer v3.0 Studio"; Flags: nowait postinstall skipifsilent
