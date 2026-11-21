namespace PersonaBot;

/// <summary>Explicit copies into/out of a QEMU Linux desktop. Only the chosen archive is served; no host folder is mounted.</summary>
public sealed class GuestFileTransfer(IGuestShell shell, GuestProfile profile, string importFolder = "PersonaBot")
{
    private string Desktop(string command) => "exec su -s /bin/sh " + GuestShellText.Quote(profile.DesktopUser)
        + " -c " + GuestShellText.Quote("export DISPLAY=:0 HOME=" + GuestShellText.Quote(profile.DesktopHome) + "; " + command);

    private static string NewId() => DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8];
    public async Task<string> ImportAsync(IReadOnlyList<string> paths, CancellationToken token)
    {
        var id = NewId();
        var archive = Path.Combine(Settings.Root, "Transfers", id + ".tar");
        Directory.CreateDirectory(Path.GetDirectoryName(archive)!);
        try
        {
            await Task.Run(() => TransferArchive.Pack(paths, archive, token), token);
            var destination = profile.DesktopHome + "/Downloads/" + importFolder + "/" + id;
            await using var endpoint = new EndpointScope();
            var command = "set -e; umask 077; transfer_secret=" + endpoint.Endpoint.GuestUrl
                + "; f=$(mktemp); trap 'rm -f \"$f\"' EXIT; curl --noproxy '*' -fsS --max-time 900 \"$transfer_secret\" -o \"$f\"; "
                + "mkdir -p " + GuestShellText.Quote(destination) + "; tar -xf \"$f\" -C " + GuestShellText.Quote(destination);
            await ExchangeAsync(endpoint.Endpoint, archive, receive: false, Desktop(command), token);
            return destination;
        }
        finally { if (File.Exists(archive)) File.Delete(archive); }
    }

    public async Task<IReadOnlyList<string>> ExportAsync(IReadOnlyList<string> paths, CancellationToken token)
    {
        if (paths.Count == 0 || paths.Count > 128 || paths.Any(p => !p.StartsWith('/') || p.Contains('\0')))
            throw new IOException("Only local Linux files and folders can be copied.");
        var id = NewId();
        var root = Path.Combine(Settings.Root, "Transfers"); Directory.CreateDirectory(root);
        var archive = Path.Combine(root, id + ".tar");
        var arguments = string.Join(" ", paths.Select(p => "-C " + GuestShellText.Quote(p[..(p.LastIndexOf('/') + 1)]) + " " + GuestShellText.Quote("./" + p[(p.LastIndexOf('/') + 1)..])));
        try
        {
            await using var endpoint = new EndpointScope();
            var command = "set -e; umask 077; transfer_secret=" + endpoint.Endpoint.GuestUrl
                + "; f=$(mktemp); trap 'rm -f \"$f\"' EXIT; tar -cf \"$f\" " + arguments
                + $"; test $(wc -c <\"$f\") -le {TransferArchive.MaximumBytes}; "
                + "curl --noproxy '*' -fsS --max-time 900 -H 'Expect:' --data-binary @\"$f\" \"$transfer_secret\"";
            await ExchangeAsync(endpoint.Endpoint, archive, receive: true, Desktop(command), token);
            return await Task.Run(() => TransferArchive.Unpack(archive, Path.Combine(root, id), token), token);
        }
        finally { if (File.Exists(archive)) File.Delete(archive); }
    }

    private async Task ExchangeAsync(TransferEndpoint endpoint, string archive, bool receive, string command, CancellationToken token)
    {
        using var limit = CancellationTokenSource.CreateLinkedTokenSource(token); limit.CancelAfter(TimeSpan.FromMinutes(3));
        var server = endpoint.ServeAsync(archive, receive, limit.Token);
        var guest = shell.ExecuteAsync(command, limit.Token);
        try
        {
            var completed = await Task.WhenAny(server, guest);
            if (completed == server) await server; // Stop the guest if the transfer server failed.
            var result = await guest;
            if (!result.TrimEnd().EndsWith("\nExit code: 0", StringComparison.Ordinal)) throw new IOException("The guest file copy failed. Check that curl is installed and the selected files are readable.");
            await server;
        }
        finally
        {
            limit.Cancel();
            try { await server; } catch (Exception) when (limit.IsCancellationRequested) { }
            try { await guest; } catch (Exception) when (limit.IsCancellationRequested) { }
        }
    }

    private sealed class EndpointScope : IAsyncDisposable
    {
        public TransferEndpoint Endpoint { get; } = new();
        public ValueTask DisposeAsync() { Endpoint.Dispose(); return ValueTask.CompletedTask; }
    }
}
