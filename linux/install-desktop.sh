#!/bin/sh
# Adds VRX to this user's application menu: the VRX logo in the hicolor icon
# theme and a desktop entry that starts VRX - linux/vrx-linux in the
# repository, or ./vrx in a portable package. Nothing outside
# ${XDG_DATA_HOME:-~/.local/share} is changed. Run it again after moving the
# repository or package; remove the two files it names to uninstall.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -d "$here/share/vrx/icons" ]; then
    icons="$here/share/vrx/icons"
    program="$here/vrx"
else
    icons="$here/VRX.Linux/Assets"
    program="$here/vrx-linux"
fi
data="${XDG_DATA_HOME:-$HOME/.local/share}"
for size in 16 24 32 48 64 128 256; do
    directory="$data/icons/hicolor/${size}x${size}/apps"
    mkdir -p "$directory"
    cp "$icons/vrx-$size.png" "$directory/vrx.png"
done
mkdir -p "$data/applications"
entry="$data/applications/vrx-linux.desktop"
cat > "$entry.tmp" <<ENTRY
[Desktop Entry]
Type=Application
Name=VRX
GenericName=Flat games in VR
Comment=Play a window or screen in 3D in an OpenXR headset
Exec="$program"
Icon=vrx
Terminal=false
Categories=Game;
Keywords=VR;OpenXR;SteamVR;3D;stereo;
StartupWMClass=vrx-linux
ENTRY
mv "$entry.tmp" "$entry"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$data/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$data/icons/hicolor" || true
echo "Installed $entry and the vrx icon in $data/icons/hicolor"
