namespace PersonaBot;

/// <summary>
/// Keeps display :0 for PersonaBot's own desktop on Debian and Ubuntu machines. A package a bot installs
/// can bring a login screen (KDE's SDDM, GNOME's GDM, LightDM…). Its X server then takes :0 first, and
/// PersonaBot's xinit fails ("server already running") every few seconds, so the machine never becomes
/// ready. New machines get this at first boot (cloud-provision.sh writes the same drop-in); machines made
/// before it are repaired during start-up by <see cref="CloudRuntime"/>.
/// </summary>
public static class CloudGuestRepair
{
    /// <summary>Keep repeated VM starts from substituting Firefox's crash-recovery dialog for a browser.</summary>
    public const string BrowserStartupScript = """
        set -e
        mkdir -p /etc/systemd/system/personabot-desktop.service.d
        if ! grep -qx 'Environment=MOZ_DISABLE_AUTO_SAFE_MODE=1' /etc/systemd/system/personabot-desktop.service.d/browser-startup.conf 2>/dev/null; then
            printf '[Service]\nEnvironment=MOZ_DISABLE_AUTO_SAFE_MODE=1\n' > /etc/systemd/system/personabot-desktop.service.d/browser-startup.conf
            systemctl daemon-reload
        fi
        if xdotool search --onlyvisible --name '^Open Firefox in Troubleshoot Mode' >/dev/null 2>&1; then
            systemctl restart personabot-desktop.service
        fi
        ready=0
        for n in $(seq 1 90); do
            if xdotool search --onlyvisible --classname Navigator >/dev/null 2>&1; then
                ready=1
                break
            fi
            sleep 0.5
        done
        test "$ready" = 1
        """;
    /// <summary>The login screens PersonaBot switches off.</summary>
    public const string DisplayManagers = "sddm gdm gdm3 lightdm lxdm xdm slim nodm";

    /// <summary>The line xinit writes when another X server already has display :0.</summary>
    public const string DisplayTakenMarker = "_XSERVTransMakeAllCOTSServerListeners: server already running";

    /// <summary>
    /// The systemd drop-in for personabot-desktop.service: before each start, mask and stop any login
    /// screen. "$$u" is systemd's escape for a literal "$u" in an Exec line.
    /// </summary>
    public const string DropIn = """
        [Service]
        # PersonaBot's desktop owns display :0: any login screen a package brings in is masked and stopped.
        ExecStartPre=-/bin/sh -c 'for u in sddm gdm gdm3 lightdm lxdm xdm slim nodm; do systemctl mask "$$u.service" >/dev/null 2>&1; systemctl --no-block stop "$$u.service" >/dev/null 2>&1; done; true'
        """;

    /// <summary>
    /// Run as root on every start once the desktop is up: the same masks and drop-in, and the current
    /// desktop launchers and switcher (<see cref="DesktopScripts"/>), without restarting anything. A login
    /// screen doesn't always win the race for :0 (on 14 Nov 2026 SDDM started and was stopped again), so
    /// waiting for the failure would leave it able to win next time. Idempotent.
    /// </summary>
    /// <remarks>It runs inside the console's own shell, so it must never call exit.</remarks>
    public static string EnsureScript => EnsureScriptFor(GuestNames.DesktopUser);

    /// <summary><see cref="EnsureScript"/> for a machine whose desktop runs as <paramref name="user"/>.</summary>
    public static string EnsureScriptFor(string user) => DesktopScriptsFor(user) + "\n" + $$"""
        if test -f /etc/systemd/system/personabot-desktop.service.d/one-display.conf; then
        echo "PersonaBot one-display: already in place"
        else
        for u in {{DisplayManagers}}; do systemctl mask "$u.service" >/dev/null 2>&1; done
        mkdir -p /etc/systemd/system/personabot-desktop.service.d
        cat > /etc/systemd/system/personabot-desktop.service.d/one-display.conf <<'PERSONABOT_EOF'
        {{DropIn}}
        PERSONABOT_EOF
        systemctl daemon-reload
        echo "PersonaBot one-display: installed"
        fi
        """;

    /// <summary>The file naming the desktop session PersonaBot starts on display :0 ("icewm" when missing).</summary>
    public const string DesktopFile = "/etc/personabot/desktop";

