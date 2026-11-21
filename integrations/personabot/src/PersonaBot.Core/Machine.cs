using System.Diagnostics;
using System.Globalization;

namespace PersonaBot;

/// <summary>A guest screen size the desktops support (DISTROS.md).</summary>
public sealed record GuestResolution(int Width, int Height)
{
    public static GuestResolution Default { get; } = new(1280, 800);

    /// <summary>The sizes offered. Every one is a standard VESA/VBE mode, so Tiny Core's VESA driver, virtio-gpu and Xvfb all have it.</summary>
    public static IReadOnlyList<GuestResolution> Offered { get; } =
        [new(1024, 768), new(1280, 800), new(1280, 1024), new(1366, 768), new(1440, 900), new(1600, 900), new(1680, 1050), new(1920, 1080)];

    public static bool IsOffered(int width, int height) => Offered.Any(r => r.Width == width && r.Height == height);

    public override string ToString() => $"{Width} × {Height}";
}

/// <summary>
/// One Linux computer PersonaBot can run (DISTROS.md §3): a distribution on an architecture, with its own
/// disk in its own folder. Machines can run concurrently in separate tabs; the default starts with the app.
/// </summary>
public sealed class Machine
{
    /// <summary>The id of the install that existed before machines: it stays in Vm/ itself.</summary>
    public const string OriginalId = "original";

    public required string Id { get; init; }
    public required string Name { get; set; }

    /// <summary>Catalogue key, e.g. "tinycore-17", "debian-13", "ubuntu-24.04".</summary>
    public required string Distribution { get; init; }

    /// <summary>"x86_64" or "aarch64".</summary>
    public required string Architecture { get; init; }

    /// <summary>The machine's folder: its disk, seed, firmware variables and logs.</summary>
    public required string Folder { get; init; }

    /// <summary>
    /// The user the desktop runs as on a Debian or Ubuntu machine, fixed when it is installed: the person's own account
    /// if they had made one then (PersonAccounts), otherwise <see cref="GuestNames.DesktopUser"/>.
    /// </summary>
    public string DesktopUser { get; init; } = GuestNames.DesktopUser;

    /// <summary>That user's home folder.</summary>
    public string DesktopHome => "/home/" + DesktopUser;

    public int ScreenWidth { get; set; } = GuestResolution.Default.Width;
    public int ScreenHeight { get; set; } = GuestResolution.Default.Height;
    public DateTimeOffset CreatedAt { get; init; } = DateTimeOffset.UtcNow;

    public bool IsOriginal => Id == OriginalId;
    public GuestResolution Resolution => new(ScreenWidth, ScreenHeight);
    public string ArchitectureName => MachineCatalog.ArchitectureName(Architecture);
}

/// <summary>A distribution and architecture that can be installed (DISTROS.md §2 and §7).</summary>
public sealed record CatalogueEntry(string Distribution, string Name, string Architecture, string PackageManager, int DownloadMb, bool Offered);

/// <summary>What PersonaBot can run: the first catalogue is Debian 13 and Ubuntu 24.04 minimal on x86-64 and ARM64 (DISTROS.md §7).</summary>
public static class MachineCatalog
{
    public static IReadOnlyList<CatalogueEntry> Entries { get; } =
    [
        new("ubuntu-24.04", "Ubuntu 24.04 minimal", "x86_64", "apt", 248, true),
        new("ubuntu-24.04", "Ubuntu 24.04 minimal", "aarch64", "apt", 214, true),
        new("debian-13", "Debian 13", "x86_64", "apt", 326, true),
        new("debian-13", "Debian 13", "aarch64", "apt", 323, true),
        new("android-16", "Android 16 (AOSP)", "aarch64", "APK", 1060, true),
        new("android-17", "Android 17 (AOSP)", "x86_64", "APK", 1110, true),
        // Existing installs only: not offered for new machines.
        new("tinycore-17", "Tiny Core 17.1", "x86_64", "tce", 42, false),
        new("debian-13-proot", "Debian 13 (built in)", "aarch64", "apt", 100, false)
    ];

    public static string ArchitectureName(string architecture) => architecture switch
    {
        "x86_64" => "x86-64",
        "aarch64" => "ARM64",
        _ => architecture
    };

