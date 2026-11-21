using System.Diagnostics;
using System.Runtime.CompilerServices;
using Automaton.Core.Services.ComputerUse;

namespace PersonaBot;

/// <summary>
/// A bot's own display inside a Debian or Ubuntu machine (XMEETING.md §3, its "VPS"): an Xvnc server run
/// as the bot's Linux user, with a desktop session of its own. The app reaches it over RFB through a
/// QEMU port forward that listens on this PC's loopback only. Display :0 stays the machine's main display.
/// </summary>
/// <remarks>
/// Prototype (15 Nov 2026). The RFB ports have no password, by decision: they are never exposed over the
/// network (the PC's forward listens on 127.0.0.1 only; the guest's port is on QEMU's private user
/// network). Keep it that way: never forward them from another address. Another account inside the guest
/// could still connect to one; windows on the display are protected by an X authority cookie.
/// </remarks>
public static class BotDisplays
{
    /// <summary>How many bot displays a machine has ports for: displays :1 to :<see cref="Count"/>.</summary>
    public const int Count = 16;

    /// <summary>The address QEMU's user network gives the guest, which forwarded ports arrive at.</summary>
    public const string GuestAddress = "10.0.2.15";

    /// <summary>The launcher, run as the bot's user.</summary>
    public const string Launcher = "/usr/local/bin/personabot-vps";

    /// <summary>Where each display's log is kept, named by its number.</summary>
    public const string LogFolder = "/run/personabot-vps";

    /// <summary>What the launcher writes once its X server is up and the session is starting.</summary>
    public const string ReadyMarker = "PERSONABOT_VPS_READY";

    /// <summary>What the launcher or the start script writes when a display can't start.</summary>
    public const string FailedMarker = "PERSONABOT_VPS_FAILED";

    /// <summary>What the install script writes when the packages are in.</summary>
    public const string InstalledMarker = "PERSONABOT_VPS_INSTALLED";

    /// <summary>The X server and the tools the launcher needs, besides the desktop's own packages.</summary>
    public const string Packages = "tigervnc-standalone-server xauth dbus-x11";

    /// <summary>The RFB port a display listens on inside the guest.</summary>
    public static int GuestPort(int number) => 5900 + Checked(number);

    /// <summary>The bot's X authority file for a display.</summary>
    public static string Authority(GuestAccount account, int number)
    {
        ArgumentNullException.ThrowIfNull(account);
        return account.DisplayAuthority(Checked(number));
    }

    /// <summary>The QEMU user-network options that forward a loopback port on this PC to each display's port.</summary>
    public static string Forwards(IReadOnlyList<int> hostPorts)
    {
        ArgumentNullException.ThrowIfNull(hostPorts);
        Trace.WriteLine($"BotDisplays.Forwards enter: {hostPorts.Count} host ports [{string.Join(',', hostPorts)}]");
        if (hostPorts.Count > Count)
        {
            Trace.TraceWarning($"BotDisplays.Forwards: {hostPorts.Count} ports is more than {Count} displays");
            throw new ArgumentException($"A machine has at most {Count} bot displays.");
        }
        if (hostPorts.Any(p => p is < 1 or > 65535) || hostPorts.Distinct().Count() != hostPorts.Count)
        {
            Trace.TraceWarning("BotDisplays.Forwards: ports are out of range or repeated");
            throw new ArgumentException("Use different valid loopback ports.");
        }
        Trace.WriteLine($"BotDisplays.Forwards exit: forwarding displays :1 to :{hostPorts.Count}");
        return string.Concat(hostPorts.Select((port, index) => $",hostfwd=tcp:127.0.0.1:{port}-{GuestAddress}:{GuestPort(index + 1)}"));
    }

    /// <summary>The desktops a bot display can run: those of the main display (<see cref="CloudGuestRepair.Desktops"/>).</summary>
    public static (string Name, string Title, string Program, string Command, string Environment, string Packages) Desktop(string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        var found = CloudGuestRepair.Desktops.FirstOrDefault(d => d.Name == name);
        if (found.Name == null) Trace.TraceWarning($"BotDisplays.Desktop: unknown desktop '{name}'");
        return found.Name != null ? found : throw new ArgumentException($"Unknown desktop '{name}'. Choose one of: {string.Join(", ", CloudGuestRepair.Desktops.Select(d => d.Name))}.");
    }

