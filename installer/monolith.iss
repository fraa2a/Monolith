; Monolith — per-user Inno Setup installer.
;
; Build:  iscc /DMonolithVersion=X.Y.Z monolith.iss
; CI passes MonolithVersion from the git tag (.github/workflows/version-tag.yml).
; The payload is native Monolith.exe plus codec/updater DLLs at the root and
; the self-contained WinUI 3 settings sidecar under .\settings — no runtime
; prerequisites on the target machine.
;
; Per-user by design: installs under {localappdata}\Programs\Monolith with
; PrivilegesRequired=lowest so WinSparkle can run the updater without UAC.

#ifndef MonolithVersion
  #define MonolithVersion "1.2.1"
#endif

#define MonolithName "Monolith"
#define MonolithExe "Monolith.exe"
#define MonolithPublisher "fraa2a"
#define MonolithRepoUrl "https://github.com/fraa2a/Monolith"
#define PayloadDir "..\build\app\recorder\Release"

[Setup]
; Stable AppId: future installers upgrade in place over this identity.
AppId={{81c3dff1-5dbe-4c96-bab0-a945e3a22b63}
AppName={#MonolithName}
AppVersion={#MonolithVersion}
AppVerName={#MonolithName} {#MonolithVersion}
AppPublisher={#MonolithPublisher}
AppPublisherURL={#MonolithRepoUrl}
AppSupportURL={#MonolithRepoUrl}
AppUpdatesURL={#MonolithRepoUrl}
DefaultDirName={localappdata}\Programs\{#MonolithName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=MonolithSetup-{#MonolithVersion}
SetupIconFile=..\app\assets\Monolith.ico
UninstallDisplayIcon={app}\{#MonolithExe}
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
VersionInfoVersion={#MonolithVersion}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startupicon"; Description: "Start {#MonolithName} when you sign in"; GroupDescription: "Startup:"

[Files]
; Native recorder payload. Keep this explicit so stale WinUI/.NET publish files
; in the root output directory cannot be bundled accidentally.
Source: "{#PayloadDir}\Monolith.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\WinSparkle.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\av*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\sw*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\lib*.dll"; DestDir: "{app}"; Flags: ignoreversion
; WinUI settings sidecar and its self-contained runtime live in a subfolder.
Source: "{#PayloadDir}\settings\*"; DestDir: "{app}\settings"; Flags: recursesubdirs ignoreversion
; Default config is resolved from <exe dir>\config\default-config.json.
Source: "..\config\default-config.json"; DestDir: "{app}\config"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\{#MonolithName}"; Filename: "{app}\{#MonolithExe}"
Name: "{userdesktop}\{#MonolithName}"; Filename: "{app}\{#MonolithExe}"; Tasks: desktopicon

[Registry]
; Per-user autostart (HKCU — no elevation needed); removed on uninstall.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#MonolithName}"; ValueData: """{app}\{#MonolithExe}"""; \
    Flags: uninsdeletevalue; Tasks: startupicon

[Run]
Filename: "{app}\{#MonolithExe}"; Description: "{cm:LaunchProgram,{#MonolithName}}"; \
    Flags: nowait postinstall skipifsilent

; User data (config.json, logs) lives under {localappdata}\Monolith and is
; intentionally NOT touched by uninstall — settings survive reinstalls.
