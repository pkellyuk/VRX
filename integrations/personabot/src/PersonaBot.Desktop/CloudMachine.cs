using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using Automaton.Core.Services.ComputerUse;
using DiscUtils.Iso9660;

namespace PersonaBot;

/// <summary>A QEMU runtime driven over QMP, for the shared input code.</summary>
public interface IQmpRuntime
{
    QmpClient Qmp { get; }
    ScreenRect ScreenBounds { get; }
}

/// <summary>Where a distribution's cloud image comes from and how its checksum is published (DISTROS.md §2).</summary>
public sealed record CloudImage(string Distribution, string Architecture, string BaseUrl, string FileName, string ChecksumFile, HashAlgorithmName Hash);

public static class CloudImages
{
    public static IReadOnlyList<CloudImage> All { get; } =
    [
        new("ubuntu-24.04", "x86_64", "https://cloud-images.ubuntu.com/minimal/releases/noble/release/", "ubuntu-24.04-minimal-cloudimg-amd64.img", "SHA256SUMS", HashAlgorithmName.SHA256)
    ];

    public static CloudImage? For(Machine machine)
    {
        ArgumentNullException.ThrowIfNull(machine);
        return All.FirstOrDefault(i => i.Distribution == machine.Distribution && i.Architecture == machine.Architecture);
    }

    /// <summary>
    /// The checksum of <paramref name="fileName"/> in a published list: Debian writes "hash  name",
    /// Ubuntu "hash *name".
    /// </summary>
    public static string ReadChecksum(string manifest, string fileName)
    {
        ArgumentNullException.ThrowIfNull(manifest);
        ArgumentException.ThrowIfNullOrWhiteSpace(fileName);
        foreach (var line in manifest.Split('\n'))
        {
            var parts = line.Trim().Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 2 && parts[1].Trim().TrimStart('*') == fileName && parts[0].All(Uri.IsHexDigit))
            {
                Trace.WriteLine($"CloudImages.ReadChecksum: found {fileName} ({parts[0].Length} hex digits)");
                return parts[0].ToLowerInvariant();
            }
        }
        Trace.TraceError($"CloudImages.ReadChecksum: {fileName} isn't in the published list ({manifest.Length} chars)");
        throw new IOException($"The published checksums don't list {fileName}.");
    }

    /// <summary>The marker in cloud-provision.sh where the desktop launchers and switcher go.</summary>
    public const string DesktopScriptsMarker = "#PERSONABOT_DESKTOP_SCRIPTS#";

    /// <summary>The marker in cloud-provision.sh where the desktop user's name goes.</summary>
    public const string DesktopUserMarker = "#PERSONABOT_DESKTOP_USER#";

    /// <summary>
    /// The first-boot script as the guest runs it: Unix line endings, with the desktop user's name and
    /// <see cref="CloudGuestRepair.DesktopScripts"/> for that user in place.
    /// </summary>
    public static string ProvisionScript(string template, string? desktopUser = null)
    {
        ArgumentNullException.ThrowIfNull(template);
        var user = GuestNames.CheckUser(desktopUser ?? GuestNames.DesktopUser);
        var script = template.Replace("\r\n", "\n");
        Trace.WriteLine($"CloudImages.ProvisionScript: filling the provisioning script ({script.Length} chars) for desktop user {user}");
        if (!script.Contains(DesktopScriptsMarker, StringComparison.Ordinal))
        {
            Trace.TraceError("CloudImages.ProvisionScript: the template has no desktop scripts marker");
            throw new InvalidDataException("The provisioning script has no place for the desktop scripts.");
        }
        if (!script.Contains(DesktopUserMarker, StringComparison.Ordinal))
        {
            Trace.TraceError("CloudImages.ProvisionScript: the template has no desktop user marker");
            throw new InvalidDataException("The provisioning script has no place for the desktop user.");
        }
        return script.Replace(DesktopUserMarker, user).Replace(DesktopScriptsMarker, CloudGuestRepair.DesktopScriptsFor(user).Replace("\r\n", "\n"));
    }

    /// <summary>The first-boot cloud-config: the desktop user, the packages, and the provisioning script.</summary>
    /// <param name="desktopUser">The machine's <see cref="Machine.DesktopUser"/>; <see cref="GuestNames.DesktopUser"/> when null.</param>
    public static string BuildCloudConfig(string distribution, string? desktopUser = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(distribution);
        var user = GuestNames.CheckUser(desktopUser ?? GuestNames.DesktopUser);
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("PersonaBot.Guest.cloud-provision.sh")
            ?? throw new IOException("PersonaBot's guest provisioning script is missing from the app.");
        using var reader = new StreamReader(stream);
        var script = Convert.ToBase64String(Encoding.UTF8.GetBytes(ProvisionScript(reader.ReadToEnd(), user)));
        // Firefox ESR is a Debian package; Ubuntu's comes from Mozilla's repository in the script.
        var browser = distribution.StartsWith("debian", StringComparison.Ordinal) ? "\n  - firefox-esr" : "";
        // Sound (CloudSound): installed with the desktop whether or not it is switched on now.
        var sound = string.Concat(CloudSound.Packages.Split(' ').Select(package => "\n  - " + package));
        Trace.WriteLine($"CloudImages.BuildCloudConfig: {distribution} for {user}, provisioning script {script.Length} base64 chars, firefox-esr={browser.Length > 0}");
        return $"""
            #cloud-config
            users:
              - name: {user}
                shell: /bin/bash
                lock_passwd: true
                groups: [video, render, audio]
            ssh_pwauth: false
            disable_root: true
            package_update: true
            packages:
              - xserver-xorg
              - xserver-xorg-video-all
              - libgl1-mesa-dri
              - libegl-mesa0
              - mesa-utils
              - xinit
              - x11-xserver-utils
              - x11-utils
              - xterm
              - icewm
              - xdotool
              - tesseract-ocr
              - tesseract-ocr-eng
              - fonts-dejavu-core
              - fontconfig
              - ca-certificates
              - curl
              - sudo{browser}{sound}
            write_files:
              - path: /usr/local/sbin/personabot-provision
                permissions: '0700'
                encoding: b64
                content: {script}
            runcmd:
              - [/bin/bash, /usr/local/sbin/personabot-provision]
            power_state:
              mode: reboot
              delay: now
              timeout: 120
              condition: test -f {GuestNames.ProvisionedMarker}
            """.Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
        // Unix line endings whatever git checked this file out with (core.autocrlf gives CRLF on Windows).
    }
}

/// <summary>
/// Installs a Debian or Ubuntu machine from its cloud image (DISTROS.md phase 2): downloads the image,
/// checks it against the distribution's published checksums, copies it to the machine's own disk at the
/// chosen size, and writes the cloud-init seed that sets up the desktop on first boot.
/// </summary>
public static class CloudInstaller
{
    public const string DiskName = "desktop.qcow2";
    public const string ReadyName = "image-ready.txt";

    /// <summary>Written in the machine's folder when its first start has finished provisioning it.</summary>
    public const string ProvisionedName = "provisioned.txt";

    public static bool IsInstalled(Machine machine)
    {
        ArgumentNullException.ThrowIfNull(machine);
        return File.Exists(Path.Combine(machine.Folder, DiskName)) && File.Exists(Path.Combine(machine.Folder, ReadyName));
    }