    /// <summary>The root helper any bot may run with sudo to switch the desktop session.</summary>
    public const string SetDesktopHelper = "/usr/local/bin/personabot-set-desktop";

    /// <summary>How long a chosen desktop has to keep running before PersonaBot trusts it started.</summary>
    public const int FallbackSeconds = 30;

    /// <summary>What the launcher writes (to the console, through xinit) when it falls back to IceWM.</summary>
    public const string FallbackMarker = "PERSONABOT_DESKTOP_FALLBACK";

    /// <summary>What the launcher writes when Firefox crashes and is opened again, or is left closed.</summary>
    public const string FirefoxMarker = "PERSONABOT_FIREFOX";

    /// <summary>What the launcher writes when it takes Plasma's GTK modules out of the user's GTK settings for another desktop.</summary>
    public const string KdeGtkModulesMarker = "PERSONABOT_GTK_MODULES";

    /// <summary>What the launcher writes when it starts the sound server.</summary>
    public const string SoundMarker = "PERSONABOT_SOUND";

    /// <summary>What the desktop scripts print when they have just sent the journal's warnings to the console.</summary>
    public const string JournalMarker = "PERSONABOT_JOURNAL";

    /// <summary>The start of the once-a-minute health line on the console.</summary>
    public const string HealthMarker = "PERSONABOT_HEALTH";

    /// <summary>The script that writes it, run by personabot-health.timer.</summary>
    public const string HealthScript = "/usr/local/bin/personabot-health";

    /// <summary>X's configuration for the software cursor (see DesktopScripts).</summary>
    public const string CursorConfig = "/etc/X11/xorg.conf.d/20-personabot-cursor.conf";

    /// <summary>What the desktop scripts print when they have just turned the software cursor on (X must restart).</summary>
    public const string CursorMarker = "PERSONABOT_CURSOR";

    /// <summary>Crashes in a row (each within a minute of opening) after which Firefox is left closed.</summary>
    public const int FirefoxRestarts = 5;

    /// <summary>
    /// The desktops a bot can switch to, with the program that must be installed and the packages that
    /// provide it. KDE Plasma: on Ubuntu 24.04 and Debian 12, kde-plasma-desktop only recommends SDDM;
    /// plasma-desktop doesn't list it; startplasma-x11 is in plasma-workspace (packages.ubuntu.com and
    /// packages.debian.org, checked 15 Nov 2026). kwin-x11 is only recommended, so it is named.
    /// GNOME: gdm3 is only a recommendation of gnome-shell on both; /usr/bin/gnome-session is in
    /// gnome-session-bin, and gnome-session (which depends on gnome-shell) ships the X11 session
    /// gnome-xorg.desktop (checked 15 Nov 2026). It runs PersonaBot's copy of the "gnome" session (see
    /// <see cref="GnomeSessionName"/>), as "gnome-session --session=personabot-gnome". The first
    /// version used --builtin, but gnome-session 46.0 rejects it ("Unknown option --builtin", exit 1),
    /// which a bot found and proved in a nested X server on 15 Nov 2026. Without a per-user systemd,
    /// gnome-session's activation of org.freedesktop.systemd1 fails and it carries on. It draws in
    /// software, as the VM has no GPU, so it is the heaviest choice.
    /// </summary>
    /// <remarks>Each entry: its name, title, the program that must be installed, the session command (run
    /// under dbus-run-session, except IceWM), the environment it needs, and the packages that provide it.</remarks>
    public static readonly IReadOnlyList<(string Name, string Title, string Program, string Command, string Environment, string Packages)> Desktops =
    [
        ("icewm", "IceWM", "icewm-session", "icewm-session", "", "icewm"),
        ("kde", "KDE Plasma", "startplasma-x11", "startplasma-x11", "XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true", "plasma-desktop kwin-x11"),
        ("xfce", "Xfce", "startxfce4", "startxfce4", "XDG_CURRENT_DESKTOP=XFCE", "xfce4"),
        // $gnome_session is PersonaBot's copy of GNOME's session (GnomeSessionName), or GNOME's own if it can't be made.
        ("gnome", "GNOME", "gnome-session", "gnome-session --session=$gnome_session", "XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_DESKTOP=gnome-xorg GDK_BACKEND=x11", "gnome-session gnome-terminal")
    ];

