using System.Diagnostics;

namespace PersonaBot;

/// <summary>
/// Sound for Debian and Ubuntu machines on Windows. QEMU gives the guest an Intel HD Audio controller with an
/// output-only codec (hda-output: a line-out, no microphone, so nothing on the PC can be heard by the guest)
/// and plays it through DirectSound, the Windows audio backend in PersonaBot's QEMU build ("-audiodev help":
/// none, dbus, dsound, wav). Linux's snd_hda_intel drives it on both architectures. In the guest, PipeWire runs
/// as the desktop user and its PulseAudio server also listens on <see cref="SharedSocket"/>, which every
/// persona's Linux account reaches through /etc/pulse/client.conf.d, so a bot's Firefox is heard too.
/// </summary>
public static class CloudSound
{
    /// <summary>QEMU's id for the audio backend.</summary>
    public const string AudioDevId = "personabot-sound";

    /// <summary>When set to a file path, guest sound is written to that WAV file instead of the speakers (for checking sound with no one listening).</summary>
    public const string WavEnvironmentVariable = "PERSONABOT_SOUND_WAV";

    /// <summary>The PulseAudio socket every Linux account on the guest uses.</summary>
    public const string SharedSocket = SharedFolder + "/native";

    /// <summary>The folder holding <see cref="SharedSocket"/>: the desktop user's, readable by all.</summary>
    public const string SharedFolder = "/run/personabot-sound";

    /// <summary>What the start-up sound check prints when the guest has a working sound output.</summary>
    public const string ReadyMarker = "PERSONABOT_SOUND_READY";

    /// <summary>The guest packages: the PipeWire sound server with its PulseAudio server and session manager, the
    /// PulseAudio tools (pactl, paplay), ALSA's tools, and ALSA's PulseAudio plugin so ALSA-only programs are heard
    /// too. Names checked on packages.ubuntu.com (noble) and packages.debian.org (bookworm, trixie), 15 Nov 2026.</summary>
    public const string Packages = "pipewire pipewire-pulse wireplumber pulseaudio-utils alsa-utils libasound2-plugins";

    /// <summary>QEMU's arguments for the sound card: played on this PC, or written to <paramref name="wavPath"/> when given.</summary>
    public static IReadOnlyList<string> Arguments(string architecture, string? wavPath = null, bool stream = false)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(architecture);
        Trace.WriteLine($"CloudSound.Arguments: {architecture}, {(wavPath == null ? "DirectSound" : "WAV " + wavPath)}");
        // QEMU option values escape a comma by doubling it.
        var backend = stream ? $"none,id={AudioDevId}" : wavPath == null
            ? $"dsound,id={AudioDevId}"
            : $"wav,id={AudioDevId},path={Path.GetFullPath(wavPath).Replace(",", ",,", StringComparison.Ordinal)}";
        // q35 is an ICH9 chipset, whose own HD Audio controller is ich9-intel-hda; the ARM virt board takes the plain PCI one.
        var controller = architecture == "aarch64" ? "intel-hda" : "ich9-intel-hda";
        return ["-audiodev", backend, "-device", $"{controller},id=personabot-hda", "-device", $"hda-output,bus=personabot-hda.0,audiodev={AudioDevId}"];
    }

    /// <summary>The WAV file named by <see cref="WavEnvironmentVariable"/>, or null for the speakers.</summary>
    public static string? WavPathFromEnvironment()
    {
        var path = Environment.GetEnvironmentVariable(WavEnvironmentVariable);
        if (!string.IsNullOrWhiteSpace(path)) Trace.WriteLine($"CloudSound.WavPathFromEnvironment: {WavEnvironmentVariable} sends sound to {path}");
        return string.IsNullOrWhiteSpace(path) ? null : path;
    }

    /// <summary>
    /// Run as root at every start when sound is on: installs the sound packages a machine made before sound lacks
    /// (and, on Ubuntu, the kernel's extra modules, where snd-hda-intel lives), puts the desktop user in the audio
    /// group, loads the driver, and restarts the desktop once if anything changed so PipeWire starts with it.
    /// Then reports the sound server and its outputs, ending with <see cref="ReadyMarker"/> when there is one.
    /// </summary>
    public static string GuestScript => GuestScriptFor(GuestNames.DesktopUser);

    /// <summary><see cref="GuestScript"/> for a machine whose desktop runs as <paramref name="user"/>.</summary>
    public static string GuestScriptFor(string user)
    {
        GuestNames.CheckUser(user);
        Trace.WriteLine($"CloudSound.GuestScriptFor: building the sound check script for {user}");
        return $$"""
        export DEBIAN_FRONTEND=noninteractive
        changed=0
        missing=""
        for package in {{Packages}}; do
            dpkg-query -W -f='${Status}\n' "$package" 2>/dev/null | grep -qx 'install ok installed' || missing="$missing $package"
        done
        if ! find /lib/modules/$(uname -r) -name 'snd-hda-intel.ko*' 2>/dev/null | grep -q .; then
            . /etc/os-release
            [ "$ID" = ubuntu ] && missing="$missing linux-modules-extra-$(uname -r)"
        fi
        if [ -n "$missing" ]; then
            echo "Installing sound packages:$missing"
            apt-get update -q >/dev/null 2>&1
            apt-get install -y -q --no-install-recommends $missing >/dev/null 2>&1 || apt-get install -y -q --no-install-recommends $missing 2>&1 | tail -5
            changed=1
        fi
        if ! id -nG {{user}} | tr ' ' '\n' | grep -qx audio; then
            usermod -a -G audio {{user}}
            changed=1
        fi
        if [ ! -d /proc/asound/card0 ]; then
            modprobe snd_hda_intel 2>&1 && changed=1
            for n in $(seq 1 10); do [ -d /proc/asound/card0 ] && break; sleep 1; done
        fi
        if [ "$changed" = 1 ] && command -v pipewire-pulse >/dev/null 2>&1; then
            echo "Restarting the desktop so its sound server starts"
            systemctl reset-failed personabot-desktop.service >/dev/null 2>&1
            systemctl restart personabot-desktop.service
            for n in $(seq 1 90); do
                xdotool search --onlyvisible --class firefox >/dev/null 2>&1 && break
                sleep 1
            done
        fi
        for n in $(seq 1 20); do
            pactl info >/dev/null 2>&1 && break
            sleep 1
        done
        cat /proc/asound/cards 2>&1
        pactl info 2>&1 | grep -E '^(Server Name|Default Sink)'
        pactl list short sinks 2>&1
        if pactl list short sinks 2>/dev/null | grep -q alsa_output; then echo {{ReadyMarker}}; fi
        """;
    }
}