    public static async Task PrepareAsync(Machine machine, Settings settings, Action<string> status, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(machine);
        ArgumentNullException.ThrowIfNull(settings);
        ArgumentNullException.ThrowIfNull(status);
        var image = CloudImages.For(machine) ?? throw new InvalidOperationException($"{machine.Name} has no cloud image to install from.");
        Trace.WriteLine($"CloudInstaller.PrepareAsync enter: {machine.Id} from {image.FileName}");
        Directory.CreateDirectory(machine.Folder);
        var disk = Path.Combine(machine.Folder, DiskName);
        var watch = Stopwatch.StartNew();
        if (!IsInstalled(machine))
        {
            using var client = new HttpClient { Timeout = TimeSpan.FromMinutes(60) };
            status($"Downloading {image.ChecksumFile} for {machine.Name}…");
            Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} not installed; fetching {image.BaseUrl}{image.ChecksumFile}");
            var checksum = CloudImages.ReadChecksum(await client.GetStringAsync(image.BaseUrl + image.ChecksumFile, token), image.FileName);
            var source = Path.Combine(machine.Folder, image.FileName);
            if (!File.Exists(source) || !await MatchesAsync(source, checksum, image.Hash, token))
            {
                Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} downloading {image.FileName} (existing copy: {File.Exists(source)})");
                await DownloadAsync(client, image.BaseUrl + image.FileName, source, machine.Name, status, token);
                if (!await MatchesAsync(source, checksum, image.Hash, token))
                {
                    Trace.TraceError($"CloudInstaller.PrepareAsync: {machine.Id} {image.FileName} failed its {image.Hash.Name} check; deleting it");
                    File.Delete(source);
                    throw new IOException($"The {machine.Name} download didn't match its published {image.Hash.Name} checksum, so it was deleted. Try again.");
                }
            }
            else
            {
                Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} reusing the verified {image.FileName}");
            }
            status($"Creating {machine.Name}'s disk ({settings.DiskGb} GB)…");
            Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} creating {DiskName} at {settings.DiskGb} GB");
            var qemuImg = Path.Combine(Path.GetDirectoryName(settings.QemuPath)!, OperatingSystem.IsWindows() ? "qemu-img.exe" : "qemu-img");
            var partial = disk + ".partial";
            if (File.Exists(partial)) File.Delete(partial);
            await VmImage.RunAsync(qemuImg, machine.Folder, token, "convert", "-f", "qcow2", "-O", "qcow2", source, partial);
            await VmImage.RunAsync(qemuImg, machine.Folder, token, "resize", partial, settings.DiskGb + "G");
            File.Move(partial, disk, overwrite: true);
            // The verified image is only a source; the machine's disk is the copy.
            File.Delete(source);
            await File.WriteAllTextAsync(Path.Combine(machine.Folder, ReadyName), $"{image.FileName} {image.Hash.Name} {checksum}\n", token);
            Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} disk ready after {watch.Elapsed.TotalSeconds:0}s");
        }
        else
        {
            Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} disk already installed");
            await GrowDiskAsync(machine, settings, status, token);
        }
        if (machine.Architecture == "aarch64")
        {
            var variables = Path.Combine(machine.Folder, "uefi-vars.fd");
            if (!File.Exists(variables))
            {
                Trace.WriteLine($"CloudInstaller.PrepareAsync: {machine.Id} copying fresh UEFI variables");
                File.Copy(Path.Combine(CloudRuntime.FirmwareDirectory(settings.QemuPath), "edk2-aarch64-vars.fd"), variables + ".partial", true);
                File.Move(variables + ".partial", variables, true);
            }
        }
        WriteSeed(machine);
        status(File.Exists(Path.Combine(machine.Folder, ProvisionedName))
            ? $"{machine.Name} is ready to start."
            : $"{machine.Name} is ready to start. Its first boot installs the desktop and Firefox.");
        Trace.WriteLine($"CloudInstaller.PrepareAsync exit: {machine.Id} ready after {watch.Elapsed.TotalSeconds:0}s");
    }

    private const long GiB = 1024L * 1024 * 1024;

    /// <summary>
    /// Grows an existing machine's disk to the Disk size setting when that's bigger (never smaller: shrinking would
    /// cut off data). Runs before QEMU starts, so the disk isn't in use; the guest's start-up repair then grows its
    /// partition and filesystem into the new space (CloudGuestRepair.DiskGrownMarker). A disk that can't be grown
    /// (a QEMU still holding it, a snapshot inside it) is left as it is and the machine starts anyway.
    /// </summary>
    public static async Task GrowDiskAsync(Machine machine, Settings settings, Action<string> status, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(machine);
        ArgumentNullException.ThrowIfNull(settings);
        ArgumentNullException.ThrowIfNull(status);
        var disk = Path.Combine(machine.Folder, DiskName);
        if (!File.Exists(disk)) return;
        var wanted = settings.DiskGb * GiB;
        var qemuImg = Path.Combine(Path.GetDirectoryName(settings.QemuPath)!, OperatingSystem.IsWindows() ? "qemu-img.exe" : "qemu-img");
        Trace.WriteLine($"CloudInstaller.GrowDiskAsync enter: {machine.Id}, wanted {settings.DiskGb} GB");
        try
        {
            var current = VirtualSize(await VmImage.RunAsync(qemuImg, machine.Folder, token, "info", "--output=json", DiskName));
            if (current >= wanted)
            {
                Trace.WriteLine($"CloudInstaller.GrowDiskAsync exit: {machine.Id} is {current / (double)GiB:0.#} GB, at least the {settings.DiskGb} GB wanted; nothing to do");
                return;
            }
            status($"Growing {machine.Name}'s disk from {current / (double)GiB:0.#} GB to {settings.DiskGb} GB…");
            await VmImage.RunAsync(qemuImg, machine.Folder, token, "resize", DiskName, settings.DiskGb + "G");
            var grown = VirtualSize(await VmImage.RunAsync(qemuImg, machine.Folder, token, "info", "--output=json", DiskName));
            MachineDiary.Write(machine.Folder, machine.Id, "disk", $"disk grown from {current / (double)GiB:0.#} GB to {grown / (double)GiB:0.#} GB; Linux grows into it at this start");
            Trace.WriteLine($"CloudInstaller.GrowDiskAsync exit: {machine.Id} grown {current} -> {grown} bytes");
        }
        catch (Exception ex) when (ex is IOException or System.Text.Json.JsonException or System.ComponentModel.Win32Exception)
        {
            // Not worth refusing to start over: it runs at its old size, and the next start tries again.
            Trace.TraceWarning($"CloudInstaller.GrowDiskAsync: {machine.Id} couldn't be grown to {settings.DiskGb} GB: {ex.Message.Trim()}");
            MachineDiary.Write(machine.Folder, machine.Id, "disk", $"couldn't grow the disk to {settings.DiskGb} GB: {ex.Message.Trim()}");
            status($"{machine.Name}'s disk couldn't be grown to {settings.DiskGb} GB ({ex.Message.Trim()}); starting at its current size.");
        }
    }

    /// <summary>The disk's size as the guest sees it ("virtual-size"), from qemu-img info's JSON.</summary>
    public static long VirtualSize(string infoJson)
    {
        ArgumentNullException.ThrowIfNull(infoJson);
        using var document = System.Text.Json.JsonDocument.Parse(infoJson);
        return document.RootElement.GetProperty("virtual-size").GetInt64();
    }

    /// <summary>
    /// The cloud-init NoCloud seed: an ISO 9660 volume labelled "cidata" with meta-data and user-data. Its
    /// instance-id carries the configuration's revision, so a machine not yet provisioned picks up a changed
    /// configuration. A provisioned machine keeps the seed it was provisioned with: a new instance-id makes cloud-init
    /// treat it as a brand-new machine at its next start (new SSH host keys, its first-boot jobs again), and on
    /// 16 Nov 2026 that first-boot run left an existing Ubuntu machine in emergency mode, its /boot partition lost
    /// mid-boot. Provisioned machines are kept current by the start-up repair (CloudGuestRepair.EnsureScript) instead.
    /// </summary>
    public static void WriteSeed(Machine machine)
    {
        ArgumentNullException.ThrowIfNull(machine);
        var config = CloudImages.BuildCloudConfig(machine.Distribution, machine.DesktopUser);
        var revision = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(config))).ToLowerInvariant()[..16];
        var seed = Path.Combine(machine.Folder, "seed.iso");
        var revisionFile = Path.Combine(machine.Folder, "seed.revision");
        var provisioned = File.Exists(Path.Combine(machine.Folder, ProvisionedName));
        var recorded = File.Exists(revisionFile) ? File.ReadAllText(revisionFile).Trim() : null;
        if (provisioned && File.Exists(seed))
        {
            Trace.WriteLine($"CloudInstaller.WriteSeed: {machine.Id} is provisioned; keeping its seed (revision {recorded ?? "unrecorded"}{(recorded == revision ? "" : $", the current configuration is {revision}")})");
            return;
        }
        if (File.Exists(seed) && recorded == revision)
        {
            Trace.WriteLine($"CloudInstaller.WriteSeed: {machine.Id} seed is current (revision {revision})");
            return;
        }
        // A provisioned machine whose seed went missing gets it back under the identity it was provisioned with.
        if (provisioned && recorded is { Length: > 0 })
        {
            Trace.WriteLine($"CloudInstaller.WriteSeed: {machine.Id} is provisioned but its seed is missing; rebuilding it as revision {recorded}");
            revision = recorded;
        }
        Trace.WriteLine($"CloudInstaller.WriteSeed enter: {machine.Id} revision {revision}");
        var builder = new CDBuilder { UseJoliet = true, VolumeIdentifier = "cidata" };
        builder.AddFile("meta-data", Encoding.UTF8.GetBytes($"instance-id: personabot-{machine.Id}-{revision}\nlocal-hostname: personabot\n"));
        builder.AddFile("user-data", Encoding.UTF8.GetBytes(config));
        var partial = seed + ".partial";
        builder.Build(partial);
        File.Move(partial, seed, true);
        File.WriteAllText(revisionFile, revision + "\n");
        Trace.WriteLine($"CloudInstaller.WriteSeed exit: {machine.Id} wrote {seed} ({new FileInfo(seed).Length} bytes)");
    }

    private static async Task DownloadAsync(HttpClient client, string url, string destination, string name, Action<string> status, CancellationToken token)
    {
        var partial = destination + ".partial";
        Trace.WriteLine($"CloudInstaller.DownloadAsync enter: {url} -> {destination}");
        var watch = Stopwatch.StartNew();
        using var response = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token);
        Trace.WriteLine($"CloudInstaller.DownloadAsync: HTTP {(int)response.StatusCode}, length {response.Content.Headers.ContentLength?.ToString() ?? "unknown"}");
        response.EnsureSuccessStatusCode();
        var total = response.Content.Headers.ContentLength;
        long received = 0;
        await using (var from = await response.Content.ReadAsStreamAsync(token))
        await using (var to = File.Create(partial))
        {
            var buffer = new byte[1 << 20];
            long done = 0;
            var lastReport = Stopwatch.StartNew();
            int read;
            while ((read = await from.ReadAsync(buffer, token)) > 0)
            {
                await to.WriteAsync(buffer.AsMemory(0, read), token);
                done += read;
                if (lastReport.ElapsedMilliseconds > 500)
                {
                    status(total is > 0 ? $"Downloading {name}: {done / 1048576} of {total.Value / 1048576} MB ({100 * done / total.Value}%)…" : $"Downloading {name}: {done / 1048576} MB…");
                    lastReport.Restart();
                }
            }
            received = done;
        }
        File.Move(partial, destination, true);
        Trace.WriteLine($"CloudInstaller.DownloadAsync exit: {received} bytes in {watch.Elapsed.TotalSeconds:0}s");
    }

    private static async Task<bool> MatchesAsync(string path, string expected, HashAlgorithmName hash, CancellationToken token)
    {
        await using var stream = File.OpenRead(path);
        var actual = hash == HashAlgorithmName.SHA512 ? await SHA512.HashDataAsync(stream, token) : await SHA256.HashDataAsync(stream, token);
        var matches = Convert.ToHexString(actual).Equals(expected, StringComparison.OrdinalIgnoreCase);
        Trace.WriteLine($"CloudInstaller.MatchesAsync: {Path.GetFileName(path)} ({stream.Length} bytes) {hash.Name} matches={matches}");
        return matches;
    }
}

/// <summary>
/// A Debian or Ubuntu machine in QEMU (DISTROS.md §4), on x86-64 or ARM64, from the Mac's Debian runtime
/// made general: virtio disk, GPU and network, a root shell on the virtio console, QMP for the screen and
/// input. x86-64 uses q35 with SeaBIOS; ARM64 uses the virt machine with UEFI. Both run in software
/// emulation (TCG) on Windows until WHPX is supported.
/// </summary>
public sealed class CloudRuntime : IGuestRuntime, IGuestDisplayStream, IQmpRuntime, IBotDisplayHost
{
    private readonly Machine _machine;
    private readonly Action<string> _progress;
    private readonly SemaphoreSlim _captureGate = new(1);
    private Process? _process;
    private Task? _errorPump;
    private bool _disposed;
    private bool _disableSharedGpu;
    private QemuFramebuffer? _framebuffer;
    internal bool RequireSharedGpu { get; set; }
    internal bool RequireHardwareCpu { get; set; }
    internal Func<IGuestShell, CancellationToken, Task<bool>>? PrepareDesktopAccountAsync { get; init; }
    internal event Action<byte[]>? AudioReceived;

