using System.Buffers.Binary;
using System.Text.Json;

namespace PersonaBot;

/// <summary>Per-machine memory and grow-only disk configuration; takes effect on the next cold start.</summary>
public sealed record MachineResources(int MemoryMb, int DiskGb)
{
    public static bool CanGrowDisk(Machine machine) => machine.Distribution is "ubuntu-24.04" or "debian-13";
    public static string DiskPath(Machine machine) => Path.Combine(machine.Folder, machine.Distribution.StartsWith("android-", StringComparison.Ordinal) ? "userdata.qcow2" : "desktop.qcow2");
    public static long VirtualDiskBytes(Machine machine)
    {
        var path = DiskPath(machine);
        if (!File.Exists(path)) return 0;
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        Span<byte> header = stackalloc byte[32]; file.ReadExactly(header);
        if (BinaryPrimitives.ReadUInt32BigEndian(header) != 0x514649fb) throw new IOException("The guest disk is not QCOW2.");
        return checked((long)BinaryPrimitives.ReadUInt64BigEndian(header[24..]));
    }

    public static MachineResources Load(Machine machine, Settings settings)
    {
        var json = PersonaBotDatabase.EnsureReady().ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = @Key", ChatStore.P("@Key", "MachineResources:" + machine.Id));
        var saved = json == null ? null : JsonSerializer.Deserialize<MachineResources>(json);
        var disk = (int)Math.Ceiling(VirtualDiskBytes(machine) / (1024d * 1024 * 1024));
        return saved ?? new MachineResources(Math.Max(machine.Distribution.StartsWith("android-", StringComparison.Ordinal) ? 4096 : 1024, settings.MemoryMb),
            CanGrowDisk(machine) ? Math.Max(settings.DiskGb, disk) : Math.Max(8, disk));
    }

    public void Save(Machine machine)
    {
        var minimumMemory = machine.Distribution.StartsWith("android-", StringComparison.Ordinal) ? 4096 : 1024;
        if (MemoryMb < minimumMemory || MemoryMb > 131072 || DiskGb is < 8 or > 512) throw new ArgumentException("Choose a supported memory and disk size.");
        if (DiskGb * (1024L * 1024 * 1024) < VirtualDiskBytes(machine)) throw new ArgumentException("Disks can grow, but cannot be shrunk.");
        if (!CanGrowDisk(machine) && DiskGb != Math.Max(8, (int)Math.Ceiling(VirtualDiskBytes(machine) / (1024d * 1024 * 1024))))
            throw new NotSupportedException("Disk resizing is currently supported for Ubuntu and Debian. RAM can still be changed for this VM.");
        PersonaBotDatabase.EnsureReady().Execute("INSERT INTO PersonaBotPreferences (Key, Value) VALUES (@Key, @Value) ON CONFLICT(Key) DO UPDATE SET Value = excluded.Value",
            ChatStore.P("@Key", "MachineResources:" + machine.Id), ChatStore.P("@Value", JsonSerializer.Serialize(this)));
    }
}
