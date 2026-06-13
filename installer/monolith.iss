; Monolith — per-user Inno Setup installer.
;
; Build:  iscc /DMonolithVersion=X.Y.Z monolith.iss
; CI passes MonolithVersion from the git tag (.github/workflows/version-tag.yml).
; The payload is the flat self-contained build output: native Monolith.exe,
; FFmpeg/codec DLLs, WinSparkle.dll, and the self-contained WinUI 3 settings
; sidecar — no runtime prerequisites on the target machine.
;
; Per-user by design: installs under {localappdata}\Programs\Monolith with
; PrivilegesRequired=lowest so WinSparkle can run the updater without UAC.

#ifndef MonolithVersion
  #define MonolithVersion "0.0.0"
#endif

#define MonolithName "Monolith"
#define MonolithExe "Monolith.exe"
#define MonolithPublisher "Monolith"
#define MonolithRepoUrl "https://github.com/fraa2a/Monolith-releases"
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
; Entire flat self-contained payload (recorder + settings sidecar + DLLs).
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
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