    /// <summary>
    /// Run as root: installs the display server and the desktop's packages in the background (it can take
    /// longer than one shell command may), logging to <see cref="LogFolder"/>/install.log and ending it with
    /// <see cref="InstalledMarker"/> or <see cref="FailedMarker"/>. Poll with <see cref="InstallStatusScript"/>.
    /// </summary>
    public static string InstallScript(string desktop)
    {
        var packages = Packages + " " + Desktop(desktop).Packages;
        Trace.WriteLine($"BotDisplays.InstallScript: building the install script for desktop '{desktop}', packages: {packages}");
        return $$"""
            install -d -m 755 {{LogFolder}}
            if dpkg -s {{packages}} >/dev/null 2>&1; then
                echo "{{InstalledMarker}}: already installed" > {{LogFolder}}/install.log
            elif ! pgrep -f personabot-vps-install >/dev/null 2>&1; then
                cat > {{LogFolder}}/personabot-vps-install <<'PERSONABOT_EOF'
            #!/bin/bash
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -q
            if apt-get install -y -q --no-install-recommends {{packages}}; then
                # A desktop's packages may have brought a login screen: it must never take display :0.
                for u in {{CloudGuestRepair.DisplayManagers}}; do systemctl mask --now "$u.service" >/dev/null 2>&1; done
                echo "{{InstalledMarker}}"
            else
                echo "{{FailedMarker}}: the packages didn't install"
            fi
            PERSONABOT_EOF
                chmod 700 {{LogFolder}}/personabot-vps-install
                setsid nohup {{LogFolder}}/personabot-vps-install > {{LogFolder}}/install.log 2>&1 < /dev/null &
            fi
            echo "PersonaBot bot displays: installing {{packages}}"
            """;
    }

    /// <summary>Run as root: the last lines of the install log, which end with a marker when it has finished.</summary>
    public static string InstallStatusScript => $"tail -n 3 {LogFolder}/install.log 2>/dev/null";

