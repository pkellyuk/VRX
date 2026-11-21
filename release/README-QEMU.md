# QEMU release assets for VRX

VRX downloads QEMU on the first Ubuntu start. Developer
builds can use a runtime staged by `desktop/prepare-vrex.ps1`.

The source and runtime are separate assets for the public VRX repository’s
`qemu-windows-e9fa1f66` release. The public release should include these four
files from the same generated output directory.

- `vrx-qemu-win-x64.zip` is the pinned runtime archive. VRX verifies the size
  and SHA-256 before installing it.
- `vrx-qemu-source.zip` contains QEMU, the Windows patches and build recipe,
  pinned firmware source, and MSYS2 sources for the other runtime libraries.
- `mingw-w64-angleproject-2.1.r25748.890b5d8f-6.src.tar.zst` is the matching
  MSYS2 ANGLE source package.
- `SHA256SUMS.txt` identifies all three archives.

Keep the GitHub release accessible from the Steam beta's release notes and
from the notice installed with VRX. Confirm the public assets can be downloaded
and pass the checksums before promoting a Steam build. This also lets you test
first-run download using a clean VRX profile. The Steam payload contains the
launcher and notices. QEMU downloads into the VRX profile, and the Ubuntu VM
image downloads separately from Canonical.

Run `release/build-release.ps1 -Stage SteamPayload` for the depot files. This
stage writes the installation directory and file checksums under the release
output without building an installer.

## Preparing a replacement build

`prepare-qemu-release.py` takes the runtime staged by `prepare-vrex.ps1`, the
original QEMU build output (with the Windows patch and build script), the
matching QEMU checkout, and a directory of corresponding source inputs.

- `msys2-sources.json` lists each MSYS2 source package with its `base`,
  `version`, `archive`, and the DLL names in `files`. Put the exact source
  archives in `msys2/`.
- Put the QEMU `roms/` submodule archives in `firmware/`, named
  `<module>-<commit>.tar.gz`. This build also needs the shipped e500 U-Boot
  version and the EDK2 submodule archives listed in
  `firmware/edk2-sources.json`.

The script checks the build revision, patch, runtime files, and source
inventory before writing the archives. Retain the output for each distributed
runtime. For any replacement runtime, use a fresh release tag and update the
URL, size, and SHA-256 in `desktop/VRX.VrexHost/QemuRuntimeFiles.cs` and the
release link in `THIRD-PARTY-NOTICES.txt`.
