#ifndef Payload
  #error Payload directory required
#endif
#ifndef Artifacts
  #error Artifacts directory required
#endif
[Setup]
AppId={{7D3A93E4-2967-4DE9-B66E-DFDA40B744E0}
AppName=VRX
AppVersion=1.8.0
AppPublisher=VRX
AppPublisherURL=https://github.com/pkellyuk/VRX
DefaultDirName={localappdata}\Programs\VRX
DefaultGroupName=VRX
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir={#Artifacts}
OutputBaseFilename=VRX-Setup-1.8.0
SetupIconFile=..\desktop\VRX.Desktop\Assets\vrx.ico
UninstallDisplayIcon={app}\VRX.Desktop.exe
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
#ifdef SignedUninstallerDir
; Signed builds (release\build-release.ps1 -Stage Payload/Package): the uninstaller is
; signed separately, then embedded from this folder.
SignedUninstaller=yes
SignedUninstallerDir={#SignedUninstallerDir}
#endif
CloseApplications=yes
RestartApplications=no
[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked
[InstallDelete]
; The geometry stereo runtime (VRX's Vulkan layer, DXVK, vkd3d-proton) is replaced as a
; whole, so an update never leaves a DLL of an older version beside the new ones.
Type: filesandordirs; Name: "{app}\geometry"
[Files]
; The whole payload, including geometry\layer, geometry\dxvk and geometry\vkd3d.
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[UninstallDelete]
; Compiled shaders the engine cached for this PC. The rest of {localappdata}\VRX
; (profiles, settings, logs) is deliberately kept.
Type: filesandordirs; Name: "{localappdata}\VRX\shader-cache"
; The geometry runtime folder, with anything added to it after installation.
Type: filesandordirs; Name: "{app}\geometry"
[Icons]
Name: "{group}\VRX"; Filename: "{app}\VRX.Desktop.exe"
Name: "{autodesktop}\VRX"; Filename: "{app}\VRX.Desktop.exe"; Tasks: desktopicon
[Run]
Filename: "{app}\VRX.Desktop.exe"; Description: "Launch VRX"; Flags: nowait postinstall skipifsilent
