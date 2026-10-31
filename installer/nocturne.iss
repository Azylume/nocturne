; Nocturne for Windows: one folder under Program Files that holds the UI, the engine, its supervisor and tools, and
; the DLLs they load. Built by tools/make-installer.ps1, which stages the files and passes AppVersion and StageDir.
; Uninstalling removes the program, its firewall rules and the autostart task; %LOCALAPPDATA%\Nocturne stays.

#ifndef AppVersion
  #error Pass /DAppVersion=X.Y.Z (tools/make-installer.ps1 reads it from CMakeLists.txt)
#endif
#ifndef StageDir
  #error Pass /DStageDir=DIR (tools/make-installer.ps1 stages the files there)
#endif

[Setup]
AppId={{45BD7F96-9263-4FCD-AE72-9E6DDF388D0F}
AppName=Nocturne
AppVersion={#AppVersion}
AppPublisher=Azylume
AppPublisherURL=https://github.com/Azylume/nocturne
DefaultDirName={autopf}\Nocturne
DefaultGroupName=Nocturne
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 2004: the engine's process loopback audio capture needs it.
MinVersion=10.0.19041
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\Nocturne UI.exe
UninstallDisplayName=Nocturne
CloseApplications=force
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputBaseFilename=Nocturne-{#AppVersion}-windows-x64-unsigned

[Tasks]
Name: autostart; Description: "Start the engine under its supervisor at every logon (for a show machine)"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\Nocturne"; Filename: "{app}\Nocturne UI.exe"
Name: "{group}\Uninstall Nocturne"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Nocturne engine"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Nocturne engine"" dir=in action=allow program=""{app}\nocturne.exe"" enable=yes profile=private"; Flags: runhidden; StatusMsg: "Allowing the engine through the firewall..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Nocturne desk"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Nocturne desk"" dir=in action=allow program=""{app}\nocturne-desk.exe"" enable=yes profile=private"; Flags: runhidden
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\autostart.ps1"" install"; Tasks: autostart; Flags: runhidden; StatusMsg: "Setting up autostart..."
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\autostart.ps1"" uninstall"; Tasks: not autostart; Flags: runhidden
Filename: "{app}\Nocturne UI.exe"; Description: "Open Nocturne"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\autostart.ps1"" uninstall"; Flags: runhidden; RunOnceId: "autostart"
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -Command ""Get-Process -Name 'Nocturne UI' -ErrorAction SilentlyContinue | Where-Object Path -like '{app}\*' | Stop-Process -Force -PassThru | Wait-Process -Timeout 10"""; Flags: runhidden; RunOnceId: "close-ui"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Nocturne engine"""; Flags: runhidden; RunOnceId: "firewall-engine"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Nocturne desk"""; Flags: runhidden; RunOnceId: "firewall-desk"
