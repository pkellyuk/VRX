namespace PersonaBot;

/// <summary>Exclusive ownership of a VM folder, including preparation and shutdown.</summary>
public sealed class MachineLease : IDisposable
{
    private readonly FileStream _file;
    private MachineLease(FileStream file) => _file = file;

    public static MachineLease Acquire(string folder)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        System.Diagnostics.Trace.WriteLine($"MachineLease.Acquire enter: {folder}");
        Directory.CreateDirectory(folder);
        try
        {
            var lease = new MachineLease(new FileStream(Path.Combine(folder, ".personabot-running.lock"), FileMode.OpenOrCreate,
                FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose));
            System.Diagnostics.Trace.WriteLine($"MachineLease.Acquire exit: leased {folder}");
            return lease;
        }
        catch (IOException ex)
        {
            System.Diagnostics.Trace.TraceWarning($"MachineLease.Acquire: {folder} is already leased: {ex.Message}");
            throw new IOException("This virtual machine is already in use by another tab or PersonaBot instance.", ex);
        }
    }

    public void Dispose()
    {
        System.Diagnostics.Trace.WriteLine($"MachineLease.Dispose: releasing {_file.Name}");
        _file.Dispose();
    }
}