    /// <summary>
    /// Run as root, idempotent: writes the launcher. It is written beside its target and moved into place,
    /// as a running script must never be rewritten.
    /// </summary>
    public static string LauncherScript => $$"""
        cat > {{Launcher}}.new <<'PERSONABOT_EOF'
        #!/bin/bash
        # A bot's own display, run as the bot's Linux user: personabot-vps <number> <width>x<height> <desktop>
        # Starts Xvnc on display :<number> and the desktop session on it, and ends when the session does.
        set -uo pipefail
        n=$1; size=$2; desktop=${3:-icewm}
        export HOME=$(getent passwd "$(id -u)" | cut -d: -f6) USER=$(id -un) LOGNAME=$(id -un)
        cd "$HOME" || exit 1
        export DISPLAY=:$n LANG=C.UTF-8 XDG_SESSION_TYPE=x11 XDG_RUNTIME_DIR=/run/user/$(id -u)
        export XAUTHORITY=$HOME/.personabot-vps-$n.Xauthority
        # Only this user may open windows on the display: an X authority cookie made for this run.
        rm -f "$XAUTHORITY"; (umask 077; : > "$XAUTHORITY")
        xauth -q add ":$n" . "$(mcookie)"
        Xvnc ":$n" -geometry "$size" -depth 24 -auth "$XAUTHORITY" -rfbport $((5900 + n)) -interface {{GuestAddress}} \
            -SecurityTypes None -AlwaysShared -AcceptSetDesktopSize=0 -desktop "PersonaBot $USER" &
        server=$!
        up=0
        for i in $(seq 1 100); do
            if xdpyinfo >/dev/null 2>&1; then up=1; break; fi
            kill -0 "$server" 2>/dev/null || break
            sleep 0.2
        done
        if [ "$up" != 1 ]; then
            echo "{{FailedMarker}}: the display server didn't start"
            kill "$server" 2>/dev/null
            exit 1
        fi
        session=icewm-session
        gnome_session=gnome
        if [ "$desktop" = gnome ] && [ -f {{CloudGuestRepair.GnomeSystemSession}} ]; then
            # As on the main display (personabot-desktop): GNOME's session without gsd-usb-protection.
            config="$HOME/.config"
            mkdir -p "$config/gnome-session/sessions" "$config/autostart"
            if sed -e 's/org\.gnome\.SettingsDaemon\.UsbProtection;//' {{CloudGuestRepair.GnomeSystemSession}} > "$config/gnome-session/sessions/{{CloudGuestRepair.GnomeSessionName}}.session.new" \
                && mv -f "$config/gnome-session/sessions/{{CloudGuestRepair.GnomeSessionName}}.session.new" "$config/gnome-session/sessions/{{CloudGuestRepair.GnomeSessionName}}.session"; then
                printf '[Desktop Entry]\nType=Application\nName=GNOME USB protection\nHidden=true\n' > "$config/autostart/org.gnome.SettingsDaemon.UsbProtection.desktop"
                gnome_session={{CloudGuestRepair.GnomeSessionName}}
            fi
        fi
        case "$desktop" in
        {{string.Join("\n", CloudGuestRepair.Desktops.Where(d => d.Name != "icewm").Select(d => $"""    {d.Name}) if command -v {d.Program} >/dev/null 2>&1; then export {d.Environment}; session="dbus-run-session -- {d.Command}"; fi ;;"""))}}
        esac
        echo "{{ReadyMarker}}: display :$n for $USER, $size, $session"
        $session
        code=$?
        echo "PersonaBot display :$n: the session ended with code $code"
        kill "$server" 2>/dev/null
        exit "$code"
        PERSONABOT_EOF
        chmod 755 {{Launcher}}.new
        mv -f {{Launcher}}.new {{Launcher}}
        """;

    /// <summary>
    /// Run as root: starts a display for a bot, unless it is already running, and waits until its X server is
    /// up. Its output ends with <see cref="ReadyMarker"/> or <see cref="FailedMarker"/>. It runs in a
    /// subshell, because the console's own shell must never exit.
    /// </summary>
    public static string StartScript(GuestAccount account, int number, GuestResolution size, string desktop)
    {
        ArgumentNullException.ThrowIfNull(account);
        ArgumentNullException.ThrowIfNull(size);
        var name = Desktop(desktop).Name;
        var user = GuestShellText.Quote(account.User);
        var log = $"{LogFolder}/{Checked(number)}.log";
        Trace.WriteLine($"BotDisplays.StartScript: building the start script for :{number}, user {account.User}, {size.Width}x{size.Height}, desktop {name}, log {log}");
        return $$"""
            (
            user={{user}}
            if ! id "$user" >/dev/null 2>&1; then echo "{{FailedMarker}}: there is no Linux user $user"; exit 1; fi
            if ! command -v Xvnc >/dev/null 2>&1; then echo "{{FailedMarker}}: the display server isn't installed"; exit 1; fi
            if pgrep -u "$user" -f "^Xvnc :{{number}} " >/dev/null 2>&1; then echo "{{ReadyMarker}}: display :{{number}} is already running"; exit 0; fi
            # Without a login session nothing else makes the runtime folder a desktop needs.
            install -d -m 700 -o "$user" -g "$(id -gn "$user")" "/run/user/$(id -u "$user")"
            install -d -m 755 {{LogFolder}}
            : > {{log}}
            setsid /usr/sbin/runuser -u "$user" -- {{Launcher}} {{number}} {{size.Width}}x{{size.Height}} {{name}} >> {{log}} 2>&1 < /dev/null &
            for i in $(seq 1 150); do
                grep -q -e {{ReadyMarker}} -e {{FailedMarker}} {{log}} && break
                sleep 0.2
            done
            tail -n 5 {{log}}
            grep -q {{ReadyMarker}} {{log}} || echo "{{FailedMarker}}: display :{{number}} didn't come up"
            )
            """;
    }

    /// <summary>Run as root: stops a bot's display; its session ends with the X server.</summary>
    public static string StopScript(GuestAccount account, int number)
    {
        ArgumentNullException.ThrowIfNull(account);
        Trace.WriteLine($"BotDisplays.StopScript: building the stop script for :{number}, user {account.User}");
        return $"pkill -u {GuestShellText.Quote(account.User)} -f '^Xvnc :{Checked(number)} ' ; rm -f {GuestShellText.Quote(Authority(account, number))}";
    }

    /// <summary>A command run as root against a bot's display, e.g. xdotool or xprop, with its X authority.</summary>
    public static string OnDisplay(GuestAccount account, int number, string command)
    {
        ArgumentNullException.ThrowIfNull(account);
        ArgumentException.ThrowIfNullOrWhiteSpace(command);
        return $"DISPLAY=:{Checked(number)} XAUTHORITY={GuestShellText.Quote(Authority(account, number))} {command}";
    }

    private static int Checked(int number) =>
        number is >= 1 and <= Count ? number : throw new ArgumentOutOfRangeException(nameof(number), $"Bot displays are numbered 1 to {Count}.");
}