    /// <summary>GNOME's own session definition, which PersonaBot's is made from.</summary>
    public const string GnomeSystemSession = "/usr/share/gnome-session/sessions/gnome.session";

    /// <summary>PersonaBot's GNOME session, in the desktop user's ~/.config/gnome-session/sessions: GNOME's without gsd-usb-protection.</summary>
    public const string GnomeSessionName = "personabot-gnome";

    /// <summary>
    /// Run as root, idempotent, never restarting anything: writes PersonaBot's desktop launchers and the
    /// desktop switcher. New machines run it at first boot (cloud-provision.sh); every start runs it again
    /// (<see cref="EnsureScript"/>), so machines made before it get it too. Each file is written beside
    /// its target and moved into place: personabot-session is running while this runs, and bash reads a
    /// running script as it goes, so it must never be rewritten in place.
    /// </summary>
    public static string DesktopScripts => DesktopScriptsFor(GuestNames.DesktopUser);

    /// <summary><see cref="DesktopScripts"/> for a machine whose desktop runs as <paramref name="user"/> (its <see cref="Machine.DesktopUser"/>).</summary>
    public static string DesktopScriptsFor(string user)
    {
        GuestNames.CheckUser(user);
        System.Diagnostics.Trace.WriteLine($"CloudGuestRepair.DesktopScriptsFor: building the desktop scripts for {user}");
        var home = "/home/" + user;
        return $$"""
        mkdir -p /etc/personabot
        test -s {{DesktopFile}} || echo icewm > {{DesktopFile}}
        cat > /usr/local/bin/personabot-desktop.new <<'PERSONABOT_EOF'
        #!/bin/bash
        # PersonaBot's desktop on display :0, run as the desktop user: the session named in {{DesktopFile}}
        # (IceWM if it is missing or not installed), and Firefox. If the chosen desktop stops within its first
        # {{FallbackSeconds}} seconds, it didn't start: IceWM takes over, so the person never loses the desktop.
        set -uo pipefail
        export DISPLAY=:0 LANG=C.UTF-8 XDG_SESSION_TYPE=x11 XDG_RUNTIME_DIR=/run/user/$(id -u)
        desktop=$(cat {{DesktopFile}} 2>/dev/null || echo icewm)
        session=icewm-session
        gnome_session=gnome
        if [ "$desktop" = gnome ] && [ -f {{GnomeSystemSession}} ]; then
            # GNOME 46 requires gsd-usb-protection, which segfaults without org.gnome.ScreenSaver (only there
            # with GDM): after a few restarts gnome-session gives up and shows "Oh no! Something has gone wrong"
            # (15 Nov 2026). PersonaBot's copy of the session, made afresh from GNOME's own each start, leaves
            # it out, and it isn't autostarted. Both are the desktop user's files; GNOME's stay untouched.
            config="${HOME:-{{home}}}/.config"
            mkdir -p "$config/gnome-session/sessions" "$config/autostart"
            if sed -e 's/org\.gnome\.SettingsDaemon\.UsbProtection;//' {{GnomeSystemSession}} > "$config/gnome-session/sessions/{{GnomeSessionName}}.session.new" \
                && mv -f "$config/gnome-session/sessions/{{GnomeSessionName}}.session.new" "$config/gnome-session/sessions/{{GnomeSessionName}}.session"; then
                printf '[Desktop Entry]\nType=Application\nName=GNOME USB protection\nHidden=true\n' > "$config/autostart/org.gnome.SettingsDaemon.UsbProtection.desktop"
                gnome_session={{GnomeSessionName}}
            fi
        fi
        case "$desktop" in
        {{string.Join("\n", Desktops.Where(d => d.Name != "icewm").Select(d => $"""    {d.Name}) if command -v {d.Program} >/dev/null 2>&1; then export {d.Environment}; session="dbus-run-session -- {d.Command}"; fi ;;"""))}}
        esac
        if [ "$desktop" != kde ]; then
            # Plasma writes its GTK modules into the user's GTK settings (kde-gtk-config). Outside Plasma, GNOME's
            # settings daemon then unloads them from every GTK app a few seconds after it starts, and Firefox
            # crashes in a signal handler they left behind (15 Nov 2026). They belong to Plasma only, and Plasma
            # writes them again when it next starts.
            for settings in "${HOME:-{{home}}}/.config/gtk-3.0/settings.ini" "${HOME:-{{home}}}/.config/gtk-4.0/settings.ini"; do
                if grep -q -E '^gtk-modules=.*(colorreload|window-decorations)-gtk-module' "$settings" 2>/dev/null; then
                    sed -i '/^gtk-modules=/d' "$settings"
                    echo "{{KdeGtkModulesMarker}}: removed Plasma's GTK modules from $settings for $desktop" >&2
                fi
            done
        fi
        if command -v pipewire-pulse >/dev/null 2>&1 && ! pgrep -u "$(id -u)" -x pipewire >/dev/null 2>&1; then
            # Sound (CloudSound): no login session means no per-user systemd to start PipeWire, so the desktop does.
            # WirePlumber, which turns the sound card into an output, exits without a D-Bus session bus ("Failed
            # to connect to bus", 15 Nov 2026), so the three share one of their own. PipeWire's PulseAudio server
            # also listens on {{CloudSound.SharedSocket}}, which other accounts' programs use, so that socket is
            # made reachable by everyone (umask 0); its folder belongs to this user.
            bus=""
            command -v dbus-run-session >/dev/null 2>&1 && bus="dbus-run-session --"
            $bus bash -c 'pipewire & for n in $(seq 1 50); do [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break; sleep 0.1; done; wireplumber & (umask 0; exec pipewire-pulse) & wait' >/dev/null 2>&1 &
            echo "{{SoundMarker}}: started PipeWire for $desktop" >&2
        fi
        $session &
        started=$!
        # Restarting or switching the desktop stops Firefox mid-run; it must never then stop at its
        # "Open Firefox in Troubleshoot Mode?" question, which a bot can't see past.
        export MOZ_DISABLE_AUTO_SAFE_MODE=1
        browser=$(command -v firefox-esr || command -v firefox)
        (
            # Firefox opens once the desktop's window manager has taken the screen (a window made while
            # GNOME Shell or KWin takes over can crash it), and opens again if it crashes. Closing it
            # normally leaves it closed; {{FirefoxRestarts}} quick crashes in a row and it stays closed.
            for n in $(seq 1 20); do
                xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id' && break
                sleep 1
            done
            quick=0
            while true; do
                opened=$(date +%s)
                "$browser" --no-remote --profile {{GuestNames.BrowserProfile(home)}} about:blank
                code=$?
                ran=$(( $(date +%s) - opened ))
                [ "$code" = 0 ] && break
                if [ "$ran" -lt 60 ]; then quick=$((quick + 1)); else quick=1; fi
                if [ "$quick" -ge {{FirefoxRestarts}} ]; then
                    echo "{{FirefoxMarker}}: Firefox crashed $quick times in a row (code $code); leaving it closed" >&2
                    break
                fi
                echo "{{FirefoxMarker}}: Firefox stopped with code $code after ${ran}s; opening it again" >&2
                sleep 2
            done
        ) &
        if [ "$session" != icewm-session ]; then
            for n in $(seq 1 {{FallbackSeconds}}); do
                kill -0 "$started" 2>/dev/null || break
                sleep 1
            done
            if ! kill -0 "$started" 2>/dev/null; then
                wait "$started"
                echo "{{FallbackMarker}}: $desktop stopped with code $? while starting; using IceWM" >&2
                icewm-session &
            fi
        fi
        wait
        PERSONABOT_EOF
        cat > /usr/local/bin/personabot-session.new <<'PERSONABOT_EOF'
        #!/bin/bash
        # Started by xinit as root: waits for the X server, starts the desktop as the desktop user, and tells
        # PersonaBot on the serial console once Firefox is showing. That happens once per boot: a desktop
        # switch restarts this later, and must not write into the console's shell.
        set -uo pipefail
        export DISPLAY=:0 LANG=C.UTF-8
        console=/dev/null
        for candidate in /dev/ttyAMA0 /dev/ttyS0; do
            if [ -w "$candidate" ]; then console=$candidate; break; fi
        done
        for n in $(seq 1 60); do
            xdpyinfo >/dev/null 2>&1 && break
            sleep 1
        done
        # Plasma and Xfce need a runtime folder; without a login session nothing else makes one.
        install -d -m 700 -o {{user}} -g {{user}} "/run/user/$(id -u {{user}})"
        # The desktop's sound server puts the socket every account uses here (CloudSound).
        install -d -m 755 -o {{user}} -g {{user}} {{CloudSound.SharedFolder}}
        /usr/sbin/runuser -u {{user}} -- /usr/local/bin/personabot-desktop &
        desktop=$!
        ready=0
        for n in $(seq 1 300); do
            if xdotool search --onlyvisible --class firefox >/dev/null 2>&1; then ready=1; break; fi
            sleep 1
        done
        if [ "$ready" = 1 ]; then
            if [ ! -e /run/personabot-desktop-ready ]; then
                touch /run/personabot-desktop-ready
                echo {{GuestNames.BootReady}} > "$console"
            fi
            wait "$desktop"
        else
            echo 'PersonaBot Firefox startup timed out' > "$console"
            false
        fi
        PERSONABOT_EOF
        cat > {{SetDesktopHelper}}.new <<'PERSONABOT_EOF'
        #!/bin/bash
        # Switches PersonaBot's desktop on display :0. Any bot may run it with sudo:
        #   sudo personabot-set-desktop            shows the desktop in use and the choices
        #   sudo personabot-set-desktop kde        switches to KDE Plasma (it must be installed first)
        set -uo pipefail
        current=$(cat {{DesktopFile}} 2>/dev/null || echo icewm)
        want=${1:-}
        program=""; packages=""; title=""
        case "$want" in
        {{string.Join("\n", Desktops.Select(d => $"""    {d.Name}) program={d.Program}; packages="{d.Packages}"; title="{d.Title}" ;;"""))}}
        esac
        if [ -z "$want" ]; then
            echo "The desktop is $current."
        {{string.Join("\n", Desktops.Select(d => $"""    if command -v {d.Program} >/dev/null 2>&1; then echo "  {d.Name}: {d.Title}, installed"; else echo "  {d.Name}: {d.Title}, install with: sudo apt-get install -y --no-install-recommends {d.Packages}"; fi"""))}}
        elif [ -z "$program" ]; then
            echo "Unknown desktop '$want'. Choose one of: {{string.Join(", ", Desktops.Select(d => d.Name))}}." >&2
            false
        elif ! command -v "$program" >/dev/null 2>&1; then
            echo "$title isn't installed. Install it first: sudo apt-get install -y --no-install-recommends $packages" >&2
            false
        else
            echo "$want" > {{DesktopFile}}
            # A desktop's packages may have brought a login screen: it must never take display :0.
            for u in {{DisplayManagers}}; do systemctl mask --now "$u.service" >/dev/null 2>&1; done
            systemctl reset-failed personabot-desktop.service >/dev/null 2>&1
            systemctl --no-block restart personabot-desktop.service
            echo "Switching the desktop to $title: the screen restarts, then Firefox reopens (allow up to a minute)."
        fi
        PERSONABOT_EOF
        # Sound (CloudSound): the desktop user's PipeWire also serves {{CloudSound.SharedSocket}} to every account,
        # and every account's PulseAudio and ALSA programs play through it. Harmless before the packages are in.
        mkdir -p /etc/pipewire/pipewire-pulse.conf.d /etc/pulse/client.conf.d
        cat > /etc/pipewire/pipewire-pulse.conf.d/personabot.conf <<'PERSONABOT_EOF'
        # PersonaBot: every Linux account on this machine (each persona has one) plays sound here.
        pulse.properties = {
            server.address = [
                "unix:native"
                { address = "unix:{{CloudSound.SharedSocket}}" client.access = "unrestricted" }
            ]
        }
        PERSONABOT_EOF
        printf 'default-server = unix:%s\nautospawn = no\n' {{CloudSound.SharedSocket}} > /etc/pulse/client.conf.d/personabot.conf
        if [ ! -e /etc/asound.conf ] || grep -q PersonaBot /etc/asound.conf; then
            printf '# PersonaBot: ALSA programs play through the shared sound server.\npcm.!default { type pulse fallback "sysdefault" }\nctl.!default { type pulse fallback "sysdefault" }\n' > /etc/asound.conf
        fi
        # The cursor drawn into the screen (X's software cursor): QEMU's capture leaves out the hardware cursor, so
        # without this the person and the bots never see Linux's own pointer change shape (resize arrows, the hand
        # over links, the text bar). Checked on the Ubuntu machine with shared GPU, 15 Nov 2026: "SWcursor" in the
        # log, and the resize arrows in the captured frame at a window's edge. Takes effect when X next starts.
        mkdir -p /etc/X11/xorg.conf.d
        printf '%s\n' '# PersonaBot: draw the cursor into the screen, so captures show its real shape.' 'Section "Device"' '    Identifier "PersonaBot virtio GPU"' '    Driver "modesetting"' '    Option "SWcursor" "true"' 'EndSection' > {{CursorConfig}}.new
        if cmp -s {{CursorConfig}}.new {{CursorConfig}}; then rm -f {{CursorConfig}}.new; else mv -f {{CursorConfig}}.new {{CursorConfig}}; echo "{{CursorMarker}}: software cursor configured"; fi
        # The machine's own log on its serial console, which the app reads (and sends to syslog when asked): every
        # service's warnings and errors from the journal, not only the kernel's, so a crash arrives with its cause.
        personabot_tty=ttyS0
        if [ -e /dev/ttyAMA0 ]; then personabot_tty=ttyAMA0; fi
        mkdir -p /etc/systemd/journald.conf.d
        # The journal also kept on disk (so the next start can read the boot that died) and written out every 10
        # seconds rather than every 5 minutes, so a panic loses at most the last few seconds.
        printf '[Journal]\nForwardToConsole=yes\nTTYPath=/dev/%s\nMaxLevelConsole=warning\nStorage=persistent\nSyncIntervalSec=10s\n' "$personabot_tty" > /etc/systemd/journald.conf.d/personabot-console.conf.new
        if cmp -s /etc/systemd/journald.conf.d/personabot-console.conf.new /etc/systemd/journald.conf.d/personabot-console.conf; then
            rm -f /etc/systemd/journald.conf.d/personabot-console.conf.new
        else
            # The journal reads it at the machine's next start; this script never stops or starts anything.
            mv -f /etc/systemd/journald.conf.d/personabot-console.conf.new /etc/systemd/journald.conf.d/personabot-console.conf
            echo "{{JournalMarker}}: the journal's warnings reach the console from the next start"
        fi
        # Once a minute, a health line on the console: memory, swap, load, disk and processes, to show whether the
        # machine was starved before trouble.
        cat > {{HealthScript}}.new <<'PERSONABOT_EOF'
        #!/bin/sh
        console=/dev/null
        for candidate in /dev/ttyAMA0 /dev/ttyS0; do
            if [ -w "$candidate" ]; then console=$candidate; break; fi
        done
        memory=$(free -m | awk '/^Mem:/ {print $3 "/" $2 " MB used, " $7 " MB available"}')
        swap=$(free -m | awk '/^Swap:/ {print $3 "/" $2 " MB"}')
        load=$(cut -d' ' -f1-3 /proc/loadavg)
        disk=$(df -h / | awk 'NR==2 {print $3 "/" $2 " (" $5 ")"}')
        processes=$(ps -e --no-headers | wc -l)
        kernel=$(awk '/^(Slab|PageTables|Shmem):/ {sub(":", "", $1); printf "%s %d MB, ", $1, $2 / 1024}' /proc/meminfo)
        biggest=$(ps -eo rss=,comm= --sort=-rss | head -4 | awk '{printf "%s %d MB, ", $2, $1 / 1024}')
        crashes=$(dmesg 2>/dev/null | grep -c 'segfault at\|potentially unexpected fatal signal')
        up=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
        echo "{{HealthMarker}} up ${up}s; memory $memory; swap $swap; load $load; disk $disk; $processes processes; kernel ${kernel%, }; biggest ${biggest%, }; $crashes program crashes since boot" > "$console"
        PERSONABOT_EOF
        chmod 755 {{HealthScript}}.new
        mv -f {{HealthScript}}.new {{HealthScript}}
        printf '[Unit]\nDescription=PersonaBot health line on the console\n[Service]\nType=oneshot\nExecStart={{HealthScript}}\n' > /etc/systemd/system/personabot-health.service
        printf '[Unit]\nDescription=PersonaBot health line every minute\n[Timer]\nOnBootSec=60\nOnUnitActiveSec=60\nAccuracySec=5\n[Install]\nWantedBy=timers.target\n' > /etc/systemd/system/personabot-health.timer
        if ! systemctl is-active --quiet personabot-health.timer; then
            systemctl daemon-reload
            systemctl enable --now personabot-health.timer >/dev/null 2>&1 || true
        fi
        # When a program crashes, the kernel says which, where (the faulting address, the instruction, its registers)
        # on the console, every time rather than a few a minute: a burst of crashes across unrelated programs points at
        # memory going bad underneath them, and the addresses say where. Takes effect now and at every start.
        printf '%s\n' '# PersonaBot: report every crashing program on the console, with where it crashed.' 'kernel.printk = 7 4 1 7' 'kernel.print-fatal-signals = 1' 'debug.exception-trace = 1' 'kernel.printk_ratelimit = 0' > /etc/sysctl.d/90-personabot-crash-log.conf.new
        if cmp -s /etc/sysctl.d/90-personabot-crash-log.conf.new /etc/sysctl.d/90-personabot-crash-log.conf; then
            rm -f /etc/sysctl.d/90-personabot-crash-log.conf.new
        else
            mv -f /etc/sysctl.d/90-personabot-crash-log.conf.new /etc/sysctl.d/90-personabot-crash-log.conf
            echo "{{CrashLogMarker}}: crashing programs are reported on the console"
        fi
        sysctl -q -p /etc/sysctl.d/90-personabot-crash-log.conf >/dev/null 2>&1 || true
        # A disk grown on the Windows side (Settings, Disk size): the root partition and its filesystem grow into the
        # new space. growpart only grows into free space after the partition and says NOCHANGE otherwise; growing a
        # filesystem that already fills its partition does nothing. Ubuntu's cloud-init may have grown the partition
        # at boot already, so the filesystem is grown either way and the size compared before and after.
        personabot_root=$(findmnt -n -o SOURCE /)
        personabot_disk=$(lsblk -no PKNAME "$personabot_root" 2>/dev/null | head -n 1)
        personabot_partno=$(cat "/sys/class/block/$(basename "$personabot_root")/partition" 2>/dev/null)
        personabot_before=$(df -B1 --output=size / | tail -n 1 | tr -d ' ')
        if [ -n "$personabot_disk" ] && [ -n "$personabot_partno" ] && command -v growpart >/dev/null 2>&1; then
            growpart "/dev/$personabot_disk" "$personabot_partno" >/dev/null 2>&1 || true
        fi
        case "$(findmnt -n -o FSTYPE /)" in
            ext2|ext3|ext4) resize2fs "$personabot_root" >/dev/null 2>&1 || true ;;
            xfs) xfs_growfs / >/dev/null 2>&1 || true ;;
            btrfs) btrfs filesystem resize max / >/dev/null 2>&1 || true ;;
        esac
        personabot_after=$(df -B1 --output=size / | tail -n 1 | tr -d ' ')
        if [ "$personabot_after" -gt "$personabot_before" ] 2>/dev/null; then
            echo "{{DiskGrownMarker}}: / grew from $((personabot_before / 1048576)) MB to $((personabot_after / 1048576)) MB"
        fi
        # Program shadow stacks (Intel CET) off, from the next start. Under the Windows hypervisor one virtual CPU's
        # shadow-stack pointer leaked between programs: on 16 Nov 2026 some twenty unrelated programs (systemd,
        # journald, bash, Xorg...) crashed within seconds, all on CPU 14, all at one address, all shadow-stack faults
        # (page fault error 0x44/0x46), and earlier the same took down systemd itself (a kernel panic). Glibc turns
        # shadow stacks on for every program when the CPU offers them; clearcpuid=user_shstk is the kernel's switch.
        if [ -d /etc/default/grub.d ] && command -v update-grub >/dev/null 2>&1; then
            printf '%s\n' '# PersonaBot: no program shadow stacks (they crash under the Windows hypervisor).' 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT clearcpuid=user_shstk"' > /etc/default/grub.d/90-personabot-no-shadow-stacks.cfg.new
            if cmp -s /etc/default/grub.d/90-personabot-no-shadow-stacks.cfg.new /etc/default/grub.d/90-personabot-no-shadow-stacks.cfg; then
                rm -f /etc/default/grub.d/90-personabot-no-shadow-stacks.cfg.new
            else
                mv -f /etc/default/grub.d/90-personabot-no-shadow-stacks.cfg.new /etc/default/grub.d/90-personabot-no-shadow-stacks.cfg
                update-grub >/dev/null 2>&1 || true
            fi
        fi
        if grep -qw user_shstk /proc/cpuinfo; then
            echo "{{ShadowStackMarker}}: shadow stacks are on until the next start"
        fi
        # The journal on disk from now (journald moves to /var/log/journal when it exists and is flushed).
        mkdir -p /var/log/journal
        systemd-tmpfiles --create --prefix /var/log/journal >/dev/null 2>&1 || true
        journalctl --flush >/dev/null 2>&1 || true
        chmod 755 /usr/local/bin/personabot-desktop.new /usr/local/bin/personabot-session.new {{SetDesktopHelper}}.new
        mv -f /usr/local/bin/personabot-desktop.new /usr/local/bin/personabot-desktop
        mv -f /usr/local/bin/personabot-session.new /usr/local/bin/personabot-session
        mv -f {{SetDesktopHelper}}.new {{SetDesktopHelper}}
        if command -v visudo >/dev/null 2>&1 && test -d /etc/sudoers.d; then
            printf '%s\n' 'ALL ALL=(root) NOPASSWD: {{SetDesktopHelper}}' > /etc/sudoers.d/personabot-desktop.new
            chmod 440 /etc/sudoers.d/personabot-desktop.new
            if visudo -cf /etc/sudoers.d/personabot-desktop.new >/dev/null 2>&1; then mv -f /etc/sudoers.d/personabot-desktop.new /etc/sudoers.d/personabot-desktop; else rm -f /etc/sudoers.d/personabot-desktop.new; fi
        fi
        echo "PersonaBot desktop: $(cat {{DesktopFile}}), switcher ready"
        """;
    }

    /// <summary>The ensure script's line when Linux has grown into a disk made bigger on the Windows side.</summary>
    public const string DiskGrownMarker = "PERSONABOT_DISK_GROWN";

    /// <summary>The ensure script's line while program shadow stacks are still on (until the machine next starts).</summary>
    public const string ShadowStackMarker = "PERSONABOT_SHADOW_STACKS";

    /// <summary>The ensure script's line when it turns on reporting crashing programs (the first start after this update).</summary>
    public const string CrashLogMarker = "PERSONABOT_CRASH_LOG";

    /// <summary>
    /// Run as root at the start after a crash: the guest's own record of the boot that died, from its journal on disk
    /// (the kernel's messages, every warning and error, crashed programs). Read-only; the app saves it in the crash's
    /// folder (CrashReport). A machine whose journal wasn't kept on disk yet says so.
    /// </summary>
    public const string PostMortemScript = """
        echo "== Boots the journal remembers =="
        journalctl --list-boots --no-pager 2>&1 | tail -n 5
        echo
        echo "== The kernel's messages from the boot that died (last 400) =="
        journalctl -k -b -1 --no-pager -o short-precise 2>&1 | tail -n 400
        echo
        echo "== Warnings and errors from the boot that died (last 300) =="
        journalctl -b -1 -p warning --no-pager -o short-precise 2>&1 | tail -n 300
        echo
        echo "== The last 200 lines of the boot that died =="
        journalctl -b -1 --no-pager -o short-precise 2>&1 | tail -n 200
        echo
        echo "== Programs that crashed (core dumps) =="
        coredumpctl list --no-pager 2>&1 | tail -n 20
        ls -la /var/crash 2>&1 | tail -n 20
        """;

    /// <summary>Run as root on the machine's console: switch the login screens off, install the drop-in, restart the desktop.</summary>
    public static string DisplayManagerScript => $$"""
        for u in {{DisplayManagers}}; do systemctl mask --now "$u.service" >/dev/null 2>&1; done
        mkdir -p /etc/systemd/system/personabot-desktop.service.d
        cat > /etc/systemd/system/personabot-desktop.service.d/one-display.conf <<'PERSONABOT_EOF'
        {{DropIn}}
        PERSONABOT_EOF
        systemctl daemon-reload
        systemctl reset-failed personabot-desktop.service >/dev/null 2>&1
        systemctl --no-block restart personabot-desktop.service
        echo "PersonaBot switched off: $(for u in {{DisplayManagers}}; do systemctl is-enabled "$u.service" 2>/dev/null | grep -q masked && dpkg -s "$u" >/dev/null 2>&1 && printf '%s ' "$u"; done)"
        """;
}
