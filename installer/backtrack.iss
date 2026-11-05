; Backtrack installer (Inno Setup 6). Built by installer\make-installer.ps1, which passes
; /DAppVersion=<version from CMakeLists.txt>. Per-user install: no administrator rights, and the
; program folder is writable, so backtrack.ini, library.json and the log live next to the exe
; as the README describes.

#ifndef AppVersion
  #error Run installer\make-installer.ps1 (it passes /DAppVersion)
#endif

[Setup]
; Never change AppId: Windows recognises updates and the uninstaller by it.
AppId={{6E3B1C2A-8F4D-4C55-9A7E-2B5D9C1F0A71}
AppName=Backtrack
AppVersion={#AppVersion}
AppVerName=Backtrack {#AppVersion}
AppPublisher=Findiepman
AppPublisherURL=https://github.com/Findiepman/BackTrack
DefaultDirName={localappdata}\Programs\Backtrack
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1903, what the recorder needs (README "Building").
MinVersion=10.0.18362
OutputDir=..\build\installer
OutputBaseFilename=Backtrack-Setup-{#AppVersion}
SetupIconFile=..\build\backtrack.ico
UninstallDisplayIcon={app}\backtrack.exe
UninstallDisplayName=Backtrack
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; A running Backtrack is asked to close (it finishes the clip it is saving) and started again.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start Backtrack with Windows, so Instant Replay is always ready"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "Put a shortcut on the desktop"; GroupDescription: "Options:"; Flags: unchecked

[Files]
Source: "..\build\Release\backtrack.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; The same name and AppUserModelID the app keeps in the Start menu itself, so there is one entry.
Name: "{userprograms}\Backtrack"; Filename: "{app}\backtrack.exe"; WorkingDir: "{app}"; AppUserModelID: "Backtrack.Backtrack"; Comment: "Saves the last moments of your screen and sound with one key"
Name: "{userdesktop}\Backtrack"; Filename: "{app}\backtrack.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Exactly what Settings > General > "Launch when Windows starts" writes, so the switch shows it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Backtrack"; ValueData: """{app}\backtrack.exe"""; Tasks: startup

[Run]
Filename: "{app}\backtrack.exe"; Description: "Start Backtrack now"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Settings, library, thumbnails and the log next to the exe. Clips in Videos\Backtrack are never touched.
Type: files; Name: "{app}\backtrack.ini"
Type: files; Name: "{app}\backtrack.ini.tmp"
Type: files; Name: "{app}\library.json"
Type: files; Name: "{app}\library.json.tmp"
Type: files; Name: "{app}\library.*.json"
Type: files; Name: "{app}\backtrack.log*"
Type: filesandordirs; Name: "{app}\cache"
Type: dirifempty; Name: "{app}"

[Code]
// Uninstalling removes the startup entry only when it starts this copy (with or without
// --hidden): another Backtrack on the same PC (a second install, a developer build) keeps its own.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Value: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Backtrack', Value) then
    begin
      if Pos(Lowercase(ExpandConstant('{app}\backtrack.exe')), Lowercase(Value)) > 0 then
        RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Backtrack');
    end;
  end;
end;
