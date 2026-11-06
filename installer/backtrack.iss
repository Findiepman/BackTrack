; Backtrack installer (Inno Setup 6). Built by installer\make-installer.ps1, which passes
; /DAppVersion=<version from CMakeLists.txt>. Per-user install: no administrator rights, and the
; program folder is writable, so backtrack.ini, library.json and the log live next to the exe
; as the README describes.
;
; Backtrack's updater (src/platform/Updater.cpp) runs this file silently:
;   /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /WAITPID=<pid> "/LOG=<file>" [/RELAUNCH[=hidden]]
; /WAITPID waits until that Backtrack has ended (it quits the normal way, finishing a clip it is
; saving) before anything is replaced; /RELAUNCH starts Backtrack again afterwards (with --hidden
; for /RELAUNCH=hidden), also when the update failed, so the updater can never leave the user
; without Backtrack. An update keeps the folder, the tasks chosen at the first install and every
; file Backtrack wrote next to the exe: [Files] only replaces backtrack.exe, and [UninstallDelete]
; runs only when uninstalling.

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
; An update installs into the folder and with the tasks of the first install (both are Inno's
; defaults; spelled out because the updater relies on them).
UsePreviousAppDir=yes
UsePreviousTasks=yes

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
; A silent update does not bring back a desktop shortcut the user deleted.
Name: "{userdesktop}\Backtrack"; Filename: "{app}\backtrack.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Check: not IsSilentUpdate

[Registry]
; Exactly what Settings > General > "Launch when Windows starts" writes, so the switch shows it.
; Left alone when it already starts this copy (it may have --hidden from "Start minimized"), and
; never written by a silent update: the user may have turned it off in Backtrack since.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Backtrack"; ValueData: """{app}\backtrack.exe"""; Tasks: startup; Check: ShouldWriteStartup

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
; Downloaded updates. The rest of %LOCALAPPDATA%\Backtrack (a library kept there when the program
; folder was read-only) stays unless it is empty.
Type: filesandordirs; Name: "{localappdata}\Backtrack\updates"
Type: dirifempty; Name: "{localappdata}\Backtrack"

[Code]
const
  SYNCHRONIZE = $00100000;
  WAIT_TIMEOUT = $00000102;
  // Backtrack finishes the clip it is saving before it ends; that takes seconds, not minutes.
  WaitForBacktrackMs = 60000;

function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Longint; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Longint;
  external 'CloseHandle@kernel32.dll stdcall';

var
  Relaunch: Boolean;
  RelaunchHidden: Boolean;
  // Found before anything is installed: a first install has the uninstall entry afterwards too.
  WasInstalled: Boolean;

// The value of /NAME=value, or '' when it is not on the command line.
function ParamValue(const Name: String): String;
var
  I: Integer;
  Prefix: String;
begin
  Result := '';
  Prefix := '/' + Uppercase(Name) + '=';
  for I := 1 to ParamCount do
    if Pos(Prefix, Uppercase(ParamStr(I))) = 1 then
      Result := Copy(ParamStr(I), Length(Prefix) + 1, MaxInt);
end;

function HasParam(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/' + Name) = 0 then
      Result := True;
end;

function IsSilentUpdate: Boolean;
begin
  Result := WizardSilent and WasInstalled;
end;

function ShouldWriteStartup: Boolean;
var
  Value: String;
begin
  Result := not IsSilentUpdate;
  if Result and RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Backtrack', Value) then
    Result := Pos(Lowercase(ExpandConstant('{app}\backtrack.exe')), Lowercase(Value)) = 0;
end;

function InitializeSetup: Boolean;
var
  Pid: Integer;
  Process: THandle;
begin
  Result := True;
  WasInstalled := RegKeyExists(HKEY_CURRENT_USER,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E3B1C2A-8F4D-4C55-9A7E-2B5D9C1F0A71}_is1');
  RelaunchHidden := CompareText(ParamValue('RELAUNCH'), 'hidden') = 0;
  Relaunch := HasParam('RELAUNCH') or RelaunchHidden;
  Pid := StrToIntDef(ParamValue('WAITPID'), 0);
  if Pid > 0 then
  begin
    Process := OpenProcess(SYNCHRONIZE, 0, Pid);
    if Process <> 0 then
    begin
      Log(Format('Waiting for Backtrack (process %d) to end', [Pid]));
      if WaitForSingleObject(Process, WaitForBacktrackMs) = WAIT_TIMEOUT then
        Log('Backtrack is still running; it is asked to close');
      CloseHandle(Process);
    end;
  end;
end;

// Runs whether the install worked or not, so /RELAUNCH always gets Backtrack back.
procedure DeinitializeSetup;
var
  Exe, Params: String;
  Code: Integer;
begin
  if not Relaunch then
    Exit;
  try
    Exe := ExpandConstant('{app}\backtrack.exe');
  except
    // Setup ended before it knew the folder: the one the first install used.
    if RegQueryStringValue(HKEY_CURRENT_USER,
      'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E3B1C2A-8F4D-4C55-9A7E-2B5D9C1F0A71}_is1',
      'InstallLocation', Exe) then
      Exe := AddBackslash(Exe) + 'backtrack.exe'
    else
      Exe := '';
  end;
  if (Exe = '') or not FileExists(Exe) then
    Exit;
  if RelaunchHidden then
    Params := '--hidden'
  else
    Params := '';
  if not ExecAsOriginalUser(Exe, Params, ExtractFileDir(Exe), SW_SHOWNORMAL, ewNoWait, Code) then
    Log('Could not start Backtrack again: ' + SysErrorMessage(Code));
end;

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
