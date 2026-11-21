# Vrex Ubuntu integration

VRX uses one persistent Ubuntu 24.04 LTS x86-64 VM. This directory contains the
Automaton and PersonaBot sources used by Vrex for chat, the Ubuntu guest, and the
offline voice helper. `upstream.json` records the source revision and retained
file inventory. VRX-specific adaptations live in this repository.

The VM requires the Windows Hypervisor Platform and a VirGL renderer backed by
the host GPU. The worker verifies the renderer from inside Ubuntu before
reporting the desktop ready. QMP, the guest shell and its framebuffer bind to
loopback. The framebuffer carries optional positional guest audio without
exposing a microphone to Ubuntu.

## Build and run on Windows

1. Build PersonaBot’s bundled shared-GPU QEMU, then run
   `powershell -File desktop/prepare-vrex.ps1` from the VRX root.
2. Run `dotnet build desktop/VRX.Desktop/VRX.Desktop.csproj -c Release` and
   build the renderer with `bench/native/openxr/build.bat --desktop`.
3. On desktop, use **Room → Ubuntu desktop settings**. In VR, open **Ubuntu**
   from the main floating menu and select **Start Ubuntu**.
4. Open the Vrex chat from the Vrex logo. **Settings** contains the AI provider,
   model, key, and PC tool permissions. **Room → Vrex** contains avatar and voice
   controls.

First start downloads and verifies the Ubuntu image, creates its persistent disk
and installs a lightweight desktop with Firefox. The default VM folder is
`%LOCALAPPDATA%/VRX/Vrex/Ubuntu`. Stop Ubuntu before moving its storage. Closing
the floating panel leaves Ubuntu running. Stopping VRX shuts down the guest.
The key uses Windows DPAPI for the current user.

## Verification

```powershell
desktop/VRX.VrexHost/bin/Release/net10.0-windows/VRX.VrexHost.exe --self-test
python desktop/test-vrex-vm.py
bench/native/openxr/test-controllers.bat
bench/native/openxr/test-room-life.bat
```

The VM test uses an isolated disk under the system temporary directory and an
offline model fixture. It checks the installed distribution, CPU acceleration,
hardware graphics, live framebuffer and guest tools.
