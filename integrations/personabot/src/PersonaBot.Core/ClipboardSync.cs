namespace PersonaBot;

/// <summary>Text-only clipboard reconciliation for one active computer. Never stores clipboard data on disk.</summary>
public sealed class ClipboardSync
{
    private bool _initialized;
    private string? _host, _guest;

    public async Task TickAsync(Func<Task<string?>> readHost, Func<Task<string?>> readGuest,
        Func<string, Task> writeHost, Func<string, Task> writeGuest, Func<bool> stillActive)
    {
        if (!stillActive()) return;
        var host = await readHost();
        if (!stillActive()) return;
        var guest = await readGuest();
        if (!stillActive()) return;
        // A tab becoming active receives the current Windows text; never exports its stale clipboard.
        if (!_initialized || host != _host)
        {
            if (host != null && host != guest)
            {
                await writeGuest(host);
                if (!stillActive()) return;
                guest = host;
            }
        }
        else if (guest != null && guest != _guest && guest != host)
        {
            // The host may have copied something while the guest query was in flight.
            var current = await readHost();
            if (!stillActive() || current != host) return;
            await writeHost(guest);
            host = guest;
        }
        _host = host; _guest = guest; _initialized = true;
    }
}