    /// <param name="accelerator">QemuAcceleration.Whpx or .Tcg; WHPX falls back to TCG when QEMU can't start with it.</param>
    public CloudRuntime(Machine machine, Action<string>? progress = null, string accelerator = QemuAcceleration.Tcg)
    {
        _machine = machine ?? throw new ArgumentNullException(nameof(machine));
        ArgumentException.ThrowIfNullOrWhiteSpace(accelerator);
        _progress = progress ?? (_ => { });
        Accelerator = accelerator;
        ScreenBounds = new ScreenRect(0, 0, machine.ScreenWidth, machine.ScreenHeight);
        Shell = new ConsoleShell(Transport);
        Trace.WriteLine($"CloudRuntime: created for {machine.Id} ({machine.Distribution}, {machine.Architecture}), {machine.ScreenWidth}x{machine.ScreenHeight}, {accelerator}");
    }

    private int _displayPort;
    private IReadOnlyList<int> _botDisplayPorts = [];
    private readonly Dictionary<int, BotDisplayScreen> _botDisplays = [];

    /// <summary>
    /// Starts a bot's own display (XMEETING.md §3) and returns it as a screen; the same screen if it is
    /// already running. <see cref="BotDisplays.InstallScript"/> must have finished on this machine first.
    /// </summary>
    public async Task<BotDisplayScreen> StartBotDisplayAsync(GuestAccount account, int number, GuestResolution size, string desktop, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(account);
        ArgumentNullException.ThrowIfNull(size);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_botDisplayPorts.Count == 0)
        {
            Trace.TraceWarning($"CloudRuntime.StartBotDisplayAsync: {_machine.Id} hasn't started; no bot display ports");
            throw new InvalidOperationException("Start the machine before a bot's display.");
        }
        if (number < 1 || number > _botDisplayPorts.Count)
        {
            Trace.TraceWarning($"CloudRuntime.StartBotDisplayAsync: display :{number} is out of range 1-{_botDisplayPorts.Count}");
            throw new ArgumentOutOfRangeException(nameof(number), $"Bot displays are numbered 1 to {_botDisplayPorts.Count}.");
        }
        lock (_botDisplays)
            if (_botDisplays.TryGetValue(number, out var running))
            {
                Trace.WriteLine($"CloudRuntime.StartBotDisplayAsync: :{number} is already running for {running.Account.User}");
                return running;
            }
        Trace.WriteLine($"CloudRuntime.StartBotDisplayAsync enter: :{number} for {account.User}, {size}, {desktop}");
        await Shell.ExecuteAsync(BotDisplays.LauncherScript, token);
        var output = await Shell.ExecuteAsync(BotDisplays.StartScript(account, number, size, desktop), token);
        if (!output.Contains(BotDisplays.ReadyMarker, StringComparison.Ordinal) || output.Contains(BotDisplays.FailedMarker, StringComparison.Ordinal))
        {
            Trace.TraceError($"CloudRuntime.StartBotDisplayAsync: :{number} for {account.User} didn't start: {output.Trim()}");
            throw new IOException($"{account.DisplayName}'s display didn't start: {output.Trim()}");
        }
        var screen = new BotDisplayScreen(account, number, _botDisplayPorts[number - 1], size);
        // If its server stops (a bot restarted it, or it crashed), the next connection starts it again on the same
        // display, so the screen the app and the tools hold keeps working.
        screen.Revive = async reviveToken =>
        {
            Trace.WriteLine($"CloudRuntime: starting {account.User}'s display :{number} again");
            var again = await Shell.ExecuteAsync(BotDisplays.StartScript(account, number, size, desktop), reviveToken);
            if (!again.Contains(BotDisplays.ReadyMarker, StringComparison.Ordinal) || again.Contains(BotDisplays.FailedMarker, StringComparison.Ordinal))
            {
                Trace.TraceError($"CloudRuntime: {account.User}'s display :{number} couldn't be started again: {again.Trim()}");
                throw new IOException($"{account.DisplayName}'s display couldn't be started again: {again.Trim()}");
            }
            Trace.WriteLine($"CloudRuntime: {account.User}'s display :{number} is running again");
        };
        lock (_botDisplays) _botDisplays[number] = screen;
        Trace.WriteLine($"CloudRuntime.StartBotDisplayAsync exit: :{number} on loopback port {_botDisplayPorts[number - 1]}");
        return screen;
    }

    private readonly SemaphoreSlim _botDisplayGate = new(1);
    private bool _botDisplaysInstalled;

    /// <summary>The desktop a bot's display runs (Paul, 15 Nov 2026: GNOME everywhere).</summary>
    public const string BotDesktop = "gnome";

    /// <summary>
    /// A bot's own display, at the machine's screen size: installs the display server and desktop the first
    /// time (minutes on a slow link), makes sure the bot's Linux user exists, and gives the bot the lowest
    /// free display number. One at a time, so two bots never race for a number or the installer.
    /// </summary>
    public async Task<IBotScreen> OpenBotDisplayAsync(GuestAccount account, Action<string> status, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(account);
        ArgumentNullException.ThrowIfNull(status);
        Trace.WriteLine($"CloudRuntime.OpenBotDisplayAsync enter: {_machine.Id} for {account.User}, installed={_botDisplaysInstalled}");
        await _botDisplayGate.WaitAsync(token);
        try
        {
            lock (_botDisplays)
                if (_botDisplays.Values.FirstOrDefault(d => d.Account.User == account.User) is { } open)
                {
                    Trace.WriteLine($"CloudRuntime.OpenBotDisplayAsync exit: {account.User} already has :{open.Number}");
                    return open;
                }
            if (!_botDisplaysInstalled)
            {
                status($"Getting {account.DisplayName}'s own screen ready…");
                Trace.WriteLine($"CloudRuntime.OpenBotDisplayAsync: {_machine.Id} installing the bot display packages ({BotDesktop})");
                await Shell.ExecuteAsync(BotDisplays.InstallScript(BotDesktop), token);
                var watch = Stopwatch.StartNew();
                while (true)
                {
                    var progress = await Shell.ExecuteAsync(BotDisplays.InstallStatusScript, token);
                    if (progress.Contains(BotDisplays.FailedMarker, StringComparison.Ordinal))
                    {
                        Trace.TraceError($"CloudRuntime.OpenBotDisplayAsync: {_machine.Id} package install failed after {watch.Elapsed.TotalSeconds:0}s: {progress.Trim()}");
                        throw new IOException($"The packages for bots' own screens didn't install on {_machine.Name}. See {BotDisplays.LogFolder}/install.log in the machine.");
                    }
                    if (progress.Contains(BotDisplays.InstalledMarker, StringComparison.Ordinal))
                    {
                        Trace.WriteLine($"CloudRuntime.OpenBotDisplayAsync: {_machine.Id} packages installed after {watch.Elapsed.TotalSeconds:0}s");
                        break;
                    }
                    if (watch.Elapsed > TimeSpan.FromHours(1))
                    {
                        Trace.TraceError($"CloudRuntime.OpenBotDisplayAsync: {_machine.Id} package install still running after an hour: {progress.Trim()}");
                        throw new IOException("Installing the packages for bots' own screens took over an hour.");
                    }
                    status($"Installing the desktop for bots' own screens (first time on this machine, {watch.Elapsed:m\\:ss})…");
                    await Task.Delay(3000, token);
                }
                _botDisplaysInstalled = true;
            }
            // The account's first command creates its Linux user.
            await new AccountShell(Shell, account).ExecuteAsync("true", token);
            int number;
            lock (_botDisplays) number = Enumerable.Range(1, _botDisplayPorts.Count).FirstOrDefault(n => !_botDisplays.ContainsKey(n));
            if (number == 0)
            {
                Trace.TraceWarning($"CloudRuntime.OpenBotDisplayAsync: all {_botDisplayPorts.Count} bot displays on {_machine.Id} are in use");
                throw new InvalidOperationException($"All {_botDisplayPorts.Count} bot screens on {_machine.Name} are in use.");
            }
            status($"Starting {account.DisplayName}'s own screen…");
            Trace.WriteLine($"CloudRuntime.OpenBotDisplayAsync exit: giving {account.User} display :{number}");
            return await StartBotDisplayAsync(account, number, _machine.Resolution, BotDesktop, token);
        }
        finally { _botDisplayGate.Release(); }
    }

    /// <summary>Stops a bot's display and closes the app's connections to it.</summary>
    public async Task StopBotDisplayAsync(int number, CancellationToken token)
    {
        BotDisplayScreen? screen;
        lock (_botDisplays)
        {
            if (!_botDisplays.Remove(number, out screen))
            {
                Trace.WriteLine($"CloudRuntime.StopBotDisplayAsync: :{number} isn't running on {_machine.Id}");
                return;
            }
        }
        Trace.WriteLine($"CloudRuntime.StopBotDisplayAsync enter: :{number} for {screen.Account.User} on {_machine.Id}");
        await screen.DisposeAsync();
        var stopped = await Shell.ExecuteAsync(BotDisplays.StopScript(screen.Account, number), token);
        Trace.WriteLine($"CloudRuntime.StopBotDisplayAsync exit: :{number} stopped: {stopped.Trim()}");
    }

    public string MachineId => _machine.Id;

    /// <summary>The accelerator QEMU is running with: "whpx" or "tcg".</summary>
    public string Accelerator { get; private set; }

    /// <summary>Why WHPX was given up for this start, when it was; null otherwise.</summary>
    public string? AcceleratorFallback { get; private set; }

    public bool SharedGpuEnabled { get; private set; }
    public bool SharedGpuVerified { get; private set; }
    public string? GraphicsFallback { get; private set; }
    public string? GraphicsRenderer { get; private set; }
    public string GraphicsSummary => SharedGpuVerified ? "Shared GPU · VirGL"
        : SharedGpuEnabled ? "VirGL · GPU unverified" : "Basic graphics";
    public string GraphicsStatus => GraphicsSummary + (GraphicsFallback is { } reason ? ": " + reason : "") + (SoundStatus is { } sound ? " · " + sound : "");

    /// <summary>X draws the cursor into the screen (CloudGuestRepair's software cursor), so the view shows Linux's own pointer.</summary>
    public bool DrawsOwnCursor => true;

    /// <summary>Whether this start gave the guest a sound card (Settings.PlaySound).</summary>
    public bool SoundEnabled { get; private set; }

    /// <summary>"Sound on", "Sound off", or why sound is unavailable; null until checked.</summary>
    public string? SoundStatus { get; private set; }

    public string Description => $"{_machine.Name} · QEMU/{(Accelerator == QemuAcceleration.Whpx ? "WHPX" : "TCG")}";
    public bool Restored => false;
    public ScreenRect ScreenBounds { get; private set; }
    public QmpClient Qmp { get; private set; } = new();
    public GuestShell Transport { get; private set; } = new();
    public IGuestShell Shell { get; private set; }
    public ITextRecognizer? TextRecognizer => null;
    public bool SupportsCheckpoints => false;
    public int CpuCount { get; private set; }
    public int MemoryMb { get; private set; }
    public event Action? DisplayReady;
    public event Action<string>? Stopped;

    /// <summary>
    /// Why a machine's QEMU ended without being asked to, from the end of its serial log (the guest's own last
    /// words) and QEMU's log: words for the person.
    /// </summary>
    public static string WhyStopped(string bootLogTail, string? qemuLastLine, int? exitCode)
    {
        ArgumentNullException.ThrowIfNull(bootLogTail);
        if (bootLogTail.Contains("Kernel panic", StringComparison.Ordinal))
            return "its Linux kernel crashed (a kernel panic)";
        if (GuestRestarted(bootLogTail))
            return "it restarted itself, which ends QEMU under the Windows hypervisor";
        if (bootLogTail.Contains("reboot: Power down", StringComparison.Ordinal))
            return "it was shut down from inside";
        if (!string.IsNullOrWhiteSpace(qemuLastLine)) return "QEMU stopped: " + qemuLastLine.Trim();
        return exitCode is { } code ? $"QEMU stopped with code {code}" : "QEMU stopped";
    }

    private CancellationTokenSource? _consoleFollow;
    private CancellationTokenSource? _events;
    private HostWatch? _hostWatch;

    /// <summary>Copies the last run's guest-boot.log to guest-boot.previous.log before a new start empties it.</summary>
    private void KeepPreviousBootLog(string bootLog)
    {
        try
        {
            var info = new FileInfo(bootLog);
            if (!info.Exists || info.Length == 0) return;
            using var source = new FileStream(bootLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var target = File.Create(Path.Combine(_machine.Folder, "guest-boot.previous.log"));
            source.CopyTo(target);
            Trace.WriteLine($"CloudRuntime.KeepPreviousBootLog: {_machine.Id} kept {info.Length} bytes as guest-boot.previous.log");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Trace.WriteLine($"CloudRuntime.KeepPreviousBootLog: {_machine.Id} couldn't keep the last console: {ex.Message}");
        }
    }

    /// <summary>
    /// After a crash, the next start reads the guest's own journal of the boot that died (its kernel messages, every
    /// warning, the crashed programs) into that crash's folder: the journal sees far more than the console did.
    /// </summary>
    private async Task CollectPostMortemAsync(CancellationToken token)
    {
        if (CrashReport.PendingPostMortem(_machine.Folder) is not { } crashFolder) return;
        Trace.WriteLine($"CloudRuntime.CollectPostMortemAsync enter: {_machine.Id} for {crashFolder}");
        try
        {
            var journal = await Transport.ExecuteAsync(CloudGuestRepair.PostMortemScript, token);
            CrashReport.CompletePostMortem(_machine.Folder, crashFolder, journal);
            MachineDiary.Write(_machine.Folder, _machine.Id, "crash", $"the guest's journal of the boot that died saved in {crashFolder} ({journal.Length} characters)");
            Trace.WriteLine($"CloudRuntime.CollectPostMortemAsync exit: {journal.Length} characters");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException || ex is OperationCanceledException && !token.IsCancellationRequested)
        {
            // Not worth failing a start over: the note stays and the next start tries again.
            Trace.TraceWarning($"CloudRuntime.CollectPostMortemAsync: {_machine.Id} couldn't collect the guest's journal: {ex.GetType().Name}: {ex.Message}");
        }
    }

    /// <summary>
    /// Sends the guest's serial console (guest-boot.log, which QEMU writes) to the log line by line as it grows, while
    /// the machine runs, when machine logs are on: its kernel, systemd and desktop messages, up to its last words.
    /// </summary>
    private void FollowConsole(string bootLog)
    {
        _consoleFollow?.Cancel();
        var stop = _consoleFollow = new CancellationTokenSource();
        var machineId = _machine.Id;
        Trace.WriteLine($"CloudRuntime.FollowConsole: following {bootLog} for {machineId}");
        _ = Task.Run(async () =>
        {
            long position = 0;
            var partial = new StringBuilder();
            var buffer = new byte[16384];
            try
            {
                while (!stop.IsCancellationRequested)
                {
                    await Task.Delay(500, stop.Token);
                    if (!PersonaBotLogging.MachineLogsOn) continue;
                    try
                    {
                        await using var stream = new FileStream(bootLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                        // The log starts again with each QEMU (a guest restart): follow it from the top.
                        if (stream.Length < position) { position = 0; partial.Clear(); }
                        stream.Position = position;
                        int read;
                        while ((read = await stream.ReadAsync(buffer, stop.Token)) > 0)
                        {
                            position += read;
                            partial.Append(Encoding.UTF8.GetString(buffer, 0, read));
                            var text = partial.ToString();
                            var end = text.LastIndexOf('\n');
                            if (end < 0) continue;
                            foreach (var line in text[..end].Split('\n')) PersonaBotLogging.Machine(machineId, "guest", line);
                            partial.Clear().Append(text[(end + 1)..]);
                        }
                    }
                    catch (IOException) { }
                }
            }
            catch (OperationCanceledException) { }
            // Whatever the guest wrote last, without a line end (its final words before a crash).
            if (partial.Length > 0) PersonaBotLogging.Machine(machineId, "guest", partial.ToString());
            Trace.WriteLine($"CloudRuntime.FollowConsole: stopped following {machineId}'s console at byte {position}");
        });
    }

    /// <summary>Raises <see cref="Stopped"/> if this QEMU ends while the machine is still meant to be running.</summary>
    private void WatchForUnexpectedStop(Process process)
    {
        Trace.WriteLine($"CloudRuntime.WatchForUnexpectedStop: watching {_machine.Id}'s QEMU");
        _ = Task.Run(async () =>
        {
            try { await process.WaitForExitAsync(); }
            catch (Exception ex) when (ex is InvalidOperationException or ObjectDisposedException)
            {
                Trace.WriteLine($"CloudRuntime.WatchForUnexpectedStop: {_machine.Id} stopped watching: {ex.GetType().Name}");
                return;
            }
            // A stop the app asked for, or a QEMU replaced since (a graphics fallback), is not news.
            if (_disposed || !ReferenceEquals(process, _process))
            {
                Trace.WriteLine($"CloudRuntime.WatchForUnexpectedStop: {_machine.Id}'s QEMU ended as expected (disposed={_disposed})");
                return;
            }
            int? code = null;
            try { code = process.ExitCode; } catch (InvalidOperationException) { }
            var stoppedAt = DateTimeOffset.Now;
            var samples = _hostWatch?.Recent ?? [];
            _hostWatch?.Dispose();
            var bootLog = ReadShared(Path.Combine(_machine.Folder, "guest-boot.log"));
            var tail = bootLog.Length > 20000 ? bootLog[^20000..] : bootLog;
            var why = WhyStopped(tail, QemuLog.LastLine(Path.Combine(_machine.Folder, "qemu.log")), code);
            // The console's last lines reach the log before the reason does.
            await Task.Delay(1200);
            _consoleFollow?.Cancel();
            // Everything that might say why, saved before a restart can overwrite it.
            try { CrashReport.Write(_machine.Folder, _machine.Id, why, code, samples, stoppedAt); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Trace.TraceWarning($"CloudRuntime.WatchForUnexpectedStop: {_machine.Id} crash report failed: {ex.Message}");
            }
            Trace.TraceError($"CloudRuntime: {_machine.Id} stopped unexpectedly: {why}");
            Stopped?.Invoke(why);
        });
    }

    /// <summary>Follows the size the guest's screen really came up at (see QemuRuntime.AdoptScreenSize).</summary>
    public void AdoptScreenSize(int width, int height)
    {
        if (width < 320 || height < 200)
        {
            Trace.TraceWarning($"CloudRuntime.AdoptScreenSize: {width}x{height} isn't usable");
            throw new ArgumentException($"{width} × {height} is not a usable screen size.");
        }
        Trace.WriteLine($"CloudRuntime.AdoptScreenSize: {_machine.Id} {ScreenBounds.Width}x{ScreenBounds.Height} -> {width}x{height}");
        ScreenBounds = new ScreenRect(0, 0, width, height);
    }

    public static string FirmwareDirectory(string qemuPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(qemuPath);
        foreach (var candidate in new[] { Path.Combine(Path.GetDirectoryName(qemuPath)!, "share"), Path.Combine(Path.GetDirectoryName(qemuPath)!, "..", "share", "qemu") })
        {
            if (File.Exists(Path.Combine(candidate, "edk2-aarch64-code.fd"))) return Path.GetFullPath(candidate);
        }
        Trace.TraceWarning($"CloudRuntime.FirmwareDirectory: no edk2-aarch64-code.fd beside {qemuPath}");
        throw new FileNotFoundException("QEMU's ARM64 UEFI firmware (edk2-aarch64-code.fd) wasn't found beside " + qemuPath);
    }

    /// <summary>The QEMU program for an architecture, beside the configured x86-64 one.</summary>
    public static string QemuFor(string qemuPath, string architecture)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(qemuPath);
        var folder = Path.GetDirectoryName(qemuPath)!;
        var name = (architecture == "aarch64" ? "qemu-system-aarch64" : "qemu-system-x86_64") + (OperatingSystem.IsWindows() ? ".exe" : "");
        return Path.Combine(folder, name);
    }

    public static IReadOnlyList<string> BuildArguments(Machine machine, int cpus, int memoryMb, string firmwareDirectory, int qmpPort, int shellPort, string accelerator = QemuAcceleration.Tcg, bool sharedGpu = false, int? framebufferPort = null, IReadOnlyList<string>? sound = null, IReadOnlyList<int>? botDisplayPorts = null, int? eventsPort = null, bool streamAudio = false)
    {
        ArgumentNullException.ThrowIfNull(machine);
        var acceleration = QemuAcceleration.Arguments(accelerator, machine.Architecture);
        if (qmpPort is < 1 or > 65535 || shellPort is < 1 or > 65535 || qmpPort == shellPort) throw new ArgumentOutOfRangeException(nameof(qmpPort), "Use two different valid loopback ports.");
        if (sharedGpu && framebufferPort == null || framebufferPort != null && (framebufferPort is < 5900 or > 65535 || framebufferPort == qmpPort || framebufferPort == shellPort))
            throw new ArgumentOutOfRangeException(nameof(framebufferPort), "Shared graphics needs a separate loopback framebuffer port (5900–65535).");
        var arm = machine.Architecture == "aarch64";
        var args = new List<string> { "-name", "PersonaBot " + machine.Name, "-nodefaults" };
        if (arm)
        {
            var code = Path.Combine(firmwareDirectory, "edk2-aarch64-code.fd").Replace(",", ",,", StringComparison.Ordinal);
            args.AddRange(["-machine", "virt"]);
            args.AddRange(acceleration);
            args.AddRange(["-drive", $"if=pflash,format=raw,readonly=on,file={code}", "-drive", "if=pflash,format=raw,file=uefi-vars.fd"]);
        }
        else
        {
            args.AddRange(["-machine", "q35"]);
            args.AddRange(acceleration);
        }
        args.AddRange(["-smp", cpus.ToString(System.Globalization.CultureInfo.InvariantCulture), "-m", memoryMb.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-drive", $"file={CloudInstaller.DiskName},format=qcow2,if=virtio",
            "-drive", "file=seed.iso,format=raw,if=virtio,readonly=on",
            .. QemuGraphics.Arguments(machine.ScreenWidth, machine.ScreenHeight, sharedGpu),
            "-device", "qemu-xhci", "-device", "usb-tablet", "-device", "usb-kbd",
            "-device", "virtio-rng-pci", "-nic", "user,model=virtio-net-pci" + (botDisplayPorts == null ? "" : BotDisplays.Forwards(botDisplayPorts)),
            "-qmp", $"tcp:127.0.0.1:{qmpPort},server=on,wait=off", "-monitor", "none",
            "-serial", "file:guest-boot.log", "-device", "virtio-serial-pci",
            "-chardev", $"socket,id=shell,host=127.0.0.1,port={shellPort},server=on,wait=off",
            "-device", "virtconsole,chardev=shell"]);
        // The guest tells QEMU when its kernel panics (QEMU's GUEST_PANICKED event), and QEMU leaves the rest as it was:
        // the kernel's own panic=-1 restart, which -no-reboot turns into QEMU ending.
        args.AddRange(["-device", machine.Architecture == "aarch64" ? "pvpanic-pci" : "pvpanic", "-action", "panic=none"]);
        if (framebufferPort is { } port)
            args.AddRange(["-vnc", $"127.0.0.1:{port - 5900}" + (streamAudio ? $",audiodev={CloudSound.AudioDevId}" : "")]);
        if (sound != null) args.AddRange(sound);
        // Under WHPX a guest reboot hangs QEMU ("WHPX: Unexpected VP exit code 4", qemu#2042, qemu#2402;
        // seen here 15 Nov 2026), so QEMU exits instead and PersonaBot starts it again.
        if (accelerator == QemuAcceleration.Whpx) args.Add("-no-reboot");
        // A second QMP connection that only listens, so QEMU's events (a panic, a reset, a shutdown) are logged as they
        // happen rather than only while a command waits for its answer.
        if (eventsPort is { } events)
        {
            if (events is < 1 or > 65535 || events == qmpPort || events == shellPort) throw new ArgumentOutOfRangeException(nameof(eventsPort), "Use a separate loopback port for QEMU's events.");
            args.AddRange(["-qmp", $"tcp:127.0.0.1:{events},server=on,wait=off"]);
        }
        Trace.WriteLine($"CloudRuntime.BuildArguments: {machine.Id} {machine.Architecture} {accelerator}, {cpus} CPUs, {memoryMb} MB, sharedGpu={sharedGpu}, qmp={qmpPort}, shell={shellPort}, framebuffer={framebufferPort?.ToString() ?? "none"}, events={eventsPort?.ToString() ?? "none"}, sound={sound != null}, botDisplays={botDisplayPorts?.Count ?? 0}, {args.Count} arguments");
        return args;
    }

    /// <summary>The Linux kernel's last line when the guest restarts (kernel/reboot.c), as opposed to "reboot: Power down".</summary>
    public const string GuestRestartMarker = "reboot: Restarting system";

    /// <summary>The most times one start relaunches QEMU for a guest restart, so a guest that keeps rebooting still fails.</summary>
    public const int MaxGuestRestarts = 3;

    /// <summary>Whether QEMU stopped because the guest restarted itself: under -no-reboot that ends QEMU, and PersonaBot starts it again.</summary>
    public static bool GuestRestarted(string bootLog)
    {
        ArgumentNullException.ThrowIfNull(bootLog);
        return bootLog.Contains(GuestRestartMarker, StringComparison.Ordinal);
    }

    /// <summary>How long a start waits for the last QEMU of this machine to let go of guest-boot.log.</summary>
    public static readonly TimeSpan BootLogWait = TimeSpan.FromSeconds(90);

    /// <summary>
    /// Empties guest-boot.log for a new start. Right after PersonaBot restarts, the last QEMU of this machine can
    /// still be shutting down with the log open (seen 15 Nov 2026: "The process cannot access the file …
    /// guest-boot.log because it is being used by another process"), so this waits for it to let go, saying so,
    /// and only fails, plainly, if it never does.
    /// </summary>
    public static async Task ClearBootLogAsync(string bootLog, string machineName, Action<string> progress, TimeSpan wait, CancellationToken token)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(bootLog);
        ArgumentException.ThrowIfNullOrWhiteSpace(machineName);
        ArgumentNullException.ThrowIfNull(progress);
        if (wait < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(wait));
        Trace.WriteLine($"CloudRuntime.ClearBootLogAsync enter: {bootLog}");
        var watch = Stopwatch.StartNew();
        var told = false;
        while (true)
        {
            try
            {
                await File.WriteAllTextAsync(bootLog, "", token);
                Trace.WriteLine($"CloudRuntime.ClearBootLogAsync exit: cleared after {watch.Elapsed.TotalSeconds:0.0}s");
                return;
            }
            catch (IOException ex) when (ex is not FileNotFoundException and not DirectoryNotFoundException)
            {
                if (watch.Elapsed >= wait)
                {
                    Trace.WriteLine($"CloudRuntime.ClearBootLogAsync: still held after {watch.Elapsed.TotalSeconds:0}s: {ex.Message}");
                    throw new IOException($"The last run of {machineName} is still closing (its QEMU still has guest-boot.log open after {wait.TotalSeconds:0} seconds). " +
                        "Wait for it to finish, or end qemu-system in Task Manager, then start the machine again.", ex);
                }
                if (!told)
                {
                    told = true;
                    Trace.WriteLine($"CloudRuntime.ClearBootLogAsync: held by another process, waiting: {ex.Message}");
                    progress($"Waiting for the last run of {machineName} to finish closing…");
                }
                await Task.Delay(500, token);
            }
        }
    }

    public async Task StartAsync(Settings settings, CancellationToken token, SessionState? resume = null)
    {
        ArgumentNullException.ThrowIfNull(settings);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (RequireHardwareCpu && Accelerator != QemuAcceleration.Whpx)
            throw new IOException("Ubuntu requires Windows Hypervisor Platform for hardware CPU acceleration.");
        if (_process != null)
        {
            Trace.TraceWarning($"CloudRuntime.StartAsync: {_machine.Id} was already started");
            throw new InvalidOperationException("This machine was already started.");
        }
        Trace.WriteLine($"CloudRuntime.StartAsync enter: {_machine.Id} ({_machine.Distribution}, {_machine.Architecture}), {Accelerator}, sharedGpu={settings.UseSharedGpu}, gpuDisabled={_disableSharedGpu}, sound={settings.PlaySound}");
        var qmpPort = Port();
        var shellPort = Port();
        while (shellPort == qmpPort) shellPort = Port();
        CpuCount = settings.CpuCount;
        // Desktop Linux with Firefox needs about 2 GB; QEMU on Windows reserves all of it up front.
        MemoryMb = Math.Max(settings.MemoryMb, 2048);
        var firmware = _machine.Architecture == "aarch64" ? FirmwareDirectory(settings.QemuPath) : "";
        if (settings.UseSharedGpu && !_disableSharedGpu)
        {
            _progress("Checking shared GPU support…");
            var support = await QemuGraphics.ProbeAsync(QemuFor(settings.QemuPath, _machine.Architecture), _machine.Architecture, token);
            SharedGpuEnabled = support.Available;
            GraphicsFallback = support.Reason;
            Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} shared GPU available={support.Available}{(support.Reason is { } why ? ": " + why : "")}");
            if (RequireSharedGpu && !support.Available) throw new IOException("Required GPU support is unavailable: " + support.Reason);
            if (!support.Available) _progress($"Shared GPU unavailable ({support.Reason}); using basic graphics.");
        }
        var graphicsSupported = SharedGpuEnabled;
        SoundEnabled = settings.PlaySound;
        var soundWav = CloudSound.WavPathFromEnvironment();
        var streamAudio = SoundEnabled && AudioReceived != null;
        var sound = SoundEnabled ? CloudSound.Arguments(_machine.Architecture, soundWav, streamAudio) : null;
        SoundStatus = SoundEnabled ? null : "Sound off";
        Trace.WriteLine($"CloudRuntime.StartAsync: sound {(SoundEnabled ? soundWav ?? "on" : "off")}");
        var framebufferPort = Port();
        while (framebufferPort < 5900 || framebufferPort == qmpPort || framebufferPort == shellPort) framebufferPort = Port();
        _displayPort = framebufferPort;
        // QEMU's events (QmpEventLog) come on a connection of their own.
        var eventsPort = Port();
        while (eventsPort == qmpPort || eventsPort == shellPort || eventsPort == framebufferPort) eventsPort = Port();
        // A loopback port for each bot display (XMEETING.md §3); QEMU forwards them from the start.
        var taken = new HashSet<int> { qmpPort, shellPort, framebufferPort };
        var botPorts = new List<int>();
        while (botPorts.Count < BotDisplays.Count)
        {
            var port = Port();
            if (taken.Add(port)) botPorts.Add(port);
        }
        _botDisplayPorts = botPorts;
        Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} ports qmp={qmpPort}, shell={shellPort}, framebuffer={framebufferPort}, events={eventsPort}, bot displays {string.Join(',', botPorts)}; {CpuCount} CPUs, {MemoryMb} MB");
        ProcessStartInfo Launch()
        {
            var info = new ProcessStartInfo(QemuFor(settings.QemuPath, _machine.Architecture)) { WorkingDirectory = _machine.Folder, UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true };
            QemuGraphics.ConfigureEnvironment(info, SharedGpuEnabled);
            foreach (var arg in BuildArguments(_machine, CpuCount, MemoryMb, firmware, qmpPort, shellPort, Accelerator, SharedGpuEnabled, framebufferPort, sound,
                settings.BotsUseOwnScreens ? _botDisplayPorts : null, eventsPort, streamAudio)) info.ArgumentList.Add(arg);
            Trace.WriteLine($"CloudRuntime.StartAsync: launching with {Accelerator}");
            return info;
        }
        var bootLog = Path.Combine(_machine.Folder, "guest-boot.log");
        // The last run's console is kept (a crash's last words survive "Start it again"), then the log starts afresh.
        KeepPreviousBootLog(bootLog);
        await ClearBootLogAsync(bootLog, _machine.Name, _progress, BootLogWait, token);
        MachineDiary.Write(_machine.Folder, _machine.Id, "start", $"starting: {Accelerator}, {CpuCount} CPUs, {MemoryMb} MB, shared GPU {(SharedGpuEnabled ? "on" : "off")}, app pid {Environment.ProcessId}");
        FollowConsole(bootLog);
        var firstBoot = !File.Exists(Path.Combine(_machine.Folder, CloudInstaller.ProvisionedName));
        Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} firstBoot={firstBoot}");
        try
        {
            while (true)
            {
                _process = Process.Start(Launch()) ?? throw new IOException("QEMU could not start.");
                Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} QEMU pid {_process.Id} started");
                _errorPump = RecordErrorsAsync(_process);
                await Task.Delay(1000, token);
                if (!_process.HasExited) break;
                if (_errorPump != null) await _errorPump;
                var reason = QemuLog.LastLine(Path.Combine(_machine.Folder, "qemu.log")) is { Length: > 0 } why ? why : $"QEMU stopped with code {_process.ExitCode}";
                Trace.TraceWarning($"CloudRuntime.StartAsync: {_machine.Id} QEMU exited at once (code {_process.ExitCode}, {Accelerator}, sharedGpu={SharedGpuEnabled}): {reason}");
                // Retry graphics independently: an EGL failure must not mark WHPX unavailable.
                if (SharedGpuEnabled)
                {
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} retrying with basic graphics (required GPU={RequireSharedGpu})");
                    if (RequireSharedGpu) throw new IOException("Required GPU launch failed: " + reason);
                    GraphicsFallback = reason;
                    SharedGpuEnabled = false;
                    _progress($"Shared GPU couldn't start ({reason}); retrying with basic graphics.");
                }
                else if (Accelerator == QemuAcceleration.Whpx)
                {
                    if (RequireHardwareCpu) throw new IOException("Required hardware CPU acceleration failed: " + reason);
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} falling back from WHPX to TCG; shared GPU again={graphicsSupported}");
                    AcceleratorFallback = reason;
                    _progress($"The {WindowsHypervisor.Name} didn't work for {_machine.Name} ({reason}); using software emulation.");
                    Accelerator = QemuAcceleration.Tcg;
                    // The first failure may have been WHPX rather than graphics. Try the proven GPU
                    // backend again with TCG before deciding that this machine needs basic graphics.
                    SharedGpuEnabled = graphicsSupported;
                    if (SharedGpuEnabled) GraphicsFallback = null;
                }
                else
                {
                    Trace.TraceError($"CloudRuntime.StartAsync: {_machine.Id} QEMU failed with basic graphics under TCG; no fallback left");
                    ThrowIfExited();
                }
                _process.Dispose();
            }
            ThrowIfExited();
            async Task ConnectQmpAsync()
            {
                Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} connecting QMP on port {qmpPort}");
                for (var attempt = 0; ; attempt++)
                {
                    try
                    {
                        await Qmp.ConnectAsync(qmpPort, token);
                        Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} QMP connected after {attempt + 1} attempt(s)");
                        break;
                    }
                    catch (SocketException ex) when (attempt < 40)
                    {
                        // Log the first refusal only: QEMU often needs a few tries before it listens.
                        if (attempt == 0) Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} QMP not listening yet ({ex.SocketErrorCode}); retrying up to 40 times");
                        ThrowIfExited();
                        await Task.Delay(250, token);
                    }
                    catch (IOException ex) when (_process.HasExited || WaitedForExit(_process))
                    {
                        Trace.TraceError($"CloudRuntime.StartAsync: {_machine.Id} QEMU exited while QMP was connecting: {ex.Message}");
                        ThrowIfExited();
                        throw;
                    }
                }
                // Each QEMU (a guest restart starts another) gets its events listened to.
                _events?.Cancel();
                _events = QmpEventLog.Listen(eventsPort, _machine.Id, _machine.Folder);
            }
            await ConnectQmpAsync();
            var guestRestarts = 0;
            // First boot installs the desktop under emulation: allow hours on an emulated ARM machine.
            using var bootTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            bootTimeout.CancelAfter(firstBoot ? TimeSpan.FromHours(_machine.Architecture == MachineCatalog.HostArchitecture ? 2 : 4) : TimeSpan.FromMinutes(20));
            var how = Accelerator == QemuAcceleration.Whpx ? "with the Windows hypervisor" : "installing under emulation is slow";
            var shellConnected = false;
            var displayRepaired = false;
            var watch = Stopwatch.StartNew();
            var lastProgress = "";
            string? lastStage = null;
            while (true)
            {
                if (_process is { HasExited: true } && Accelerator == QemuAcceleration.Whpx && guestRestarts < MaxGuestRestarts && GuestRestarted(ReadShared(bootLog)))
                {
                    // The guest restarted itself (first-boot setup ends with one), and -no-reboot ended QEMU: start it again.
                    guestRestarts++;
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} restarted itself under WHPX; relaunching QEMU ({guestRestarts}/{MaxGuestRestarts})");
                    _progress($"{_machine.Name} restarted itself; starting it again…");
                    if (_errorPump != null) await _errorPump;
                    _process.Dispose();
                    // Keep what the guest said before restarting, and start the log afresh so the marker is only seen once.
                    File.Copy(bootLog, Path.Combine(_machine.Folder, "guest-boot.before-restart.log"), overwrite: true);
                    await ClearBootLogAsync(bootLog, _machine.Name, _progress, BootLogWait, token);
                    _process = Process.Start(Launch()) ?? throw new IOException("QEMU could not start.");
                    _errorPump = RecordErrorsAsync(_process);
                    await Task.Delay(1000, token);
                    ThrowIfExited();
                    await ConnectQmpAsync();
                    shellConnected = false;
                    continue;
                }
                ThrowIfExited();
                var output = ReadShared(bootLog);
                if (GuestNames.SaysProvisionFailed(output))
                {
                    Trace.TraceError($"CloudRuntime.StartAsync: {_machine.Id} provisioning failed after {watch.Elapsed.TotalSeconds:0}s ({GuestNames.ProvisionFailed} in guest-boot.log)");
                    throw new IOException($"Setting up {_machine.Name}'s desktop failed. See {Path.Combine(_machine.Folder, "guest-boot.log")} ({GuestNames.ProvisionFailed}) for the package error.");
                }
                if (GuestNames.SaysBootReady(output) ||
                    (PrepareDesktopAccountAsync != null && !firstBoot &&
                     output.Contains("PersonaBot Firefox startup timed out", StringComparison.Ordinal)))
                {
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} boot ready after {watch.Elapsed.TotalSeconds:0}s ({guestRestarts} guest restarts)");
                    break;
                }
                if (!displayRepaired && output.Contains(CloudGuestRepair.DisplayTakenMarker, StringComparison.Ordinal))
                {
                    // Another login screen has display :0 (a package brought it in): switch it off over the root console.
                    displayRepaired = true;
                    Trace.WriteLine($"CloudRuntime.StartAsync: display :0 is taken on {_machine.Id}; repairing");
                    _progress($"Starting {_machine.Name}: another login screen took the display; switching it off…");
                    if (!shellConnected)
                    {
                        await Transport.ConnectAsync(shellPort, bootTimeout.Token);
                        shellConnected = true;
                    }
                    var repair = await Transport.ExecuteAsync(CloudGuestRepair.DisplayManagerScript, bootTimeout.Token);
                    Trace.WriteLine($"CloudRuntime.StartAsync: repair said: {repair.Trim()}");
                    // Start the clock afresh: the desktop can only start from now.
                    bootTimeout.CancelAfter(TimeSpan.FromMinutes(20));
                }
                var progress = LatestProgress(output);
                var line = firstBoot
                    ? $"First start of {_machine.Name}: {progress ?? "booting"} ({watch.Elapsed:h\\:mm\\:ss}; {how})…"
                    : $"Starting {_machine.Name}… {watch.Elapsed:m\\:ss}";
                if (progress != lastStage)
                {
                    // Only when the stage changes, not on every poll.
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} boot stage: {progress ?? "booting"} ({watch.Elapsed.TotalSeconds:0}s)");
                    lastStage = progress;
                }
                if (line != lastProgress) { _progress(line); lastProgress = line; }
                await Task.Delay(2000, bootTimeout.Token);
            }
            if (!shellConnected)
            {
                Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} connecting the root console on port {shellPort}");
                await Transport.ConnectAsync(shellPort, bootTimeout.Token);
            }
            bool restartForAccount = PrepareDesktopAccountAsync != null &&
                await PrepareDesktopAccountAsync(Shell, bootTimeout.Token);
            // Keep display :0 PersonaBot's on later starts too, in machines made before the first-boot masks.
            try
            {
                var ensured = await Transport.ExecuteAsync(CloudGuestRepair.EnsureScriptFor(_machine.DesktopUser), bootTimeout.Token);
                Trace.WriteLine($"CloudRuntime.StartAsync: {ensured.Trim()}");
                if (ensured.Contains(CloudGuestRepair.ShadowStackMarker, StringComparison.Ordinal))
                {
                    // The kernel switch that stops the shadow-stack crashes is in place; it applies from the next start.
                    MachineDiary.Write(_machine.Folder, _machine.Id, "stability", "program shadow stacks are still on; clearcpuid=user_shstk applies from the next start");
                    _progress($"Starting {_machine.Name}: a stability fix is ready and takes effect the next time it starts.");
                }
                if (ensured.Split('\n').FirstOrDefault(l => l.Contains(CloudGuestRepair.DiskGrownMarker, StringComparison.Ordinal)) is { } grown)
                {
                    MachineDiary.Write(_machine.Folder, _machine.Id, "disk", grown.Trim());
                    _progress($"Starting {_machine.Name}: its disk has grown ({grown[(grown.IndexOf(':') + 1)..].Trim()})…");
                }
                if (!restartForAccount && ensured.Contains(CloudGuestRepair.CursorMarker, StringComparison.Ordinal))
                {
                    // A machine made before the software cursor: X reads it only when it starts, so restart the desktop once.
                    _progress($"Starting {_machine.Name}: turning on the visible cursor…");
                    var restarted = await Transport.ExecuteAsync("""
                        systemctl reset-failed personabot-desktop.service >/dev/null 2>&1
                        systemctl restart personabot-desktop.service
                        for n in $(seq 1 90); do DISPLAY=:0 xdotool search --onlyvisible --class firefox >/dev/null 2>&1 && break; sleep 1; done
                        """, bootTimeout.Token);
                    Trace.WriteLine($"CloudRuntime.StartAsync: desktop restarted for the software cursor: {restarted.Trim()}");
                }
            }
            catch (Exception ex) when (ex is IOException or OperationCanceledException && !token.IsCancellationRequested)
            {
                Trace.WriteLine($"CloudRuntime.StartAsync: couldn't run the start-up repairs: {ex.Message}");
            }
            if (restartForAccount)
            {
                var restarted = await Shell.ExecuteAsync("""
                    grep -Fq 'runuser -u {{desktop_user}} --' /usr/local/bin/personabot-session &&
                    systemctl restart personabot-desktop.service
                    """.Replace("{{desktop_user}}", _machine.DesktopUser, StringComparison.Ordinal), bootTimeout.Token);
                if (!restarted.TrimEnd().EndsWith("Exit code: 0", StringComparison.Ordinal))
                    throw new IOException("The Ubuntu desktop could not restart with its new account: " + restarted.Trim());
            }
            if (SoundEnabled) await CheckSoundAsync(bootTimeout.Token);
            if (firstBoot)
            {
                Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} first boot complete; writing provisioned.txt");
                await File.WriteAllTextAsync(Path.Combine(_machine.Folder, CloudInstaller.ProvisionedName), DateTimeOffset.Now.ToString("O") + "\n", token);
            }
            Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} checking Firefox's window");
            var browser = await Shell.ExecuteAsync(CloudGuestRepair.BrowserStartupScript, bootTimeout.Token);
            if (!browser.TrimEnd().EndsWith("Exit code: 0", StringComparison.Ordinal))
            {
                Trace.TraceError($"CloudRuntime.StartAsync: {_machine.Id} Firefox didn't open its window: {browser.Trim()}");
                throw new IOException("Firefox didn't open its browser window: " + browser.Trim());
            }
            if (SharedGpuEnabled) await VerifyGraphicsAsync(bootTimeout.Token);
            if (RequireSharedGpu && !SharedGpuVerified)
                throw new IOException(GraphicsFallback ?? "Ubuntu did not confirm hardware GPU acceleration.");
            if (SharedGpuEnabled)
            {
                try
                {
                    _framebuffer = new QemuFramebuffer();
                    await _framebuffer.ConnectAsync(framebufferPort, bootTimeout.Token);
                    var visible = false;
                    var attempts = 0;
                    for (var attempt = 0; attempt < 6; attempt++)
                    {
                        attempts++;
                        var image = await CaptureAsync(bootTimeout.Token);
                        WindowsPlatform.PngSize(image);
                        if (QemuGraphics.HasDesktopContent(image)) { visible = true; break; }
                        await Task.Delay(500, bootTimeout.Token);
                    }
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} accelerated framebuffer visible={visible} after {attempts} capture(s)");
                    if (!visible) throw new IOException("The accelerated framebuffer returned blank images after Firefox started.");
                }
                catch (Exception ex) when (ex is IOException or SocketException || ex is OperationCanceledException && !bootTimeout.IsCancellationRequested)
                {
                    GraphicsFallback = "The accelerated desktop couldn't be captured; using basic graphics. " + ex.Message;
                    _progress(GraphicsFallback);
                    Trace.TraceWarning($"CloudRuntime: {_machine.Id} {GraphicsFallback} ({ex.GetType().Name})");
                    try
                    {
                        await File.WriteAllTextAsync(Path.Combine(_machine.Folder, "qemu-gpu-failure.log"), ReadShared(Path.Combine(_machine.Folder, "qemu.log")), token);
                    }
                    catch (IOException logError) { Trace.TraceWarning($"CloudRuntime.StartAsync: couldn't save qemu-gpu-failure.log: {logError.Message}"); }
                    if (RequireSharedGpu) throw new IOException("Required GPU readback failed: " + ex.Message, ex);
                    Trace.WriteLine($"CloudRuntime.StartAsync: {_machine.Id} restarting with basic graphics");
                    await StopProcessAsync();
                    Qmp = new QmpClient();
                    Transport = new GuestShell();
                    Shell = new ConsoleShell(Transport);
                    SharedGpuEnabled = SharedGpuVerified = false;
                    _disableSharedGpu = true;
                    await StartAsync(settings, token);
                    return;
                }
            }
            await CollectPostMortemAsync(bootTimeout.Token);
            DisplayReady?.Invoke();
            if (_process != null)
            {
                WatchForUnexpectedStop(_process);
                _hostWatch?.Dispose();
                _hostWatch = HostWatch.Start(_process, _machine.Folder, _machine.Id);
            }
            MachineDiary.Write(_machine.Folder, _machine.Id, "start", $"ready after {watch.Elapsed.TotalSeconds:0}s: QEMU pid {_process?.Id}, shared GPU {(SharedGpuEnabled ? "on" : "off")}");
            Trace.WriteLine($"CloudRuntime.StartAsync exit: ready after {watch.Elapsed.TotalSeconds:F0}s");
        }
        catch (OperationCanceledException ex) when (!token.IsCancellationRequested)
        {
            Trace.TraceError($"CloudRuntime.StartAsync: {_machine.Id} didn't become ready in time (firstBoot={firstBoot}); see {bootLog}");
            await DisposeAsync();
            throw new TimeoutException($"{_machine.Name} didn't become ready in time. See {bootLog}.", ex);
        }
        catch (Exception ex)
        {
            Trace.TraceError($"CloudRuntime.StartAsync: {_machine.Id} failed to start: {ex.GetType().Name}: {ex.Message}");
            await DisposeAsync();
            throw;
        }
    }

    /// <summary>Sets up and checks the guest's sound (CloudSound.GuestScript); a failure is reported, never fatal.</summary>
    private async Task CheckSoundAsync(CancellationToken token)
    {
        Trace.WriteLine($"CloudRuntime.CheckSoundAsync enter: {_machine.Id}");
        _progress("Checking sound…");
        try
        {
            var output = await Shell.ExecuteAsync(CloudSound.GuestScriptFor(_machine.DesktopUser), token);
            await File.WriteAllTextAsync(Path.Combine(_machine.Folder, "guest-sound.log"), output, token);
            SoundStatus = output.Contains(CloudSound.ReadyMarker, StringComparison.Ordinal)
                ? "Sound on"
                : "Sound unavailable: the guest has no sound output. See guest-sound.log.";
            Trace.WriteLine($"CloudRuntime.CheckSoundAsync exit: {SoundStatus}");
        }
        catch (IOException ex)
        {
            SoundStatus = "Sound unavailable: " + ex.Message;
            Trace.WriteLine($"CloudRuntime.CheckSoundAsync: {ex.Message}");
        }
        if (SoundStatus != "Sound on") _progress(SoundStatus);
    }

    private async Task VerifyGraphicsAsync(CancellationToken token)
    {
        Trace.WriteLine($"CloudRuntime.VerifyGraphicsAsync enter: {_machine.Id}");
        _progress("Checking the guest's shared GPU renderer…");
        try
        {
            var output = await Shell.ExecuteAsync(SharedGraphicsScriptFor(_machine.DesktopUser), token);
            GraphicsRenderer = output;
            SharedGpuVerified = output.TrimEnd().EndsWith("Exit code: 0", StringComparison.Ordinal) && QemuGraphics.IsHardwareVirgl(output);
            Trace.WriteLine($"CloudRuntime.VerifyGraphicsAsync exit: {_machine.Id} verified={SharedGpuVerified}; renderer output in guest-gpu.log");
            if (!SharedGpuVerified)
            {
                Trace.TraceWarning($"CloudRuntime.VerifyGraphicsAsync: {_machine.Id} didn't confirm hardware VirGL: {output.Trim()}");
                GraphicsFallback = "The guest did not confirm hardware VirGL rendering. See guest-gpu.log.";
                _progress(GraphicsFallback);
            }
            await File.WriteAllTextAsync(Path.Combine(_machine.Folder, "guest-gpu.log"), output, token);
        }
        catch (IOException ex)
        {
            Trace.TraceWarning($"CloudRuntime.VerifyGraphicsAsync: {_machine.Id} couldn't verify the GPU: {ex.Message}");
            GraphicsFallback = "Couldn't verify the guest GPU: " + ex.Message;
            _progress(GraphicsFallback);
        }
    }

    /// <summary>Also upgrades existing cloud machines; never forces Firefox's driver blocklist off.</summary>
    public static string SharedGraphicsScript => SharedGraphicsScriptFor(GuestNames.DesktopUser);

    /// <summary><see cref="SharedGraphicsScript"/> for a machine whose desktop runs as <paramref name="user"/>.</summary>
    public static string SharedGraphicsScriptFor(string user)
    {
        GuestNames.CheckUser(user);
        return $$"""
        set -e
        missing=0
        for package in libgl1-mesa-dri libegl-mesa0 mesa-utils; do
            dpkg-query -W -f='${Status}\n' "$package" 2>/dev/null | grep -qx 'install ok installed' || missing=1
        done
        if test "$missing" = 1; then
            export DEBIAN_FRONTEND=noninteractive
            apt-get update
            apt-get install -y --no-install-recommends libgl1-mesa-dri libegl-mesa0 mesa-utils
            systemctl restart personabot-desktop.service
        fi
        usermod -a -G video,render {{user}}
        for n in $(seq 1 30); do
            if xdotool search --onlyvisible --class firefox >/dev/null 2>&1; then
                runuser -u {{user}} -- env DISPLAY=:0 glxinfo -B
                break
            fi
            sleep 1
        done
        """;
    }

    private static readonly System.Text.RegularExpressions.Regex TerminalFormatting = new(
        @"\x1B(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1B]*(?:\x07|\x1B\\))|[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    /// <summary>A human-readable startup stage. Raw terminal output stays in guest-boot.log.</summary>
    public static string? LatestProgress(string bootLog)
    {
        ArgumentNullException.ThrowIfNull(bootLog);
        var lines = TerminalFormatting.Replace(bootLog, "").Split('\n');
        for (var i = lines.Length - 1; i >= 0; i--)
        {
            var line = lines[i].Trim();
            var at = line.IndexOf("PERSONABOT_PROGRESS ", StringComparison.Ordinal);
            if (at >= 0)
            {
                var progress = System.Text.RegularExpressions.Regex.Replace(line[(at + "PERSONABOT_PROGRESS ".Length)..], @"\s+", " ").Trim();
                if (progress.Length > 0) return progress.Length > 120 ? progress[..117] + "…" : progress;
            }
            if (GuestNames.SaysBootReady(line)) return "Desktop ready";
            if (line.Contains("reboot: Restarting system", StringComparison.Ordinal)
                || line.Contains("provisioning done; restarting into the desktop", StringComparison.OrdinalIgnoreCase)) return "Restarting into the desktop";
            if (line.Contains("personabot-desktop.service", StringComparison.Ordinal)
                || line.Contains("cloud-init.target", StringComparison.Ordinal)
                || line.Contains("cloud-init", StringComparison.OrdinalIgnoreCase) && line.Contains("finished at", StringComparison.Ordinal)) return "Starting the desktop and Firefox";
            if (line.Contains("cloud-init", StringComparison.OrdinalIgnoreCase) && line.Contains("running 'modules:final'", StringComparison.Ordinal)) return "Installing desktop packages";
            if (line.Contains("cloud-init", StringComparison.OrdinalIgnoreCase) && line.Contains("running '", StringComparison.Ordinal)) return "Preparing Linux";
        }
        return null;
    }

    private static string ReadShared(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }

    private static int Port()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private async Task RecordErrorsAsync(Process process)
    {
        Trace.WriteLine($"CloudRuntime.RecordErrorsAsync enter: {_machine.Id} pid {process.Id}");
        var lines = 0;
        await using var writer = new StreamWriter(Path.Combine(_machine.Folder, "qemu.log")) { AutoFlush = true };
        while (await process.StandardError.ReadLineAsync() is { } line)
        {
            await writer.WriteLineAsync(line);
            PersonaBotLogging.Machine(_machine.Id, "qemu", line);
            lines++;
        }
        Trace.WriteLine($"CloudRuntime.RecordErrorsAsync exit: {_machine.Id} QEMU wrote {lines} stderr lines; exited={process.HasExited}");
        PersonaBotLogging.Machine(_machine.Id, "qemu", process.HasExited ? $"QEMU ended with code {process.ExitCode}" : "QEMU closed its error output");
    }

    /// <summary>QEMU closing its control connection usually means it is exiting; give it a moment to finish.</summary>
    private static bool WaitedForExit(Process process) => process.WaitForExit(2000);

    /// <summary>When QEMU has stopped, says why in its own words (the last line of qemu.log), e.g. not enough memory.</summary>
    private void ThrowIfExited()
    {
        if (_process is not { HasExited: true }) return;
        var log = Path.Combine(_machine.Folder, "qemu.log");
        var reason = QemuLog.LastLine(log);
        Trace.TraceError($"CloudRuntime.ThrowIfExited: {_machine.Id} QEMU exited with code {_process.ExitCode}: {reason}");
        if (reason.Contains("cannot set up guest memory", StringComparison.Ordinal))
            throw new IOException($"QEMU couldn't reserve {MemoryMb} MB for {_machine.Name}: Windows is short of free memory. Close other programs, or lower the machine's memory in Settings, then try again. ({reason})");
        throw new IOException($"QEMU stopped with code {_process.ExitCode}{(reason.Length > 0 ? ": " + reason : "")}. See {log}.");
    }

    public async Task<byte[]> CaptureAsync(CancellationToken token)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await _captureGate.WaitAsync(token);
        try
        {
            if (_framebuffer != null) return await _framebuffer.CaptureAsync(token);
            var path = Path.Combine(_machine.Folder, "screen.png");
            await Qmp.ExecuteAsync("screendump", new { filename = path, format = "png" }, token);
            return await File.ReadAllBytesAsync(path, token);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            // Captures are polled: only failures are logged.
            Trace.TraceWarning($"CloudRuntime.CaptureAsync: {_machine.Id} capture failed ({(_framebuffer != null ? "framebuffer" : "screendump")}): {ex.GetType().Name}: {ex.Message}");
            throw;
        }
        finally { _captureGate.Release(); }
    }

    public async IAsyncEnumerable<GuestDisplayFrame> StreamDisplayAsync(
        [System.Runtime.CompilerServices.EnumeratorCancellation] CancellationToken token)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        // Dedicated RFB client: an idle incremental read must never block an AI screenshot.
        Trace.WriteLine($"CloudRuntime.StreamDisplayAsync enter: {_machine.Id} on port {_displayPort}");
        await using var display = new QemuFramebuffer(audio: SoundEnabled ? AudioReceived : null);
        await display.ConnectAsync(_displayPort, token);
        var incremental = false;
        while (!token.IsCancellationRequested)
        {
            yield return await display.ReadFrameAsync(incremental, token);
            incremental = true;
        }
        Trace.WriteLine($"CloudRuntime.StreamDisplayAsync exit: {_machine.Id} stream cancelled");
    }

    public IGuestInput CreateInput()
    {
        Trace.WriteLine($"CloudRuntime.CreateInput: {_machine.Id} QMP input, screen {ScreenBounds.Width}x{ScreenBounds.Height}");
        return new QemuInput(this);
    }

    public Task SuspendAsync(string tag, CancellationToken token) => throw new NotSupportedException("Snapshots of this machine aren't supported yet.");
    public Task ResumeAsync() => throw new NotSupportedException("Snapshots of this machine aren't supported yet.");
    public Task DeleteCheckpointAsync(string tag) => throw new NotSupportedException("Snapshots of this machine aren't supported yet.");
    public Task<IReadOnlyList<GuestCheckpoint>> ListCheckpointsAsync(CancellationToken token) => Task.FromResult<IReadOnlyList<GuestCheckpoint>>([]);
    public Task RestoreCheckpointAsync(string tag, CancellationToken token) => throw new NotSupportedException("Snapshots of this machine aren't supported yet.");

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        Trace.WriteLine($"CloudRuntime.DisposeAsync: {_machine.Id} disposing");
        await StopProcessAsync();
        _consoleFollow?.Cancel();
        _events?.Cancel();
        _hostWatch?.Dispose();
        _hostWatch = null;
        MachineDiary.Write(_machine.Folder, _machine.Id, "stop", "stopped by PersonaBot");
    }

    private async Task StopProcessAsync()
    {
        Trace.WriteLine($"CloudRuntime.DisposeAsync enter: {_machine.Id}");
        try
        {
            BotDisplayScreen[] displays;
            lock (_botDisplays) { displays = [.. _botDisplays.Values]; _botDisplays.Clear(); }
            Trace.WriteLine($"CloudRuntime.StopProcessAsync: {_machine.Id} closing {displays.Length} bot display(s), QEMU running={_process?.HasExited == false}");
            foreach (var display in displays) await display.DisposeAsync();
            if (_process is { HasExited: false })
            {
                // A clean shutdown, so the disk is never cut off mid-write; QEMU is stopped only if it hangs.
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
                var watch = Stopwatch.StartNew();
                try
                {
                    await Shell.ExecuteAsync("sync; systemctl poweroff --no-block", timeout.Token);
                    await _process.WaitForExitAsync(timeout.Token);
                    Trace.WriteLine($"CloudRuntime.StopProcessAsync: {_machine.Id} powered off in {watch.Elapsed.TotalSeconds:0.0}s");
                }
                catch (Exception ex) when (ex is IOException or OperationCanceledException or NullReferenceException or InvalidOperationException)
                {
                    Trace.WriteLine("CloudRuntime.DisposeAsync: clean shutdown unavailable, stopping QEMU: " + ex.Message);
                    if (!_process.HasExited) { _process.Kill(entireProcessTree: true); await _process.WaitForExitAsync(); }
                }
            }
            await Transport.DisposeAsync();
            await Qmp.DisposeAsync();
            if (_framebuffer != null) await _framebuffer.DisposeAsync();
            if (_errorPump != null) await _errorPump;
        }
        finally
        {
            _process?.Dispose();
            _process = null;
            _errorPump = null;
            _framebuffer = null;
            Trace.WriteLine("CloudRuntime.DisposeAsync exit");
        }
    }

    /// <summary>The root console, with the desktop's display and a UTF-8 locale.</summary>
    private sealed class ConsoleShell(GuestShell transport) : IGuestShell
    {
        public Task<string> ExecuteAsync(string script, CancellationToken token = default)
        {
            ArgumentNullException.ThrowIfNull(script);
            return transport.ExecuteAsync("export DISPLAY=:0 LANG=C.UTF-8 LC_ALL=C.UTF-8\n" + script, token);
        }

        public Task SynchronizeAsync(CancellationToken token) => transport.SynchronizeAsync(token);
    }
}