    public static CatalogueEntry? Find(string distribution, string architecture) =>
        Entries.FirstOrDefault(e => e.Distribution == distribution && e.Architecture == architecture);

    /// <summary>The distribution new installs default to (DISTROS.md §7, decision 1).</summary>
    public const string RecommendedDistribution = "ubuntu-24.04";

    /// <summary>
    /// The choice to preselect among <paramref name="choices"/>: Ubuntu 24.04 minimal on the host's own
    /// architecture, else any machine of the host's architecture, else the first; null when there are none.
    /// </summary>
    public static CatalogueEntry? Recommended(IReadOnlyList<CatalogueEntry> choices, string hostArchitecture)
    {
        ArgumentNullException.ThrowIfNull(choices);
        ArgumentException.ThrowIfNullOrWhiteSpace(hostArchitecture);
        var recommended = choices.FirstOrDefault(e => e.Distribution == RecommendedDistribution && e.Architecture == hostArchitecture)
            ?? choices.FirstOrDefault(e => e.Architecture == hostArchitecture)
            ?? choices.FirstOrDefault();
        Trace.WriteLine($"MachineCatalog.Recommended: {(recommended == null ? "none" : recommended.Distribution + "/" + recommended.Architecture)} of {choices.Count} choices on {hostArchitecture}");
        return recommended;
    }

    /// <summary>
    /// How fast a guest architecture runs on this host: native speed needs the host's own architecture
    /// and a hypervisor (HVF on the Mac, WHPX on Windows when switched on); anything else is emulated.
    /// </summary>
    public static string SpeedHint(string guestArchitecture, string hostArchitecture, bool hypervisor)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(guestArchitecture);
        ArgumentException.ThrowIfNullOrWhiteSpace(hostArchitecture);
        if (guestArchitecture != hostArchitecture) return "Emulated: slower";
        return hypervisor ? "Fast" : "Emulated (no hypervisor)";
    }

    /// <summary>This computer's architecture in the catalogue's terms.</summary>
    public static string HostArchitecture => System.Runtime.InteropServices.RuntimeInformation.OSArchitecture switch
    {
        System.Runtime.InteropServices.Architecture.Arm64 => "aarch64",
        _ => "x86_64"
    };
}

/// <summary>
/// The machines, in PersonaBot's database (PersonaBotMachines). The default is a preference. The install
/// that existed before machines is registered as the "original" machine, left in Vm/ where it is.
/// </summary>
public sealed class MachineStore
{
    private readonly Automaton.Core.Services.ConfigDatabase _db;

