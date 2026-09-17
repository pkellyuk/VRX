# Building the Windows release

Run `release/build-release.ps1` from PowerShell with the existing model and native
build prerequisites installed, plus .NET 10 SDK and Inno Setup 6. The script uses
a new timestamped folder under ignored `release/out` by default; `-OutputRoot`
can select another fresh folder. It never deletes existing releases.

The native build uses `VRX_NATIVE_OUT` to isolate outputs from running developer
sessions. The desktop is published self-contained for win-x64. Model and library
paths resolve from the executable directory, with repository fallback only for
development builds. The packaged OpenXR loader discovers the active runtime;
SteamVR itself is not redistributed.

Outputs: installer EXE, portable ZIP and SHA256SUMS.txt. Each payload includes
BUILD.txt (source revision and dependency versions), FILES.sha256.txt, third-party
notices, setup instructions, bundled depth model, app-local VC runtime DLLs and
the self-contained .NET desktop runtime.

Before tagging, build from a clean committed tree. Test a relocated/installed
copy with `VRX.Desktop.exe --smoke-test` and `engine/xrplayer.exe --check-package`.
The desktop smoke test creates fixture output beneath that copy's `desktop/out`;
do not run it directly in the payload that will be archived. With no other VRX
session active, also run `engine/xrplayer.exe --synthetic --selftest` from an
unrelated working directory to verify bundled model/runtime discovery and GPU
output. These checks do not replace testing on a clean second machine.

Publish the installer, ZIP and checksum file to the matching GitHub tag using
`release/NOTES.md` for release notes. No signing certificate is configured; the
installer is unsigned. Source and notes belong in Git; release binaries and
downloaded/build outputs do not.
