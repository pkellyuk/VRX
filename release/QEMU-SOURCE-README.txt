VRX Windows x64 QEMU source distribution

The runtime is QEMU under the GNU General Public License, version 2.
Component-specific terms are in licenses/, the original source trees, and the
MSYS2 packages. build.json identifies the upstream revision and patch checksum.
runtime-files.json identifies the runtime files this source set was prepared for.

Download vrx-qemu-source.zip and the matching mingw-w64-angleproject
.src.tar.zst asset from the same release. SHA256SUMS.txt covers both source
assets and the Windows runtime ZIP. The ANGLE archive is the original MSYS2
source package and includes its build dependencies.

The Windows changes in the runtime built November 17, 2026 are recorded in
windows-egl-readback.patch and windows-meson.patch. They change the headless
EGL/GLES context and framebuffer setup in hw/display/virtio-gpu-virgl.c,
include/ui/egl-helpers.h, ui/egl-headless.c, and ui/egl-helpers.c, change VNC
refresh and damage handling in ui/vnc.c, and skip the Windows build-tree
symlink hook in meson.build.

To build from the bundled source, extract qemu.tar.gz. Apply both patches with
git apply from the extracted QEMU root. Extract the dtc and keycodemapdb
archives into subprojects/. Open an MSYS2 UCRT64 shell with GCC, Make, Python,
Meson, Ninja, pkgconf, GLib, pixman, libepoxy, VirGL, libslirp, libpng and ANGLE.
The package recipe and source files for the runtime libraries are in msys2/.
Run the following from the patched QEMU tree:

  mkdir build
  cd build
  ../configure --target-list=x86_64-softmmu,aarch64-softmmu \
      --prefix="$PWD/install" --enable-whpx --enable-opengl \
      --enable-virglrenderer --enable-slirp --enable-vnc --disable-gtk \
      --disable-sdl --disable-docs --disable-werror --disable-guest-agent \
      --disable-debug-info --disable-rust
  ninja qemu-system-x86_64.exe qemu-system-aarch64.exe qemu-img.exe
  ninja install

Build-SharedGpuQemu.ps1 is the original build script for a Git checkout at the
recorded revision. It downloads QEMU if needed and applies both Windows changes.

The firmware/ directory includes the sources pinned by QEMU's roms/
submodules and the EDK2 source dependencies with their original paths in
edk2-sources.json. Use the firmware build targets and configurations in QEMU's
roms/ directory. For the bundled e500 firmware, use U-Boot v2021.07, as recorded
in QEMU commit a0c3747e14689583b84a6c15b80e7bc2209111be. QEMU commit
e7764c7cda58e7dca21d744efe8c3f8f43d4df45 records the EDK2 update to stable202608.