    /// <param name="original">What the platform's existing install is (distribution and architecture), registered when there are no machines yet.</param>
    /// <param name="registerOriginal">
    /// False when the setup wizard is about to add the machine the person chose instead, so a fresh install
    /// doesn't also list an original install that was never made.
    /// </param>
    public MachineStore((string Distribution, string Architecture, string Name) original, bool registerOriginal = true)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(original.Distribution);
        ArgumentException.ThrowIfNullOrWhiteSpace(original.Architecture);
        Trace.WriteLine($"MachineStore ctor enter: original is {original.Distribution}/{original.Architecture}, register {registerOriginal}");
        _db = PersonaBotDatabase.EnsureReady();
        if (registerOriginal && IsEmpty) RegisterOriginal(original);
        Trace.WriteLine($"MachineStore ctor exit: {All.Count} machines");
    }

    /// <summary>True before any machine is registered (a fresh install, before the wizard's choice is added).</summary>
    public bool IsEmpty => _db.ExecuteScalar<long>("SELECT COUNT(*) FROM PersonaBotMachines") == 0;

    /// <summary>Registers the platform's existing install as the original machine, unless it is already registered.</summary>
    public Machine RegisterOriginal((string Distribution, string Architecture, string Name) original)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(original.Distribution);
        ArgumentException.ThrowIfNullOrWhiteSpace(original.Architecture);
        Trace.WriteLine($"MachineStore.RegisterOriginal enter: {original.Distribution}/{original.Architecture} \"{original.Name}\"");
        if (All.FirstOrDefault(m => m.IsOriginal) is { } existing)
        {
            Trace.WriteLine($"MachineStore.RegisterOriginal exit: already registered in {existing.Folder}");
            return existing;
        }
        var machine = new Machine
        {
            Id = Machine.OriginalId, Name = original.Name, Distribution = original.Distribution, Architecture = original.Architecture,
            Folder = Settings.VmDirectory
        };
        Insert(machine);
        Trace.WriteLine("MachineStore.RegisterOriginal: registered the original install");
        return machine;
    }

    public IReadOnlyList<Machine> All => _db.Query("SELECT * FROM PersonaBotMachines ORDER BY CreatedAt").Select(Read).ToList();

    public Machine Get(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        if (All.FirstOrDefault(m => m.Id == id) is { } machine) return machine;
        Trace.TraceWarning($"MachineStore.Get: there is no machine '{id}'");
        throw new ArgumentException($"There is no machine '{id}'.");
    }

    /// <summary>The machine that starts with the app: the chosen default, or the oldest machine.</summary>
    public string DefaultId
    {
        get
        {
            var id = _db.ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = 'DefaultMachine'");
            var all = All;
            if (all.Count == 0) throw new InvalidOperationException("There are no machines yet.");
            return id != null && all.Any(m => m.Id == id) ? id : all[0].Id;
        }
    }

    public Machine Default => Get(DefaultId);

    public void SetDefault(string id)
    {
        Get(id);
        Trace.WriteLine($"MachineStore.SetDefault: {id}");
        _db.Execute(@"INSERT INTO PersonaBotPreferences (Key, Value) VALUES ('DefaultMachine', @Id)
                ON CONFLICT(Key) DO UPDATE SET Value = excluded.Value", ChatStore.P("@Id", id));
    }

    /// <summary>Sets a machine's screen size; it applies the next time the machine starts.</summary>
    public void SetResolution(string id, GuestResolution resolution)
    {
        ArgumentNullException.ThrowIfNull(resolution);
        Get(id);
        if (!GuestResolution.IsOffered(resolution.Width, resolution.Height))
        {
            Trace.TraceWarning($"MachineStore.SetResolution: refused {resolution.Width}x{resolution.Height} for {id}: not offered");
            throw new ArgumentException($"{resolution} isn't one of the offered screen sizes.");
        }
        Trace.WriteLine($"MachineStore.SetResolution: {id} -> {resolution}");
        _db.Execute("UPDATE PersonaBotMachines SET ScreenWidth = @W, ScreenHeight = @H WHERE Id = @Id",
            ChatStore.P("@W", resolution.Width), ChatStore.P("@H", resolution.Height), ChatStore.P("@Id", id));
    }

    public void Rename(string id, string name)
    {
        Get(id);
        var clean = (name ?? "").Trim();
        if (clean.Length is 0 or > 60)
        {
            Trace.TraceWarning($"MachineStore.Rename: refused a {clean.Length}-char name for {id}");
            throw new ArgumentException("Give the machine a name of up to 60 characters.");
        }
        _db.Execute("UPDATE PersonaBotMachines SET Name = @Name WHERE Id = @Id", ChatStore.P("@Name", clean), ChatStore.P("@Id", id));
        Trace.WriteLine($"MachineStore.Rename: {id} -> \"{clean}\"");
    }

    /// <summary>
    /// Removes a machine and deletes its disk. Not the running machine, and never the last one. The original
    /// install's files sit in Vm/ beside other machines' folders, so only its own files go.
    /// </summary>
    public void Remove(string id, string? runningId)
    {
        var machine = Get(id);
        if (id == runningId)
        {
            Trace.TraceWarning($"MachineStore.Remove: refused {id}: it is running");
            throw new InvalidOperationException("Shut this machine down, or switch to another, before removing it.");
        }
        if (All.Count <= 1)
        {
            Trace.TraceWarning($"MachineStore.Remove: refused {id}: it is the only machine");
            throw new InvalidOperationException("This is the only machine. Add another before removing it.");
        }
        Trace.WriteLine($"MachineStore.Remove enter: {id} in {machine.Folder}");
        _db.ExecuteInTransaction((_, _) =>
        {
            _db.Execute("DELETE FROM PersonaBotMachines WHERE Id = @Id", ChatStore.P("@Id", id));
            if (_db.ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = 'DefaultMachine'") == id)
                _db.Execute("DELETE FROM PersonaBotPreferences WHERE Key = 'DefaultMachine'");
        });
        DeleteFiles(machine);
        Trace.WriteLine("MachineStore.Remove exit");
    }

    /// <summary>
    /// Registers a new machine from the catalogue, with its own folder (Vm/{id}). Installing its disk is
    /// the platform's job (DISTROS.md phase 2); until then the machine shows as not installed.
    /// </summary>
    public Machine Add(string distribution, string architecture, GuestResolution? resolution = null)
    {
        Trace.WriteLine($"MachineStore.Add enter: {distribution}/{architecture}, screen {(resolution == null ? "default" : resolution.Width + "x" + resolution.Height)}");
        if (MachineCatalog.Find(distribution, architecture) == null) Trace.TraceWarning($"MachineStore.Add: refused {distribution}/{architecture}: not in the catalogue");
        var entry = MachineCatalog.Find(distribution, architecture) ?? throw new ArgumentException($"{distribution} on {architecture} isn't in the catalogue.");
        if (!entry.Offered)
        {
            Trace.TraceWarning($"MachineStore.Add: refused {distribution}/{architecture}: not offered for new machines");
            throw new ArgumentException($"{entry.Name} isn't offered for new machines.");
        }
        var screen = resolution ?? GuestResolution.Default;
        if (!GuestResolution.IsOffered(screen.Width, screen.Height))
        {
            Trace.TraceWarning($"MachineStore.Add: refused screen {screen.Width}x{screen.Height}: not offered");
            throw new ArgumentException($"{screen} isn't one of the offered screen sizes.");
        }
        var all = All;
        var baseId = $"{distribution}-{architecture}";
        var id = baseId;
        for (var n = 2; all.Any(m => m.Id == id); n++) id = $"{baseId}-{n}";
        var name = $"{entry.Name} · {MachineCatalog.ArchitectureName(architecture)}";
        if (id != baseId) name += $" ({id[(baseId.Length + 1)..]})";
        var machine = new Machine
        {
            Id = id, Name = name, Distribution = distribution, Architecture = architecture, Folder = FolderFor(id),
            ScreenWidth = screen.Width, ScreenHeight = screen.Height,
            // The desktop is the person's own account when they have one (it can't change once the machine is installed).
            DesktopUser = PersonAccounts.Load()?.LinuxUser ?? GuestNames.DesktopUser
        };
        Insert(machine);
        Trace.WriteLine($"MachineStore.Add exit: {id} \"{name}\" in {machine.Folder}, desktop user {machine.DesktopUser}");
        return machine;
    }

    /// <summary>The space a machine's disk and files take, in bytes; 0 before it is installed.</summary>
    public static long DiskBytes(Machine machine)
    {
        ArgumentNullException.ThrowIfNull(machine);
        if (!Directory.Exists(machine.Folder))
        {
            Trace.WriteLine($"MachineStore.DiskBytes: {machine.Id} has no folder yet ({machine.Folder})");
            return 0;
        }
        // The original machine's folder (Vm/) also holds the other machines' folders: count only its own files.
        var files = machine.IsOriginal
            ? Directory.EnumerateFiles(machine.Folder).Concat(OriginalSubfolders.Select(s => Path.Combine(machine.Folder, s)).Where(Directory.Exists).SelectMany(d => Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories)))
            : Directory.EnumerateFiles(machine.Folder, "*", SearchOption.AllDirectories);
        var bytes = files.Sum(f => new FileInfo(f).Length);
        Trace.WriteLine($"MachineStore.DiskBytes: {machine.Id} uses {bytes / (1024 * 1024)} MB");
        return bytes;
    }

    /// <summary>A new machine's folder: Vm/{id}.</summary>
    public static string FolderFor(string id) => Path.Combine(Settings.VmDirectory, id);

    private static readonly string[] OriginalSubfolders = ["system", "seed"];

    private static void DeleteFiles(Machine machine)
    {
        Trace.WriteLine($"MachineStore.DeleteFiles enter: {machine.Id} in {machine.Folder}, original={machine.IsOriginal}");
        if (!Directory.Exists(machine.Folder))
        {
            Trace.WriteLine("MachineStore.DeleteFiles exit: no folder");
            return;
        }
        if (!machine.IsOriginal)
        {
            Directory.Delete(machine.Folder, recursive: true);
            Trace.WriteLine($"MachineStore.DeleteFiles exit: deleted {machine.Folder}");
            return;
        }
        foreach (var file in Directory.EnumerateFiles(machine.Folder)) File.Delete(file);
        foreach (var sub in OriginalSubfolders.Select(s => Path.Combine(machine.Folder, s)).Where(Directory.Exists)) Directory.Delete(sub, recursive: true);
        Trace.WriteLine("MachineStore.DeleteFiles exit: deleted the original install's own files");
    }

    /// <summary>
    /// Points every machine kept under <paramref name="from"/> at the same place under <paramref name="to"/>,
    /// after their files were moved there. Machines elsewhere are left alone. Returns how many moved.
    /// </summary>
    public int Relocate(string from, string to)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(from);
        ArgumentException.ThrowIfNullOrWhiteSpace(to);
        var oldRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(from));
        var newRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(to));
        Trace.WriteLine($"MachineStore.Relocate enter: {oldRoot} -> {newRoot}");
        var moved = 0;
        _db.ExecuteInTransaction((_, _) =>
        {
            foreach (var machine in All)
            {
                var folder = Path.TrimEndingDirectorySeparator(machine.Folder);
                string? rest = string.Equals(folder, oldRoot, StringComparison.OrdinalIgnoreCase) ? ""
                    : folder.StartsWith(oldRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ? folder[(oldRoot.Length + 1)..] : null;
                if (rest == null) continue;
                var target = rest.Length == 0 ? newRoot : Path.Combine(newRoot, rest);
                _db.Execute("UPDATE PersonaBotMachines SET Folder = @Folder WHERE Id = @Id", ChatStore.P("@Folder", StoredFolder(target)), ChatStore.P("@Id", machine.Id));
                Trace.WriteLine($"MachineStore.Relocate: {machine.Id} -> {target}");
                moved++;
            }
        });
        Trace.WriteLine($"MachineStore.Relocate exit: {moved} machine(s)");
        return moved;
    }

    /// <summary>How a folder is stored: relative to the data folder when inside it (so moving PersonaBot's data keeps it), else in full.</summary>
    private static string StoredFolder(string folder)
    {
        var full = Path.GetFullPath(folder);
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Settings.Root));
        return full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || string.Equals(full, root, StringComparison.OrdinalIgnoreCase)
            ? Path.GetRelativePath(root, full) : full;
    }

    private void Insert(Machine machine)
    {
        Trace.WriteLine($"MachineStore.Insert: {machine.Id} ({machine.Distribution}/{machine.Architecture}, {machine.ScreenWidth}x{machine.ScreenHeight}) stored as {StoredFolder(machine.Folder)}");
        _db.Execute(@"INSERT INTO PersonaBotMachines (Id, Name, Distribution, Architecture, Folder, ScreenWidth, ScreenHeight, CreatedAt, DesktopUser)
                VALUES (@Id, @Name, @Distribution, @Architecture, @Folder, @W, @H, @Created, @User)",
            ChatStore.P("@Id", machine.Id), ChatStore.P("@Name", machine.Name), ChatStore.P("@Distribution", machine.Distribution),
            ChatStore.P("@Architecture", machine.Architecture), ChatStore.P("@Folder", StoredFolder(machine.Folder)), ChatStore.P("@W", machine.ScreenWidth),
            ChatStore.P("@H", machine.ScreenHeight), ChatStore.P("@Created", machine.CreatedAt.ToString("O", CultureInfo.InvariantCulture)),
            ChatStore.P("@User", machine.DesktopUser));
    }

    private static Machine Read(Dictionary<string, object?> row) => new()
    {
        Id = (string)row["Id"]!, Name = (string)row["Name"]!, Distribution = (string)row["Distribution"]!, Architecture = (string)row["Architecture"]!,
        Folder = Path.GetFullPath(Path.Combine(Settings.Root, (string)row["Folder"]!)), ScreenWidth = Convert.ToInt32(row["ScreenWidth"]), ScreenHeight = Convert.ToInt32(row["ScreenHeight"]),
        CreatedAt = DateTimeOffset.Parse((string)row["CreatedAt"]!, CultureInfo.InvariantCulture),
        DesktopUser = row.TryGetValue("DesktopUser", out var user) && user is string { Length: > 0 } named ? named : GuestNames.DesktopUser
    };
}