/// <summary>
/// One running bot display, as the app sees it: a screen the computer-use tools can be given, with a live
/// stream for the viewer. Every capture, stream and input channel has its own RFB connection.
/// </summary>
public sealed class BotDisplayScreen : IBotScreen, IGuestDisplayStream, IAsyncDisposable
{
    private readonly int _port;
    private readonly SemaphoreSlim _captureGate = new(1);
    private readonly List<RfbInput> _inputs = [];
    private QemuFramebuffer? _capture;
    private bool _disposed;

    /// <param name="port">The loopback port on this PC that QEMU forwards to the display.</param>
    public BotDisplayScreen(GuestAccount account, int number, int port, GuestResolution size)
    {
        Account = account ?? throw new ArgumentNullException(nameof(account));
        ArgumentNullException.ThrowIfNull(size);
        if (port is < 1 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        Number = number;
        _port = port;
        ScreenBounds = new ScreenRect(0, 0, size.Width, size.Height);
        Trace.WriteLine($"BotDisplayScreen: created :{number} for {account.User}, {size.Width}x{size.Height}, loopback port {port}");
    }

    public GuestAccount Account { get; }

    /// <summary>The X display number inside the guest.</summary>
    public int Number { get; }

    public int DisplayNumber => Number;

    public ScreenRect ScreenBounds { get; }

    /// <summary>
    /// Starts the display again if its server has stopped (set by <see cref="CloudRuntime"/>); null where nothing
    /// can. Used when a connection to the display can't be opened.
    /// </summary>
    public Func<CancellationToken, Task>? Revive { get; set; }

    private readonly SemaphoreSlim _reviveGate = new(1);
    private long _revivedAt;

    /// <summary>How many times in a row the picture stream reconnects before it gives up.</summary>
    private const int StreamRetries = 6;

    /// <summary>
    /// Opens and negotiates a new RFB connection to the display. If that fails, the display's server is started
    /// again (once at a time, and at most every few seconds) and the connection tried once more.
    /// </summary>
    internal async Task<QemuFramebuffer> ConnectAsync(CancellationToken token)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Trace.WriteLine($"BotDisplayScreen.ConnectAsync enter: :{Number} on port {_port}, canRevive={Revive != null}");
        try { return await OpenAsync(token); }
        catch (Exception ex) when (Revive != null && ex is IOException or System.Net.Sockets.SocketException or EndOfStreamException)
        {
            Trace.WriteLine($"BotDisplayScreen.ConnectAsync: :{Number} didn't answer ({ex.Message}); starting it again");
        }
        await _reviveGate.WaitAsync(token);
        try
        {
            // Another connection may just have revived it.
            var sinceRevive = Environment.TickCount64 - Interlocked.Read(ref _revivedAt);
            if (sinceRevive > 5000)
            {
                Trace.WriteLine($"BotDisplayScreen.ConnectAsync: reviving :{Number}");
                var watch = Stopwatch.StartNew();
                await Revive!(token);
                Interlocked.Exchange(ref _revivedAt, Environment.TickCount64);
                Trace.WriteLine($"BotDisplayScreen.ConnectAsync: :{Number} revived in {watch.ElapsedMilliseconds}ms");
            }
            else
            {
                Trace.WriteLine($"BotDisplayScreen.ConnectAsync: :{Number} was revived {sinceRevive}ms ago; not reviving again");
            }
        }
        finally { _reviveGate.Release(); }
        Trace.WriteLine($"BotDisplayScreen.ConnectAsync exit: retrying :{Number} after the revive");
        return await OpenAsync(token);
    }

