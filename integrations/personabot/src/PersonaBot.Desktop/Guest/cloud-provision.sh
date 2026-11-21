#!/bin/bash
# PersonaBot's first boot on a Debian or Ubuntu cloud image (DISTROS.md phase 2), run once by cloud-init
# inside the guest. It works on x86-64 and ARM64. Nothing from the host is mounted here.
# Progress and the result go to the first serial port, which PersonaBot reads as guest-boot.log.
set -Eeuo pipefail
CONSOLE=/dev/null
for candidate in /dev/ttyAMA0 /dev/ttyS0; do
    if [ -w "$candidate" ]; then CONSOLE=$candidate; break; fi
done
say() { echo "PERSONABOT_PROGRESS $*" > "$CONSOLE"; }
trap 'result=$?; echo "PERSONABOT_PROVISION_FAILED line=$LINENO exit=$result" > "$CONSOLE"; exit "$result"' ERR
. /etc/os-release
arch=$(dpkg --print-architecture)
say "Setting up $PRETTY_NAME ($arch)"
mkdir -p /opt/personabot /etc/systemd/system/serial-getty@hvc0.service.d
rm -f /opt/personabot/provisioned
# PersonaBot's shell: a root console on the virtio console (hvc0), never a network service.
cat > /etc/systemd/system/serial-getty@hvc0.service.d/personabot.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I 115200,38400,9600 vt100
EOF
systemctl enable serial-getty@hvc0.service
export DEBIAN_FRONTEND=noninteractive
dpkg --configure -a

# Cloud kernels leave out graphics drivers. Install the distribution's full kernel when the virtio
# GPU driver is missing, and make it the one GRUB boots.
if ! modinfo virtio_gpu >/dev/null 2>&1 || ! find /lib/modules -name 'virtio-gpu.ko*' | grep -q .; then
    case "$ID" in
        debian) kernel_package="linux-image-$arch" ;;
        ubuntu) kernel_package="linux-image-generic" ;;
        *) kernel_package="linux-image-$arch" ;;
    esac
    say "Installing the desktop kernel ($kernel_package)"
    apt-get install -y --no-install-recommends "$kernel_package"
    kernel=$(find /boot -maxdepth 1 -type f -name 'vmlinuz-*' ! -name '*cloud*' ! -name '*kvm*' | sort -V | tail -1)
    test -n "$kernel"
    test -s "/boot/initrd.img-${kernel##*/vmlinuz-}"
    mkdir -p /etc/default/grub.d
    printf 'GRUB_TOP_LEVEL="%s"\nGRUB_DEFAULT=0\n' "$kernel" > /etc/default/grub.d/90-personabot-graphics.cfg
    update-grub
fi

# Firefox: Debian ships Firefox ESR as a package; Ubuntu's "firefox" is only a snap wrapper, so Ubuntu
# gets Mozilla's own APT repository (signed, pinned above Ubuntu's transitional package).
if [ "$ID" = ubuntu ]; then
    say "Adding Mozilla's Firefox repository"
    install -d -m 0755 /etc/apt/keyrings
    curl -fsSL https://packages.mozilla.org/apt/repo-signing-key.gpg -o /etc/apt/keyrings/packages.mozilla.org.asc
    echo "deb [signed-by=/etc/apt/keyrings/packages.mozilla.org.asc] https://packages.mozilla.org/apt mozilla main" > /etc/apt/sources.list.d/mozilla.list
    printf 'Package: *\nPin: origin packages.mozilla.org\nPin-Priority: 1001\n' > /etc/apt/preferences.d/mozilla
    apt-get update
    say "Installing Firefox"
    apt-get install -y --allow-downgrades firefox
fi
browser=$(command -v firefox-esr || command -v firefox)
test -n "$browser"

# The desktop user: the person's own account, or "personabot" (the app puts the name here).
desktop_user=#PERSONABOT_DESKTOP_USER#
desktop_home=/home/$desktop_user
say "Preparing the desktop user"
mkdir -p $desktop_home/.mozilla/firefox/personabot $desktop_home/.icewm $desktop_home/evidence
cat > $desktop_home/.mozilla/firefox/profiles.ini <<'EOF'
[Profile0]
Name=PersonaBot
IsRelative=1
Path=personabot
Default=1
[General]
StartWithLastProfile=1
EOF
cat > $desktop_home/.mozilla/firefox/personabot/user.js <<'EOF'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("network.proxy.type", 0);
user_pref("browser.sessionstore.resume_from_crash", false);
EOF
printf 'TaskBarAutoHide=0\nTaskBarShowCPUStatus=0\nTaskBarShowNetStatus=0\nWorkspaceNames=" Desktop "\n' > $desktop_home/.icewm/preferences
printf 'prog "Firefox" firefox %s --profile %s\nprog "Terminal" utilities-terminal xterm\n' "$browser" "$desktop_home/.mozilla/firefox/personabot" > "$desktop_home/.icewm/menu"
cp $desktop_home/.icewm/menu $desktop_home/.icewm/toolbar
chown -R "$desktop_user:$desktop_user" "$desktop_home"

# The desktop launchers and the desktop switcher: the app puts CloudGuestRepair.DesktopScripts here, the
# same script that keeps machines made before it up to date on every start.
#PERSONABOT_DESKTOP_SCRIPTS#
cat > /etc/systemd/system/personabot-desktop.service <<'EOF'
[Unit]
Description=PersonaBot isolated Linux desktop
After=network-online.target cloud-final.service
Wants=network-online.target
ConditionPathExists=/opt/personabot/provisioned
[Service]
Type=simple
Environment=DISPLAY=:0
# Access control is local to the guest; the X server never listens on TCP.
ExecStart=/usr/bin/xinit /usr/local/bin/personabot-session -- /usr/lib/xorg/Xorg :0 vt1 -ac -s 0 -nolisten tcp -noreset
Restart=on-failure
RestartSec=5
StandardOutput=journal+console
StandardError=journal+console
[Install]
WantedBy=graphical.target
EOF
# Display :0 is PersonaBot's: a login screen a later package brings in (SDDM, GDM, LightDM...) would
# take it first and the desktop would never start. Mask them now, and again before every desktop start
# (CloudGuestRepair in the app writes the same drop-in into machines made before this).
for u in sddm gdm gdm3 lightdm lxdm xdm slim nodm; do systemctl mask "$u.service" >/dev/null 2>&1; done
mkdir -p /etc/systemd/system/personabot-desktop.service.d
cat > /etc/systemd/system/personabot-desktop.service.d/one-display.conf <<'EOF'
[Service]
# PersonaBot's desktop owns display :0: any login screen a package brings in is masked and stopped.
ExecStartPre=-/bin/sh -c 'for u in sddm gdm gdm3 lightdm lxdm xdm slim nodm; do systemctl mask "$$u.service" >/dev/null 2>&1; systemctl --no-block stop "$$u.service" >/dev/null 2>&1; done; true'
EOF
systemctl daemon-reload
systemctl enable personabot-desktop.service
systemctl set-default graphical.target
touch /opt/personabot/provisioned
echo 'PersonaBot provisioning done; restarting into the desktop' > "$CONSOLE"
