; Windows installer for Drift, built by the Release workflow from dist\Drift
; (drift.exe, its Qt, mpv.exe and yt-dlp.exe). Installs per user, no admin rights.
; ISCC /DAppVersion=<pkgver> windows\drift.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
; Never change AppId: upgrades and the uninstaller find the installed Drift by it.
AppId={{E6F55BF0-6E27-4872-BF12-9B9A1E285640}
AppName=Drift
AppVersion={#AppVersion}
AppPublisher=Gamawn
AppPublisherURL=https://github.com/Gamawn/drift
DefaultDirName={autopf}\Drift
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\pkgbuild\drift.ico
UninstallDisplayIcon={app}\drift.exe
OutputDir=..
OutputBaseFilename=Drift-setup-x64
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
; Drift may be playing in the background with its window closed.
CloseApplications=force
; Drift looks for this to stay out of the way while its files are being replaced.
SetupMutex=DriftSetup

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\Drift\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
; The Start menu shortcut is also how Windows names Drift in the media flyout.
Name: "{autoprograms}\Drift"; Filename: "{app}\drift.exe"
Name: "{autodesktop}\Drift"; Filename: "{app}\drift.exe"; Tasks: desktopicon

[Registry]
; drift://play/<id> links, where Discord's "Listen in Drift" button ends up.
Root: HKA; Subkey: "Software\Classes\drift"; ValueType: string; ValueName: ""; ValueData: "URL:Drift"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\drift"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\drift\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\drift.exe"" ""%1"""

[Run]
; --from-setup: the installer is still running when it starts Drift, which otherwise stays out of its way.
Filename: "{app}\drift.exe"; Parameters: "--from-setup"; Description: "{cm:LaunchProgram,Drift}"; Flags: nowait postinstall skipifsilent
; Drift's own updater runs the installer silently with /RELAUNCH=1. As the user,
; not with the rights an all-users install gives the installer.
Filename: "{app}\drift.exe"; Parameters: "--from-setup"; Flags: nowait runasoriginaluser; Check: Relaunch

[Code]
function Relaunch: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;