    private async Task<QemuFramebuffer> OpenAsync(CancellationToken token)
    {
        var connection = new QemuFramebuffer();
        try { await connection.ConnectAsync(_port, token); }
        catch (Exception ex)
        {
            Trace.TraceWarning($"BotDisplayScreen.OpenAsync: :{Number} on port {_port} failed: {ex.GetType().Name}: {ex.Message}");
            await connection.DisposeAsync();
            throw;
        }
        Trace.WriteLine($"BotDisplayScreen.OpenAsync: :{Number} connected on port {_port}");
        return connection;
    }

    public async Task<byte[]> CaptureAsync(CancellationToken token)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await _captureGate.WaitAsync(token);
        try
        {
            if (_capture == null) Trace.WriteLine($"BotDisplayScreen.CaptureAsync: opening a capture connection to :{Number}");
            _capture ??= await ConnectAsync(token);
            try { return await _capture.CaptureAsync(token); }
            catch (Exception ex)
            {
                Trace.TraceWarning($"BotDisplayScreen.CaptureAsync: :{Number} capture failed, dropping the connection: {ex.GetType().Name}: {ex.Message}");
                // A broken connection is half-read: never reuse it.
                await _capture.DisposeAsync();
                _capture = null;
                throw;
            }
        }
        finally { _captureGate.Release(); }
    }

    /// <summary>
    /// The live picture. A dropped connection (the display's server restarted, or QEMU's forward closed) is opened
    /// again, starting the server if need be, so the view carries on; it gives up after several failures in a row.
    /// </summary>
    public async IAsyncEnumerable<GuestDisplayFrame> StreamDisplayAsync([EnumeratorCancellation] CancellationToken token)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var failures = 0;
        Trace.WriteLine($"BotDisplayScreen.StreamDisplayAsync enter: :{Number}");
        while (!token.IsCancellationRequested)
        {
            var display = await ConnectAsync(token);
            Trace.WriteLine($"BotDisplayScreen.StreamDisplayAsync: streaming :{Number} (failures so far {failures})");
            try
            {
                var incremental = false;
                while (!token.IsCancellationRequested)
                {
                    GuestDisplayFrame frame;
                    try { frame = await display.ReadFrameAsync(incremental, token); }
                    catch (Exception ex) when (ex is IOException or System.Net.Sockets.SocketException or EndOfStreamException or ObjectDisposedException)
                    {
                        if (++failures > StreamRetries)
                        {
                            Trace.TraceError($"BotDisplayScreen.StreamDisplayAsync: :{Number} gave up after {StreamRetries} reconnects: {ex.Message}");
                            throw new IOException($"Display :{Number} keeps dropping its connection: {ex.Message}", ex);
                        }
                        Trace.WriteLine($"BotDisplayScreen.StreamDisplayAsync: :{Number} dropped ({ex.Message}); reconnecting ({failures}/{StreamRetries})");
                        break;
                    }
                    failures = 0;
                    yield return frame;
                    incremental = true;
                }
            }
            finally { await display.DisposeAsync(); }
            if (!token.IsCancellationRequested) await Task.Delay(TimeSpan.FromMilliseconds(300 * failures), token);
        }
        Trace.WriteLine($"BotDisplayScreen.StreamDisplayAsync exit: :{Number} stream cancelled");
    }

    public IGuestInput CreateInput()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Trace.WriteLine($"BotDisplayScreen.CreateInput enter: :{Number}");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        async Task<(Stream, IAsyncDisposable?)> Open(CancellationToken t)
        {
            var connection = await ConnectAsync(t);
            return (connection.Stream, connection);
        }
        var input = Task.Run(() => RfbInput.ConnectAsync(Open, token), token).GetAwaiter().GetResult();
        int inputCount;
        lock (_inputs) { _inputs.Add(input); inputCount = _inputs.Count; }
        Trace.WriteLine($"BotDisplayScreen.CreateInput exit: :{Number} now has {inputCount} input connections");
        return input;
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        RfbInput[] inputs;
        lock (_inputs) { inputs = [.. _inputs]; _inputs.Clear(); }
        Trace.WriteLine($"BotDisplayScreen.DisposeAsync enter: :{Number}, {inputs.Length} inputs, capture={_capture != null}");
        foreach (var input in inputs) await input.DisposeAsync();
        if (_capture != null) await _capture.DisposeAsync();
        Trace.WriteLine($"BotDisplayScreen.DisposeAsync exit: :{Number}");
    }
}
